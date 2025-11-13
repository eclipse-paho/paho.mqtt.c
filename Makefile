#*******************************************************************************
#  Copyright (c) 2009, 2021 IBM Corp.
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
#     Ian Craggs - initial API and implementation and/or initial documentation
#     Allan Stockdill-Mander - SSL updates
#     Andy Piper - various fixes
#     Ian Craggs - OSX build
#     Rainer Poisel - support for multi-core builds and cross-compilation
#*******************************************************************************/

# Note: on OS X you should install XCode and the associated command-line tools

SHELL = /bin/sh
.PHONY: clean mkdir install install-strip uninstall html strip-options

MAJOR_VERSION := $(shell cat version.major)
MINOR_VERSION := $(shell cat version.minor)
PATCH_VERSION := $(shell cat version.patch)

ifndef release.version
  release.version = $(MAJOR_VERSION).$(MINOR_VERSION).$(PATCH_VERSION)
endif

# determine current platform
BUILD_TYPE ?= debug
ifeq ($(OS),Windows_NT)
	OSTYPE ?= $(OS)
	MACHINETYPE ?= $(PROCESSOR_ARCHITECTURE)
else
	OSTYPE ?= $(shell uname -s)
	MACHINETYPE ?= $(shell uname -m)
	build.level = $(shell date)
endif # OS
ifeq ($(OSTYPE),linux)
	OSTYPE = Linux
endif

# assume this is normally run in the main Paho directory
ifndef srcdir
  srcdir = src
endif

ifndef blddir
  blddir = build/output
endif

ifndef blddir_work
  blddir_work = build
endif

ifndef docdir
  docdir = $(blddir)/doc
endif

ifndef docdir_work
  docdir_work = $(blddir)/../doc
endif

ifndef prefix
	prefix = /usr/local
endif

ifndef exec_prefix
	exec_prefix = ${prefix}
endif

bindir = $(exec_prefix)/bin
includedir = $(prefix)/include
libdir = $(exec_prefix)/lib
datarootdir = $(prefix)/share
mandir = $(datarootdir)/man
man1dir = $(mandir)/man1
man2dir = $(mandir)/man2
man3dir = $(mandir)/man3

