/*
 * Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
 * or more contributor license agreements. Licensed under the Elastic License
 * 2.0 and the following additional limitation. Functionality enabled by the
 * files subject to the Elastic License 2.0 may only be used in production when
 * invoked by an Elasticsearch process with a license key installed that permits
 * use of machine learning features. You may not use this file except in
 * compliance with the Elastic License 2.0 and the foregoing additional
 * limitation.
 */

#include <maths/time_series/CTimeSeriesDecompositionDetail.h>

#include <core/CIEEE754.h>
#include <core/CLogger.h>
#include <core/CMemoryDef.h>
#include <core/CPersistUtils.h>
#include <core/CStatePersistInserter.h>
#include <core/CStateRestoreTraverser.h>
#include <core/CTimeUtils.h>
#include <core/CTimezone.h>
#include <core/Constants.h>
#include <core/RestoreMacros.h>

#include <maths/common/CBasicStatistics.h>
#include <maths/common/CBasicStatisticsPersist.h>
#include <maths/common/CChecksum.h>
#include <maths/common/CIntegerTools.h>
#include <maths/common/CLeastSquaresOnlineRegressionDetail.h>
#include <maths/common/CLinearAlgebra.h>
#include <maths/common/CLinearAlgebraPersist.h>
#include <maths/common/COrderings.h>
#include <maths/common/COrderingsSimultaneousSort.h>
#include <maths/common/CSampling.h>
#include <maths/common/CSetTools.h>
#include <maths/common/CStatisticalTests.h>
#include <maths/common/CTools.h>
#include <maths/common/Constants.h>

#include <maths/time_series/CCalendarComponent.h>
#include <maths/time_series/CExpandingWindow.h>
#include <maths/time_series/CSeasonalComponentAdaptiveBucketing.h>
#include <maths/time_series/CSeasonalTime.h>
#include <maths/time_series/CTimeSeriesDecomposition.h>
#include <maths/time_series/CTimeSeriesSegmentation.h>
#include <maths/time_series/CTimeSeriesTestForChange.h>
#include <maths/time_series/CTimeSeriesTestForSeasonality.h>

