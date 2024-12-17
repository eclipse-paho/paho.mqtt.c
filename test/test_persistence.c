/*******************************************************************************
 * Copyright (c) 2012, 2022 IBM Corp., Ian Craggs
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
 *    Ian Craggs - correct some compile warnings
 *    Ian Craggs - add binary will message test
 *    Ian Craggs - MQTT V5 updates
 *******************************************************************************/


/**
 * @file
 * Offline buffering and automatic reconnect tests for the Paho Asynchronous MQTT C client
 *
 */


#include "MQTTAsync.h"
#include <string.h>
#include <stdlib.h>
#include "Thread.h"

#if !defined(_WINDOWS)
	#include <sys/time.h>
  #include <sys/socket.h>
	#include <unistd.h>
  #include <errno.h>
#else
  #include <windows.h>
#endif

char unique[50]; // unique suffix/prefix to add to clientid/topic etc

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

void usage(void)
{
	printf("help!!\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	char* connection;            /**< connection to system under test. */
	char* proxy_connection;      /**< connection to proxy */
	int verbose;
	int test_no;
} options =
{
	"localhost:1883",
	"localhost:1884",
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
		else if (strcmp(argv[count], "--connection") == 0)
		{
			if (++count < argc)
				options.connection = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--proxy_connection") == 0)
		{
			if (++count < argc)
				options.proxy_connection = argv[count];
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
	struct timeb ts;

	struct tm *timeinfo;

	if (LOGA_level == LOGA_DEBUG && options.verbose == 0)
		return;

	ftime(&ts);
	timeinfo = localtime(&ts.time);
	strftime(msg_buf, 80, "%Y%m%d %H%M%S", timeinfo);

	sprintf(&msg_buf[strlen(msg_buf)], ".%.3hu ", ts.millitm);

	va_start(args, format);
	vsnprintf(&msg_buf[strlen(msg_buf)], sizeof(msg_buf) - strlen(msg_buf),
			format, args);
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
	return (res.tv_sec) * 1000 + (res.tv_usec) / 1000;
}
#endif

#define assert(a, b, c, d) myassert(__FILE__, __LINE__, a, b, c, d)
#define assert1(a, b, c, d, e) myassert(__FILE__, __LINE__, a, b, c, d, e)

#define MAXMSGS 30;

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

		cur_output += sprintf(cur_output, "<failure type=\"%s\">file %s, line %d </failure>\n",
                        description, filename, lineno);
	}
	else
		MyLog(LOGA_DEBUG, "Assertion succeeded, file %s, line %d, description: %s",
				filename, lineno, description);
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
		  MyLog(LOGA_DEBUG, intformat, name, props->array[i].value.byte);
		  break;
		case MQTTPROPERTY_TYPE_TWO_BYTE_INTEGER:
		  MyLog(LOGA_DEBUG, intformat, name, props->array[i].value.integer2);
		  break;
		case MQTTPROPERTY_TYPE_FOUR_BYTE_INTEGER:
		  MyLog(LOGA_DEBUG, intformat, name, props->array[i].value.integer4);
		  break;
		case MQTTPROPERTY_TYPE_VARIABLE_BYTE_INTEGER:
		  MyLog(LOGA_DEBUG, intformat, name, props->array[i].value.integer4);
		  break;
		case MQTTPROPERTY_TYPE_BINARY_DATA:
		case MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING:
		  MyLog(LOGA_DEBUG, "Property name %s value len %.*s", name,
				  props->array[i].value.data.len, props->array[i].value.data.data);
		  break;
		case MQTTPROPERTY_TYPE_UTF_8_STRING_PAIR:
		  MyLog(LOGA_DEBUG, "Property name %s key %.*s value %.*s", name,
			  props->array[i].value.data.len, props->array[i].value.data.data,
		  	  props->array[i].value.value.len, props->array[i].value.value.data);
		  break;
		}
	}
}

char willTopic[100];
char test_topic[50];

/*********************************************************************

Test7: Fill up TCP buffer with QoS 0 messages

*********************************************************************/
int test7c_connected = 0;
int test7_will_message_received = 0;
int test7_messages_received = 0;
int test7Finished = 0;
int test7OnFailureCalled = 0;

int test7_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;

	MyLog(LOGA_DEBUG, "Message received on topic %s, \"%.*s\"", topicName, message->payloadlen, message->payload);

	if (memcmp(message->payload, "will message", message->payloadlen) == 0)
	  test7_will_message_received = 1;
	else
	  test7_messages_received++;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}

void test7cConnected(void* context, char* cause)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In connected callback for client c, context %p\n", context);
	test7c_connected = 1;
}

void test7cOnConnectFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In c connect onFailure callback, context %p", context);

	test7OnFailureCalled++;
	test7Finished = 1;
}

