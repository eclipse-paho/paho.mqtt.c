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
 * Test for MQTTClient_connect, when the options ask for MQTT 5.
 *
 * MQTTClient_connect returns only the reason code, so it can't return the CONNACK
 * properties which MQTTClient_connect5 returns in its response. When a client
 * created with MQTTClient_create connected with MQTTVersion 5 in the options,
 * the connect succeeded as MQTT 5, and the properties were never freed. The
 * client was then an MQTT 5 client, although it wasn't created for MQTT 5, as
 * MQTTClient_createOptions requires. Now MQTTClient_connect rejects MQTT 5.
 *
 * Test 1: MQTTClient_connect, with MQTTVersion 5 in the options, is rejected with
 * MQTTCLIENT_WRONG_MQTT_VERSION, and the client can still connect with MQTT 3.1.1.
 * Test 2: MQTTClient_connect5, with a client created for MQTT 5, and the response
 * freed by the test, as an application does.
 *
 * The test runs its own minimal MQTT server in a thread, which answers CONNECT
 * with a CONNACK, with a reason string property for MQTT 5. Memory left unfreed is
 * reported by the heap tracking when the client is destroyed. No broker is needed.
 */


#include "MQTTClient.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#if !defined(_WINDOWS)
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#else
#include <windows.h>
#define snprintf _snprintf
#endif

