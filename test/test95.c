/*******************************************************************************
 * Copyright (c) 2012, 2023 IBM Corp., Ian Craggs
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
	while (i > 0 && ++count < 10);
	assert("Number of getPendingTokens should be 0", i == 0, "i was %d ", i);
}


void assert3PendingTokens(MQTTAsync c)
{
	int i = 0, rc = 0;
	MQTTAsync_token *tokens;

	rc = MQTTAsync_getPendingTokens(c, &tokens);
	assert("Good rc from getPendingTokens", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	i = 0;
	if (tokens)
	{
		while (tokens[i] != -1)
			++i;
		MQTTAsync_free(tokens);
	}
	assert("Number of getPendingTokens should be 3", i == 3, "i was %d ", i);
}


/*********************************************************************

 Tests: offline buffering - sending messages while disconnected

 1. send some messages while disconnected, check that they are sent
 2. repeat test 1 using serverURIs
 3. repeat test 1 using auto reconnect
 4. repeat test 2 using auto reconnect
 5. check max-buffered
 6. check auto-reconnect parms alter behaviour as expected

 Tests: automatic reconnect

 - check that connected() is called
 - check that reconnect() causes reconnect attempt
 - check that reconnect() fails if no connect has been previously attempted

 *********************************************************************/




/*********************************************************************

 Test1: offline buffering - sending messages while disconnected

 1. call connect
 2. use proxy to disconnect the client
 3. while the client is disconnected, send more messages
 4. when the client reconnects, check that those messages are sent

 *********************************************************************/

int test1_will_message_received = 0;
int test1_messages_received = 0;

int test1_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;
	static int first = 1;

	if (first == 1)
	{
		first = 0;
		return 0; /* to force queue persistence */
	}

	MyLog(LOGA_DEBUG, "Message received on topic %s, \"%.*s\"", topicName, message->payloadlen, message->payload);

	if (message->payloadlen == strlen("will message") && memcmp(message->payload, "will message", message->payloadlen) == 0)
	  test1_will_message_received = 1;
	else
	  test1_messages_received++;

	if (message->struct_version == 1)
	{
		assert("Properties count should be > 0", message->properties.count > 0,
			"Properties count was %d\n", message->properties.count);
		logProperties(&message->properties);
	}

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}

int test1Finished = 0;

int test1OnFailureCalled = 0;

void test1cOnFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test1OnFailureCalled++;
	test1Finished = 1;
}

void test1dOnFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test1OnFailureCalled++;
	test1Finished = 1;
}

void test1cOnConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client d, context %p\n", context);
	MQTTAsync c = (MQTTAsync)context;
	int rc;
	static int done = 0;

	if (done == 0)
	{
		/* send a message to the proxy to break the connection */
		pubmsg.payload = "TERMINATE";
		pubmsg.payloadlen = (int)strlen(pubmsg.payload);
		pubmsg.qos = 0;
		pubmsg.retained = 0;
		rc = MQTTAsync_sendMessage(c, "MQTTSAS topic", &pubmsg, NULL);
		assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		done = 1; /* only do this once */
	}
}


int test1dReady = 0;
char willTopic[100];
char test_topic[100];

void test1donSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback for client d, %p granted qos %d", c,
			response->reasonCode);
	test1dReady = 1;
}


void test1dOnConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;
	int qoss[2] = {2, 2};
	char* topics[2] = {willTopic, test_topic};

	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client c, context %p\n", context);
	opts.onSuccess5 = test1donSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribeMany(c, 2, topics, qoss, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test1Finished = 1;
}

int test1c_connected = 0;

void test1cConnected(void* context, char* cause)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In connected callback for client c, context %p\n", context);
	test1c_connected = 1;
}


