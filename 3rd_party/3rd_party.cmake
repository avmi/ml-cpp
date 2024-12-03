#
# Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
# or more contributor license agreements. Licensed under the Elastic License
# 2.0 and the following additional limitation. Functionality enabled by the
# files subject to the Elastic License 2.0 may only be used in production when
# invoked by an Elasticsearch process with a license key installed that permits
# use of machine learning features. You may not use this file except in
# compliance with the Elastic License 2.0 and the foregoing additional
# limitation.
#

#
# Various other C programs/libraries need to be installed on the local machine,
# in directories listed in the platform specific variables listed below.
#
# Usage:
#  cmake -D INSTALL_DIR=${INSTALL_DIR} -P 3rd_party.cmake
#
# A complication is that Java's zip functionality turns symlinks into actual
# files, so we need to ensure libraries are only picked up by the name that the
# dynamic loader will look for, and no symlinks are used.  Otherwise the
# installer becomes very bloated.
#
if(NOT INSTALL_DIR)
  message(FATAL_ERROR "INSTALL_DIR not specified")
endif()

string(TOLOWER ${CMAKE_HOST_SYSTEM_NAME} HOST_SYSTEM_NAME)
message(STATUS "3rd_party: HOST_SYSTEM_NAME=${HOST_SYSTEM_NAME}")

if ("${HOST_SYSTEM_NAME}" STREQUAL "windows")
  # We only support x86_64
  set(HOST_SYSTEM_PROCESSOR "x86_64")
else()
  execute_process(COMMAND uname -m OUTPUT_VARIABLE HOST_SYSTEM_PROCESSOR OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REPLACE arm aarch HOST_SYSTEM_PROCESSOR ${HOST_SYSTEM_PROCESSOR})
endif()
message(STATUS "3rd_party: HOST_SYSTEM_PROCESSOR=${HOST_SYSTEM_PROCESSOR}")
set(CMAKE_TOOLCHAIN_FILE "cmake/${HOST_SYSTEM_NAME}-${HOST_SYSTEM_PROCESSOR}.cmake")

set(ARCH ${HOST_SYSTEM_PROCESSOR})

if ("${HOST_SYSTEM_NAME}" STREQUAL "darwin")
  message(STATUS "3rd_party: Copying macOS 3rd party libraries")
  set(BOOST_LOCATION "/usr/local/lib")
  set(BOOST_COMPILER "clang")
  if( "${ARCH}" STREQUAL "x86_64" )
    set(BOOST_ARCH "x64")
  else()
    set(BOOST_ARCH "a64")
  endif()
  set(BOOST_EXTENSION "mt-${BOOST_ARCH}-1_86.dylib")
  set(BOOST_LIBRARIES "atomic" "chrono" "date_time" "filesystem" "iostreams" "log" "log_setup" "program_options" "regex" "system" "thread" "unit_test_framework")
  set(XML_LOCATION)
  set(GCC_RT_LOCATION)
  set(STL_LOCATION)
  set(OMP_LOCATION)
  set(ZLIB_LOCATION)
  set(TORCH_LIBRARIES "torch_cpu" "c10")
  set(TORCH_LOCATION "/usr/local/lib")
  set(TORCH_EXTENSION ".dylib")
