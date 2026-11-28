/*******************************************************************************
 * Copyright (c) 2026 Roman Ondráček
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
 *    Roman Ondráček - initial implementation
 *******************************************************************************/

/**
 * @file
 * Tests for the trace callback of the synchronous client, or of the asynchronous
 * client when built with TEST_ASYNC defined.
 * The public trace callback type has a different level enum and message type from
 * the internal Log one. The callback used to be called through a pointer to the
 * internal type, which is undefined behaviour: run with -fsanitize=function (clang)
 * to detect that.
 * Creating and destroying a client produces trace messages, even when the library
 * is built with PAHO_HIGH_PERFORMANCE, so no broker is needed.
 * Test 1: the callback is called, with valid levels and messages
 * Test 2: messages below the trace level aren't passed to the callback, apart
 *         from the header written when tracing starts
 * Test 3: setting the callback to NULL stops it being called
 * Test 4: setting another callback replaces the first one
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#if defined(TEST_ASYNC)
#include "MQTTAsync.h"
typedef MQTTAsync client_handle;
#define CLIENT_SUCCESS MQTTASYNC_SUCCESS
#define client_create MQTTAsync_create
#define client_destroy MQTTAsync_destroy
#define setTraceCallback MQTTAsync_setTraceCallback
#define setTraceLevel MQTTAsync_setTraceLevel
#define TRACE_LEVELS MQTTASYNC_TRACE_LEVELS
#define TRACE_MAXIMUM_LEVEL MQTTASYNC_TRACE_MAXIMUM
#define TRACE_MINIMUM_LEVEL MQTTASYNC_TRACE_MINIMUM
#define TRACE_PROTOCOL_LEVEL MQTTASYNC_TRACE_PROTOCOL
#define TRACE_FATAL_LEVEL MQTTASYNC_TRACE_FATAL
#define CLIENT_NAME "async"
#else
#include "MQTTClient.h"
typedef MQTTClient client_handle;
#define CLIENT_SUCCESS MQTTCLIENT_SUCCESS
#define client_create MQTTClient_create
#define client_destroy MQTTClient_destroy
#define setTraceCallback MQTTClient_setTraceCallback
#define setTraceLevel MQTTClient_setTraceLevel
#define TRACE_LEVELS MQTTCLIENT_TRACE_LEVELS
#define TRACE_MAXIMUM_LEVEL MQTTCLIENT_TRACE_MAXIMUM
#define TRACE_MINIMUM_LEVEL MQTTCLIENT_TRACE_MINIMUM
#define TRACE_PROTOCOL_LEVEL MQTTCLIENT_TRACE_PROTOCOL
#define TRACE_FATAL_LEVEL MQTTCLIENT_TRACE_FATAL
#define CLIENT_NAME "sync"
#endif

#if !defined(_WINDOWS)
#include <sys/time.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

void usage(void)
{
	printf("Options:\n");
	printf("  --test_no <n>  run test number n (default all)\n");
	printf("  --verbose\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	int verbose;
	int test_no;
} options =
{
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


/* What the trace callbacks were called with. The clients are only created and
 * destroyed, which doesn't start any library threads, so the callbacks are called
 * on the main thread. */
struct trace_calls
{
	int calls;
	int invalid; /* calls with a level out of range, or no message */
	int below_level; /* calls with a level below the trace level set */
};

static struct trace_calls first;
static struct trace_calls second;
static enum TRACE_LEVELS trace_level = TRACE_MINIMUM_LEVEL;

static void record(struct trace_calls* t, enum TRACE_LEVELS level, const char* message)
{
	++t->calls;
	if (level < TRACE_MAXIMUM_LEVEL || level > TRACE_FATAL_LEVEL || message == NULL || message[0] == '\0')
		++t->invalid;
	if (level < trace_level)
		++t->below_level;
	MyLog(LOGA_DEBUG, "Trace %d: %s", level, message ? message : "(null)");
}