int test1(struct Options options)
{
	char* testname = "test1";
	int subsqos = 2;
	MQTTAsync c, d;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_createOptions createOptions = MQTTAsync_createOptions_initializer;
	int rc = 0;
	int count = 0;
	char clientidc[70];
	char clientidd[70];
	int i = 0;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperties willProps = MQTTProperties_initializer;
	MQTTProperty property;

	sprintf(willTopic, "paho-test95-1-%s", unique);
	sprintf(clientidc, "paho-test95-1-c-%s", unique);
	sprintf(clientidd, "paho-test95-1-d-%s", unique);
	sprintf(test_topic, "paho-test95-1-test topic %s", unique);

	test1Finished = 0;
	failures = 0;
	MyLog(LOGA_INFO, "Starting Offline buffering 1 - messages while disconnected");
	fprintf(xml, "<testcase classname=\"test1\" name=\"%s\"", testname);
	global_start_time = start_clock();

	createOptions.sendWhileDisconnected = 1;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.proxy_connection, clientidc,
		MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	createOptions.sendWhileDisconnected = 0;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&d, options.connection, clientidd,
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;

	rc = MQTTAsync_setCallbacks(d, d, NULL, test1_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.will = NULL; /* don't need will for this client, as it's going to be connected all the time */
	opts.context = d;
	opts.onSuccess5 = test1dOnConnect;
	opts.onFailure5 = test1dOnFailure;
	MyLog(LOGA_DEBUG, "Connecting client d");
	rc = MQTTAsync_connect(d, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait until d is ready: connected and subscribed */
	count = 0;
	while (!test1dReady && ++count < 10000)
		MySleep(100);
	assert("Count should be less than 10000", count < 10000, "count was %d", count); /* wrong */

	rc = MQTTAsync_setConnected(c, c, test1cConnected);
	assert("Good rc from setConnectedCallback", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	/* let client c go: connect, and send disconnect command to proxy */
	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = willTopic;

	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "test user property";
	property.value.data.len = (int)strlen(property.value.data.data);
	property.value.value.data = "test user property value";
	property.value.value.len = (int)strlen(property.value.value.data);
	MQTTProperties_add(&willProps, &property);
	opts.willProperties = &willProps;

	opts.onSuccess5 = test1cOnConnect;
	opts.onFailure5 = test1cOnFailure;
	opts.context = c;

	opts.cleanstart = 0;
	property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
	property.value.integer4 = 30;
	MQTTProperties_add(&props, &property);

	opts.connectProperties = &props;

	MyLog(LOGA_DEBUG, "Connecting client c");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	MQTTProperties_free(&props);
	MQTTProperties_free(&willProps);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait for will message */
	while (!test1_will_message_received && ++count < 10000)
		MySleep(100);

	MyLog(LOGA_DEBUG, "Now we can send some messages to be buffered");

	test1c_connected = 0;
	/* send some messages.  Then reconnect (check connected callback), and check that those messages are received */
	for (i = 0; i < 3; ++i)
	{
		char buf[50];
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
		MQTTProperty property;
		MQTTProperties props = MQTTProperties_initializer;

		sprintf(buf, "QoS %d message", i);
		pubmsg.payload = buf;
		pubmsg.payloadlen = (int)strlen(pubmsg.payload) + 1;
		pubmsg.qos = i;
		pubmsg.retained = 0;
	 	MyLog(LOGA_DEBUG, "Sending qos %d message", pubmsg.qos);

		property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
		property.value.data.data = "test user property";
		property.value.data.len = (int)strlen(property.value.data.data);
		property.value.value.data = "test user property value";
		property.value.value.len = (int)strlen(property.value.value.data);
		MQTTProperties_add(&props, &property);
		pubmsg.properties = props;
		rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
		assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
		MQTTProperties_free(&props);
		MyLog(LOGA_DEBUG, "Sent qos %d message, token %d", pubmsg.qos, opts.token);
	}

 	assert3PendingTokens(c);

 	/* destroy and recreate to read from persistence */
 	MyLog(LOGA_DEBUG, "Destroy and recreate client c");
 	MQTTAsync_destroy(&c);

	createOptions.sendWhileDisconnected = 1;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.proxy_connection, clientidc,
		MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	assert3PendingTokens(c);

	rc = MQTTAsync_setConnected(c, c, test1cConnected);
	assert("Good rc from setConnectedCallback", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	opts.will = &wopts;
	opts.onSuccess5 = test1cOnConnect;
	opts.onFailure5 = test1cOnFailure;
	opts.context = c;
	opts.cleanstart = 0;
	MyLog(LOGA_DEBUG, "Reconnecting client c");
	test1c_connected = 0;
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	MQTTProperties_free(&props);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait for client to be reconnected */
	while (!test1c_connected && ++count < 10000)
		MySleep(100);

	/* wait for messages to be received */
	while (test1_messages_received < 3 && ++count < 10000)
		MySleep(100);

	waitForNoPendingTokens(c);

	rc = MQTTAsync_disconnect(c, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	rc = MQTTAsync_disconnect(d, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

exit:
	MQTTAsync_destroy(&c);
	MQTTAsync_destroy(&d);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}


/*********************************************************************

 Test2: offline buffering - sending messages while disconnected

 1. call connect
 2. use proxy to disconnect the client
 3. while the client is disconnected, send more messages
 4. when the client reconnects, check that those messages are sent

 *********************************************************************/

int test2_will_message_received = 0;
int test2_messages_received = 0;

int test2_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;

	MyLog(LOGA_DEBUG, "Message received on topic %s, \"%.*s\"", topicName, message->payloadlen, message->payload);

	if (message->payloadlen == strlen("will message") && memcmp(message->payload, "will message", message->payloadlen) == 0)
	  test2_will_message_received = 1;
	else
	  test2_messages_received++;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}

int test2Finished = 0;

int test2OnFailureCalled = 0;

void test2cOnFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test2OnFailureCalled++;
	test2Finished = 1;
}

void test2dOnFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test2OnFailureCalled++;
	test2Finished = 1;
}

void test2cOnConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client d, context %p\n", context);
	MQTTAsync c = (MQTTAsync)context;
	int rc;

	/* send a message to the proxy to break the connection */
	pubmsg.payload = "TERMINATE";
	pubmsg.payloadlen = (int)strlen(pubmsg.payload);
	pubmsg.qos = 0;
	pubmsg.retained = 0;
	rc = MQTTAsync_sendMessage(c, "MQTTSAS topic", &pubmsg, NULL);
	assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


int test2dReady = 0;
char willTopic[100];
char test_topic[100];

void test2donSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback for client d, %p granted qos %d", c,
			response->reasonCode);
	test2dReady = 1;
}


void test2dOnConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;
	int qoss[2] = {2, 2};
	char* topics[2] = {willTopic, test_topic};

	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client c, context %p\n", context);
	opts.onSuccess5 = test2donSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribeMany(c, 2, topics, qoss, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test2Finished = 1;
}

int test2c_connected = 0;

void test2cConnected(void* context, char* cause)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In connected callback for client c, context %p\n", context);
	test2c_connected = 1;
}


