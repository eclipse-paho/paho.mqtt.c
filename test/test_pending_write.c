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
 * Test that an incomplete write is continued as soon as the socket is writeable.
 *
 * When a packet can't be written all at once, the rest is written when the
 * socket becomes writeable again. In the poll() build, the blocking poll in
 * Socket_getReadySocket didn't wait for sockets with an incomplete write to
 * become writeable, so the write was only continued once each poll timeout
 * (1 second), one socket send buffer at a time. A large message could take
 * minutes to send. The select() build waits for them. In both builds, a poll
 * or select already in progress when the write was found to be incomplete
 * wasn't woken up, delaying the rest of the write by up to the timeout.
 *
 * The test runs its own minimal MQTT server in a thread, which stops reading
 * after the CONNACK. The client's socket send buffer is made small, and a
 * large message is published, so the write is incomplete. Then the server
 * reads again, and checks how long the rest of the PUBLISH takes to arrive.
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
	printf("  --test_no <n>  1: QoS 0 message; 2: QoS 1 message (default all)\n");
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

#define TOPIC "pending/write/publish"
#define PAYLOAD_LEN (1024 * 1024)
#define SOCKET_BUFFER_SIZE 16384 /* client send and server receive buffer size */
#define MAX_WRITE_SECONDS 0.5    /* without the fix, about PAYLOAD_LEN / SOCKET_BUFFER_SIZE seconds */

char* payload = NULL;

/*
 * A minimal MQTT 3.1.1 server, enough for this test: it accepts one connection,
 * answers CONNECT, PUBLISH and PINGREQ, and records the PUBLISH packets it
 * receives. It stops reading after the CONNACK until told to resume.
 */

struct
{
	int listener;
	int port;
	int conn_fd;
	int publishes;              /* number of PUBLISH packets received */
	int good_publishes;         /* number with the expected topic and payload */
	volatile int stalled;       /* the server has stopped reading */
	volatile int resume;        /* set by the test to let the server continue */
	volatile int stop;
	struct timeval resumed;     /* when the server started reading again */
	struct timeval received;    /* when the PUBLISH was received */
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
	size_t multiplier = 1;
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
	if ((*body = malloc(len + 1)) == NULL)
		return -1;
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
	size_t pos = 2 + topiclen;
	int msgid = 0;
	int good = 0;

	gettimeofday(&server.received, NULL);
	if (qos > 0)
	{
		msgid = ((unsigned char)body[pos] << 8) + (unsigned char)body[pos + 1];
		pos += 2;
	}
	good = topiclen == strlen(TOPIC) && memcmp(&body[2], TOPIC, topiclen) == 0 &&
		bodylen - pos == PAYLOAD_LEN && memcmp(&body[pos], payload, PAYLOAD_LEN) == 0;
	MyLog(LOGA_INFO, "Server: PUBLISH received, qos %d msgid %d, payload length %d, %s",
		qos, msgid, (int)(bodylen - pos), good ? "topic and payload correct" : "topic or payload CORRUPTED");
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
	int fd = accept(server.listener, NULL, NULL);
	int header = 0;
	char* body = NULL;
	size_t bodylen = 0;

