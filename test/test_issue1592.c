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
 * Test for issues 1592 and 1310: corrupted PUBLISH after a socket write error.
 *
 * When the write of a QoS 1 or 2 PUBLISH fails with a socket error (such as
 * EPIPE or ECONNRESET, when the broker has dropped the connection), the async
 * client calls the publish failure callback and frees the command, including
 * its topic and payload. But those were already handed over to the stored
 * message kept for retry, so the stored message is left pointing at freed
 * memory. When the client reconnects and resends the message, the topic and
 * payload are garbage, or the memory is freed twice.
 *
 * To make the write fail at a known point, the test runs its own minimal MQTT
 * server in a thread, and once the client has connected, shuts down the
 * sending side of the client's socket. The client doesn't notice, as reads
 * still work, but its next write fails with EPIPE. A QoS 1 message is then
 * published, which should fail. The test then reuses any freed memory, by
 * allocating and filling a lot of memory, and reconnects with the same
 * session. The server checks the topic and payload of the resent PUBLISH.
 *
 * Test 1 uses cleansession 0, so the message is kept and resent on reconnect.
 * Test 2 uses cleansession 1, so the message is discarded at the disconnect.
 * In both cases the stored message's topic and payload are freed a second time
 * when it is discarded. Builds with heap tracking report that, which fails the
 * test; in high performance builds the C library may abort the process.
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
	printf("  --test_no <n>  1: cleansession 0, message resent; 2: cleansession 1 (default all)\n");
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

#define TOPIC "issue1592/corrupted/publish"
#define PAYLOAD_LEN 1000

char payload[PAYLOAD_LEN];

/*
 * A minimal MQTT 3.1.1 server, enough for this test: it accepts connections one
 * after the other, answers CONNECT, PUBLISH and PINGREQ, and records the
 * PUBLISH packets it receives.
 */

struct
{
	int listener;
	int port;
	int connections;            /* number of connections accepted */
	int session_present;        /* value for the CONNACK session present flag */
	int publishes;              /* number of PUBLISH packets received */
	int good_publishes;         /* number with the expected topic and payload */
	int stop;
	int conn_fds[8];
} server;

int read_fully(int fd, char* buf, size_t len)
{
	size_t got = 0;

	while (got < len)
	{
		ssize_t rc = recv(fd, buf + got, len - got, 0);
		if (rc <= 0)
			return -1;
		got += rc;
	}
	return 0;
}

/* read an MQTT packet: returns the first header byte, or -1 at EOF */
int read_packet(int fd, char** body, size_t* bodylen)
{
	unsigned char header = 0;
	size_t len = 0;
	int multiplier = 1;
	unsigned char c = 0;

	if (read_fully(fd, (char*)&header, 1) != 0)
		return -1;
	do
	{
		if (read_fully(fd, (char*)&c, 1) != 0)
			return -1;
		len += (c & 127) * multiplier;
		multiplier *= 128;
	} while (c & 128);
	*body = malloc(len + 1);
	if (len > 0 && read_fully(fd, *body, len) != 0)
	{
		free(*body);
		return -1;
	}
	*bodylen = len;
	return header;
}

void handle_publish(int fd, unsigned char header, char* body, size_t bodylen)
{
	int qos = (header >> 1) & 0x03;
	size_t topiclen = ((unsigned char)body[0] << 8) + (unsigned char)body[1];
	char* topic = &body[2];
	size_t pos = 2 + topiclen;
	int msgid = 0;
	int good = 0;

	if (qos > 0)
	{
		msgid = ((unsigned char)body[pos] << 8) + (unsigned char)body[pos + 1];
		pos += 2;
	}
	good = topiclen == strlen(TOPIC) && memcmp(topic, TOPIC, topiclen) == 0 &&
		bodylen - pos == PAYLOAD_LEN && memcmp(&body[pos], payload, PAYLOAD_LEN) == 0;
	MyLog(LOGA_INFO, "Server: PUBLISH received, dup %d qos %d msgid %d, topic length %d, payload length %d, %s",
		(header >> 3) & 0x01, qos, msgid, (int)topiclen, (int)(bodylen - pos),
		good ? "topic and payload correct" : "topic or payload CORRUPTED");
	server.publishes++;
	if (good)
		server.good_publishes++;
	if (qos > 0)
	{
		char ack[4] = {(qos == 1) ? 0x40 : 0x50, 0x02, (char)(msgid >> 8), (char)(msgid & 0xFF)};
		send(fd, ack, sizeof(ack), 0);
	}
}

