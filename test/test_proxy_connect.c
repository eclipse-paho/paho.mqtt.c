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
 * Test the CONNECT request sent to an HTTP proxy. When the broker's URI had no
 * port, the request used the proxy's default port, 8080, rather than the
 * broker's, which is 1883 for tcp, 8883 for ssl, 80 for ws and 443 for wss. The
 * closing bracket of an IPv6 address was also left out. The Host header had no
 * port, but RFC 7230 section 5.4 says it's the same as the target, host:port.
 * A proxy in a thread of this test captures the CONNECT request, and rejects
 * it, so the connect fails, and no broker is needed. The CONNECT request is sent
 * before the TLS handshake, so no TLS server is needed either.
 * Test 1: tcp, no port: 1883
 * Test 2: tcp, with a port: the port is kept
 * Test 3: ws, no port: 80
 * Test 4: ws, IPv6 address with a port
 * Test 5: ssl, no port: 8883 (only with TLS)
 * Test 6: wss, no port: 443 (only with TLS)
 * The test uses POSIX sockets and threads, so it isn't built on Windows.
 */

#include "MQTTClient.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
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


/* The proxy, which captures the CONNECT request and rejects it */

#define REQUEST_SIZE 4096

struct proxy
{
	int listen_fd;
	int port;
	char request[REQUEST_SIZE];
	const char* error; /* set if the proxy failed */
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

static void* proxy_thread(void* arg)
{
	static const char* rejected = "HTTP/1.1 403 Forbidden\r\n\r\n";
	struct proxy* p = arg;
	struct pollfd pfd;
	struct timeval timeout = {10, 0};
	int fd;

	pfd.fd = p->listen_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 10000) != 1 || (fd = accept(p->listen_fd, NULL, NULL)) < 0)
	{
		p->error = "the client didn't connect";
		return NULL;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	if (read_request(fd, p->request, sizeof(p->request)) != 0)
		p->error = "failed to read the CONNECT request";
	else
		/* rejecting the request makes the client's connect fail straight away */
		send(fd, rejected, strlen(rejected), MSG_NOSIGNAL);
	close(fd);
	return NULL;
}

static int start_proxy(struct proxy* p)
{
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	int on = 1;

	memset(p, '\0', sizeof(*p));
	if ((p->listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1;
	setsockopt(p->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&addr, '\0', sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0; /* any free port */
	if (bind(p->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
			listen(p->listen_fd, 1) != 0 ||
			getsockname(p->listen_fd, (struct sockaddr*)&addr, &addr_len) != 0 ||
			pthread_create(&p->thread, NULL, proxy_thread, p) != 0)
	{
		close(p->listen_fd);
		return -1;
	}
	p->port = ntohs(addr.sin_port);
	return 0;
}

static void stop_proxy(const struct proxy* p)
{
	pthread_join(p->thread, NULL);
	close(p->listen_fd);
}


/* connect to the broker through the proxy, and check the request line of the CONNECT request */
static int run_test(const char* testname, const char* broker, int ssl, const char* expected_target)
{
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
#if defined(TEST_SSL)
	MQTTClient_SSLOptions sslopts = MQTTClient_SSLOptions_initializer;
#endif
	MQTTClient c = NULL;
	struct proxy proxy;
	char proxy_uri[100];
	char expected[200];
	int rc;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;

	rc = start_proxy(&proxy);
	assert("proxy started", rc == 0, "rc was %d\n", rc);
	if (rc != 0)
		goto exit;

	rc = MQTTClient_create(&c, broker, "proxy connect", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto stop;

	opts.connectTimeout = 2;
	/* only one attempt: with the default, the client tries MQTT 3.1 after 3.1.1 */
	opts.MQTTVersion = MQTTVERSION_3_1_1;
	snprintf(proxy_uri, sizeof(proxy_uri), "http://127.0.0.1:%d", proxy.port);
	if (ssl)
	{
#if defined(TEST_SSL)
		opts.ssl = &sslopts;
		opts.httpsProxy = proxy_uri;
#endif
	}
	else
		opts.httpProxy = proxy_uri;
	/* the proxy rejects the CONNECT request, so the connect fails */
	rc = MQTTClient_connect(c, &opts);
	assert("connect fails", rc != MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	MQTTClient_destroy(&c);

stop:
	stop_proxy(&proxy);
	assert("proxy succeeded", proxy.error == NULL, "proxy error: %s\n", proxy.error);
	if (proxy.error == NULL)
	{
		MyLog(LOGA_DEBUG, "CONNECT request:\n%s", proxy.request);
		snprintf(expected, sizeof(expected), "CONNECT %s HTTP/1.1\r\n", expected_target);
		assert("CONNECT request line", strncmp(proxy.request, expected, strlen(expected)) == 0,
			"request was \"%.*s\", expected \"%s\"\n", (int)strcspn(proxy.request, "\r"), proxy.request,
			expected_target);
		snprintf(expected, sizeof(expected), "\r\nHost: %s\r\n", expected_target);
		assert("Host header", strstr(proxy.request, expected) != NULL, "request was\n%s\nexpected Host: %s\n",
			proxy.request, expected_target);
	}
exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}

int test1(struct Options options)
{
	return run_test("test_proxy_connect-1 - tcp, no port", "tcp://broker.invalid", 0, "broker.invalid:1883");
}

int test2(struct Options options)
{
	return run_test("test_proxy_connect-2 - tcp, with a port", "tcp://broker.invalid:1884", 0,
		"broker.invalid:1884");
}

int test3(struct Options options)
{
	return run_test("test_proxy_connect-3 - ws, no port", "ws://broker.invalid/mqtt", 0, "broker.invalid:80");
}

int test4(struct Options options)
{
	return run_test("test_proxy_connect-4 - ws, IPv6 address", "ws://[::1]:8080/mqtt", 0, "[::1]:8080");
}

#if defined(TEST_SSL)
int test5(struct Options options)
{
	return run_test("test_proxy_connect-5 - ssl, no port", "ssl://broker.invalid", 1, "broker.invalid:8883");
}

int test6(struct Options options)
{
	return run_test("test_proxy_connect-6 - wss, no port", "wss://broker.invalid/mqtt", 1, "broker.invalid:443");
}
#endif


int main(int argc, char** argv)
{
	int rc = 0;
#if defined(TEST_SSL)
	int (*test_funcs[])(struct Options) = {NULL, test1, test2, test3, test4, test5, test6};
#else
	int (*test_funcs[])(struct Options) = {NULL, test1, test2, test3, test4};
#endif
	int i;

	getopts(argc, argv);
	signal(SIGPIPE, SIG_IGN);
	/* the proxies are only set through the API */
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
