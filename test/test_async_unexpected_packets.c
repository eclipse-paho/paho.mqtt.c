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
 * Test for packets which the asynchronous client doesn't expect from a server.
 *
 * The receive thread logs a packet of a type it doesn't handle. It didn't free
 * an AUTH packet, so an MQTT 5 server could make the client's memory grow without
 * limit by sending AUTH packets. Now they're freed.
 *
 * Test 1: an MQTT 5 server sends AUTH, with properties, which are freed too.
 * Test 2: an MQTT 3.1.1 server sends PINGREQ, which has only a header, and is in
 * a static buffer, so it mustn't be freed.
 *
 * The test runs its own minimal MQTT server in a thread, which sends the packet
 * after the CONNACK, followed by a QoS 0 PUBLISH. The packets are read in order
 * by the receive thread, so once the message has arrived, the packet before it
 * has been processed. Memory left unfreed is reported by the heap tracking when
 * the client is destroyed. No broker is needed.
 */

#include "MQTTAsync.h"
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
	printf("  --test_no <n>  1: AUTH from an MQTT 5 server; 2: PINGREQ from an MQTT 3.1.1 server, not freed (default all)\n");
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
	volatile int connected;
	volatile int message_arrived;
	volatile int disconnected;
	volatile int heap_errors;
} results;

void onConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In connect onSuccess callback");
	results.connected = 1;
}

void onConnect5(void* context, MQTTAsync_successData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onSuccess5 callback");
	results.connected = 1;
}

void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_INFO, "In connect onFailure callback, rc %d", response ? response->code : 0);
	results.connected = -1;
}

void onConnectFailure5(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_INFO, "In connect onFailure5 callback, rc %d", response ? response->code : 0);
	results.connected = -1;
}

int messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MyLog(LOGA_DEBUG, "In messageArrived callback");
	results.message_arrived++;
	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);
	return 1;
}

void onDisconnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In disconnect onSuccess callback");
	results.disconnected = 1;
}

void onDisconnect5(void* context, MQTTAsync_successData5* response)
{
	MyLog(LOGA_DEBUG, "In disconnect onSuccess5 callback");
	results.disconnected = 1;
}

void traceCallback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
	if (strstr(message, "Some memory not freed") || strstr(message, "Failed to remove heap item"))
	{
		MyLog(LOGA_INFO, "Heap error: %s", message);
		results.heap_errors++;
	}
	else if (strstr(message, "Heap element size"))
		MyLog(LOGA_INFO, "%s", message);
}

/* wait for a flag to be set, up to timeout milliseconds */
void wait_for(volatile int* flag, long timeout)
{
	while (*flag == 0 && timeout > 0)
	{
		MySleep(100);
		timeout -= 100;
	}
}

/**
 * Connect to the test server, which sends the packets after the CONNACK, wait for
 * the message which follows them, disconnect, and destroy the client, which then
 * reports any memory not freed.
 */
int run_test(char* testname, int MQTTVersion, const unsigned char* packets, size_t packets_len)
{
	MQTTAsync c = NULL;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_connectOptions opts5 = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
	MQTTAsync_disconnectOptions dopts5 = MQTTAsync_disconnectOptions_initializer5;
	char uri[64];
	int rc;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;
	memset((void*)&results, '\0', sizeof(results));
	MQTTAsync_setTraceCallback(traceCallback);
	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);

	if (start_server(MQTTVersion, packets, packets_len) != 0)
	{
		assert("test server started", 0, "errno %d\n", errno);
		goto exit;
	}

	snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%d", server.port);
	createOpts.MQTTVersion = MQTTVersion;
	rc = MQTTAsync_createWithOptions(&c, uri, "unexpected packets", MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;
	MQTTAsync_setCallbacks(c, NULL, NULL, messageArrived, NULL);

	if (MQTTVersion >= MQTTVERSION_5)
	{
		opts5.keepAliveInterval = 20;
		opts5.cleanstart = 1;
		opts5.onSuccess5 = onConnect5;
		opts5.onFailure5 = onConnectFailure5;
		rc = MQTTAsync_connect(c, &opts5);
	}
	else
	{
		opts.keepAliveInterval = 20;
		opts.cleansession = 1;
		opts.MQTTVersion = MQTTVersion;
		opts.onSuccess = onConnect;
		opts.onFailure = onConnectFailure;
		rc = MQTTAsync_connect(c, &opts);
	}
	assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto destroy;
	wait_for(&results.connected, 10000);
	assert("connected", results.connected == 1, "connected %d\n", results.connected);
	if (results.connected != 1)
		goto destroy;

	/* the message follows the unexpected packet, which has then been processed */
	wait_for(&results.message_arrived, 5000);
	assert("message arrived", results.message_arrived == 1, "message_arrived %d\n", results.message_arrived);
	assert("still connected", MQTTAsync_isConnected(c), "connected %d\n", MQTTAsync_isConnected(c));

	if (MQTTVersion >= MQTTVERSION_5)
	{
		dopts5.onSuccess5 = onDisconnect5;
		rc = MQTTAsync_disconnect(c, &dopts5);
	}
	else
	{
		dopts.onSuccess = onDisconnect;
		rc = MQTTAsync_disconnect(c, &dopts);
	}
	assert("good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	wait_for(&results.disconnected, 5000);
	assert("disconnected", results.disconnected == 1, "disconnected %d\n", results.disconnected);

destroy:
	MQTTAsync_destroy(&c); /* the last client, so the heap is checked */
	assert("no memory left unfreed", results.heap_errors == 0, "%d heap errors\n", results.heap_errors);
exit:
	stop_server();
	MQTTAsync_setTraceCallback(NULL);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}

#endif

int test1(struct Options options)
{
	char* testname = "test_async_unexpected_packets - AUTH from an MQTT 5 server";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	static const unsigned char packets[] =
	{
		/* AUTH, reason code continue authentication, with a reason string */
		0xF0, 0x09, 0x18, 0x07, 0x1F, 0x00, 0x04, 'a', 'u', 't', 'h',
		/* PUBLISH QoS 0 to "t/end", no properties */
		0x30, 0x09, 0x00, 0x05, 't', '/', 'e', 'n', 'd', 0x00, 'x',
	};

	return run_test(testname, MQTTVERSION_5, packets, sizeof(packets));
#endif
}


int test2(struct Options options)
{
	char* testname = "test_async_unexpected_packets - PINGREQ from an MQTT 3.1.1 server";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	static const unsigned char packets[] =
	{
		/* PINGREQ */
		0xC0, 0x00,
		/* PUBLISH QoS 0 to "t/end" */
		0x30, 0x08, 0x00, 0x05, 't', '/', 'e', 'n', 'd', 'x',
	};

	return run_test(testname, MQTTVERSION_3_1_1, packets, sizeof(packets));
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
