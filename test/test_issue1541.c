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
 * Test for issue 1541: disconnect onSuccess or onFailure not called after connectionLost.
 *
 * When there are inflight QoS 1 or 2 messages, MQTTAsync_disconnect waits for
 * them to complete, up to its timeout, before closing the connection and
 * calling onSuccess. Whether they have completed is checked periodically.
 *
 * Test 1: if the connection is lost during that wait, the session is closed
 * and connectionLost is called, but the disconnect was no longer checked, so
 * neither of its callbacks was called. Now onFailure is called, followed by
 * connectionLost.
 * Test 2: once the client had been connected for longer than the connect
 * timeout, the wait was treated as a connect timing out at the next check,
 * which closed the connection, called connectionLost, and again called neither
 * disconnect callback. Now the disconnect waits for its own timeout, then
 * calls onSuccess.
 *
 * The test runs its own minimal MQTT server in a thread, which never
 * acknowledges a PUBLISH, so the message stays inflight. No broker is needed.
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
	printf("  --test_no <n>  1: connection lost during disconnect; 2: disconnect after the connect timeout (default all)\n");
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

#define TOPIC "issue1541/disconnect"
#define DISCONNECT_TIMEOUT 4000 /* milliseconds: long enough for the connection to be lost, or the connect timeout to pass, first */

/*
 * A minimal MQTT 3.1.1 server, enough for this test: it accepts connections one
 * after the other, answers CONNECT and PINGREQ, and never acknowledges a PUBLISH.
 */
struct
{
	int listen_fd;
	int port;
	volatile int conn_fd;
	pthread_t thread;
} server = {-1, 0, -1};

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

		server.conn_fd = fd;
		while (read_packet(fd, &type) == 0)
		{
			if ((type >> 4) == 1) /* CONNECT */
			{
				unsigned char connack[] = {0x20, 0x02, 0x00, 0x00};
				send(fd, connack, sizeof(connack), 0);
			}
			else if ((type >> 4) == 12) /* PINGREQ */
			{
				unsigned char pingresp[] = {0xD0, 0x00};
				send(fd, pingresp, sizeof(pingresp), 0);
			}
			else if ((type >> 4) == 14) /* DISCONNECT */
				break;
			/* PUBLISH is never acknowledged */
		}
		server.conn_fd = -1;
		close(fd);
	}
	return NULL;
}

int start_server(void)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	int on = 1;

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

/* close the connection from the server side */
void server_close_connection(void)
{
	int fd = server.conn_fd;

	if (fd >= 0)
		shutdown(fd, SHUT_RDWR);
}


struct
{
	volatile int connected;
	volatile int connection_lost;
	volatile int disconnect_success;
	volatile int disconnect_failure;
	volatile int disconnect_failure_code;
	volatile int sequence;       /* order of the callbacks */
	volatile int connection_lost_seq;
	volatile int disconnect_failure_seq;
} results;

void onConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In connect onSuccess callback");
	results.connected = 1;
}

void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_INFO, "In connect onFailure callback, rc %d", response ? response->code : 0);
	results.connected = -1;
}

void connectionLost(void* context, char* cause)
{
	MyLog(LOGA_INFO, "In connectionLost callback");
	results.connection_lost++;
	results.connection_lost_seq = ++results.sequence;
}

int messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);
	return 1;
}

void onDisconnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_INFO, "In disconnect onSuccess callback");
	results.disconnect_success++;
}

void onDisconnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_INFO, "In disconnect onFailure callback, rc %d", response ? response->code : 0);
	results.disconnect_failure++;
	results.disconnect_failure_code = response ? response->code : 0;
	results.disconnect_failure_seq = ++results.sequence;
}

/* wait for any disconnect callback, up to timeout milliseconds */
void wait_for_disconnect_callback(long timeout)
{
	while (results.disconnect_success == 0 && results.disconnect_failure == 0 && timeout > 0)
	{
		MySleep(100);
		timeout -= 100;
	}
}

/**
 * Connect to the test server and publish a QoS 1 message, which won't be acknowledged.
 * @return the client, or NULL on failure
 */
