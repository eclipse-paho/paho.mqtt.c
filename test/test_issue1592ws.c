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
 * Test for issue 1592 over websockets: PUBLISH left masked after an incomplete write.
 *
 * A websocket client masks the data it sends. WebSocket_putdatas masks the
 * packet buffers in place, including the topic and payload of the stored
 * message kept for retry, and unmasks them after the write. But if the write
 * is incomplete (TCPSOCKET_INTERRUPTED), the buffers are left masked, as the
 * rest is written later from the same memory, and nothing unmasks them when
 * that write completes or is abandoned. So the stored message is left masked:
 * when it is resent, after a reconnect, the topic and payload are garbage.
 * The resend also writes the masked data to persistence, so the message is
 * still corrupted after the application restarts.
 *
 * The test runs its own minimal websocket MQTT server in a thread. To make a
 * write incomplete, the server stops reading, and a large QoS 1 message is
 * published, which fills the socket buffers. The library logs a partial write,
 * and the test waits for that before letting the server continue.
 *
 * Test 1: the server reads the rest of the PUBLISH and checks it, then closes
 * the connection without a PUBACK. The client reconnects and resends.
 * Test 2: the server resets the connection without reading the PUBLISH, so
 * the write is never completed. The client reconnects and resends.
 * Test 3: as test 2, with file persistence. The server closes the connection
 * after the resend without a PUBACK, and a new client is created which restores
 * the message from persistence and resends it again.
 *
 * In each case the server checks that the topic and payload of every PUBLISH
 * it receives are correct. No broker is needed.
 */

#include "MQTTAsync.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#if !defined(_WINDOWS)
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <ftw.h>
#else
#include <windows.h>
#define snprintf _snprintf
#endif

