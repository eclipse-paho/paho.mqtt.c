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
 * Test for issue 1627: handling of SSL_ERROR_WANT_WRITE, and other errors, from SSL_connect.
 *
 * When SSL_connect returns SSL_ERROR_WANT_WRITE, it must be called again when the socket
 * becomes writeable. It wasn't called again at all, so the connect failed when connectTimeout
 * expired. Errors other than WANT_READ and WANT_WRITE must end the connect at once, and not
 * when connectTimeout expires.
 *
 * WANT_WRITE needs the socket's send buffer to be full, which can't be arranged reliably on a
 * local connection, so this test replaces SSL_connect and SSL_get_error with versions which
 * return the errors given by the test, before calling the real SSL_connect. Defining the
 * functions in this program replaces them for the static library, and on Linux for the shared
 * library too. On macOS, the shared library's calls are bound to the OpenSSL library, so this
 * test is only built for the static library. The test is skipped on Windows.
 *
 * The same source is built for the asynchronous (the default) and synchronous (TEST_SYNC defined)
 * clients. It needs a TLS broker which authenticates with the server certificate only, as used
 * by test3 and test5.
 */

#if !defined(_WINDOWS) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* for RTLD_NEXT on glibc */
#endif

#if defined(TEST_SYNC)
#include "MQTTClient.h"
#else
#include "MQTTAsync.h"
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#if !defined(_WINDOWS)
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <openssl/ssl.h>
#else
#include <windows.h>
#define snprintf _snprintf
#endif

