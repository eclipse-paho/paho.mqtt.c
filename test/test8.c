/*******************************************************************************
 * Copyright (c) 2012, 2024 IBM Corp., Ian Craggs and others
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
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *******************************************************************************/

/**
 * @file
 * Tests for the Paho MQTT Async C client
 */


#include "MQTTAsync.h"
#include "Thread.h"
#include <string.h>
#include <stdlib.h>

#if !defined(_WINDOWS)
	#include <sys/time.h>
	#include <sys/socket.h>
	#include <unistd.h>
	#include <errno.h>
	#define WINAPI
#else
	#include <windows.h>
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

void usage(void)
{
	printf("help!!\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	char* connection;         /**< connection to system under test. */
	int verbose;
	int test_no;
	int size;									/**< size of big message */
} options =
{
	"tcp://localhost:1883",
	0,
	-1,
	5000000,
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
		else if (strcmp(argv[count], "--size") == 0)
		{
			if (++count < argc)
				options.size = atoi(argv[count]);
			else
				usage();
		}
		else if (strcmp(argv[count], "--connection") == 0)
		{
			if (++count < argc)
				options.connection = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--verbose") == 0)
			options.verbose = 1;
		count++;
	}
}


#define LOGA_DEBUG 0
#define LOGA_INFO 1
#include <stdarg.h>
#include <time.h>
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
	struct tm timeinfo;

	if (LOGA_level == LOGA_DEBUG && options.verbose == 0)
	  return;

#if defined(_WIN32) || defined(_WINDOWS)
	ftime(&ts);
	localtime_s(&timeinfo, &ts.time);
#else
	gettimeofday(&ts, NULL);
	localtime_r(&ts.tv_sec, &timeinfo);
#endif
	strftime(msg_buf, 80, "%Y%m%d %H%M%S", &timeinfo);

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


#if defined(_WIN32) || defined(_WINDOWS)
#define mqsleep(A) Sleep(1000*A)
#define START_TIME_TYPE DWORD
static DWORD start_time = 0;
START_TIME_TYPE start_clock(void)
{
	return GetTickCount();
}
#elif defined(AIX)
#define mqsleep sleep
#define START_TIME_TYPE struct timespec
START_TIME_TYPE start_clock(void)
{
	static struct timespec start;
	clock_gettime(CLOCK_REALTIME, &start);
	return start;
}
#else
#define mqsleep sleep
#define START_TIME_TYPE struct timeval
/* TODO - unused - remove? static struct timeval start_time; */
START_TIME_TYPE start_clock(void)
{
	struct timeval start_time;
	gettimeofday(&start_time, NULL);
	return start_time;
}
#endif


#if defined(_WIN32)
long elapsed(START_TIME_TYPE start_time)
{
	return GetTickCount() - start_time;
}
#elif defined(AIX)
#define assert(a)
long elapsed(struct timespec start)
{
	struct timespec now, res;

	clock_gettime(CLOCK_REALTIME, &now);
	ntimersub(now, start, res);
	return (res.tv_sec)*1000L + (res.tv_nsec)/1000000L;
}
#else
long elapsed(START_TIME_TYPE start_time)
{
	struct timeval now, res;

	gettimeofday(&now, NULL);
	timersub(&now, &start_time, &res);
	return (res.tv_sec)*1000 + (res.tv_usec)/1000;
}
#endif


START_TIME_TYPE global_start_time;


#define assert(a, b, c, d) myassert(__FILE__, __LINE__, a, b, c, d)
#define assert1(a, b, c, d, e) myassert(__FILE__, __LINE__, a, b, c, d, e)


int tests = 0;
int failures = 0;


void myassert(char* filename, int lineno, char* description, int value, char* format, ...)
{
	++tests;
	if (!value)
	{
		va_list args;

		++failures;
		printf("Assertion failed, file %s, line %d, description: %s\n", filename, lineno, description);

		va_start(args, format);
		vprintf(format, args);
		va_end(args);
	}
    else
    	MyLog(LOGA_DEBUG, "Assertion succeeded, file %s, line %d, description: %s", filename, lineno, description);
}

volatile int test_finished = 0;

char* test_topic = "async test topic";


void test1_onDisconnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", c);
	test_finished = 1;
}


