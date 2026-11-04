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
 * Test for issue 1539: double free of the SSL options in MQTTAsync_connect.
 *
 * MQTTAsync_connect frees the client's copy of the SSL options from the last
 * connect, and replaces it with a copy of the new ones. It did this without
 * holding the async mutex, so two calls of connect on the same client at the
 * same time could both free the same strings.
 *
 * Two threads each call connect repeatedly on the same client, with SSL
 * options set, to a port where nothing is listening, so each connect fails
 * quickly. Each thread waits for the connect to fail before calling it again.
 * The double free crashes the program in a build without heap tracking. With
 * heap tracking, the second free is reported in the trace as an error, which
 * the test looks for.
 *
 * No broker is needed.
 */

#include "MQTTAsync.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#if !defined(_WINDOWS)
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#else
#include <windows.h>
#define snprintf _snprintf
#endif

void usage(void)
{
	printf("Options:\n");
	printf("  --test_no <n>  1: concurrent connects with SSL options (default all)\n");
	printf("  --connection <uri>  a server URI where nothing is listening (default ssl://localhost:1)\n");
	printf("  --iterations <n>  connects per thread (default 200)\n");
	printf("  --verbose\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	char* connection;
	int iterations;
	int verbose;
	int test_no;
} options =
{
	"ssl://localhost:1",
	200,
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
		else if (strcmp(argv[count], "--iterations") == 0)
		{
			if (++count < argc)
				options.iterations = atoi(argv[count]);
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


#if !defined(_WINDOWS)

#define CALLBACK_WAIT_SECONDS 10

MQTTAsync client = NULL;
MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
MQTTAsync_SSLOptions ssl_opts = MQTTAsync_SSLOptions_initializer;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
int callbacks = 0;     /* onSuccess and onFailure calls */
int heap_errors = 0;   /* bad frees reported in the trace by heap tracking */
int no_callbacks = 0;  /* connects with no callback within CALLBACK_WAIT_SECONDS */
int connect_errors = 0;

void trace_callback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
	if (strstr(message, "Failed to remove heap item"))
	{
		pthread_mutex_lock(&mutex);
		++heap_errors;
		pthread_mutex_unlock(&mutex);
		MyLog(LOGA_INFO, "Trace: %s", message);
	}
}

void connect_finished(void)
{
	pthread_mutex_lock(&mutex);
	++callbacks;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);
}

void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, rc %d", response ? response->code : 0);
	connect_finished();
}

void onConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_INFO, "Connect succeeded unexpectedly to %s", options.connection);
	connect_finished();
}

/* call connect repeatedly, waiting for each one to fail before the next */
void* connect_thread(void* arg)
{
	int i;

	for (i = 0; i < options.iterations; ++i)
	{
		int rc, before;
		struct timespec deadline;

		pthread_mutex_lock(&mutex);
		before = callbacks;
		pthread_mutex_unlock(&mutex);

		rc = MQTTAsync_connect(client, &conn_opts);
		if (rc == MQTTASYNC_COMMAND_IGNORED)
			continue; /* the other thread's connect is still queued */
		if (rc != MQTTASYNC_SUCCESS)
		{
			pthread_mutex_lock(&mutex);
			++connect_errors;
			pthread_mutex_unlock(&mutex);
			MyLog(LOGA_INFO, "MQTTAsync_connect rc %d", rc);
			continue;
		}

		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += CALLBACK_WAIT_SECONDS;
		pthread_mutex_lock(&mutex);
		while (callbacks == before)
		{
			if (pthread_cond_timedwait(&cond, &mutex, &deadline) != 0)
			{
				++no_callbacks;
				break;
			}
		}
		pthread_mutex_unlock(&mutex);
	}
	return NULL;
}

#endif

int test1(struct Options options)
{
	char* testname = "test1539 - concurrent connects with SSL options";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	pthread_t threads[2];
	int rc;
	int i;

	MyLog(LOGA_INFO, "Starting %s", testname);
	failures = 0;
	callbacks = heap_errors = no_callbacks = connect_errors = 0;

	MQTTAsync_setTraceCallback(trace_callback);
	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);

	rc = MQTTAsync_create(&client, options.connection, "issue1539", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	/* the files are never opened, as the TCP connection fails: the SSL options need only be set,
	   so that each connect frees and replaces the copies made by the last */
	ssl_opts.trustStore = "test/ssl/test-root-ca.crt";
	ssl_opts.keyStore = "test/ssl/client.pem";
	ssl_opts.privateKey = "test/ssl/client.pem";
	ssl_opts.privateKeyPassword = "password";
	ssl_opts.enabledCipherSuites = "DEFAULT";
	ssl_opts.CApath = "test/ssl";
	conn_opts.ssl = &ssl_opts;
	conn_opts.keepAliveInterval = 20;
	conn_opts.cleansession = 1;
	conn_opts.connectTimeout = 1;
	conn_opts.onSuccess = onConnect;
	conn_opts.onFailure = onConnectFailure;

	for (i = 0; i < 2; ++i)
		pthread_create(&threads[i], NULL, connect_thread, NULL);
	for (i = 0; i < 2; ++i)
		pthread_join(threads[i], NULL);

	MyLog(LOGA_INFO, "%d connect callbacks, %d connects without a callback", callbacks, no_callbacks);
	assert("no bad frees reported", heap_errors == 0, "%d bad frees reported\n", heap_errors);
	assert("connect callbacks were called", callbacks > 0, "callbacks %d\n", callbacks);
	assert("no connect errors", connect_errors == 0, "connect errors %d\n", connect_errors);

	MQTTAsync_destroy(&client);
exit:
	MQTTAsync_setTraceCallback(NULL);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
#endif
}


int main(int argc, char** argv)
{
	int rc = 0;
	int (*test_funcs[])(struct Options) = {NULL, test1};
	int i;

	getopts(argc, argv);

	if (options.test_no == 0)
	{
		for (i = 1; i < (int)(sizeof(test_funcs) / sizeof(test_funcs[0])); ++i)
			rc += test_funcs[i](options);
	}
	else if (options.test_no > 0 && options.test_no < (int)(sizeof(test_funcs) / sizeof(test_funcs[0])))
		rc = test_funcs[options.test_no](options);
	else
		usage();

	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");
	return rc;
}