void usage(void)
{
	printf("Options:\n");
	printf("  --test_no <n>  run test n (default all)\n");
	printf("  --hostname <hostname>  TLS broker host, server certificate authentication on port 18885 (default localhost)\n");
	printf("  --server_key <file>  CA certificate of the broker (default ../../../test/ssl/test-root-ca.crt)\n");
	printf("  --verbose\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	char connection[100];
	char* server_key_file;
	int verbose;
	int test_no;
} options =
{
	"ssl://localhost:18885",
	"../../../test/ssl/test-root-ca.crt",
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
		else if (strcmp(argv[count], "--hostname") == 0)
		{
			if (++count < argc)
				snprintf(options.connection, sizeof(options.connection), "ssl://%s:18885", argv[count]);
			else
				usage();
		}
		else if (strcmp(argv[count], "--server_key") == 0)
		{
			if (++count < argc)
				options.server_key_file = argv[count];
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

/* The errors that the replacement SSL_connect returns, one per call, ending with 0. When they
 * have all been returned, the real SSL_connect is called. */
#define MAX_INJECTED 10
int inject[MAX_INJECTED];
int inject_count = 0;  /* errors returned so far */
int connect_calls = 0; /* calls of the replacement SSL_connect */
SSL* injected_ssl = NULL; /* the SSL object that the last error was returned for */
int injected_error = 0;   /* the error for SSL_get_error to return, 0 if none */
pthread_mutex_t inject_mutex = PTHREAD_MUTEX_INITIALIZER;

void set_inject(int* errors)
{
	int i = 0;

	pthread_mutex_lock(&inject_mutex);
	memset(inject, '\0', sizeof(inject));
	while (errors[i] != 0 && i < MAX_INJECTED - 1)
	{
		inject[i] = errors[i];
		++i;
	}
	inject_count = connect_calls = injected_error = 0;
	injected_ssl = NULL;
	pthread_mutex_unlock(&inject_mutex);
}

/* the replacements have the same names as the real functions, so calls from the paho library go
 * to them. The real functions are found in the OpenSSL library. */
int real_SSL_connect(SSL* ssl)
{
	static int (*fn)(SSL*) = NULL;

	if (fn == NULL)
		fn = (int (*)(SSL*))dlsym(RTLD_NEXT, "SSL_connect");
	return fn(ssl);
}

int real_SSL_get_error(const SSL* ssl, int ret)
{
	static int (*fn)(const SSL*, int) = NULL;

	if (fn == NULL)
		fn = (int (*)(const SSL*, int))dlsym(RTLD_NEXT, "SSL_get_error");
	return fn(ssl, ret);
}

int SSL_connect(SSL* ssl)
{
	int error = 0;

	pthread_mutex_lock(&inject_mutex);
	++connect_calls;
	if (inject[inject_count] != 0)
	{
		error = inject[inject_count++];
		injected_ssl = ssl;
		injected_error = error;
	}
	pthread_mutex_unlock(&inject_mutex);
	if (error == 0)
		return real_SSL_connect(ssl);
	MyLog(LOGA_DEBUG, "SSL_connect returning error %d", error);
	return (error == SSL_ERROR_ZERO_RETURN) ? 0 : -1;
}

int SSL_get_error(const SSL* ssl, int ret)
{
	int error = 0;

	pthread_mutex_lock(&inject_mutex);
	if (ssl == injected_ssl && injected_error != 0)
	{
		error = injected_error;
		injected_error = 0;
	}
	pthread_mutex_unlock(&inject_mutex);
	return (error != 0) ? error : real_SSL_get_error(ssl, ret);
}

#define CONNECT_TIMEOUT 10 /* seconds: each connect must end well before this */
#define MAX_CONNECT_TIME 5

double start_time = 0.0;

double now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

#if defined(TEST_SYNC)

/* connect, and return the MQTTClient_connect return code */
int do_connect(int* errors, double* elapsed)
{
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_SSLOptions sslopts = MQTTClient_SSLOptions_initializer;
	int rc;

	rc = MQTTClient_create(&c, options.connection, "issue1627sync", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		return rc;
	sslopts.trustStore = options.server_key_file;
	sslopts.enableServerCertAuth = 1;
	opts.ssl = &sslopts;
	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.connectTimeout = CONNECT_TIMEOUT;
	opts.MQTTVersion = MQTTVERSION_3_1_1; /* so that a failed connect isn't retried with MQTT 3.1 */

	set_inject(errors);
	start_time = now();
	rc = MQTTClient_connect(c, &opts);
	*elapsed = now() - start_time;
	MyLog(LOGA_INFO, "MQTTClient_connect rc %d after %.2f seconds, %d SSL_connect calls", rc, *elapsed, connect_calls);
	if (rc == MQTTCLIENT_SUCCESS)
		MQTTClient_disconnect(c, 1000);
	MQTTClient_destroy(&c);
	return rc;
}

#else

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
int connect_result = -1; /* -1: no callback, 0: onSuccess, 1: onFailure */
double callback_time = 0.0;

void connect_finished(int result)
{
	pthread_mutex_lock(&mutex);
	connect_result = result;
	callback_time = now() - start_time;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);
}

void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, rc %d message %s", response ? response->code : 0,
			(response && response->message) ? response->message : "(none)");
	connect_finished(1);
}

void onConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In connect onSuccess callback");
	connect_finished(0);
}

/* connect, and return -1 if no callback was called, 0 for onSuccess, 1 for onFailure */
int do_connect(int* errors, double* elapsed)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	struct timespec deadline;
	int rc;

	rc = MQTTAsync_create(&c, options.connection, "issue1627", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		return -1;
	sslopts.trustStore = options.server_key_file;
	sslopts.enableServerCertAuth = 1;
	opts.ssl = &sslopts;
	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.connectTimeout = CONNECT_TIMEOUT;
	opts.MQTTVersion = MQTTVERSION_3_1_1; /* so that a failed connect isn't retried with MQTT 3.1 */
	opts.onSuccess = onConnect;
	opts.onFailure = onConnectFailure;

	set_inject(errors);
	connect_result = -1;
	start_time = now();
	rc = MQTTAsync_connect(c, &opts);
	assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc == MQTTASYNC_SUCCESS)
	{
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += CONNECT_TIMEOUT + 5;
		pthread_mutex_lock(&mutex);
		while (connect_result == -1)
		{
			if (pthread_cond_timedwait(&cond, &mutex, &deadline) != 0)
				break;
		}
		pthread_mutex_unlock(&mutex);
	}
	*elapsed = callback_time;
	MyLog(LOGA_INFO, "Connect result %s after %.2f seconds, %d SSL_connect calls",
			connect_result == 0 ? "onSuccess" : (connect_result == 1 ? "onFailure" : "no callback"),
			*elapsed, connect_calls);
	if (connect_result == 0)
		MQTTAsync_disconnect(c, NULL);
	MySleep(100);
	MQTTAsync_destroy(&c);
	return connect_result;
}

