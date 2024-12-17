/*******************************************************************************
 * Copyright (c) 2009, 2020 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    https://www.eclipse.org/legal/epl-v20.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php
 *
 * Contributors:
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *    Ian Craggs - MQTT 3.1.1 support
 *    Ian Craggs - test8 - failure callbacks
 *    Ian Craggs - MQTT V5
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
	MQTTVERSION_5,
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


void MySleep(long milliseconds)
{
#if defined(_WIN32) || defined(_WIN64)
	Sleep(milliseconds);
#else
	usleep(milliseconds*1000);
#endif
}


#if defined(_WIN32) || defined(_WINDOWS)
#define START_TIME_TYPE DWORD
static DWORD start_time = 0;
START_TIME_TYPE start_clock(void)
{
	return GetTickCount();
}
#elif defined(AIX)
#define START_TIME_TYPE struct timespec
START_TIME_TYPE start_clock(void)
{
	static struct timespec start;
	clock_gettime(CLOCK_REALTIME, &start);
	return start;
}
#else
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


void logProperties(MQTTProperties *props)
{
	int i = 0;

	for (i = 0; i < props->count; ++i)
	{
		int id = props->array[i].identifier;
		const char* name = MQTTPropertyName(id);
		char* intformat = "Property name %s value %d";

		switch (MQTTProperty_getType(id))
		{
		case MQTTPROPERTY_TYPE_BYTE:
		  MyLog(LOGA_INFO, intformat, name, props->array[i].value.byte);
		  break;
		case MQTTPROPERTY_TYPE_TWO_BYTE_INTEGER:
		  MyLog(LOGA_INFO, intformat, name, props->array[i].value.integer2);
		  break;
		case MQTTPROPERTY_TYPE_FOUR_BYTE_INTEGER:
		  MyLog(LOGA_INFO, intformat, name, props->array[i].value.integer4);
		  break;
		case MQTTPROPERTY_TYPE_VARIABLE_BYTE_INTEGER:
		  MyLog(LOGA_INFO, intformat, name, props->array[i].value.integer4);
		  break;
		case MQTTPROPERTY_TYPE_BINARY_DATA:
		case MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING:
		  MyLog(LOGA_INFO, "Property name %s value len %.*s", name,
				  props->array[i].value.data.len, props->array[i].value.data.data);
		  break;
		case MQTTPROPERTY_TYPE_UTF_8_STRING_PAIR:
		  MyLog(LOGA_INFO, "Property name %s key %.*s value %.*s", name,
			  props->array[i].value.data.len, props->array[i].value.data.data,
		  	  props->array[i].value.value.len, props->array[i].value.value.data);
		  break;
		}
	}
}


void waitForNoPendingTokens(MQTTAsync c)
{
	int i = 0, rc = 0, count = 0;
	MQTTAsync_token *tokens;

	/* acks for outgoing messages could arrive after incoming exchanges are complete */
	do
	{
		rc = MQTTAsync_getPendingTokens(c, &tokens);
		assert("Good rc from getPendingTokens", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
		i = 0;
		if (tokens)
		{
			while (tokens[i] != -1)
				++i;
			MQTTAsync_free(tokens);
		}
		if (i > 0)
			MySleep(100);
	}
	while (i > 0 && ++count < 100);
	assert("Number of getPendingTokens should be 0", i == 0, "i was %d ", i);
}


volatile int test_finished = 0;

char* test_topic = "async test topic";


void test1_onDisconnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", c);
	test_finished = 1;
}


void test1_onUnsubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	MQTTProperty property;
	int rc;

	MyLog(LOGA_DEBUG, "In onUnsubscribe onSuccess callback %p", c);
	MyLog(LOGA_INFO, "Unsuback properties:");
	logProperties(&response->properties);
	assert("A property should exist", response->properties.count > 0,
			"Property count was %d\n", response->properties);

	opts.onSuccess = test1_onDisconnect;
	opts.context = c;
	opts.reasonCode = MQTTREASONCODE_UNSPECIFIED_ERROR;

	property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
	property.value.integer4 = 0;
	MQTTProperties_add(&opts.properties, &property);

	rc = MQTTAsync_disconnect(c, &opts);
	assert("Disconnect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	MQTTProperties_free(&opts.properties);
}


