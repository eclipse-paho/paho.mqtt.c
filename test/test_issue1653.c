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
 * Test for issue 1653: the Host and Origin headers of the WebSocket upgrade
 * request always included the port. RFC 7230 section 2.7.1 says that the port is
 * omitted if it's the default for the scheme, 80 for ws and 443 for wss, and
 * proxies which check AWS SigV4 signatures reject the request otherwise. The
 * closing bracket of an IPv6 address was also left out of the headers.
 * A server in a thread of this test captures the upgrade request, and then
 * rejects it, so the connect fails, and no broker is needed. To test
 * the default port, which can't be listened on without privileges, the server
 * acts as an HTTP proxy: the client sends it a CONNECT request for the broker,
 * and then the upgrade request.
 * Test 1: a direct connection to a port which isn't the default keeps the port
 * Test 2: through a proxy, the default port 80 is omitted
 * Test 3: through a proxy, a port which isn't the default is kept
 * Test 4: through a proxy, an IPv6 address keeps its brackets
 * wss uses the same code, with 443 as the default port, but needs a TLS server.
 * The test uses POSIX sockets and threads, so it isn't built on Windows.
 */

#include "MQTTClient.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
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
	printf("  --test_no <n>  run test number n (default all)\n");
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
	static char msg_buf[1024];
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


/* The server, which captures the upgrade request, and acts as an HTTP proxy first if asked */

#define REQUEST_SIZE 4096

struct server
{
	int listen_fd;
	int port;
	int proxy; /* whether to handle a CONNECT request before the upgrade request */
	char connect_request[REQUEST_SIZE];
	char upgrade_request[REQUEST_SIZE];
	const char* error; /* set if the server failed */
	pthread_t thread;
};

/* read an HTTP request, up to the empty line which ends its headers */
static int read_request(int fd, char* request, size_t size)
{
	size_t len = 0;

	while (len < size - 1 && (len < 4 || memcmp(&request[len - 4], "\r\n\r\n", 4) != 0))
	{
		ssize_t rc = recv(fd, &request[len], 1, 0);

		if (rc <= 0)
			return -1;
		++len;
	}
	request[len] = '\0';
	return 0;
}

static void* server_thread(void* arg)
{
	static const char* established = "HTTP/1.1 200 Connection established\r\n\r\n";
	static const char* rejected = "HTTP/1.1 400 Bad Request\r\n\r\n";
	struct server* s = arg;
	struct pollfd pfd;
	struct timeval timeout = {10, 0};
	int fd;

	pfd.fd = s->listen_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 10000) != 1 || (fd = accept(s->listen_fd, NULL, NULL)) < 0)
	{
		s->error = "the client didn't connect";
		return NULL;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	if (s->proxy)
	{
		if (read_request(fd, s->connect_request, sizeof(s->connect_request)) != 0)
			s->error = "failed to read the CONNECT request";
		else if (send(fd, established, strlen(established), MSG_NOSIGNAL) != (ssize_t)strlen(established))
			s->error = "failed to send the CONNECT response";
	}
	if (s->error == NULL && read_request(fd, s->upgrade_request, sizeof(s->upgrade_request)) != 0)
		s->error = "failed to read the upgrade request";
	/* rejecting the upgrade makes the client's connect fail straight away */
	if (s->error == NULL)
		send(fd, rejected, strlen(rejected), MSG_NOSIGNAL);
	close(fd);
	return NULL;
}