elseif ("${HOST_SYSTEM_NAME}" STREQUAL "linux")
  if(NOT DEFINED ENV{CPP_CROSS_COMPILE} OR "$ENV{CPP_CROSS_COMPILE}" STREQUAL "")
    message(STATUS "3rd_party: NOT cross compiling. Copying Linux 3rd party libraries")
    set(BOOST_LOCATION "/usr/local/gcc133/lib")
    set(BOOST_COMPILER "gcc")
    if( "${ARCH}" STREQUAL "aarch64" )
      set(BOOST_ARCH "a64")
    else()
      set(BOOST_ARCH "x64")
      set(MKL_LOCATION "/usr/local/gcc133/lib")
      set(MKL_EXTENSION ".so.2")
      set(MKL_PREFIX "libmkl_")
      set(MKL_LIBRARIES "avx2" "avx512" "core" "def" "gnu_thread" "intel_lp64" "mc3" "vml_avx2" "vml_avx512" "vml_cmpt" "vml_def" "vml_mc3")
    endif()
    set(BOOST_EXTENSION mt-${BOOST_ARCH}-1_83.so.1.83.0)
    set(BOOST_LIBRARIES "atomic" "chrono" "date_time" "filesystem" "iostreams" "log" "log_setup" "program_options" "regex" "system" "thread" "unit_test_framework")
    set(XML_LOCATION "/usr/local/gcc133/lib")
    set(XML_EXTENSION ".so.2")
    set(GCC_RT_LOCATION "/usr/local/gcc133/lib64")
    set(GCC_RT_EXTENSION ".so.1")
    set(STL_LOCATION "/usr/local/gcc133/lib64")
    set(STL_PATTERN "libstdc++")
    set(STL_EXTENSION ".so.6")
    set(OMP_LOCATION "/usr/local/gcc133/lib64")
    set(OMP_PATTERN "libgomp")
    set(OMP_EXTENSION ".so.1")
    set(ZLIB_LOCATION)
    set(TORCH_LIBRARIES "libtorch_cpu" "libc10")
    set(TORCH_LOCATION "/usr/local/gcc133/lib")
    set(TORCH_EXTENSION ".so")
  elseif("$ENV{CPP_CROSS_COMPILE}" STREQUAL "macosx")
    message(STATUS "3rd_party: Cross compile for macosx: Copying macOS 3rd party libraries")
    set(SYSROOT "/usr/local/sysroot-x86_64-apple-macosx10.14")
    set(BOOST_LOCATION "${SYSROOT}/usr/local/lib")
    set(BOOST_COMPILER "clang")
    set(BOOST_EXTENSION "mt-x64-1_83.dylib")
    set(BOOST_LIBRARIES "atomic" "chrono" "date_time" "filesystem" "iostreams" "log" "log_setup" "program_options" "regex" "system" "thread" "unit_test_framework")
    set(XML_LOCATION)
    set(GCC_RT_LOCATION)
    set(STL_LOCATION)
    set(OMP_LOCATION)
    set(ZLIB_LOCATION)
    set(TORCH_LIBRARIES "libtorch_cpu" "libc10")
    set(TORCH_LOCATION "${SYSROOT}/usr/local/lib")
    set(TORCH_EXTENSION ".dylib")
  else()
    message(STATUS "3rd_party: Cross compile for linux-aarch64: Copying Linux 3rd party libraries")
    set(SYSROOT "/usr/local/sysroot-$ENV{CPP_CROSS_COMPILE}-linux-gnu")
    set(BOOST_LOCATION "${SYSROOT}/usr/local/gcc133/lib")
    set(BOOST_COMPILER "gcc")
    if("$ENV{CPP_CROSS_COMPILE}" STREQUAL "aarch64")
      set(BOOST_ARCH "a64")
    else()
      message(FATAL_ERROR "Cannot cross compile to $ENV{CPP_CROSS_COMPILE}")
      return()
    endif()
    set(BOOST_EXTENSION "mt-${BOOST_ARCH}-1_83.so.1.83.0")
    set(BOOST_LIBRARIES "atomic" "chrono" "date_time" "filesystem" "iostreams" "log" "log_setup" "program_options" "regex" "system" "thread" "unit_test_framework")
    set(XML_LOCATION "${SYSROOT}/usr/local/gcc133/lib")
    set(XML_EXTENSION ".so.2")
    set(GCC_RT_LOCATION "${SYSROOT}/usr/local/gcc133/lib64")
    set(GCC_RT_EXTENSION ".so.1")
    set(STL_LOCATION "${SYSROOT}/usr/local/gcc133/lib64")
    set(STL_PATTERN "libstdc++")
    set(STL_EXTENSION ".so.6")
    set(OMP_LOCATION "${SYSROOT}/usr/local/gcc133/lib64")
    set(OMP_PATTERN "libgomp")
    set(OMP_EXTENSION ".so.1")
    set(ZLIB_LOCATION)
    set(TORCH_LIBRARIES "libtorch_cpu" "libc10")
    set(TORCH_LOCATION "${SYSROOT}/usr/local/gcc133/lib")
    set(TORCH_EXTENSION ".so")
  endif()