int test1_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;
	int rc;

	MyLog(LOGA_DEBUG, "In messageArrived callback %p", c);

	assert("Message structure version should be 1", message->struct_version == 1,
			"message->struct_version was %d", message->struct_version);
	if (message->struct_version == 1)
	{
		assert("Properties count should be 2", message->properties.count == 2,
			"Properties count was %d\n", message->properties.count);
		logProperties(&message->properties);
	}

	if (++message_count == 1)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		MQTTAsync_callOptions opts = MQTTAsync_callOptions_initializer;
		MQTTProperty property;
		MQTTProperties props = MQTTProperties_initializer;

		property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
		property.value.data.data = "test user property";
		property.value.data.len = (int)strlen(property.value.data.data);
		property.value.value.data = "test user property value";
		property.value.value.len = (int)strlen(property.value.value.data);
		MQTTProperties_add(&props, &property);
		pubmsg.properties = props;

		pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.qos = 2;
		pubmsg.retained = 0;
		rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
		assert("Publish should return 0", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		MQTTProperties_free(&props);
	}
	else
	{
		MQTTProperty property;
		MQTTProperties props = MQTTProperties_initializer;
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
		property.value.data.data = "test user property";
		property.value.data.len = (int)strlen(property.value.data.data);
		property.value.value.data = "test user property value";
		property.value.value.len = (int)strlen(property.value.value.data);
		MQTTProperties_add(&props, &property);
		opts.properties = props;

		opts.onSuccess5 = test1_onUnsubscribe;
		opts.context = c;
		rc = MQTTAsync_unsubscribe(c, test_topic, &opts);
		assert("Unsubscribe successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		MQTTProperties_free(&props);
	}

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}

void test1_onSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;
	MQTTProperty property;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTAsync_callOptions opts = MQTTAsync_callOptions_initializer;

	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback %p granted qos %d", c, response->reasonCode);
	assert("Subscribe response should be 2", response->reasonCode == MQTTREASONCODE_GRANTED_QOS_2,
			"response was %d", response->reasonCode);

	MyLog(LOGA_INFO, "Suback properties:");
	logProperties(&response->properties);

	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "test user property";
	property.value.data.len = (int)strlen(property.value.data.data);
	property.value.value.data = "test user property value";
	property.value.value.len = (int)strlen(property.value.value.data);
	MQTTProperties_add(&props, &property);
	opts.properties = props;

	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.qos = 2;
	pubmsg.retained = 0;

	rc = MQTTAsync_send(c, test_topic, pubmsg.payloadlen, pubmsg.payload, pubmsg.qos, pubmsg.retained, &opts);
	MQTTProperties_free(&props);
}


void test1_onConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTProperty property;
	MQTTProperties props = MQTTProperties_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_SUCCESS,
		   "Reason code was %d\n", response->reasonCode);

	MyLog(LOGA_INFO, "Connack properties:");
	logProperties(&response->properties);

	opts.onSuccess5 = test1_onSubscribe;
	opts.context = c;

	property.identifier = MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER;
	property.value.integer4 = 33;
	MQTTProperties_add(&props, &property);
	opts.properties = props;

	opts.subscribeOptions.retainAsPublished = 1;

	rc = MQTTAsync_subscribe(c, test_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished = 1;
	MQTTProperties_free(&props);
}


/*********************************************************************

Test1: Basic connect, subscribe send and receive.

*********************************************************************/
int test1(struct Options options)
{
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperties willProps = MQTTProperties_initializer;
	MQTTProperty property;
	int rc = 0;
	char* test_topic = "V5 C client test1";
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;

	MyLog(LOGA_INFO, "Starting V5 test 1 - asynchronous connect");
	fprintf(xml, "<testcase classname=\"test45\" name=\"asynchronous connect\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "async_test",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test1_messageArrived, NULL);
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
	opts.onSuccess5 = test1_onConnect;
	opts.onFailure5 = NULL;
	opts.context = c;

	property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
	property.value.integer4 = 30;
	MQTTProperties_add(&props, &property);

	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "test user property";
	property.value.data.len = (int)strlen(property.value.data.data);
	property.value.value.data = "test user property value";
	property.value.value.len = (int)strlen(property.value.value.data);
	MQTTProperties_add(&props, &property);

	opts.connectProperties = &props;
	opts.willProperties = &willProps;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		MySleep(100);

	MQTTProperties_free(&props);
	MQTTProperties_free(&willProps);
	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST1: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}

int test2_onFailure_called = 0;

void test2_onFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test2_onFailure_called++;
	test_finished = 1;
}