	if (fd < 0)
		return NULL;
	server.conn_fd = fd;
	while (!server.stop && (header = read_packet(fd, &body, &bodylen)) != -1)
	{
		switch (header >> 4)
		{
		case 1: /* CONNECT */
		{
			char connack[4] = {0x20, 0x02, 0x00, 0x00};
			int count = 0;

			send(fd, connack, sizeof(connack), 0);
			/* stop reading until the test says to continue */
			server.stalled = 1;
			while (!server.resume && !server.stop && ++count < 1000)
				MySleep(10);
			gettimeofday(&server.resumed, NULL);
			server.stalled = 0;
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
	return NULL;
}

int start_server(void)
{
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	pthread_t thread;
	int bufsize = SOCKET_BUFFER_SIZE;

	memset(&server, 0, sizeof(server));
	server.conn_fd = -1;
	server.listener = socket(AF_INET, SOCK_STREAM, 0);
	if (server.listener == -1)
		return -1;
	/* a small receive buffer, inherited by the accepted socket, so the client's write is
	 * incomplete while the server isn't reading */
	setsockopt(server.listener, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
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
	server.stop = 1;
	shutdown(server.listener, SHUT_RDWR);
	close(server.listener);
	if (server.conn_fd != -1)
	{
		shutdown(server.conn_fd, SHUT_RDWR);
		close(server.conn_fd);
	}
	MySleep(100); /* let the server thread finish */
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

#endif /* !_WINDOWS */


volatile int connected = 0;
volatile int partial_writes = 0;

/* the library logs an incomplete write like this */
void traceCallback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
	if (strstr(message, "Partial write"))
	{
		MyLog(LOGA_DEBUG, "Library: %s", message);
		partial_writes++;
	}
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

int run_test(char* testname, int qos)
{
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
#else
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int bufsize = SOCKET_BUFFER_SIZE;
	char uri[64];
	double seconds = 0;
	int rc = 0;
	int fd = -1;
	int i;

	MyLog(LOGA_INFO, "Starting %s", testname);
	connected = partial_writes = 0;
	MQTTAsync_setTraceCallback(traceCallback);
	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_MINIMUM);

	if (payload == NULL && (payload = malloc(PAYLOAD_LEN)) == NULL)
		goto exit;
	for (i = 0; i < PAYLOAD_LEN; ++i)
		payload[i] = 'a' + i % 26;

	rc = start_server();
	assert("server started", rc == 0, "rc was %d\n", rc);
	if (rc != 0)
		goto exit;
	snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%d", server.port);

	rc = MQTTAsync_create(&c, uri, "pending_write", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;
	MQTTAsync_setCallbacks(c, NULL, NULL, messageArrived, NULL);

	opts.keepAliveInterval = 60;
	opts.cleansession = 1;
	opts.onSuccess = onConnect;
	opts.onFailure = onConnectFailure;
	rc = MQTTAsync_connect(c, &opts);
	assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	assert("connected", wait_for(&connected, 5000), "connected was %d\n", connected);
	if (!connected)
		goto destroy;
	assert("server stopped reading", wait_for(&server.stalled, 5000), "stalled was %d\n", server.stalled);

	/* a small send buffer, so the write is incomplete, and the rest is written a little at a time */
	fd = find_client_socket();
	assert("client socket found", fd != -1, "fd was %d\n", fd);
	if (fd == -1)
		goto destroy;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

	pubmsg.payload = payload;
	pubmsg.payloadlen = PAYLOAD_LEN;
	pubmsg.qos = qos;
	rc = MQTTAsync_sendMessage(c, TOPIC, &pubmsg, NULL);
	assert("good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	assert("write of PUBLISH incomplete", wait_for(&partial_writes, 5000),
		"partial_writes was %d\n", partial_writes);

	server.resume = 1;
	for (i = 0; server.publishes == 0 && i < 1000; ++i)
		MySleep(10);

	assert("PUBLISH received", server.publishes == 1, "%d PUBLISH packets received\n", server.publishes);
	assert("topic and payload correct", server.good_publishes == server.publishes,
		"%d of %d PUBLISH packets were corrupted\n", server.publishes - server.good_publishes, server.publishes);
	if (server.publishes > 0)
	{
		seconds = (server.received.tv_sec - server.resumed.tv_sec) +
			(server.received.tv_usec - server.resumed.tv_usec) / 1000000.0;
		MyLog(LOGA_INFO, "Rest of the PUBLISH received %.3f seconds after the server started reading", seconds);
		assert("incomplete write continued promptly", seconds < MAX_WRITE_SECONDS,
			"took %.3f seconds\n", seconds);
	}

	MQTTAsync_disconnect(c, NULL);
	MySleep(100);
destroy:
	MQTTAsync_destroy(&c);
	stop_server();
exit:
	MQTTAsync_setTraceCallback(NULL);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
#endif
	return failures;
}

int test1(struct Options options)
{
	return run_test("pending write - incomplete write of QoS 0 PUBLISH continued promptly", 0);
}

int test2(struct Options options)
{
	return run_test("pending write - incomplete write of QoS 1 PUBLISH continued promptly", 1);
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
