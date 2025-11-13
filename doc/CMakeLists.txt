#*******************************************************************************
#  Copyright (c) 2015 logi.cals GmbH
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
#*******************************************************************************/

### documentation settings
find_package(Doxygen)
if(NOT DOXYGEN_FOUND)
  message(FATAL_ERROR "Doxygen is needed to build the documentation.")
endif()

set(DOXYTARGETS)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/doc)

foreach(DOXYFILE_SRC DoxyfileV3ClientAPI;DoxyfileV3AsyncAPI;DoxyfileV3ClientInternal)
  set(DOXYFILE_IN ${DOXYFILE_SRC}.in)
  set(DOXYFILE ${CMAKE_CURRENT_BINARY_DIR}/${DOXYFILE_SRC})

  configure_file(${DOXYFILE_IN} ${DOXYFILE} @ONLY)
  add_custom_target(${DOXYFILE_SRC}.target
    COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYFILE}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating API documentation with Doxygen"
    VERBATIM
  )
  set(DOXYTARGETS ${DOXYTARGETS} ${DOXYFILE_SRC}.target)
endforeach(DOXYFILE_SRC)

add_custom_target(doc ALL DEPENDS ${DOXYTARGETS})

if(PAHO_ENABLE_CPACK)
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/doc/ DESTINATION ${CMAKE_INSTALL_DOCDIR})
else()
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/doc DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()
