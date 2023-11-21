#*******************************************************************************
#  Copyright (c) 2015, 2023 logi.cals GmbH and others
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

# Note: on OS X you should install XCode and the associated command-line tools

CMAKE_MINIMUM_REQUIRED(VERSION 2.8.12)
PROJECT("Eclipse Paho C" C)
MESSAGE(STATUS "CMake version: " ${CMAKE_VERSION})
MESSAGE(STATUS "CMake system name: " ${CMAKE_SYSTEM_NAME})

SET(CMAKE_SCRIPTS "${CMAKE_SOURCE_DIR}/cmake")
SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

## build settings
file(READ version.major PAHO_VERSION_MAJOR)
file(READ version.minor PAHO_VERSION_MINOR)
file(READ version.patch PAHO_VERSION_PATCH)
SET(CLIENT_VERSION ${PAHO_VERSION_MAJOR}.${PAHO_VERSION_MINOR}.${PAHO_VERSION_PATCH})

INCLUDE(GNUInstallDirs)

STRING(TIMESTAMP BUILD_TIMESTAMP UTC)
MESSAGE(STATUS "Timestamp is ${BUILD_TIMESTAMP}")

IF(WIN32)
  ADD_DEFINITIONS(-D_CRT_SECURE_NO_DEPRECATE -DWIN32_LEAN_AND_MEAN)
ELSEIF(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
  ADD_DEFINITIONS(-DOSX)
ENDIF()

## build options
SET(PAHO_WITH_SSL FALSE CACHE BOOL "Flag that defines whether to build ssl-enabled binaries too. ")
SET(PAHO_WITH_LIBUUID FALSE CACHE BOOL "Flag that defines whether libuuid or a custom uuid implementation should be used")
SET(PAHO_BUILD_SHARED TRUE CACHE BOOL "Build shared library")
SET(PAHO_BUILD_STATIC FALSE CACHE BOOL "Build static library")
SET(PAHO_BUILD_DOCUMENTATION FALSE CACHE BOOL "Create and install the HTML based API documentation (requires Doxygen)")
SET(PAHO_BUILD_SAMPLES FALSE CACHE BOOL "Build sample programs")
SET(PAHO_BUILD_DEB_PACKAGE FALSE CACHE BOOL "Build debian package")
SET(PAHO_ENABLE_TESTING TRUE CACHE BOOL "Build tests and run")
SET(PAHO_ENABLE_CPACK TRUE CACHE BOOL "Enable CPack")
SET(PAHO_HIGH_PERFORMANCE FALSE CACHE BOOL "Disable tracing and heap tracking")
SET(PAHO_USE_SELECT FALSE CACHE BOOL "Revert to select system call instead of poll")

IF (PAHO_HIGH_PERFORMANCE)
  ADD_DEFINITIONS(-DHIGH_PERFORMANCE=1)
ENDIF()

IF (PAHO_USE_SELECT)
  ADD_DEFINITIONS(-DUSE_SELECT=1)
ENDIF()

IF (PAHO_WITH_LIBUUID)
  ADD_DEFINITIONS(-DUSE_LIBUUID=1)
ENDIF()

IF (NOT PAHO_BUILD_SHARED AND NOT PAHO_BUILD_STATIC)
    MESSAGE(FATAL_ERROR "You must set either PAHO_BUILD_SHARED, PAHO_BUILD_STATIC, or both")
ENDIF()

IF (PAHO_BUILD_SAMPLES AND NOT PAHO_WITH_SSL)
    MESSAGE(FATAL_ERROR "You must build with SSL to build the samples")
ENDIF()

IF(PAHO_BUILD_DEB_PACKAGE)
    set(CMAKE_INSTALL_DOCDIR share/doc/libpaho-mqtt)
    set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
    set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS_POLICY ">=")
ENDIF()

ADD_SUBDIRECTORY(src)
IF(PAHO_BUILD_SAMPLES)
    ADD_SUBDIRECTORY(src/samples)
ENDIF()

IF(PAHO_BUILD_DOCUMENTATION)
    ADD_SUBDIRECTORY(doc)
ENDIF()

IF (PAHO_ENABLE_CPACK)
    ### packaging settings
    FILE(GLOB samples "src/samples/*.c")
    INSTALL(FILES ${samples} DESTINATION ${CMAKE_INSTALL_DOCDIR}/samples)

    SET(CPACK_PACKAGE_VENDOR "Eclipse Paho")
    SET(CPACK_PACKAGE_NAME "Eclipse-Paho-MQTT-C")
    INSTALL(FILES CONTRIBUTING.md epl-v20 edl-v10 README.md notice.html DESTINATION ${CMAKE_INSTALL_DOCDIR})

    IF (WIN32)
        SET(CPACK_GENERATOR "ZIP")
    ELSEIF(PAHO_BUILD_DEB_PACKAGE)
        INSTALL(FILES CONTRIBUTING.md epl-v20 edl-v10 README.md notice.html DESTINATION ${CMAKE_INSTALL_DOCDIR})

        SET(CPACK_GENERATOR "DEB")
        CONFIGURE_FILE(${CMAKE_SCRIPTS}/CPackDebConfig.cmake.in
                ${CMAKE_BINARY_DIR}/CPackDebConfig.cmake @ONLY)
        SET(CPACK_PROJECT_CONFIG_FILE ${CMAKE_BINARY_DIR}/CPackDebConfig.cmake)
    ELSE()
            SET(CPACK_GENERATOR "TGZ")
    ENDIF()
ELSE()
       FILE(GLOB samples "src/samples/*.c")
       INSTALL(FILES ${samples} DESTINATION ${CMAKE_INSTALL_DOCDIR})
ENDIF()

SET(CPACK_PACKAGE_VERSION_MAJOR ${PAHO_VERSION_MAJOR})
SET(CPACK_PACKAGE_VERSION_MINOR ${PAHO_VERSION_MINOR})
SET(CPACK_PACKAGE_VERSION_PATCH ${PAHO_VERSION_PATCH})
INCLUDE(CPack)

IF(PAHO_ENABLE_TESTING)
    ENABLE_TESTING()
    INCLUDE_DIRECTORIES(test src)
    ADD_SUBDIRECTORY(test)
ELSE()
    INCLUDE_DIRECTORIES(src)
ENDIF()