void test1_onUnsubscribe(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In onUnsubscribe onSuccess callback %p", c);
	opts.onSuccess = test1_onDisconnect;
	opts.context = c;
	opts.timeout = 1000;

	rc = MQTTAsync_disconnect(c, &opts);
	assert("Disconnect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


int test1_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;
	int rc;

	MyLog(LOGA_DEBUG, "In messageArrived callback %p", c);

	if (++message_count == 1)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
		pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.qos = 2;
		pubmsg.retained = 0;
		rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
	}
	else
	{
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		opts.onSuccess = test1_onUnsubscribe;
		opts.context = c;
		rc = MQTTAsync_unsubscribe(c, test_topic, &opts);
		assert("Unsubscribe successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}

void test1_onSubscribe(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback %p granted qos %d", c, response->alt.qos);

	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.qos = 2;
	pubmsg.retained = 0;

	rc = MQTTAsync_send(c, test_topic, pubmsg.payloadlen, pubmsg.payload, pubmsg.qos, pubmsg.retained, NULL);
}


void test1_onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	opts.onSuccess = test1_onSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribe(c, test_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished = 1;
}


void test1_onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test_finished = 1;
}


/*********************************************************************

Test1: Basic connect, subscribe send and receive.

*********************************************************************/
int test1(struct Options options)
{
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test1";
	char* serverURIs[2] = {"tcp://localhost:1882", options.connection};

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 1 - asynchronous connect");

	rc = MQTTAsync_create(&c, options.connection, "async_8_test1",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test1_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = test1_onConnect;
	opts.onFailure = test1_onConnectFailure;
	opts.context = c;
	opts.serverURIcount = 2;
	opts.serverURIs = serverURIs;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	rc = 0;
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST1: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);

	return failures;
}

int test2_onFailure_called = 0;

void test2_onFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test2_onFailure_called++;
	test_finished = 1;
}


void test2_onConnect(void* context, MQTTAsync_successData* response)
{

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p\n", context);

	assert("Connect should not succeed", 0, "connect success callback was called", 0);

	test_finished = 1;
}

/*********************************************************************

Test2: connect timeout

*********************************************************************/
int test2(struct Options options)
{
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test2";

	test_finished = 0;

	MyLog(LOGA_INFO, "Starting test 2 - connect timeout");

	rc = MQTTAsync_create(&c, "tcp://9.20.96.160:66", "connect timeout",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test1_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.connectTimeout = 5;
	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = test2_onConnect;
	opts.onFailure = test2_onFailure;
	opts.context = c;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	rc = 0;
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	MQTTAsync_destroy(&c);

exit:
	assert("Connect onFailure should be called once", test2_onFailure_called == 1,
			"connect onFailure was called %d times", test2_onFailure_called);

	MyLog(LOGA_INFO, "TEST2: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);

	return failures;
}


typedef struct
{
	MQTTAsync c;
	int index;
	char clientid[24];
	char test_topic[100];
	int	message_count;
} client_data;


void test3_onDisconnect(void* context, MQTTAsync_successData* response)
{
	client_data* cd = (client_data*)context;
	MyLog(LOGA_DEBUG, "In onDisconnect callback for client \"%s\"", cd->clientid);
	test_finished++;
}


void test3_onPublish(void* context,  MQTTAsync_successData* response)
{
	client_data* cd = (client_data*)context;
	MyLog(LOGA_DEBUG, "In QoS 0 onPublish callback for client \"%s\"", cd->clientid);
}


void test3_onUnsubscribe(void* context, MQTTAsync_successData* response)
{
	client_data* cd = (client_data*)context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In onUnsubscribe onSuccess callback \"%s\"", cd->clientid);
	opts.onSuccess = test3_onDisconnect;
	opts.context = cd;
	opts.timeout = 1000;

	rc = MQTTAsync_disconnect(cd->c, &opts);
	assert("Disconnect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


int test3_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	client_data* cd = (client_data*)context;
	int rc;

	MyLog(LOGA_DEBUG, "In messageArrived callback \"%s\" message count ", cd->clientid);

	if (++cd->message_count == 1)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
		pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 25;
		pubmsg.qos = 1;
		pubmsg.retained = 0;
		rc = MQTTAsync_sendMessage(cd->c, cd->test_topic, &pubmsg, &opts);
		assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}
	else if (cd->message_count == 2)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
		pubmsg.payload = "a QoS 0 message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 29;
		pubmsg.qos = 0;
		pubmsg.retained = 0;
		opts.context = cd;
		opts.onSuccess = test3_onPublish;

		rc = MQTTAsync_sendMessage(cd->c, cd->test_topic, &pubmsg, &opts);
		assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}
	else
	{
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		opts.onSuccess = test3_onUnsubscribe;
		opts.context = cd;
		rc = MQTTAsync_unsubscribe(cd->c, cd->test_topic, &opts);
		assert("Unsubscribe successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}
	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);
	return 1;
}

void test3_onSubscribe(void* context, MQTTAsync_successData* response)
{
	client_data* cd = (client_data*)context;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback \"%s\"", cd->clientid);

	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.qos = 2;
	pubmsg.retained = 0;

	rc = MQTTAsync_send(cd->c, cd->test_topic, pubmsg.payloadlen, pubmsg.payload, pubmsg.qos, pubmsg.retained, NULL);
	assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


void test3_onConnect(void* context, MQTTAsync_successData* response)
{
	client_data* cd = (client_data*)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, \"%s\"", cd->clientid);
	opts.onSuccess = test3_onSubscribe;
	opts.context = cd;

	rc = MQTTAsync_subscribe(cd->c, cd->test_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished++;
}


void test3_onFailure(void* context, MQTTAsync_failureData* response)
{
	client_data* cd = (client_data*)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

	assert("Should have connected", 0, "failed to connect", NULL);
	MyLog(LOGA_DEBUG, "In connect onFailure callback, \"%s\" rc %d\n", cd->clientid, response->code);
	if (response->message)
		MyLog(LOGA_DEBUG, "In connect onFailure callback, \"%s\"\n", response->message);

	test_finished++;
}


/*********************************************************************

Test3: More than one client object - simultaneous working.

*********************************************************************/
int test3(struct Options options)
{
	#define TEST3_CLIENTS 10
	int num_clients = TEST3_CLIENTS;
	int subsqos = 2;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	int i;
	client_data clientdata[TEST3_CLIENTS];

	test_finished = 0;
	MyLog(LOGA_INFO, "Starting test 3 - multiple connections");

	for (i = 0; i < num_clients; ++i)
	{
		sprintf(clientdata[i].clientid, "async_test3_num_%d", i);
		sprintf(clientdata[i].test_topic, "async test3 topic num %d", i);
		clientdata[i].index = i;
		clientdata[i].message_count = 0;

		rc = MQTTAsync_create(&(clientdata[i].c), options.connection, clientdata[i].clientid,
			MQTTCLIENT_PERSISTENCE_NONE, NULL);
		assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);

		rc = MQTTAsync_setCallbacks(clientdata[i].c, &clientdata[i], NULL, test3_messageArrived, NULL);
		assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

		opts.keepAliveInterval = 20;
		opts.cleansession = 1;
		opts.username = "testuser";
		opts.password = "testpassword";

		opts.will = &wopts;
		opts.will->message = "will message";
		opts.will->qos = 1;
		opts.will->retained = 0;
		opts.will->topicName = "will topic";
		opts.onSuccess = test3_onConnect;
		opts.onFailure = test3_onFailure;
		opts.context = &clientdata[i];

		MyLog(LOGA_DEBUG, "Connecting");
		rc = MQTTAsync_connect(clientdata[i].c, &opts);
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}

	while (test_finished < num_clients)
	{
		MyLog(LOGA_DEBUG, "num_clients %d test_finished %d\n", num_clients, test_finished);
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif
	}

	MyLog(LOGA_DEBUG, "TEST3: destroying clients");

	for (i = 0; i < num_clients; ++i)
		MQTTAsync_destroy(&clientdata[i].c);

/*exit:*/
	MyLog(LOGA_INFO, "TEST3: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);

	return failures;
}


void* test4_payload = NULL;
int test4_payloadlen = 0;

void test4_onPublish(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In publish onSuccess callback, context %p", context);
}

int test4_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;
	int rc, i;

	MyLog(LOGA_DEBUG, "In messageArrived callback %p", c);

	assert("Message size correct", message->payloadlen == test4_payloadlen,
				 "message size was %d", message->payloadlen);

	for (i = 0; i < options.size; ++i)
	{
		if (((char*)test4_payload)[i] != ((char*)message->payload)[i])
		{
			assert("Message contents correct", ((char*)test4_payload)[i] != ((char*)message->payload)[i],
				 "message content was %c", ((char*)message->payload)[i]);
			break;
		}
	}

	if (++message_count == 1)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		pubmsg.payload = test4_payload;
		pubmsg.payloadlen = test4_payloadlen;
		pubmsg.qos = 1;
		pubmsg.retained = 0;
		opts.onSuccess = test4_onPublish;
		opts.context = c;

		rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
	}
	else if (message_count == 2)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		pubmsg.payload = test4_payload;
		pubmsg.payloadlen = test4_payloadlen;
		pubmsg.qos = 0;
		pubmsg.retained = 0;
		opts.onSuccess = test4_onPublish;
		opts.context = c;
		rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
	}
	else
	{
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		opts.onSuccess = test1_onUnsubscribe;
		opts.context = c;
		rc = MQTTAsync_unsubscribe(c, test_topic, &opts);
		assert("Unsubscribe successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


void test4_onSubscribe(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc, i;

	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback %p", c);

	pubmsg.payload = test4_payload = malloc(options.size);
	pubmsg.payloadlen = test4_payloadlen = options.size;

	srand(33);
	for (i = 0; i < options.size; ++i)
		((char*)pubmsg.payload)[i] = rand() % 256;

	pubmsg.qos = 2;
	pubmsg.retained = 0;

	rc = MQTTAsync_send(c, test_topic, pubmsg.payloadlen, pubmsg.payload, pubmsg.qos, pubmsg.retained, NULL);
	assert("Send successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


void test4_onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	opts.onSuccess = test4_onSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribe(c, test_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished = 1;
}


/*********************************************************************

Test4: Send and receive big messages

*********************************************************************/
int test4(struct Options options)
{
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test4";

	test_finished = failures = 0;
	MyLog(LOGA_INFO, "Starting test 4 - big messages");

	rc = MQTTAsync_create(&c, options.connection, "async_test_4",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test4_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = test4_onConnect;
	opts.onFailure = NULL;
	opts.context = c;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	rc = 0;
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(1000L);
		#endif

	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST4: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);

	return failures;
}


int test5_onConnect_called = 0;
int test5_onFailure_called = 0;

void test5_onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);

	test5_onConnect_called++;
	test_finished = 1;
}

void test5_onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test5_onFailure_called++;
	test_finished = 1;
}

/*********************************************************************

Test5a: All HA connections out of service.

*********************************************************************/
int test5a(struct Options options)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test5a";
	char* serverURIs[3] = {"tcp://localhost:1880", "mqtt://localhost:1881", "tcp://localhost:1882"};

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 5a - All HA connections out of service");

	rc = MQTTAsync_create(&c, "rubbish", "all_ha_down",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.onSuccess = test5_onConnect;
	opts.onFailure = test5_onConnectFailure;
	opts.context = c;
	opts.serverURIcount = 3;
	opts.serverURIs = serverURIs;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	rc = 0;
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	MQTTAsync_destroy(&c);

exit:
	assert("Connect onFailure should be called once", test5_onFailure_called == 1,
			"connect onFailure was called %d times", test5_onFailure_called);

	MyLog(LOGA_INFO, "TEST5a: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);

	return failures;
}

/*********************************************************************

Test5b: All HA connections out of service except the last one.

*********************************************************************/
int test5b(struct Options options)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test5b";
	char* serverURIs[3] = {"tcp://localhost:1880", "mqtt://localhost:1881", options.connection};

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 5b - All HA connections out of service except the last one");

	rc = MQTTAsync_create(&c, "rubbish", "all_ha_down_except_last_one",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.onSuccess = test5_onConnect;
	opts.onFailure = test5_onConnectFailure;
	opts.context = c;
	opts.serverURIcount = 3;
	opts.serverURIs = serverURIs;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	rc = 0;
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	MQTTAsync_destroy(&c);

exit:
	assert("Connect onConnect should be called once", test5_onConnect_called == 1,
			"connect onConnect was called %d times", test5_onConnect_called);

	MyLog(LOGA_INFO, "TEST5b: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);

	return failures;
}

/*********************************************************************

Test5c: All HA connections out of service except the first one.

*********************************************************************/
int test5c(struct Options options)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test5c";
	char* serverURIs[3] = {options.connection, "tcp://localhost:1881", "tcp://localhost:1882"};

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 5c - All HA connections out of service except the first one");

	rc = MQTTAsync_create(&c, "rubbish", "all_ha_down_except_first_one",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.onSuccess = test5_onConnect;
	opts.onFailure = test5_onConnectFailure;
	opts.context = c;
	opts.serverURIcount = 3;
	opts.serverURIs = serverURIs;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	rc = 0;
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	MQTTAsync_destroy(&c);

exit:
	assert("Connect onConnect should be called once", test5_onConnect_called == 1,
			"connect onConnect was called %d times", test5_onConnect_called);

	MyLog(LOGA_INFO, "TEST5c: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);

	return failures;
}


/*********************************************************************

Test6: Acks blocked due to sending messages

*********************************************************************/
char* test6_topic = "test6_topic";
char* test6_payload = NULL;
int test6_connected = 0;
int test6_payloadlen = 0;
int test6_message_count = 0;
int test6_disconnected = 0;


void test6_onDisconnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In onDisconnect callback");
	test6_disconnected = 1;
}


int test6_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In messageArrived callback %p", c);

	assert("Message size correct", message->payloadlen == test6_payloadlen,
				 "message size was %d", message->payloadlen);

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


void test6_onPublishFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_INFO, "In publish onFailure callback, context %p", context);
	failures++;
	test_finished = 1;
}


void test6_onPublish(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;
	static int publish_count = 0;

	if (++publish_count > 100)
	{
		test_finished = 1;
		goto exit;
	}

	MyLog(LOGA_INFO, "In publish onSuccess callback, count %d", publish_count);

	if (test6_payload == NULL) {
		test6_payload = malloc(options.size);
		memset(test6_payload, ' ', options.size);
	}

	MyLog(LOGA_DEBUG, "In publish onSuccess callback, context %p", context);
	pubmsg.payload = test6_payload;
	pubmsg.payloadlen = options.size;
	pubmsg.qos = 0;
	pubmsg.retained = 0;

	opts.onSuccess = test6_onPublish;
	opts.onFailure = test6_onPublishFailure;
	opts.context = c;

	MyLog(LOGA_INFO, "Calling sendMessage, count %d", publish_count);
	rc = MQTTAsync_sendMessage(c, "test6_big_messages", &pubmsg, &opts);
	MyLog(LOGA_INFO, "Called sendMessage, count %d rc %d", publish_count, rc);
	assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
exit:
	MyLog(LOGA_DEBUG, "Leaving publish onSuccess callback, count %d, context %p", publish_count, context);
}


void test6_onSubscribeFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_INFO, "In subscribe onFailure callback, context %p", context);
	failures++;
	test_finished = 1;
}


