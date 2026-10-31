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
 * Test for issue 1355: AsyncClient sends periodic messages too slowly with
 * QoS 2 and maxInflight 1.
 *
 * Two QoS 2 messages (A and B) are published back to back every period ms,
 * with maxInflight set to 1. B cannot be sent until the PUBCOMP for A has been
 * received. If the send thread is not woken when that PUBCOMP arrives, B waits
 * until the next send thread wakeup (the next API call, or the 1 second wait
 * timeout), so each B completes about one period late and a backlog builds up.
 *
 * The test measures, for each iteration, the time from the A/B sendMessage
 * calls to the completion (onSuccess, i.e. PUBCOMP) of B, and fails if that
 * is longer than max_latency ms.
 */

#include "MQTTAsync.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#if !defined(_WINDOWS)
#include <sys/time.h>
#include <unistd.h>
#else
#include <windows.h>
#define snprintf _snprintf
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

void usage(void)
{
	printf("Options:\n");
	printf("  --connection <uri>     broker to connect to (default tcp://localhost:1883)\n");
	printf("  --iterations <n>       number of A/B publish pairs (default 10)\n");
	printf("  --period <ms>          interval between pairs (default 200)\n");
	printf("  --max_latency <ms>     maximum allowed time to complete B (default 100)\n");
	printf("  --MQTTversion <3|4|5>  MQTT version (default 4)\n");
	printf("  --verbose\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	char* connection;
	int verbose;
	int test_no;
	int iterations;
	int period;
	int max_latency;
	int MQTTVersion;
} options =
{
	"tcp://localhost:1883",
	0,
	0,
	10,
	200,
	100,
	MQTTVERSION_3_1_1,
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
		else if (strcmp(argv[count], "--iterations") == 0)
		{
			if (++count < argc)
				options.iterations = atoi(argv[count]);
			else
				usage();
		}
		else if (strcmp(argv[count], "--period") == 0)
		{
			if (++count < argc)
				options.period = atoi(argv[count]);
			else
				usage();
		}
		else if (strcmp(argv[count], "--max_latency") == 0)
		{
			if (++count < argc)
				options.max_latency = atoi(argv[count]);
			else
				usage();
		}
		else if (strcmp(argv[count], "--MQTTversion") == 0)
		{
			if (++count < argc)
				options.MQTTVersion = atoi(argv[count]);
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
#if defined(_WIN32)
	Sleep(milliseconds);
#else
	usleep(milliseconds*1000);
#endif
}

/* monotonic clock in milliseconds */
long long now_ms(void)
{
#if defined(_WIN32)
	return (long long)GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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


#define MAX_ITERATIONS 1000

volatile int connected = 0;
volatile int connect_failed = 0;
volatile int disconnected = 0;
volatile int completed = 0;
volatile int publish_failures = 0;

/* per iteration: when the pair was sent, and when A and B completed */
long long sent_time[MAX_ITERATIONS];
volatile long long a_done_time[MAX_ITERATIONS];
volatile long long b_done_time[MAX_ITERATIONS];

/* context for each publish: iteration number * 2 + (0 for A, 1 for B) */
intptr_t contexts[MAX_ITERATIONS * 2];

void onConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "Connected");
	connected = 1;
}

void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_INFO, "Connect failed, rc %d", response ? response->code : -1);
	connect_failed = 1;
}

void onConnect5(void* context, MQTTAsync_successData5* response)
{
	onConnect(context, NULL);
}

void onConnectFailure5(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_INFO, "Connect failed, rc %d", response ? response->code : -1);
	connect_failed = 1;
}

void recordCompletion(void* context)
{
	intptr_t id = *(intptr_t*)context;
	int iteration = (int)(id / 2);
	long long t = now_ms();

	if (id % 2 == 0)
		a_done_time[iteration] = t;
	else
		b_done_time[iteration] = t;
	MyLog(LOGA_DEBUG, "Publish %c %d complete, %lld ms after send", (id % 2 == 0) ? 'A' : 'B',
			iteration, t - sent_time[iteration]);
	completed++;
}

void onPublish(void* context, MQTTAsync_successData* response)
{
	recordCompletion(context);
}

void onPublish5(void* context, MQTTAsync_successData5* response)
{
	recordCompletion(context);
}

void onPublishFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_INFO, "Publish failed, rc %d", response ? response->code : -1);
	publish_failures++;
	completed++;
}

void onPublishFailure5(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_INFO, "Publish failed, rc %d", response ? response->code : -1);
	publish_failures++;
	completed++;
}

void onDisconnect(void* context, MQTTAsync_successData* response)
{
	disconnected = 1;
}

void onDisconnect5(void* context, MQTTAsync_successData5* response)
{
	disconnected = 1;
}

int publish(MQTTAsync c, const char* topic, int id)
{
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	char payload[64];

	contexts[id] = id;
	opts.context = &contexts[id];
	if (options.MQTTVersion >= MQTTVERSION_5)
	{
		opts.onSuccess5 = onPublish5;
		opts.onFailure5 = onPublishFailure5;
	}
	else
	{
		opts.onSuccess = onPublish;
		opts.onFailure = onPublishFailure;
	}

	snprintf(payload, sizeof(payload), "Periodic message %c %d", (id % 2 == 0) ? 'A' : 'B', id / 2);
	pubmsg.payload = payload;
	pubmsg.payloadlen = (int)strlen(payload);
	pubmsg.qos = 2;
	pubmsg.retained = 0;
	return MQTTAsync_sendMessage(c, topic, &pubmsg, &opts);
}

