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
#     Ian Craggs (IBM Corp.) - merge master
#     Ian Craggs - update for MQTTV5 support
#*******************************************************************************/

# Note: on OS X you should install XCode and the associated command-line tools

## compilation/linkage settings

CONFIGURE_FILE(VersionInfo.h.in
    ${CMAKE_BINARY_DIR}/VersionInfo.h
    @ONLY
    )

SET(common_src
  MQTTTime.c
  MQTTProtocolClient.c
  Clients.c
  utf-8.c
  MQTTPacket.c
  MQTTPacketOut.c
  Messages.c
  Tree.c
  Socket.c
  Log.c
  MQTTPersistence.c
  Thread.c
  MQTTProtocolOut.c
  MQTTPersistenceDefault.c
  SocketBuffer.c
  LinkedList.c
  MQTTProperties.c
  MQTTReasonCodes.c
  Base64.c
  SHA1.c
  WebSocket.c
  Proxy.c
)

IF (NOT PAHO_HIGH_PERFORMANCE)
  SET(common_src ${common_src}
    StackTrace.c
    Heap.c
    )
ENDIF()

IF (WIN32)
    SET(LIBS_SYSTEM ws2_32 crypt32 RpcRT4)
ELSEIF (UNIX)
    IF(CMAKE_SYSTEM_NAME MATCHES "Linux")
        SET(LIBS_SYSTEM c dl pthread rt)
        # anl is only available with glibc so check if it is found before using
        # it or build will fail on uclibc or musl
        FIND_LIBRARY(LIB_ANL anl)
        IF(LIB_ANL)
            SET(LIBS_SYSTEM "${LIBS_SYSTEM}" anl)
        ENDIF()
        IF(PAHO_WITH_LIBUUID)
            SET(LIBS_SYSTEM "${LIBS_SYSTEM}" uuid)
        ENDIF()
        ADD_DEFINITIONS(-D_GNU_SOURCE -fvisibility=hidden)
    ELSEIF (CMAKE_SYSTEM_NAME MATCHES "Android")
        SET(LIBS_SYSTEM c dl)
    ELSEIF (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
        SET(LIBS_SYSTEM compat pthread)
    ELSE()
        SET(LIBS_SYSTEM c pthread)
    ENDIF()
ENDIF()

IF (PAHO_BUILD_SHARED)
# common compilation for libpaho-mqtt3c and libpaho-mqtt3a
ADD_LIBRARY(common_obj OBJECT ${common_src})
SET_TARGET_PROPERTIES(common_obj PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    COMPILE_DEFINITIONS "PAHO_MQTT_EXPORTS=1")
ADD_EXECUTABLE(MQTTVersion MQTTVersion.c)
SET_TARGET_PROPERTIES(MQTTVersion PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    COMPILE_DEFINITIONS "PAHO_MQTT_IMPORTS=1")
ENDIF()

IF (PAHO_BUILD_STATIC)    
ADD_LIBRARY(common_obj_static OBJECT ${common_src})
SET_TARGET_PROPERTIES(common_obj_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    COMPILE_DEFINITIONS "PAHO_MQTT_STATIC=1")
ENDIF()

IF (PAHO_BUILD_SHARED)
    ADD_LIBRARY(paho-mqtt3c SHARED $<TARGET_OBJECTS:common_obj> MQTTClient.c)
    ADD_LIBRARY(paho-mqtt3a SHARED $<TARGET_OBJECTS:common_obj> MQTTAsync.c MQTTAsyncUtils.c)
    
    TARGET_LINK_LIBRARIES(paho-mqtt3c ${LIBS_SYSTEM})
    TARGET_LINK_LIBRARIES(paho-mqtt3a ${LIBS_SYSTEM})
    TARGET_LINK_LIBRARIES(MQTTVersion paho-mqtt3a paho-mqtt3c ${LIBS_SYSTEM})
    
    SET_TARGET_PROPERTIES(
        paho-mqtt3c paho-mqtt3a PROPERTIES
        VERSION ${CLIENT_VERSION}
        SOVERSION ${PAHO_VERSION_MAJOR}
        COMPILE_DEFINITIONS "PAHO_MQTT_EXPORTS=1")

    IF(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
		SET(MQTTCLIENT_ENTRY_POINT _MQTTClient_init)
		SET(MQTTASYNC_ENTRY_POINT _MQTTAsync_init)
	ELSEIF (NOT WIN32)
		SET(MQTTCLIENT_ENTRY_POINT MQTTClient_init)
		SET(MQTTASYNC_ENTRY_POINT MQTTAsync_init)
	ENDIF()

    IF (NOT WIN32)
        SET_TARGET_PROPERTIES(
            paho-mqtt3c PROPERTIES
            LINK_FLAGS "-Wl,-init,${MQTTCLIENT_ENTRY_POINT}")
        SET_TARGET_PROPERTIES(
            paho-mqtt3a PROPERTIES
            LINK_FLAGS "-Wl,-init,${MQTTASYNC_ENTRY_POINT}")
    ENDIF()

    FOREACH(TARGET paho-mqtt3c paho-mqtt3a)
        TARGET_INCLUDE_DIRECTORIES(${TARGET}
            PUBLIC
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            PRIVATE
                ${CMAKE_BINARY_DIR})
    ENDFOREACH()
    
	INSTALL(TARGETS paho-mqtt3c paho-mqtt3a
		EXPORT eclipse-paho-mqtt-cTargets
		ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
		LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    INSTALL(TARGETS MQTTVersion
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
ENDIF()

IF (PAHO_BUILD_STATIC)
    ADD_LIBRARY(paho-mqtt3c-static STATIC $<TARGET_OBJECTS:common_obj_static> MQTTClient.c)
    ADD_LIBRARY(paho-mqtt3a-static STATIC $<TARGET_OBJECTS:common_obj_static> MQTTAsync.c MQTTAsyncUtils.c)

    TARGET_LINK_LIBRARIES(paho-mqtt3c-static ${LIBS_SYSTEM})
    TARGET_LINK_LIBRARIES(paho-mqtt3a-static ${LIBS_SYSTEM})
    IF (NOT WIN32)
        SET_TARGET_PROPERTIES(paho-mqtt3c-static PROPERTIES OUTPUT_NAME paho-mqtt3c)
        SET_TARGET_PROPERTIES(paho-mqtt3a-static PROPERTIES OUTPUT_NAME paho-mqtt3a)
    ENDIF()       
    SET_TARGET_PROPERTIES(
        paho-mqtt3c-static paho-mqtt3a-static PROPERTIES
        VERSION ${CLIENT_VERSION}
        SOVERSION ${PAHO_VERSION_MAJOR}
        COMPILE_DEFINITIONS "PAHO_MQTT_STATIC=1")

    FOREACH(TARGET paho-mqtt3c-static paho-mqtt3a-static)
        TARGET_INCLUDE_DIRECTORIES(${TARGET}
            PUBLIC
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            PRIVATE
                ${CMAKE_BINARY_DIR})
    ENDFOREACH()

    IF (NOT PAHO_BUILD_SHARED)
        INSTALL(TARGETS paho-mqtt3c-static paho-mqtt3a-static
            EXPORT eclipse-paho-mqtt-cTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    ELSE()
        INSTALL(TARGETS paho-mqtt3c-static paho-mqtt3a-static
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    ENDIF()
ENDIF()

INSTALL(FILES MQTTAsync.h MQTTClient.h MQTTClientPersistence.h MQTTProperties.h MQTTReasonCodes.h MQTTSubscribeOpts.h MQTTExportDeclarations.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

IF (PAHO_WITH_SSL)
    SET(OPENSSL_ROOT_DIR "" CACHE PATH "Directory containing OpenSSL libraries and includes")
    find_package(OpenSSL REQUIRED)

    IF (PAHO_BUILD_SHARED)
        ## common compilation for libpaho-mqtt3cs and libpaho-mqtt3as
    	## Note: SSL libraries must be recompiled due ifdefs
    	ADD_LIBRARY(common_ssl_obj OBJECT ${common_src})
    	TARGET_INCLUDE_DIRECTORIES(common_ssl_obj PUBLIC ${OPENSSL_INCLUDE_DIR})
    	SET_PROPERTY(TARGET common_ssl_obj PROPERTY	POSITION_INDEPENDENT_CODE ON)
    	SET_PROPERTY(TARGET common_ssl_obj PROPERTY COMPILE_DEFINITIONS "OPENSSL=1;PAHO_MQTT_EXPORTS=1")
    
        ADD_LIBRARY(paho-mqtt3cs SHARED $<TARGET_OBJECTS:common_ssl_obj> MQTTClient.c SSLSocket.c)
        ADD_LIBRARY(paho-mqtt3as SHARED $<TARGET_OBJECTS:common_ssl_obj> MQTTAsync.c MQTTAsyncUtils.c SSLSocket.c)
    
        SET_TARGET_PROPERTIES(
            paho-mqtt3cs paho-mqtt3as PROPERTIES
            VERSION ${CLIENT_VERSION}
            SOVERSION ${PAHO_VERSION_MAJOR}
            COMPILE_DEFINITIONS "OPENSSL=1;PAHO_MQTT_EXPORTS=1")

        IF(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
		    SET(MQTTCLIENT_ENTRY_POINT _MQTTClient_init)
		    SET(MQTTASYNC_ENTRY_POINT _MQTTAsync_init)
        ELSEIF (NOT WIN32)
            SET(MQTTCLIENT_ENTRY_POINT MQTTClient_init)
		    SET(MQTTASYNC_ENTRY_POINT MQTTAsync_init)
        ENDIF()

        IF (NOT WIN32)
            SET_TARGET_PROPERTIES(
	           paho-mqtt3cs PROPERTIES
	           LINK_FLAGS "-Wl,-init,${MQTTCLIENT_ENTRY_POINT}")
	        SET_TARGET_PROPERTIES(
	           paho-mqtt3as PROPERTIES
	           LINK_FLAGS "-Wl,-init,${MQTTASYNC_ENTRY_POINT}")
        ENDIF()

        FOREACH(TARGET paho-mqtt3cs paho-mqtt3as)
            TARGET_INCLUDE_DIRECTORIES(${TARGET}
                PUBLIC
                    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
                    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                PRIVATE
                    ${CMAKE_BINARY_DIR})
            TARGET_LINK_LIBRARIES(${TARGET}
                PUBLIC
                    OpenSSL::SSL OpenSSL::Crypto ${LIBS_SYSTEM})
        ENDFOREACH()
        INSTALL(TARGETS paho-mqtt3cs paho-mqtt3as
            EXPORT eclipse-paho-mqtt-cTargets
            ARCHIVE DESTINATION  ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION  ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION  ${CMAKE_INSTALL_BINDIR})
    ENDIF()

    IF (PAHO_BUILD_STATIC)
        ## common compilation for libpaho-mqtt3cs and libpaho-mqtt3as
    	## Note: SSL libraries must be recompiled due ifdefs
    	ADD_LIBRARY(common_ssl_obj_static OBJECT ${common_src})
    	TARGET_INCLUDE_DIRECTORIES(common_ssl_obj_static PUBLIC ${OPENSSL_INCLUDE_DIR})
    	SET_PROPERTY(TARGET common_ssl_obj_static PROPERTY POSITION_INDEPENDENT_CODE ON)
    	SET_PROPERTY(TARGET common_ssl_obj_static PROPERTY COMPILE_DEFINITIONS "OPENSSL=1;PAHO_MQTT_STATIC=1")
    
        ADD_LIBRARY(paho-mqtt3cs-static STATIC $<TARGET_OBJECTS:common_ssl_obj_static> MQTTClient.c SSLSocket.c)
        ADD_LIBRARY(paho-mqtt3as-static STATIC $<TARGET_OBJECTS:common_ssl_obj_static> MQTTAsync.c MQTTAsyncUtils.c SSLSocket.c)

        SET_TARGET_PROPERTIES(
            paho-mqtt3cs-static paho-mqtt3as-static PROPERTIES
            VERSION ${CLIENT_VERSION}
            SOVERSION ${PAHO_VERSION_MAJOR}
            COMPILE_DEFINITIONS "OPENSSL=1;PAHO_MQTT_STATIC=1")
        IF (NOT WIN32)
            SET_TARGET_PROPERTIES(paho-mqtt3cs-static PROPERTIES OUTPUT_NAME paho-mqtt3cs)
            SET_TARGET_PROPERTIES(paho-mqtt3as-static PROPERTIES OUTPUT_NAME paho-mqtt3as)
        ENDIF()

	    IF(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
			SET(MQTTCLIENT_ENTRY_POINT _MQTTClient_init)
			SET(MQTTASYNC_ENTRY_POINT _MQTTAsync_init)
		ELSEIF (NOT WIN32)
			SET(MQTTCLIENT_ENTRY_POINT MQTTClient_init)
			SET(MQTTASYNC_ENTRY_POINT MQTTAsync_init)
		ENDIF()

		IF (NOT WIN32)
		    SET_TARGET_PROPERTIES(
	            paho-mqtt3cs-static PROPERTIES
	            LINK_FLAGS "-Wl,-init,${MQTTCLIENT_ENTRY_POINT}")
	        SET_TARGET_PROPERTIES(
	            paho-mqtt3as-static PROPERTIES
	            LINK_FLAGS "-Wl,-init,${MQTTASYNC_ENTRY_POINT}")
	    ENDIF()

        IF (NOT PAHO_BUILD_SHARED)
            INSTALL(TARGETS paho-mqtt3cs-static paho-mqtt3as-static
                EXPORT eclipse-paho-mqtt-cTargets
                ARCHIVE DESTINATION  ${CMAKE_INSTALL_LIBDIR}
                LIBRARY DESTINATION  ${CMAKE_INSTALL_LIBDIR}
                RUNTIME DESTINATION  ${CMAKE_INSTALL_BINDIR})
        ELSE()
            INSTALL(TARGETS paho-mqtt3cs-static paho-mqtt3as-static
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
        ENDIF()
        FOREACH(TARGET paho-mqtt3cs-static paho-mqtt3as-static)
            TARGET_INCLUDE_DIRECTORIES(${TARGET}
                PUBLIC
                    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
                    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                PRIVATE
                    ${CMAKE_BINARY_DIR})
            TARGET_LINK_LIBRARIES(${TARGET}
                PUBLIC
                    OpenSSL::SSL OpenSSL::Crypto ${LIBS_SYSTEM})
        ENDFOREACH()
    ENDIF()
ENDIF()

INSTALL(EXPORT eclipse-paho-mqtt-cTargets
    FILE eclipse-paho-mqtt-cConfig.cmake
    NAMESPACE eclipse-paho-mqtt-c::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/eclipse-paho-mqtt-c)

INCLUDE(CMakePackageConfigHelpers)
WRITE_BASIC_PACKAGE_VERSION_FILE("eclipse-paho-mqtt-cConfigVersion.cmake"
    VERSION ${CLIENT_VERSION}
    COMPATIBILITY SameMajorVersion)
INSTALL(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/eclipse-paho-mqtt-cConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/eclipse-paho-mqtt-c)

# Base64 test
ADD_EXECUTABLE( Base64Test EXCLUDE_FROM_ALL Base64.c Base64.h )
TARGET_COMPILE_DEFINITIONS( Base64Test PUBLIC "-DBASE64_TEST" )
IF (PAHO_WITH_SSL)
	ADD_EXECUTABLE( Base64TestOpenSSL EXCLUDE_FROM_ALL Base64.c Base64.h )
	TARGET_LINK_LIBRARIES( Base64TestOpenSSL OpenSSL::SSL OpenSSL::Crypto)
	TARGET_COMPILE_DEFINITIONS( Base64TestOpenSSL PUBLIC "-DBASE64_TEST -DOPENSSL=1" )
ENDIF (PAHO_WITH_SSL)

# SHA1 test
ADD_EXECUTABLE( Sha1Test EXCLUDE_FROM_ALL SHA1.c SHA1.h )
TARGET_COMPILE_DEFINITIONS( Sha1Test PUBLIC "-DSHA1_TEST" )
IF (PAHO_WITH_SSL)
	ADD_EXECUTABLE( Sha1TestOpenSSL EXCLUDE_FROM_ALL SHA1.c SHA1.h )
	TARGET_LINK_LIBRARIES( Sha1TestOpenSSL OpenSSL::SSL OpenSSL::Crypto)
	TARGET_COMPILE_DEFINITIONS( Sha1TestOpenSSL PUBLIC "-DSHA1_TEST -DOPENSSL=1" )
ENDIF (PAHO_WITH_SSL)