void test7cOnConnectSuccess(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client c, context %p\n", context);

	/* send a message to the proxy to break the connection */
	pubmsg.payload = "TERMINATE";
	pubmsg.payloadlen = (int)strlen(pubmsg.payload);
	pubmsg.qos = 0;
	pubmsg.retained = 0;
	//rc = MQTTAsync_sendMessage(c, "MQTTSAS topic", &pubmsg, NULL);
	//assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


int test7(struct Options options)
{
	char* testname = "test7";
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	int rc = 0;
	int count = 0;
	char clientidc[50];
	int i = 0;

	test7_will_message_received = 0;
	test7_messages_received = 0;
	test7Finished = 0;
	test7OnFailureCalled = 0;
	test7c_connected = 0;

	sprintf(willTopic, "paho-test95-7-%s", unique);
	sprintf(clientidc, "paho-test9-7-c-%s", "same"); //unique);
	sprintf(test_topic, "longer paho-test9-7-test topic %s", unique);

	test7Finished = 0;
	failures = 0;
	MyLog(LOGA_INFO, "Starting Offline buffering 7 - many persisted messages");
	fprintf(xml, "<testcase classname=\"test7\" name=\"%s\"", testname);
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	createOpts.allowDisconnectedSendAtAnyTime = 1;
	createOpts.sendWhileDisconnected = 1;
	createOpts.maxBufferedMessages = 64000;
	createOpts.persistQoS0 = 1;
	printf("Create starting\n");
	START_TIME_TYPE start = start_clock();
	rc = MQTTAsync_createWithOptions(&c, options.proxy_connection, clientidc, MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL, &createOpts);
	long duration = elapsed(start);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}
	printf("Create finished after %ld ms\n", duration);
	MQTTAsync_token *tokens, *cur_token;
	MQTTAsync_getPendingTokens(c, &tokens);
	int token_count = 0;
	if ((cur_token = tokens) != NULL)
	{
		while (*cur_token != -1)
		{
			cur_token++;
			token_count++;
		}
	}
	printf("%d messages restored\n", token_count);
	if (tokens)
		MQTTAsync_free(tokens);

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;

	rc = MQTTAsync_setCallbacks(c, c, NULL, test7_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

#if 0
	opts.will = NULL; /* don't need will for this client, as it's going to be connected all the time */
	opts.context = c;
	opts.onSuccess5 = test7cOnConnectSuccess;
	opts.onFailure5 = test7cOnConnectFailure;
	MyLog(LOGA_DEBUG, "Connecting client c");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait until d is ready: connected and subscribed */
	count = 0;
	while (!test7cReady && ++count < 10000)
	{
		if (test7Finished)
		  goto exit;
		MySleep(100);
	}
	assert("Count should be less than 10000", count < 10000, "count was %d", count); /* wrong */
#endif

	rc = MQTTAsync_setConnected(c, c, test7cConnected);
	assert("Good rc from setConnectedCallback", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	/* let client c go: connect, and send disconnect command to proxy */
	opts.will = &wopts;
	opts.will->payload.data = "will message";
	opts.will->payload.len = (int)strlen(opts.will->payload.data) + 1;
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = willTopic;
	opts.onSuccess5 = test7cOnConnectSuccess;
	opts.onFailure5 = test7cOnConnectFailure;
	opts.context = c;
	opts.cleansession = 0;
	/*opts.automaticReconnect = 1;
	opts.minRetryInterval = 3;
	opts.maxRetryInterval = 6;*/

#if 0
	MyLog(LOGA_DEBUG, "Connecting client c");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	count = 0;
	while (!test7c_connected && ++count < 10000)
		MySleep(100);
	assert("Count should be less than 10000", count < 10000, "count was %d", count); /* wrong */
#endif

	/* wait for will message */
	//while (test7_will_message_received == 0 && ++count < 10000)
	//	MySleep(100);

	MyLog(LOGA_DEBUG, "Now we can send some messages to be buffered by TCP");

	test7c_connected = 0;
#define PAYLOAD_LEN 500
	char buf[PAYLOAD_LEN];
	/* send some messages.  Then reconnect (check connected callback), and check that those messages are received */
	for (i = 0; i < 50000; ++i)
	{
	  MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	  MQTTAsync_responseOptions pubopts = MQTTAsync_responseOptions_initializer;
	  pubmsg.qos = i % 3;
	  sprintf(buf, "QoS %d message", pubmsg.qos);
	  pubmsg.payload = buf;
	  pubmsg.payloadlen = PAYLOAD_LEN;
	  pubmsg.retained = 0;
	  rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &pubopts);
	  assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	  if (rc != MQTTASYNC_SUCCESS)
	  {
		MySleep(3000);
		MyLog(LOGA_DEBUG, "Connecting client c");
		rc = MQTTAsync_connect(c, &opts);
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
		if (rc != MQTTASYNC_SUCCESS)
		{
			failures++;
			goto exit;
		}

		count = 0;
		while (!test7c_connected && ++count < 10000)
			MySleep(100);
		assert("Count should be less than 10000", count < 10000, "count was %d", count); /* wrong */
		MySleep(3000);
		break;
	  }
	}

exit:
	rc = MQTTAsync_disconnect(c, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	/*rc = MQTTAsync_disconnect(d, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);*/

	MySleep(200);
	MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}



void handleTrace(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
	printf("%s\n", message);
}


int main(int argc, char** argv)
{
	int* numtests = &tests;
	int rc = 0;
	int (*tests[])() = { NULL, test7 };
	time_t randtime;

	srand((unsigned) time(&randtime));
	sprintf(unique, "%u", rand());
	MyLog(LOGA_INFO, "Random prefix/suffix is %s", unique);

	xml = fopen("TEST-test9.xml", "w");
	fprintf(xml, "<testsuite name=\"test9\" tests=\"%d\">\n", (int)(ARRAY_SIZE(tests) - 1));

	MQTTAsync_setTraceCallback(handleTrace);
	getopts(argc, argv);

	if (options.test_no == 0)
	{ /* run all the tests */
		for (options.test_no = 1; options.test_no < ARRAY_SIZE(tests); ++options.test_no)
		{
			failures = 0;
			MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_PROTOCOL);
			rc += tests[options.test_no](options); /* return number of failures.  0 = test succeeded */
		}
	}
	else
	{
		MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_PROTOCOL);
		rc = tests[options.test_no](options); /* run just the selected test */
	}

	MyLog(LOGA_INFO, "Total tests run: %d", *numtests);
	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	fprintf(xml, "</testsuite>\n");
	fclose(xml);

	return rc;
}