#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/unordered_map.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace ml {
namespace maths {
namespace time_series {
namespace {

using TSeasonalComponentVec = maths_t::TSeasonalComponentVec;
using TCalendarComponentVec = maths_t::TCalendarComponentVec;
using TBoolVec = std::vector<bool>;
using TDoubleVec = std::vector<double>;
using TSizeVec = std::vector<std::size_t>;
using TSizeVecVec = std::vector<TSizeVec>;
using TSizeSizeMap = std::map<std::size_t, std::size_t>;
using TStrVec = std::vector<std::string>;
using TTimeVec = std::vector<core_t::TTime>;
using TTimeTimePr = std::pair<core_t::TTime, core_t::TTime>;
using TTimeTimePrVec = std::vector<TTimeTimePr>;
using TTimeTimePrDoubleFMap = boost::container::flat_map<TTimeTimePr, double>;
using TTimeTimePrSizeFMap = boost::container::flat_map<TTimeTimePr, std::size_t>;
using TMeanAccumulator = common::CBasicStatistics::SSampleMean<double>::TAccumulator;
using TFloatMeanAccumulator =
    common::CBasicStatistics::SSampleMean<common::CFloatStorage>::TAccumulator;
using TFloatMeanAccumulatorVec = std::vector<TFloatMeanAccumulator>;
using TSeasonalComponentPtrVec = std::vector<CSeasonalComponent*>;
using TCalendarComponentPtrVec = std::vector<CCalendarComponent*>;

const core_t::TTime DAY{core::constants::DAY};
const core_t::TTime WEEK{core::constants::WEEK};
const core_t::TTime MONTH{4 * WEEK};
const std::ptrdiff_t MAXIMUM_COMPONENTS{8};
const TSeasonalComponentVec NO_SEASONAL_COMPONENTS;
const TCalendarComponentVec NO_CALENDAR_COMPONENTS;

//! We scale the time used for the regression model to improve
//! the condition of the design matrix.
double scaleTime(core_t::TTime time, core_t::TTime origin) {
    return static_cast<double>(time - origin) / static_cast<double>(WEEK);
}

//! Get the aging factor to apply for \p dt elapsed time.
double ageFactor(double decayRate, core_t::TTime dt, core_t::TTime scale = DAY) {
    return std::exp(-decayRate * static_cast<double>(dt) / static_cast<double>(scale));
}

//! Compute the mean of \p mean of \p components.
template<typename MEAN_FUNCTION>
double meanOf(MEAN_FUNCTION mean, const TSeasonalComponentVec& components) {
    // We can choose to partition the trend model into windows.
    // In particular, we check for the presence of weekday/end
    // patterns. In this function we want to compute the sum of
    // the mean average of the different components: we use an
    // additive decomposition of the trend. However, if we have
    // detected a partition we want to average the models for
    // the different windows.

    double unwindowed{0.0};
    TTimeTimePrDoubleFMap windows;
    windows.reserve(components.size());
    for (const auto& component : components) {
        if (component.initialized()) {
            TTimeTimePr window{component.time().window()};
            if (window.second - window.first == component.time().windowRepeat()) {
                unwindowed += (component.*mean)();
            } else {
                windows[window] += (component.*mean)();
            }
        }
    }

    TMeanAccumulator windowed;
    for (const auto& window : windows) {
        double weight{static_cast<double>(window.first.second - window.first.first)};
        windowed.add(window.second, weight);
    }

    return unwindowed + common::CBasicStatistics::mean(windowed);
}

//! Compute the values to add to the trend and each component.
//!
//! \param[in] trend The long term trend prediction.
//! \param[in] seasonal The seasonal components.
//! \param[in] calendar The calendar components.
//! \param[in] time The time of value to decompose.
//! \param[in] deltas The delta offset to apply to the difference
//! between each component value and its mean, used to minimize
//! slope in the longer periods.
//! \param[in,out] decomposition Updated to contain the value to
//! add to each by component.
//! \param[out] predictions Filled in with the component predictions.
//! \param[out] referenceError Filled in with the error w.r.t. the trend.
//! \param[out] error Filled in with the prediction error.
//! \param[out] scale Filled in with the normalization scaling.
void decompose(double trend,
               const TSeasonalComponentPtrVec& seasonal,
               const TCalendarComponentPtrVec& calendar,
               const core_t::TTime time,
               const TDoubleVec& deltas,
               double gain,
               TDoubleVec& decomposition,
               TDoubleVec& predictions,
               double& referenceError,
               double& error,
               double& scale) {
    std::size_t m{seasonal.size()};
    std::size_t n{calendar.size()};

    double x0{trend};
    TDoubleVec x(m + n);
    double xhat{x0};
    for (std::size_t i = 0; i < m; ++i) {
        x[i] = seasonal[i]->value(time, 0.0).mean();
        xhat += x[i];
    }
    for (std::size_t i = m; i < m + n; ++i) {
        x[i] = calendar[i - m]->value(time, 0.0).mean();
        xhat += x[i];
    }

    // Note we are adding on the a proportion of the error to the
    // target value for each component. This constant controls the
    // proportion of the overall error we add. There is no need
    // to arrange for the sum error added to all components to be
    // equal to the actual error to avoid bias: noise will still
    // average down to zero (since the errors will be both positive
    // and negative). It will however affect the variance in the
    // limit the trend has been fit. This can be thought of as a
    // trade off between the rate at which each component reacts
    // to errors verses the error variance in the steady state with
    // smaller values of Z corresponding to greater responsiveness.
    double Z{std::max(static_cast<double>(m + n + 1) / gain, 1.0)};

    error = decomposition[0] - xhat;
    referenceError = decomposition[0] - x0;
    decomposition[0] = x0 + (decomposition[0] - xhat) / Z;
    for (std::size_t i = 0; i < m; ++i) {
        predictions[i] = x[i] - seasonal[i]->meanValue();
        decomposition[i + 1] = x[i] + (decomposition[i + 1] - xhat) / Z + deltas[i];
    }
    for (std::size_t i = m; i < m + n; ++i) {
        predictions[i] = x[i] - calendar[i - m]->meanValue();
        decomposition[i + 1] = x[i] + (decomposition[i + 1] - xhat) / Z;
    }

    // Because we add in more than the prediction error across the
    // different components, i.e. because Z < m + n + 1, we end up
    // with a bias in our variance estimates. We can mostly correct
    // the bias by scaling the variance estimate, but need to calculate
    // the scale.
    scale = Z / static_cast<double>(m + n + 1);
}

//! Propagate \p target forwards to account for \p end - \p start elapsed
//! time in steps or size \p step.
template<typename F>
void stepwisePropagateForwards(core_t::TTime start,
                               core_t::TTime end,
                               core_t::TTime step,
                               const F& propagateForwardsByTime) {
    start = common::CIntegerTools::floor(start, step);
    end = common::CIntegerTools::floor(end, step);
    if (end > start) {
        double time{static_cast<double>(end - start) / static_cast<double>(step)};
        propagateForwardsByTime(time);
    }
}

//! Add on mean zero \p variance normally distributed noise to \p values.
void addMeanZeroNormalNoise(double variance, TFloatMeanAccumulatorVec& values) {
    if (variance > 0.0) {
        common::CPRNG::CXorOShiro128Plus rng;
        for (auto& value : values) {
            common::CBasicStatistics::moment<0>(value) +=
                common::CSampling::normalSample(rng, 0.0, variance);
        }
    }
}

// Change Detector Test State Machine
// States
const std::size_t CD_TEST = 0;
const std::size_t CD_NOT_TESTING = 1;
const std::size_t CD_ERROR = 2;
const TStrVec CD_STATES{"TEST", "NOT_TESTING", "ERROR"};
// Alphabet
const std::size_t CD_DISABLE = 0;
const std::size_t CD_RESET = 1;
const TStrVec CD_ALPHABET{"DISABLE", "RESET"};
// Transition Function
const TSizeVecVec CD_TRANSITION_FUNCTION{{CD_NOT_TESTING, CD_NOT_TESTING, CD_ERROR},
                                         {CD_TEST, CD_NOT_TESTING, CD_TEST}};

// Seasonality Test State Machine
// States
const std::size_t PT_INITIAL = 0;
const std::size_t PT_TEST = 1;
const std::size_t PT_NOT_TESTING = 2;
const std::size_t PT_ERROR = 3;
const TStrVec PT_STATES{"INITIAL", "TEST", "NOT_TESTING", "ERROR"};
// Alphabet
const std::size_t PT_NEW_VALUE = 0;
const std::size_t PT_RESET = 1;
const TStrVec PT_ALPHABET{"NEW_VALUE", "RESET"};
// Transition Function
const TSizeVecVec PT_TRANSITION_FUNCTION{{PT_TEST, PT_TEST, PT_NOT_TESTING, PT_ERROR},
                                         {PT_INITIAL, PT_INITIAL, PT_NOT_TESTING, PT_INITIAL}};

// Calendar Cyclic Test State Machine
// States
const std::size_t CC_INITIAL = 0;
const std::size_t CC_TEST = 1;
const std::size_t CC_NOT_TESTING = 2;
const std::size_t CC_ERROR = 3;
const TStrVec CC_STATES{"INITIAL", "TEST", "NOT_TESTING", "ERROR"};
// Alphabet
const std::size_t CC_NEW_VALUE = 0;
const std::size_t CC_RESET = 1;
const TStrVec CC_ALPHABET{"NEW_VALUE", "RESET"};
// Transition Function
const TSizeVecVec CC_TRANSITION_FUNCTION{
    TSizeVec{CC_TEST, CC_TEST, CC_NOT_TESTING, CC_ERROR},
    TSizeVec{CC_INITIAL, CC_INITIAL, CC_NOT_TESTING, CC_INITIAL}};

// Components State Machine
// States
const std::size_t SC_NEW_COMPONENTS = 0;
const std::size_t SC_NORMAL = 1;
const std::size_t SC_DISABLED = 2;
const std::size_t SC_ERROR = 3;
const TStrVec SC_STATES{"NEW_COMPONENTS", "NORMAL", "DISABLED", "ERROR"};
// Alphabet
const std::size_t SC_ADDED_COMPONENTS = 0;
const std::size_t SC_INTERPOLATED = 1;
const std::size_t SC_RESET = 2;
const TStrVec SC_ALPHABET{"ADDED_COMPONENTS", "INTERPOLATED", "RESET"};
// Transition Function
const TSizeVecVec SC_TRANSITION_FUNCTION{
    TSizeVec{SC_NEW_COMPONENTS, SC_NEW_COMPONENTS, SC_DISABLED, SC_ERROR},
    TSizeVec{SC_NORMAL, SC_NORMAL, SC_DISABLED, SC_ERROR},
    TSizeVec{SC_NORMAL, SC_NORMAL, SC_NORMAL, SC_NORMAL}};

const std::string VERSION_6_3_TAG("6.3");
const std::string VERSION_6_4_TAG("6.4");

// Change Detector Test Tags
// Version 7.11
const core::TPersistenceTag CHANGE_DETECTOR_TEST_MACHINE_7_11_TAG{"a", "change_detector_test_machine"};
const core::TPersistenceTag SLIDING_WINDOW_7_11_TAG{"b", "sliding_window"};
const core::TPersistenceTag MEAN_OFFSET_7_11_TAG{"c", "mean_offset"};
const core::TPersistenceTag RESIDUAL_MOMENTS_7_11_TAG{"d", "residual_moments"};
const core::TPersistenceTag LARGE_ERROR_FRACTION_7_11_TAG{"e", "large_error_fraction"};
const core::TPersistenceTag TOTAL_COUNT_WEIGHT_ADJUSTMENT_7_11_TAG{
    "f", "total_count_weight_adjustment"};
const core::TPersistenceTag MINIMUM_TOTAL_COUNT_WEIGHT_ADJUSTMENT_7_11_TAG{
    "g", "minimum_total_count_weight_adjustment"};
const core::TPersistenceTag LAST_TEST_TIME_7_11_TAG{"h", "last_test_time"};
const core::TPersistenceTag LAST_CHANGE_POINT_TIME_7_11_TAG{"i", "last_change_point_time"};
const core::TPersistenceTag LAST_CANDIDATE_CHANGE_POINT_TIME_7_11_TAG{
    "j", "last_candidate_change_point_time"};
const core::TPersistenceTag LAST_CHANGE_POINT_7_11_TAG{"k", "last_change_point"};
// Version 8.3
const core::TPersistenceTag OUTLIER_WEIGHT_DERATE_8_3_TAG{"l", "winsorization_derate"};

// Seasonality Test Tags
// Version 6.3
const core::TPersistenceTag SEASONALITY_TEST_MACHINE_6_3_TAG{"a", "periodicity_test_machine"};
// Version 7.9
const core::TPersistenceTag SHORT_WINDOW_7_9_TAG{"e", "short_window_7_9"};
const core::TPersistenceTag LONG_WINDOW_7_9_TAG{"f", "long_window_7_9"};
// Old versions can't be restored.

// Calendar Cyclic Test Tags
// Version 6.3
const core::TPersistenceTag CALENDAR_TEST_MACHINE_6_3_TAG{"a", "calendar_test_machine"};
const core::TPersistenceTag LAST_MONTH_6_3_TAG{"b", "last_month"};
const core::TPersistenceTag CALENDAR_TEST_6_3_TAG{"c", "calendar_test"};
// These work for all versions.

// Components Tags
// Version 6.3
const core::TPersistenceTag COMPONENTS_MACHINE_6_3_TAG{"a", "components_machine"};
const core::TPersistenceTag DECAY_RATE_6_3_TAG{"b", "decay_rate"};
const core::TPersistenceTag TREND_6_3_TAG{"c", "trend"};
const core::TPersistenceTag SEASONAL_6_3_TAG{"d", "seasonal"};
const core::TPersistenceTag CALENDAR_6_3_TAG{"e", "calendar"};
const core::TPersistenceTag COMPONENT_6_3_TAG{"f", "component"};
const core::TPersistenceTag MEAN_VARIANCE_SCALE_6_3_TAG{"h", "mean_variance_scale"};
const core::TPersistenceTag MOMENTS_6_3_TAG{"i", "moments"};
const core::TPersistenceTag MOMENTS_MINUS_TREND_6_3_TAG{"j", "moments_minus_trend"};
const core::TPersistenceTag USING_TREND_FOR_PREDICTION_6_3_TAG{"k", "using_trend_for_prediction"};
const core::TPersistenceTag GAIN_CONTROLLER_6_3_TAG{"l", "gain_controller"};
// Version 6.4
const core::TPersistenceTag COMPONENT_6_4_TAG{"f", "component"};
const core::TPersistenceTag ERRORS_6_4_TAG{"g", "errors"};
const core::TPersistenceTag REGRESSION_ORIGIN_6_4_TAG{"a", "regression_origin"};
const core::TPersistenceTag MEAN_SUM_AMPLITUDES_6_4_TAG{"b", "mean_sum_amplitudes"};
const core::TPersistenceTag MEAN_SUM_AMPLITUDES_TREND_6_4_TAG{"c", "mean_sum_amplitudes_trend"};

// This implements the mapping from restored states to their best
// equivalents; specifically:
// SC_NEW_COMPONENTS |-> SC_NEW_COMPONENTS
// SC_NORMAL |-> SC_NORMAL
// SC_FORECASTING |-> SC_NORMAL
// SC_DISABLED |-> SC_DISABLED
// SC_ERROR |-> SC_ERROR
// Note that we don't try and restore the periodicity test state
// (see CTimeSeriesDecomposition::acceptRestoreTraverser) and the
// calendar test state is unchanged.
const TSizeSizeMap SC_STATES_UPGRADING_TO_VERSION_6_3{{0, 0}, {1, 1}, {2, 1}, {3, 2}, {4, 3}};

////////////////////////////////////////////////////////////////////////
}

//////// SMessage ////////

CTimeSeriesDecompositionDetail::SMessage::SMessage(core_t::TTime time,
                                                   core_t::TTime lastTime,
                                                   const TMemoryCircuitBreaker& memoryCircuitBreaker)
    : s_Time{time}, s_LastTime{lastTime}, s_MemoryCircuitBreaker{memoryCircuitBreaker} {
}

//////// SAddValue ////////

CTimeSeriesDecompositionDetail::SAddValue::SAddValue(
    core_t::TTime time,
    core_t::TTime lastTime,
    core_t::TTime timeShift,
    double value,
    const maths_t::TDoubleWeightsAry& weights,
    double occupancy,
    core_t::TTime firstValueTime,
    double trend,
    double seasonal,
    double calendar,
    CTimeSeriesDecomposition& decomposition,
    const TMakePredictor& makePredictor,
    const TMakeFilteredPredictor& makeSeasonalityTestPreconditioner,
    const TMakeTestForSeasonality& makeTestForSeasonality,
    const TMemoryCircuitBreaker& memoryCircuitBreaker)
    : SMessage{time, lastTime, memoryCircuitBreaker},
      s_TimeShift{timeShift}, s_Value{value}, s_Weights{weights}, s_Occupancy{occupancy},
      s_FirstValueTime{firstValueTime}, s_Trend{trend}, s_Seasonal{seasonal},
      s_Calendar{calendar}, s_Decomposition{&decomposition}, s_MakePredictor{makePredictor},
      s_MakeSeasonalityTestPreconditioner{makeSeasonalityTestPreconditioner},
      s_MakeTestForSeasonality{makeTestForSeasonality} {
}

//////// SDetectedSeasonal ////////

CTimeSeriesDecompositionDetail::SDetectedSeasonal::SDetectedSeasonal(
    core_t::TTime time,
    core_t::TTime lastTime,
    CSeasonalDecomposition components,
    const TMemoryCircuitBreaker& memoryCircuitBreaker)
    : SMessage{time, lastTime, memoryCircuitBreaker}, s_Components{std::move(components)} {
}

//////// SDetectedCalendar ////////

CTimeSeriesDecompositionDetail::SDetectedCalendar::SDetectedCalendar(
    core_t::TTime time,
    core_t::TTime lastTime,
    CCalendarFeature feature,
    core_t::TTime timeZoneOffset,
    const TMemoryCircuitBreaker& memoryCircuitBreaker)
    : SMessage{time, lastTime, memoryCircuitBreaker}, s_Feature{feature}, s_TimeZoneOffset{timeZoneOffset} {
}

//////// SDetectedTrend ////////

CTimeSeriesDecompositionDetail::SDetectedTrend::SDetectedTrend(
    const TPredictor& predictor,
    const TComponentChangeCallback& componentChangeCallback,
    const TMemoryCircuitBreaker& memoryCircuitBreaker)
    : SMessage{0, 0, memoryCircuitBreaker}, s_Predictor{predictor},
      s_ComponentChangeCallback{componentChangeCallback} {
}

//////// SDetectedChangePoint ////////

CTimeSeriesDecompositionDetail::SDetectedChangePoint::SDetectedChangePoint(
    core_t::TTime time,
    core_t::TTime lastTime,
    TChangePointUPtr change,
    const TMemoryCircuitBreaker& memoryCircuitBreaker)
    : SMessage{time, lastTime, memoryCircuitBreaker}, s_Change{std::move(change)} {
}

//////// CHandler ////////

void CTimeSeriesDecompositionDetail::CHandler::handle(const SAddValue&) {
}

void CTimeSeriesDecompositionDetail::CHandler::handle(const SDetectedSeasonal&) {
}

void CTimeSeriesDecompositionDetail::CHandler::handle(const SDetectedCalendar&) {
}

void CTimeSeriesDecompositionDetail::CHandler::handle(const SDetectedTrend&) {
}

void CTimeSeriesDecompositionDetail::CHandler::handle(const SDetectedChangePoint&) {
}

void CTimeSeriesDecompositionDetail::CHandler::mediator(CMediator* mediator) {
    m_Mediator = mediator;
}

CTimeSeriesDecompositionDetail::CMediator*
CTimeSeriesDecompositionDetail::CHandler::mediator() const {
    return m_Mediator;
}

//////// CMediator ////////

template<typename M>
void CTimeSeriesDecompositionDetail::CMediator::forward(const M& message) const {
    for (CHandler& handler : m_Handlers) {
        handler.handle(message);
    }
}

void CTimeSeriesDecompositionDetail::CMediator::registerHandler(CHandler& handler) {
    m_Handlers.push_back(std::ref(handler));
    handler.mediator(this);
}

void CTimeSeriesDecompositionDetail::CMediator::debugMemoryUsage(
    const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CMediator");
    core::memory_debug::dynamicSize("m_Handlers", m_Handlers, mem);
}

std::size_t CTimeSeriesDecompositionDetail::CMediator::memoryUsage() const {
    return core::memory::dynamicSize(m_Handlers);
}

//////// CChangePointTest ////////

CTimeSeriesDecompositionDetail::CChangePointTest::CChangePointTest(double decayRate,
                                                                   core_t::TTime bucketLength)
    : m_Machine{core::CStateMachine::create(CD_ALPHABET, CD_STATES, CD_TRANSITION_FUNCTION, CD_TEST)},
      m_DecayRate{decayRate}, m_BucketLength{bucketLength},
      m_Window(this->windowSize(), TFloatMeanAccumulator{}),
      m_LastTestTime{std::numeric_limits<core_t::TTime>::min() / 2},
      m_LastChangePointTime{std::numeric_limits<core_t::TTime>::min() / 2},
      m_LastCandidateChangePointTime{std::numeric_limits<core_t::TTime>::min() / 2} {
}

CTimeSeriesDecompositionDetail::CChangePointTest::CChangePointTest(const CChangePointTest& other,
                                                                   bool isForForecast)
    : m_Machine{other.m_Machine}, m_DecayRate{other.m_DecayRate},
      m_BucketLength{other.m_BucketLength}, m_Window{other.m_Window},
      m_MeanOffset{other.m_MeanOffset}, m_ResidualMoments{other.m_ResidualMoments},
      m_LargeErrorFraction{other.m_LargeErrorFraction},
      m_TotalCountWeightAdjustment{other.m_TotalCountWeightAdjustment},
      m_MinimumTotalCountWeightAdjustment{other.m_MinimumTotalCountWeightAdjustment},
      m_LastTestTime{other.m_LastTestTime}, m_LastChangePointTime{other.m_LastChangePointTime},
      m_LastCandidateChangePointTime{other.m_LastCandidateChangePointTime},
      m_LastChangeOutlierWeightDerate{other.m_LastChangeOutlierWeightDerate} {

    if (isForForecast) {
        this->apply(CD_DISABLE);
    } else if (m_UndoableLastChange != nullptr) {
        m_UndoableLastChange = other.m_UndoableLastChange->undoable();
    }
}

bool CTimeSeriesDecompositionDetail::CChangePointTest::acceptRestoreTraverser(
    core::CStateRestoreTraverser& traverser) {
    do {
        const std::string& name{traverser.name()};
        RESTORE(CHANGE_DETECTOR_TEST_MACHINE_7_11_TAG,
                traverser.traverseSubLevel([this](core::CStateRestoreTraverser& traverser_) {
                    return m_Machine.acceptRestoreTraverser(traverser_);
                }))
        RESTORE(SLIDING_WINDOW_7_11_TAG,
                core::CPersistUtils::restore(SLIDING_WINDOW_7_11_TAG, m_Window, traverser))
        RESTORE(MEAN_OFFSET_7_11_TAG, m_MeanOffset.fromDelimited(traverser.value()))
        RESTORE(RESIDUAL_MOMENTS_7_11_TAG,
                m_ResidualMoments.fromDelimited(traverser.value()))
        RESTORE_BUILT_IN(LARGE_ERROR_FRACTION_7_11_TAG, m_LargeErrorFraction)
        RESTORE_BUILT_IN(TOTAL_COUNT_WEIGHT_ADJUSTMENT_7_11_TAG, m_TotalCountWeightAdjustment)
        RESTORE_BUILT_IN(MINIMUM_TOTAL_COUNT_WEIGHT_ADJUSTMENT_7_11_TAG,
                         m_MinimumTotalCountWeightAdjustment)
        RESTORE_BUILT_IN(LAST_TEST_TIME_7_11_TAG, m_LastTestTime)
        RESTORE_BUILT_IN(LAST_CHANGE_POINT_TIME_7_11_TAG, m_LastChangePointTime)
        RESTORE_BUILT_IN(LAST_CANDIDATE_CHANGE_POINT_TIME_7_11_TAG, m_LastCandidateChangePointTime)
        RESTORE(LAST_CHANGE_POINT_7_11_TAG, traverser.traverseSubLevel([
            this, serializer = CUndoableChangePointStateSerializer{}
        ](auto& traverser_) {
            return serializer(m_UndoableLastChange, traverser_);
        }))
        RESTORE(OUTLIER_WEIGHT_DERATE_8_3_TAG,
                traverser.traverseSubLevel([this](core::CStateRestoreTraverser& traverser_) {
                    return m_LastChangeOutlierWeightDerate.acceptRestoreTraverser(traverser_);
                }))
    } while (traverser.next());
    return true;
}

void CTimeSeriesDecompositionDetail::CChangePointTest::acceptPersistInserter(
    core::CStatePersistInserter& inserter) const {
    inserter.insertLevel(CHANGE_DETECTOR_TEST_MACHINE_7_11_TAG, [this](auto& inserter_) {
        return m_Machine.acceptPersistInserter(inserter_);
    });
    core::CPersistUtils::persist(SLIDING_WINDOW_7_11_TAG, m_Window, inserter);
    inserter.insertValue(MEAN_OFFSET_7_11_TAG, m_MeanOffset.toDelimited());
    inserter.insertValue(RESIDUAL_MOMENTS_7_11_TAG, m_ResidualMoments.toDelimited());
    inserter.insertValue(LARGE_ERROR_FRACTION_7_11_TAG, m_LargeErrorFraction,
                         core::CIEEE754::E_DoublePrecision);
    inserter.insertValue(TOTAL_COUNT_WEIGHT_ADJUSTMENT_7_11_TAG,
                         m_TotalCountWeightAdjustment, core::CIEEE754::E_DoublePrecision);
    inserter.insertValue(MINIMUM_TOTAL_COUNT_WEIGHT_ADJUSTMENT_7_11_TAG,
                         m_MinimumTotalCountWeightAdjustment,
                         core::CIEEE754::E_DoublePrecision);
    inserter.insertValue(LAST_TEST_TIME_7_11_TAG, m_LastTestTime);
    inserter.insertValue(LAST_CHANGE_POINT_TIME_7_11_TAG, m_LastChangePointTime);
    inserter.insertValue(LAST_CANDIDATE_CHANGE_POINT_TIME_7_11_TAG,
                         m_LastCandidateChangePointTime);
    if (m_UndoableLastChange != nullptr) {
        inserter.insertLevel(LAST_CHANGE_POINT_7_11_TAG, [
            this, serializer = CUndoableChangePointStateSerializer{}
        ](auto& inserter_) { serializer(*m_UndoableLastChange, inserter_); });
    }
    inserter.insertLevel(OUTLIER_WEIGHT_DERATE_8_3_TAG, [this](auto& inserter_) {
        return m_LastChangeOutlierWeightDerate.acceptPersistInserter(inserter_);
    });
}

void CTimeSeriesDecompositionDetail::CChangePointTest::swap(CChangePointTest& other) {
    std::swap(m_Machine, other.m_Machine);
    std::swap(m_DecayRate, other.m_DecayRate);
    std::swap(m_BucketLength, other.m_BucketLength);
    m_Window.swap(other.m_Window);
    std::swap(m_MeanOffset, other.m_MeanOffset);
    std::swap(m_ResidualMoments, other.m_ResidualMoments);
    std::swap(m_LargeErrorFraction, other.m_LargeErrorFraction);
    std::swap(m_TotalCountWeightAdjustment, other.m_TotalCountWeightAdjustment);
    std::swap(m_MinimumTotalCountWeightAdjustment, other.m_MinimumTotalCountWeightAdjustment);
    std::swap(m_LastTestTime, other.m_LastTestTime);
    std::swap(m_LastChangePointTime, other.m_LastChangePointTime);
    std::swap(m_LastCandidateChangePointTime, other.m_LastCandidateChangePointTime);
    std::swap(m_UndoableLastChange, other.m_UndoableLastChange);
    std::swap(m_LastChangeOutlierWeightDerate, other.m_LastChangeOutlierWeightDerate);
}

void CTimeSeriesDecompositionDetail::CChangePointTest::handle(const SAddValue& message) {
    core_t::TTime lastTime{message.s_LastTime};
    core_t::TTime time{message.s_Time};
    double value{message.s_Value};
    double prediction{message.s_Trend + message.s_Seasonal + message.s_Calendar};
    // We have explicit handling of outliers in CTimeSeriesTestForChange.
    double weight{maths_t::count(message.s_Weights)};
    double weightForResidualMoments{maths_t::countForUpdate(message.s_Weights)};
    std::size_t steps{
        std::min(static_cast<std::size_t>((this->startOfWindowBucket(time) -
                                           this->startOfWindowBucket(lastTime)) /
                                          this->windowBucketLength()),
                 m_Window.size())};

    switch (m_Machine.state()) {
    case CD_TEST:
        for (std::size_t i = 0; i < steps; ++i) {
            m_Window.push_back(TFloatMeanAccumulator{});
        }
        m_Window.back().add(value, weight);
        m_MeanOffset.add(static_cast<double>(time % m_BucketLength), weight);
        m_ResidualMoments.add(value - prediction, weightForResidualMoments);
        this->updateTotalCountWeights(message);
        this->testForCandidateChange(message);
        this->testUndoLastChange(message);
        this->testForChange(message);
        break;
    case CD_NOT_TESTING:
        break;
    default:
        LOG_ERROR(<< "Test in a bad state: " << m_Machine.state());
        this->apply(CD_RESET);
        break;
    }
}

void CTimeSeriesDecompositionDetail::CChangePointTest::handle(const SDetectedSeasonal& message) {
    this->reset(message.s_Time);
}

void CTimeSeriesDecompositionDetail::CChangePointTest::reset(core_t::TTime time) {
    if (m_Window.empty() == false) {
        m_Window.assign(m_Window.size(), TFloatMeanAccumulator{});
    }
    m_ResidualMoments = TMeanVarAccumulator{};
    m_LargeErrorFraction = 0.0;
    m_TotalCountWeightAdjustment = 0.0;
    m_MinimumTotalCountWeightAdjustment = 0.0;
    m_LastCandidateChangePointTime = time - 4 * this->maximumIntervalToDetectChange(1.0);
}

double CTimeSeriesDecompositionDetail::CChangePointTest::countWeight(core_t::TTime) const {
    // We shape the count weight we apply initially using a small weight after
    // detecting a candidate change before switching to a large weight after
    // accepting a change or waiting maximumIntervalToDetectChange. We arrange
    // for the integral of the adjusted weight over time to be one.
    if (m_TotalCountWeightAdjustment > m_MinimumTotalCountWeightAdjustment &&
        m_LargeErrorFraction > 0.25) {
        return CHANGE_COUNT_WEIGHT;
    }
    return 1.0 + std::min(1.0, -m_TotalCountWeightAdjustment);
}

double CTimeSeriesDecompositionDetail::CChangePointTest::outlierWeightDerate(core_t::TTime time,
                                                                             double error) const {
    return std::max(1.0 - static_cast<double>(time - m_LastChangePointTime) /
                              static_cast<double>(3 * DAY),
                    0.0) *
           m_LastChangeOutlierWeightDerate.value(error);
}

void CTimeSeriesDecompositionDetail::CChangePointTest::propagateForwards(core_t::TTime start,
                                                                         core_t::TTime end) {
    stepwisePropagateForwards(start, end, DAY, [this](double time) {
        m_ResidualMoments.age(std::exp(-m_DecayRate * time / 8.0));
    });
}

std::uint64_t CTimeSeriesDecompositionDetail::CChangePointTest::checksum(std::uint64_t seed) const {
    seed = common::CChecksum::calculate(seed, m_Machine);
    seed = common::CChecksum::calculate(seed, m_DecayRate);
    seed = common::CChecksum::calculate(seed, m_BucketLength);
    seed = common::CChecksum::calculate(seed, m_Window);
    seed = common::CChecksum::calculate(seed, m_MeanOffset);
    seed = common::CChecksum::calculate(seed, m_ResidualMoments);
    seed = common::CChecksum::calculate(seed, m_LargeErrorFraction);
    seed = common::CChecksum::calculate(seed, m_TotalCountWeightAdjustment);
    seed = common::CChecksum::calculate(seed, m_MinimumTotalCountWeightAdjustment);
    seed = common::CChecksum::calculate(seed, m_LastTestTime);
    seed = common::CChecksum::calculate(seed, m_LastChangePointTime);
    seed = common::CChecksum::calculate(seed, m_LastCandidateChangePointTime);
    seed = common::CChecksum::calculate(seed, m_UndoableLastChange);
    return common::CChecksum::calculate(seed, m_LastChangeOutlierWeightDerate);
}

void CTimeSeriesDecompositionDetail::CChangePointTest::debugMemoryUsage(
    const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CChangePointTest");
    core::memory_debug::dynamicSize("m_Window", m_Window, mem);
    core::memory_debug::dynamicSize("m_UndoableLastChange", m_UndoableLastChange, mem);
}

std::size_t CTimeSeriesDecompositionDetail::CChangePointTest::memoryUsage() const {
    return core::memory::dynamicSize(m_Window) +
           core::memory::dynamicSize(m_UndoableLastChange);
}

void CTimeSeriesDecompositionDetail::CChangePointTest::apply(std::size_t symbol) {

    std::size_t old{m_Machine.state()};
    m_Machine.apply(symbol);
    std::size_t state{m_Machine.state()};

    if (state != old) {
        LOG_TRACE(<< CD_STATES[old] << "," << CD_ALPHABET[symbol] << " -> "
                  << CD_STATES[state]);
        switch (state) {
        case CD_TEST:
            m_Window = TFloatMeanAccumulatorCBuf(this->windowSize(),
                                                 TFloatMeanAccumulator{});
            m_MeanOffset = TFloatMeanAccumulator{};
            m_LargeErrorFraction = 0.0;
            break;
        case CD_NOT_TESTING:
            m_Window = TFloatMeanAccumulatorCBuf{};
            m_MeanOffset = TFloatMeanAccumulator{};
            m_LargeErrorFraction = 0.0;
            break;
        default:
            LOG_ERROR(<< "Test in a bad state: " << state);
            this->apply(CD_RESET);
            break;
        }
    }
}

void CTimeSeriesDecompositionDetail::CChangePointTest::updateTotalCountWeights(const SAddValue& message) {
    core_t::TTime lastTime{message.s_LastTime};
    core_t::TTime time{message.s_Time};
    double occupancy{message.s_Occupancy};
    m_TotalCountWeightAdjustment += static_cast<double>(time - lastTime) /
                                    static_cast<double>(m_BucketLength) *
                                    (this->countWeight(time) - 1.0);
    m_TotalCountWeightAdjustment = std::min(m_TotalCountWeightAdjustment, 0.0);
    if (m_TotalCountWeightAdjustment == 0.0) {
        m_MinimumTotalCountWeightAdjustment =
            (CHANGE_COUNT_WEIGHT - 1.0) *
            static_cast<double>(this->maximumIntervalToDetectChange(occupancy)) /
            static_cast<double>(m_BucketLength);
    }
    if (m_TotalCountWeightAdjustment < m_MinimumTotalCountWeightAdjustment) {
        m_MinimumTotalCountWeightAdjustment = 0.0;
    }
}

void CTimeSeriesDecompositionDetail::CChangePointTest::testForCandidateChange(const SAddValue& message) {
    core_t::TTime firstValueTime{message.s_FirstValueTime};
    core_t::TTime time{message.s_Time};

    // We're prone to detect changes at model startup before, for example, we
    // detect and model seasonality. Since the most common seasonality in the
    // data we model is daily, this delays detecting changes until we've had
    // the chance to see several repeats.
    if (time < firstValueTime + 3 * DAY) {
        return;
    }

    double occupancy{message.s_Occupancy};
    double value{message.s_Value};
    double prediction{message.s_Trend + message.s_Seasonal + message.s_Calendar};
    double error{std::fabs(value - prediction)};

    double beta{static_cast<double>(m_BucketLength) /
                (4.0 * static_cast<double>(this->windowBucketLength()))};
    double alpha{1.0 - beta};
    bool mayHaveChangedBefore{this->mayHaveChanged()};
    m_LargeErrorFraction = alpha * m_LargeErrorFraction +
                           beta * (error > this->largeError() ? 1.0 : 0.0);
    if (this->mayHaveChanged() && mayHaveChangedBefore == false &&
        time > m_LastCandidateChangePointTime +
                   2 * this->maximumIntervalToDetectChange(occupancy)) {
        m_LastCandidateChangePointTime = time;
    }
    LOG_TRACE(<< "large error fraction = " << m_LargeErrorFraction
              << ", error = " << error << ", large error = " << this->largeError());
}

void CTimeSeriesDecompositionDetail::CChangePointTest::testForChange(const SAddValue& message) {
    core_t::TTime time{message.s_Time};
    double occupancy{message.s_Occupancy};
    if (this->shouldTest(time, occupancy) == false) {
        return;
    }

    core_t::TTime lastTime{message.s_LastTime};
    core_t::TTime timeShift{message.s_TimeShift};
    bool seasonal{message.s_Decomposition->seasonalComponents().empty() == false};
    const auto& makePredictor = message.s_MakePredictor;
    CTimeSeriesDecomposition& decomposition{*message.s_Decomposition};

    auto begin = std::find_if(m_Window.begin(), m_Window.end(), [](const auto& bucket) {
        return common::CBasicStatistics::count(bucket) > 0.0;
    });
    std::ptrdiff_t length{std::distance(begin, m_Window.end())};

    if (this->windowBucketLength() * length <= this->minimumChangeLength(occupancy)) {
        return;
    }

    LOG_TRACE(<< "Testing for change at " << time);

    int testFor{seasonal ? CTimeSeriesTestForChange::E_All
                         : CTimeSeriesTestForChange::E_LevelShift};
    TPredictor predictor{makePredictor()};
    core_t::TTime bucketsStartTime{this->bucketsStartTime(time, length)};
    core_t::TTime valuesStartTime{this->valuesStartTime(bucketsStartTime)};
    TFloatMeanAccumulatorVec values{begin, m_Window.end()};
    LOG_TRACE(<< "buckets start time = " << bucketsStartTime
              << ", values start time = " << valuesStartTime
              << ", last candidate time = " << m_LastCandidateChangePointTime);

    CTimeSeriesTestForChange changeTest(
        testFor, valuesStartTime - timeShift, bucketsStartTime - timeShift,
        this->windowBucketLength(), m_BucketLength, predictor, std::move(values),
        0.0, CTimeSeriesTestForChange::OUTLIER_FRACTION * occupancy);

    auto change = changeTest.test();
    m_LastTestTime = time;

    if (change != nullptr && // did we detect a change at all
        change->largeEnough(this->largeError()) &&
        change->longEnough(time, this->minimumChangeLength(occupancy))) {
        addMeanZeroNormalNoise(common::CBasicStatistics::variance(m_ResidualMoments),
                               change->residuals());
        change->apply(decomposition);
        m_LargeErrorFraction = 0.0;
        m_LastChangePointTime = time;
        m_LastCandidateChangePointTime =
            std::min(m_LastCandidateChangePointTime,
                     time - this->maximumIntervalToDetectChange(occupancy));
        m_UndoableLastChange = change->undoable();
        m_LastChangeOutlierWeightDerate =
            change->outlierWeightDerate(bucketsStartTime, time, predictor);
        this->mediator()->forward(SDetectedChangePoint{
            time, lastTime, std::move(change), message.s_MemoryCircuitBreaker});
    } else if (change != nullptr) {
        m_LastCandidateChangePointTime = change->time();
    }
    LOG_TRACE(<< (change != nullptr ? "maybe " + change->print() : "no change"));
}

void CTimeSeriesDecompositionDetail::CChangePointTest::testUndoLastChange(const SAddValue& message) {
    if (m_UndoableLastChange == nullptr) {
        return;
    }

    core_t::TTime time{message.s_Time};
    core_t::TTime timeShift{message.s_TimeShift};
    core_t::TTime lastTime{message.s_LastTime};
    double occupancy{message.s_Occupancy};
    double value{message.s_Value};
    double weight{maths_t::count(message.s_Weights)};
    const auto& makePredictor = message.s_MakePredictor;
    CTimeSeriesDecomposition& decomposition{*message.s_Decomposition};

    m_UndoableLastChange->add(time - timeShift, lastTime - timeShift, value,
                              weight, makePredictor());

    if (time - m_LastChangePointTime > this->minimumChangeLength(occupancy) / 10 &&
        m_UndoableLastChange->shouldUndo()) {
        m_UndoableLastChange->apply(decomposition);
        this->mediator()->forward(SDetectedChangePoint{
            time, lastTime, std::move(m_UndoableLastChange), message.s_MemoryCircuitBreaker});
        m_UndoableLastChange.reset();
        return;
    }

    if (time - m_LastChangePointTime > this->maximumIntervalToDetectChange(occupancy)) {
        m_UndoableLastChange.reset();
    }
}

bool CTimeSeriesDecompositionDetail::CChangePointTest::mayHaveChanged() const {
    return m_LargeErrorFraction > 0.5;
}

double CTimeSeriesDecompositionDetail::CChangePointTest::largeError() const {
    return 3.0 * std::sqrt(common::CBasicStatistics::variance(m_ResidualMoments));
}

bool CTimeSeriesDecompositionDetail::CChangePointTest::shouldTest(core_t::TTime time,
                                                                  double occupancy) const {
    return (m_UndoableLastChange == nullptr) &&
           ((time > m_LastTestTime + this->minimumChangeLength(occupancy)) ||
            (time > m_LastTestTime + 3 * this->windowBucketLength() &&
             time < m_LastCandidateChangePointTime + this->maximumIntervalToDetectChange(occupancy) &&
             time > m_LastCandidateChangePointTime + this->minimumChangeLength(occupancy)));
}

core_t::TTime
CTimeSeriesDecompositionDetail::CChangePointTest::minimumChangeLength(double occupancy) const {
    // Transient changes tend to last 1 day. In such cases we do not want to
    // apply any change and mearly ignore the interval. By waiting 30 hours
    // we give ourselves a margin to see the revert before we commit to making
    // a change. Note for sparse data we delay detecting changes because we're
    // more prone to FP in this case, since we get less information per unit
    // time.
    core_t::TTime length{
        std::max(30 * core::constants::HOUR, 5 * this->windowBucketLength())};
    length = static_cast<core_t::TTime>(
        std::min(1.0 / occupancy, 2.0) * static_cast<double>(length) + 0.5);
    return common::CIntegerTools::ceil(length, this->windowBucketLength());
}

core_t::TTime
CTimeSeriesDecompositionDetail::CChangePointTest::maximumIntervalToDetectChange(double occupancy) const {
    return 5 * this->minimumChangeLength(occupancy) / 3;
}

core_t::TTime
CTimeSeriesDecompositionDetail::CChangePointTest::bucketsStartTime(core_t::TTime time,
                                                                   core_t::TTime bucketsLength) const {
    return this->startOfWindowBucket(time) -
           static_cast<core_t::TTime>(bucketsLength - 1) * this->windowBucketLength();
}

core_t::TTime
CTimeSeriesDecompositionDetail::CChangePointTest::valuesStartTime(core_t::TTime bucketsStartTime) const {
    core_t::TTime bucketEndTime{bucketsStartTime + this->windowBucketLength() - 1};
    core_t::TTime firstSampleInBucket{
        common::CIntegerTools::ceil(bucketsStartTime, m_BucketLength)};
    core_t::TTime lastSampleInBucket{common::CIntegerTools::floor(bucketEndTime, m_BucketLength)};
    return firstSampleInBucket + (lastSampleInBucket - firstSampleInBucket) / 2 +
           static_cast<core_t::TTime>(common::CBasicStatistics::mean(m_MeanOffset) + 0.5);
}

core_t::TTime
CTimeSeriesDecompositionDetail::CChangePointTest::startOfWindowBucket(core_t::TTime time) const {
    return common::CIntegerTools::floor(time, this->windowBucketLength());
}

core_t::TTime CTimeSeriesDecompositionDetail::CChangePointTest::windowLength() const {
    return static_cast<core_t::TTime>(m_Window.size()) * this->windowBucketLength();
}

core_t::TTime CTimeSeriesDecompositionDetail::CChangePointTest::windowBucketLength() const {
    return std::max(MINIMUM_WINDOW_BUCKET_LENGTH, m_BucketLength);
}

std::size_t CTimeSeriesDecompositionDetail::CChangePointTest::windowSize() const {
    return std::max(static_cast<std::size_t>((4 * core::constants::DAY) /
                                             this->windowBucketLength()),
                    std::size_t{32});
}

//////// CSeasonalityTest ////////

namespace {

using TTimeTimeVecPrVec = std::vector<std::pair<core_t::TTime, TTimeVec>>;

//! \brief Manages the choice of the tests' window parameters as a function
//! of the job's bucket length.
//!
//! DESCRIPTION:\n
//! The exact choice of window parameters is a tradeoff between the number
//! of points used in the test and how quickly it finds periodic components.
//! The fewer points the higher the chance of the false positives, but for
//! long bucket lengths using many buckets means it takes a long time to
//! find significant periodic components.
class CSeasonalityTestParameters {
public:
    static bool test(core_t::TTime bucketLength) {
        return bucketLength <= 604800;
    }

