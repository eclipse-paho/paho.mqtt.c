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
 * Test for AUTH packets received by the synchronous client from an MQTT 5 server.
 *
 * Enhanced authentication isn't supported, and an AUTH packet wasn't freed, so a
 * server could make the client's memory grow without limit by sending AUTH
 * packets. Now they're freed.
 *
 * Test 1: with callbacks, so that packets are received by the background thread.
 * Test 2: without callbacks, so that packets are received by MQTTClient_receive.
 *
 * The test runs its own minimal MQTT server in a thread, which sends AUTH, with
 * properties, after the CONNACK, followed by a QoS 0 PUBLISH. The packets are read
 * in order, so once the message has arrived, the AUTH packet has been processed.
 * Memory left unfreed is reported by the heap tracking when the client is
 * destroyed. No broker is needed.
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
	printf("  --test_no <n>  1: AUTH received by the background thread; 2: AUTH received by MQTTClient_receive (default all)\n");
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
				unsigned char connack5[] = {0x20, 0x03, 0x00, 0x00, 0x00};

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
	volatile int message_arrived;
	volatile int heap_errors;
} results;

int messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	MyLog(LOGA_DEBUG, "In messageArrived callback");
	results.message_arrived++;
	MQTTClient_freeMessage(&message);
	MQTTClient_free(topicName);
	return 1;
}

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
 * Connect to the test server, which sends AUTH after the CONNACK, wait for the
 * message which follows it, disconnect, and destroy the client, which then
 * reports any memory not freed.
 * @param use_callbacks whether to set callbacks, so that packets are received by the
 * background thread, rather than by MQTTClient_receive
 */
int run_test(char* testname, int use_callbacks)
{
	static const unsigned char packets[] =
	{
		/* AUTH, reason code continue authentication, with a reason string */
		0xF0, 0x09, 0x18, 0x07, 0x1F, 0x00, 0x04, 'a', 'u', 't', 'h',
		/* PUBLISH QoS 0 to "t/end", no properties */
		0x30, 0x09, 0x00, 0x05, 't', '/', 'e', 'n', 'd', 0x00, 'x',
	};
	MQTTClient c = NULL;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTResponse response = MQTTResponse_initializer;
	char uri[64];
	int rc, i;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;
	memset((void*)&results, '\0', sizeof(results));
	MQTTClient_setTraceCallback(traceCallback);
	MQTTClient_setTraceLevel(MQTTCLIENT_TRACE_ERROR);

	if (start_server(MQTTVERSION_5, packets, sizeof(packets)) != 0)
	{
		assert("test server started", 0, "errno %d\n", errno);
		goto exit;
	}

	snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%d", server.port);
	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, uri, "unexpected packets", MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;
	if (use_callbacks)
	{
		rc = MQTTClient_setCallbacks(c, NULL, NULL, messageArrived, NULL);
		assert("good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	}

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	response = MQTTClient_connect5(c, &opts, NULL, NULL);
	rc = response.reasonCode;
	MQTTResponse_free(response);
	assert("good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto destroy;

	/* the message follows the AUTH packet, which has then been processed */
	for (i = 0; results.message_arrived == 0 && i < 50; ++i)
	{
		if (use_callbacks)
			MySleep(100);
		else
		{
			MQTTClient_message* message = NULL;
			char* topicName = NULL;
			int topicLen = 0;

			MQTTClient_receive(c, &topicName, &topicLen, &message, 100L);
			if (message)
				messageArrived(NULL, topicName, topicLen, message);
		}
	}
	assert("message arrived", results.message_arrived == 1, "message_arrived %d\n", results.message_arrived);
	assert("still connected", MQTTClient_isConnected(c), "connected %d\n", MQTTClient_isConnected(c));

	rc = MQTTClient_disconnect5(c, 1000, MQTTREASONCODE_SUCCESS, NULL);
	assert("good rc from disconnect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

destroy:
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
	char* testname = "test_sync_unexpected_packets - AUTH received by the background thread";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	return run_test(testname, 1);
#endif
}


int test2(struct Options options)
{
	char* testname = "test_sync_unexpected_packets - AUTH received by MQTTClient_receive";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	return run_test(testname, 0);
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