void test6_onSubscribe(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	if (test6_payload == NULL) {
		test6_payload = malloc(options.size);
		memset(test6_payload, ' ', options.size);
	}

	MyLog(LOGA_INFO, "In subscribe onSuccess callback, context %p", context);
	pubmsg.payload = test6_payload;
	pubmsg.payloadlen = options.size;
	pubmsg.qos = 0;
	pubmsg.retained = 0;

	opts.onSuccess = test6_onPublish;
	opts.onFailure = test6_onPublishFailure;
	opts.context = c;

	rc = MQTTAsync_sendMessage(c, "test6_big_messages", &pubmsg, &opts);
	assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


void test6_onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_INFO, "In connect onFailure callback, context %p", context);
	failures++;
	test_finished = 1;
}


void test6_onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_INFO, "In connect onSuccess callback, context %p", context);
	opts.onSuccess = test6_onSubscribe;
	opts.onFailure = test6_onSubscribeFailure;
	opts.context = c;

	rc = MQTTAsync_subscribe(c, test6_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished = 1;
}


void test6_onConnected(void* context, MQTTAsync_successData* response)
{
	MQTTAsync d = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	test6_connected = 1;
}


void test6_connectionLost(void* context, char* cause) {
	MyLog(LOGA_INFO, "Connection lost");
	test_finished = 1;
}


