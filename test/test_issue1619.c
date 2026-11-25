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
 * Test for issue 1619 in the synchronous client: connectionLost wasn't called when
 * an MQTT 5 server sent DISCONNECT, if a disconnected callback was set. Without a
 * disconnected callback, the DISCONNECT packet was never freed.
 * Client c connects, then client d connects with the same client id, so the server
 * sends DISCONNECT with reason code 0x8E (session taken over) to client c.
 * Test 1: with a disconnected callback, which should be called before connectionLost
 * Test 2: without a disconnected callback
 * Both tests check that connectionLost is called, that client c isn't connected
 * afterwards, and that no memory is left unfreed when the library terminates.
 */

#include "MQTTClient.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#if !defined(_WINDOWS)
#include <sys/time.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

void usage(void)
{
	printf("Options:\n");
	printf("  --test_no <n>  1: with a disconnected callback; 2: without (default all)\n");
	printf("  --connection <uri>  broker to connect to (default tcp://localhost:1883)\n");
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

void MySleep(long milliseconds)
{
#if defined(_WIN32) || defined(_WINDOWS)
	Sleep(milliseconds);
#else
	usleep(milliseconds*1000);
#endif
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


/* the order in which the callbacks were called: 1 for the first, 2 for the second */
volatile int callback_count = 0;
volatile int connection_lost = 0;
volatile int disconnected = 0;
volatile enum MQTTReasonCodes disconnect_reason = MQTTREASONCODE_SUCCESS;
int heap_errors = 0;

void connectionLost(void* context, char* cause)
{
	MyLog(LOGA_DEBUG, "Callback: connectionLost");
	connection_lost = ++callback_count;
}

void disconnectedCallback(void* context, MQTTProperties* props, enum MQTTReasonCodes rc)
{
	MyLog(LOGA_DEBUG, "Callback: disconnected, reason code \"%s\"", MQTTReasonCode_toString(rc));
	disconnect_reason = rc;
	disconnected = ++callback_count;
}

int messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	MQTTClient_freeMessage(&message);
	MQTTClient_free(topicName);
	return 1;
}

void traceCallback(enum MQTTCLIENT_TRACE_LEVELS level, char* message)
{
	if (strstr(message, "Some memory not freed") || strstr(message, "Failed to remove heap item"))
	{
		MyLog(LOGA_INFO, "Heap error: %s", message);
		++heap_errors;
	}
	else if (strstr(message, "Heap element size"))
		MyLog(LOGA_INFO, "%s", message);
}


int run_test(char* testname, int set_disconnected)
{
	MQTTClient c = NULL, d = NULL;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTResponse response = MQTTResponse_initializer;
	char* clientid = "issue1619 sync session taken over";
	int rc, i;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = heap_errors = 0;
	callback_count = connection_lost = disconnected = 0;
	disconnect_reason = MQTTREASONCODE_SUCCESS;
	MQTTClient_setTraceCallback(traceCallback);
	MQTTClient_setTraceLevel(MQTTCLIENT_TRACE_ERROR);

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, clientid, MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
	assert("good rc from create c", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	rc = MQTTClient_createWithOptions(&d, options.connection, clientid, MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
	assert("good rc from create d", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (c == NULL || d == NULL)
		goto destroy;

	rc = MQTTClient_setCallbacks(c, NULL, connectionLost, messageArrived, NULL);
	assert("good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (set_disconnected)
	{
		rc = MQTTClient_setDisconnected(c, NULL, disconnectedCallback);
		assert("good rc from setDisconnected", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	}

	opts.cleanstart = 1;
	response = MQTTClient_connect5(c, &opts, NULL, NULL);
	assert("client c connected", response.reasonCode == MQTTREASONCODE_SUCCESS, "reason code was %d\n", response.reasonCode);
	MQTTResponse_free(response);
	if (!MQTTClient_isConnected(c))
		goto destroy;

	/* connecting with the same client id makes the server disconnect client c */
	response = MQTTClient_connect5(d, &opts, NULL, NULL);
	assert("client d connected", response.reasonCode == MQTTREASONCODE_SUCCESS, "reason code was %d\n", response.reasonCode);
	MQTTResponse_free(response);

	for (i = 0; i < 500 && (connection_lost == 0 || (set_disconnected && disconnected == 0)); ++i)
		MySleep(10);
	MyLog(LOGA_INFO, "connectionLost %d, disconnected %d, reason code 0x%x", connection_lost, disconnected, disconnect_reason);

	assert("connectionLost called", connection_lost > 0, "connection_lost was %d\n", connection_lost);
	assert("client c not connected", MQTTClient_isConnected(c) == 0, "isConnected was %d\n", MQTTClient_isConnected(c));
	if (set_disconnected)
	{
		assert("disconnected called", disconnected > 0, "disconnected was %d\n", disconnected);
		assert("reason code is session taken over", disconnect_reason == MQTTREASONCODE_SESSION_TAKEN_OVER,
			"reason code was 0x%x\n", disconnect_reason);
		assert("disconnected called before connectionLost", disconnected == 1 && connection_lost == 2,
			"disconnected was %d, connectionLost %d\n", disconnected, connection_lost);
	}
	else
		assert("disconnected not called", disconnected == 0, "disconnected was %d\n", disconnected);

	MQTTClient_disconnect5(d, 0, MQTTREASONCODE_SUCCESS, NULL);

destroy:
	/* the library terminates when the last client is destroyed, and reports any memory not freed */
	if (c)
		MQTTClient_destroy(&c);
	if (d)
		MQTTClient_destroy(&d);
	assert("no memory left unfreed", heap_errors == 0, "%d heap errors\n", heap_errors);
	MQTTClient_setTraceCallback(NULL);

	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}

int test1(struct Options options)
{
	return run_test("test1619-1 - sync, server DISCONNECT, with a disconnected callback", 1);
}

int test2(struct Options options)
{
	return run_test("test1619-2 - sync, server DISCONNECT, without a disconnected callback", 0);
}


int main(int argc, char** argv)
{
	int rc = 0;
	int (*test_funcs[])(struct Options) = {NULL, test1, test2};
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