int test2(struct Options options)
{
	char* testname = "test2";
	int subsqos = 2;
	MQTTAsync c, d;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_createOptions createOptions = MQTTAsync_createOptions_initializer;
	int rc = 0;
	int count = 0;
	char clientidc[70];
	char clientidd[70];
	int i = 0;
	char *URIs[2] = {"rubbish", options.proxy_connection};
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperty property;

	sprintf(willTopic, "paho-test95-2-%s", unique);
	sprintf(clientidc, "paho-test95-2-c-%s", unique);
	sprintf(clientidd, "paho-test95-2-d-%s", unique);
	sprintf(test_topic, "paho-test95-2-test topic %s", unique);

	test2Finished = 0;
	failures = 0;
	MyLog(LOGA_INFO, "Starting Offline buffering 2 - messages while disconnected with serverURIs");
	fprintf(xml, "<testcase classname=\"test2\" name=\"%s\"", testname);
	global_start_time = start_clock();

	createOptions.sendWhileDisconnected = 1;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, "not used", clientidc, MQTTCLIENT_PERSISTENCE_DEFAULT,
	      NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	createOptions.sendWhileDisconnected = 0;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&d, options.connection, clientidd, MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = MQTTVERSION_5;

	rc = MQTTAsync_setCallbacks(d, d, NULL, test2_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.will = NULL; /* don't need will for this client, as it's going to be connected all the time */
	opts.context = d;
	opts.onSuccess5 = test2dOnConnect;
	opts.onFailure5 = test2dOnFailure;
	MyLog(LOGA_DEBUG, "Connecting client d");
	rc = MQTTAsync_connect(d, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait until d is ready: connected and subscribed */
	count = 0;
	while (!test2dReady && ++count < 10000)
		MySleep(100);
	assert("Count should be less than 10000", count < 10000, "count was %d", count); /* wrong */

	rc = MQTTAsync_setConnected(c, c, test2cConnected);
	assert("Good rc from setConnectedCallback", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	/* let client c go: connect, and send disconnect command to proxy */
	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = willTopic;
	opts.onSuccess5 = test2cOnConnect;
	opts.onFailure5 = test2cOnFailure;
	opts.context = c;
	opts.MQTTVersion = MQTTVERSION_5;
	opts.cleanstart = 0;
	property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
	property.value.integer4 = 30;
	MQTTProperties_add(&props, &property);
	opts.connectProperties = &props;
	opts.serverURIs = URIs;
	opts.serverURIcount = 2;

	MyLog(LOGA_DEBUG, "Connecting client c");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}
	MQTTProperties_free(&props);

	/* wait for will message */
	while (!test2_will_message_received && ++count < 10000)
		MySleep(100);

	MyLog(LOGA_DEBUG, "Now we can send some messages to be buffered");

	test2c_connected = 0;
	/* send some messages.  Then reconnect (check connected callback), and check that those messages are received */
	for (i = 0; i < 3; ++i)
	{
	  char buf[50];

	  MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	  MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	  sprintf(buf, "QoS %d message", i);
	  pubmsg.payload = buf;
	  pubmsg.payloadlen = (int)(strlen(pubmsg.payload) + 1);
	  pubmsg.qos = i;
	  pubmsg.retained = 0;
	  rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
	  assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	}

	assert3PendingTokens(c);

	rc = MQTTAsync_reconnect(c);
 	assert("Good rc from reconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	/* wait for client to be reconnected */
	while (!test2c_connected && ++count < 10000)
		MySleep(100);

	/* wait for success or failure callback */
	while (test2_messages_received < 3 && ++count < 10000)
		MySleep(100);

 	waitForNoPendingTokens(c);

	rc = MQTTAsync_disconnect(c, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	rc = MQTTAsync_disconnect(d, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

exit:
	MySleep(200);
	MQTTAsync_destroy(&c);
	MQTTAsync_destroy(&d);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 test3: offline buffering - sending messages while disconnected

 1. call connect
 2. use proxy to disconnect the client
 3. while the client is disconnected, send more messages
 4. when the client auto reconnects, check that those messages are sent

 *********************************************************************/

int test3_will_message_received = 0;
int test3_messages_received = 0;

int test3_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;

	MyLog(LOGA_DEBUG, "Message received on topic %s, \"%.*s\"", topicName, message->payloadlen, message->payload);

	if (message->payloadlen == strlen("will message") && memcmp(message->payload, "will message", message->payloadlen) == 0)
	  test3_will_message_received = 1;
	else
	  test3_messages_received++;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}

int test3Finished = 0;

int test3OnFailureCalled = 0;

void test3cOnFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test3OnFailureCalled++;
	test3Finished = 1;
}

void test3dOnFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test3OnFailureCalled++;
	test3Finished = 1;
}

void test3cOnConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client d, context %p\n", context);
	MQTTAsync c = (MQTTAsync)context;
	int rc;

	/* send a message to the proxy to break the connection */
	pubmsg.payload = "TERMINATE";
	pubmsg.payloadlen = (int)strlen(pubmsg.payload);
	pubmsg.qos = 0;
	pubmsg.retained = 0;
	rc = MQTTAsync_sendMessage(c, "MQTTSAS topic", &pubmsg, NULL);
	assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


int test3dReady = 0;
char willTopic[100];
char test_topic[100];

void test3donSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback for client d, %p granted qos %d", c,
			response->reasonCode);
	test3dReady = 1;
}


void test3dOnConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;
	int qoss[2] = {2, 2};
	char* topics[2] = {willTopic, test_topic};

	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client c, context %p\n", context);
	opts.onSuccess5 = test3donSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribeMany(c, 2, topics, qoss, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test3Finished = 1;
}

int test3c_connected = 0;

void test3cConnected(void* context, char* cause)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In connected callback for client c, context %p\n", context);
	test3c_connected = 1;
}