void test2_onConnect(void* context, MQTTAsync_successData5* response)
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
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test2";
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;

	test_finished = 0;

	MyLog(LOGA_INFO, "Starting test 2 - connect timeout");
	fprintf(xml, "<testcase classname=\"test4\" name=\"connect timeout\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, "tcp://9.20.96.160:66", "connect timeout",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
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
	opts.onSuccess5 = test2_onConnect;
	opts.onFailure5 = test2_onFailure;
	opts.context = c;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		MySleep(100);

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


void test3_onDisconnect(void* context, MQTTAsync_successData5* response)
{
	client_data* cd = (client_data*)context;
	MyLog(LOGA_DEBUG, "In onDisconnect callback for client \"%s\"", cd->clientid);
	test_finished++;
}


void test3_onPublish(void* context,  MQTTAsync_successData5* response)
{
	client_data* cd = (client_data*)context;
	MyLog(LOGA_DEBUG, "In QoS 0 onPublish callback for client \"%s\"", cd->clientid);
}


void test3_onUnsubscribe(void* context, MQTTAsync_successData5* response)
{
	client_data* cd = (client_data*)context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In onUnsubscribe onSuccess callback \"%s\"", cd->clientid);
	opts.onSuccess5 = test3_onDisconnect;
	opts.context = cd;

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
		opts.onSuccess5 = test3_onPublish;

		rc = MQTTAsync_sendMessage(cd->c, cd->test_topic, &pubmsg, &opts);
		assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}
	else
	{
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		opts.onSuccess5 = test3_onUnsubscribe;
		opts.context = cd;
		rc = MQTTAsync_unsubscribe(cd->c, cd->test_topic, &opts);
		assert("Unsubscribe successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}
	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);
	return 1;
}

void test3_onSubscribe(void* context, MQTTAsync_successData5* response)
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


void test3_onConnect(void* context, MQTTAsync_successData5* response)
{
	client_data* cd = (client_data*)context;
	MQTTAsync_callOptions opts = MQTTAsync_callOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, \"%s\"", cd->clientid);
	opts.onSuccess5 = test3_onSubscribe;
	opts.context = cd;

	rc = MQTTAsync_subscribe(cd->c, cd->test_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished++;
}


void test3_onFailure(void* context, MQTTAsync_failureData5* response)
{
	client_data* cd = (client_data*)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

	assert("Should have connected", 0, "%s failed to connect\n", cd->clientid);
	MyLog(LOGA_DEBUG, "In connect onFailure callback, \"%s\" rc %d\n", cd->clientid, response ? response->reasonCode : -999);
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
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	int i;
	client_data clientdata[num_clients];
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;

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

		createOpts.MQTTVersion = MQTTVERSION_5;
		rc = MQTTAsync_createWithOptions(&(clientdata[i].c), options.connection, clientdata[i].clientid,
			MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
		assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);

		rc = MQTTAsync_setCallbacks(clientdata[i].c, &clientdata[i], NULL, test3_messageArrived, NULL);
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
		opts.onSuccess5 = test3_onConnect;
		opts.onFailure5 = test3_onFailure;
		opts.context = &clientdata[i];

		MyLog(LOGA_DEBUG, "Connecting");
		rc = MQTTAsync_connect(clientdata[i].c, &opts);
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}

	while (test_finished < num_clients)
	{
		MyLog(LOGA_DEBUG, "num_clients %d test_finished %d\n", num_clients, test_finished);
		MySleep(100);
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

void test4_onPublish(void* context, MQTTAsync_successData5* response)
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
		MQTTAsync_callOptions opts = MQTTAsync_callOptions_initializer;

		pubmsg.payload = test4_payload;
		pubmsg.payloadlen = test4_payloadlen;
		pubmsg.qos = 1;
		pubmsg.retained = 0;
		opts.onSuccess5 = test4_onPublish;
		opts.context = c;

		rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
	}
	else if (message_count == 2)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		MQTTAsync_callOptions opts = MQTTAsync_callOptions_initializer;

		pubmsg.payload = test4_payload;
		pubmsg.payloadlen = test4_payloadlen;
		pubmsg.qos = 0;
		pubmsg.retained = 0;
		opts.onSuccess5 = test4_onPublish;
		opts.context = c;
		rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
	}
	else
	{
		MQTTAsync_callOptions opts = MQTTAsync_callOptions_initializer;
		MQTTProperties props = MQTTProperties_initializer;
		MQTTProperty property;

		opts.onSuccess5 = test1_onUnsubscribe;
		opts.context = c;

		property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
		property.value.data.data = "test user property";
		property.value.data.len = (int)strlen(property.value.data.data);
		property.value.value.data = "test user property value";
		property.value.value.len = (int)strlen(property.value.value.data);
		MQTTProperties_add(&props, &property);

		opts.properties = props;
		rc = MQTTAsync_unsubscribe(c, test_topic, &opts);
		assert("Unsubscribe successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		MQTTProperties_free(&props);
	}

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}