void* server_thread(void* arg)
{
	while (!server.stop && server.connections < (int)(sizeof(server.conn_fds) / sizeof(server.conn_fds[0])))
	{
		int fd = accept(server.listener, NULL, NULL);
		int header = 0;
		char* body = NULL;
		size_t bodylen = 0;

		if (fd < 0)
			break;
		server.conn_fds[server.connections++] = fd;
		MyLog(LOGA_DEBUG, "Server: connection %d accepted", server.connections);
		while ((header = read_packet(fd, &body, &bodylen)) != -1)
		{
			switch (header >> 4)
			{
			case 1: /* CONNECT */
			{
				char connack[4] = {0x20, 0x02, (char)server.session_present, 0x00};
				send(fd, connack, sizeof(connack), 0);
				break;
			}
			case 3: /* PUBLISH */
				handle_publish(fd, (unsigned char)header, body, bodylen);
				break;
			case 12: /* PINGREQ */
			{
				char pingresp[2] = {(char)0xD0, 0x00};
				send(fd, pingresp, sizeof(pingresp), 0);
				break;
			}
			default:
				break;
			}
			free(body);
			body = NULL;
		}
		/* EOF: the client has closed or shut down its side. Keep this end open,
		 * so the client isn't told about the closure, and wait for the next connection */
		MyLog(LOGA_DEBUG, "Server: connection %d ended", server.connections);
	}
	return NULL;
}

int start_server(void)
{
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	pthread_t thread;

	memset(&server, 0, sizeof(server));
	server.listener = socket(AF_INET, SOCK_STREAM, 0);
	if (server.listener == -1)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(server.listener, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
		listen(server.listener, 5) != 0 ||
		getsockname(server.listener, (struct sockaddr*)&addr, &addrlen) != 0)
		return -1;
	server.port = ntohs(addr.sin_port);
	if (pthread_create(&thread, NULL, server_thread, NULL) != 0)
		return -1;
	pthread_detach(thread);
	return 0;
}

void stop_server(void)
{
	int i;

	server.stop = 1;
	shutdown(server.listener, SHUT_RDWR);
	close(server.listener);
	for (i = 0; i < server.connections; ++i)
		close(server.conn_fds[i]);
}

/* find the client's socket: the one whose peer is the server's port */
int find_client_socket(void)
{
	int fd;
	int max = (int)sysconf(_SC_OPEN_MAX);

	if (max <= 0 || max > 65536)
		max = 65536;
	for (fd = 0; fd < max; ++fd)
	{
		struct sockaddr_in peer;
		socklen_t peerlen = sizeof(peer);

		if (getpeername(fd, (struct sockaddr*)&peer, &peerlen) == 0 &&
			peer.sin_family == AF_INET && ntohs(peer.sin_port) == server.port)
			return fd;
	}
	return -1;
}

/* Allocate and fill lots of memory, so that anything freed is reused and overwritten */
#define SCRIBBLES 4096
void* scribbles[SCRIBBLES];

void scribble_heap(void)
{
	size_t sizes[] = {16, 24, 32, 40, 48, 64, PAYLOAD_LEN, PAYLOAD_LEN + 8, PAYLOAD_LEN + 16, PAYLOAD_LEN + 24, PAYLOAD_LEN + 32};
	int i;

	for (i = 0; i < SCRIBBLES; ++i)
	{
		size_t size = sizes[i % (sizeof(sizes) / sizeof(sizes[0]))];

		if ((scribbles[i] = malloc(size)) != NULL)
			memset(scribbles[i], 'X', size);
	}
}

void free_scribbles(void)
{
	int i;

	for (i = 0; i < SCRIBBLES; ++i)
	{
		free(scribbles[i]);
		scribbles[i] = NULL;
	}
}

#endif /* !_WINDOWS */


volatile int connected = 0;
volatile int connect_failed = 0;
volatile int connection_lost = 0;
volatile int published = 0;
volatile int publish_failed = 0;
volatile int publish_failure_code = 0;
volatile int heap_errors = 0;

/* the heap tracking in non high performance builds reports a double free like this */
void traceCallback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
	if (strstr(message, "Failed to remove heap item"))
	{
		MyLog(LOGA_INFO, "Library error: %s", message);
		heap_errors++;
	}
}