/* the message is char*, not const char*, because that's the public callback type */
static void traceCallback(enum TRACE_LEVELS level, char* message)
{
	record(&first, level, message);
}

static void secondTraceCallback(enum TRACE_LEVELS level, char* message)
{
	record(&second, level, message);
}

static void reset_calls(void)
{
	memset(&first, '\0', sizeof(first));
	memset(&second, '\0', sizeof(second));
}

static void reset(enum TRACE_LEVELS level)
{
	reset_calls();
	trace_level = level;
	setTraceLevel(level);
}

static int create(client_handle* c, const char* clientid)
{
	int rc = client_create(c, "tcp://localhost:1883", clientid, MQTTCLIENT_PERSISTENCE_NONE, NULL);

	assert("good rc from create", rc == CLIENT_SUCCESS, "rc was %d\n", rc);
	return rc;
}

/* create and destroy a client, which produces trace messages */
static void create_and_destroy(void)
{
	client_handle c = NULL;

	if (create(&c, "trace callback " CLIENT_NAME) == CLIENT_SUCCESS)
		client_destroy(&c);
}

static void start_test(const char* testname)
{
	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;
}

static int end_test(const char* testname)
{
	setTraceCallback(NULL);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}


int test1(struct Options options)
{
	const char* testname = "trace callback " CLIENT_NAME " 1 - the callback is called with valid levels and messages";

	start_test(testname);
	reset(TRACE_MAXIMUM_LEVEL);
	setTraceCallback(traceCallback);
	create_and_destroy();
	assert("callback called", first.calls > 0, "calls was %d\n", first.calls);
	assert("valid levels and messages", first.invalid == 0, "%d invalid calls\n", first.invalid);
	return end_test(testname);
}


int test2(struct Options options)
{
	const char* testname = "trace callback " CLIENT_NAME " 2 - messages below the trace level aren't passed";

	client_handle c = NULL;

	start_test(testname);
	reset(TRACE_PROTOCOL_LEVEL);
	setTraceCallback(traceCallback);
	/* The header written when tracing starts, with the first client, isn't filtered
	 * by level. So keep a client while another one is created and destroyed, which
	 * traces at the minimum level, below the protocol level. */
	if (create(&c, "trace callback " CLIENT_NAME " first") == CLIENT_SUCCESS)
	{
		reset_calls();
		create_and_destroy();
		assert("no messages below the trace level", first.below_level == 0,
			"%d of %d calls were below the trace level\n", first.below_level, first.calls);
		assert("valid levels and messages", first.invalid == 0, "%d invalid calls\n", first.invalid);
		client_destroy(&c);
	}
	return end_test(testname);
}


int test3(struct Options options)
{
	const char* testname = "trace callback " CLIENT_NAME " 3 - setting the callback to NULL stops it being called";

	start_test(testname);
	reset(TRACE_MINIMUM_LEVEL);
	setTraceCallback(traceCallback);
	create_and_destroy();
	assert("callback called", first.calls > 0, "calls was %d\n", first.calls);

	reset(TRACE_MINIMUM_LEVEL);
	setTraceCallback(NULL);
	create_and_destroy();
	assert("callback not called after it was set to NULL", first.calls == 0, "calls was %d\n", first.calls);
	return end_test(testname);
}


int test4(struct Options options)
{
	const char* testname = "trace callback " CLIENT_NAME " 4 - setting another callback replaces the first one";

	start_test(testname);
	reset(TRACE_MINIMUM_LEVEL);
	setTraceCallback(traceCallback);
	setTraceCallback(secondTraceCallback);
	create_and_destroy();
	assert("first callback not called", first.calls == 0, "calls was %d\n", first.calls);
	assert("second callback called", second.calls > 0, "calls was %d\n", second.calls);
	assert("valid levels and messages", second.invalid == 0, "%d invalid calls\n", second.invalid);
	return end_test(testname);
}


int main(int argc, char** argv)
{
	int rc = 0;
	int (*test_funcs[])(struct Options) = {NULL, test1, test2, test3, test4};
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