int test4_packet_size = 10000;

#if !defined(min)
#define min(a, b) ((a < b) ? a : b)
#endif

void test4_onSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc, i;
	int max_packet_size = min(test4_packet_size, options.size);

	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback %p", c);

	pubmsg.payload = test4_payload = malloc(options.size);
	pubmsg.payloadlen = test4_payloadlen = options.size;

	MyLog(LOGA_INFO, "Max packet size %d", max_packet_size);
	srand(33);
	for (i = 0; i < (max_packet_size-100); ++i)
		((char*)pubmsg.payload)[i] = rand() % 256;

	pubmsg.qos = 2;
	pubmsg.retained = 0;

	rc = MQTTAsync_send(c, test_topic, pubmsg.payloadlen, pubmsg.payload, pubmsg.qos, pubmsg.retained, NULL);
}


void test4_onConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_callOptions opts = MQTTAsync_callOptions_initializer;
	int rc;

	test4_packet_size = MQTTProperties_getNumericValue(&response->properties, MQTTPROPERTY_CODE_MAXIMUM_PACKET_SIZE);

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	opts.onSuccess5 = test4_onSubscribe;
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
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test4";
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;

	test_finished = failures = 0;
	MyLog(LOGA_INFO, "Starting test 4 - big messages");
	fprintf(xml, "<testcase classname=\"test4\" name=\"big messages\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "async_test_4",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test4_messageArrived, NULL);
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
	opts.onSuccess5 = test4_onConnect;
	opts.onFailure5 = NULL;
	opts.context = c;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		MySleep(100);

	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST4: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}


void test5_onConnectFailure(void* context, MQTTAsync_failureData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	MyLog(LOGA_INFO, "Connack rc is %d", response ? response->reasonCode : -999);

	test_finished = 1;
}


