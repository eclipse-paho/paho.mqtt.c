#*******************************************************************************
#  Copyright (c) 2015, 2020 logi.cals GmbH and others
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
#     Ian Craggs - update sample names
#*******************************************************************************/

## compilation/linkage settings
include_directories(
  .
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_BINARY_DIR}
)

if(WIN32)
  add_definitions(/DCMAKE_BUILD /D_CRT_SECURE_NO_DEPRECATE)
endif()

# sample files c

if(PAHO_BUILD_SHARED)
  if(PAHO_WITH_SSL)
    foreach(TARGET paho_c_pub paho_c_sub paho_cs_pub paho_cs_sub)
      add_executable(${TARGET} ${TARGET}.c pubsub_opts.c)
      set_target_properties(${TARGET} PROPERTIES
        COMPILE_DEFINITIONS "PAHO_MQTT_IMPORTS=1"
      )
    endforeach()

    target_link_libraries(paho_c_pub paho-mqtt3as)
    target_link_libraries(paho_c_sub paho-mqtt3as)

    target_link_libraries(paho_cs_pub paho-mqtt3cs)
    target_link_libraries(paho_cs_sub paho-mqtt3cs)

    install(
      TARGETS
        paho_c_sub
        paho_c_pub
        paho_cs_sub
        paho_cs_pub

      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
  endif()

  foreach(TARGET
    MQTTAsync_subscribe
    MQTTAsync_publish
    MQTTAsync_publish_time
    MQTTClient_subscribe
    MQTTClient_publish
    MQTTClient_publish_async
  )
    add_executable(${TARGET} ${TARGET}.c)
    set_target_properties(${TARGET} PROPERTIES
      COMPILE_DEFINITIONS "PAHO_MQTT_IMPORTS=1"
  )
  endforeach()

  foreach(TARGET MQTTAsync_subscribe MQTTAsync_publish MQTTAsync_publish_time)
    target_link_libraries(${TARGET} paho-mqtt3a)
  endforeach()

  foreach(TARGET MQTTClient_subscribe MQTTClient_publish MQTTClient_publish_async)
    target_link_libraries(${TARGET} paho-mqtt3c)
  endforeach()

  install(
    TARGETS
      MQTTAsync_subscribe
      MQTTAsync_publish
      MQTTAsync_publish_time
      MQTTClient_subscribe
      MQTTClient_publish
      MQTTClient_publish_async

    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()

if(PAHO_BUILD_STATIC)
  if(PAHO_WITH_SSL)
    foreach(TARGET paho_c_pub paho_c_sub paho_cs_pub paho_cs_sub)
      add_executable(${TARGET}_static ${TARGET}.c pubsub_opts.c)
    endforeach()

    target_link_libraries(paho_c_pub_static paho-mqtt3as-static)
    target_link_libraries(paho_c_sub_static paho-mqtt3as-static)

    target_link_libraries(paho_cs_pub_static paho-mqtt3cs-static)
    target_link_libraries(paho_cs_sub_static paho-mqtt3cs-static)

    install(
      TARGETS
        paho_c_sub_static
        paho_c_pub_static
        paho_cs_sub_static
        paho_cs_pub_static

      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
  endif()

  foreach(TARGET
    MQTTAsync_subscribe
    MQTTAsync_publish
    MQTTAsync_publish_time
    MQTTClient_subscribe
    MQTTClient_publish
    MQTTClient_publish_async
  )
    add_executable(${TARGET}_static ${TARGET}.c)
  endforeach()

  foreach(TARGET MQTTAsync_subscribe MQTTAsync_publish MQTTAsync_publish_time)
    target_link_libraries(${TARGET}_static paho-mqtt3a-static)
  endforeach()

  foreach(TARGET MQTTClient_subscribe MQTTClient_publish MQTTClient_publish_async)
    target_link_libraries(${TARGET}_static paho-mqtt3c-static)
  endforeach()

  install(
    TARGETS
      MQTTAsync_subscribe_static
      MQTTAsync_publish_static
      MQTTAsync_publish_time_static
      MQTTClient_subscribe_static
      MQTTClient_publish_static
      MQTTClient_publish_async_static

    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()