    static std::size_t numberBuckets(int window, core_t::TTime bucketLength) {
        const auto* params = windowParameters(window, bucketLength);
        return params != nullptr ? params->s_NumberBuckets : 0;
    }

    static core_t::TTime maxBucketLength(int window, core_t::TTime bucketLength) {
        const auto* bucketLengths_ = bucketLengths(window, bucketLength);
        return bucketLengths_ != nullptr ? bucketLengths_->back() : 0;
    }

    static const TTimeVec* bucketLengths(int window, core_t::TTime bucketLength) {
        const auto* params = windowParameters(window, bucketLength);
        return params != nullptr ? &params->s_BucketLengths : nullptr;
    }

    static const TTimeVec& testSchedule(int window, core_t::TTime bucketLength) {
        const auto* params = windowParameters(window, bucketLength);
        return params != nullptr ? params->s_TestSchedule : EMPTY_TEST_SCHEDULE;
    }

    static core_t::TTime shortestComponent(int window, core_t::TTime bucketLength) {
        const auto* params = windowParameters(window, bucketLength);
        return params != nullptr ? params->s_ShortestComponent : 0;
    }

    static std::size_t minimumResolutionToTestModelledComponent(int window,
                                                                core_t::TTime bucketLength,
                                                                bool shorterWindowAvailable) {
        const auto* params = windowParameters(window, bucketLength);
        return params != nullptr && shorterWindowAvailable ? params->s_MinimumResolution : 2;
    }

private:
    struct SParameters {
        SParameters() = default;
        SParameters(core_t::TTime bucketLength,
                    core_t::TTime shortestComponent,
                    std::size_t numberBuckets,
                    std::size_t minimumResolution,
                    const std::initializer_list<core_t::TTime>& bucketLengths,
                    const std::initializer_list<core_t::TTime>& testSchedule)
            : s_BucketLength{bucketLength}, s_ShortestComponent{shortestComponent},
              s_NumberBuckets{numberBuckets}, s_MinimumResolution{minimumResolution},
              s_BucketLengths{bucketLengths}, s_TestSchedule{testSchedule} {}
        bool operator<(core_t::TTime rhs) const { return s_BucketLength < rhs; }