int test3(struct Options options)
{
	char* testname = "test3";
	int subsqos = 2;
	MQTTAsync c, d;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_createOptions createOptions = MQTTAsync_createOptions_initializer;
	int rc = 0;
	int count = 0;
	char clientidc[70];
	char clientidd[70];
	int i = 0;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperty property;

	sprintf(willTopic, "paho-test95-3-%s", unique);
	sprintf(clientidc, "paho-test95-3-c-%s", unique);
	sprintf(clientidd, "paho-test95-3-d-%s", unique);
	sprintf(test_topic, "paho-test95-3-test topic %s", unique);

	test3Finished = 0;
	failures = 0;
	MyLog(LOGA_INFO, "Starting Offline buffering 3 - messages while disconnected");
	fprintf(xml, "<testcase classname=\"test3\" name=\"%s\"", testname);
	global_start_time = start_clock();

	createOptions.sendWhileDisconnected = 1;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.proxy_connection, clientidc, MQTTCLIENT_PERSISTENCE_DEFAULT,
	      NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	createOptions.sendWhileDisconnected = 0;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&d, options.connection, clientidd, MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = MQTTVERSION_5;

	rc = MQTTAsync_setCallbacks(d, d, NULL, test3_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.will = NULL; /* don't need will for this client, as it's going to be connected all the time */
	opts.context = d;
	opts.onSuccess5 = test3dOnConnect;
	opts.onFailure5 = test3dOnFailure;
	MyLog(LOGA_DEBUG, "Connecting client d");
	rc = MQTTAsync_connect(d, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait until d is ready: connected and subscribed */
	count = 0;
	while (!test3dReady && ++count < 10000)
		MySleep(100);
	assert("Count should be less than 10000", count < 10000, "count was %d", count); /* wrong */

	rc = MQTTAsync_setConnected(c, c, test3cConnected);
	assert("Good rc from setConnectedCallback", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	/* let client c go: connect, and send disconnect command to proxy */
	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = willTopic;
	opts.onSuccess5 = test3cOnConnect;
	opts.onFailure5 = test3cOnFailure;
	opts.context = c;
	opts.cleanstart = 0;
	property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
	property.value.integer4 = 30;
	MQTTProperties_add(&props, &property);
	opts.connectProperties = &props;
	opts.MQTTVersion = MQTTVERSION_5;
	opts.automaticReconnect = 1;

	MyLog(LOGA_DEBUG, "Connecting client c");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	MQTTProperties_free(&props);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait for will message */
	while (!test3_will_message_received && ++count < 10000)
		MySleep(100);

	MyLog(LOGA_DEBUG, "Now we can send some messages to be buffered");

	test3c_connected = 0;
	/* send some messages.  Then reconnect (check connected callback), and check that those messages are received */
	for (i = 0; i < 3; ++i)
	{
	  char buf[50];

	  MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	  MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	  sprintf(buf, "QoS %d message", i);
	  pubmsg.payload = buf;
	  pubmsg.payloadlen = (int)(strlen(pubmsg.payload) + 1);
	  pubmsg.qos = i;
	  pubmsg.retained = 0;
	  rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
	  assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	}

	assert3PendingTokens(c);

	/* wait for client to be reconnected */
	while (!test3c_connected && ++count < 10000)
		MySleep(100);

	/* wait for success or failure callback */
	while (test3_messages_received < 3 && ++count < 10000)
		MySleep(100);

 	waitForNoPendingTokens(c);

	rc = MQTTAsync_disconnect(c, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	rc = MQTTAsync_disconnect(d, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

exit:
	MySleep(200);
	MQTTAsync_destroy(&c);
	MQTTAsync_destroy(&d);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 test4: offline buffering - sending messages while disconnected

 1. call connect
 2. use proxy to disconnect the client
 3. while the client is disconnected, send more messages
 4. when the client auto reconnects, check that those messages are sent

 *********************************************************************/

int test4_will_message_received = 0;
int test4_messages_received = 0;

int test4_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;

	MyLog(LOGA_DEBUG, "Message received on topic %s, \"%.*s\"", topicName, message->payloadlen,
			message->payload);

	if (message->payloadlen == strlen("will message") && memcmp(message->payload, "will message", message->payloadlen) == 0)
	  test4_will_message_received = 1;
	else
	  test4_messages_received++;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}

int test4Finished = 0;

int test4OnFailureCalled = 0;

void test4cOnFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test4OnFailureCalled++;
	test4Finished = 1;
}

void test4dOnFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test4OnFailureCalled++;
	test4Finished = 1;
}

void test4cOnConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client d, context %p\n", context);
	MQTTAsync c = (MQTTAsync)context;
	int rc;

	/* send a message to the proxy to break the connection */
	pubmsg.payload = "TERMINATE";
	pubmsg.payloadlen = (int)strlen(pubmsg.payload);
	pubmsg.qos = 0;
	pubmsg.retained = 0;
	rc = MQTTAsync_sendMessage(c, "MQTTSAS topic", &pubmsg, NULL);
	assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


int test4dReady = 0;
char willTopic[100];
char test_topic[100];

void test4donSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback for client d, %p granted qos %d", c,
			response->reasonCode);
	test4dReady = 1;
}


void test4dOnConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;
	int qoss[2] = {2, 2};
	char* topics[2] = {willTopic, test_topic};

	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client c, context %p\n", context);
	opts.onSuccess5 = test4donSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribeMany(c, 2, topics, qoss, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test4Finished = 1;
}

int test4c_connected = 0;

void test4cConnected(void* context, char* cause)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In connected callback for client c, context %p\n", context);
	test4c_connected = 1;
}