int test6(struct Options options)
{
	MQTTAsync c, d;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc = 0;
	char* test_topic = "C client test8 - test6";
	int messages_sent = 0;
	int count = 0;

	failures = 0;
	test_finished = 0;
	MyLog(LOGA_INFO, "Starting test 6 - acks blocked due to sending messages");
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, options.connection, "acks blocked",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, test6_connectionLost, test6_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.onSuccess = test6_onConnect;
	opts.onFailure = test6_onConnectFailure;
	opts.context = c;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	rc = MQTTAsync_create(&d, options.connection, "acks blocked - send loop",
			MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&d);
		goto exit;
	}

	opts.onSuccess = test6_onConnected;
	opts.context = d;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(d, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (test6_connected == 0 && test_finished == 0 && failures == 0)
	{
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif
	}

	pubmsg.payload = "small payload in loop";
	pubmsg.payloadlen = test6_payloadlen = strlen(pubmsg.payload)+1;
	pubmsg.qos = 1;
	pubmsg.retained = 0;

	while (test_finished == 0 && failures == 0)
	{
		rc = MQTTAsync_sendMessage(d, test6_topic, &pubmsg, NULL);
		assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		if (rc != MQTTASYNC_SUCCESS)
			break;
		messages_sent++;

		#if defined(_WIN32)
			Sleep(1000);
		#else
			usleep(100000L);
		#endif
	}

	dopts.onSuccess = test6_onDisconnect;
	dopts.timeout = 1000;

	dopts.context = d;
	test6_disconnected = 0;
	rc = MQTTAsync_disconnect(d, &dopts);
	assert("Disconnect start successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	while (test6_disconnected == 0 && ++count < 5)
	{
		#if defined(_WIN32)
			Sleep(1000);
		#else
			usleep(100000L);
		#endif
	}

	dopts.context = c;
	test6_disconnected = 0;
	rc = MQTTAsync_disconnect(c, &dopts);
	assert("Disconnect start successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	count = 0;
	while (test6_disconnected == 0 && ++count < 5)
	{
		#if defined(_WIN32)
			Sleep(1000);
		#else
			usleep(100000L);
		#endif
	}

	MQTTAsync_destroy(&c);
	MQTTAsync_destroy(&d);

exit:
	if (test6_payload)
		free(test6_payload);
	MyLog(LOGA_INFO, "TEST6: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	return failures;
}



void trace_callback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
	//if ((strstr(message, "onnect") && !strstr(message, "isconnect")) || level == MQTTASYNC_TRACE_ERROR)
		printf("Trace : %d, %s\n", level, message);
}


int main(int argc, char** argv)
{
	int rc = 0;
	int (*tests[])(struct Options) = {NULL, test1, test2, test3, test4, test5a, test5b, test5c, test6}; /* indexed starting from 1 */
	MQTTAsync_nameValue* info;

	getopts(argc, argv);

	MQTTAsync_setTraceCallback(trace_callback);

	info = MQTTAsync_getVersionInfo();

	while (info->name)
	{
	  MyLog(LOGA_INFO, "%s: %s", info->name, info->value);
	  info++;
	}

 	if (options.test_no == -1)
	{ /* run all the tests */
		for (options.test_no = 1; options.test_no < ARRAY_SIZE(tests); ++options.test_no)
		{
			failures = 0;
			MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
			rc += tests[options.test_no](options); /* return number of failures.  0 = test succeeded */
		}
	}
	else
	{
		MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
		rc = tests[options.test_no](options); /* run just the selected test */
	}

	if (failures == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	return rc;
}
