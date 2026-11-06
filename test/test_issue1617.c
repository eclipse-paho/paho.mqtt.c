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
 * Test for issue 1617: an acknowledgement which doesn't fit the message it is for.
 *
 * Azure IoT Hub answers a QoS 2 PUBLISH with a PUBACK. That was logged, but the message stayed
 * in the client's outbound list, while the publish command was freed with its topic and payload,
 * which were then freed again when the message was removed: a double free. The same happened
 * with a PUBCOMP for a QoS 1 message, a PUBCOMP before the PUBREC for a QoS 2 message, and,
 * in MQTT 5, a PUBREC with a failure reason code for a QoS 1 message. In those cases the
 * publish success callback was called too.
 *
 * An acknowledgement like this is a protocol error, so the client now closes the connection.
 * The asynchronous client calls the publish onFailure callback, with MQTTASYNC_OPERATION_INCOMPLETE,
 * and connectionLost. The synchronous client calls connectionLost, and doesn't call
 * deliveryComplete or the MQTT 5 published callback.
 *
 * The test runs its own minimal MQTT server in a thread, which answers each PUBLISH with the
 * acknowledgement given by the test. No broker is needed. The same source is built for the
 * asynchronous (the default) and synchronous (TEST_SYNC defined) clients.
 */

#if defined(TEST_SYNC)
#include "MQTTClient.h"
#else
#include "MQTTAsync.h"
#endif
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
	printf("  --test_no <n>  run test n (default all)\n");
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

#define TOPIC "issue1617"
#define PUBACK_TYPE 4
#define PUBREC_TYPE 5
#define PUBCOMP_TYPE 7

/*
 * A minimal MQTT server, enough for this test: it accepts connections one after the other,
 * answers CONNECT and PINGREQ, and answers a PUBLISH of QoS 1 or 2 with the acknowledgement
 * in server.ack_type, whatever the QoS.
 */
struct
{
	int listen_fd;
	int port;
	pthread_t thread;
	volatile int ack_type;      /* PUBACK_TYPE, PUBREC_TYPE or PUBCOMP_TYPE */
	volatile int ack_reason;    /* MQTT 5 reason code to add to the acknowledgement, or -1 */
	volatile int publishes;
} server = {-1, 0};

/* read one packet, returning its first byte in type and its contents in buf */
int read_packet(int fd, unsigned char* type, unsigned char* buf, int buflen, int* len)
{
	unsigned char b;
	int multiplier = 1, got = 0;

	*len = 0;
	if (recv(fd, type, 1, 0) != 1)
		return -1;
	do
	{
		if (recv(fd, &b, 1, 0) != 1)
			return -1;
		*len += (b & 127) * multiplier;
		multiplier *= 128;
	} while (b & 128);
	if (*len > buflen)
		return -1;
	while (got < *len)
	{
		int n = (int)recv(fd, buf + got, (size_t)(*len - got), 0);
		if (n <= 0)
			return -1;
		got += n;
	}
	return 0;
}