void usage(void)
{
	printf("Options:\n");
	printf("  --test_no <n>  1: interrupted write completed; 2: interrupted write abandoned;\n");
	printf("                 3: interrupted write abandoned, with persistence (default all)\n");
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

#define TOPIC "issue1592/websocket/masked/publish"
#define PAYLOAD_LEN (256 * 1024) /* big enough to fill the socket buffers */
#define SOCKET_BUFFER_SIZE 16384    /* client send and server receive buffer size while the write is held up */
#define PERSISTENCE_DIR "issue1592ws_persistence"

char* payload = NULL;


/* SHA-1 and base64, for the Sec-WebSocket-Accept header of the websocket handshake */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

void sha1(const unsigned char* data, size_t len, unsigned char out[20])
{
	uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	size_t total = ((len + 8) / 64 + 1) * 64;
	unsigned char* msg = calloc(total, 1);
	uint64_t bits = (uint64_t)len * 8;
	size_t chunk;
	int i;

	memcpy(msg, data, len);
	msg[len] = 0x80;
	for (i = 0; i < 8; ++i)
		msg[total - 1 - i] = (unsigned char)(bits >> (8 * i));
	for (chunk = 0; chunk < total; chunk += 64)
	{
		uint32_t w[80];
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

		for (i = 0; i < 16; ++i)
			w[i] = ((uint32_t)msg[chunk + 4*i] << 24) | ((uint32_t)msg[chunk + 4*i + 1] << 16) |
				((uint32_t)msg[chunk + 4*i + 2] << 8) | (uint32_t)msg[chunk + 4*i + 3];
		for (i = 16; i < 80; ++i)
			w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
		for (i = 0; i < 80; ++i)
		{
			uint32_t f, k, temp;

			if (i < 20)
			{
				f = (b & c) | (~b & d);
				k = 0x5A827999;
			}
			else if (i < 40)
			{
				f = b ^ c ^ d;
				k = 0x6ED9EBA1;
			}
			else if (i < 60)
			{
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDC;
			}
			else
			{
				f = b ^ c ^ d;
				k = 0xCA62C1D6;
			}
			temp = ROL(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = ROL(b, 30);
			b = a;
			a = temp;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}
	for (i = 0; i < 20; ++i)
		out[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
	free(msg);
}

void base64(const unsigned char* in, size_t len, char* out)
{
	static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i;

	for (i = 0; i < len; i += 3)
	{
		uint32_t v = (uint32_t)in[i] << 16;

		if (i + 1 < len)
			v |= (uint32_t)in[i + 1] << 8;
		if (i + 2 < len)
			v |= in[i + 2];
		*out++ = chars[(v >> 18) & 0x3F];
		*out++ = chars[(v >> 12) & 0x3F];
		*out++ = (i + 1 < len) ? chars[(v >> 6) & 0x3F] : '=';
		*out++ = (i + 2 < len) ? chars[v & 0x3F] : '=';
	}
	*out = '\0';
}


/*
 * A minimal websocket MQTT 3.1.1 server, enough for this test: it accepts
 * connections one after the other, answers CONNECT, PUBLISH and PINGREQ, and
 * records the PUBLISH packets it receives. What it does on each connection
 * is set by the test.
 */

enum server_modes
{
	NORMAL,             /* acknowledge PUBLISH */
	STALL_NO_PUBACK,    /* stop reading after CONNECT until told to resume, then close after PUBLISH without PUBACK */
	STALL_RESET,        /* stop reading after CONNECT until told to resume, then reset the connection */
	NO_PUBACK,          /* close after PUBLISH without PUBACK */
};

#define MAX_CONNECTIONS 8

struct
{
	int listener;
	int port;
	int connections;            /* number of connections accepted */
	int session_present;        /* value for the CONNACK session present flag */
	int publishes;              /* number of PUBLISH packets received */
	int good_publishes;         /* number with the expected topic and payload */
	volatile int stalled;       /* the server has stopped reading */
	volatile int resume;        /* set by the test to let the server continue */
	int stop;
	enum server_modes modes[MAX_CONNECTIONS];
	int conn_fds[MAX_CONNECTIONS];
} server;

struct ws_conn
{
	int fd;
	unsigned char* frame;    /* unmasked payload of the current websocket frame */
	size_t framelen;
	size_t pos;              /* how much of the frame has been read */
};

int read_fully(int fd, void* buf, size_t len)
{
	size_t got = 0;

	while (got < len)
	{
		ssize_t rc = recv(fd, (char*)buf + got, len - got, 0);
		if (rc <= 0)
			return -1;
		got += rc;
	}
	return 0;
}

/* read the next websocket data frame, unmasking it */
int ws_read_frame(struct ws_conn* c)
{
	unsigned char hdr[2];
	unsigned char mask[4] = {0, 0, 0, 0};
	uint64_t len = 0;
	int opcode;
	size_t i;

	free(c->frame);
	c->frame = NULL;
	c->framelen = c->pos = 0;
	do
	{
		if (read_fully(c->fd, hdr, 2) != 0)
			return -1;
		opcode = hdr[0] & 0x0F;
		len = hdr[1] & 0x7F;
		if (len == 126)
		{
			unsigned char ext[2];

			if (read_fully(c->fd, ext, 2) != 0)
				return -1;
			len = ((uint64_t)ext[0] << 8) | ext[1];
		}
		else if (len == 127)
		{
			unsigned char ext[8];

			if (read_fully(c->fd, ext, 8) != 0)
				return -1;
			len = 0;
			for (i = 0; i < 8; ++i)
				len = (len << 8) | ext[i];
		}
		if ((hdr[1] & 0x80) && read_fully(c->fd, mask, 4) != 0)
			return -1;
		if ((c->frame = malloc(len ? len : 1)) == NULL)
			return -1;
		if (len > 0 && read_fully(c->fd, c->frame, len) != 0)
			return -1;
		for (i = 0; i < len; ++i)
			c->frame[i] ^= mask[i % 4];
		c->framelen = len;
		if (opcode == 8) /* close */
			return -1;
		if (opcode != 0 && opcode != 2) /* not data: ignore */
		{
			free(c->frame);
			c->frame = NULL;
			c->framelen = 0;
		}
	} while (c->frame == NULL);
	return 0;
}

/* read from the stream of websocket data */
int ws_read(struct ws_conn* c, void* buf, size_t len)
{
	size_t got = 0;

	while (got < len)
	{
		size_t n;

		if (c->pos == c->framelen && ws_read_frame(c) != 0)
			return -1;
		n = c->framelen - c->pos;
		if (n > len - got)
			n = len - got;
		memcpy((char*)buf + got, c->frame + c->pos, n);
		c->pos += n;
		got += n;
	}
	return 0;
}

/* send a small MQTT packet in an unmasked binary frame */
void ws_send(int fd, const char* data, size_t len)
{
	char frame[2 + 125];

	frame[0] = (char)0x82;
	frame[1] = (char)len;
	memcpy(&frame[2], data, len);
	send(fd, frame, 2 + len, 0);
}

int ws_handshake(int fd)
{
	static const char* guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	char request[4096];
	char keyguid[128];
	char accept[64];
	char response[512];
	unsigned char hash[20];
	size_t len = 0;
	char* key = NULL;
	char* eol = NULL;
	char* p;

	while (len < sizeof(request) - 1)
	{
		if (read_fully(fd, &request[len], 1) != 0)
			return -1;
		request[++len] = '\0';
		if (len >= 4 && strcmp(&request[len - 4], "\r\n\r\n") == 0)
			break;
	}
	for (p = request; *p; ++p) /* find the key header, case-insensitively */
	{
		const char* name = "sec-websocket-key:";
		size_t i;

		for (i = 0; name[i] && tolower((unsigned char)p[i]) == name[i]; ++i)
			;
		if (name[i] == '\0')
		{
			key = p + i;
			break;
		}
	}
	if (key == NULL)
		return -1;
	while (*key == ' ')
		++key;
	if ((eol = strstr(key, "\r\n")) == NULL)
		return -1;
	*eol = '\0';
	snprintf(keyguid, sizeof(keyguid), "%s%s", key, guid);
	sha1((unsigned char*)keyguid, strlen(keyguid), hash);
	base64(hash, sizeof(hash), accept);
	snprintf(response, sizeof(response),
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n"
		"Sec-WebSocket-Protocol: mqtt\r\n"
		"\r\n", accept);
	send(fd, response, strlen(response), 0); /* the client expects this in one read */
	return 0;
}

/* read an MQTT packet: returns the first header byte, or -1 at EOF */
int read_packet(struct ws_conn* c, char** body, size_t* bodylen)
{
	unsigned char header = 0;
	size_t len = 0;
	size_t multiplier = 1;
	unsigned char b = 0;

	if (ws_read(c, &header, 1) != 0)
		return -1;
	do
	{
		if (ws_read(c, &b, 1) != 0)
			return -1;
		len += (b & 127) * multiplier;
		multiplier *= 128;
	} while (b & 128);
	if ((*body = malloc(len + 1)) == NULL)
		return -1;
	if (len > 0 && ws_read(c, *body, len) != 0)
	{
		free(*body);
		return -1;
	}
	*bodylen = len;
	return header;
}

/* returns 1 if the connection is to be closed */
int handle_publish(int fd, enum server_modes mode, unsigned char header, char* body, size_t bodylen)
{
	int qos = (header >> 1) & 0x03;
	size_t topiclen = ((unsigned char)body[0] << 8) + (unsigned char)body[1];
	char* topic = &body[2];
	size_t pos = 2 + topiclen;
	int msgid = 0;
	int good = 0;

	if (pos > bodylen)
		pos = bodylen;
	if (qos > 0 && pos + 2 <= bodylen)
	{
		msgid = ((unsigned char)body[pos] << 8) + (unsigned char)body[pos + 1];
		pos += 2;
	}
	good = topiclen == strlen(TOPIC) && pos <= bodylen && memcmp(topic, TOPIC, topiclen) == 0 &&
		bodylen - pos == PAYLOAD_LEN && memcmp(&body[pos], payload, PAYLOAD_LEN) == 0;
	MyLog(LOGA_INFO, "Server: PUBLISH received on connection %d, dup %d qos %d msgid %d, topic length %d, payload length %d, %s",
		server.connections, (header >> 3) & 0x01, qos, msgid, (int)topiclen, (int)(bodylen - pos),
		good ? "topic and payload correct" : "topic or payload CORRUPTED");
	server.publishes++;
	if (good)
		server.good_publishes++;
	if (mode == STALL_NO_PUBACK || mode == NO_PUBACK)
		return 1;
	if (qos > 0)
	{
		char ack[4] = {(qos == 1) ? 0x40 : 0x50, 0x02, (char)(msgid >> 8), (char)(msgid & 0xFF)};
		ws_send(fd, ack, sizeof(ack));
	}
	return 0;
}

/* stop reading until the test says to continue */
void stall(void)
{
	int count = 0;

	server.stalled = 1;
	while (!server.resume && !server.stop && ++count < 1000)
		MySleep(10);
	server.stalled = 0;
}

void* server_thread(void* arg)
{
	while (!server.stop && server.connections < MAX_CONNECTIONS)
	{
		int fd = accept(server.listener, NULL, NULL);
		enum server_modes mode;
		struct ws_conn conn;
		int header = 0;
		char* body = NULL;
		size_t bodylen = 0;
		int finished = 0;

		if (fd < 0)
			break;
		mode = server.modes[server.connections];
		server.conn_fds[server.connections++] = fd;
		MyLog(LOGA_DEBUG, "Server: connection %d accepted, mode %d", server.connections, mode);
		memset(&conn, 0, sizeof(conn));
		conn.fd = fd;
		if (ws_handshake(fd) != 0)
		{
			MyLog(LOGA_INFO, "Server: websocket handshake failed");
			finished = 1;
		}
		while (!finished && (header = read_packet(&conn, &body, &bodylen)) != -1)
		{
			switch (header >> 4)
			{
			case 1: /* CONNECT */
			{
				char connack[4] = {0x20, 0x02, (char)server.session_present, 0x00};
				ws_send(fd, connack, sizeof(connack));
				if (mode == STALL_NO_PUBACK || mode == STALL_RESET)
				{
					stall();
					if (mode == STALL_RESET)
					{
						struct linger reset = {1, 0};

						setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
						MyLog(LOGA_DEBUG, "Server: resetting connection %d", server.connections);
						finished = 1;
					}
				}
				break;
			}
			case 3: /* PUBLISH */
				finished = handle_publish(fd, mode, (unsigned char)header, body, bodylen);
				break;
			case 12: /* PINGREQ */
			{
				char pingresp[2] = {(char)0xD0, 0x00};
				ws_send(fd, pingresp, sizeof(pingresp));
				break;
			}
			default:
				break;
			}
			free(body);
			body = NULL;
		}
		free(conn.frame);
		if (finished)
		{
			MyLog(LOGA_DEBUG, "Server: closing connection %d", server.connections);
			close(fd);
			server.conn_fds[server.connections - 1] = -1;
		}
		else
			MyLog(LOGA_DEBUG, "Server: connection %d ended", server.connections);
	}
	return NULL;
}

int start_server(void)
{
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	pthread_t thread;
	int bufsize = SOCKET_BUFFER_SIZE;
	int i;

	memset(&server, 0, sizeof(server));
	for (i = 0; i < MAX_CONNECTIONS; ++i)
		server.conn_fds[i] = -1;
	server.listener = socket(AF_INET, SOCK_STREAM, 0);
	if (server.listener == -1)
		return -1;
	/* a small receive buffer, inherited by accepted sockets, so the client's writes are incomplete
	 * while the server isn't reading */
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
	int i;

	server.stop = 1;
	shutdown(server.listener, SHUT_RDWR);
	close(server.listener);
	for (i = 0; i < server.connections; ++i)
	{
		if (server.conn_fds[i] != -1)
			close(server.conn_fds[i]);
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

int remove_entry(const char* path, const struct stat* sb, int flag, struct FTW* ftwbuf)
{
	return remove(path);
}

void remove_persistence(void)
{
	nftw(PERSISTENCE_DIR, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

#endif /* !_WINDOWS */


volatile int connected = 0;
volatile int connect_failed = 0;
volatile int connection_lost = 0;
volatile int published = 0;
volatile int publish_failed = 0;
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
	MyLog(LOGA_DEBUG, "Publish failed, rc %d", response ? response->code : -1);
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

#if !defined(_WINDOWS)

int create_client(MQTTAsync* c, int persistence)
{
	char uri[64];
	int rc = 0;

	snprintf(uri, sizeof(uri), "ws://127.0.0.1:%d", server.port);
	if (persistence)
		rc = MQTTAsync_create(c, uri, "issue1592ws", MQTTCLIENT_PERSISTENCE_DEFAULT, PERSISTENCE_DIR);
	else
		rc = MQTTAsync_create(c, uri, "issue1592ws", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc == MQTTASYNC_SUCCESS)
		MQTTAsync_setCallbacks(*c, NULL, connectionLost, messageArrived, NULL);
	return rc;
}

/* connect with cleansession 0, so that the message is kept and resent on reconnect */
int connect_client(MQTTAsync c)
{
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	int rc = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 0;
	opts.onSuccess = onConnect;
	opts.onFailure = onConnectFailure;
	connected = connect_failed = connection_lost = 0;
	rc = MQTTAsync_connect(c, &opts);
	assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	assert("connected", wait_for(&connected, 5000), "connected was %d\n", connected);
	return connected;
}

#endif

int run_test(char* testname, enum server_modes first_mode, int persistence)
{
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping %s on Windows", testname);
#else
	MQTTAsync c = NULL;
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int bufsize = SOCKET_BUFFER_SIZE;
	int expected_publishes = 0;
	int rc = 0;
	int fd = -1;
	int i;

	MyLog(LOGA_INFO, "Starting %s", testname);
	published = publish_failed = partial_writes = 0;
	MQTTAsync_setTraceCallback(traceCallback);
	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_MINIMUM);

	if (payload == NULL && (payload = malloc(PAYLOAD_LEN)) == NULL)
		goto exit;
	for (i = 0; i < PAYLOAD_LEN; ++i)
		payload[i] = 'a' + i % 26;
	remove_persistence();

	rc = start_server();
	assert("server started", rc == 0, "rc was %d\n", rc);
	if (rc != 0)
		goto exit;
	server.session_present = 1;
	server.modes[0] = first_mode;
	server.modes[1] = persistence ? NO_PUBACK : NORMAL;
	server.modes[2] = NORMAL;

	if (create_client(&c, persistence) != MQTTASYNC_SUCCESS)
		goto exit;
	if (!connect_client(c))
		goto destroy;
	assert("server stopped reading", wait_for(&server.stalled, 5000), "stalled was %d\n", server.stalled);

	/* a small send buffer too, so the client's write is incomplete */
	if ((fd = find_client_socket()) != -1)
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

	pubmsg.payload = payload;
	pubmsg.payloadlen = PAYLOAD_LEN;
	pubmsg.qos = 1;
	ropts.onSuccess = onPublish;
	ropts.onFailure = onPublishFailure;
	rc = MQTTAsync_sendMessage(c, TOPIC, &pubmsg, &ropts);
	assert("good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	assert("write of PUBLISH incomplete", wait_for(&partial_writes, 5000),
		"partial_writes was %d\n", partial_writes);
	/* The library only continues an incomplete write once each poll timeout (1s), so
	 * enlarge the send buffer to let the rest be written in one go */
	bufsize = 4 * PAYLOAD_LEN;
	if (fd != -1)
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
	server.resume = 1;
	if (first_mode == STALL_NO_PUBACK)
		expected_publishes++; /* the first PUBLISH is completed and received */

	/* the server closes the connection, and the client reconnects with the same session,
	 * so that the message is resent */
	assert("connection lost", wait_for(&connection_lost, 10000), "connection_lost was %d\n", connection_lost);
	if (!connect_client(c))
		goto destroy;
	expected_publishes++;

	if (persistence)
	{
		/* the server closes the connection after the resend. Restart the client, so that the
		 * message is restored from persistence and resent again */
		assert("connection lost", wait_for(&connection_lost, 10000), "connection_lost was %d\n", connection_lost);
		MQTTAsync_destroy(&c);
		if (create_client(&c, persistence) != MQTTASYNC_SUCCESS)
			goto exit;
		if (!connect_client(c))
			goto destroy;
		expected_publishes++;
	}
	for (i = 0; server.publishes < expected_publishes && i < 1000; ++i)
		MySleep(10);
	MySleep(500); /* allow time for any unexpected PUBLISH */

	MyLog(LOGA_INFO, "Server: %d connections, %d PUBLISH packets received, %d correct",
		server.connections, server.publishes, server.good_publishes);
	assert("all expected PUBLISH packets received", server.publishes == expected_publishes,
		"%d PUBLISH packets received, %d expected\n", server.publishes, expected_publishes);
	assert("topic and payload not corrupted", server.good_publishes == server.publishes,
		"%d of %d PUBLISH packets were corrupted\n", server.publishes - server.good_publishes, server.publishes);

	MQTTAsync_disconnect(c, NULL);
	MySleep(100);
destroy:
	MQTTAsync_destroy(&c);
	stop_server();
exit:
	MQTTAsync_setTraceCallback(NULL);
	remove_persistence();
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
#endif
	return failures;
}

int test1(struct Options options)
{
	return run_test("test1592ws - PUBLISH not left masked after an interrupted write is completed",
		STALL_NO_PUBACK, 0);
}

int test2(struct Options options)
{
	return run_test("test1592ws - PUBLISH not left masked after an interrupted write is abandoned",
		STALL_RESET, 0);
}

int test3(struct Options options)
{
	return run_test("test1592ws - PUBLISH not persisted masked after an interrupted write",
		STALL_RESET, 1);
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
