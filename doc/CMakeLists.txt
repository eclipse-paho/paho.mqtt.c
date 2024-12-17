#*******************************************************************************
#  Copyright (c) 2015 logi.cals GmbH
# 
#  All rights reserved. This program and the accompanying materials
#  are made available under the terms of the Eclipse Public License v2.0
#  and Eclipse Distribution License v1.0 which accompany this distribution. 
# 
#  The Eclipse Public License is available at 
#     https://www.eclipse.org/legal/epl-v20.html
#  and the Eclipse Distribution License is available at 
#    http://www.eclipse.org/org/documents/edl-v10.php
# 
#  Contributors:
#     Rainer Poisel - initial version
#*******************************************************************************/

# Note: on OS X you should install XCode and the associated command-line tools

### documentation settings
FIND_PACKAGE(Doxygen)
IF(NOT DOXYGEN_FOUND)
    message(FATAL_ERROR "Doxygen is needed to build the documentation.")
ENDIF()
SET(DOXYTARGETS)
FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/doc)
FOREACH(DOXYFILE_SRC DoxyfileV3ClientAPI;DoxyfileV3AsyncAPI;DoxyfileV3ClientInternal)
    SET(DOXYFILE_IN ${DOXYFILE_SRC}.in)
    SET(DOXYFILE ${CMAKE_CURRENT_BINARY_DIR}/${DOXYFILE_SRC})

    CONFIGURE_FILE(${DOXYFILE_IN} ${DOXYFILE} @ONLY)
    ADD_CUSTOM_TARGET(${DOXYFILE_SRC}.target
        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYFILE}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM
    )
    SET(DOXYTARGETS ${DOXYTARGETS} ${DOXYFILE_SRC}.target)
ENDFOREACH(DOXYFILE_SRC)

ADD_CUSTOM_TARGET(doc ALL DEPENDS ${DOXYTARGETS})
IF (PAHO_ENABLE_CPACK)
    INSTALL(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/doc/ DESTINATION ${CMAKE_INSTALL_DOCDIR})
ELSE()
    INSTALL(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/doc DESTINATION ${CMAKE_INSTALL_DOCDIR})
ENDIF()
