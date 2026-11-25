/*******************************************************************************
 * Copyright (c) 2026 Ian Craggs
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    https://www.eclipse.org/legal/epl-2.0/
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * AI Disclosure: This file was partly AI-generated. The AI-generated
 * portions are made available under CC0-1.0 and not subject to the
 * project's licence. The human contributor has reviewed and verified
 * that the code is correct.
 *
 * SPDX-License-Identifier: EPL-2.0 and CC0-1.0
 *
 * Contributors:
 *    Ian Craggs - initial implementation
 *******************************************************************************/

/**
 * @file
 * Test for issue 1727: MQTTAsync_createOptions versions.
 * The persistQoS0 field was added in version 3 of MQTTAsync_createOptions, but
 * the initializers set version 2, and the library read persistQoS0 from version 2
 * structures. Also MQTTAsync_createWithOptions copied the whole of the current
 * structure, whatever the caller's version, so read past the end of older ones.
 * The persistence tests use a user persistence implementation which counts the
 * commands persisted, and publish while disconnected, so no broker is needed.
 * Test 1: the initializers set version 3
 * Test 2: version 3, persistQoS0 0: QoS 0 publish commands are not persisted
 * Test 3: version 3, persistQoS0 1: QoS 0 publish commands are persisted
 * Test 4: version 2: persistQoS0 isn't part of the structure, so is ignored
 * Test 5: version 0, 1 and 2 structures placed just before an inaccessible
 *         memory page, so that reading past their end crashes
 * The other API structures had the same problem: fields were read from older
 * versions which don't have them, and MQTTAsync_sendMessage wrote the message
 * properties into the caller's response options, past the end of a version 0
 * structure. Tests 6 to 8 put each older version before an inaccessible page.
 * Test 6: MQTTAsync_connect, connect options versions 0 to 7 (no broker needed)
 * Test 7: MQTT 3.1.1 subscribe, unsubscribe, send and sendMessage with version 0
 *         response options and message
 * Test 8: MQTT 5 sendMessage with a version 0 response options or message,
 *         which mustn't change the caller's response options. Also disconnect with
 *         the version 0 initializer, which includes onSuccess5: unlike the other
 *         structures, version 0 disconnect options have had the MQTT 5 callbacks
 *         since 1.3.1, so they must still be used.
 */

#include "MQTTAsync.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <time.h>

#if !defined(_WINDOWS)
#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include <windows.h>
#define snprintf _snprintf
#endif