        core_t::TTime s_BucketLength{0};
        core_t::TTime s_ShortestComponent{0};
        std::size_t s_NumberBuckets{0};
        std::size_t s_MinimumResolution{0};
        TTimeVec s_BucketLengths;
        TTimeVec s_TestSchedule;
    };
    using TParametersVecVec = std::vector<std::vector<SParameters>>;
    using TTimeParametersUMap = boost::unordered_map<core_t::TTime, SParameters>;

private:
    static const SParameters* windowParameters(int window, core_t::TTime bucketLength) {
        auto result = std::lower_bound(WINDOW_PARAMETERS[window].begin(),
                                       WINDOW_PARAMETERS[window].end(), bucketLength);
        return result != WINDOW_PARAMETERS[window].end() ? &(*result) : nullptr;
    }

private:
    static const TParametersVecVec WINDOW_PARAMETERS;
    static const TTimeVec EMPTY_TEST_SCHEDULE;
};

// These parameterise the windows used to test for periodic components. From
// left to right the parameters are:
//   1. The job bucket length,
//   2. The minimum period seasonal component we'll accept testing on the window,
//   3. The number buckets in the window,
//   4. The bucket lengths we'll cycle through as we test progressively longer
//      windows,
//   5. The times, in addition to "number buckets" * "window bucket lengths",
//      when we'll test for seasonal components.
const CSeasonalityTestParameters::TParametersVecVec CSeasonalityTestParameters::WINDOW_PARAMETERS{
    /* SHORT WINDOW */
    {{1, 1, 180, 10, {1, 5, 10, 30, 60, 300, 600}, {}},
     {5, 1, 180, 10, {5, 10, 30, 60, 300, 600}, {}},
     {10, 1, 180, 10, {10, 30, 60, 300, 600}, {}},
     {30, 1, 180, 10, {30, 60, 300, 600}, {}},
     {60, 1, 336, 12, {60, 300, 900, 3600, 7200}, {3 * 604800}},
     {300, 1, 336, 12, {300, 900, 3600, 7200}, {3 * 604800}},
     {600, 1, 336, 12, {600, 3600, 7200}, {3 * 604800}},
     {900, 1, 336, 12, {900, 3600, 7200}, {3 * 604800}},
     {1200, 1, 336, 12, {1200, 3600, 7200}, {3 * 86400, 3 * 604800}},
     {1800, 1, 336, 12, {1800, 3600, 7200}, {3 * 86400, 3 * 604800}},
     {3600, 1, 336, 12, {3600, 7200}, {3 * 86400, 604800, 3 * 604800}},
     {7200, 1, 336, 12, {7200, 14400}, {3 * 86400, 604800, 3 * 604800}},
     {14400, 1, 336, 6, {14400}, {604800, 3 * 604800}},
     {21600, 1, 224, 6, {21600}, {604800, 3 * 604800}},
     {28800, 1, 168, 6, {28800}, {3 * 604800}},
     {43200, 1, 112, 6, {43200}, {4 * 604800}},
     {86400, 1, 56, 6, {86400}, {}}},
    /* LONG WINDOW */
    {{1, 30601, 336, 12, {900, 3600, 7200}, {3 * 604800}},
     {5, 30601, 336, 12, {900, 3600, 7200}, {3 * 604800}},
     {10, 30601, 336, 12, {900, 3600, 7200}, {3 * 604800}},
     {30, 30601, 336, 12, {900, 3600, 7200}, {3 * 604800}},
     {60, 648001, 156, 6, {43200, 86400, 604800}, {104 * 604800}},
     {300, 648001, 156, 6, {43200, 86400, 604800}, {104 * 604800}},
     {600, 648001, 156, 6, {43200, 86400, 604800}, {104 * 604800}},
     {900, 648001, 156, 6, {43200, 86400, 604800}, {104 * 604800}},
     {1200, 648001, 156, 6, {43200, 86400, 604800}, {104 * 604800}},
     {1800, 648001, 156, 6, {43200, 86400, 604800}, {104 * 604800}},
     {3600, 648001, 156, 6, {43200, 86400, 604800}, {104 * 604800}},
     {7200, 648001, 156, 6, {43200, 86400, 604800}, {104 * 604800}},
     {14400, 648001, 156, 6, {43200, 86400, 604800}, {104 * 604800}},
     {86400, 648001, 156, 6, {43200, 86400, 604800}, {104 * 604800}},
     {604800, 648001, 156, 6, {43200, 86400, 604800}, {104 * 604800}}}};
}

const TTimeVec CSeasonalityTestParameters::EMPTY_TEST_SCHEDULE;

CTimeSeriesDecompositionDetail::CSeasonalityTest::CSeasonalityTest(double decayRate,
                                                                   core_t::TTime bucketLength)
    : m_Machine{core::CStateMachine::create(
          PT_ALPHABET,
          PT_STATES,
          PT_TRANSITION_FUNCTION,
          CSeasonalityTestParameters::test(bucketLength) ? PT_INITIAL : PT_NOT_TESTING)},
      m_DecayRate{decayRate}, m_BucketLength{bucketLength} {
}

CTimeSeriesDecompositionDetail::CSeasonalityTest::CSeasonalityTest(const CSeasonalityTest& other,
                                                                   bool isForForecast)
    : m_Machine{other.m_Machine}, m_DecayRate{other.m_DecayRate}, m_BucketLength{
                                                                      other.m_BucketLength} {
    if (isForForecast == false) {
        for (auto i : {E_Short, E_Long}) {
            if (other.m_Windows[i] != nullptr) {
                m_Windows[i] = std::make_unique<CExpandingWindow>(*other.m_Windows[i]);
            }
        }
    }
}

bool CTimeSeriesDecompositionDetail::CSeasonalityTest::acceptRestoreTraverser(
    core::CStateRestoreTraverser& traverser) {
    do {
        const std::string& name{traverser.name()};
        RESTORE(SEASONALITY_TEST_MACHINE_6_3_TAG,
                traverser.traverseSubLevel([this](core::CStateRestoreTraverser& traverser_) {
                    return m_Machine.acceptRestoreTraverser(traverser_);
                }))
        RESTORE_SETUP_TEARDOWN(
            SHORT_WINDOW_7_9_TAG, m_Windows[E_Short] = this->newWindow(E_Short),
            m_Windows[E_Short] && traverser.traverseSubLevel([this](auto& traverser_) {
                return m_Windows[E_Short]->acceptRestoreTraverser(traverser_);
            }),
            /**/)
        RESTORE_SETUP_TEARDOWN(
            LONG_WINDOW_7_9_TAG, m_Windows[E_Long] = this->newWindow(E_Long),
            m_Windows[E_Long] && traverser.traverseSubLevel([this](auto& traverser_) {
                return m_Windows[E_Long]->acceptRestoreTraverser(traverser_);
            }),
            /**/)
    } while (traverser.next());
    return true;
}

void CTimeSeriesDecompositionDetail::CSeasonalityTest::acceptPersistInserter(
    core::CStatePersistInserter& inserter) const {
    inserter.insertLevel(SEASONALITY_TEST_MACHINE_6_3_TAG, [this](auto& inserter_) {
        m_Machine.acceptPersistInserter(inserter_);
    });
    if (m_Windows[E_Short] != nullptr) {
        inserter.insertLevel(SHORT_WINDOW_7_9_TAG, [this](auto& inserter_) {
            m_Windows[E_Short]->acceptPersistInserter(inserter_);
        });
    }
    if (m_Windows[E_Long] != nullptr) {
        inserter.insertLevel(LONG_WINDOW_7_9_TAG, [this](auto& inserter_) {
            m_Windows[E_Long]->acceptPersistInserter(inserter_);
        });
    }
}

void CTimeSeriesDecompositionDetail::CSeasonalityTest::swap(CSeasonalityTest& other) {
    std::swap(m_Machine, other.m_Machine);
    std::swap(m_DecayRate, other.m_DecayRate);
    std::swap(m_BucketLength, other.m_BucketLength);
    m_Windows[E_Short].swap(other.m_Windows[E_Short]);
    m_Windows[E_Long].swap(other.m_Windows[E_Long]);
}

void CTimeSeriesDecompositionDetail::CSeasonalityTest::handle(const SAddValue& message) {

    core_t::TTime time{message.s_Time};
    double value{message.s_Value};
    double prediction{message.s_Seasonal + message.s_Calendar};
    // We have explicit handling of outliers and we can't accurately assess
    // them anyway before we've detected periodicity.
    double weight{maths_t::count(message.s_Weights)};

    this->test(message);

    switch (m_Machine.state()) {
    case PT_TEST:
        // The seasonality test memory can increase as we add new values
        // so we stop updating it in hard limit.
        if (message.s_MemoryCircuitBreaker.areAllocationsAllowed() == false) {
            break;
        }
        for (auto& window : m_Windows) {
            if (window != nullptr) {
                window->add(time, value, prediction, weight);
            }
        }
        break;
    case PT_NOT_TESTING:
        break;
    case PT_INITIAL:
        this->apply(PT_NEW_VALUE, message);
        this->handle(message);
        break;
    default:
        LOG_ERROR(<< "Test in a bad state: " << m_Machine.state());
        this->apply(PT_RESET, message);
        break;
    }
}

void CTimeSeriesDecompositionDetail::CSeasonalityTest::handle(const SDetectedTrend& message) {
    TPredictor predictor{message.s_Predictor};
    TComponentChangeCallback componentChangeCallback{message.s_ComponentChangeCallback};
    componentChangeCallback(this->residuals(predictor));
}

void CTimeSeriesDecompositionDetail::CSeasonalityTest::test(const SAddValue& message) {
    core_t::TTime time{message.s_Time};
    core_t::TTime lastTime{message.s_LastTime};
    double occupancy{message.s_Occupancy};
    const auto& makeTest = message.s_MakeTestForSeasonality;
    const auto& makePreconditioner = message.s_MakeSeasonalityTestPreconditioner;

    switch (m_Machine.state()) {
    case PT_TEST:
        for (auto i : {E_Short, E_Long}) {
            if (this->shouldTest(i, time)) {
                const auto& window = m_Windows[i];
                core_t::TTime minimumPeriod{
                    CSeasonalityTestParameters::shortestComponent(i, m_BucketLength)};
                std::size_t minimumResolutionToTestModelledComponent{
                    CSeasonalityTestParameters::minimumResolutionToTestModelledComponent(
                        i, m_BucketLength, window->haveShorterWindows())};
                auto seasonalityTest = makeTest(*window, minimumPeriod,
                                                minimumResolutionToTestModelledComponent,
                                                makePreconditioner(), occupancy);

                auto decomposition = seasonalityTest.decompose();
                if (decomposition.componentsChanged()) {
                    this->mediator()->forward(
                        SDetectedSeasonal{time, lastTime, std::move(decomposition),
                                          message.s_MemoryCircuitBreaker});
                }
            }
        }
        break;
    case PT_NOT_TESTING:
    case PT_INITIAL:
        break;
    default:
        LOG_ERROR(<< "Test in a bad state: " << m_Machine.state());
        this->apply(PT_RESET, message);
        break;
    }
}

void CTimeSeriesDecompositionDetail::CSeasonalityTest::shiftTime(core_t::TTime time,
                                                                 core_t::TTime shift) {
    for (auto& window : m_Windows) {
        if (window != nullptr) {
            window->shiftTime(time, shift);
        }
    }
}

void CTimeSeriesDecompositionDetail::CSeasonalityTest::propagateForwards(core_t::TTime start,
                                                                         core_t::TTime end) {
    if (m_Windows[E_Short] != nullptr) {
        stepwisePropagateForwards(start, end, DAY, [this](double time) {
            m_Windows[E_Short]->propagateForwardsByTime(time / 8.0);
        });
    }
    if (m_Windows[E_Long] != nullptr) {
        stepwisePropagateForwards(start, end, WEEK, [this](double time) {
            m_Windows[E_Long]->propagateForwardsByTime(time / 8.0);
        });
    }
}

CTimeSeriesDecompositionDetail::TFloatMeanAccumulatorVec
CTimeSeriesDecompositionDetail::CSeasonalityTest::residuals(const TPredictor& predictor) const {
    TFloatMeanAccumulatorVec result;
    for (auto i : {E_Short, E_Long}) {
        if (m_Windows[i] != nullptr) {
            // Add on any noise we smooth away by averaging over longer buckets.
            result = m_Windows[i]->valuesMinusPrediction(predictor);
            addMeanZeroNormalNoise(m_Windows[i]->withinBucketVariance(), result);
            break;
        }
    }
    return result;
}

std::uint64_t CTimeSeriesDecompositionDetail::CSeasonalityTest::checksum(std::uint64_t seed) const {
    seed = common::CChecksum::calculate(seed, m_Machine);
    seed = common::CChecksum::calculate(seed, m_DecayRate);
    seed = common::CChecksum::calculate(seed, m_BucketLength);
    return common::CChecksum::calculate(seed, m_Windows);
}

void CTimeSeriesDecompositionDetail::CSeasonalityTest::debugMemoryUsage(
    const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CSeasonalityTest");
    core::memory_debug::dynamicSize("m_Windows", m_Windows, mem);
}

std::size_t CTimeSeriesDecompositionDetail::CSeasonalityTest::memoryUsage() const {
    std::size_t usage{core::memory::dynamicSize(m_Windows)};
    if (m_Machine.state() == PT_INITIAL) {
        usage += this->extraMemoryOnInitialization();
    }
    return usage;
}

std::size_t CTimeSeriesDecompositionDetail::CSeasonalityTest::extraMemoryOnInitialization() const {
    static std::size_t result{0};
    if (result == 0) {
        for (auto i : {E_Short, E_Long}) {
            auto window = this->newWindow(i, false);
            // The 0.3 is a rule-of-thumb estimate of the worst case
            // compression ratio we achieve on the test state.
            result += static_cast<std::size_t>(
                0.3 * static_cast<double>(core::memory::dynamicSize(window)));
        }
    }
    return result;
}

void CTimeSeriesDecompositionDetail::CSeasonalityTest::apply(std::size_t symbol,
                                                             const SMessage& message) {
    core_t::TTime time{message.s_Time};

    std::size_t old{m_Machine.state()};
    m_Machine.apply(symbol);
    std::size_t state{m_Machine.state()};

    if (state != old) {
        LOG_TRACE(<< PT_STATES[old] << "," << PT_ALPHABET[symbol] << " -> "
                  << PT_STATES[state]);

        auto initialize = [time, this]() {
            for (auto i : {E_Short, E_Long}) {
                m_Windows[i] = this->newWindow(i);
                if (m_Windows[i] != nullptr) {
                    m_Windows[i]->initialize(common::CIntegerTools::floor(
                        time, CSeasonalityTestParameters::maxBucketLength(i, m_BucketLength)));
                }
            }
        };

        switch (state) {
        case PT_TEST:
            if (std::all_of(m_Windows.begin(), m_Windows.end(),
                            [](const auto& window) { return window == nullptr; })) {
                initialize();
            }
            break;
        case PT_INITIAL:
            initialize();
            break;
        case PT_NOT_TESTING:
            m_Windows[0].reset();
            m_Windows[1].reset();
            break;
        default:
            LOG_ERROR(<< "Test in a bad state: " << state);
            this->apply(PT_RESET, message);
            break;
        }
    }
}

CTimeSeriesDecompositionDetail::CSeasonalityTest::TExpandingWindowUPtr
CTimeSeriesDecompositionDetail::CSeasonalityTest::newWindow(ETest test, bool deflate) const {

    using TTimeCRng = CExpandingWindow::TTimeCRng;

    std::size_t numberBuckets{CSeasonalityTestParameters::numberBuckets(test, m_BucketLength)};
    const TTimeVec* bucketLengths{CSeasonalityTestParameters::bucketLengths(test, m_BucketLength)};

    if (bucketLengths != nullptr) {
        return std::make_unique<CExpandingWindow>(
            m_BucketLength, TTimeCRng{*bucketLengths, 0, bucketLengths->size()},
            numberBuckets, m_DecayRate, deflate);
    }

    return {};
}

bool CTimeSeriesDecompositionDetail::CSeasonalityTest::shouldTest(ETest test,
                                                                  core_t::TTime time) const {
    // We need to test more frequently than we compress because it
    // would significantly delay when we first detect short periodic
    // components for longer bucket lengths otherwise.
    auto scheduledTest = [&]() {
        core_t::TTime length{time - m_Windows[test]->beginValuesTime()};
        for (auto schedule : CSeasonalityTestParameters::testSchedule(test, m_BucketLength)) {
            if (length >= schedule && length < schedule + m_BucketLength) {
                return true;
            }
        }
        return false;
    };
    return m_Windows[test] != nullptr &&
           (m_Windows[test]->needToCompress(time) || scheduledTest());
}

//////// CCalendarCyclic ////////

CTimeSeriesDecompositionDetail::CCalendarTest::CCalendarTest(double decayRate,
                                                             core_t::TTime bucketLength)
    : m_Machine{core::CStateMachine::create(CC_ALPHABET,
                                            CC_STATES,
                                            CC_TRANSITION_FUNCTION,
                                            bucketLength > DAY ? CC_NOT_TESTING : CC_INITIAL)},
      m_DecayRate{decayRate}, m_BucketLength{bucketLength}, m_LastMonth{} {
}

CTimeSeriesDecompositionDetail::CCalendarTest::CCalendarTest(const CCalendarTest& other,
                                                             bool isForForecast)
    : m_Machine{other.m_Machine}, m_DecayRate{other.m_DecayRate},
      m_BucketLength{other.m_BucketLength}, m_LastMonth{other.m_LastMonth},
      m_Test{isForForecast == false && other.m_Test
                 ? std::make_unique<CCalendarCyclicTest>(*other.m_Test)
                 : nullptr} {
}

bool CTimeSeriesDecompositionDetail::CCalendarTest::acceptRestoreTraverser(core::CStateRestoreTraverser& traverser) {
    do {
        const std::string& name{traverser.name()};
        RESTORE(CALENDAR_TEST_MACHINE_6_3_TAG,
                traverser.traverseSubLevel([this](core::CStateRestoreTraverser& traverser_) {
                    return m_Machine.acceptRestoreTraverser(traverser_);
                }))
        RESTORE_BUILT_IN(LAST_MONTH_6_3_TAG, m_LastMonth)
        RESTORE_SETUP_TEARDOWN(
            CALENDAR_TEST_6_3_TAG,
            m_Test = std::make_unique<CCalendarCyclicTest>(m_BucketLength, m_DecayRate),
            traverser.traverseSubLevel([this](auto& traverser_) {
                return m_Test->acceptRestoreTraverser(traverser_);
            }),
            /**/)
    } while (traverser.next());
    return true;
}

void CTimeSeriesDecompositionDetail::CCalendarTest::acceptPersistInserter(
    core::CStatePersistInserter& inserter) const {
    inserter.insertLevel(CALENDAR_TEST_MACHINE_6_3_TAG, [this](auto& inserter_) {
        m_Machine.acceptPersistInserter(inserter_);
    });
    inserter.insertValue(LAST_MONTH_6_3_TAG, m_LastMonth);
    if (m_Test != nullptr) {
        inserter.insertLevel(CALENDAR_TEST_6_3_TAG, [this](auto& inserter_) {
            m_Test->acceptPersistInserter(inserter_);
        });
    }
}

void CTimeSeriesDecompositionDetail::CCalendarTest::swap(CCalendarTest& other) {
    std::swap(m_Machine, other.m_Machine);
    std::swap(m_DecayRate, other.m_DecayRate);
    std::swap(m_BucketLength, other.m_BucketLength);
    std::swap(m_LastMonth, other.m_LastMonth);
    m_Test.swap(other.m_Test);
}

void CTimeSeriesDecompositionDetail::CCalendarTest::handle(const SAddValue& message) {
    core_t::TTime time{message.s_Time};
    double value{message.s_Value};
    double error{message.s_Value - message.s_Trend - message.s_Seasonal -
                 message.s_Calendar};
    const maths_t::TDoubleWeightsAry& weights{message.s_Weights};

    this->test(message);

    switch (m_Machine.state()) {
    case CC_TEST:
        // The calendar test memory can increase as we add new values
        // so we stop updating it in hard limit.
        if (message.s_MemoryCircuitBreaker.areAllocationsAllowed() == false) {
            break;
        }
        m_Test->add(time, value, error, maths_t::countForUpdate(weights));
        break;
    case CC_NOT_TESTING:
        break;
    case CC_INITIAL:
        this->apply(CC_NEW_VALUE, message);
        this->handle(message);
        break;
    default:
        LOG_ERROR(<< "Test in a bad state: " << m_Machine.state());
        this->apply(CC_RESET, message);
        break;
    }
}

void CTimeSeriesDecompositionDetail::CCalendarTest::handle(const SDetectedSeasonal& message) {
    switch (m_Machine.state()) {
    case CC_TEST:
        m_Test->forgetErrorDistribution();
        break;
    case CC_NOT_TESTING:
    case CC_INITIAL:
        break;
    default:
        LOG_ERROR(<< "Test in a bad state: " << m_Machine.state());
        this->apply(CC_RESET, message);
        break;
    }
}

void CTimeSeriesDecompositionDetail::CCalendarTest::test(const SMessage& message) {
    core_t::TTime time{message.s_Time};
    core_t::TTime lastTime{message.s_LastTime};

    if (this->shouldTest(time)) {
        switch (m_Machine.state()) {
        case CC_TEST: {
            auto result = m_Test->test();
            for (auto component : result) {
                auto[feature, timeZoneOffset] = component;
                this->mediator()->forward(SDetectedCalendar(
                    time, lastTime, feature, timeZoneOffset, message.s_MemoryCircuitBreaker));
            }
            break;
        }
        case CC_NOT_TESTING:
        case CC_INITIAL:
            break;
        default:
            LOG_ERROR(<< "Test in a bad state: " << m_Machine.state());
            this->apply(CC_RESET, message);
            break;
        }
    }
}

void CTimeSeriesDecompositionDetail::CCalendarTest::propagateForwards(core_t::TTime start,
                                                                      core_t::TTime end) {
    if (m_Test != nullptr) {
        stepwisePropagateForwards(start, end, DAY, [this](double time) {
            m_Test->propagateForwardsByTime(time / 8.0);
        });
    }
}

std::uint64_t CTimeSeriesDecompositionDetail::CCalendarTest::checksum(std::uint64_t seed) const {
    seed = common::CChecksum::calculate(seed, m_Machine);
    seed = common::CChecksum::calculate(seed, m_DecayRate);
    seed = common::CChecksum::calculate(seed, m_BucketLength);
    seed = common::CChecksum::calculate(seed, m_LastMonth);
    return common::CChecksum::calculate(seed, m_Test);
}

void CTimeSeriesDecompositionDetail::CCalendarTest::debugMemoryUsage(
    const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CCalendarTest");
    core::memory_debug::dynamicSize("m_Test", m_Test, mem);
}

std::size_t CTimeSeriesDecompositionDetail::CCalendarTest::memoryUsage() const {
    std::size_t usage{core::memory::dynamicSize(m_Test)};
    if (m_Machine.state() == CC_INITIAL) {
        usage += this->extraMemoryOnInitialization();
    }
    return usage;
}

std::size_t CTimeSeriesDecompositionDetail::CCalendarTest::extraMemoryOnInitialization() const {
    static std::size_t result{0};
    if (result == 0) {
        TCalendarCyclicTestPtr test =
            std::make_unique<CCalendarCyclicTest>(m_BucketLength, m_DecayRate);
        result = core::memory::dynamicSize(test);
    }
    return result;
}

void CTimeSeriesDecompositionDetail::CCalendarTest::apply(std::size_t symbol,
                                                          const SMessage& message) {
    core_t::TTime time{message.s_Time};

    std::size_t old{m_Machine.state()};
    m_Machine.apply(symbol);
    std::size_t state{m_Machine.state()};

    if (state != old) {
        LOG_TRACE(<< CC_STATES[old] << "," << CC_ALPHABET[symbol] << " -> "
                  << CC_STATES[state]);

        switch (state) {
        case CC_TEST:
            if (m_Test == nullptr) {
                m_Test = std::make_unique<CCalendarCyclicTest>(m_BucketLength, m_DecayRate);
                m_LastMonth = this->month(time) + 2;
            }
            break;
        case CC_NOT_TESTING:
        case CC_INITIAL:
            m_Test.reset();
            m_LastMonth = int{};
            break;
        default:
            LOG_ERROR(<< "Test in a bad state: " << state);
            this->apply(CC_RESET, message);
            break;
        }
    }
}

bool CTimeSeriesDecompositionDetail::CCalendarTest::shouldTest(core_t::TTime time) {
    int month{this->month(time)};
    if (month == (m_LastMonth + 1) % 12) {
        m_LastMonth = month;
        return true;
    }
    return false;
}

int CTimeSeriesDecompositionDetail::CCalendarTest::month(core_t::TTime time) const {
    int dummy;
    int month;
    core::CTimezone::instance().dateFields(time, dummy, dummy, dummy, month, dummy, dummy);
    return month;
}

//////// CComponents ////////

CTimeSeriesDecompositionDetail::CComponents::CComponents(double decayRate,
                                                         core_t::TTime bucketLength,
                                                         std::size_t seasonalComponentSize)
    : m_Machine{core::CStateMachine::create(SC_ALPHABET, SC_STATES, SC_TRANSITION_FUNCTION, SC_NORMAL)},
      m_DecayRate{decayRate}, m_BucketLength{bucketLength}, m_SeasonalComponentSize{seasonalComponentSize},
      m_CalendarComponentSize{seasonalComponentSize / 3}, m_Trend{decayRate},
      m_ComponentChangeCallback{[](TFloatMeanAccumulatorVec) {}} {
}

CTimeSeriesDecompositionDetail::CComponents::CComponents(const CComponents& other)
    : m_Machine{other.m_Machine}, m_DecayRate{other.m_DecayRate},
      m_BucketLength{other.m_BucketLength}, m_GainController{other.m_GainController},
      m_SeasonalComponentSize{other.m_SeasonalComponentSize},
      m_CalendarComponentSize{other.m_CalendarComponentSize}, m_Trend{other.m_Trend},
      m_Seasonal{other.m_Seasonal ? std::make_unique<CSeasonal>(*other.m_Seasonal) : nullptr},
      m_Calendar{other.m_Calendar ? std::make_unique<CCalendar>(*other.m_Calendar) : nullptr},
      m_MeanVarianceScale{other.m_MeanVarianceScale},
      m_PredictionErrorWithoutTrend{other.m_PredictionErrorWithoutTrend},
      m_PredictionErrorWithTrend{other.m_PredictionErrorWithTrend},
      m_ComponentChangeCallback{[](TFloatMeanAccumulatorVec) {}},
      m_UsingTrendForPrediction{other.m_UsingTrendForPrediction} {
}

bool CTimeSeriesDecompositionDetail::CComponents::acceptRestoreTraverser(
    const common::SDistributionRestoreParams& params,
    core::CStateRestoreTraverser& traverser) {
    if (traverser.name() == VERSION_6_3_TAG) {
        while (traverser.next()) {
            const std::string& name{traverser.name()};
            RESTORE(COMPONENTS_MACHINE_6_3_TAG,
                    traverser.traverseSubLevel([this](core::CStateRestoreTraverser& traverser_) {
                        return m_Machine.acceptRestoreTraverser(traverser_);
                    }))
            RESTORE_BUILT_IN(DECAY_RATE_6_3_TAG, m_DecayRate)
            RESTORE(GAIN_CONTROLLER_6_3_TAG, traverser.traverseSubLevel([this](auto& traverser_) {
                return m_GainController.acceptRestoreTraverser(traverser_);
            }))
            RESTORE(TREND_6_3_TAG, traverser.traverseSubLevel([&](auto& traverser_) {
                return m_Trend.acceptRestoreTraverser(params, traverser_);
            }))
            RESTORE_SETUP_TEARDOWN(
                SEASONAL_6_3_TAG, m_Seasonal = std::make_unique<CSeasonal>(),
                traverser.traverseSubLevel([this](auto& traverser_) {
                    return m_Seasonal->acceptRestoreTraverser(
                        m_DecayRate, m_BucketLength, traverser_);
                }),
                /**/)
            RESTORE_SETUP_TEARDOWN(
                CALENDAR_6_3_TAG, m_Calendar = std::make_unique<CCalendar>(),
                traverser.traverseSubLevel([this](auto& traverser_) {
                    return m_Calendar->acceptRestoreTraverser(
                        m_DecayRate, m_BucketLength, traverser_);
                }),
                /**/)
            RESTORE(MEAN_VARIANCE_SCALE_6_3_TAG,
                    m_MeanVarianceScale.fromDelimited(traverser.value()))
            RESTORE(MOMENTS_6_3_TAG,
                    m_PredictionErrorWithoutTrend.fromDelimited(traverser.value()))
            RESTORE(MOMENTS_MINUS_TREND_6_3_TAG,
                    m_PredictionErrorWithTrend.fromDelimited(traverser.value()))
            RESTORE_BUILT_IN(USING_TREND_FOR_PREDICTION_6_3_TAG, m_UsingTrendForPrediction)
        }

        this->decayRate(m_DecayRate);
    } else {
        return false;
    }
    return true;
}

void CTimeSeriesDecompositionDetail::CComponents::acceptPersistInserter(
    core::CStatePersistInserter& inserter) const {

    inserter.insertValue(VERSION_6_3_TAG, "");
    inserter.insertLevel(COMPONENTS_MACHINE_6_3_TAG, [this](auto& inserter_) {
        m_Machine.acceptPersistInserter(inserter_);
    });
    inserter.insertValue(DECAY_RATE_6_3_TAG, m_DecayRate, core::CIEEE754::E_SinglePrecision);
    inserter.insertLevel(GAIN_CONTROLLER_6_3_TAG, [this](auto& inserter_) {
        m_GainController.acceptPersistInserter(inserter_);
    });
    inserter.insertLevel(TREND_6_3_TAG, [this](auto& inserter_) {
        m_Trend.acceptPersistInserter(inserter_);
    });
    if (m_Seasonal != nullptr) {
        inserter.insertLevel(SEASONAL_6_3_TAG, [this](auto& inserter_) {
            m_Seasonal->acceptPersistInserter(inserter_);
        });
    }
    if (m_Calendar != nullptr) {
        inserter.insertLevel(CALENDAR_6_3_TAG, [this](auto& inserter_) {
            m_Calendar->acceptPersistInserter(inserter_);
        });
    }
    inserter.insertValue(MEAN_VARIANCE_SCALE_6_3_TAG, m_MeanVarianceScale.toDelimited());
    inserter.insertValue(MOMENTS_6_3_TAG, m_PredictionErrorWithoutTrend.toDelimited());
    inserter.insertValue(MOMENTS_MINUS_TREND_6_3_TAG,
                         m_PredictionErrorWithTrend.toDelimited());
    inserter.insertValue(USING_TREND_FOR_PREDICTION_6_3_TAG, m_UsingTrendForPrediction);
}

void CTimeSeriesDecompositionDetail::CComponents::swap(CComponents& other) {
    std::swap(m_Machine, other.m_Machine);
    std::swap(m_DecayRate, other.m_DecayRate);
    std::swap(m_BucketLength, other.m_BucketLength);
    std::swap(m_SeasonalComponentSize, other.m_SeasonalComponentSize);
    std::swap(m_CalendarComponentSize, other.m_CalendarComponentSize);
    m_Trend.swap(other.m_Trend);
    m_Seasonal.swap(other.m_Seasonal);
    m_Calendar.swap(other.m_Calendar);
    std::swap(m_GainController, other.m_GainController);
    std::swap(m_MeanVarianceScale, other.m_MeanVarianceScale);
    std::swap(m_PredictionErrorWithoutTrend, other.m_PredictionErrorWithoutTrend);
    std::swap(m_PredictionErrorWithTrend, other.m_PredictionErrorWithTrend);
    std::swap(m_UsingTrendForPrediction, other.m_UsingTrendForPrediction);
}

void CTimeSeriesDecompositionDetail::CComponents::handle(const SAddValue& message) {
    switch (m_Machine.state()) {
    case SC_NORMAL:
    case SC_NEW_COMPONENTS: {
        this->interpolate(message);

        core_t::TTime time{message.s_Time};
        double value{message.s_Value};
        double trend{message.s_Trend};
        const maths_t::TDoubleWeightsAry& weights{message.s_Weights};
        const TMakePredictor& makePredictor{message.s_MakePredictor};

        TSeasonalComponentPtrVec seasonalComponents;
        TCalendarComponentPtrVec calendarComponents;
        TComponentErrorsPtrVec seasonalErrors;
        TComponentErrorsPtrVec calendarErrors;
        TDoubleVec deltas;

        if (m_Seasonal != nullptr) {
            m_Seasonal->componentsErrorsAndDeltas(time, seasonalComponents,
                                                  seasonalErrors, deltas);
        }
        if (m_Calendar != nullptr) {
            m_Calendar->componentsAndErrors(time, calendarComponents, calendarErrors);
        }

        double weight{maths_t::countForUpdate(weights)};
        double initialWeight{maths_t::count(weights)};
        std::size_t m{seasonalComponents.size()};
        std::size_t n{calendarComponents.size()};

        TDoubleVec values(m + n + 1, value);
        TDoubleVec predictions(m + n, 0.0);
        double referenceError;
        double error;
        double scale;
        decompose(trend, seasonalComponents, calendarComponents, time, deltas,
                  m_GainController.gain(), values, predictions, referenceError,
                  error, scale);

        TDoubleVec variances(m + n + 1, 0.0);
        if (m_UsingTrendForPrediction) {
            variances[0] = m_Trend.variance(0.0).mean();
        }
        for (std::size_t i = 1; i <= m; ++i) {
            variances[i] = seasonalComponents[i - 1]->variance(time, 0.0).mean();
        }
        for (std::size_t i = m + 1; i <= m + n; ++i) {
            variances[i] = calendarComponents[i - m - 1]->variance(time, 0.0).mean();
        }
        double variance{std::accumulate(variances.begin(), variances.end(), 0.0)};
        double expectedVarianceIncrease{1.0 / static_cast<double>(m + n + 1)};

        bool testForTrend{(m_UsingTrendForPrediction == false) &&
                          (m_Trend.observedInterval() > 6 * m_BucketLength)};

        m_Trend.add(time, values[0], weight);
        m_Trend.dontShiftLevel(time, value);
        for (std::size_t i = 1; i <= m; ++i) {
            CSeasonalComponent* component{seasonalComponents[i - 1]};
            CComponentErrors* error_{seasonalErrors[i - 1]};
            double varianceIncrease{variance == 0.0 ? 1.0 : variances[i] / variance / expectedVarianceIncrease};
            component->add(time, values[i],
                           component->initialized() ? weight : initialWeight, 0.5);
            error_->add(referenceError, error, predictions[i - 1], varianceIncrease, weight);
        }
        for (std::size_t i = m + 1; i <= m + n; ++i) {
            CCalendarComponent* component{calendarComponents[i - m - 1]};
            CComponentErrors* error_{calendarErrors[i - m - 1]};
            double varianceIncrease{variance == 0.0 ? 1.0 : variances[i] / variance / expectedVarianceIncrease};
            component->add(time, values[i], component->initialized() ? weight : initialWeight);
            error_->add(referenceError, error, predictions[i - 1], varianceIncrease, weight);
        }

        m_MeanVarianceScale.add(scale, weight);
        m_PredictionErrorWithoutTrend.add(error + trend, weight);
        m_PredictionErrorWithTrend.add(error, weight);
        m_GainController.add(time, predictions);

        if (testForTrend && this->shouldUseTrendForPrediction()) {
            LOG_DEBUG(<< "Detected trend at " << time);
            this->mediator()->forward(SDetectedTrend{makePredictor(), m_ComponentChangeCallback,
                                                     message.s_MemoryCircuitBreaker});
            m_ModelAnnotationCallback("Detected trend");
        }
    } break;
    case SC_DISABLED:
        break;
    default:
        LOG_ERROR(<< "Components in a bad state: " << m_Machine.state());
        this->apply(SC_RESET, message);
        break;
    }
}

void CTimeSeriesDecompositionDetail::CComponents::handle(const SDetectedSeasonal& message) {
    if (this->size() + m_SeasonalComponentSize > this->maxSize()) {
        return;
    }

    switch (m_Machine.state()) {
    case SC_NORMAL:
    case SC_NEW_COMPONENTS: {
        if (m_Seasonal == nullptr) {
            m_Seasonal = std::make_unique<CSeasonal>();
        }

        core_t::TTime time{message.s_Time};
        const auto& components = message.s_Components;
        LOG_DEBUG(<< "Detected change in seasonal components at " << time);

        this->addSeasonalComponents(components, message.s_MemoryCircuitBreaker);
        this->apply(SC_ADDED_COMPONENTS, message);
        break;
    }
    case SC_DISABLED:
        break;
    default:
        LOG_ERROR(<< "Components in a bad state: " << m_Machine.state());
        this->apply(SC_RESET, message);
        break;
    }
}

void CTimeSeriesDecompositionDetail::CComponents::handle(const SDetectedCalendar& message) {
    if (this->size() + m_CalendarComponentSize > this->maxSize()) {
        return;
    }

    switch (m_Machine.state()) {
    case SC_NORMAL:
    case SC_NEW_COMPONENTS: {
        if (m_Calendar == nullptr) {
            m_Calendar = std::make_unique<CCalendar>();
        }

        core_t::TTime time{message.s_Time};
        CCalendarFeature feature{message.s_Feature};
        core_t::TTime timeZoneOffset{message.s_TimeZoneOffset};

        if (m_Calendar->haveComponent(feature)) {
            break;
        }

        LOG_DEBUG(<< "Detected feature '" << feature.print() << "' at " << time);
        this->addCalendarComponent(feature, message.s_MemoryCircuitBreaker, timeZoneOffset);
        this->apply(SC_ADDED_COMPONENTS, message);
        break;
    }
    case SC_DISABLED:
        break;
    default:
        LOG_ERROR(<< "Components in a bad state: " << m_Machine.state());
        this->apply(SC_RESET, message);
        break;
    }
}

void CTimeSeriesDecompositionDetail::CComponents::handle(const SDetectedChangePoint& message) {
    core_t::TTime time{message.s_Time};
    const auto& change = *message.s_Change;
    change.apply(m_Trend);
    if (m_Seasonal != nullptr) {
        m_Seasonal->apply(change);
    }
    if (m_Calendar != nullptr) {
        m_Calendar->apply(change);
    }
    if (m_UsingTrendForPrediction == false) {
        m_ComponentChangeCallback(change.residuals());
        m_UsingTrendForPrediction = true;
    }
    LOG_DEBUG(<< "Detected " << change.print() << " at " << time);
    m_ModelAnnotationCallback("Detected " + change.print());
}

void CTimeSeriesDecompositionDetail::CComponents::interpolateForForecast(core_t::TTime time) {
    if (this->shouldInterpolate(time)) {
        if (m_Seasonal != nullptr) {
            m_Seasonal->interpolate(time, false);
        }
        if (m_Calendar != nullptr) {
            m_Calendar->interpolate(time, true);
        }
    }
}

void CTimeSeriesDecompositionDetail::CComponents::dataType(maths_t::EDataType dataType) {
    m_Trend.dataType(dataType);
}

void CTimeSeriesDecompositionDetail::CComponents::decayRate(double decayRate) {
    m_DecayRate = decayRate;
    m_Trend.decayRate(decayRate);
    if (m_Seasonal != nullptr) {
        m_Seasonal->decayRate(decayRate);
    }
    if (m_Calendar != nullptr) {
        m_Calendar->decayRate(decayRate);
    }
}

double CTimeSeriesDecompositionDetail::CComponents::decayRate() const {
    return m_DecayRate;
}

void CTimeSeriesDecompositionDetail::CComponents::propagateForwards(core_t::TTime start,
                                                                    core_t::TTime end) {
    m_Trend.propagateForwardsByTime(end - start);
    if (m_Seasonal != nullptr) {
        m_Seasonal->propagateForwards(start, end);
    }
    if (m_Calendar != nullptr) {
        m_Calendar->propagateForwards(start, end);
    }
    double factor{ageFactor(m_DecayRate, end - start)};
    m_MeanVarianceScale.age(factor);
    m_PredictionErrorWithTrend.age(factor);
    m_PredictionErrorWithoutTrend.age(factor);
    m_GainController.age(factor);
}

bool CTimeSeriesDecompositionDetail::CComponents::initialized() const {
    return m_UsingTrendForPrediction && m_Trend.initialized()
               ? true
               : (m_Seasonal != nullptr && m_Calendar != nullptr
                      ? m_Seasonal->initialized() || m_Calendar->initialized()
                      : (m_Seasonal != nullptr
                             ? m_Seasonal->initialized()
                             : (m_Calendar ? m_Calendar->initialized() : false)));
}

const CTrendComponent& CTimeSeriesDecompositionDetail::CComponents::trend() const {
    return m_Trend;
}

const TSeasonalComponentVec& CTimeSeriesDecompositionDetail::CComponents::seasonal() const {
    return m_Seasonal != nullptr ? m_Seasonal->components() : NO_SEASONAL_COMPONENTS;
}

const maths_t::TCalendarComponentVec&
CTimeSeriesDecompositionDetail::CComponents::calendar() const {
    return m_Calendar != nullptr ? m_Calendar->components() : NO_CALENDAR_COMPONENTS;
}

bool CTimeSeriesDecompositionDetail::CComponents::usingTrendForPrediction() const {
    return m_UsingTrendForPrediction;
}

void CTimeSeriesDecompositionDetail::CComponents::useTrendForPrediction() {
    m_UsingTrendForPrediction = true;
}

CTimeSeriesDecompositionDetail::TMakeTestForSeasonality
CTimeSeriesDecompositionDetail::CComponents::makeTestForSeasonality(
    const TMakeFilteredPredictor& makePredictor) const {
    return [makePredictor,
            this](const CExpandingWindow& window, core_t::TTime minimumPeriod,
                  std::size_t minimumResolutionToTestModelledComponent,
                  const TFilteredPredictor& preconditioner, double occupancy) {
        core_t::TTime valuesStartTime{window.beginValuesTime()};
        core_t::TTime windowBucketStartTime{window.bucketStartTime()};
        core_t::TTime windowBucketLength{window.bucketLength()};
        auto values = window.values();
        TBoolVec testableMask;
        for (const auto& component : this->seasonal()) {
            testableMask.push_back(CTimeSeriesTestForSeasonality::canTestModelledComponent(
                values, windowBucketStartTime, windowBucketLength, minimumPeriod,
                minimumResolutionToTestModelledComponent, component.time()));
        }
        values = window.valuesMinusPrediction(std::move(values), [&](core_t::TTime time) {
            return preconditioner(time, testableMask);
        });
        CTimeSeriesTestForSeasonality test(valuesStartTime, windowBucketStartTime,
                                           windowBucketLength, m_BucketLength,
                                           std::move(values), occupancy);

        test.minimumPeriod(minimumPeriod)
            .minimumModelSize(2 * m_SeasonalComponentSize / 3)
            .maximumModelSize(2 * m_SeasonalComponentSize)
            .sampleVariance(window.withinBucketVariance())
            .modelledSeasonalityPredictor(makePredictor());
        std::ptrdiff_t maximumNumberComponents{MAXIMUM_COMPONENTS};
        for (const auto& component : this->seasonal()) {
            test.addModelledSeasonality(component.time(), minimumResolutionToTestModelledComponent,
                                        component.size());
            --maximumNumberComponents;
        }
        test.maximumNumberOfComponents(maximumNumberComponents);
        test.prepareWindowForDecompose();

        return test;
    };
}

double CTimeSeriesDecompositionDetail::CComponents::meanValue(core_t::TTime time) const {
    return this->initialized()
               ? ((m_UsingTrendForPrediction ? m_Trend.value(time, 0.0).mean() : 0.0) +
                  meanOf(&CSeasonalComponent::meanValue, this->seasonal()))
               : 0.0;
}

double CTimeSeriesDecompositionDetail::CComponents::meanVariance() const {
    return this->initialized()
               ? ((m_UsingTrendForPrediction ? this->trend().variance(0.0).mean() : 0.0) +
                  meanOf(&CSeasonalComponent::meanVariance, this->seasonal()))
               : 0.0;
}

double CTimeSeriesDecompositionDetail::CComponents::meanVarianceScale() const {
    return common::CBasicStatistics::mean(m_MeanVarianceScale);
}

std::uint64_t CTimeSeriesDecompositionDetail::CComponents::checksum(std::uint64_t seed) const {
    seed = common::CChecksum::calculate(seed, m_Machine);
    seed = common::CChecksum::calculate(seed, m_DecayRate);
    seed = common::CChecksum::calculate(seed, m_BucketLength);
    seed = common::CChecksum::calculate(seed, m_SeasonalComponentSize);
    seed = common::CChecksum::calculate(seed, m_CalendarComponentSize);
    seed = common::CChecksum::calculate(seed, m_Trend);
    seed = common::CChecksum::calculate(seed, m_Seasonal);
    seed = common::CChecksum::calculate(seed, m_Calendar);
    seed = common::CChecksum::calculate(seed, m_MeanVarianceScale);
    seed = common::CChecksum::calculate(seed, m_PredictionErrorWithoutTrend);
    seed = common::CChecksum::calculate(seed, m_PredictionErrorWithTrend);
    seed = common::CChecksum::calculate(seed, m_GainController);
    return common::CChecksum::calculate(seed, m_UsingTrendForPrediction);
}

void CTimeSeriesDecompositionDetail::CComponents::debugMemoryUsage(
    const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CComponents");
    core::memory_debug::dynamicSize("m_Trend", m_Trend, mem);
    core::memory_debug::dynamicSize("m_Seasonal", m_Seasonal, mem);
    core::memory_debug::dynamicSize("m_Calendar", m_Calendar, mem);
}

std::size_t CTimeSeriesDecompositionDetail::CComponents::memoryUsage() const {
    return core::memory::dynamicSize(m_Trend) + core::memory::dynamicSize(m_Seasonal) +
           core::memory::dynamicSize(m_Calendar);
}

std::size_t CTimeSeriesDecompositionDetail::CComponents::size() const {
    return (m_Seasonal ? m_Seasonal->size() : 0) + (m_Calendar ? m_Calendar->size() : 0);
}

std::size_t CTimeSeriesDecompositionDetail::CComponents::maxSize() const {
    return MAXIMUM_COMPONENTS * m_SeasonalComponentSize;
}

void CTimeSeriesDecompositionDetail::CComponents::addSeasonalComponents(
    const CSeasonalDecomposition& components,
    const TMemoryCircuitBreaker& memoryCircuitBreaker) {

    LOG_TRACE(<< "remove mask = " << components.seasonalToRemoveMask());

    LOG_TRACE(<< "Estimate size change = "
              << m_Seasonal->estimateSizeChange(components, m_DecayRate, m_BucketLength));

    if (memoryCircuitBreaker.areAllocationsAllowed() == false &&
        m_Seasonal->estimateSizeChange(components, m_DecayRate, m_BucketLength) > 0) {
        // In the hard_limit state, we do not change the state of components if
        // adding new components will consume more memory than removing old ones.
        LOG_TRACE(<< "Not adding new seasonal components because we are in the hard limit state");
        return;
    }

    if (m_Seasonal->remove(components.seasonalToRemoveMask()) == false) {
        // We don't know how to apply the changes so just bail.
        return;
    }

    if (components.seasonal().empty()) {
        LOG_DEBUG(<< "removed all seasonality");
        m_ModelAnnotationCallback("removed all seasonality");
    }

    for (const auto& component : components.seasonal()) {

        m_Seasonal->add(component.createSeasonalComponent(
            m_DecayRate, static_cast<double>(m_BucketLength)));
        m_ModelAnnotationCallback(component.annotationText());
    }

    m_Seasonal->refreshForNewComponents();

    this->clearComponentErrors();

    core_t::TTime startTime{components.trend()->initialValuesStartTime()};
    core_t::TTime endTime{components.trend()->initialValuesEndTime()};
    core_t::TTime dt{components.trend()->bucketLength()};
    auto initialValues = components.trend()->initialValues();

    // Reinitialize the gain controller.
    TDoubleVec predictions;
    m_GainController.clear();
    for (core_t::TTime time = startTime; time < endTime; time += m_BucketLength) {
        predictions.clear();
        if (m_Seasonal != nullptr) {
            m_Seasonal->appendPredictions(time, predictions);
        }
        if (m_Calendar != nullptr) {
            m_Calendar->appendPredictions(time, predictions);
        }
        m_GainController.seed(predictions);
        m_GainController.age(ageFactor(m_DecayRate, m_BucketLength));
    }

    // Fit a trend model.
    CTrendComponent newTrend{m_Trend.defaultDecayRate()};
    this->fitTrend(startTime, dt, initialValues, newTrend);
    m_Trend.swap(newTrend);
    m_UsingTrendForPrediction = true;

    // Pass the residuals to the component changed callback.
    core_t::TTime time{startTime};
    for (std::size_t i = 0; i < initialValues.size(); ++i, time += dt) {
        if (common::CBasicStatistics::count(initialValues[i]) > 0.0) {
            common::CBasicStatistics::moment<0>(initialValues[i]) -=
                m_Trend.value(time, 0.0).mean();
        }
    }

    // We typically underestimate the values variance if the window bucket length
    // is longer than the job bucket length. This adds noise to the values we use
    // to reinitialize the residual model to compensate.
    addMeanZeroNormalNoise(components.withinBucketVariance(), initialValues);
    m_ComponentChangeCallback(std::move(initialValues));
}

void CTimeSeriesDecompositionDetail::CComponents::addCalendarComponent(
    const CCalendarFeature& feature,
    const TMemoryCircuitBreaker& allocator,
    core_t::TTime timeZoneOffset) {
    if (allocator.areAllocationsAllowed() == false) {
        // In the hard_limit state we are not adding any new components to the model.
        LOG_TRACE(<< "Not adding new calendar component because we are in the hard limit state");
        return;
    }

    double bucketLength{static_cast<double>(m_BucketLength)};
    m_Calendar->add(feature, timeZoneOffset, m_CalendarComponentSize, m_DecayRate, bucketLength);
    m_ModelAnnotationCallback("Detected calendar feature: " + feature.print());
}

void CTimeSeriesDecompositionDetail::CComponents::fitTrend(core_t::TTime startTime,
                                                           core_t::TTime dt,
                                                           const TFloatMeanAccumulatorVec& values,
                                                           CTrendComponent& trend) const {
    core_t::TTime time{startTime};
    for (const auto& value : values) {
        if (common::CBasicStatistics::count(value) > 0.0) {
            trend.add(time, common::CBasicStatistics::mean(value),
                      common::CBasicStatistics::count(value));
            trend.propagateForwardsByTime(dt);
        }
        time += dt;
    }
}

void CTimeSeriesDecompositionDetail::CComponents::clearComponentErrors() {
    if (m_Seasonal != nullptr) {
        m_Seasonal->clearPredictionErrors();
    }
    if (m_Calendar != nullptr) {
        m_Calendar->clearPredictionErrors();
    }
}

void CTimeSeriesDecompositionDetail::CComponents::apply(std::size_t symbol,
                                                        const SMessage& message) {
    if (symbol == SC_RESET) {
        m_Trend.clear();
        m_Seasonal.reset();
        m_Calendar.reset();
    }

    std::size_t old{m_Machine.state()};
    m_Machine.apply(symbol);
    std::size_t state{m_Machine.state()};

    if (state != old) {
        LOG_TRACE(<< SC_STATES[old] << "," << SC_ALPHABET[symbol] << " -> "
                  << SC_STATES[state]);

        switch (state) {
        case SC_NORMAL:
        case SC_NEW_COMPONENTS:
            this->interpolate(message);
            break;
        case SC_DISABLED:
            m_Trend.clear();
            m_Seasonal.reset();
            m_Calendar.reset();
            break;
        default:
            LOG_ERROR(<< "Components in a bad state: " << m_Machine.state());
            this->apply(SC_RESET, message);
            break;
        }
    }
}

bool CTimeSeriesDecompositionDetail::CComponents::shouldUseTrendForPrediction() {
    double v0{common::CBasicStatistics::variance(m_PredictionErrorWithoutTrend)};
    double v1{common::CBasicStatistics::variance(m_PredictionErrorWithTrend)};
    double df0{common::CBasicStatistics::count(m_PredictionErrorWithoutTrend) - 1.0};
    double df1{common::CBasicStatistics::count(m_PredictionErrorWithTrend) -
               m_Trend.parameters()};
    if (df0 > 0.0 && df1 > 0.0 && v0 > 0.0) {
        double relativeLogSignificance{
            common::CTools::fastLog(common::CStatisticalTests::leftTailFTest(v1, v0, df1, df0)) /
            common::CTools::fastLog(0.001)};
        double vt{0.6 * v0};
        double p{common::CTools::logisticFunction(relativeLogSignificance, 0.1, 1.0) *
                 (vt > v1 ? common::CTools::logisticFunction(vt / v1, 1.0, 1.0, +1.0)
                          : common::CTools::logisticFunction(v1 / vt, 0.1, 1.0, -1.0))};
        m_UsingTrendForPrediction = (p >= 0.25);
    }
    return m_UsingTrendForPrediction;
}

bool CTimeSeriesDecompositionDetail::CComponents::shouldInterpolate(core_t::TTime time) {
    return m_Machine.state() == SC_NEW_COMPONENTS ||
           (m_Seasonal && m_Seasonal->shouldInterpolate(time)) ||
           (m_Calendar && m_Calendar->shouldInterpolate(time));
}

void CTimeSeriesDecompositionDetail::CComponents::interpolate(const SMessage& message) {
    core_t::TTime time{message.s_Time};
    std::size_t state{m_Machine.state()};

    switch (state) {
    case SC_NORMAL:
    case SC_NEW_COMPONENTS:
        this->canonicalize(time);
        if (this->shouldInterpolate(time)) {
            LOG_TRACE(<< "Interpolating values at " << time);

            // As well as interpolating we also remove components that contain
            // invalid (not finite) values, along with the associated prediction
            // errors and signal that the set of components has been modified.

            if (m_Seasonal != nullptr) {
                if (m_Seasonal->removeComponentsWithBadValues(time)) {
                    m_ComponentChangeCallback({});
                }
                m_Seasonal->interpolate(time, true);
            }
            if (m_Calendar != nullptr) {
                if (m_Calendar->removeComponentsWithBadValues(time)) {
                    m_ComponentChangeCallback({});
                }
                m_Calendar->interpolate(time, true);
            }

            this->apply(SC_INTERPOLATED, message);
        }
        break;
    case SC_DISABLED:
        break;
    default:
        LOG_ERROR(<< "Components in a bad state: " << state);
        this->apply(SC_RESET, message);
        break;
    }
}

void CTimeSeriesDecompositionDetail::CComponents::shiftOrigin(core_t::TTime time) {
    time -= static_cast<core_t::TTime>(static_cast<double>(DAY) / m_DecayRate / 2.0);
    m_Trend.shiftOrigin(time);
    if (m_Seasonal != nullptr) {
        m_Seasonal->shiftOrigin(time);
    }
    m_GainController.shiftOrigin(time);
}

void CTimeSeriesDecompositionDetail::CComponents::canonicalize(core_t::TTime time) {

    // There is redundancy in the specification of the additive decomposition. For
    // any collection of models {m_i} then for any set of |{m_i}| constants {c_j}
    // satisfying sum_j c_j = 0 all models of the form m_i' = s_i + c_{j(i)} for
    // any permutation j(.) give the same predictions. Here we choose a canonical
    // form which minimises the values of the components to avoid issues with
    // cancellation errors.

    using TMinMaxAccumulator = common::CBasicStatistics::CMinMax<double>;

    this->shiftOrigin(time);

    if (m_Seasonal != nullptr && m_Seasonal->prune(time, m_BucketLength)) {
        m_Seasonal.reset();
    }
    if (m_Calendar != nullptr && m_Calendar->prune(time, m_BucketLength)) {
        m_Calendar.reset();
    }

    if (m_Seasonal != nullptr) {
        // Compute the sum level and slope for each separate window if the
        // components are time windowed.
        TTimeTimePrDoubleFMap levels;
        TTimeTimePrDoubleFMap slopes;
        TTimeTimePrDoubleFMap numberLevels;
        TTimeTimePrDoubleFMap numberSlopes;
        for (auto& component : m_Seasonal->components()) {
            auto window = component.time().windowed() ? component.time().window()
                                                      : TTimeTimePr{0, 0};
            levels[window] += component.meanValue();
            numberLevels[window] += 1.0;
            if (component.slopeAccurate(time)) {
                slopes[window] += component.slope();
                numberSlopes[window] += 1.0;
            }
        }

        TMinMaxAccumulator commonLevel;
        for (const auto& level : levels) {
            commonLevel.add(level.second);
        }
        if (commonLevel.signMargin() != 0.0) {
            for (auto& component : m_Seasonal->components()) {
                auto window = component.time().windowed() ? component.time().window()
                                                          : TTimeTimePr{0, 0};
                component.shiftLevel((levels[window] - commonLevel.signMargin()) /
                                         numberLevels[window] -
                                     component.meanValue());
            }
            m_Trend.shiftLevel(commonLevel.signMargin());
        }

        TMinMaxAccumulator commonSlope;
        for (const auto& slope : slopes) {
            commonSlope.add(slope.second);
        }
        if (commonSlope.signMargin() != 0.0) {
            for (auto& component : m_Seasonal->components()) {
                if (component.slopeAccurate(time)) {
                    auto window = component.time().windowed()
                                      ? component.time().window()
                                      : TTimeTimePr{0, 0};
                    component.shiftSlope(time, (slopes[window] - commonSlope.signMargin()) /
                                                       numberSlopes[window] -
                                                   component.slope());
                }
            }
            m_Trend.shiftSlope(time, commonSlope.signMargin());
        }
    }
}

CTimeSeriesDecompositionDetail::CComponents::CScopeAttachComponentChangeCallback::CScopeAttachComponentChangeCallback(
    CComponents& components,
    TComponentChangeCallback componentChangeCallback,
    maths_t::TModelAnnotationCallback modelAnnotationCallback)
    : m_Components{components} {
    components.m_ComponentChangeCallback = std::move(componentChangeCallback);
    components.m_ModelAnnotationCallback = std::move(modelAnnotationCallback);
}

CTimeSeriesDecompositionDetail::CComponents::CScopeAttachComponentChangeCallback::~CScopeAttachComponentChangeCallback() {
    m_Components.m_ComponentChangeCallback = [](TFloatMeanAccumulatorVec) {};
    m_Components.m_ModelAnnotationCallback = {};
}

bool CTimeSeriesDecompositionDetail::CComponents::CGainController::acceptRestoreTraverser(
    core::CStateRestoreTraverser& traverser) {
    do {
        const std::string& name{traverser.name()};
        RESTORE_BUILT_IN(REGRESSION_ORIGIN_6_4_TAG, m_RegressionOrigin)
        RESTORE(MEAN_SUM_AMPLITUDES_6_4_TAG,
                m_MeanSumAmplitudes.fromDelimited(traverser.value()))
        RESTORE(MEAN_SUM_AMPLITUDES_TREND_6_4_TAG,
                traverser.traverseSubLevel([this](auto& traverser_) {
                    return m_MeanSumAmplitudesTrend.acceptRestoreTraverser(traverser_);
                }))
    } while (traverser.next());
    return true;
}

void CTimeSeriesDecompositionDetail::CComponents::CGainController::acceptPersistInserter(
    core::CStatePersistInserter& inserter) const {
    inserter.insertValue(REGRESSION_ORIGIN_6_4_TAG, m_RegressionOrigin);
    inserter.insertValue(MEAN_SUM_AMPLITUDES_6_4_TAG, m_MeanSumAmplitudes.toDelimited());
    inserter.insertLevel(MEAN_SUM_AMPLITUDES_TREND_6_4_TAG, [this](auto& inserter_) {
        m_MeanSumAmplitudesTrend.acceptPersistInserter(inserter_);
    });
}

void CTimeSeriesDecompositionDetail::CComponents::CGainController::clear() {
    m_RegressionOrigin = 0;
    m_MeanSumAmplitudes = TFloatMeanAccumulator{};
    m_MeanSumAmplitudesTrend = TRegression{};
}

double CTimeSeriesDecompositionDetail::CComponents::CGainController::gain() const {
    if (m_MeanSumAmplitudesTrend.count() > 0.0) {
        TRegression::TArray params;
        m_MeanSumAmplitudesTrend.parameters(params);
        if (params[1] > 0.01 * common::CBasicStatistics::mean(m_MeanSumAmplitudes)) {
            // Anything less than one is sufficient to ensure that the basic update
            // dynamics are stable (poles of the Z-transform inside the unit circle).
            // There are however other factors at play which are hard to quantify
            // such as the sample weight and the fact that there's a lag detecting
            // instability. This gives us a margin for error.
            return 0.8;
        }
    }
    return 3.0;
}

void CTimeSeriesDecompositionDetail::CComponents::CGainController::seed(const TDoubleVec& predictions) {
    m_MeanSumAmplitudes.add(std::accumulate(
        predictions.begin(), predictions.end(), 0.0,
        [](double sum, double prediction) { return sum + std::fabs(prediction); }));
}

void CTimeSeriesDecompositionDetail::CComponents::CGainController::add(core_t::TTime time,
                                                                       const TDoubleVec& predictions) {
    if (predictions.empty() == false) {
        m_MeanSumAmplitudes.add(std::accumulate(
            predictions.begin(), predictions.end(), 0.0, [](double sum, double prediction) {
                return sum + std::fabs(prediction);
            }));
        m_MeanSumAmplitudesTrend.add(scaleTime(time, m_RegressionOrigin),
                                     common::CBasicStatistics::mean(m_MeanSumAmplitudes),
                                     common::CBasicStatistics::count(m_MeanSumAmplitudes));
    }
}

void CTimeSeriesDecompositionDetail::CComponents::CGainController::age(double factor) {
    m_MeanSumAmplitudes.age(factor);
    m_MeanSumAmplitudesTrend.age(factor);
}

void CTimeSeriesDecompositionDetail::CComponents::CGainController::shiftOrigin(core_t::TTime time) {
    time = common::CIntegerTools::floor(time, WEEK);
    if (time > m_RegressionOrigin) {
        m_MeanSumAmplitudesTrend.shiftAbscissa(-scaleTime(time, m_RegressionOrigin));
        m_RegressionOrigin = time;
    }
}

std::uint64_t
CTimeSeriesDecompositionDetail::CComponents::CGainController::checksum(std::uint64_t seed) const {
    seed = common::CChecksum::calculate(seed, m_RegressionOrigin);
    seed = common::CChecksum::calculate(seed, m_MeanSumAmplitudes);
    return common::CChecksum::calculate(seed, m_MeanSumAmplitudesTrend);
}

bool CTimeSeriesDecompositionDetail::CComponents::CComponentErrors::fromDelimited(const std::string& str_) {
    std::string str{str_};
    std::size_t n{str.find(common::CBasicStatistics::EXTERNAL_DELIMITER)};
    if (m_MeanErrors.fromDelimited(str.substr(0, n)) == false) {
        LOG_ERROR(<< "Failed to parse '" << str << "'");
        return false;
    }
    str = str.substr(n + 1);
    if (m_MaxVarianceIncrease.fromDelimited(str) == false) {
        LOG_ERROR(<< "Failed to parse '" << str << "'");
        return false;
    }
    return true;
}

std::string CTimeSeriesDecompositionDetail::CComponents::CComponentErrors::toDelimited() const {
    return m_MeanErrors.toDelimited() + common::CBasicStatistics::EXTERNAL_DELIMITER +
           m_MaxVarianceIncrease.toDelimited();
}

void CTimeSeriesDecompositionDetail::CComponents::CComponentErrors::add(double referenceError,
                                                                        double error,
                                                                        double prediction,
                                                                        double varianceIncrease,
                                                                        double weight) {
    TVector errors;
    errors(0) = common::CTools::pow2(referenceError);
    errors(1) = common::CTools::pow2(error);
    errors(2) = common::CTools::pow2(error + prediction);
    m_MeanErrors.add(this->winsorise(errors), weight);
    m_MaxVarianceIncrease.add(varianceIncrease);
}

void CTimeSeriesDecompositionDetail::CComponents::CComponentErrors::clear() {
    m_MeanErrors = TVectorMeanAccumulator{};
    m_MaxVarianceIncrease = TMaxAccumulator{};
}

bool CTimeSeriesDecompositionDetail::CComponents::CComponentErrors::remove(core_t::TTime bucketLength,
                                                                           core_t::TTime period) const {
    double history{common::CBasicStatistics::count(m_MeanErrors) *
                   static_cast<double>(bucketLength)};
    double errorWithNoComponents{common::CBasicStatistics::mean(m_MeanErrors)(0)};
    double errorWithComponent{common::CBasicStatistics::mean(m_MeanErrors)(1)};
    double errorWithoutComponent{common::CBasicStatistics::mean(m_MeanErrors)(2)};
    return (history > static_cast<double>(WEEK) && errorWithComponent > errorWithNoComponents) ||
           (history > 5.0 * static_cast<double>(period) &&
            m_MaxVarianceIncrease[0] < 1.2 && errorWithoutComponent <= errorWithComponent);
}

void CTimeSeriesDecompositionDetail::CComponents::CComponentErrors::age(double factor) {
    m_MeanErrors.age(factor);
    m_MaxVarianceIncrease.age(factor);
}

std::uint64_t
CTimeSeriesDecompositionDetail::CComponents::CComponentErrors::checksum(std::uint64_t seed) const {
    seed = common::CChecksum::calculate(seed, m_MeanErrors);
    return common::CChecksum::calculate(seed, m_MaxVarianceIncrease);
}

CTimeSeriesDecompositionDetail::CComponents::CComponentErrors::TVector
CTimeSeriesDecompositionDetail::CComponents::CComponentErrors::winsorise(const TVector& squareError) const {
    return common::CBasicStatistics::count(m_MeanErrors) > 10.0
               ? min(squareError, common::CFloatStorage{36} *
                                      common::CBasicStatistics::mean(m_MeanErrors))
               : squareError;
}

bool CTimeSeriesDecompositionDetail::CComponents::CSeasonal::acceptRestoreTraverser(
    double decayRate,
    core_t::TTime bucketLength_,
    core::CStateRestoreTraverser& traverser) {
    double bucketLength{static_cast<double>(bucketLength_)};
    if (traverser.name() == VERSION_6_4_TAG) {
        while (traverser.next()) {
            const std::string& name{traverser.name()};
            RESTORE_NO_ERROR(COMPONENT_6_4_TAG,
                             m_Components.emplace_back(decayRate, bucketLength, traverser))
            RESTORE(ERRORS_6_4_TAG,
                    core::CPersistUtils::restore(ERRORS_6_4_TAG, m_PredictionErrors, traverser))
        }
    } else {
        LOG_ERROR(<< "Input error: unsupported state serialization version '"
                  << traverser.name()
                  << "'. Currently supported minimum version: " << VERSION_6_4_TAG);
        return false;
    }
    return true;
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::acceptPersistInserter(
    core::CStatePersistInserter& inserter) const {
    inserter.insertValue(VERSION_6_4_TAG, "");
    for (const auto& component : m_Components) {
        inserter.insertLevel(COMPONENT_6_4_TAG, [&component](auto& inserter_) {
            component.acceptPersistInserter(inserter_);
        });
    }
    core::CPersistUtils::persist(ERRORS_6_4_TAG, m_PredictionErrors, inserter);
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::decayRate(double decayRate) {
    for (auto& component : m_Components) {
        component.decayRate(decayRate);
    }
}

bool CTimeSeriesDecompositionDetail::CComponents::CSeasonal::removeComponentsWithBadValues(core_t::TTime time) {

    TBoolVec remove(m_Components.size(), false);
    bool anyBadComponentsFound{false};
    for (std::size_t i = 0; i < m_Components.size(); ++i) {
        const CSeasonalTime& time_{m_Components[i].time()};
        if (m_Components[i].isBad()) {
            LOG_DEBUG(<< "Removing seasonal component"
                      << " with period '"
                      << core::CTimeUtils::durationToString(time_.period())
                      << "' at " << time << ". Invalid values detected.");
            remove[i] = true;
            anyBadComponentsFound |= true;
        }
    }

    if (anyBadComponentsFound) {
        common::CSetTools::simultaneousRemoveIf(
            [](bool remove_) { return remove_; }, remove, m_Components, m_PredictionErrors);
    }
    return anyBadComponentsFound;
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::propagateForwards(core_t::TTime start,
                                                                               core_t::TTime end) {
    for (std::size_t i = 0; i < m_Components.size(); ++i) {
        core_t::TTime period{m_Components[i].time().period()};
        stepwisePropagateForwards(start, end, period, [&](double time) {
            m_Components[i].propagateForwardsByTime(time / 6.0, 0.25);
            m_PredictionErrors[i].age(std::exp(-m_Components[i].decayRate() * time));
        });
    }
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::clearPredictionErrors() {
    for (auto& errors : m_PredictionErrors) {
        errors.clear();
    }
}

std::size_t CTimeSeriesDecompositionDetail::CComponents::CSeasonal::size() const {
    std::size_t result{0};
    for (const auto& component : m_Components) {
        result += component.size();
    }
    return result;
}

std::ptrdiff_t CTimeSeriesDecompositionDetail::CComponents::CSeasonal::estimateSizeChange(
    const CSeasonalDecomposition& components,
    double decayRate,
    double bucketLength) const {
    // loop over components that will be removed and compute total size
    const CSeasonalDecomposition::TBoolVec& removeComponentsMask =
        components.seasonalToRemoveMask();
    if (removeComponentsMask.size() != m_Components.size()) {
        LOG_ERROR(<< "Unexpected seasonal components to remove " << removeComponentsMask
                  << ". Have " << m_Components.size() << " components.");
        return 0; //The size change is 0 because the attempt to remove seasonal components will fail.
    }
    std::ptrdiff_t removeComponentsSize{0};
    for (std::size_t i = 0; i < removeComponentsMask.size(); ++i) {
        if (removeComponentsMask[i] == true) {
            removeComponentsSize += core::memory::dynamicSize(m_Components[i]);
        }
    }

    // loop over components that will be added and compute total size
    std::ptrdiff_t addComponentsSize{0};
    for (const auto& component : components.seasonal()) {
        addComponentsSize += core::memory::dynamicSize(
            component.createSeasonalComponent(decayRate, bucketLength));
    }
    LOG_TRACE(<< "Add components size: " << addComponentsSize
              << ", remove components size: " << removeComponentsSize
              << ", difference: " << addComponentsSize - removeComponentsSize << ".");

    // compute difference between components to be added and removed
    return addComponentsSize - removeComponentsSize;
}

const maths_t::TSeasonalComponentVec&
CTimeSeriesDecompositionDetail::CComponents::CSeasonal::components() const {
    return m_Components;
}

maths_t::TSeasonalComponentVec&
CTimeSeriesDecompositionDetail::CComponents::CSeasonal::components() {
    return m_Components;
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::componentsErrorsAndDeltas(
    core_t::TTime time,
    TSeasonalComponentPtrVec& components,
    TComponentErrorsPtrVec& errors,
    TDoubleVec& deltas) {
    std::size_t n{m_Components.size()};

    components.reserve(n);
    errors.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        if (m_Components[i].time().inWindow(time)) {
            components.push_back(&m_Components[i]);
            errors.push_back(&m_PredictionErrors[i]);
        }
    }

    deltas.resize(components.size(), 0.0);
    for (std::size_t i = 1; i < components.size(); ++i) {
        core_t::TTime period{components[i]->time().period()};
        for (int j{static_cast<int>(i - 1)}; j > -1; --j) {
            core_t::TTime period_{components[j]->time().period()};
            if (period % period_ == 0) {
                double value{components[j]->value(time, 0.0).mean() -
                             components[j]->meanValue()};
                double delta{0.1 * components[i]->delta(time, period_, value)};
                deltas[j] += delta;
                deltas[i] -= delta;
                break;
            }
        }
    }
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::appendPredictions(
    core_t::TTime time,
    TDoubleVec& predictions) const {
    predictions.reserve(predictions.size() + m_Components.size());
    for (const auto& component : m_Components) {
        if (component.time().inWindow(time)) {
            predictions.push_back(component.value(time, 0.0).mean() - component.meanValue());
        }
    }
}

bool CTimeSeriesDecompositionDetail::CComponents::CSeasonal::shouldInterpolate(core_t::TTime time) const {
    for (const auto& component : m_Components) {
        if (component.shouldInterpolate(time)) {
            return true;
        }
    }
    return false;
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::interpolate(core_t::TTime time,
                                                                         bool refine) {
    for (auto& component : m_Components) {
        if (component.shouldInterpolate(time)) {
            component.interpolate(time, refine);
        }
    }
}

bool CTimeSeriesDecompositionDetail::CComponents::CSeasonal::initialized() const {
    for (const auto& component : m_Components) {
        if (component.initialized()) {
            return true;
        }
    }
    return false;
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::add(CSeasonalComponent&& component) {
    m_Components.push_back(std::move(component));
    m_PredictionErrors.emplace_back();
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::apply(const CChangePoint& change) {
    for (std::size_t i = 0; i < m_Components.size(); ++i) {
        if (change.apply(m_Components[i])) {
            m_PredictionErrors[i].clear();
        }
    }
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::refreshForNewComponents() {
    common::COrderings::simultaneousSortWith(
        [](const CSeasonalComponent& lhs, const CSeasonalComponent& rhs) {
            return lhs.time() < rhs.time();
        },
        m_Components, m_PredictionErrors);
}

bool CTimeSeriesDecompositionDetail::CComponents::CSeasonal::remove(const TBoolVec& removeComponentsMask) {
    if (removeComponentsMask.size() != m_Components.size()) {
        LOG_ERROR(<< "Unexpected seasonal components to remove " << removeComponentsMask
                  << ". Have " << m_Components.size() << " components.");
        return false;
    }
    std::size_t end{0};
    for (std::size_t i = 0; i < removeComponentsMask.size();
         end += removeComponentsMask[i++] ? 0 : 1) {
        if (i != end) {
            std::swap(m_Components[end], m_Components[i]);
            std::swap(m_PredictionErrors[end], m_PredictionErrors[i]);
        }
    }
    m_Components.erase(m_Components.begin() + end, m_Components.end());
    m_PredictionErrors.erase(m_PredictionErrors.begin() + end,
                             m_PredictionErrors.end());
    return true;
}

bool CTimeSeriesDecompositionDetail::CComponents::CSeasonal::prune(core_t::TTime time,
                                                                   core_t::TTime bucketLength) {
    std::size_t n{m_Components.size()};

    if (n > 1) {
        TTimeTimePrSizeFMap windowed;
        windowed.reserve(n);
        for (const auto& component : m_Components) {
            const CSeasonalTime& time_{component.time()};
            if (time_.windowed()) {
                ++windowed[time_.window()];
            }
        }

        TBoolVec remove(n, false);
        TTimeTimePrDoubleFMap shifts;
        shifts.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const CSeasonalTime& time_{m_Components[i].time()};
            auto j = windowed.find(time_.window());
            if (j == windowed.end() || j->second > 1) {
                if (m_PredictionErrors[i].remove(bucketLength, time_.period())) {
                    LOG_DEBUG(<< "Removing seasonal component"
                              << " with period '"
                              << core::CTimeUtils::durationToString(time_.period())
                              << "' at " << time);
                    remove[i] = true;
                    shifts[time_.window()] += m_Components[i].meanValue();
                    --j->second;
                }
            }
        }

        common::CSetTools::simultaneousRemoveIf(
            [](bool remove_) { return remove_; }, remove, m_Components, m_PredictionErrors);

        for (auto& shift : shifts) {
            if (windowed.count(shift.first) > 0) {
                for (auto& component : m_Components) {
                    if (shift.first == component.time().window()) {
                        component.shiftLevel(shift.second);
                        break;
                    }
                }
            } else {
                bool fallback = true;
                for (auto& component : m_Components) {
                    if (component.time().windowed() == false) {
                        component.shiftLevel(shift.second);
                        fallback = false;
                        break;
                    }
                }
                if (fallback) {
                    TTimeTimePrVec shifted;
                    shifted.reserve(m_Components.size());
                    for (auto& component : m_Components) {
                        const CSeasonalTime& time_ = component.time();
                        auto containsWindow = [&time_](const TTimeTimePr& window) {
                            return (time_.windowEnd() <= window.first ||
                                    time_.windowStart() >= window.second) == false;
                        };
                        if (std::find_if(shifted.begin(), shifted.end(),
                                         containsWindow) == shifted.end()) {
                            component.shiftLevel(shift.second);
                        }
                    }
                }
            }
        }
    }

    return m_Components.empty();
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::shiftOrigin(core_t::TTime time) {
    for (auto& component : m_Components) {
        component.shiftOrigin(time);
    }
}

std::uint64_t
CTimeSeriesDecompositionDetail::CComponents::CSeasonal::checksum(std::uint64_t seed) const {
    seed = common::CChecksum::calculate(seed, m_Components);
    return common::CChecksum::calculate(seed, m_PredictionErrors);
}

void CTimeSeriesDecompositionDetail::CComponents::CSeasonal::debugMemoryUsage(
    const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CSeasonal");
    core::memory_debug::dynamicSize("m_Components", m_Components, mem);
    core::memory_debug::dynamicSize("m_PredictionErrors", m_PredictionErrors, mem);
}

std::size_t CTimeSeriesDecompositionDetail::CComponents::CSeasonal::memoryUsage() const {
    return core::memory::dynamicSize(m_Components) +
           core::memory::dynamicSize(m_PredictionErrors);
}

bool CTimeSeriesDecompositionDetail::CComponents::CCalendar::acceptRestoreTraverser(
    double decayRate,
    core_t::TTime bucketLength_,
    core::CStateRestoreTraverser& traverser) {
    double bucketLength{static_cast<double>(bucketLength_)};
    if (traverser.name() == VERSION_6_4_TAG) {
        while (traverser.next()) {
            const std::string& name{traverser.name()};
            RESTORE_NO_ERROR(COMPONENT_6_4_TAG,
                             m_Components.emplace_back(decayRate, bucketLength, traverser))
            RESTORE(ERRORS_6_4_TAG,
                    core::CPersistUtils::restore(ERRORS_6_4_TAG, m_PredictionErrors, traverser))
        }
    } else {
        LOG_ERROR(<< "Input error: unsupported state serialization version '"
                  << traverser.name()
                  << "'. Currently supported minimum version: " << VERSION_6_4_TAG);
        return false;
    }
    return true;
}

void CTimeSeriesDecompositionDetail::CComponents::CCalendar::acceptPersistInserter(
    core::CStatePersistInserter& inserter) const {
    inserter.insertValue(VERSION_6_4_TAG, "");
    for (const auto& component : m_Components) {
        inserter.insertLevel(COMPONENT_6_4_TAG, [&component](auto& inserter_) {
            return component.acceptPersistInserter(inserter_);
        });
    }
    core::CPersistUtils::persist(ERRORS_6_4_TAG, m_PredictionErrors, inserter);
}

void CTimeSeriesDecompositionDetail::CComponents::CCalendar::decayRate(double decayRate) {
    for (auto& component : m_Components) {
        component.decayRate(decayRate);
    }
}

void CTimeSeriesDecompositionDetail::CComponents::CCalendar::propagateForwards(core_t::TTime start,
                                                                               core_t::TTime end) {
    for (std::size_t i = 0; i < m_Components.size(); ++i) {
        stepwisePropagateForwards(start, end, MONTH, [&](double time) {
            m_Components[i].propagateForwardsByTime(time / 6.0);
            m_PredictionErrors[i].age(std::exp(-m_Components[i].decayRate() * time));
        });
    }
}

void CTimeSeriesDecompositionDetail::CComponents::CCalendar::clearPredictionErrors() {
    for (auto& errors : m_PredictionErrors) {
        errors.clear();
    }
}

std::size_t CTimeSeriesDecompositionDetail::CComponents::CCalendar::size() const {
    std::size_t result{0};
    for (const auto& component : m_Components) {
        result += component.size();
    }
    return result;
}

const maths_t::TCalendarComponentVec&
CTimeSeriesDecompositionDetail::CComponents::CCalendar::components() const {
    return m_Components;
}

bool CTimeSeriesDecompositionDetail::CComponents::CCalendar::haveComponent(CCalendarFeature feature) const {
    for (const auto& component : m_Components) {
        if (component.feature() == feature) {
            return true;
        }
    }
    return false;
}

void CTimeSeriesDecompositionDetail::CComponents::CCalendar::componentsAndErrors(
    core_t::TTime time,
    TCalendarComponentPtrVec& components,
    TComponentErrorsPtrVec& errors) {
    std::size_t n = m_Components.size();
    components.reserve(n);
    errors.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (m_Components[i].feature().inWindow(time)) {
            components.push_back(&m_Components[i]);
            errors.push_back(&m_PredictionErrors[i]);
        }
    }
}

void CTimeSeriesDecompositionDetail::CComponents::CCalendar::appendPredictions(
    core_t::TTime time,
    TDoubleVec& predictions) const {
    predictions.reserve(predictions.size() + m_Components.size());
    for (const auto& component : m_Components) {
        if (component.feature().inWindow(time)) {
            predictions.push_back(component.value(time, 0.0).mean() - component.meanValue());
        }
    }
}

bool CTimeSeriesDecompositionDetail::CComponents::CCalendar::shouldInterpolate(core_t::TTime time) const {
    return std::any_of(m_Components.begin(), m_Components.end(), [&](const auto& component) {
        return component.shouldInterpolate(time);
    });
}

void CTimeSeriesDecompositionDetail::CComponents::CCalendar::interpolate(core_t::TTime time,
                                                                         bool refine) {
    for (auto& component : m_Components) {
        if (component.shouldInterpolate(time)) {
            component.interpolate(time, refine);
        }
    }
}

bool CTimeSeriesDecompositionDetail::CComponents::CCalendar::initialized() const {
    return std::any_of(m_Components.begin(), m_Components.end(), [&](const auto& component) {
        return component.initialized();
    });
}

void CTimeSeriesDecompositionDetail::CComponents::CCalendar::add(const CCalendarFeature& feature,
                                                                 core_t::TTime timeZoneOffset,
                                                                 std::size_t size,
                                                                 double decayRate,
                                                                 double bucketLength) {
    m_Components.emplace_back(feature, timeZoneOffset, size, decayRate,
                              bucketLength, common::CSplineTypes::E_Natural);
    m_Components.back().initialize();
    m_PredictionErrors.resize(m_Components.size());
}

void CTimeSeriesDecompositionDetail::CComponents::CCalendar::apply(const CChangePoint& change) {
    for (auto& component : m_Components) {
        change.apply(component);
    }
}

bool CTimeSeriesDecompositionDetail::CComponents::CCalendar::prune(core_t::TTime time,
                                                                   core_t::TTime bucketLength) {
    TBoolVec remove(m_Components.size(), false);
    for (std::size_t i = 0; i < m_Components.size(); ++i) {
        if (m_PredictionErrors[i].remove(bucketLength, m_Components[i].feature().window())) {
            LOG_DEBUG(<< "Removing calendar component"
                      << " '" << m_Components[i].feature().print() << "' at " << time);
            remove[i] = true;
        }
    }

    common::CSetTools::simultaneousRemoveIf([](bool remove_) { return remove_; },
                                            remove, m_Components, m_PredictionErrors);

    return m_Components.empty();
}

bool CTimeSeriesDecompositionDetail::CComponents::CCalendar::removeComponentsWithBadValues(core_t::TTime time) {

    TBoolVec remove(m_Components.size(), false);
    bool anyBadComponentsFound{false};
    for (std::size_t i = 0; i < m_Components.size(); ++i) {
        if (m_Components[i].isBad()) {
            LOG_DEBUG(<< "Removing calendar component"
                      << " '" << m_Components[i].feature().print() << "' at "
                      << time << ". Invalid value detected.");
            remove[i] = true;
            anyBadComponentsFound |= true;
        }
    }

    if (anyBadComponentsFound) {
        common::CSetTools::simultaneousRemoveIf(
            [](bool remove_) { return remove_; }, remove, m_Components, m_PredictionErrors);
        return true;
    }

    return false;
}

std::uint64_t
CTimeSeriesDecompositionDetail::CComponents::CCalendar::checksum(std::uint64_t seed) const {
    seed = common::CChecksum::calculate(seed, m_Components);
    return common::CChecksum::calculate(seed, m_PredictionErrors);
}

void CTimeSeriesDecompositionDetail::CComponents::CCalendar::debugMemoryUsage(
    const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CCalendar");
    core::memory_debug::dynamicSize("m_Components", m_Components, mem);
    core::memory_debug::dynamicSize("m_PredictionErrors", m_PredictionErrors, mem);
}

std::size_t CTimeSeriesDecompositionDetail::CComponents::CCalendar::memoryUsage() const {
    return core::memory::dynamicSize(m_Components) +
           core::memory::dynamicSize(m_PredictionErrors);
}
}
}
}