else()
  message(STATUS "Copying Windows 3rd party libraries")
  set(LOCAL_DRIVE "C:")
  if(DEFINED ENV{LOCAL_DRIVE})
    set(LOCAL_DRIVE $ENV{LOCAL_DRIVE})
  endif()
  # These directories are correct for the way our Windows 2016 build
  # server is currently set up
  set(BOOST_LOCATION "${LOCAL_DRIVE}/usr/local/lib")
  set(BOOST_COMPILER "vc")
  set(BOOST_EXTENSION "mt-x64-1_86.dll")
  set(BOOST_LIBRARIES "atomic" "chrono" "date_time" "filesystem" "iostreams" "log" "log_setup" "program_options" "regex" "system" "thread" "unit_test_framework")
  set(XML_LOCATION "${LOCAL_DRIVE}/usr/local/bin")
  set(XML_EXTENSION ".dll")
  set(GCC_RT_LOCATION)
  # Read VCBASE from environment if defined, otherwise default to VS Professional 2019
  message(STATUS "3rd_party: \$ENV{VCBASE} $ENV{VCBASE}")
  set(VCBASE "${LOCAL_DRIVE}/Program Files (x86)/Microsoft Visual Studio/2019/Professional")
  if (DEFINED ENV{VCBASE})
    set(VCBASE $ENV{VCBASE})
  endif()
  file(GLOB MSVC_VERS "${VCBASE}/VC/Tools/MSVC/*")

  list(GET MSVC_VERS -1 MSVC_VER)
  if(${MSVC_VER} MATCHES "/([^/]+)$")
    set(VCVER ${CMAKE_MATCH_1})
  endif()
  message(STATUS "VCVER: ${VCVER}")

  set(STL_LOCATION "${LOCAL_DRIVE}/${VCBASE}/VC/Redist/MSVC/${VCVER}/x64/Microsoft.VC142.CRT")
  set(STL_PATTERN "140")
  set(STL_EXTENSION ".dll")
  set(OMP_LOCATION "${LOCAL_DRIVE}/${VCBASE}/VC/Redist/MSVC/${VCVER}/x64/Microsoft.VC142.OpenMP")
  set(OMP_PATTERN "vcomp")
  set(OMP_EXTENSION ".dll")
  set(ZLIB_LOCATION "${LOCAL_DRIVE}/usr/local/bin")
  set(ZLIB_EXTENSION "1.dll")
  set(TORCH_LIBRARIES "asmjit" "c10" "fbgemm" "torch_cpu")
  set(TORCH_LOCATION "${LOCAL_DRIVE}/usr/local/bin")
  set(TORCH_EXTENSION ".dll")
endif()