int test1(struct Options options)
{
	char* testname = "test1355 - QoS 2 with maxInflight 1, back to back publishes";
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
	int rc = 0;
	int i = 0;
	int count = 0;
	int late = 0;
	long long max_b = 0, total_b = 0;
	long long start = 0, end = 0;

	MyLog(LOGA_INFO, "Starting %s", testname);
	MyLog(LOGA_INFO, "%d iterations, period %d ms, max latency %d ms, MQTT version %d",
			options.iterations, options.period, options.max_latency, options.MQTTVersion);

	createOpts.MQTTVersion = options.MQTTVersion;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "issue1355_publisher",
			MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	opts.keepAliveInterval = 20;
	opts.maxInflight = 1;
	opts.MQTTVersion = options.MQTTVersion;
	if (options.MQTTVersion >= MQTTVERSION_5)
	{
		MQTTAsync_connectOptions opts5 = MQTTAsync_connectOptions_initializer5;

		opts5.keepAliveInterval = opts.keepAliveInterval;
		opts5.maxInflight = opts.maxInflight;
		opts5.cleanstart = 1;
		opts5.onSuccess5 = onConnect5;
		opts5.onFailure5 = onConnectFailure5;
		opts = opts5;
	}
	else
	{
		opts.cleansession = 1;
		opts.onSuccess = onConnect;
		opts.onFailure = onConnectFailure;
	}

	rc = MQTTAsync_connect(c, &opts);
	assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto destroy;

	while (!connected && !connect_failed && ++count < 1000)
		MySleep(10);
	assert("connected", connected == 1, "connected was %d\n", connected);
	if (!connected)
		goto destroy;

	/* Like the issue: publish A and B with no delay between them, then sleep.
	 * B can only go once A's PUBCOMP has been received.
	 */
	start = now_ms();
	for (i = 0; i < options.iterations; ++i)
	{
		sent_time[i] = now_ms();
		rc = publish(c, "issue1355/PeriodicTopicA", i * 2);
		assert("good rc from publish A", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
		rc = publish(c, "issue1355/PeriodicTopicB", i * 2 + 1);
		assert("good rc from publish B", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
		MySleep(options.period);
	}

	/* wait for any stragglers, but don't make any more API calls in the meantime,
	 * as they would wake the send thread */
	count = 0;
	while (completed < options.iterations * 2 && ++count < 500)
		MySleep(10);
	end = now_ms();

	assert("all publishes completed", completed == options.iterations * 2,
			"completed was %d, expected %d\n", completed, options.iterations * 2);
	assert("no publish failures", publish_failures == 0,
			"publish_failures was %d\n", publish_failures);

	for (i = 0; i < options.iterations; ++i)
	{
		long long a_latency = a_done_time[i] ? a_done_time[i] - sent_time[i] : -1;
		long long b_latency = b_done_time[i] ? b_done_time[i] - sent_time[i] : -1;

		MyLog(LOGA_INFO, "Iteration %2d: A completed after %4lld ms, B completed after %4lld ms",
				i, a_latency, b_latency);
		if (b_latency < 0 || b_latency > options.max_latency)
			late++;
		if (b_latency > max_b)
			max_b = b_latency;
		if (b_latency > 0)
			total_b += b_latency;
	}
	MyLog(LOGA_INFO, "B latency: max %lld ms, average %lld ms; %d of %d B messages late",
			max_b, total_b / options.iterations, late, options.iterations);
	MyLog(LOGA_INFO, "All %d messages took %lld ms, expected about %d ms",
			options.iterations * 2, end - start, options.iterations * options.period);

	assert("B messages completed promptly after A", late == 0,
			"%d of %d B messages took longer than %d ms to complete, max %lld ms\n",
			late, options.iterations, options.max_latency, max_b);

	if (options.MQTTVersion >= MQTTVERSION_5)
		dopts.onSuccess5 = onDisconnect5;
	else
		dopts.onSuccess = onDisconnect;
	rc = MQTTAsync_disconnect(c, &dopts);
	assert("good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	count = 0;
	while (!disconnected && ++count < 200)
		MySleep(10);

destroy:
	MQTTAsync_destroy(&c);
exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}


int main(int argc, char** argv)
{
	int rc = 0;

	getopts(argc, argv);
	if (options.iterations > MAX_ITERATIONS)
		options.iterations = MAX_ITERATIONS;

	if (options.verbose)
	{
		MQTTAsync_nameValue* info = MQTTAsync_getVersionInfo();
		while (info->name)
		{
			MyLog(LOGA_INFO, "%s: %s", info->name, info->value);
			info++;
		}
	}

	rc = test1(options);

	MyLog(LOGA_INFO, "Total tests run: %d", tests);
	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	return rc;
}