void test5_onConnect(void* context, MQTTAsync_successData5* response)
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
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test1";
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;

	test_finished = failures = 0;
	MyLog(LOGA_INFO, "Starting test 5 - connack return codes");
	fprintf(xml, "<testcase classname=\"test45\" name=\"connack return codes\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "a clientid that is too long to be accepted",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test1_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.onSuccess5 = test5_onConnect;
	opts.onFailure5 = test5_onConnectFailure;
	opts.context = c;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		MySleep(100);

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

void test6_onConnectFailure(void* context, MQTTAsync_failureData5* response)
{
	test6_client_info cinfo = *(test6_client_info*)context;

	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	if (response)
		MyLog(LOGA_INFO, "Connack rc is %d", response->reasonCode);

	assert("Should fail to connect", cinfo.should_fail, "should_fail was %d", cinfo.should_fail);

	test_finished = 1;
}


void test6_onConnect(void* context, MQTTAsync_successData5* response)
{
	test6_client_info cinfo = *(test6_client_info*)context;

	MyLog(LOGA_DEBUG, "In connect success callback, context %p", context);

	assert("Should connect correctly", !cinfo.should_fail, "should_fail was %d", cinfo.should_fail);

	test_finished = 1;
}


void test6_onDisconnect5(void* context, MQTTAsync_successData5* response)
{
	test6_client_info cinfo = *(test6_client_info*)context;

	MyLog(LOGA_DEBUG, "In onDisconnect5 callback %p", cinfo.c);
	test_finished = 1;
}


/********************************************************************

Test6: HA connections

*********************************************************************/
int test6(struct Options options)
{
	int subsqos = 2;
	test6_client_info cinfo;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test1";
	char* uris[2] = {options.connection, options.connection};
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 6 - HA connections");
	fprintf(xml, "<testcase classname=\"test4\" name=\"HA connections\"");
	global_start_time = start_clock();

	test_finished = 0;
	cinfo.should_fail = 1; /* fail to connect */
	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&cinfo.c, "tcp://rubbish:1883", "async ha connection",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&cinfo.c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(cinfo.c, cinfo.c, NULL, test1_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.onSuccess5 = test6_onConnect;
	opts.onFailure5 = test6_onConnectFailure;
	opts.context = &cinfo;
	opts.MQTTVersion = options.MQTTVersion;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(cinfo.c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		MySleep(100);

	test_finished = 0;
	cinfo.should_fail = 0; /* should connect through the serverURIs in connect options*/
	opts.serverURIs = uris;
	opts.serverURIcount = 2;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(cinfo.c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test_finished)
		MySleep(100);

	test_finished = 0;
	dopts.timeout = 0;
	dopts.onSuccess5 = test6_onDisconnect5;
	dopts.context = cinfo.c;
	dopts.timeout = 0;
	MQTTAsync_disconnect(cinfo.c, &dopts);

	while (!test_finished)
		MySleep(100);

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


void test7_onDisconnectFailure5(void* context, MQTTAsync_failureData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect5 failure callback %p", c);

	assert("Successful disconnect", 0, "disconnect failed", 0);

	test_finished = 1;
}


void test7_onDisconnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", c);
	test_finished = 1;
}


void test7_onDisconnect5(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect5 callback %p", c);
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

	rc = MQTTAsync_disconnect(c, &opts);
	assert("Disconnect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


void test7_onUnsubscribe5(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In onUnsubscribe onSuccess5 callback %p", c);
	opts.onSuccess5 = test7_onDisconnect5;
	opts.context = c;

	rc = MQTTAsync_disconnect(c, &opts);
	assert("Disconnect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


int test7_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;

	MyLog(LOGA_DEBUG, "Test7: received message id %d, %.*s", message->msgid,
			message->payloadlen, message->payload);

	test7_messageCount++;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


static int test7_subscribed = 0;

void test7_onSubscribe5(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In subscribe onSuccess5 callback %p granted qos %d", c, response->reasonCode);

	test7_subscribed = 1;
}


void test7_onSubscribe(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback %p granted qos %d", c,
			response->alt.qos);

	test7_subscribed = 1;
}

static int test7_just_connect = 0;

void test7_onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);

	if (test7_just_connect == 1)
	{
		test7_just_connect = 2;
		return;
	}

	opts.onSuccess = test7_onSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribe(c, test7_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished = 1;
}


void test7_onConnect5(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess5 callback, context %p", context);

	if (test7_just_connect == 1)
	{ /* we don't need to subscribe if we reconnected */
		test7_subscribed = 1;
		return;
	}

	opts.onSuccess5 = test7_onSubscribe5;
	opts.context = c;

	rc = MQTTAsync_subscribe(c, test7_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished = 1;
}


/*********************************************************************

Test7: Pending tokens

*********************************************************************/
int test7_run(int qos, int start_mqtt_version, int restore_mqtt_version)
{
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
	MQTTAsync_token* tokens = NULL;
	int msg_count = 6;
	MQTTProperty property;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	int i = 0;

	MyLog(LOGA_INFO, "Starting test 7 - persistence, qos %d, MQTT versions: %s then %s", qos,
			(start_mqtt_version == MQTTVERSION_5) ? "5" : "3.1.1",
			(restore_mqtt_version == MQTTVERSION_5) ? "5" : "3.1.1");

	fprintf(xml, "<testcase classname=\"test45\" name=\"pending tokens\"");
	global_start_time = start_clock();
	test_finished = 0;

	createOpts.MQTTVersion = start_mqtt_version;
	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
	rc = MQTTAsync_createWithOptions(&c, options.connection, "async_test7",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
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
	opts.MQTTVersion = start_mqtt_version;

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.context = c;

	/* connect clean and then leave messages lying around */
	test_finished = 0;
	test7_subscribed = 0;
	MyLog(LOGA_DEBUG, "Connecting");

	if (start_mqtt_version == MQTTVERSION_5)
	{
		opts.cleanstart = 1;
		test7_just_connect = 0;
		opts.connectProperties = &props;
		property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
		property.value.integer4 = 999999;
		MQTTProperties_add(opts.connectProperties, &property);
		opts.onSuccess5 = test7_onConnect5;
		opts.onFailure5 = NULL;
		rc = MQTTAsync_connect(c, &opts);
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		MQTTProperties_free(opts.connectProperties);
		if (rc != MQTTASYNC_SUCCESS)
			goto exit;
	}
	else
	{	/* MQTT 3 version */
		opts.cleanstart = 0;
		opts.cleansession = 1; /* clean up */
		test7_just_connect = 1;
		opts.onSuccess = test7_onConnect;
		opts.onFailure = NULL;
		rc = MQTTAsync_connect(c, &opts);
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		if (rc != MQTTASYNC_SUCCESS)
			goto exit;
		while (test7_just_connect < 2)
			MySleep(100);
		test_finished = 0;
		dopts.onSuccess5 = NULL;
		dopts.onFailure5 = NULL;
		dopts.onFailure = test7_onDisconnectFailure;
		dopts.onSuccess = test7_onDisconnect;
		rc = MQTTAsync_disconnect(c, &dopts);
		assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		while (!test_finished)
			MySleep(100);
		test_finished = 0;
		opts.cleansession = 0; /* now it's clean */
		test7_just_connect = 0;
		rc = MQTTAsync_connect(c, &opts);
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		if (rc != MQTTASYNC_SUCCESS)
			goto exit;
	}

	while (!test7_subscribed)
		MySleep(100);

	test7_messageCount = 0;
	pubmsg.payload = "first test of a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.qos = qos;
	pubmsg.retained = 0;
	rc = MQTTAsync_send(c, test7_topic, pubmsg.payloadlen, pubmsg.payload, pubmsg.qos, pubmsg.retained, &ropts);
	assert("Good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	MyLog(LOGA_DEBUG, "Token was %d", ropts.token);
	rc = MQTTAsync_waitForCompletion(c, ropts.token, 5000L);
	assert("Good rc from waitForCompletion", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	rc = MQTTAsync_isComplete(c, ropts.token);
	assert("1 rc from isComplete", rc == 1, "rc was %d", rc);

	i = 0;
	pubmsg.qos = qos;
	for (i = 0; i < msg_count; ++i)
	{
		char buf[100];
		sprintf(buf, "%d a much longer message that we can shorten to the extent that we need", i);
		pubmsg.payload = buf;
		pubmsg.payloadlen = 11 + i;
		pubmsg.retained = 0;
		rc = MQTTAsync_sendMessage(c, test7_topic, &pubmsg, &ropts);
	}
	/* disconnect immediately without receiving the incoming messages */
	dopts.timeout = 0;
	if (start_mqtt_version == MQTTVERSION_5)
		dopts.onSuccess5 = test7_onDisconnect5;
	else
		dopts.onSuccess = test7_onDisconnect;
	dopts.context = c;
	dopts.timeout = 0;
	MQTTAsync_disconnect(c, &dopts); /* now there should be "orphaned" publications */

	while (!test_finished)
		MySleep(100);
	test_finished = 0;

	rc = MQTTAsync_getPendingTokens(c, &tokens);
 	assert("getPendingTokens rc == 0", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	assert("should get some tokens back", tokens != NULL, "tokens was %p", tokens);
	if (tokens)
		MQTTAsync_free(tokens);
	MQTTProperties_free(opts.connectProperties);

	MQTTAsync_destroy(&c); /* force re-reading persistence on create */

	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
	createOpts.MQTTVersion = restore_mqtt_version;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "async_test7",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);

	if (start_mqtt_version == MQTTVERSION_5 && restore_mqtt_version == MQTTVERSION_3_1_1)
		assert("Persistence error from create", rc == MQTTASYNC_PERSISTENCE_ERROR, "rc was %d", rc);
	else
		assert("Good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		//MQTTAsync_destroy(&c);
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
		assert("no of tokens should be > 0", i > 0, "no of tokens %d", i);
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test7_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Reconnecting");
	opts.context = c;
	opts.MQTTVersion = restore_mqtt_version;
	if (restore_mqtt_version == MQTTVERSION_5)
	{
		opts.cleanstart = opts.cleansession = 0;
		test7_just_connect = 1;
		opts.connectProperties = &props;
		property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
		property.value.integer4 = 0; /* clean up at end of test */
		MQTTProperties_add(opts.connectProperties, &property);
		opts.onSuccess = NULL;
		opts.onFailure = NULL;
		opts.onSuccess5 = test7_onConnect5;
		opts.onFailure5 = NULL;
		rc = MQTTAsync_connect(c, &opts);
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		MQTTProperties_free(opts.connectProperties);
		if (rc != MQTTASYNC_SUCCESS)
			goto exit;
	}
	else
	{	/* MQTT 3 version */
		opts.cleanstart = opts.cleansession = 0;
		test7_just_connect = 1;
		opts.onSuccess5 = NULL;
		opts.onFailure5 = NULL;
		opts.onSuccess = test7_onConnect;
		opts.onFailure = NULL;
		rc = MQTTAsync_connect(c, &opts);
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		if (rc != MQTTASYNC_SUCCESS)
			goto exit;
	}

	waitForNoPendingTokens(c);

	if (qos == 2)
		assert("no of messages should be count", test7_messageCount == msg_count + 1,
				"messages received %d\n", test7_messageCount);
	else if (qos == 1)
		assert("no of messages should be at least count", test7_messageCount >= msg_count + 1,
				"messages received %d\n", test7_messageCount);

	if (restore_mqtt_version == MQTTVERSION_5)
	{
		dopts.onFailure5 = test7_onDisconnectFailure5;
		dopts.onSuccess5 = test7_onDisconnect5;
	}
	else
	{
		dopts.onFailure = test7_onDisconnectFailure;
		dopts.onSuccess = test7_onDisconnect;
	}
	dopts.timeout = 300;
	rc = MQTTAsync_disconnect(c, &dopts);
	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	i = 0;
	while (!test_finished && ++i < 20)
		MySleep(100);
	assert("Test finished should be true", test_finished,
		   "test_finished was %d", test_finished);

	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST7: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}


int test7(struct Options options)
{
	int rc = 0;
	fprintf(xml, "<testcase classname=\"test7\" name=\"persistence\"");
	global_start_time = start_clock();
	rc = test7_run(1, MQTTVERSION_5, MQTTVERSION_5) +
		 test7_run(2, MQTTVERSION_5, MQTTVERSION_5) +
		 test7_run(1, MQTTVERSION_3_1_1, MQTTVERSION_3_1_1) +
		 test7_run(2, MQTTVERSION_3_1_1, MQTTVERSION_3_1_1) /*+
		 test7_run(2, MQTTVERSION_3_1_1, MQTTVERSION_5) +
		 test7_run(2, MQTTVERSION_5, MQTTVERSION_3_1_1)*/;
	fprintf(xml, " time=\"%ld\" >\n", elapsed(global_start_time) / 1000);
	if (cur_output != output)
	{
		fprintf(xml, "%s", output);
		cur_output = output;
	}
	fprintf(xml, "</testcase>\n");
	return rc;
}



/*********************************************************************

Test8: Incomplete commands and requests

*********************************************************************/

char* test8_topic = "C client test8";
int test8_messageCount = 0;
int test8_subscribed = 0;
int test8_publishFailures = 0;

void test8_onPublish(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In publish onSuccess callback %p token %d", c, response->token);

}

void test8_onPublishFailure(void* context, MQTTAsync_failureData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onPublish failure callback %p", c);

	assert("Response code should be interrupted", response->code == MQTTASYNC_OPERATION_INCOMPLETE,
			"rc was %d", response->code);

	test8_publishFailures++;
}


void test8_onDisconnectFailure(void* context, MQTTAsync_failureData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect failure callback %p", c);

	assert("Successful disconnect", 0, "disconnect failed", 0);

	test_finished = 1;
}


void test8_onDisconnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", c);
	test_finished = 1;
}


void test8_onSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback %p granted qos %d", c, response->reasonCode);

	test8_subscribed = 1;
}


void test8_onConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	opts.onSuccess5 = test8_onSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribe(c, test8_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
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
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	int rc = 0;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
	MQTTAsync_token* tokens = NULL;
	int msg_count = 6;
	MQTTProperty property;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;

	MyLog(LOGA_INFO, "Starting test 8 - incomplete commands");
	fprintf(xml, "<testcase classname=\"test4\" name=\"incomplete commands\"");
	global_start_time = start_clock();
	test_finished = 0;

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "async_test8",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
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
	opts.cleanstart = 1;
	opts.onSuccess5 = test8_onConnect;
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test8_subscribed)
		MySleep(100);

	int i = 0;
	pubmsg.qos = 2;
	ropts.onSuccess5 = test8_onPublish;
	ropts.onFailure5 = test8_onPublishFailure;
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
	dopts.onSuccess5 = test8_onDisconnect;
	dopts.context = c;
	rc = MQTTAsync_disconnect(c, &dopts); /* now there should be incomplete commands */
	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	while (!test_finished)
		MySleep(100);
	test_finished = 0;

	waitForNoPendingTokens(c);

	assert("test8_publishFailures > 0", test8_publishFailures > 0,
		   "test8_publishFailures = %d", test8_publishFailures);

	/* Now elicit failure callbacks on destroy */

	test8_subscribed = test8_publishFailures = 0;

	MyLog(LOGA_DEBUG, "Connecting");
	opts.onSuccess5 = test8_onConnect;

	property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
	property.value.integer4 = 30;
	MQTTProperties_add(&props, &property);

	opts.connectProperties = &props;

	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test8_subscribed)
		MySleep(100);

	i = 0;
	pubmsg.qos = 2;
	ropts.onSuccess5 = test8_onPublish;
	ropts.onFailure5 = test8_onPublishFailure;
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
	dopts.onSuccess5 = test8_onDisconnect;
	dopts.context = c;
	rc = MQTTAsync_disconnect(c, &dopts); /* now there should be incomplete commands */
	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	while (!test_finished)
		MySleep(100);
	test_finished = 0;

	rc = MQTTAsync_getPendingTokens(c, &tokens);
	assert("getPendingTokens rc == 0", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	assert("should get some tokens back", tokens != NULL, "tokens was %p", tokens);
	MQTTAsync_free(tokens);
	MQTTProperties_free(&props);

	assert("test8_publishFailures == 0", test8_publishFailures == 0,
		   "test8_publishFailures = %d", test8_publishFailures);

	MQTTAsync_destroy(&c);

	assert("test8_publishFailures > 0", test8_publishFailures > 0,
		   "test8_publishFailures = %d", test8_publishFailures);

	/* cleanup persistence of any left over message data*/

	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
	rc = MQTTAsync_createWithOptions(&c, options.connection, "async_test8",
				MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	test8_subscribed = 0;
	opts.connectProperties = NULL;
	opts.cleanstart = 1;
	opts.context = c;

	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test8_subscribed)
		MySleep(100);

	test_finished = 0;
	dopts.context = c;
	rc = MQTTAsync_disconnect(c, &dopts);
	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	while (!test_finished)
		MySleep(100);

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
	int rc = -1;
 	int (*tests[])() = {NULL, test1, test2, test3, test4, test5, test6, test7, test8}; /* indexed starting from 1 */
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
				failures = rc = 0;
				MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
				rc += tests[options.test_no](options); /* return number of failures.  0 = test succeeded */
			}
		}
		else
		{
			if (options.test_no >= ARRAY_SIZE(tests))
				MyLog(LOGA_INFO, "No test number %d", options.test_no);
			else
			{
				MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
				rc = tests[options.test_no](options); /* run just the selected test */
			}
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