function(install_libs _target _source_dir _prefix _postfix)

  if(NOT EXISTS ${_source_dir})
    return()
  endif()

  set(LIBRARIES ${ARGN})

  file(GLOB _LIBS ${_source_dir}/*${_prefix}*${_postfix})

  if(_LIBS)
    foreach(LIBRARY ${LIBRARIES})
      file(GLOB _INST_LIBS ${_source_dir}/*${_prefix}${LIBRARY}*${_postfix})
      file(GLOB _OLD_LIBS ${INSTALL_DIR}/*${_prefix}${LIBRARY}*${_postfix})
      if (_INST_LIBS)
        foreach(_OLD_LIB ${_OLD_LIBS})
          if(_OLD_LIB)
            file(REMOVE ${_OLD_LIB})
          endif()
        endforeach()
        foreach(_INST_LIB ${_INST_LIBS})
          # Resolve symlinks so that we have a handle
          # on a plain file.
          file(REAL_PATH ${_INST_LIB} _RESOLVED_PATH)
          # Strip off any leading directories
          set(_RESOLVED_LIB)
          if(${_RESOLVED_PATH} MATCHES "/([^/]+)$")
            set(_RESOLVED_LIB ${CMAKE_MATCH_1})
          endif()
          if(${_RESOLVED_PATH} MATCHES "/([^/]+${_postfix})")
            set(_LIB ${CMAKE_MATCH_1})
            file(COPY ${_RESOLVED_PATH} DESTINATION ${INSTALL_DIR})
            # The file we copied may not be precisely the name that we want as we had to resolve symlinks.
            # If so we need to rename it.
            if(NOT EXISTS ${INSTALL_DIR}/${_LIB})
              file(RENAME ${INSTALL_DIR}/${_RESOLVED_LIB} ${INSTALL_DIR}/${_LIB})
            endif()
            file(CHMOD ${INSTALL_DIR}/${_LIB} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
          else()
            file(COPY ${_RESOLVED_PATH} DESTINATION ${INSTALL_DIR})
            file(CHMOD ${INSTALL_DIR}/${_RESOLVED_LIB} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
          endif()
        endforeach()
      endif()
    endforeach()
  else()
    message(FATAL_ERROR "${_target} not found")
    return()
  endif()
endfunction()

file(MAKE_DIRECTORY "${INSTALL_DIR}")

install_libs("Boost" ${BOOST_LOCATION} "boost*" "${BOOST_COMPILER}*${BOOST_EXTENSION}" ${BOOST_LIBRARIES})
install_libs("libxml2" ${XML_LOCATION} "" "${XML_EXTENSION}" "libxml2")
install_libs("gcc runtime library" ${GCC_RT_LOCATION} "" "${GCC_RT_EXTENSION}" "libgcc_s")
install_libs("C++ standard library" ${STL_LOCATION} "" "${STL_EXTENSION}" "${STL_PATTERN}")
install_libs("OMP runtime library" ${OMP_LOCATION} "" "${OMP_EXTENSION}" "${OMP_PATTERN}")
install_libs("zlib" ${ZLIB_LOCATION} "" "${ZLIB_EXTENSION}" "zlib")
install_libs("Torch libraries" ${TORCH_LOCATION} "" "${TORCH_EXTENSION}" "${TORCH_LIBRARIES}")
install_libs("Intel MKL libraries" ${MKL_LOCATION} "${MKL_PREFIX}" "${MKL_EXTENSION}" "${MKL_LIBRARIES}")

# On Linux, replace the RPATH for 3rd party libraries that already have one.
# (Only Linux targets will have a location for the gcc runtime library.)
if (GCC_RT_LOCATION)
  execute_process(COMMAND find . -type f COMMAND egrep -v "^core|-debug$|libMl" COMMAND xargs COMMAND sed -e "s/ /;/g" OUTPUT_VARIABLE FOUND_LIBRARIES WORKING_DIRECTORY "${INSTALL_DIR}" OUTPUT_STRIP_TRAILING_WHITESPACE)
  foreach(LIBRARY ${FOUND_LIBRARIES})
    execute_process(COMMAND patchelf --print-rpath ${LIBRARY} COMMAND grep lib OUTPUT_VARIABLE RPATH_VAR ERROR_VARIABLE RPATH_ERR WORKING_DIRECTORY "${INSTALL_DIR}" OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(RPATH_VAR)
      message(STATUS "Attempting to overwrite existing RPATH ${RPATH_VAR} in ${LIBRARY}")
      execute_process(COMMAND patchelf --force-rpath --set-rpath "$ORIGIN" ${LIBRARY} OUTPUT_VARIABLE SET_RPATH_OUT ERROR_VARIABLE SET_RPATH_ERR WORKING_DIRECTORY "${INSTALL_DIR}" OUTPUT_STRIP_TRAILING_WHITESPACE)
      if(SET_RPATH_ERR)
        message(FATAL_ERROR "Error setting RPATH in ${LIBRARY}: ${SET_RPATH_ERR}")
      else()
        message(STATUS "Set RPATH in ${LIBRARY}")
      endif()
    else()
      message(STATUS "Did not set RPATH in ${LIBRARY}")
    endif()
  endforeach()
endif()