void usage(void)
{
	printf("Options:\n");
	printf("  --test_no <n>  1: MQTTClient_connect with MQTT 5 rejected; 2: MQTTClient_connect5 (default all)\n");
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

/*
 * A minimal MQTT server, enough for this test: it accepts connections one after
 * the other, answers CONNECT with a CONNACK followed by the packets of the test,
 * and then reads until the client disconnects.
 */
struct
{
	int listen_fd;
	int port;
	pthread_t thread;
	int MQTTVersion;           /* of the CONNACK */
	const unsigned char* packets; /* sent after the CONNACK */
	size_t packets_len;
} server = {-1, 0};

int read_packet(int fd, unsigned char* type)
{
	unsigned char b;
	int len = 0, multiplier = 1;
	char buf[1024];

	if (recv(fd, type, 1, 0) != 1)
		return -1;
	do
	{
		if (recv(fd, &b, 1, 0) != 1)
			return -1;
		len += (b & 127) * multiplier;
		multiplier *= 128;
	} while (b & 128);
	while (len > 0)
	{
		int n = (int)recv(fd, buf, (len > (int)sizeof(buf)) ? sizeof(buf) : (size_t)len, 0);
		if (n <= 0)
			return -1;
		len -= n;
	}
	return 0;
}

void* server_thread(void* arg)
{
	int fd;

	while ((fd = accept(server.listen_fd, NULL, NULL)) >= 0)
	{
		unsigned char type;

		while (read_packet(fd, &type) == 0)
		{
			if ((type >> 4) == 1) /* CONNECT */
			{
				unsigned char connack3[] = {0x20, 0x02, 0x00, 0x00};
				/* with a reason string property, which the client copies into the connect response */
				unsigned char connack5[] = {0x20, 0x0B, 0x00, 0x00, 0x08, 0x1F, 0x00, 0x05, 'h', 'e', 'l', 'l', 'o'};

				if (server.MQTTVersion >= MQTTVERSION_5)
					send(fd, connack5, sizeof(connack5), 0);
				else
					send(fd, connack3, sizeof(connack3), 0);
				send(fd, server.packets, server.packets_len, 0);
			}
			else if ((type >> 4) == 14) /* DISCONNECT */
				break;
		}
		close(fd);
	}
	return NULL;
}

int start_server(int MQTTVersion, const unsigned char* packets, size_t packets_len)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	int on = 1;

	server.MQTTVersion = MQTTVersion;
	server.packets = packets;
	server.packets_len = packets_len;
	if ((server.listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1;
	setsockopt(server.listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&addr, '\0', sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(server.listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
		listen(server.listen_fd, 5) != 0 ||
		getsockname(server.listen_fd, (struct sockaddr*)&addr, &len) != 0)
		return -1;
	server.port = ntohs(addr.sin_port);
	return pthread_create(&server.thread, NULL, server_thread, NULL);
}

void stop_server(void)
{
	if (server.listen_fd >= 0)
	{
		shutdown(server.listen_fd, SHUT_RDWR);
		close(server.listen_fd);
		pthread_join(server.thread, NULL);
		server.listen_fd = -1;
	}
}


struct
{
	volatile int heap_errors;
} results;

void traceCallback(enum MQTTCLIENT_TRACE_LEVELS level, char* message)
{
	if (strstr(message, "Some memory not freed") || strstr(message, "Failed to remove heap item"))
	{
		MyLog(LOGA_INFO, "Heap error: %s", message);
		results.heap_errors++;
	}
	else if (strstr(message, "Heap element size"))
		MyLog(LOGA_INFO, "%s", message);
}

/**
 * Connect to the test server, disconnect, and destroy the client, which then
 * reports any memory not freed.
 * @param use_connect5 whether to connect with MQTTClient_connect5, with a client
 * created for MQTT 5, rather than with MQTTClient_connect, with a client created
 * by MQTTClient_create, which is rejected with MQTT 5 in the options and then
 * connects with MQTT 3.1.1
 */
int run_test(char* testname, int use_connect5)
{
	MQTTClient c = NULL;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	char uri[64];
	int rc;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;
	memset((void*)&results, '\0', sizeof(results));
	MQTTClient_setTraceCallback(traceCallback);
	MQTTClient_setTraceLevel(MQTTCLIENT_TRACE_ERROR);

	/* for MQTTClient_connect, the server answers the MQTT 3.1.1 connect which follows the rejected MQTT 5 one */
	if (start_server(use_connect5 ? MQTTVERSION_5 : MQTTVERSION_3_1_1, NULL, 0) != 0)
	{
		assert("test server started", 0, "errno %d\n", errno);
		goto exit;
	}

	snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%d", server.port);
	if (use_connect5)
	{
		createOpts.MQTTVersion = MQTTVERSION_5;
		rc = MQTTClient_createWithOptions(&c, uri, "connect response", MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
	}
	else /* an MQTT 3.1.1 client, which MQTTClient_connect accepts */
		rc = MQTTClient_create(&c, uri, "connect response", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	if (use_connect5)
	{
		MQTTResponse response = MQTTClient_connect5(c, &opts, NULL, NULL);

		rc = response.reasonCode;
		assert("CONNACK properties in the response", response.properties != NULL && response.properties->count == 1,
				"properties %p\n", (void*)response.properties);
		MQTTResponse_free(response);
	}
	else
	{
		rc = MQTTClient_connect(c, &opts);
		assert("MQTT 5 rejected by MQTTClient_connect", rc == MQTTCLIENT_WRONG_MQTT_VERSION, "rc was %d\n", rc);
		assert("not connected", !MQTTClient_isConnected(c), "connected %d\n", MQTTClient_isConnected(c));
		if (MQTTClient_isConnected(c)) /* connected with MQTT 5, so close the server's connection */
			MQTTClient_disconnect(c, 0);
		opts.MQTTVersion = MQTTVERSION_3_1_1;
		opts.cleanstart = 0; /* MQTT 5 only */
		opts.cleansession = 1;
		rc = MQTTClient_connect(c, &opts);
	}
	assert("good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	if (rc == MQTTCLIENT_SUCCESS)
	{
		rc = use_connect5 ? MQTTClient_disconnect5(c, 1000, MQTTREASONCODE_SUCCESS, NULL) : MQTTClient_disconnect(c, 1000);
		assert("good rc from disconnect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	}

	MQTTClient_destroy(&c); /* the last client, so the heap is checked */
	assert("no memory left unfreed", results.heap_errors == 0, "%d heap errors\n", results.heap_errors);
exit:
	stop_server();
	MQTTClient_setTraceCallback(NULL);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}

#endif

int test1(struct Options options)
{
	char* testname = "test_sync_connect_response - MQTTClient_connect with MQTT 5 rejected";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	return run_test(testname, 0);
#endif
}


int test2(struct Options options)
{
	char* testname = "test_sync_connect_response - MQTTClient_connect5";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	return run_test(testname, 1);
#endif
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