void* server_thread(void* arg)
{
	int fd;

	while ((fd = accept(server.listen_fd, NULL, NULL)) >= 0)
	{
		unsigned char type;
		unsigned char buf[1024];
		int len = 0;
		int version = 4;

		while (read_packet(fd, &type, buf, sizeof(buf), &len) == 0)
		{
			if ((type >> 4) == 1) /* CONNECT: the protocol level follows the protocol name "MQTT" */
			{
				unsigned char connack[] = {0x20, 0x02, 0x00, 0x00, 0x00};

				version = buf[6];
				if (version == 5)
					connack[1] = 0x03; /* with an empty property length */
				send(fd, connack, connack[1] + 2, 0);
			}
			else if ((type >> 4) == 3) /* PUBLISH */
			{
				int qos = (type >> 1) & 0x03;
				int topiclen = buf[0] * 256 + buf[1];

				server.publishes++;
				if (qos > 0)
				{
					unsigned char ack[] = {0x00, 0x02, buf[2 + topiclen], buf[3 + topiclen], 0x00};

					ack[0] = (unsigned char)(server.ack_type << 4);
					if (version == 5 && server.ack_reason >= 0)
					{
						ack[1] = 0x03;
						ack[4] = (unsigned char)server.ack_reason;
					}
					send(fd, ack, ack[1] + 2, 0);
				}
			}
			else if ((type >> 4) == 12) /* PINGREQ */
			{
				unsigned char pingresp[] = {0xD0, 0x00};
				send(fd, pingresp, sizeof(pingresp), 0);
			}
			else if ((type >> 4) == 14) /* DISCONNECT */
				break;
		}
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


struct
{
	volatile int connection_lost;
	volatile int heap_errors;    /* bad frees reported in the trace by heap tracking */
	volatile int success;        /* async: publish onSuccess; sync: deliveryComplete */
	volatile int published;      /* sync: MQTT 5 published callback */
	volatile int failure;        /* async: publish onFailure */
	volatile int failure_code;
} results;

void connectionLost(void* context, char* cause)
{
	MyLog(LOGA_DEBUG, "In connectionLost callback");
	results.connection_lost++;
}

#if defined(TEST_SYNC)

void trace_callback(enum MQTTCLIENT_TRACE_LEVELS level, char* message)
{
	if (strstr(message, "Failed to remove heap item"))
	{
		++results.heap_errors;
		MyLog(LOGA_INFO, "Trace: %s", message);
	}
}

int messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	MQTTClient_freeMessage(&message);
	MQTTClient_free(topicName);
	return 1;
}

void deliveryComplete(void* context, MQTTClient_deliveryToken token)
{
	MyLog(LOGA_DEBUG, "In deliveryComplete callback, token %d", token);
	results.success++;
}

void published(void* context, int dt, int packet_type, MQTTProperties* properties, enum MQTTReasonCodes reasonCode)
{
	MyLog(LOGA_DEBUG, "In published callback, token %d packet type %d reason code %d", dt, packet_type, reasonCode);
	results.published++;
}

/**
 * Connect to the test server, publish a message, and wait for the publish to complete.
 * @return the return code from MQTTClient_waitForCompletion
 */
int publish(int mqtt_version, int qos)
{
	MQTTClient c = NULL;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_connectOptions opts5 = MQTTClient_connectOptions_initializer5;
	MQTTClient_deliveryToken dt = 0;
	char uri[64];
	int rc;

	memset(&results, '\0', sizeof(results));
	snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%d", server.port);
	createOpts.MQTTVersion = mqtt_version;
	rc = MQTTClient_createWithOptions(&c, uri, "issue1617", MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		return rc;
	MQTTClient_setCallbacks(c, NULL, connectionLost, messageArrived, deliveryComplete);
	if (mqtt_version == MQTTVERSION_5)
	{
		MQTTResponse response;

		MQTTClient_setPublished(c, NULL, published);
		opts5.keepAliveInterval = 20;
		response = MQTTClient_connect5(c, &opts5, NULL, NULL);
		rc = response.reasonCode;
		MQTTResponse_free(response);
	}
	else
	{
		opts.keepAliveInterval = 20;
		opts.cleansession = 1;
		opts.MQTTVersion = mqtt_version;
		rc = MQTTClient_connect(c, &opts);
	}
	assert("good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	if (mqtt_version == MQTTVERSION_5)
	{
		MQTTResponse response = MQTTClient_publish5(c, TOPIC, 5, "hello", qos, 0, NULL, &dt);
		rc = response.reasonCode;
		MQTTResponse_free(response);
	}
	else
		rc = MQTTClient_publish(c, TOPIC, 5, "hello", qos, 0, &dt);
	assert("good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	rc = MQTTClient_waitForCompletion(c, dt, 5000L);
	MyLog(LOGA_INFO, "waitForCompletion rc %d, deliveryComplete %d, published %d, connectionLost %d",
			rc, results.success, results.published, results.connection_lost);
	MySleep(200); /* let connectionLost be called */
	if (MQTTClient_isConnected(c))
		MQTTClient_disconnect(c, 1000);
exit:
	MQTTClient_destroy(&c);
	return rc;
}

/* check the outcome of a publish whose acknowledgement doesn't fit the message */
void check_wrong_ack(int rc)
{
	assert("publish did not complete", rc != MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	assert("deliveryComplete not called", results.success == 0, "deliveryComplete called %d times\n", results.success);
	assert("published not called", results.published == 0, "published called %d times\n", results.published);
	assert("connectionLost called", results.connection_lost == 1, "connectionLost called %d times\n", results.connection_lost);
	assert("no bad frees reported", results.heap_errors == 0, "%d bad frees reported\n", results.heap_errors);
}

/* check the outcome of a publish whose acknowledgement is correct */
void check_good_ack(int rc)
{
	assert("publish completed", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	assert("deliveryComplete called", results.success == 1, "deliveryComplete called %d times\n", results.success);
	assert("connectionLost not called", results.connection_lost == 0, "connectionLost called %d times\n", results.connection_lost);
	assert("no bad frees reported", results.heap_errors == 0, "%d bad frees reported\n", results.heap_errors);
}

#define CLIENT "sync"
#define SET_TRACE_CALLBACK MQTTClient_setTraceCallback
#define SET_TRACE_LEVEL() MQTTClient_setTraceLevel(MQTTCLIENT_TRACE_ERROR)

#else

void trace_callback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
	if (strstr(message, "Failed to remove heap item"))
	{
		++results.heap_errors;
		MyLog(LOGA_INFO, "Trace: %s", message);
	}
}

volatile int connected = 0;

void onConnect(void* context, MQTTAsync_successData* response)
{
	connected = 1;
}

void onConnect5(void* context, MQTTAsync_successData5* response)
{
	connected = 1;
}

void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	connected = -1;
}

void onConnectFailure5(void* context, MQTTAsync_failureData5* response)
{
	connected = -1;
}

int messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);
	return 1;
}

void onPublish(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In publish onSuccess callback");
	results.success++;
}

void onPublish5(void* context, MQTTAsync_successData5* response)
{
	MyLog(LOGA_DEBUG, "In publish onSuccess5 callback");
	results.success++;
}

void onPublishFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In publish onFailure callback, code %d", response->code);
	results.failure_code = response->code;
	results.failure++;
}

void onPublishFailure5(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In publish onFailure5 callback, code %d reason code %d", response->code, response->reasonCode);
	results.failure_code = response->code;
	results.failure++;
}

/**
 * Connect to the test server, publish a message, and wait for a publish callback.
 * @return 0 if the publish was sent, otherwise an error code
 */
int publish(int mqtt_version, int qos)
{
	MQTTAsync c = NULL;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_connectOptions opts5 = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_connectOptions* connopts = (mqtt_version == MQTTVERSION_5) ? &opts5 : &opts;
	char uri[64];
	int rc;
	int count = 0;

	memset(&results, '\0', sizeof(results));
	connected = 0;
	snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%d", server.port);
	createOpts.MQTTVersion = mqtt_version;
	rc = MQTTAsync_createWithOptions(&c, uri, "issue1617", MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		return rc;
	MQTTAsync_setCallbacks(c, NULL, connectionLost, messageArrived, NULL);

	connopts->keepAliveInterval = 20;
	if (mqtt_version == MQTTVERSION_5)
	{
		connopts->onSuccess5 = onConnect5;
		connopts->onFailure5 = onConnectFailure5;
		ropts.onSuccess5 = onPublish5;
		ropts.onFailure5 = onPublishFailure5;
	}
	else
	{
		connopts->MQTTVersion = mqtt_version;
		connopts->onSuccess = onConnect;
		connopts->onFailure = onConnectFailure;
		ropts.onSuccess = onPublish;
		ropts.onFailure = onPublishFailure;
	}
	rc = MQTTAsync_connect(c, connopts);
	assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	while (connected == 0 && ++count < 100)
		MySleep(100);
	assert("connected", connected == 1, "connected %d\n", connected);
	if (connected != 1)
	{
		rc = MQTTASYNC_FAILURE;
		goto exit;
	}

	rc = MQTTAsync_send(c, TOPIC, 5, "hello", qos, 0, &ropts);
	assert("good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	count = 0;
	while (results.success == 0 && results.failure == 0 && ++count < 50)
		MySleep(100);
	MySleep(200); /* let connectionLost be called, and any second publish callback */
	MyLog(LOGA_INFO, "onSuccess %d, onFailure %d code %d, connectionLost %d",
			results.success, results.failure, results.failure_code, results.connection_lost);
	if (MQTTAsync_isConnected(c))
		MQTTAsync_disconnect(c, NULL);
	MySleep(100);
exit:
	MQTTAsync_destroy(&c);
	return rc;
}

/* check the outcome of a publish whose acknowledgement doesn't fit the message */
void check_wrong_ack(int rc)
{
	assert("publish onSuccess not called", results.success == 0, "onSuccess called %d times\n", results.success);
	assert("publish onFailure called once", results.failure == 1, "onFailure called %d times\n", results.failure);
	assert("onFailure code is MQTTASYNC_OPERATION_INCOMPLETE", results.failure_code == MQTTASYNC_OPERATION_INCOMPLETE,
			"code was %d\n", results.failure_code);
	assert("connectionLost called", results.connection_lost == 1, "connectionLost called %d times\n", results.connection_lost);
	assert("no bad frees reported", results.heap_errors == 0, "%d bad frees reported\n", results.heap_errors);
}

/* check the outcome of a publish whose acknowledgement is correct */
void check_good_ack(int rc)
{
	assert("publish onSuccess called once", results.success == 1, "onSuccess called %d times\n", results.success);
	assert("publish onFailure not called", results.failure == 0, "onFailure called %d times\n", results.failure);
	assert("connectionLost not called", results.connection_lost == 0, "connectionLost called %d times\n", results.connection_lost);
	assert("no bad frees reported", results.heap_errors == 0, "%d bad frees reported\n", results.heap_errors);
}

#define CLIENT "async"
#define SET_TRACE_CALLBACK MQTTAsync_setTraceCallback
#define SET_TRACE_LEVEL() MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR)

#endif

struct test_case
{
	char* description;
	int mqtt_version;
	int qos;
	int ack_type;
	int ack_reason;
	int good;           /* is the acknowledgement correct for the message? */
} test_cases[] =
{
	{NULL},
	{"PUBACK for a QoS 2 message", MQTTVERSION_3_1_1, 2, PUBACK_TYPE, -1, 0},
	{"PUBCOMP for a QoS 1 message", MQTTVERSION_3_1_1, 1, PUBCOMP_TYPE, -1, 0},
	{"PUBCOMP before PUBREC for a QoS 2 message", MQTTVERSION_3_1_1, 2, PUBCOMP_TYPE, -1, 0},
	{"PUBREC for a QoS 1 message", MQTTVERSION_3_1_1, 1, PUBREC_TYPE, -1, 0},
	{"MQTT 5 PUBACK for a QoS 2 message", MQTTVERSION_5, 2, PUBACK_TYPE, -1, 0},
	{"MQTT 5 PUBREC with a failure reason code for a QoS 1 message", MQTTVERSION_5, 1, PUBREC_TYPE, 0x80, 0},
	{"PUBACK for a QoS 1 message", MQTTVERSION_3_1_1, 1, PUBACK_TYPE, -1, 1},
};

#endif /* !_WINDOWS */


int run_test(int test_no)
{
#if defined(_WINDOWS)
	MyLog(LOGA_INFO, "Skipping test1617 %d on Windows", test_no);
	return 0;
#else
	struct test_case* t = &test_cases[test_no];
	char testname[160];
	int rc;

	snprintf(testname, sizeof(testname), "test1617-%d - %s client, %s", test_no, CLIENT, t->description);
	MyLog(LOGA_INFO, "Starting %s", testname);
	failures = 0;

	SET_TRACE_CALLBACK(trace_callback);
	SET_TRACE_LEVEL();
	rc = start_server();
	assert("server started", rc == 0, "rc was %d\n", rc);
	if (rc != 0)
		goto exit;
	server.ack_type = t->ack_type;
	server.ack_reason = t->ack_reason;
	server.publishes = 0;

	rc = publish(t->mqtt_version, t->qos);
	assert("message published", server.publishes == 1, "server received %d publishes\n", server.publishes);
	if (t->good)
		check_good_ack(rc);
	else
		check_wrong_ack(rc);

	stop_server();
exit:
	SET_TRACE_CALLBACK(NULL);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
#endif
}


int main(int argc, char** argv)
{
	int rc = 0;
	int count = 7; /* the number of test cases */
	int i;

	getopts(argc, argv);

	if (options.test_no == 0)
	{
		for (i = 1; i <= count; ++i)
			rc += run_test(i);
	}
	else if (options.test_no > 0 && options.test_no <= count)
		rc = run_test(options.test_no);
	else
		usage();

	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");
	return rc;
}
