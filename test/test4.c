/*******************************************************************************
 * Copyright (c) 2009, 2024 IBM Corp., Ian Craggs
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
 *    Ian Craggs - MQTT 3.1.1 support
 *    Ian Craggs - test8 - failure callbacks
 *******************************************************************************/


/**
 * @file
 * Tests for the Paho Asynchronous MQTT C client
 */


#include "MQTTAsync.h"
#include <string.h>
#include <stdlib.h>

#if !defined(_WINDOWS)
	#include <sys/time.h>
  #include <sys/socket.h>
	#include <unistd.h>
  #include <errno.h>
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
	int MQTTVersion;
	int iterations;
} options =
{
	"localhost:1883",
	0,
	-1,
	10000,
	MQTTVERSION_DEFAULT,
	1,
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
		else if (strcmp(argv[count], "--MQTTversion") == 0)
		{
			if (++count < argc)
			{
				options.MQTTVersion = atoi(argv[count]);
				printf("setting MQTT version to %d\n", options.MQTTVersion);
			}
			else
				usage();
		}
		else if (strcmp(argv[count], "--iterations") == 0)
		{
			if (++count < argc)
				options.iterations = atoi(argv[count]);
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

#define assert(a, b, c, d) myassert(__FILE__, __LINE__, a, b, c, d)
#define assert1(a, b, c, d, e) myassert(__FILE__, __LINE__, a, b, c, d, e)

int tests = 0;
int failures = 0;
FILE* xml;
START_TIME_TYPE global_start_time;
char output[3000];
char* cur_output = output;

void write_test_result(void)
{
	long duration = elapsed(global_start_time);

	fprintf(xml, " time=\"%ld.%.3ld\" >\n", duration / 1000, duration % 1000);
	if (cur_output != output)
	{
		fprintf(xml, "%s", output);
		cur_output = output;
	}
	fprintf(xml, "</testcase>\n");
}

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

		cur_output += sprintf(cur_output, "<failure type=\"%s\">file %s, line %d </failure>\n",
                        description, filename, lineno);
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
		assert("Good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
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
	assert("Good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
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

	MyLog(LOGA_INFO, "Starting test 1 - asynchronous connect");
	fprintf(xml, "<testcase classname=\"test4\" name=\"asynchronous connect\"");
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, options.connection, "async_test",
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
	opts.MQTTVersion = options.MQTTVersion;

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = test1_onConnect;
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
			usleep(10000L);
		#endif

	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST1: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
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
	fprintf(xml, "<testcase classname=\"test4\" name=\"connect timeout\"");
	global_start_time = start_clock();

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
	opts.binarypwd.data = "testpassword";
	opts.binarypwd.len = (int)strlen(opts.binarypwd.data);
	opts.MQTTVersion = options.MQTTVersion;

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
	write_test_result();
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

	assert("Should have connected", 0, "%s failed to connect\n", cd->clientid);
	MyLog(LOGA_DEBUG, "In connect onFailure callback, \"%s\" rc %d\n", cd->clientid, response ? response->code : -999);
	if (response && response->message)
		MyLog(LOGA_DEBUG, "In connect onFailure callback, \"%s\"\n", response->message);

	test_finished++;
}


/*********************************************************************

Test3: More than one client object - simultaneous working.

*********************************************************************/
int test3(struct Options options)
{
	#define num_clients 10
	int subsqos = 2;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	int i;
	client_data clientdata[num_clients];

	test_finished = 0;
	MyLog(LOGA_INFO, "Starting test 3 - multiple connections");
	fprintf(xml, "<testcase classname=\"test4\" name=\"multiple connections\"");
	global_start_time = start_clock();

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
		opts.MQTTVersion = options.MQTTVersion;

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

//exit:
	MyLog(LOGA_INFO, "TEST3: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
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
	fprintf(xml, "<testcase classname=\"test4\" name=\"big messages\"");
	global_start_time = start_clock();

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
	opts.MQTTVersion = options.MQTTVersion;

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
	write_test_result();
	return failures;
}


void test5_onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	MyLog(LOGA_INFO, "Connack rc is %d", response ? response->code : -999);

	test_finished = 1;
}


void test5_onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test_finished = 1;
}


/********************************************************************

Test5: Connack return codes

*********************************************************************/
int test5(struct Options options)
{
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test1";

	test_finished = failures = 0;
	MyLog(LOGA_INFO, "Starting test 5 - connack return codes");
	fprintf(xml, "<testcase classname=\"test4\" name=\"connack return codes\"");
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, options.connection, "a clientid that is too long to be accepted",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test1_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.onSuccess = test5_onConnect;
	opts.onFailure = test5_onConnectFailure;
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
	MyLog(LOGA_INFO, "TEST5: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}


typedef struct
{
	MQTTAsync c;
	int should_fail;
} test6_client_info;

void test6_onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	test6_client_info cinfo = *(test6_client_info*)context;

	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	if (response)
		MyLog(LOGA_INFO, "Connack rc is %d", response->code);

	assert("Should fail to connect", cinfo.should_fail, "should_fail was %d\n", cinfo.should_fail);

	test_finished = 1;
}


void test6_onConnect(void* context, MQTTAsync_successData* response)
{
	test6_client_info cinfo = *(test6_client_info*)context;

	MyLog(LOGA_DEBUG, "In connect success callback, context %p", context);

	assert("Should connect correctly", !cinfo.should_fail, "should_fail was %d\n", cinfo.should_fail);

	test_finished = 1;
}


void test6_onDisconnect(void* context, MQTTAsync_successData* response)
{
	test6_client_info cinfo = *(test6_client_info*)context;

	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", cinfo.c);
	test_finished = 1;
}


/********************************************************************

Test6: HA connections

*********************************************************************/
int test6(struct Options options)
{
	int subsqos = 2;
	test6_client_info cinfo;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test1";
	char* uris[2] = {options.connection, options.connection};

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 6 - HA connections");
	fprintf(xml, "<testcase classname=\"test4\" name=\"HA connections\"");
	global_start_time = start_clock();

	test_finished = 0;
	cinfo.should_fail = 1; /* fail to connect */
	rc = MQTTAsync_create(&cinfo.c, "tcp://rubbish:1883", "async ha connection",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&cinfo.c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(cinfo.c, cinfo.c, NULL, test1_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.onSuccess = test6_onConnect;
	opts.onFailure = test6_onConnectFailure;
	opts.context = &cinfo;
	opts.MQTTVersion = options.MQTTVersion;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(cinfo.c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	test_finished = 0;
	cinfo.should_fail = 0; /* should connect through the serverURIs in connect options*/
	opts.onSuccess = test6_onConnect;
	opts.onFailure = test6_onConnectFailure;
	opts.context = &cinfo;
	opts.serverURIs = uris;
	opts.serverURIcount = 2;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(cinfo.c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	test_finished = 0;
	dopts.timeout = 0;
	dopts.onSuccess = test6_onDisconnect;
	dopts.context = &cinfo;
	MQTTAsync_disconnect(cinfo.c, &dopts);

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	MQTTAsync_destroy(&cinfo.c);

exit:
	MyLog(LOGA_INFO, "TEST6: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}



/********************************************************************

Test7: Persistence

*********************************************************************/

char* test7_topic = "C client test7";
int test7_messageCount = 0;

void test7_onDisconnectFailure(void* context, MQTTAsync_failureData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect failure callback %p", c);

	assert("Successful disconnect", 0, "disconnect failed", 0);

	test_finished = 1;
}

void test7_onDisconnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", c);
	test_finished = 1;
}


void test7_onUnsubscribe(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In onUnsubscribe onSuccess callback %p", c);
	opts.onSuccess = test7_onDisconnect;
	opts.context = c;
	opts.timeout = 1000;

	rc = MQTTAsync_disconnect(c, &opts);
	assert("Disconnect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


int test7_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;

	MyLog(LOGA_DEBUG, "Test7: received message id %d", message->msgid);

	test7_messageCount++;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


static int test7_subscribed = 0;

void test7_onSubscribe(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback %p granted qos %d", c, response->alt.qos);

	test7_subscribed = 1;
}


void test7_onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	opts.onSuccess = test7_onSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribe(c, test7_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished = 1;
}


void test7_onConnectOnly(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	dopts.context = context;
	dopts.timeout = 1000;
	dopts.onSuccess = test7_onDisconnect;
	rc = MQTTAsync_disconnect(c, &dopts);

	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished = 1;
}


/*********************************************************************

Test7: Pending tokens

*********************************************************************/
int test7(struct Options options)
{
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
	MQTTAsync_token* tokens = NULL;
	int msg_count = 6;

	MyLog(LOGA_INFO, "Starting test 7 - pending tokens");
	fprintf(xml, "<testcase classname=\"test4\" name=\"pending tokens\"");
	global_start_time = start_clock();
	test_finished = 0;

	rc = MQTTAsync_create(&c, options.connection, "async_test7",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test7_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.username = "testuser";
	opts.password = "testpassword";
	opts.MQTTVersion = options.MQTTVersion;

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;

	opts.onFailure = NULL;
	opts.context = c;

	opts.cleansession = 1;
	opts.onSuccess = test7_onConnectOnly;
	MyLog(LOGA_DEBUG, "Connecting to clean up");
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

	test_finished = 0;
	MyLog(LOGA_DEBUG, "Connecting");
	opts.cleansession = 0;
	opts.onSuccess = test7_onConnect;
	rc = MQTTAsync_connect(c, &opts);
	rc = 0;
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test7_subscribed)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.qos = 2;
	pubmsg.retained = 0;
	rc = MQTTAsync_send(c, test_topic, pubmsg.payloadlen, pubmsg.payload, pubmsg.qos, pubmsg.retained, &ropts);
	MyLog(LOGA_DEBUG, "Token was %d", ropts.token);
	rc = MQTTAsync_isComplete(c, ropts.token);
	/*assert("0 rc from isComplete", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);*/
	rc = MQTTAsync_waitForCompletion(c, ropts.token, 5000L);
	assert("Good rc from waitForCompletion", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	rc = MQTTAsync_isComplete(c, ropts.token);
	assert("1 rc from isComplete", rc == 1, "rc was %d", rc);

	test7_messageCount = 0;
	int i = 0;
	pubmsg.qos = 2;
	for (i = 0; i < msg_count; ++i)
	{
		pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		//pubmsg.qos = (pubmsg.qos == 2) ? 1 : 2;
		pubmsg.retained = 0;
		rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &ropts);
	}
	/* disconnect immediately without receiving the incoming messages */
	dopts.timeout = 0;
	dopts.onSuccess = test7_onDisconnect;
	dopts.context = c;
	MQTTAsync_disconnect(c, &dopts); /* now there should be "orphaned" publications */

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif
	test_finished = 0;

	rc = MQTTAsync_getPendingTokens(c, &tokens);
 	assert("getPendingTokens rc == 0", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	assert("should get some tokens back", tokens != NULL, "tokens was %p", tokens);
	MQTTAsync_free(tokens);

	MQTTAsync_destroy(&c); /* force re-reading persistence on create */

	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
	rc = MQTTAsync_create(&c, options.connection, "async_test7", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_getPendingTokens(c, &tokens);
	assert("getPendingTokens rc == 0", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	assert("should get some tokens back", tokens != NULL, "tokens was %p", tokens);
	if (tokens)
	{
		int i = 0;
		while (tokens[i] != -1)
			MyLog(LOGA_DEBUG, "Delivery token %d", tokens[i++]);
		MQTTAsync_free(tokens);
		//The following assertion should work, does with RSMB, but not Mosquitto
		//assert1("no of tokens should be count", i == msg_count, "no of tokens %d count %d", i, msg_count);
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test7_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Reconnecting");
	opts.context = c;
	if (MQTTAsync_connect(c, &opts) != 0)
	{
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		goto exit;
	}

	#if defined(_WIN32)
		Sleep(5000);
	#else
		usleep(5000000L);
	#endif

	rc = MQTTAsync_getPendingTokens(c, &tokens);
	assert("getPendingTokens rc == 0", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	/* assert("should get no tokens back", tokens == NULL, "tokens was %p", tokens);

	assert1("no of messages should be count", test7_messageCount == msg_count, "no of tokens %d count %d",
			test7_messageCount, msg_count);

	assertions fail against Mosquitto - needs testing */

	dopts.onFailure = test7_onDisconnectFailure;
	dopts.onSuccess = test7_onDisconnect;
	dopts.timeout = 1000;
	MQTTAsync_disconnect(c, &dopts);

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST7: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}



/*********************************************************************

Test8: Incomplete commands and requests

*********************************************************************/

char* test8_topic = "C client test8";
int test8_messageCount = 0;
int test8_subscribed = 0;
int test8_publishFailures = 0;

void test8_onPublish(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In publish onSuccess callback %p token %d", c, response->token);

}

void test8_onPublishFailure(void* context, MQTTAsync_failureData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onPublish failure callback %p", c);

	assert("Response code should be interrupted", response->code == MQTTASYNC_OPERATION_INCOMPLETE,
			"rc was %d", response->code);

	test8_publishFailures++;
}


void test8_onDisconnectFailure(void* context, MQTTAsync_failureData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect failure callback %p", c);

	assert("Successful disconnect", 0, "disconnect failed", 0);

	test_finished = 1;
}


void test8_onDisconnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", c);
	test_finished = 1;
}


void test8_onSubscribe(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback %p granted qos %d", c, response->alt.qos);

	test8_subscribed = 1;
}


void test8_onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	opts.onSuccess = test8_onSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribe(c, test8_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished = 1;
}

int test8_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;

	MyLog(LOGA_DEBUG, "Test8: received message id %d", message->msgid);

	test8_messageCount++;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


int test8(struct Options options)
{
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	int rc = 0;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
	MQTTAsync_token* tokens = NULL;
	int msg_count = 6;

	MyLog(LOGA_INFO, "Starting test 8 - incomplete commands");
	fprintf(xml, "<testcase classname=\"test4\" name=\"incomplete commands\"");
	global_start_time = start_clock();
	test_finished = 0;

	rc = MQTTAsync_create(&c, options.connection, "async_test8",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test8_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.username = "testuser";
	opts.password = "testpassword";
	opts.MQTTVersion = options.MQTTVersion;

	opts.onFailure = NULL;
	opts.context = c;

	MyLog(LOGA_DEBUG, "Connecting");
	opts.cleansession = 1;
	opts.onSuccess = test8_onConnect;
	rc = MQTTAsync_connect(c, &opts);
	rc = 0;
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test8_subscribed)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	int i = 0;
	pubmsg.qos = 2;
	ropts.onSuccess = test8_onPublish;
	ropts.onFailure = test8_onPublishFailure;
	ropts.context = c;
	for (i = 0; i < msg_count; ++i)
	{
		pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.qos = (pubmsg.qos == 2) ? 1 : 2; /* alternate */
		pubmsg.retained = 0;
		rc = MQTTAsync_sendMessage(c, test8_topic, &pubmsg, &ropts);
		assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}
	/* disconnect immediately without completing the commands */
	dopts.timeout = 0;
	dopts.onSuccess = test8_onDisconnect;
	dopts.context = c;
	rc = MQTTAsync_disconnect(c, &dopts); /* now there should be incomplete commands */
	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif
	test_finished = 0;

	rc = MQTTAsync_getPendingTokens(c, &tokens);
 	assert("getPendingTokens rc == 0", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	assert("should get no tokens back", tokens == NULL, "tokens was %p", tokens);

	assert("test8_publishFailures > 0", test8_publishFailures > 0,
		   "test8_publishFailures = %d", test8_publishFailures);

	/* Now elicit failure callbacks on destroy */

	test8_subscribed = test8_publishFailures = 0;

	MyLog(LOGA_DEBUG, "Connecting");
	opts.cleansession = 0;
	opts.onSuccess = test8_onConnect;
	rc = MQTTAsync_connect(c, &opts);
	rc = 0;
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test8_subscribed)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	i = 0;
	pubmsg.qos = 2;
	ropts.onSuccess = test8_onPublish;
	ropts.onFailure = test8_onPublishFailure;
	ropts.context = c;
	for (i = 0; i < msg_count; ++i)
	{
		pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.qos = (pubmsg.qos == 2) ? 1 : 2; /* alternate */
		pubmsg.retained = 0;
		rc = MQTTAsync_sendMessage(c, test8_topic, &pubmsg, &ropts);
		assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}
	/* disconnect immediately without completing the commands */
	dopts.timeout = 0;
	dopts.onSuccess = test8_onDisconnect;
	dopts.context = c;
	rc = MQTTAsync_disconnect(c, &dopts); /* now there should be incomplete commands */
	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	while (!test_finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif
	test_finished = 0;

	rc = MQTTAsync_getPendingTokens(c, &tokens);
	assert("getPendingTokens rc == 0", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	assert("should get some tokens back", tokens != NULL, "tokens was %p", tokens);
	MQTTAsync_free(tokens);

	assert("test8_publishFailures == 0", test8_publishFailures == 0,
		   "test8_publishFailures = %d", test8_publishFailures);

	MQTTAsync_destroy(&c);

	assert("test8_publishFailures > 0", test8_publishFailures > 0,
		   "test8_publishFailures = %d", test8_publishFailures);

	/* cleanup persistence of any left over message data*/

	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
	rc = MQTTAsync_create(&c, options.connection, "async_test8",
				MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	test8_subscribed = 0;
	opts.cleansession = 1;
	opts.context = c;

	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test8_subscribed)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	test_finished = 0;
	dopts.context = c;
	rc = MQTTAsync_disconnect(c, &dopts);
	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	while (!test_finished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST8: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}



void trace_callback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
	printf("Trace : %d, %s\n", level, message);
}




int main(int argc, char** argv)
{
	int rc = 0;
 	int (*tests[])(struct Options) = {NULL, test1, test2, test3, test4, test5, test6, test7, test8}; /* indexed starting from 1 */
	MQTTAsync_nameValue* info;
	int i;

	xml = fopen("TEST-test4.xml", "w");
	fprintf(xml, "<testsuite name=\"test4\" tests=\"%d\">\n", (int)(ARRAY_SIZE(tests)) - 1);

	getopts(argc, argv);

	MQTTAsync_setTraceCallback(trace_callback);

	info = MQTTAsync_getVersionInfo();
	while (info->name)
	{
	  MyLog(LOGA_INFO, "%s: %s", info->name, info->value);
	  info++;
	}

	for (i = 0; i < options.iterations; ++i)
	{
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
	}

	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	fprintf(xml, "</testsuite>\n");
	fclose(xml);

	return rc;
}