void connectionLost(void* context, char* cause)
{
	MyLog(LOGA_DEBUG, "Connection lost");
	connection_lost = 1;
}

int messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);
	return 1;
}

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

void onPublish(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "Publish succeeded");
	published = 1;
}

void onPublishFailure(void* context, MQTTAsync_failureData* response)
{
	publish_failure_code = response ? response->code : -1;
	MyLog(LOGA_INFO, "Publish failed, rc %d", publish_failure_code);
	publish_failed = 1;
}

int wait_for(volatile int* flag, int ms)
{
	int count = 0;

	while (!*flag && count < ms / 10)
	{
		MySleep(10);
		count++;
	}
	return *flag;
}

int run_test(char* testname, int cleansession)
{
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
#else
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	char uri[64];
	int rc = 0;
	int fd = -1;
	int i;

	MyLog(LOGA_INFO, "Starting %s", testname);
	connected = connect_failed = connection_lost = published = publish_failed = publish_failure_code = heap_errors = 0;
	MQTTAsync_setTraceCallback(traceCallback);
	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);

	for (i = 0; i < PAYLOAD_LEN; ++i)
		payload[i] = '0' + i % 10;

	rc = start_server();
	assert("server started", rc == 0, "rc was %d\n", rc);
	if (rc != 0)
		goto exit;
	server.session_present = !cleansession;
	snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%d", server.port);

	rc = MQTTAsync_create(&c, uri, "issue1592", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;
	MQTTAsync_setCallbacks(c, NULL, connectionLost, messageArrived, NULL);

	opts.keepAliveInterval = 20;
	opts.cleansession = cleansession;
	opts.onSuccess = onConnect;
	opts.onFailure = onConnectFailure;
	rc = MQTTAsync_connect(c, &opts);
	assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	assert("connected", wait_for(&connected, 5000), "connected was %d\n", connected);
	if (!connected)
		goto destroy;

	/* make the client's next write fail, without it noticing beforehand */
	fd = find_client_socket();
	assert("client socket found", fd != -1, "fd was %d\n", fd);
	if (fd == -1)
		goto destroy;
	shutdown(fd, SHUT_WR);
	MyLog(LOGA_DEBUG, "Shut down writes on client socket %d", fd);

	pubmsg.payload = payload;
	pubmsg.payloadlen = PAYLOAD_LEN;
	pubmsg.qos = 1;
	ropts.onSuccess = onPublish;
	ropts.onFailure = onPublishFailure;
	rc = MQTTAsync_sendMessage(c, TOPIC, &pubmsg, &ropts);
	assert("good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	assert("publish failed", wait_for(&publish_failed, 5000), "publish_failed was %d\n", publish_failed);
	wait_for(&connection_lost, 2000);
	MyLog(LOGA_INFO, "Publish failed %d, rc %d, connection lost %d", publish_failed, publish_failure_code, connection_lost);

	/* reuse and overwrite any memory freed by the failure */
	scribble_heap();

	/* reconnect, with the same session, so that the message is resent */
	connected = 0;
	rc = MQTTAsync_connect(c, &opts);
	assert("good rc from reconnect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	assert("reconnected", wait_for(&connected, 5000), "connected was %d\n", connected);
	MySleep(1000); /* allow time for any resend */

	MyLog(LOGA_INFO, "Server: %d connections, %d PUBLISH packets received, %d correct",
		server.connections, server.publishes, server.good_publishes);
	if (cleansession)
		assert("message not resent with clean session", server.publishes == 0,
			"%d PUBLISH packets received\n", server.publishes);
	else
		assert("message resent once on reconnect", server.publishes == 1,
			"%d PUBLISH packets received\n", server.publishes);
	assert("resent topic and payload not corrupted", server.good_publishes == server.publishes,
		"%d of %d PUBLISH packets were corrupted\n", server.publishes - server.good_publishes, server.publishes);

	MQTTAsync_disconnect(c, NULL);
	MySleep(100);
destroy:
	MQTTAsync_destroy(&c);
	assert("no memory freed twice", heap_errors == 0, "%d heap errors\n", heap_errors);
	MQTTAsync_setTraceCallback(NULL);
	free_scribbles();
	stop_server();
exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
#endif
	return failures;
}

int test1(struct Options options)
{
	return run_test("test1592 - QoS 1 message not corrupted after a write error, cleansession 0", 0);
}

int test2(struct Options options)
{
	return run_test("test1592 - no double free after a write error, cleansession 1", 1);
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