SOURCE_FILES = $(wildcard $(srcdir)/*.c)
SOURCE_FILES_C = $(filter-out $(srcdir)/MQTTAsync.c $(srcdir)/MQTTAsyncUtils.c $(srcdir)/MQTTVersion.c $(srcdir)/SSLSocket.c, $(SOURCE_FILES))
SOURCE_FILES_CS = $(filter-out $(srcdir)/MQTTAsync.c $(srcdir)/MQTTAsyncUtils.c $(srcdir)/MQTTVersion.c, $(SOURCE_FILES))
SOURCE_FILES_A = $(filter-out $(srcdir)/MQTTClient.c $(srcdir)/MQTTVersion.c $(srcdir)/SSLSocket.c, $(SOURCE_FILES))
SOURCE_FILES_AS = $(filter-out $(srcdir)/MQTTClient.c $(srcdir)/MQTTVersion.c, $(SOURCE_FILES))

HEADERS = $(srcdir)/*.h
HEADERS_C = $(filter-out $(srcdir)/MQTTAsync.h, $(HEADERS))
HEADERS_A = $(HEADERS)

SAMPLE_FILES_C = MQTTClient_publish MQTTClient_publish_async MQTTClient_subscribe
SYNC_SAMPLES = ${addprefix ${blddir}/samples/,${SAMPLE_FILES_C}}

UTIL_FILES_CS = paho_cs_pub paho_cs_sub 
SYNC_UTILS = ${addprefix ${blddir}/samples/,${UTIL_FILES_CS}}

SAMPLE_FILES_A = MQTTAsync_subscribe MQTTAsync_publish
ASYNC_SAMPLES = ${addprefix ${blddir}/samples/,${SAMPLE_FILES_A}}

UTIL_FILES_AS = paho_c_pub paho_c_sub
ASYNC_UTILS = ${addprefix ${blddir}/samples/,${UTIL_FILES_AS}}

TEST_FILES_C = test1 test15 test2 sync_client_test test_mqtt4sync test10
SYNC_TESTS = ${addprefix ${blddir}/test/,${TEST_FILES_C}}

TEST_FILES_CS = test3
SYNC_SSL_TESTS = ${addprefix ${blddir}/test/,${TEST_FILES_CS}}

TEST_FILES_A = test4 test45 test6 test9 test95 test_mqtt4async test11
ASYNC_TESTS = ${addprefix ${blddir}/test/,${TEST_FILES_A}}

TEST_FILES_AS = test5
ASYNC_SSL_TESTS = ${addprefix ${blddir}/test/,${TEST_FILES_AS}}

# The names of the four different libraries to be built
MQTTLIB_C = paho-mqtt3c
MQTTLIB_CS = paho-mqtt3cs
MQTTLIB_A = paho-mqtt3a
MQTTLIB_AS = paho-mqtt3as

CC ?= gcc

ifndef INSTALL
INSTALL = install
endif
INSTALL_PROGRAM = $(INSTALL)
INSTALL_DATA =  $(INSTALL) -m 644
DOXYGEN_COMMAND = doxygen

VERSION = ${MAJOR_VERSION}.${MINOR_VERSION}

MQTTLIB_C_NAME = lib${MQTTLIB_C}.so.${VERSION}
MQTTLIB_CS_NAME = lib${MQTTLIB_CS}.so.${VERSION}
MQTTLIB_A_NAME = lib${MQTTLIB_A}.so.${VERSION}
MQTTLIB_AS_NAME = lib${MQTTLIB_AS}.so.${VERSION}
MQTTVERSION_NAME = paho_c_version
PAHO_C_PUB_NAME = paho_c_pub
PAHO_C_SUB_NAME = paho_c_sub
PAHO_CS_PUB_NAME = paho_cs_pub
PAHO_CS_SUB_NAME = paho_cs_sub

MQTTLIB_C_TARGET = ${blddir}/${MQTTLIB_C_NAME}
MQTTLIB_CS_TARGET = ${blddir}/${MQTTLIB_CS_NAME}
MQTTLIB_A_TARGET = ${blddir}/${MQTTLIB_A_NAME}
MQTTLIB_AS_TARGET = ${blddir}/${MQTTLIB_AS_NAME}
MQTTVERSION_TARGET = ${blddir}/${MQTTVERSION_NAME}
PAHO_C_PUB_TARGET = ${blddir}/samples/${PAHO_C_PUB_NAME}
PAHO_C_SUB_TARGET = ${blddir}/samples/${PAHO_C_SUB_NAME}
PAHO_CS_PUB_TARGET = ${blddir}/samples/${PAHO_CS_PUB_NAME}
PAHO_CS_SUB_TARGET = ${blddir}/samples/${PAHO_CS_SUB_NAME}

#CCFLAGS_SO = -g -fPIC $(CFLAGS) -Os -Wall -fvisibility=hidden -I$(blddir_work) 
#FLAGS_EXE = $(LDFLAGS) -I ${srcdir} -pthread -L ${blddir}
#FLAGS_EXES = $(LDFLAGS) -I ${srcdir} ${START_GROUP} -pthread -lssl -lcrypto ${END_GROUP} -L ${blddir}

CCFLAGS_SO = -g -fPIC $(CFLAGS) -D_GNU_SOURCE -Os -Wall -fvisibility=hidden -I$(blddir_work) -DPAHO_MQTT_EXPORTS=1
FLAGS_EXE = $(LDFLAGS) -I ${srcdir} ${START_GROUP} -pthread ${GAI_LIB} ${END_GROUP} -L ${blddir}
FLAGS_EXES = $(LDFLAGS) -I ${srcdir} ${START_GROUP} -pthread ${GAI_LIB} -lssl -lcrypto ${END_GROUP} -L ${blddir}

LDCONFIG ?= /sbin/ldconfig
LDFLAGS_C = $(LDFLAGS) -shared -Wl,-init,$(MQTTCLIENT_INIT) $(START_GROUP) -pthread $(GAI_LIB) $(END_GROUP)
LDFLAGS_CS = $(LDFLAGS) -shared $(START_GROUP) -pthread $(GAI_LIB) $(EXTRA_LIB) -lssl -lcrypto $(END_GROUP) -Wl,-init,$(MQTTCLIENT_INIT)
LDFLAGS_A = $(LDFLAGS) -shared -Wl,-init,$(MQTTASYNC_INIT) $(START_GROUP) -pthread $(GAI_LIB) $(END_GROUP)
LDFLAGS_AS = $(LDFLAGS) -shared $(START_GROUP) -pthread $(GAI_LIB) $(EXTRA_LIB) -lssl -lcrypto $(END_GROUP) -Wl,-init,$(MQTTASYNC_INIT)

SED_COMMAND = sed \
    -e "s/@CLIENT_VERSION@/${release.version}/g" \
    -e "s/@BUILD_TIMESTAMP@/${build.level}/g"

ifeq ($(OSTYPE),Linux)

MQTTCLIENT_INIT = MQTTClient_init
MQTTASYNC_INIT = MQTTAsync_init
START_GROUP = -Wl,--start-group
END_GROUP = -Wl,--end-group

GAI_LIB = -lanl
EXTRA_LIB = -ldl

LDFLAGS_C += -Wl,-soname,lib$(MQTTLIB_C).so.${MAJOR_VERSION}
LDFLAGS_CS += -Wl,-soname,lib$(MQTTLIB_CS).so.${MAJOR_VERSION} -Wl,-no-whole-archive
LDFLAGS_A += -Wl,-soname,lib${MQTTLIB_A}.so.${MAJOR_VERSION}
LDFLAGS_AS += -Wl,-soname,lib${MQTTLIB_AS}.so.${MAJOR_VERSION} -Wl,-no-whole-archive

else ifeq ($(OSTYPE),Darwin)

MQTTCLIENT_INIT = _MQTTClient_init
MQTTASYNC_INIT = _MQTTAsync_init
START_GROUP =
END_GROUP =

GAI_LIB = 
EXTRA_LIB = -ldl

CCFLAGS_SO += -Wno-deprecated-declarations -DOSX -I /usr/local/opt/openssl/include
LDFLAGS_C += -Wl,-install_name,lib$(MQTTLIB_C).so.${MAJOR_VERSION}
LDFLAGS_CS += -Wl,-install_name,lib$(MQTTLIB_CS).so.${MAJOR_VERSION} -L /usr/local/opt/openssl/lib
LDFLAGS_A += -Wl,-install_name,lib${MQTTLIB_A}.so.${MAJOR_VERSION}
LDFLAGS_AS += -Wl,-install_name,lib${MQTTLIB_AS}.so.${MAJOR_VERSION} -L /usr/local/opt/openssl/lib
FLAGS_EXE += -DOSX
FLAGS_EXES += -L /usr/local/opt/openssl/lib

LDCONFIG = echo

endif

all: build

build: | mkdir ${MQTTLIB_C_TARGET} ${MQTTLIB_CS_TARGET} ${MQTTLIB_A_TARGET} ${MQTTLIB_AS_TARGET} ${MQTTVERSION_TARGET} ${SYNC_SAMPLES} ${SYNC_UTILS} ${ASYNC_SAMPLES} ${ASYNC_UTILS} ${SYNC_TESTS} ${SYNC_SSL_TESTS} ${ASYNC_TESTS} ${ASYNC_SSL_TESTS}

clean:
	rm -rf ${blddir}/*
	rm -rf ${blddir_work}/*

mkdir:
	-mkdir -p ${blddir}/samples
	-mkdir -p ${blddir}/test
	echo OSTYPE is $(OSTYPE)

${SYNC_TESTS}: ${blddir}/test/%: ${srcdir}/../test/%.c $(MQTTLIB_C_TARGET)
	${CC} -DNOSTACKTRACE -DNOLOG_MESSAGES $(srcdir)/Thread.c -g -o $@ $< -l${MQTTLIB_C} ${FLAGS_EXE}

${SYNC_SSL_TESTS}: ${blddir}/test/%: ${srcdir}/../test/%.c $(MQTTLIB_CS_TARGET)
	${CC} -g -o $@ $< -l${MQTTLIB_CS} ${FLAGS_EXES}

${ASYNC_TESTS}: ${blddir}/test/%: ${srcdir}/../test/%.c $(MQTTLIB_A_TARGET)
	${CC} -g -o $@ $< -l${MQTTLIB_A} ${FLAGS_EXE}

${ASYNC_SSL_TESTS}: ${blddir}/test/%: ${srcdir}/../test/%.c $(MQTTLIB_AS_TARGET)
	${CC} -g -o $@ $< -l${MQTTLIB_AS} ${FLAGS_EXES}

${SYNC_SAMPLES}: ${blddir}/samples/%: ${srcdir}/samples/%.c $(MQTTLIB_CS_TARGET)
	${CC} -o $@ $< -l${MQTTLIB_CS} ${FLAGS_EXES} 
	
${SYNC_UTILS}: ${blddir}/samples/%: ${srcdir}/samples/%.c ${srcdir}/samples/pubsub_opts.c $(MQTTLIB_CS_TARGET)
	${CC} -o $@ $< -l${MQTTLIB_CS} ${FLAGS_EXES} ${srcdir}/samples/pubsub_opts.c

${ASYNC_SAMPLES}: ${blddir}/samples/%: ${srcdir}/samples/%.c $(MQTTLIB_AS_TARGET)
	${CC} -o $@ $< -l${MQTTLIB_AS} ${FLAGS_EXES}
	
${ASYNC_UTILS}: ${blddir}/samples/%: ${srcdir}/samples/%.c ${srcdir}/samples/pubsub_opts.c $(MQTTLIB_AS_TARGET)
	${CC} -o $@ $< -l${MQTTLIB_AS} ${FLAGS_EXES} ${srcdir}/samples/pubsub_opts.c

$(blddir_work)/VersionInfo.h: $(srcdir)/VersionInfo.h.in
	-mkdir -p $(blddir_work)
	$(SED_COMMAND) $< > $@

${MQTTLIB_C_TARGET}: ${SOURCE_FILES_C} ${HEADERS_C} $(blddir_work)/VersionInfo.h
	${CC} ${CCFLAGS_SO} -o $@ ${SOURCE_FILES_C} ${LDFLAGS_C}
	-ln -s lib$(MQTTLIB_C).so.${VERSION}  ${blddir}/lib$(MQTTLIB_C).so.${MAJOR_VERSION}
	-ln -s lib$(MQTTLIB_C).so.${MAJOR_VERSION} ${blddir}/lib$(MQTTLIB_C).so

${MQTTLIB_CS_TARGET}: ${SOURCE_FILES_CS} ${HEADERS_C} $(blddir_work)/VersionInfo.h
	${CC} ${CCFLAGS_SO} -o $@ ${SOURCE_FILES_CS} -DOPENSSL ${LDFLAGS_CS}
	-ln -s lib$(MQTTLIB_CS).so.${VERSION}  ${blddir}/lib$(MQTTLIB_CS).so.${MAJOR_VERSION}
	-ln -s lib$(MQTTLIB_CS).so.${MAJOR_VERSION} ${blddir}/lib$(MQTTLIB_CS).so

${MQTTLIB_A_TARGET}: ${SOURCE_FILES_A} ${HEADERS_A} $(blddir_work)/VersionInfo.h
	${CC} ${CCFLAGS_SO} -o $@ ${SOURCE_FILES_A} ${LDFLAGS_A}
	-ln -s lib$(MQTTLIB_A).so.${VERSION}  ${blddir}/lib$(MQTTLIB_A).so.${MAJOR_VERSION}
	-ln -s lib$(MQTTLIB_A).so.${MAJOR_VERSION} ${blddir}/lib$(MQTTLIB_A).so

${MQTTLIB_AS_TARGET}: ${SOURCE_FILES_AS} ${HEADERS_A} $(blddir_work)/VersionInfo.h
	${CC} ${CCFLAGS_SO} -o $@ ${SOURCE_FILES_AS} -DOPENSSL ${LDFLAGS_AS}
	-ln -s lib$(MQTTLIB_AS).so.${VERSION}  ${blddir}/lib$(MQTTLIB_AS).so.${MAJOR_VERSION}
	-ln -s lib$(MQTTLIB_AS).so.${MAJOR_VERSION} ${blddir}/lib$(MQTTLIB_AS).so

${MQTTVERSION_TARGET}: $(srcdir)/MQTTVersion.c $(srcdir)/MQTTAsync.h $(MQTTLIB_A_TARGET)
	${CC} ${FLAGS_EXE} -o $@ -l${MQTTLIB_A} $(srcdir)/MQTTVersion.c -ldl

strip_options:
	$(eval INSTALL_OPTS := -s)

install-strip: build strip_options install

install: build
	mkdir -p $(DESTDIR)$(PREFIX)${includedir}
	$(INSTALL_DATA) ${INSTALL_OPTS} ${MQTTLIB_C_TARGET} $(DESTDIR)${libdir}
	$(INSTALL_DATA) ${INSTALL_OPTS} ${MQTTLIB_CS_TARGET} $(DESTDIR)${libdir}
	$(INSTALL_DATA) ${INSTALL_OPTS} ${MQTTLIB_A_TARGET} $(DESTDIR)${libdir}
	$(INSTALL_DATA) ${INSTALL_OPTS} ${MQTTLIB_AS_TARGET} $(DESTDIR)${libdir}
	$(INSTALL_PROGRAM) ${INSTALL_OPTS} ${MQTTVERSION_TARGET} $(DESTDIR)${bindir}
	$(INSTALL_PROGRAM) ${INSTALL_OPTS} ${PAHO_C_PUB_TARGET} $(DESTDIR)${bindir}
	$(INSTALL_PROGRAM) ${INSTALL_OPTS} ${PAHO_C_SUB_TARGET} $(DESTDIR)${bindir}
	$(INSTALL_PROGRAM) ${INSTALL_OPTS} ${PAHO_CS_PUB_TARGET} $(DESTDIR)${bindir}
	$(INSTALL_PROGRAM) ${INSTALL_OPTS} ${PAHO_CS_SUB_TARGET} $(DESTDIR)${bindir}
	$(LDCONFIG) $(DESTDIR)${libdir}
	ln -s lib$(MQTTLIB_C).so.${MAJOR_VERSION} $(DESTDIR)${libdir}/lib$(MQTTLIB_C).so
	ln -s lib$(MQTTLIB_CS).so.${MAJOR_VERSION} $(DESTDIR)${libdir}/lib$(MQTTLIB_CS).so
	ln -s lib$(MQTTLIB_A).so.${MAJOR_VERSION} $(DESTDIR)${libdir}/lib$(MQTTLIB_A).so
	ln -s lib$(MQTTLIB_AS).so.${MAJOR_VERSION} $(DESTDIR)${libdir}/lib$(MQTTLIB_AS).so
	@if test ! -f $(DESTDIR)${libdir}/lib$(MQTTLIB_C).so.${MAJOR_VERSION}; then ln -s lib$(MQTTLIB_C).so.${VERSION} $(DESTDIR)${libdir}/lib$(MQTTLIB_C).so.${MAJOR_VERSION}; fi
	@if test ! -f $(DESTDIR)${libdir}/lib$(MQTTLIB_CS).so.${MAJOR_VERSION}; then ln -s lib$(MQTTLIB_CS).so.${VERSION} $(DESTDIR)${libdir}/lib$(MQTTLIB_CS).so.${MAJOR_VERSION}; fi
	@if test ! -f $(DESTDIR)${libdir}/lib$(MQTTLIB_A).so.${MAJOR_VERSION}; then ln -s lib$(MQTTLIB_A).so.${VERSION} $(DESTDIR)${libdir}/lib$(MQTTLIB_A).so.${MAJOR_VERSION}; fi
	@if test ! -f $(DESTDIR)${libdir}/lib$(MQTTLIB_AS).so.${MAJOR_VERSION}; then ln -s lib$(MQTTLIB_AS).so.${VERSION} $(DESTDIR)${libdir}/lib$(MQTTLIB_AS).so.${MAJOR_VERSION}; fi
	$(INSTALL_DATA) ${srcdir}/MQTTAsync.h $(DESTDIR)${includedir}
	$(INSTALL_DATA) ${srcdir}/MQTTClient.h $(DESTDIR)${includedir}
	$(INSTALL_DATA) ${srcdir}/MQTTClientPersistence.h $(DESTDIR)${includedir}
	$(INSTALL_DATA) ${srcdir}/MQTTProperties.h $(DESTDIR)${includedir}
	$(INSTALL_DATA) ${srcdir}/MQTTReasonCodes.h $(DESTDIR)${includedir}
	$(INSTALL_DATA) ${srcdir}/MQTTSubscribeOpts.h $(DESTDIR)${includedir}	
	$(INSTALL_DATA) ${srcdir}/MQTTExportDeclarations.h $(DESTDIR)${includedir}
	- $(INSTALL_DATA) doc/man/man1/paho_c_pub.1 $(DESTDIR)${man1dir}
	- $(INSTALL_DATA) doc/man/man1/paho_c_sub.1 $(DESTDIR)${man1dir}
	- $(INSTALL_DATA) doc/man/man1/paho_cs_pub.1 $(DESTDIR)${man1dir}
	- $(INSTALL_DATA) doc/man/man1/paho_cs_sub.1 $(DESTDIR)${man1dir}
	
ifneq ("$(wildcard ${blddir}/doc/MQTTClient/man/man3/MQTTClient.h.3)","")
	- $(INSTALL_DATA) ${blddir}/doc/MQTTClient/man/man3/MQTTClient.h.3 $(DESTDIR)${man3dir}
endif
ifneq ("$(wildcard ${blddir}/doc/MQTTAsync/man/man3/MQTTAsync.h.3)","")
	- $(INSTALL_DATA) ${blddir}/doc/MQTTAsync/man/man3/MQTTAsync.h.3 $(DESTDIR)${man3dir}
endif
	
uninstall:
	- rm $(DESTDIR)${libdir}/${MQTTLIB_C_NAME} 
	- rm $(DESTDIR)${libdir}/${MQTTLIB_CS_NAME} 
	- rm $(DESTDIR)${libdir}/${MQTTLIB_A_NAME} 
	- rm $(DESTDIR)${libdir}/${MQTTLIB_AS_NAME} 
	- rm $(DESTDIR)${bindir}/${MQTTVERSION_NAME} 
	- rm $(DESTDIR)${bindir}/${PAHO_C_PUB_NAME} 
	- rm $(DESTDIR)${bindir}/${PAHO_C_SUB_NAME} 
	- rm $(DESTDIR)${bindir}/${PAHO_CS_PUB_NAME} 
	- rm $(DESTDIR)${bindir}/${PAHO_CS_SUB_NAME} 
	$(LDCONFIG) $(DESTDIR)${libdir}
	- rm $(DESTDIR)${libdir}/lib$(MQTTLIB_C).so
	- rm $(DESTDIR)${libdir}/lib$(MQTTLIB_CS).so
	- rm $(DESTDIR)${libdir}/lib$(MQTTLIB_A).so
	- rm $(DESTDIR)${libdir}/lib$(MQTTLIB_AS).so
	- rm $(DESTDIR)${libdir}/lib$(MQTTLIB_C).so.${MAJOR_VERSION}
	- rm $(DESTDIR)${libdir}/lib$(MQTTLIB_CS).so.${MAJOR_VERSION}
	- rm $(DESTDIR)${libdir}/lib$(MQTTLIB_A).so.${MAJOR_VERSION}
	- rm $(DESTDIR)${libdir}/lib$(MQTTLIB_AS).so.${MAJOR_VERSION}
	- rm $(DESTDIR)${includedir}/MQTTAsync.h
	- rm $(DESTDIR)${includedir}/MQTTClient.h
	- rm $(DESTDIR)${includedir}/MQTTClientPersistence.h
	- rm $(DESTDIR)${includedir}/MQTTProperties.h
	- rm $(DESTDIR)${includedir}/MQTTReasonCodes.h
	- rm $(DESTDIR)${includedir}/MQTTSubscribeOpts.h
	- rm $(DESTDIR)${includedir}/MQTTExportDeclarations.h
	
	- rm $(DESTDIR)${man1dir}/paho_c_pub.1
	- rm $(DESTDIR)${man1dir}/paho_c_sub.1
	- rm $(DESTDIR)${man1dir}/paho_cs_pub.1
	- rm $(DESTDIR)${man1dir}/paho_cs_sub.1
	
ifneq ("$(wildcard $(DESTDIR)${man3dir}/MQTTClient.h.3)","")
	- rm $(DESTDIR)${man3dir}/MQTTClient.h.3
endif
ifneq ("$(wildcard $(DESTDIR)${man3dir}/MQTTAsync.h.3)","")
	- rm $(DESTDIR)${man3dir}/MQTTAsync.h.3
endif

REGEX_DOXYGEN := \
    's;@PROJECT_SOURCE_DIR@/src/\?;;' \
    's;@PROJECT_SOURCE_DIR@;..;' \
    's;@CMAKE_CURRENT_BINARY_DIR@;../build/output;'
SED_DOXYGEN := $(foreach sed_exp,$(REGEX_DOXYGEN),-e $(sed_exp))
define process_doxygen
	cd ${srcdir}; sed $(SED_DOXYGEN) ../doc/${1}.in > ../$(docdir_work)/${1}
	cd ${srcdir}; $(DOXYGEN_COMMAND) ../$(docdir_work)/${1}
endef
html:
	-mkdir -p $(docdir_work)
	-mkdir -p ${docdir}
	$(call process_doxygen,DoxyfileV3ClientAPI)
	$(call process_doxygen,DoxyfileV3AsyncAPI)
	$(call process_doxygen,DoxyfileV3ClientInternal)
