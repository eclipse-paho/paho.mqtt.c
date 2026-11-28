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
 * Test that the client reads the extended payload lengths of WebSocket frames.
 * A frame with a payload of 126 to 65535 bytes has a 16 bit length after the
 * first two bytes of the frame, and a longer one has a 64 bit length. These
 * lengths aren't aligned, but they used to be read through uint16_t and uint64_t
 * pointers, which is undefined behaviour, and fails on platforms which require
 * aligned access. Run with -fsanitize=alignment to detect that on platforms which
 * don't.
 * A WebSocket server in a thread of this test accepts the client's connection,
 * completes the WebSocket handshake, replies to CONNECT with CONNACK, and sends a
 * PUBLISH in a frame with an extended payload length. So no broker is needed.
 * Test 1: a 16 bit payload length
 * Test 2: a 64 bit payload length
 * The test uses POSIX sockets and threads, so it isn't built on Windows, and
 * OpenSSL for the handshake, so it's only built with PAHO_WITH_SSL.
 */

#include "MQTTClient.h"
#include <openssl/evp.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0 /* macOS: SIGPIPE is ignored in main instead */
#endif

void usage(void)
{
	printf("Options:\n");
	printf("  --test_no <n>  1: 16 bit payload length; 2: 64 bit payload length (default all)\n");
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
void MyLog(int LOGA_level, char* format, ...)
{
	static char msg_buf[256];
	va_list args;
	struct timeval ts;
	struct tm *timeinfo;

	if (LOGA_level == LOGA_DEBUG && options.verbose == 0)
	  return;

	gettimeofday(&ts, NULL);
	timeinfo = localtime(&ts.tv_sec);
	strftime(msg_buf, 80, "%Y%m%d %H%M%S", timeinfo);
	sprintf(&msg_buf[strlen(msg_buf)], ".%.3lu ", ts.tv_usec / 1000L);

	va_start(args, format);
	vsnprintf(&msg_buf[strlen(msg_buf)], sizeof(msg_buf) - strlen(msg_buf), format, args);
	va_end(args);

	printf("%s\n", msg_buf);
	fflush(stdout);
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


/* The WebSocket server */

struct server
{
	int listen_fd;
	int port;
	size_t payload_len; /* the length of the payload of the PUBLISH */
	const char* error; /* set if the server failed */
	pthread_t thread;
};

static const char* topic = "websocket/frames";

static int read_fully(int fd, unsigned char* buf, size_t len)
{
	size_t done = 0;

	while (done < len)
	{
		ssize_t rc = recv(fd, buf + done, len - done, 0);

		if (rc <= 0)
			return -1;
		done += (size_t)rc;
	}
	return 0;
}

static int send_fully(int fd, const unsigned char* buf, size_t len)
{
	size_t done = 0;

	while (done < len)
	{
		ssize_t rc = send(fd, buf + done, len - done, MSG_NOSIGNAL);

		if (rc <= 0)
			return -1;
		done += (size_t)rc;
	}
	return 0;
}

/* read the HTTP upgrade request, and reply with the Sec-WebSocket-Accept for its key */
static const char* handshake(int fd)
{
	static const char* guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	char request[4096];
	char response[256];
	char key[100];
	char key_guid[200];
	char accept[4 * ((EVP_MAX_MD_SIZE + 2) / 3) + 1];
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_len = 0;
	size_t len = 0;
	const char* p;
	const char* eol;

	while (len < sizeof(request) - 1 && (len < 4 || memcmp(&request[len - 4], "\r\n\r\n", 4) != 0))
	{
		ssize_t rc = recv(fd, &request[len], 1, 0);

		if (rc <= 0)
			return "failed to read the upgrade request";
		++len;
	}
	request[len] = '\0';

	for (p = request; *p && strncasecmp(p, "Sec-WebSocket-Key:", 18) != 0; ++p)
		;
	if (*p == '\0')
		return "no Sec-WebSocket-Key in the upgrade request";
	p += 18;
	while (*p == ' ')
		++p;
	if ((eol = strstr(p, "\r\n")) == NULL || (size_t)(eol - p) >= sizeof(key))
		return "bad Sec-WebSocket-Key";
	memcpy(key, p, eol - p);
	key[eol - p] = '\0';

	/* the accept value is the Base64 encoded SHA-1 hash of the key and the GUID */
	snprintf(key_guid, sizeof(key_guid), "%s%s", key, guid);
	if (EVP_Digest(key_guid, strlen(key_guid), digest, &digest_len, EVP_sha1(), NULL) != 1)
		return "failed to calculate the SHA-1 hash";
	EVP_EncodeBlock((unsigned char*)accept, digest, (int)digest_len);

	snprintf(response, sizeof(response), "HTTP/1.1 101 Switching Protocols\r\n"
			"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
	if (send_fully(fd, (const unsigned char*)response, strlen(response)) != 0)
		return "failed to send the upgrade response";
	return NULL;
}

/* read a frame from the client, which is masked, and ignore its payload */
static const char* read_frame(int fd)
{
	unsigned char header[2];
	unsigned char ext[8];
	unsigned char mask[4];
	unsigned char buf[256];
	uint64_t len;
	int i;

	if (read_fully(fd, header, sizeof(header)) != 0)
		return "failed to read a frame header";
	len = header[1] & 0x7F;
	if (len == 126 || len == 127)
	{
		size_t ext_len = (len == 126) ? 2 : 8;

		if (read_fully(fd, ext, ext_len) != 0)
			return "failed to read a frame length";
		for (len = 0, i = 0; i < (int)ext_len; ++i)
			len = (len << 8) | ext[i];
	}
	if ((header[1] & 0x80) == 0)
		return "frame from the client isn't masked";
	if (read_fully(fd, mask, sizeof(mask)) != 0)
		return "failed to read a frame mask";
	while (len > 0)
	{
		size_t chunk = (len > sizeof(buf)) ? sizeof(buf) : (size_t)len;

		if (read_fully(fd, buf, chunk) != 0)
			return "failed to read a frame payload";
		len -= chunk;
	}
	return NULL;
}

/* send a binary frame, with the shortest payload length encoding, as RFC 6455 requires */
static const char* send_frame(int fd, const unsigned char* payload, size_t len)
{
	unsigned char header[10];
	size_t header_len = 2;
	int i;

	header[0] = 0x82; /* final frame, binary */
	if (len < 126)
		header[1] = (unsigned char)len;
	else if (len <= 0xFFFF)
	{
		header[1] = 126;
		header[2] = (unsigned char)(len >> 8);
		header[3] = (unsigned char)len;
		header_len = 4;
	}
	else
	{
		header[1] = 127;
		for (i = 0; i < 8; ++i)
			header[2 + i] = (unsigned char)((uint64_t)len >> (56 - i * 8));
		header_len = 10;
	}
	if (send_fully(fd, header, header_len) != 0 || send_fully(fd, payload, len) != 0)
		return "failed to send a frame";
	return NULL;
}

static unsigned char payload_byte(size_t i)
{
	return (unsigned char)('a' + i % 26);
}

/* send a QoS 0 PUBLISH with a payload of the given length */
static const char* send_publish(int fd, size_t payload_len)
{
	size_t topic_len = strlen(topic);
	size_t remaining = 2 + topic_len + payload_len;
	size_t len = 0;
	size_t i;
	unsigned char* packet = malloc(1 + 4 + remaining);
	const char* error;

	if (packet == NULL)
		return "failed to allocate the PUBLISH";
	packet[len++] = 0x30; /* PUBLISH, QoS 0 */
	do
	{
		unsigned char byte = remaining % 128;

		remaining /= 128;
		if (remaining > 0)
			byte |= 0x80;
		packet[len++] = byte;
	} while (remaining > 0);
	packet[len++] = (unsigned char)(topic_len >> 8);
	packet[len++] = (unsigned char)topic_len;
	memcpy(&packet[len], topic, topic_len);
	len += topic_len;
	for (i = 0; i < payload_len; ++i)
		packet[len++] = payload_byte(i);
	error = send_frame(fd, packet, len);
	free(packet);
	return error;
}

static void* server_thread(void* arg)
{
	static const unsigned char connack[] = {0x20, 0x02, 0x00, 0x00};
	struct server* s = arg;
	struct pollfd pfd;
	struct timeval timeout = {10, 0};
	unsigned char buf[256];
	int fd;

	pfd.fd = s->listen_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 10000) != 1 || (fd = accept(s->listen_fd, NULL, NULL)) < 0)
	{
		s->error = "the client didn't connect";
		return NULL;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	if ((s->error = handshake(fd)) == NULL &&
			(s->error = read_frame(fd)) == NULL && /* CONNECT */
			(s->error = send_frame(fd, connack, sizeof(connack))) == NULL)
		s->error = send_publish(fd, s->payload_len);

	/* wait for the client to disconnect */
	while (s->error == NULL && recv(fd, buf, sizeof(buf), 0) > 0)
		;
	close(fd);
	return NULL;
}

static int start_server(struct server* s, size_t payload_len)
{
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	int on = 1;

	memset(s, '\0', sizeof(*s));
	s->payload_len = payload_len;
	if ((s->listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1;
	setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&addr, '\0', sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0; /* any free port */
	if (bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
			listen(s->listen_fd, 1) != 0 ||
			getsockname(s->listen_fd, (struct sockaddr*)&addr, &addr_len) != 0 ||
			pthread_create(&s->thread, NULL, server_thread, s) != 0)
	{
		close(s->listen_fd);
		return -1;
	}
	s->port = ntohs(addr.sin_port);
	return 0;
}

static void stop_server(const struct server* s)
{
	pthread_join(s->thread, NULL);
	close(s->listen_fd);
}


static int run_test(const char* testname, size_t payload_len)
{
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_message* msg = NULL;
	MQTTClient c = NULL;
	struct server server;
	char uri[64];
	char* topicName = NULL;
	int topicLen = 0;
	int rc;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;

	rc = start_server(&server, payload_len);
	assert("server started", rc == 0, "rc was %d\n", rc);
	if (rc != 0)
		goto exit;
	snprintf(uri, sizeof(uri), "ws://127.0.0.1:%d", server.port);
	MyLog(LOGA_DEBUG, "Connecting to %s", uri);

	rc = MQTTClient_create(&c, uri, "websocket frames", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto stop;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.connectTimeout = 10;
	rc = MQTTClient_connect(c, &opts);
	assert("good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto destroy;

	rc = MQTTClient_receive(c, &topicName, &topicLen, &msg, 10000L);
	assert("message received", rc == MQTTCLIENT_SUCCESS && msg != NULL, "rc was %d, message %p\n",
		rc, (void*)msg);
	if (msg)
	{
		size_t i;
		int good_payload = 1;

		assert("topic", strcmp(topicName, topic) == 0, "topic was %s\n", topicName);
		assert("payload length", msg->payloadlen == (int)payload_len, "payload length was %d, expected %d\n",
			msg->payloadlen, (int)payload_len);
		for (i = 0; good_payload && i < payload_len && i < (size_t)msg->payloadlen; ++i)
			good_payload = ((unsigned char*)msg->payload)[i] == payload_byte(i);
		assert("payload", good_payload, "payload differs at byte %d\n", (int)i - 1);
		MQTTClient_freeMessage(&msg);
		MQTTClient_free(topicName);
	}
	MQTTClient_disconnect(c, 0);

destroy:
	MQTTClient_destroy(&c);
stop:
	stop_server(&server);
	assert("server succeeded", server.error == NULL, "server error: %s\n", server.error);
exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}

int test1(struct Options options)
{
	/* a payload of 126 to 65535 bytes has a 16 bit length */
	return run_test("test_websocket_frames-1 - 16 bit payload length", 300);
}

int test2(struct Options options)
{
	/* a longer payload has a 64 bit length */
	return run_test("test_websocket_frames-2 - 64 bit payload length", 70000);
}


int main(int argc, char** argv)
{
	int rc = 0;
	int (*test_funcs[])(struct Options) = {NULL, test1, test2};
	int i;

	getopts(argc, argv);
	signal(SIGPIPE, SIG_IGN);

	if (options.test_no == 0)
	{
		for (i = 1; i < (int)(sizeof(test_funcs) / sizeof(test_funcs[0])); ++i)
			rc += test_funcs[i](options);
	}
	else if (options.test_no < (int)(sizeof(test_funcs) / sizeof(test_funcs[0])))
		rc = test_funcs[options.test_no](options);
	else
		usage();

	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	return rc;
}
