#*******************************************************************************
#  Copyright (c) 2015, 2026 logi.cals GmbH, Frank Pagliughi <fpagliughi@mindspring.com> and others
#
#  All rights reserved. This program and the accompanying materials
#  are made available under the terms of the Eclipse Public License v2.0
#  and Eclipse Distribution License v1.0 which accompany this distribution.
#
#  The Eclipse Public License is available at
#     https://www.eclipse.org/legal/epl-2.0/
#  and the Eclipse Distribution License is available at
#    http://www.eclipse.org/org/documents/edl-v10.php.
#
#  Contributors:
#     Rainer Poisel - initial version
#     Genis Riera Perez - Add support for building debian package
#*******************************************************************************/

cmake_minimum_required(VERSION 3.12)

project("Eclipse Paho C" 
  VERSION 1.3.16
  LANGUAGES C
)

message(STATUS "CMake version: " ${CMAKE_VERSION})
message(STATUS "CMake system name: " ${CMAKE_SYSTEM_NAME})

set(CMAKE_SCRIPTS "${PROJECT_SOURCE_DIR}/cmake")
set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/modules")

## Project Version
## Previously we read in the version from these files, but now we use the 
## CMake project setting. We just make sure the files and CMake match.
file(READ version.major PAHO_VERSION_MAJOR)
file(READ version.minor PAHO_VERSION_MINOR)
file(READ version.patch PAHO_VERSION_PATCH)
set(CLIENT_VERSION ${PAHO_VERSION_MAJOR}.${PAHO_VERSION_MINOR}.${PAHO_VERSION_PATCH})

if(NOT (CLIENT_VERSION VERSION_EQUAL PROJECT_VERSION))
  message(FATAL_ERROR "CMake project version does NOT match. CMake: ${PROJECT_VERSION}, Files: ${CLIENT_VERSION}")
endif()

include(GNUInstallDirs)

string(TIMESTAMP BUILD_TIMESTAMP UTC)
message(STATUS "Timestamp is ${BUILD_TIMESTAMP}")

if(WIN32)
  add_definitions(-D_CRT_SECURE_NO_DEPRECATE -DWIN32_LEAN_AND_MEAN)
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
  add_definitions(-DOSX)
endif()

## build options
option(PAHO_WITH_SSL "Flag that defines whether to build ssl-enabled binaries too. " FALSE)
option(PAHO_WITH_LIBRESSL "Flag that defines whether to build ssl-enabled binaries with LibreSSL instead of OpenSSL. " FALSE)
option(PAHO_WITH_LIBUUID "Flag that defines whether libuuid or a custom uuid implementation should be used" FALSE)
option(PAHO_BUILD_SHARED "Build shared library" TRUE)
option(PAHO_BUILD_STATIC "Build static library" FALSE)
option(PAHO_BUILD_DOCUMENTATION "Create and install the HTML based API documentation (requires Doxygen)" FALSE)
option(PAHO_BUILD_SAMPLES "Build sample programs" FALSE)
option(PAHO_BUILD_DEB_PACKAGE "Build debian package" FALSE)
option(PAHO_ENABLE_TESTING "Build tests and run" TRUE)
option(PAHO_ENABLE_CPACK "Enable CPack" TRUE)
option(PAHO_HIGH_PERFORMANCE "Disable tracing and heap tracking" FALSE)
option(PAHO_USE_SELECT "Revert to select system call instead of poll" FALSE)
option(PAHO_NO_TCP_NODELAY "Don't disable Nagle's algorithm on TCP sockets" FALSE)

if(NOT WIN32)
    option(PAHO_WITH_UNIX_SOCKETS "Flag that defines whether to enable Unix-domain sockets" FALSE)

    if(PAHO_WITH_UNIX_SOCKETS)
      add_definitions(-DUNIXSOCK=1)
    endif()
endif()

if(PAHO_HIGH_PERFORMANCE)
  add_definitions(-DHIGH_PERFORMANCE=1)
endif()

if(PAHO_USE_SELECT)
  add_definitions(-DUSE_SELECT=1)
endif()

if(PAHO_WITH_LIBUUID)
  add_definitions(-DUSE_LIBUUID=1)
endif()

if(PAHO_NO_TCP_NODELAY)
  add_definitions(-DNO_TCP_NODELAY=1)
endif()

if(NOT PAHO_BUILD_SHARED AND NOT PAHO_BUILD_STATIC)
  message(FATAL_ERROR "You must set either PAHO_BUILD_SHARED, PAHO_BUILD_STATIC, or both")
endif()

if(PAHO_BUILD_SAMPLES AND NOT (PAHO_WITH_SSL OR PAHO_WITH_LIBRESSL))
  message(WARNING "You must build with SSL to build the full set of samples")
endif()

if(PAHO_BUILD_DEB_PACKAGE)
  set(CMAKE_INSTALL_DOCDIR share/doc/libpaho-mqtt)
  set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
  set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS_POLICY ">=")
endif()

add_subdirectory(src)

if(PAHO_BUILD_SAMPLES)
  add_subdirectory(src/samples)
endif()

if(PAHO_BUILD_DOCUMENTATION)
  add_subdirectory(doc)
endif()

if(PAHO_ENABLE_CPACK)
  ### packaging settings
  file(GLOB samples "src/samples/*.c")
  install(FILES ${samples} DESTINATION ${CMAKE_INSTALL_DOCDIR}/samples)

  set(CPACK_PACKAGE_VENDOR "Eclipse Paho")
  set(CPACK_PACKAGE_NAME "Eclipse-Paho-MQTT-C")
  install(FILES CONTRIBUTING.md epl-v20 edl-v10 README.md notice.html DESTINATION ${CMAKE_INSTALL_DOCDIR})

  if(WIN32)
    set(CPACK_GENERATOR "ZIP")
  elseif(PAHO_BUILD_DEB_PACKAGE)
    install(FILES CONTRIBUTING.md epl-v20 edl-v10 README.md notice.html DESTINATION ${CMAKE_INSTALL_DOCDIR})

    set(CPACK_GENERATOR "DEB")
    configure_file(${CMAKE_SCRIPTS}/CPackDebConfig.cmake.in
      ${CMAKE_BINARY_DIR}/CPackDebConfig.cmake @ONLY
    )
    set(CPACK_PROJECT_CONFIG_FILE ${CMAKE_BINARY_DIR}/CPackDebConfig.cmake)
   else()
     set(CPACK_GENERATOR "TGZ")
   endif()
else()
  file(GLOB samples "src/samples/*.c")
  install(FILES ${samples} DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

include(CPack)

if(PAHO_ENABLE_TESTING)
  enable_testing()
  include_directories(test src)
  add_subdirectory(test)
else()
  include_directories(src)
endif()