int test4(struct Options options)
{
	char* testname = "test4";
	int subsqos = 2;
	MQTTAsync c, d;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_createOptions createOptions = MQTTAsync_createOptions_initializer;
	int rc = 0;
	int count = 0;
	char clientidc[70];
	char clientidd[70];
	int i = 0;
	char *URIs[2] = {"rubbish", options.proxy_connection};
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperty property;

	sprintf(willTopic, "paho-test95-4-%s", unique);
	sprintf(clientidc, "paho-test95-4-c-%s", unique);
	sprintf(clientidd, "paho-test95-4-d-%s", unique);
	sprintf(test_topic, "paho-test95-4-test topic %s", unique);

	test4Finished = 0;
	failures = 0;
	MyLog(LOGA_INFO, "Starting Offline buffering 4 - messages while disconnected with serverURIs");
	fprintf(xml, "<testcase classname=\"test4\" name=\"%s\"", testname);
	global_start_time = start_clock();

	createOptions.sendWhileDisconnected = 1;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, "not used", clientidc, MQTTCLIENT_PERSISTENCE_DEFAULT,
	      NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	createOptions.sendWhileDisconnected = 0;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&d, options.connection, clientidd, MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;

	rc = MQTTAsync_setCallbacks(d, d, NULL, test4_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.will = NULL; /* don't need will for this client, as it's going to be connected all the time */
	opts.context = d;
	opts.onSuccess5 = test4dOnConnect;
	opts.onFailure5 = test4dOnFailure;
	MyLog(LOGA_DEBUG, "Connecting client d");
	rc = MQTTAsync_connect(d, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait until d is ready: connected and subscribed */
	count = 0;
	while (!test4dReady && ++count < 10000)
		MySleep(100);
	assert("Count should be less than 10000", count < 10000, "count was %d", count); /* wrong */

	rc = MQTTAsync_setConnected(c, c, test4cConnected);
	assert("Good rc from setConnectedCallback", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	/* let client c go: connect, and send disconnect command to proxy */
	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = willTopic;
	opts.onSuccess5 = test4cOnConnect;
	opts.onFailure5 = test4cOnFailure;
	opts.context = c;
	opts.cleanstart = 0;
	property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
	property.value.integer4 = 30;
	MQTTProperties_add(&props, &property);
	opts.connectProperties = &props;
	opts.serverURIs = URIs;
	opts.serverURIcount = 2;
	opts.automaticReconnect = 1;

	MyLog(LOGA_DEBUG, "Connecting client c");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	MQTTProperties_free(&props);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait for will message */
	while (!test4_will_message_received && ++count < 10000)
		MySleep(100);

	MyLog(LOGA_DEBUG, "Now we can send some messages to be buffered");

	test4c_connected = 0;
	/* send some messages.  Then reconnect (check connected callback), and check that those messages are received */
	for (i = 0; i < 3; ++i)
	{
	  char buf[50];

	  MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	  MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	  sprintf(buf, "QoS %d message", i);
	  pubmsg.payload = buf;
	  pubmsg.payloadlen = (int)(strlen(pubmsg.payload) + 1);
	  pubmsg.qos = i;
	  pubmsg.retained = 0;
	  rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
	  assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	}

	assert3PendingTokens(c);

	/* wait for client to be reconnected */
	while (!test4c_connected && ++count < 10000)
		MySleep(100);

	/* wait for success or failure callback */
	while (test4_messages_received < 3 && ++count < 10000)
		MySleep(100);

 	waitForNoPendingTokens(c);

	rc = MQTTAsync_disconnect(c, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	rc = MQTTAsync_disconnect(d, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

exit:
	MySleep(200);
	MQTTAsync_destroy(&c);
	MQTTAsync_destroy(&d);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}


/*********************************************************************

 test5: offline buffering - check max buffered

 1. call connect
 2. use proxy to disconnect the client
 3. while the client is disconnected, send more messages
 4. when the client reconnects, check that those messages are sent

 *********************************************************************/

int test5_will_message_received = 0;
int test5_messages_received = 0;
int test5Finished = 0;
int test5OnFailureCalled = 0;
int test5c_connected = 0;

int test5_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;

	MyLog(LOGA_DEBUG, "Message received on topic %s, \"%.*s\"", topicName, message->payloadlen, message->payload);

	if (message->payloadlen == strlen("will message") && memcmp(message->payload, "will message", message->payloadlen) == 0)
	  test5_will_message_received = 1;
	else
	  test5_messages_received++;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}

void test5cOnFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test5OnFailureCalled++;
	test5Finished = 1;
}

void test5dOnFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test5OnFailureCalled++;
	test5Finished = 1;
}

void test5cOnConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client d, context %p\n", context);
	MQTTAsync c = (MQTTAsync)context;
	int rc;

	/* send a message to the proxy to break the connection */
	pubmsg.payload = "TERMINATE";
	pubmsg.payloadlen = (int)strlen(pubmsg.payload);
	pubmsg.qos = 0;
	pubmsg.retained = 0;
	rc = MQTTAsync_sendMessage(c, "MQTTSAS topic", &pubmsg, NULL);
	assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


int test5dReady = 0;
char willTopic[100];
char test_topic[100];

void test5donSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback for client d, %p granted qos %d", c,
			response->reasonCode);
	test5dReady = 1;
}