#endif

#endif /* !_WINDOWS */


#if defined(TEST_SYNC)
#define SUCCEEDED(rc) (rc == MQTTCLIENT_SUCCESS)
#define FAILED(rc) (rc != MQTTCLIENT_SUCCESS)
#define CLIENT "sync"
#else
#define SUCCEEDED(rc) (rc == 0)
#define FAILED(rc) (rc == 1)
#define CLIENT "async"
#endif

/* SSL_connect returns WANT_WRITE three times: the connect must succeed once the socket is
 * writeable, rather than fail when connectTimeout expires */
int test1(struct Options options)
{
	char* testname = "test1627 - " CLIENT " connect continues after SSL_ERROR_WANT_WRITE";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	int errors[] = {SSL_ERROR_WANT_WRITE, SSL_ERROR_WANT_WRITE, SSL_ERROR_WANT_WRITE, 0};
	double elapsed = 0.0;
	int rc;

	MyLog(LOGA_INFO, "Starting %s", testname);
	failures = 0;

	rc = do_connect(errors, &elapsed);
	assert("all the errors were returned", inject_count == 3, "%d errors returned\n", inject_count);
	assert("SSL_connect was called after the errors", connect_calls > 3, "SSL_connect calls %d\n", connect_calls);
	assert("connect succeeded", SUCCEEDED(rc), "rc was %d\n", rc);
	assert("connect finished quickly", elapsed < MAX_CONNECT_TIME, "connect took %.2f seconds\n", elapsed);

	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
#endif
}

/* SSL_connect returns WANT_WRITE, then a non-fatal error that ends the handshake: the connect
 * must fail at once, rather than when connectTimeout expires */
int test2(struct Options options)
{
	char* testname = "test1627 - " CLIENT " connect fails at once on an SSL error after WANT_WRITE";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	int errors[] = {SSL_ERROR_WANT_WRITE, SSL_ERROR_ZERO_RETURN, 0};
	double elapsed = 0.0;
	int rc;

	MyLog(LOGA_INFO, "Starting %s", testname);
	failures = 0;

	rc = do_connect(errors, &elapsed);
	assert("all the errors were returned", inject_count == 2, "%d errors returned\n", inject_count);
	assert("connect failed", FAILED(rc), "rc was %d\n", rc);
	assert("connect failed quickly", elapsed < MAX_CONNECT_TIME, "connect took %.2f seconds\n", elapsed);

	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
#endif
}

/* the first call of SSL_connect returns a non-fatal error that ends the handshake: the connect
 * must fail at once, rather than when connectTimeout expires */
int test3(struct Options options)
{
	char* testname = "test1627 - " CLIENT " connect fails at once on an SSL error";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	int errors[] = {SSL_ERROR_ZERO_RETURN, 0};
	double elapsed = 0.0;
	int rc;

	MyLog(LOGA_INFO, "Starting %s", testname);
	failures = 0;

	rc = do_connect(errors, &elapsed);
	assert("all the errors were returned", inject_count == 1, "%d errors returned\n", inject_count);
	assert("connect failed", FAILED(rc), "rc was %d\n", rc);
	assert("connect failed quickly", elapsed < MAX_CONNECT_TIME, "connect took %.2f seconds\n", elapsed);

	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
#endif
}


int main(int argc, char** argv)
{
	int rc = 0;
	int (*test_funcs[])(struct Options) = {NULL, test1, test2, test3};
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