MQTTAsync connect_and_publish(int connect_timeout)
{
	MQTTAsync c = NULL;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	char uri[64];
	int rc;
	int count = 0;

	memset(&results, '\0', sizeof(results));
	snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%d", server.port);
	rc = MQTTAsync_create(&c, uri, "issue1541", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		return NULL;
	MQTTAsync_setCallbacks(c, NULL, connectionLost, messageArrived, NULL);

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.connectTimeout = connect_timeout;
	opts.onSuccess = onConnect;
	opts.onFailure = onConnectFailure;
	rc = MQTTAsync_connect(c, &opts);
	assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	while (results.connected == 0 && ++count < 100)
		MySleep(100);
	assert("connected", results.connected == 1, "connected %d\n", results.connected);
	if (results.connected != 1)
		goto error;

	pubmsg.payload = "inflight message";
	pubmsg.payloadlen = (int)strlen(pubmsg.payload);
	pubmsg.qos = 1;
	rc = MQTTAsync_sendMessage(c, TOPIC, &pubmsg, NULL);
	assert("good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto error;
	MySleep(200); /* let it be written */
	return c;

error:
	MQTTAsync_destroy(&c);
	return NULL;
}

int disconnect(MQTTAsync c)
{
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;

	opts.timeout = DISCONNECT_TIMEOUT;
	opts.onSuccess = onDisconnect;
	opts.onFailure = onDisconnectFailure;
	return MQTTAsync_disconnect(c, &opts);
}

#endif

int test1(struct Options options)
{
	char* testname = "test1541 - connection lost while disconnect waits for an inflight message";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	MQTTAsync c = NULL;
	int rc;

	MyLog(LOGA_INFO, "Starting %s", testname);
	failures = 0;
	if (start_server() != 0)
	{
		assert("test server started", 0, "errno %d\n", errno);
		goto exit;
	}
	if ((c = connect_and_publish(30)) == NULL)
		goto exit;

	rc = disconnect(c);
	assert("good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	MySleep(500); /* the disconnect is now waiting for the inflight message */
	server_close_connection();

	wait_for_disconnect_callback(DISCONNECT_TIMEOUT + 5000);
	MySleep(500); /* allow time for any other callbacks */
	assert("disconnect onFailure called", results.disconnect_failure == 1,
			"onFailure called %d times\n", results.disconnect_failure);
	assert("disconnect onFailure code is MQTTASYNC_DISCONNECTED", results.disconnect_failure_code == MQTTASYNC_DISCONNECTED,
			"code was %d\n", results.disconnect_failure_code);
	assert("disconnect onSuccess not called", results.disconnect_success == 0,
			"onSuccess called %d times\n", results.disconnect_success);
	assert("connectionLost called", results.connection_lost == 1,
			"connectionLost called %d times\n", results.connection_lost);
	assert("disconnect onFailure called before connectionLost",
			results.disconnect_failure_seq > 0 && results.disconnect_failure_seq < results.connection_lost_seq,
			"onFailure %d, connectionLost %d\n", results.disconnect_failure_seq, results.connection_lost_seq);

	MQTTAsync_destroy(&c);
exit:
	stop_server();
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
#endif
}


int test2(struct Options options)
{
	char* testname = "test1541 - disconnect waiting for an inflight message after the connect timeout";
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
	return 0;
#else
	MQTTAsync c = NULL;
	int rc;

	MyLog(LOGA_INFO, "Starting %s", testname);
	failures = 0;
	if (start_server() != 0)
	{
		assert("test server started", 0, "errno %d\n", errno);
		goto exit;
	}
	if ((c = connect_and_publish(1)) == NULL)
		goto exit;
	MySleep(1500); /* connected for longer than the connect timeout */

	rc = disconnect(c);
	assert("good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);

	/* the disconnect timeout is checked every 3 seconds, so the callback can be that late */
	wait_for_disconnect_callback(DISCONNECT_TIMEOUT + 5000);
	MySleep(500); /* allow time for any other callbacks */
	assert("disconnect onSuccess called", results.disconnect_success == 1,
			"onSuccess called %d times\n", results.disconnect_success);
	assert("disconnect onFailure not called", results.disconnect_failure == 0,
			"onFailure called %d times\n", results.disconnect_failure);
	assert("connectionLost not called", results.connection_lost == 0,
			"connectionLost called %d times\n", results.connection_lost);

	MQTTAsync_destroy(&c);
exit:
	stop_server();
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
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