void test5dOnConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;
	int qoss[2] = {2, 2};
	char* topics[2] = {willTopic, test_topic};

	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client c, context %p\n", context);
	opts.onSuccess5 = test5donSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribeMany(c, 2, topics, qoss, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test5Finished = 1;
}

void test5cConnected(void* context, char* cause)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In connected callback for client c, context %p\n", context);
	test5c_connected = 1;
}


int test5(struct Options options)
{
	char* testname = "test5";
	int subsqos = 2;
	MQTTAsync c, d;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_createOptions createOptions = MQTTAsync_createOptions_initializer;
	int rc = 0;
	int count = 0;
	char clientidc[70];
	char clientidd[70];
	int i = 0;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperty property;

	sprintf(willTopic, "paho-test95-5-%s", unique);
	sprintf(clientidc, "paho-test95-5-c-%s", unique);
	sprintf(clientidd, "paho-test95-5-d-%s", unique);
	sprintf(test_topic, "paho-test95-5-test topic %s", unique);

	test5Finished = 0;
	failures = 0;
	MyLog(LOGA_INFO, "Starting Offline buffering 5 - max buffered");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	createOptions.sendWhileDisconnected = 1;
	createOptions.maxBufferedMessages = 3;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.proxy_connection, clientidc, MQTTCLIENT_PERSISTENCE_DEFAULT,
	      NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	createOptions.sendWhileDisconnected = 0;
	createOptions.maxBufferedMessages = 1;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&d, options.connection, clientidd, MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;

	rc = MQTTAsync_setCallbacks(d, d, NULL, test5_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.will = NULL; /* don't need will for this client, as it's going to be connected all the time */
	opts.context = d;
	opts.onSuccess5 = test5dOnConnect;
	opts.onFailure5 = test5dOnFailure;
	opts.MQTTVersion = MQTTVERSION_5;
	MyLog(LOGA_DEBUG, "Connecting client d");
	rc = MQTTAsync_connect(d, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait until d is ready: connected and subscribed */
	count = 0;
	while (!test5dReady && ++count < 10000)
		MySleep(100);
	assert("Count should be less than 10000", count < 10000, "count was %d", count); /* wrong */

	rc = MQTTAsync_setConnected(c, c, test5cConnected);
	assert("Good rc from setConnectedCallback", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	/* let client c go: connect, and send disconnect command to proxy */
	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = willTopic;
	opts.onSuccess5 = test5cOnConnect;
	opts.onFailure5 = test5cOnFailure;
	opts.context = c;
	opts.cleanstart = 0;
	opts.MQTTVersion = MQTTVERSION_5;
	property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
	property.value.integer4 = 30;
	MQTTProperties_add(&props, &property);
	opts.connectProperties = &props;

	MyLog(LOGA_DEBUG, "Connecting client c");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	MQTTProperties_free(&props);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait for will message */
	while (!test5_will_message_received && ++count < 10000)
		MySleep(100);

	MyLog(LOGA_DEBUG, "Now we can send some messages to be buffered");

	test5c_connected = 0;
	/* send some messages.  Then reconnect (check connected callback), and check that those messages are received */
	for (i = 0; i < 5; ++i)
	{
	  char buf[50];

	  MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	  MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	  sprintf(buf, "QoS %d message", i);
	  pubmsg.payload = buf;
	  pubmsg.payloadlen = (int)(strlen(pubmsg.payload) + 1);
	  pubmsg.qos = i % 3;
	  pubmsg.retained = 0;
	  rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
	  if (i <= 2)
	    assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	  else
	    assert("Bad rc from sendMessage", rc == MQTTASYNC_MAX_BUFFERED_MESSAGES, "rc was %d ", rc);
	}

	assert3PendingTokens(c);

	rc = MQTTAsync_reconnect(c);
 	assert("Good rc from reconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	/* wait for client to be reconnected */
	while (!test5c_connected && ++count < 10000)
		MySleep(100);

	/* wait for success or failure callback */
	while (test5_messages_received < 3 && ++count < 10000)
		MySleep(100);

 	waitForNoPendingTokens(c);

	rc = MQTTAsync_disconnect(c, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	rc = MQTTAsync_disconnect(d, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

exit:
	MySleep(200);
	MQTTAsync_destroy(&c);
	MQTTAsync_destroy(&d);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}


int test6(struct Options options)
{
	char* testname = "test6";
	int subsqos = 2;
	MQTTAsync c, d;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_createOptions createOptions = MQTTAsync_createOptions_initializer;
	int rc = 0;
	int count = 0;
	char clientidc[70];
	char clientidd[70];
	int i = 0;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperty property;

	test5_will_message_received = 0;
	test5_messages_received = 0;
	test5Finished = 0;
	test5OnFailureCalled = 0;
	test5c_connected = 0;

	sprintf(willTopic, "paho-test95-6-%s", unique);
	sprintf(clientidc, "paho-test95-6-c-%s", unique);
	sprintf(clientidd, "paho-test95-6-d-%s", unique);
	sprintf(test_topic, "paho-test95-6-test topic %s", unique);

	test5Finished = 0;
	failures = 0;
	MyLog(LOGA_INFO, "Starting Offline buffering 6 - max buffered with binary will");
	fprintf(xml, "<testcase classname=\"test6\" name=\"%s\"", testname);
	global_start_time = start_clock();

	createOptions.sendWhileDisconnected = 1;
	createOptions.maxBufferedMessages = 3;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.proxy_connection, clientidc, MQTTCLIENT_PERSISTENCE_DEFAULT,
	      NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	createOptions.sendWhileDisconnected = 0;
	createOptions.maxBufferedMessages = 1;
	createOptions.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&d, options.connection, clientidd, MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL, &createOptions);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = MQTTVERSION_5;

	rc = MQTTAsync_setCallbacks(d, d, NULL, test5_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.will = NULL; /* don't need will for this client, as it's going to be connected all the time */
	opts.context = d;
	opts.onSuccess5 = test5dOnConnect;
	opts.onFailure5 = test5dOnFailure;
	MyLog(LOGA_DEBUG, "Connecting client d");
	rc = MQTTAsync_connect(d, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait until d is ready: connected and subscribed */
	count = 0;
	while (!test5dReady && ++count < 10000)
		MySleep(100);
	assert("Count should be less than 10000", count < 10000, "count was %d", count); /* wrong */

	rc = MQTTAsync_setConnected(c, c, test5cConnected);
	assert("Good rc from setConnectedCallback", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	/* let client c go: connect, and send disconnect command to proxy */
	opts.will = &wopts;
	opts.will->payload.data = "will message";
	opts.will->payload.len = (int)strlen(opts.will->payload.data);
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = willTopic;
	opts.onSuccess5 = test5cOnConnect;
	opts.onFailure5 = test5cOnFailure;
	opts.context = c;
	opts.cleanstart = 0;
	opts.MQTTVersion = MQTTVERSION_5;
	property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
	property.value.integer4 = 30;
	MQTTProperties_add(&props, &property);
	opts.connectProperties = &props;

	MyLog(LOGA_DEBUG, "Connecting client c");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	MQTTProperties_free(&props);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait for will message */
	while (!test5_will_message_received && ++count < 10000)
		MySleep(100);

	MyLog(LOGA_DEBUG, "Now we can send some messages to be buffered");

	test5c_connected = 0;
	/* send some messages.  Then reconnect (check connected callback), and check that those messages are received */
	for (i = 0; i < 5; ++i)
	{
	  char buf[50];

	  MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	  MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	  sprintf(buf, "QoS %d message", i);
	  pubmsg.payload = buf;
	  pubmsg.payloadlen = (int)(strlen(pubmsg.payload) + 1);
	  pubmsg.qos = i % 3;
	  pubmsg.retained = 0;
	  rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &opts);
	  if (i <= 2)
	    assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	  else
	    assert("Bad rc from sendMessage", rc == MQTTASYNC_MAX_BUFFERED_MESSAGES, "rc was %d ", rc);
	}

	assert3PendingTokens(c);

	rc = MQTTAsync_reconnect(c);
 	assert("Good rc from reconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	/* wait for client to be reconnected */
	while (!test5c_connected && ++count < 10000)
		MySleep(100);

	/* wait for success or failure callback */
	while (test5_messages_received < 3 && ++count < 10000)
		MySleep(100);

 	waitForNoPendingTokens(c);

	rc = MQTTAsync_disconnect(c, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	rc = MQTTAsync_disconnect(d, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

exit:
	MySleep(200);
	MQTTAsync_destroy(&c);
	MQTTAsync_destroy(&d);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}


/*********************************************************************

Test7: Fill up TCP buffer with QoS 0 messages

*********************************************************************/
int test7c_connected = 0;
int test7_will_message_received = 0;
int test7_messages_received = 0;
int test7Finished = 0;
int test7OnFailureCalled = 0;
int test7dReady = 0;

int test7_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int message_count = 0;

	MyLog(LOGA_DEBUG, "Message received on topic %s, \"%.*s\"", topicName, message->payloadlen, message->payload);

	if (message->payloadlen == strlen("will message") && memcmp(message->payload, "will message", message->payloadlen) == 0)
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

void test7dOnConnectFailure(void* context, MQTTAsync_failureData5* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test7OnFailureCalled++;
	test7Finished = 1;
}


void test7donSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback for client d, %p granted qos %d", c,
			response->reasonCode);
	test7dReady = 1;
}


void test7dOnConnectSuccess(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int qoss[2] = {2, 2};
	char* topics[2] = {willTopic, test_topic};

	MyLog(LOGA_DEBUG, "In connect onSuccess callback for client c, context %p\n", context);
	opts.onSuccess5 = test7donSubscribe;
	opts.context = c;

	//rc = MQTTAsync_subscribeMany(c, 2, topics, qoss, &opts);
	//assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	//if (rc != MQTTASYNC_SUCCESS)
	//	test5Finished = 1;
	test7dReady = 1;
}


int test7(struct Options options)
{
	char* testname = "test7";
	int subsqos = 2;
	MQTTAsync c, d;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	int rc = 0;
	int count = 0;
	char clientidc[70];
	char clientidd[70];
	int i = 0;

	test7_will_message_received = 0;
	test7_messages_received = 0;
	test7Finished = 0;
	test7OnFailureCalled = 0;
	test7c_connected = 0;

	sprintf(willTopic, "paho-test95-7-%s", unique);
	sprintf(clientidc, "paho-test9-7-c-%s", unique);
	sprintf(clientidd, "paho-test9-7-d-%s", unique);
	sprintf(test_topic, "longer paho-test9-7-test topic %s", unique);

	test7Finished = 0;
	failures = 0;
	MyLog(LOGA_INFO, "Starting Offline buffering 7 - fill TCP buffer");
	fprintf(xml, "<testcase classname=\"test7\" name=\"%s\"", testname);
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.proxy_connection, clientidc, MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL, &createOpts);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&d, options.connection, clientidd, MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL, &createOpts);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;

	rc = MQTTAsync_setCallbacks(d, d, NULL, test7_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.will = NULL; /* don't need will for this client, as it's going to be connected all the time */
	opts.context = d;
	opts.onSuccess5 = test7dOnConnectSuccess;
	opts.onFailure5 = test7dOnConnectFailure;
	MyLog(LOGA_DEBUG, "Connecting client d");
	rc = MQTTAsync_connect(d, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait until d is ready: connected and subscribed */
	count = 0;
	while (!test7dReady && ++count < 10000)
	{
		if (test7Finished)
		  goto exit;
		MySleep(100);
	}
	assert("Count should be less than 10000", count < 10000, "count was %d", count); /* wrong */

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

	/* wait for will message */
	//while (test7_will_message_received == 0 && ++count < 10000)
	//	MySleep(100);

	MyLog(LOGA_DEBUG, "Now we can send some messages to be buffered by TCP");

	test7c_connected = 0;
	char buf[5000000];
	/* send some messages.  Then reconnect (check connected callback), and check that those messages are received */
	for (i = 0; i < 50000; ++i)
	{
	  MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	  MQTTAsync_responseOptions pubopts = MQTTAsync_responseOptions_initializer;
		pubmsg.qos = 0; /*i % 3;*/
	  sprintf(buf, "QoS %d message", pubmsg.qos);
	  pubmsg.payload = buf;
	  pubmsg.payloadlen = 5000000; //(int)(strlen(pubmsg.payload) + 1);
	  pubmsg.retained = 0;
	  rc = MQTTAsync_sendMessage(c, test_topic, &pubmsg, &pubopts);
	  assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
		if (rc != 0)
		{
		  //MyLog(LOGA_DEBUG, "Connecting client c");
		  //rc = MQTTAsync_connect(c, &opts);
			//MySleep(1000);
		  break;
		}
	}

#if 0
	assert3PendingTokens(c);

	rc = MQTTAsync_reconnect(c);
 	assert("Good rc from reconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	/* wait for client to be reconnected */
	while (!test5c_connected && ++count < 10000)
		MySleep(100);

	/* wait for success or failure callback */
	while (test5_messages_received < 3 && ++count < 10000)
		MySleep(100);

 	waitForNoPendingTokens(c);
#endif

exit:
	rc = MQTTAsync_disconnect(c, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	rc = MQTTAsync_disconnect(d, NULL);
 	assert("Good rc from disconnect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);

	MySleep(200);
	MQTTAsync_destroy(&c);
	MQTTAsync_destroy(&d);
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
	int (*tests[])() = { NULL, test1, test2, test3, test4, test5, test6 };
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
			MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
			rc += tests[options.test_no](options); /* return number of failures.  0 = test succeeded */
		}
	}
	else
	{
		MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
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