void usage(void)
{
	printf("Options:\n");
	printf("  --test_no <n>  run test number n (default all)\n");
	printf("  --connection <uri>  broker to connect to for tests 7 and 8 (default tcp://localhost:1883)\n");
	printf("  --verbose\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	char* connection;
	int verbose;
	int test_no;
} options =
{
	"tcp://localhost:1883",
	0,
	0,
};

void getopts(int argc, char** argv)
{
	int count = 1;

	while (count < argc)
	{
		if (strcmp(argv[count], "--test_no") == 0)
		{
			if (++count < argc)
				options.test_no = atoi(argv[count]);
			else
				usage();
		}
		else if (strcmp(argv[count], "--connection") == 0)
		{
			if (++count < argc)
				options.connection = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--verbose") == 0)
			options.verbose = 1;
		else if (strcmp(argv[count], "--help") == 0 || strcmp(argv[count], "-h") == 0)
			usage();
		count++;
	}
}


#define LOGA_DEBUG 0
#define LOGA_INFO 1
#include <sys/timeb.h>
void MyLog(int LOGA_level, char* format, ...)
{
	static char msg_buf[256];
	va_list args;
#if defined(_WIN32) || defined(_WINDOWS)
	struct timeb ts;
#else
	struct timeval ts;
#endif
	struct tm *timeinfo;

	if (LOGA_level == LOGA_DEBUG && options.verbose == 0)
	  return;

#if defined(_WIN32) || defined(_WINDOWS)
	ftime(&ts);
	timeinfo = localtime(&ts.time);
#else
	gettimeofday(&ts, NULL);
	timeinfo = localtime(&ts.tv_sec);
#endif
	strftime(msg_buf, 80, "%Y%m%d %H%M%S", timeinfo);

#if defined(_WIN32) || defined(_WINDOWS)
	sprintf(&msg_buf[strlen(msg_buf)], ".%.3hu ", ts.millitm);
#else
	sprintf(&msg_buf[strlen(msg_buf)], ".%.3lu ", ts.tv_usec / 1000L);
#endif

	va_start(args, format);
	vsnprintf(&msg_buf[strlen(msg_buf)], sizeof(msg_buf) - strlen(msg_buf), format, args);
	va_end(args);

	printf("%s\n", msg_buf);
	fflush(stdout);
}

#define assert(a, b, c, ...) myassert(__FILE__, __LINE__, a, b, c, __VA_ARGS__)

int tests = 0;
int failures = 0;

void myassert(char* filename, int lineno, char* description, int value,
		char* format, ...)
{
	++tests;
	if (!value)
	{
		va_list args;

		++failures;
		MyLog(LOGA_INFO, "Assertion failed, file %s, line %d, description: %s", filename,
				lineno, description);

		va_start(args, format);
		vprintf(format, args);
		va_end(args);
	}
	else
		MyLog(LOGA_DEBUG, "Assertion succeeded, file %s, line %d, description: %s",
				filename, lineno, description);
}


/* A user persistence implementation which stores nothing, but counts the commands persisted */

int persisted_commands = 0;

int pers_open(void** handle, const char* clientID, const char* serverURI, void* context)
{
	*handle = context;
	return 0;
}

int pers_close(void* handle)
{
	return 0;
}

int pers_put(void* handle, char* key, int bufcount, char* buffers[], int buflens[])
{
	MyLog(LOGA_DEBUG, "Persisting key %s", key);
	if (strncmp(key, "c-", 2) == 0 || strncmp(key, "c5-", 3) == 0)
		++persisted_commands;
	return 0;
}

int pers_get(void* handle, char* key, char** buffer, int* buflen)
{
	return MQTTCLIENT_PERSISTENCE_ERROR;
}

int pers_remove(void* handle, char* key)
{
	return 0;
}

int pers_keys(void* handle, char*** keys, int* nkeys)
{
	*keys = NULL;
	*nkeys = 0;
	return 0;
}

int pers_clear(void* handle)
{
	return 0;
}

int pers_containskey(void* handle, char* key)
{
	return MQTTCLIENT_PERSISTENCE_ERROR;
}


/* Create a client with the create options and publish one message of the given QoS while
 * disconnected. Returns the number of commands persisted, or -1 on error. */
int publish_while_disconnected(MQTTAsync_createOptions* createOpts, int qos)
{
	/* the context must not be NULL */
	MQTTClient_persistence persistence = {&persisted_commands, pers_open, pers_close, pers_put, pers_get,
		pers_remove, pers_keys, pers_clear, pers_containskey};
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTAsync c;
	int rc;

	persisted_commands = 0;
	rc = MQTTAsync_createWithOptions(&c, "tcp://127.0.0.1:1883", "issue1727",
			MQTTCLIENT_PERSISTENCE_USER, &persistence, createOpts);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		return -1;

	pubmsg.payload = "issue 1727";
	pubmsg.payloadlen = (int)strlen(pubmsg.payload);
	pubmsg.qos = qos;
	rc = MQTTAsync_sendMessage(c, "issue1727", &pubmsg, NULL);
	assert("good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);

	MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "struct_version %d, persistQoS0 %d, QoS %d: %d commands persisted",
		createOpts->struct_version, createOpts->persistQoS0, qos, persisted_commands);
	return (rc == MQTTASYNC_SUCCESS) ? persisted_commands : -1;
}


void start_test(char* testname)
{
	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;
}

int end_test(char* testname)
{
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}


int test1(struct Options options)
{
	char* testname = "test1727-1 - create options initializers set version 3";
	MQTTAsync_createOptions opts = MQTTAsync_createOptions_initializer;
	MQTTAsync_createOptions opts5 = MQTTAsync_createOptions_initializer5;

	start_test(testname);
	assert("initializer sets version 3", opts.struct_version == 3,
		"struct_version was %d\n", opts.struct_version);
	assert("initializer5 sets version 3", opts5.struct_version == 3,
		"struct_version was %d\n", opts5.struct_version);
	assert("persistQoS0 defaults to 1", opts.persistQoS0 == 1 && opts5.persistQoS0 == 1,
		"persistQoS0 was %d and %d\n", opts.persistQoS0, opts5.persistQoS0);
	return end_test(testname);
}


int test2(struct Options options)
{
	char* testname = "test1727-2 - version 3, persistQoS0 0, QoS 0 commands not persisted";
	MQTTAsync_createOptions opts = MQTTAsync_createOptions_initializer;
	int count;

	start_test(testname);
	opts.sendWhileDisconnected = 1;
	opts.allowDisconnectedSendAtAnyTime = 1;
	opts.persistQoS0 = 0;

	count = publish_while_disconnected(&opts, 0);
	assert("QoS 0 command not persisted", count == 0, "%d commands persisted\n", count);

	/* persistQoS0 doesn't affect QoS 1 */
	count = publish_while_disconnected(&opts, 1);
	assert("QoS 1 command persisted", count == 1, "%d commands persisted\n", count);
	return end_test(testname);
}


int test3(struct Options options)
{
	char* testname = "test1727-3 - version 3, persistQoS0 1, QoS 0 commands persisted";
	MQTTAsync_createOptions opts = MQTTAsync_createOptions_initializer;
	int count;

	start_test(testname);
	opts.sendWhileDisconnected = 1;
	opts.allowDisconnectedSendAtAnyTime = 1;
	opts.persistQoS0 = 1;

	count = publish_while_disconnected(&opts, 0);
	assert("QoS 0 command persisted", count == 1, "%d commands persisted\n", count);
	return end_test(testname);
}


int test4(struct Options options)
{
	char* testname = "test1727-4 - version 2, persistQoS0 ignored, QoS 0 commands persisted";
	MQTTAsync_createOptions opts = MQTTAsync_createOptions_initializer;
	int count;

	start_test(testname);
	opts.struct_version = 2;
	opts.sendWhileDisconnected = 1;
	opts.allowDisconnectedSendAtAnyTime = 1;
	opts.persistQoS0 = 0; /* not part of a version 2 structure, so should be ignored */

	count = publish_while_disconnected(&opts, 0);
	assert("QoS 0 command persisted", count == 1, "%d commands persisted\n", count);
	return end_test(testname);
}


/* Allocate two memory pages, with the second inaccessible, so that a read past the end of
 * memory at the end of the first page fails. Returns the start of the second page. */
char* alloc_guarded(size_t* pagesize)
{
	char* mem = NULL;
#if defined(_WIN32) || defined(_WINDOWS)
	SYSTEM_INFO info;
	DWORD old;

	GetSystemInfo(&info);
	*pagesize = info.dwPageSize;
	mem = VirtualAlloc(NULL, 2 * *pagesize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (mem == NULL || !VirtualProtect(mem + *pagesize, *pagesize, PAGE_NOACCESS, &old))
		return NULL;
#else
	*pagesize = (size_t)sysconf(_SC_PAGESIZE);
	mem = mmap(NULL, 2 * *pagesize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (mem == MAP_FAILED || mprotect(mem + *pagesize, *pagesize, PROT_NONE) != 0)
		return NULL;
#endif
	return mem + *pagesize;
}

void free_guarded(char* guard, size_t pagesize)
{
#if defined(_WIN32) || defined(_WINDOWS)
	VirtualFree(guard - pagesize, 0, MEM_RELEASE);
#else
	munmap(guard - pagesize, 2 * pagesize);
#endif
}

int test5(struct Options options)
{
	char* testname = "test1727-5 - no read past the end of older create options versions";
	/* the size of each version of the structure */
	size_t sizes[] = {
		offsetof(MQTTAsync_createOptions, MQTTVersion),
		offsetof(MQTTAsync_createOptions, allowDisconnectedSendAtAnyTime),
		offsetof(MQTTAsync_createOptions, persistQoS0)
	};
	int version;

	start_test(testname);
	for (version = 0; version < (int)(sizeof(sizes) / sizeof(sizes[0])); ++version)
	{
		MQTTAsync_createOptions opts = MQTTAsync_createOptions_initializer;
		MQTTAsync_createOptions* guarded;
		MQTTAsync c;
		size_t pagesize = 0;
		char* guard = alloc_guarded(&pagesize);
		int rc;

		assert("guarded memory allocated", guard != NULL, "guard was %p\n", guard);
		if (guard == NULL)
			break;

		/* put the structure at the end of the accessible page, so that any read past its end crashes */
		opts.struct_version = version;
		guarded = (MQTTAsync_createOptions*)(guard - sizes[version]);
		memcpy(guarded, &opts, sizes[version]);

		MyLog(LOGA_INFO, "Creating with a version %d structure of %d bytes", version, (int)sizes[version]);
		rc = MQTTAsync_createWithOptions(&c, "tcp://127.0.0.1:1883", "issue1727",
				MQTTCLIENT_PERSISTENCE_NONE, NULL, guarded);
		assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
		if (rc == MQTTASYNC_SUCCESS)
			MQTTAsync_destroy(&c);
		free_guarded(guard, pagesize);
	}
	return end_test(testname);
}


/* Copy the first len bytes of a structure to just before an inaccessible page, so that any
 * access past its end crashes. Returns the copy, or NULL on error. */
void* guarded_copy(const void* src, size_t len, char** guard, size_t* pagesize)
{
	if ((*guard = alloc_guarded(pagesize)) == NULL)
		return NULL;
	memcpy(*guard - len, src, len);
	return *guard - len;
}


void MySleep(long milliseconds)
{
#if defined(_WIN32) || defined(_WINDOWS)
	Sleep(milliseconds);
#else
	usleep(milliseconds*1000);
#endif
}

volatile int connected = 0;

void onConnect(void* context, MQTTAsync_successData* response)
{
	connected = 1;
}

void onConnect5(void* context, MQTTAsync_successData5* response)
{
	connected = 1;
}

void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	connected = -1;
}

void onConnectFailure5(void* context, MQTTAsync_failureData5* response)
{
	connected = -1;
}

volatile int disconnected = 0;

void onDisconnect5(void* context, MQTTAsync_successData5* response)
{
	disconnected = 1;
}

int wait_for_connect(void)
{
	int count = 0;

	while (connected == 0 && count++ < 500)
		MySleep(10);
	return connected == 1;
}


int test6(struct Options options)
{
	char* testname = "test1727-6 - no read past the end of older connect options versions";
	/* the size of each version of the structure */
	size_t sizes[] = {
		offsetof(MQTTAsync_connectOptions, serverURIcount),
		offsetof(MQTTAsync_connectOptions, serverURIcount),
		offsetof(MQTTAsync_connectOptions, MQTTVersion),
		offsetof(MQTTAsync_connectOptions, automaticReconnect),
		offsetof(MQTTAsync_connectOptions, binarypwd),
		offsetof(MQTTAsync_connectOptions, cleanstart),
		offsetof(MQTTAsync_connectOptions, httpHeaders),
		offsetof(MQTTAsync_connectOptions, httpProxy)
	};
	int version;

	start_test(testname);
	for (version = 0; version < (int)(sizeof(sizes) / sizeof(sizes[0])); ++version)
	{
		MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
		MQTTAsync_connectOptions* guarded;
		MQTTAsync c;
		size_t pagesize = 0;
		char* guard = NULL;
		int rc;

		/* nothing listens on port 1, so the connect fails, but the options are read in the call */
		rc = MQTTAsync_create(&c, "tcp://127.0.0.1:1", "issue1727", MQTTCLIENT_PERSISTENCE_NONE, NULL);
		assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
		if (rc != MQTTASYNC_SUCCESS)
			break;

		opts.struct_version = version;
		guarded = guarded_copy(&opts, sizes[version], &guard, &pagesize);
		assert("guarded memory allocated", guarded != NULL, "guarded was %p\n", (void*)guarded);
		if (guarded == NULL)
		{
			MQTTAsync_destroy(&c);
			break;
		}

		MyLog(LOGA_INFO, "Connecting with a version %d structure of %d bytes", version, (int)sizes[version]);
		rc = MQTTAsync_connect(c, guarded);
		assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
		MQTTAsync_destroy(&c);
		free_guarded(guard, pagesize);
	}
	return end_test(testname);
}


int test7(struct Options options)
{
	char* testname = "test1727-7 - MQTT 3.1.1, no read past the end of version 0 response options and message";
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_responseOptions response = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message message = MQTTAsync_message_initializer;
	MQTTAsync_responseOptions* response0 = NULL;
	MQTTAsync_message* message0 = NULL;
	char* guards[2] = {NULL, NULL};
	size_t pagesize = 0;
	char* topic = "issue1727/test7";
	char* const topics[2] = {"issue1727/test7/a", "issue1727/test7/b"};
	int qoss[2] = {1, 1};
	MQTTAsync c;
	int rc, i;

	start_test(testname);
	rc = MQTTAsync_create(&c, options.connection, "issue1727-7", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	connected = 0;
	opts.onSuccess = onConnect;
	opts.onFailure = onConnectFailure;
	rc = MQTTAsync_connect(c, &opts);
	assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	assert("connected", wait_for_connect(), "connected was %d\n", connected);
	if (connected != 1)
		goto destroy;

	response.struct_version = 0;
	response0 = guarded_copy(&response, offsetof(MQTTAsync_responseOptions, onSuccess5), &guards[0], &pagesize);
	message.struct_version = 0;
	message.payload = "issue 1727";
	message.payloadlen = (int)strlen(message.payload);
	message.qos = 1;
	message0 = guarded_copy(&message, offsetof(MQTTAsync_message, properties), &guards[1], &pagesize);
	assert("guarded memory allocated", response0 && message0, "%p %p\n", (void*)response0, (void*)message0);
	if (!response0 || !message0)
		goto destroy;

	rc = MQTTAsync_subscribe(c, topic, 1, response0);
	assert("good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	rc = MQTTAsync_subscribeMany(c, 2, topics, qoss, response0);
	assert("good rc from subscribeMany", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	rc = MQTTAsync_send(c, topic, message.payloadlen, message.payload, 1, 0, response0);
	assert("good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	rc = MQTTAsync_sendMessage(c, topic, message0, response0);
	assert("good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	rc = MQTTAsync_unsubscribe(c, topic, response0);
	assert("good rc from unsubscribe", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	MySleep(500);
	rc = MQTTAsync_disconnect(c, NULL);
	assert("good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	MySleep(500);

destroy:
	MQTTAsync_destroy(&c);
	for (i = 0; i < 2; ++i)
		if (guards[i])
			free_guarded(guards[i], pagesize);
exit:
	return end_test(testname);
}


int test8(struct Options options)
{
	char* testname = "test1727-8 - MQTT 5 sendMessage, version 0 response options and message";
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer5;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_responseOptions response = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message message = MQTTAsync_message_initializer;
	MQTTAsync_responseOptions* response0 = NULL;
	MQTTAsync_message* message0 = NULL;
	MQTTAsync_disconnectOptions disc = MQTTAsync_disconnectOptions_initializer;
	MQTTProperty property;
	char* guards[2] = {NULL, NULL};
	size_t pagesize = 0;
	char* topic = "issue1727/test8";
	MQTTAsync c;
	int rc, i;

	start_test(testname);
	rc = MQTTAsync_createWithOptions(&c, options.connection, "issue1727-8", MQTTCLIENT_PERSISTENCE_NONE,
			NULL, &createOpts);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	connected = 0;
	opts.onSuccess5 = onConnect5;
	opts.onFailure5 = onConnectFailure5;
	rc = MQTTAsync_connect(c, &opts);
	assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	assert("connected", wait_for_connect(), "connected was %d\n", connected);
	if (connected != 1)
		goto destroy;

	message.payload = "issue 1727";
	message.payloadlen = (int)strlen(message.payload);
	message.qos = 1;
	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "test";
	property.value.data.len = 4;
	property.value.value.data = "1727";
	property.value.value.len = 4;
	MQTTProperties_add(&message.properties, &property);

	/* MQTT 5 needs version 1 response options, so this must fail, without writing to them */
	response.struct_version = 0;
	response0 = guarded_copy(&response, offsetof(MQTTAsync_responseOptions, onSuccess5), &guards[0], &pagesize);
	response.struct_version = 1;
	assert("guarded memory allocated", response0 != NULL, "response0 was %p\n", (void*)response0);
	if (response0)
	{
		rc = MQTTAsync_sendMessage(c, topic, &message, response0);
		assert("bad MQTT option rc from sendMessage with version 0 response options",
			rc == MQTTASYNC_BAD_MQTT_OPTION, "rc was %d\n", rc);
	}

	/* the message properties are used, but must not be copied into the caller's response options */
	rc = MQTTAsync_sendMessage(c, topic, &message, &response);
	assert("good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	assert("response options properties unchanged", response.properties.count == 0 &&
		response.properties.array == NULL, "count was %d\n", response.properties.count);

	/* a version 0 message has no properties */
	message.struct_version = 0;
	message0 = guarded_copy(&message, offsetof(MQTTAsync_message, properties), &guards[1], &pagesize);
	assert("guarded memory allocated", message0 != NULL, "message0 was %p\n", (void*)message0);
	if (message0)
	{
		rc = MQTTAsync_sendMessage(c, topic, message0, &response);
		assert("good rc from sendMessage with version 0 message", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	}
	MySleep(500);

	/* the version 0 initializer includes onSuccess5, which must be called */
	disconnected = 0;
	disc.onSuccess5 = onDisconnect5;
	rc = MQTTAsync_disconnect(c, &disc);
	assert("good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	for (i = 0; disconnected == 0 && i < 500; ++i)
		MySleep(10);
	assert("disconnect onSuccess5 called with version 0 options", disconnected == 1,
		"disconnected was %d\n", disconnected);

destroy:
	MQTTAsync_destroy(&c);
	MQTTProperties_free(&message.properties);
	for (i = 0; i < 2; ++i)
		if (guards[i])
			free_guarded(guards[i], pagesize);
exit:
	return end_test(testname);
}


int main(int argc, char** argv)
{
	int rc = 0;
	int (*test_funcs[])(struct Options) = {NULL, test1, test2, test3, test4, test5, test6, test7, test8};
	int i;

	getopts(argc, argv);

	if (options.test_no == 0)
	{
		for (i = 1; i < (int)(sizeof(test_funcs) / sizeof(test_funcs[0])); ++i)
			rc += test_funcs[i](options);
	}
	else if (options.test_no < (int)(sizeof(test_funcs) / sizeof(test_funcs[0])))
		rc = test_funcs[options.test_no](options);
	else
		usage();

	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	return rc;
}
