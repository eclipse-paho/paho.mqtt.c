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
 * Test for issue 1705: socket descriptor leak when the last client is destroyed.
 *
 * Socket_outInitialize creates a socketpair used to interrupt poll/select.
 * Socket_outTerminate, called when the last client is destroyed, doesn't close
 * it, so the next create makes a new pair and the old one leaks: two
 * descriptors per create/destroy cycle.
 *
 * The test creates and destroys a client repeatedly, and checks that the
 * number of open descriptors in the process doesn't grow. No broker is
 * needed: the connect is made to a port nothing listens on.
 *
 * This file is built twice: against the synchronous library (MQTTClient), and
 * with ASYNC defined against the asynchronous library (MQTTAsync).
 */

#if defined(ASYNC)
#include "MQTTAsync.h"
#else
#include "MQTTClient.h"
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#if !defined(_WINDOWS)
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#else
#include <windows.h>
#define snprintf _snprintf
#endif

void usage(void)
{
	printf("Options:\n");
	printf("  --test_no <n>          1: create, connect and destroy; 2: create and destroy (default all)\n");
	printf("  --connection <uri>     URI to connect to, which must refuse connections (default tcp://127.0.0.1:1)\n");
	printf("  --cycles <n>           number of create/destroy cycles (default 20)\n");
	printf("  --verbose\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	char* connection;
	int verbose;
	int test_no;
	int cycles;
} options =
{
	"tcp://127.0.0.1:1",
	0,
	0,
	20,
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
		else if (strcmp(argv[count], "--cycles") == 0)
		{
			if (++count < argc)
				options.cycles = atoi(argv[count]);
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


/* the number of open descriptors (handles on Windows) in this process */
int open_descriptors(void)
{
#if defined(_WIN32)
	DWORD count = 0;

	GetProcessHandleCount(GetCurrentProcess(), &count);
	return (int)count;
#else
	int count = 0;
	int fd;
	int max = (int)sysconf(_SC_OPEN_MAX);

	if (max <= 0 || max > 65536)
		max = 65536;
	for (fd = 0; fd < max; ++fd)
	{
		if (fcntl(fd, F_GETFD) != -1)
			count++;
	}
	return count;
#endif
}


#if defined(ASYNC)

volatile int connect_finished = 0;

void onConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "Connected - unexpected");
	connect_finished = 1;
}

void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "Connect failed, rc %d", response ? response->code : -1);
	connect_finished = 1;
}

/* one create, (connect,) destroy cycle */
int cycle(int connect)
{
	MQTTAsync c;
	int rc = 0;

	rc = MQTTAsync_create(&c, options.connection, "issue1705", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	if (rc != MQTTASYNC_SUCCESS)
		return rc;
	if (connect)
	{
		MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
		int count = 0;

		opts.connectTimeout = 1;
		opts.onSuccess = onConnect;
		opts.onFailure = onConnectFailure;
		connect_finished = 0;
		if ((rc = MQTTAsync_connect(c, &opts)) == MQTTASYNC_SUCCESS)
		{
			while (!connect_finished && ++count < 300)
				MySleep(10);
		}
		rc = MQTTASYNC_SUCCESS; /* the connect is expected to fail */
	}
	MQTTAsync_destroy(&c);
	return rc;
}

#else

/* one create, (connect,) destroy cycle */
int cycle(int connect)
{
	MQTTClient c;
	int rc = 0;

	rc = MQTTClient_create(&c, options.connection, "issue1705", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	if (rc != MQTTCLIENT_SUCCESS)
		return rc;
	if (connect)
	{
		MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;

		opts.connectTimeout = 1;
		rc = MQTTClient_connect(c, &opts);
		MyLog(LOGA_DEBUG, "rc from connect %d", rc);
		MQTTClient_disconnect(c, 0);
		rc = MQTTCLIENT_SUCCESS; /* the connect is expected to fail */
	}
	MQTTClient_destroy(&c);
	return rc;
}

#endif


int run_test(char* testname, int connect)
{
	int start_fds = 0, end_fds = 0;
	int i = 0;
	int rc = 0;
	int bad_rcs = 0;

	MyLog(LOGA_INFO, "Starting %s", testname);

	/* one cycle first, so that anything the library opens once per process is
	 * already open when the descriptors are counted */
	cycle(connect);
	start_fds = open_descriptors();

	for (i = 0; i < options.cycles; ++i)
	{
		if ((rc = cycle(connect)) != 0)
		{
			MyLog(LOGA_INFO, "Cycle %d: rc %d", i, rc);
			bad_rcs++;
		}
		MyLog(LOGA_DEBUG, "After cycle %d, %d open descriptors", i, open_descriptors());
	}
	end_fds = open_descriptors();

	MyLog(LOGA_INFO, "%d open descriptors before %d cycles, %d after", start_fds, options.cycles, end_fds);

	assert("all cycles successful", bad_rcs == 0, "%d cycles had a bad rc\n", bad_rcs);
	/* a leak is 2 descriptors per cycle; allow a little slack for anything else
	 * in the process, such as handles on Windows */
	assert("descriptors not leaked", end_fds - start_fds < options.cycles / 2,
			"%d descriptors leaked in %d cycles\n", end_fds - start_fds, options.cycles);

	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}

int test1(struct Options options)
{
	return run_test("test1705 - no descriptors leaked by create, connect and destroy", 1);
}

int test2(struct Options options)
{
	return run_test("test1705 - no descriptors leaked by create and destroy", 0);
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

	MyLog(LOGA_INFO, "Total tests run: %d", tests);
	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	return rc;
}