static int start_server(struct server* s, int proxy)
{
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	int on = 1;

	memset(s, '\0', sizeof(*s));
	s->proxy = proxy;
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

/* copy the value of a header in a request, or an empty string if it isn't there */
static void get_header(const char* request, const char* name, char* value, size_t size)
{
	size_t name_len = strlen(name);
	const char* line = request;

	value[0] = '\0';
	while ((line = strstr(line, "\r\n")) != NULL)
	{
		line += 2;
		if (strncasecmp(line, name, name_len) == 0 && line[name_len] == ':')
		{
			const char* start = line + name_len + 1;
			const char* end = strstr(start, "\r\n");
			size_t len;

			while (*start == ' ')
				++start;
			len = end ? (size_t)(end - start) : strlen(start);
			if (len >= size)
				len = size - 1;
			memcpy(value, start, len);
			value[len] = '\0';
			return;
		}
	}
}


/* connect to the broker, directly or through the proxy, and check the upgrade request's headers */
static int run_test(const char* testname, const char* broker, int proxy, const char* expected_host)
{
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient c = NULL;
	struct server server;
	char uri[100];
	char proxy_uri[100];
	char expected_buf[100];
	char expected_origin[120];
	char value[200];
	int rc;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;

	rc = start_server(&server, proxy);
	assert("server started", rc == 0, "rc was %d\n", rc);
	if (rc != 0)
		goto exit;
	if (broker)
		snprintf(uri, sizeof(uri), "%s", broker);
	else
		snprintf(uri, sizeof(uri), "ws://127.0.0.1:%d/mqtt", server.port);
	MyLog(LOGA_DEBUG, "Connecting to %s", uri);

	rc = MQTTClient_create(&c, uri, "issue 1653", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto stop;

	opts.connectTimeout = 2;
	/* only one attempt: with the default, the client tries MQTT 3.1 after 3.1.1 */
	opts.MQTTVersion = MQTTVERSION_3_1_1;
	if (proxy)
	{
		snprintf(proxy_uri, sizeof(proxy_uri), "http://127.0.0.1:%d", server.port);
		opts.httpProxy = proxy_uri;
	}
	/* the server rejects the upgrade request, so the connect fails */
	rc = MQTTClient_connect(c, &opts);
	assert("connect fails", rc != MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	MQTTClient_destroy(&c);

stop:
	stop_server(&server);
	assert("server succeeded", server.error == NULL, "server error: %s\n", server.error);
	if (server.error == NULL)
	{
		MyLog(LOGA_DEBUG, "Upgrade request:\n%s", server.upgrade_request);
		if (broker == NULL)
		{
			snprintf(expected_buf, sizeof(expected_buf), "127.0.0.1:%d", server.port);
			expected_host = expected_buf;
		}
		snprintf(expected_origin, sizeof(expected_origin), "http://%s", expected_host);

		get_header(server.upgrade_request, "Host", value, sizeof(value));
		assert("Host header", strcmp(value, expected_host) == 0, "Host was \"%s\", expected \"%s\"\n",
			value, expected_host);
		get_header(server.upgrade_request, "Origin", value, sizeof(value));
		assert("Origin header", strcmp(value, expected_origin) == 0, "Origin was \"%s\", expected \"%s\"\n",
			value, expected_origin);
	}
exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}

int test1(struct Options options)
{
	/* the broker is the server, on a port which isn't the default */
	return run_test("test_issue1653-1 - direct, port isn't the default", NULL, 0, NULL);
}

int test2(struct Options options)
{
	return run_test("test_issue1653-2 - proxy, default port", "ws://broker.invalid:80/mqtt", 1,
		"broker.invalid");
}

int test3(struct Options options)
{
	return run_test("test_issue1653-3 - proxy, port isn't the default", "ws://broker.invalid:8080/mqtt", 1,
		"broker.invalid:8080");
}

int test4(struct Options options)
{
	return run_test("test_issue1653-4 - proxy, IPv6 address", "ws://[::1]:8080/mqtt", 1, "[::1]:8080");
}


int main(int argc, char** argv)
{
	int rc = 0;
	int (*test_funcs[])(struct Options) = {NULL, test1, test2, test3, test4};
	int i;

	getopts(argc, argv);
	signal(SIGPIPE, SIG_IGN);
	/* the proxy is only set through the API */
	unsetenv("PAHO_C_CLIENT_USE_HTTP_PROXY");

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
