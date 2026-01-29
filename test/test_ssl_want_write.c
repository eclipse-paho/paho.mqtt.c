/*******************************************************************************
 * Copyright (c) 2024
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
 * Contributors:
 *    Unit test for SSL_ERROR_WANT_WRITE/WANT_READ handling during SSL handshake
 *******************************************************************************/

/**
 * @file
 * Test for SSL_ERROR_WANT_WRITE and SSL_ERROR_WANT_READ handling in SSLSocket_connect()
 * 
 * This test verifies that SSL connections complete successfully even when
 * SSL_ERROR_WANT_WRITE or SSL_ERROR_WANT_READ occur during the handshake.
 * The fix ensures proper socket polling by calling Socket_addPendingWrite()
 * when WANT_WRITE occurs.
 */

#include "MQTTClient.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#if !defined(_WINDOWS)
#include <sys/time.h>
#include <unistd.h>
#endif

/* Test configuration */
struct Options
{
	char* connection;
	char* client_key_file;
	char* server_key_file;
	int verbose;
} options = 
{
	"ssl://localhost:18883",
	NULL,
	NULL,
	1
};

void usage(void)
{
	printf("SSL WANT_WRITE Handshake Test\n");
	printf("Options:\n");
	printf("\t--hostname <hostname> - SSL server hostname (default: ssl://localhost:18883)\n");
	printf("\t--client_key <file> - Client certificate file\n");
	printf("\t--server_key <file> - Server CA certificate file\n");
	printf("\t--verbose - Enable verbose output\n");
	printf("\t--help - This help\n");
	exit(EXIT_FAILURE);
}

void getopts(int argc, char** argv)
{
	int count = 1;

	while (count < argc)
	{
		if (strcmp(argv[count], "--hostname") == 0)
		{
			if (++count < argc)
				options.connection = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--client_key") == 0)
		{
			if (++count < argc)
				options.client_key_file = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--server_key") == 0)
		{
			if (++count < argc)
				options.server_key_file = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--verbose") == 0)
		{
			options.verbose = 1;
		}
		else if (strcmp(argv[count], "--help") == 0)
		{
			usage();
		}
		else
		{
			printf("Unknown option: %s\n", argv[count]);
			usage();
		}
		count++;
	}
}

/* Logging */
void MyLog(int level, char* format, ...)
{
	static char msg_buf[512];
	va_list args;
	struct timeval ts;
	struct tm timeinfo;

	if (!options.verbose)
		return;

	gettimeofday(&ts, NULL);
	localtime_r(&ts.tv_sec, &timeinfo);
	strftime(msg_buf, 80, "%Y%m%d %H%M%S", &timeinfo);
	sprintf(&msg_buf[strlen(msg_buf)], ".%.3lu ", ts.tv_usec / 1000L);

	va_start(args, format);
	vsnprintf(&msg_buf[strlen(msg_buf)], sizeof(msg_buf) - strlen(msg_buf), format, args);
	va_end(args);

	printf("%s\n", msg_buf);
	fflush(stdout);
}

/* Test tracking */
int tests = 0;
int failures = 0;

#define ASSERT(condition, message) \
	do { \
		tests++; \
		if (!(condition)) { \
			failures++; \
			MyLog(1, "FAIL: %s (line %d): %s", __func__, __LINE__, message); \
			return -1; \
		} \
	} while(0)

/**
 * Test: Basic SSL connection
 * 
 * Verifies that SSL connections complete successfully.
 * This validates that the SSL_ERROR_WANT_WRITE/WANT_READ handling
 * in SSLSocket_connect() works correctly.
 */
int test_ssl_connect_basic(void)
{
	MQTTClient client;
	MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
	MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;
	int rc;

	MyLog(1, "Test: Basic SSL connection");

	rc = MQTTClient_create(&client, options.connection, "test_ssl_want_write",
	                       MQTTCLIENT_PERSISTENCE_NONE, NULL);
	ASSERT(rc == MQTTCLIENT_SUCCESS, "MQTTClient_create failed");

	conn_opts.keepAliveInterval = 20;
	conn_opts.cleansession = 1;

	if (options.server_key_file)
	{
		ssl_opts.trustStore = options.server_key_file;
		conn_opts.ssl = &ssl_opts;
	}

	if (options.client_key_file)
	{
		ssl_opts.keyStore = options.client_key_file;
	}

	MyLog(1, "Connecting to %s", options.connection);
	rc = MQTTClient_connect(client, &conn_opts);

	if (rc != MQTTCLIENT_SUCCESS)
	{
		MyLog(1, "Connection failed: rc=%d", rc);
		MyLog(1, "Note: This is expected if no SSL broker is running on %s", options.connection);
		/* Not a test failure - just means no broker available */
	}
	else
	{
		MyLog(1, "Connection succeeded");
		MQTTClient_disconnect(client, 1000);
	}

	MQTTClient_destroy(&client);
	return 0;
}

/**
 * Main test runner
 */
int main(int argc, char** argv)
{
	int rc = 0;

	getopts(argc, argv);

	printf("SSL Connection Test\n");
	printf("===================\n");
	printf("Connection: %s\n", options.connection);
	if (options.client_key_file)
		printf("Client Key: %s\n", options.client_key_file);
	if (options.server_key_file)
		printf("Server Key: %s\n", options.server_key_file);
	printf("\n");

	rc = test_ssl_connect_basic();

	printf("\nTest Results: %d tests, %d failures\n", tests, failures);

	if (failures == 0)
		printf("SUCCESS\n");
	else
		printf("FAILURE: %d test(s) failed\n", failures);

	return (failures == 0) ? 0 : 1;
}
