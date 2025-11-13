/*******************************************************************************
 * Copyright (c) 2009, 2024 IBM Corp.
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

#if 0
#include <logaX.h>   /* For general log messages                      */
#define MyLog logaLine
#else
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
#endif


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


struct
{
	int disconnected;
	char* test_topic;
	int test_finished;
} test_client_topic_aliases_globals =
{
	0, "client topic aliases topic", 0
};


void test1_onDisconnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", c);
	test_client_topic_aliases_globals.test_finished = 1;
}


int test_client_topic_aliases_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int received = 0;

	received++;
	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	if (message->struct_version == 1)
	{
		const int props_count = 0;

		assert("No topic alias", MQTTProperties_hasProperty(&message->properties, MQTTPROPERTY_CODE_TOPIC_ALIAS) == 0,
				"topic alias is %d\n", MQTTProperties_hasProperty(&message->properties, MQTTPROPERTY_CODE_TOPIC_ALIAS));

		assert("Topic should be name", strcmp(topicName, test_client_topic_aliases_globals.test_topic) == 0,
					"Topic name was %s\n", topicName);

		logProperties(&message->properties);
	}

	if (received == 2)
		test_client_topic_aliases_globals.test_finished = 1;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


void test_client_topic_aliases_onSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTProperty property;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "Suback properties:");
	logProperties(&response->properties);

	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.retained = 0;
	pubmsg.qos = 1;

	/* first set the topic alias */
	property.identifier = MQTTPROPERTY_CODE_TOPIC_ALIAS;
	property.value.integer2 = 1;
	MQTTProperties_add(&pubmsg.properties, &property);

	rc = MQTTAsync_sendMessage(c, test_client_topic_aliases_globals.test_topic, &pubmsg, &opts);
	assert("Good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_client_topic_aliases_globals.test_finished = 1;
	else
	{
		/* now send the publish with topic alias only */
		rc = MQTTAsync_sendMessage(c, "", &pubmsg, &opts);
		assert("Good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		if (rc != MQTTASYNC_SUCCESS)
			test_client_topic_aliases_globals.test_finished = 1;
	}
	MQTTProperties_free(&pubmsg.properties);
}


void test_client_topic_aliases_disconnected(void* context, MQTTProperties* props, enum MQTTReasonCodes rc)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "Callback: disconnected, reason code \"%s\"", MQTTReasonCode_toString(rc));
	logProperties(props);
	test_client_topic_aliases_globals.disconnected = 1;
}


void test_client_topic_aliases_onConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTProperty property;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;
	static int first = 1;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_SUCCESS,
		   "Reason code was %d\n", response->reasonCode);

	MyLog(LOGA_DEBUG, "Connack properties:");
	logProperties(&response->properties);

	if (first)
	{
		first = 0;
		pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.qos = 1;
		pubmsg.retained = 0;

		/* a Topic Alias of 0 is not allowed, so we should be disconnected */
		property.identifier = MQTTPROPERTY_CODE_TOPIC_ALIAS;
		property.value.integer2 = 0;
		MQTTProperties_add(&pubmsg.properties, &property);

		rc = MQTTAsync_sendMessage(c, test_client_topic_aliases_globals.test_topic, &pubmsg, &opts);
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		if (rc != MQTTASYNC_SUCCESS)
			test_client_topic_aliases_globals.test_finished = 1;
		MQTTProperties_free(&pubmsg.properties);
	}
	else
	{
		opts.onSuccess5 = test_client_topic_aliases_onSubscribe;
		opts.context = c;
		rc = MQTTAsync_subscribe(c, test_client_topic_aliases_globals.test_topic, 2, &opts);
		assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		if (rc != MQTTASYNC_SUCCESS)
			test_client_topic_aliases_globals.test_finished = 1;

	}
}


/*********************************************************************

Test1: client topic aliases

*********************************************************************/
int test_client_topic_aliases(struct Options options)
{
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperties willProps = MQTTProperties_initializer;
	MQTTProperty property;
	int rc = 0;
	char* test_topic = "V5 C client test client topic aliases";
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;

	MyLog(LOGA_INFO, "Starting V5 test 1 - client topic aliases");
	fprintf(xml, "<testcase classname=\"test11\" name=\"client topic aliases\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "async_test_aliases",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test_client_topic_aliases_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	rc = MQTTAsync_setDisconnected(c, c, test_client_topic_aliases_disconnected);

	opts.keepAliveInterval = 20;
	opts.username = "testuser";
	opts.password = "testpassword";
	opts.MQTTVersion = options.MQTTVersion;
	opts.cleanstart = 1;

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess5 = test_client_topic_aliases_onConnect;
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

	while (test_client_topic_aliases_globals.disconnected == 0)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	/* Now try a valid topic alias */
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (test_client_topic_aliases_globals.test_finished == 0)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	MQTTProperties_free(&props);
	MQTTProperties_free(&willProps);
	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST1: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}


struct
{
	char* test_topic;
	int test_finished;
	int messages_arrived;
	int msg_count;
} test_server_topic_aliases_globals =
{
	"server topic aliases topic", 0, 0, 3
};


int test_server_topic_aliases_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	static int received = 0;
	static int first_topic_alias = 0;
	int topicAlias = 0;

	received++;
	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	if (message->struct_version == 1)
	{
		const int props_count = 0;

		if (MQTTProperties_hasProperty(&message->properties, MQTTPROPERTY_CODE_TOPIC_ALIAS))
			topicAlias = (int)MQTTProperties_getNumericValue(&message->properties, MQTTPROPERTY_CODE_TOPIC_ALIAS);

		if (received == 1)
		{
			first_topic_alias = topicAlias;
			assert("Topic should be name", strcmp(topicName, test_server_topic_aliases_globals.test_topic) == 0,
					"Topic name was %s\n", topicName);
		}
		else
		{
			assert("All topic aliases should be the same", topicAlias == first_topic_alias,
					"Topic alias was %d\n", topicAlias);
			assert("Topic should be 0 length", strcmp(topicName, "") == 0,
					"Topic name was %s\n", topicName);
		}

		assert("topicAlias should not be 0", topicAlias > 0, "Topic alias was %d\n", topicAlias);
		logProperties(&message->properties);
	}
	test_server_topic_aliases_globals.messages_arrived++;

	if (test_server_topic_aliases_globals.messages_arrived == test_server_topic_aliases_globals.msg_count)
		test_server_topic_aliases_globals.test_finished = 1;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


void test_server_topic_aliases_onSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int qos = 0, rc;

	MyLog(LOGA_DEBUG, "Suback properties:");
	logProperties(&response->properties);

	test_server_topic_aliases_globals.messages_arrived = 0;
	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.retained = 0;
	for (qos = 0; qos < test_server_topic_aliases_globals.msg_count; ++qos)
	{
		pubmsg.qos = qos;
		rc = MQTTAsync_sendMessage(c, test_server_topic_aliases_globals.test_topic, &pubmsg, &opts);
		assert("Good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		if (rc != MQTTASYNC_SUCCESS)
		{
			test_server_topic_aliases_globals.test_finished = 1;
			break;
		}
	}
}


void test_server_topic_aliases_onConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_SUCCESS,
		   "Reason code was %d\n", response->reasonCode);

	MyLog(LOGA_DEBUG, "Connack properties:");
	logProperties(&response->properties);

	opts.onSuccess5 = test_server_topic_aliases_onSubscribe;
	opts.context = c;
	rc = MQTTAsync_subscribe(c, test_server_topic_aliases_globals.test_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_server_topic_aliases_globals.test_finished = 1;
}


/*********************************************************************

Test2: server topic aliases

*********************************************************************/
int test_server_topic_aliases(struct Options options)
{
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTProperties connect_props = MQTTProperties_initializer;
	MQTTProperty property;
	int rc = 0;
	char* test_topic = "V5 C client test server topic aliases";
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;

	MyLog(LOGA_INFO, "Starting V5 test 2 - server topic aliases");
	fprintf(xml, "<testcase classname=\"test11\" name=\"server topic aliases\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "server_topic_aliases",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test_server_topic_aliases_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	/* Allow at least one server topic alias */
	property.identifier = MQTTPROPERTY_CODE_TOPIC_ALIAS_MAXIMUM;
	property.value.integer2 = 1;
	MQTTProperties_add(&connect_props, &property);

	opts.MQTTVersion = options.MQTTVersion;
	opts.onSuccess5 = test_server_topic_aliases_onConnect;
	opts.context = c;
	opts.connectProperties = &connect_props;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (test_server_topic_aliases_globals.test_finished == 0)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	MQTTProperties_free(&connect_props);
	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST2: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}



/*********************************************************************

Test3: subscription ids

*********************************************************************/

struct
{
	char* test_topic;
	int test_finished;
	int messages_arrived;
} test_subscription_ids_globals =
{
	"server subscription ids topic", 0, 0
};


int test_subscription_ids_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;

	test_subscription_ids_globals.messages_arrived++;

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	if (message->struct_version == 1)
	{
		int subsidcount = 0, i = 0;

		subsidcount = MQTTProperties_propertyCount(&message->properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER);

		for (i = 0; i < subsidcount; ++i)
		{
			int subsid = (int)MQTTProperties_getNumericValueAt(&message->properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER, i);
			assert("Subsid is i+1", subsid == i+1, "subsid is not correct %d\n", subsid);
		}
		logProperties(&message->properties);
	}

	test_subscription_ids_globals.test_finished = 1;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


void test_subscription_ids_onSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTProperty property;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;
	static int subs_count = 0;

	MyLog(LOGA_DEBUG, "Suback properties:");
	logProperties(&response->properties);

	if (++subs_count == 1)
	{
		/* subscribe to a wildcard */
		property.identifier = MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER;
		property.value.integer4 = 2;
		MQTTProperties_add(&opts.properties, &property);

		opts.onSuccess5 = test_subscription_ids_onSubscribe;
		opts.context = c;

		rc = MQTTAsync_subscribe(c, "+", 2, &opts);
		assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		if (rc != MQTTASYNC_SUCCESS)
			test_subscription_ids_globals.test_finished = 1;

		MQTTProperties_free(&opts.properties);
	}
	else
	{
		test_subscription_ids_globals.messages_arrived = 0;
		pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.retained = 0;
		pubmsg.qos = 1;
		rc = MQTTAsync_sendMessage(c, test_subscription_ids_globals.test_topic, &pubmsg, &opts);
		assert("Good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
		if (rc != MQTTASYNC_SUCCESS)
			test_subscription_ids_globals.test_finished = 1;
	}
}


void test_subscription_ids_onConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTProperty property;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_SUCCESS,
		   "Reason code was %d\n", response->reasonCode);

	MyLog(LOGA_DEBUG, "Connack properties:");
	logProperties(&response->properties);

	opts.onSuccess5 = test_subscription_ids_onSubscribe;
	opts.context = c;
	/* subscribe to the test topic */
	property.identifier = MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER;
	property.value.integer4 = 1;
	MQTTProperties_add(&opts.properties, &property);

	rc = MQTTAsync_subscribe(c, test_subscription_ids_globals.test_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_subscription_ids_globals.test_finished = 1;

	MQTTProperties_free(&opts.properties);
}


int test_subscription_ids(struct Options options)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	int rc = 0;

	MyLog(LOGA_INFO, "Starting V5 test 3 - subscription ids");
	fprintf(xml, "<testcase classname=\"test11\" name=\"subscription ids\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "subscription_ids",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test_subscription_ids_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.MQTTVersion = options.MQTTVersion;
	opts.onSuccess5 = test_subscription_ids_onConnect;
	opts.context = c;
	opts.cleanstart = 1;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (test_subscription_ids_globals.test_finished == 0)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST3: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

Test: flow control

*********************************************************************/

struct
{
	char * test_topic;
	int test_finished;
	int messages_arrived;
	int receive_maximum;
	int blocking_found;
} test_flow_control_globals =
{
	"flow control topic", 0, 0, 65535, 0
};


int test_flow_control_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;

	test_flow_control_globals.messages_arrived++;

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	if (message->struct_version == 1)
		logProperties(&message->properties);

	if (test_flow_control_globals.messages_arrived == test_flow_control_globals.receive_maximum + 2)
		test_flow_control_globals.test_finished = 1;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


void test_flow_control_onSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;
	int i = 0;

	MyLog(LOGA_DEBUG, "Suback properties:");
	logProperties(&response->properties);

	test_flow_control_globals.messages_arrived = 0;
	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.retained = 0;
	pubmsg.qos = 2;

	for (i = 0; i < test_flow_control_globals.receive_maximum + 2; ++i)
	{
		rc = MQTTAsync_sendMessage(c, test_flow_control_globals.test_topic, &pubmsg, &opts);
		assert("Good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
		if (rc != MQTTASYNC_SUCCESS)
			test_flow_control_globals.test_finished = 1;
	}
}


void test_flow_control_onConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_SUCCESS,
		   "Reason code was %d\n", response->reasonCode);

	MyLog(LOGA_DEBUG, "Connack properties:");
	logProperties(&response->properties);

	if (MQTTProperties_hasProperty(&response->properties, MQTTPROPERTY_CODE_RECEIVE_MAXIMUM))
		test_flow_control_globals.receive_maximum = (int)MQTTProperties_getNumericValue(&response->properties, MQTTPROPERTY_CODE_RECEIVE_MAXIMUM);

	opts.onSuccess5 = test_flow_control_onSubscribe;
	opts.context = c;

	rc = MQTTAsync_subscribe(c, test_flow_control_globals.test_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_flow_control_globals.test_finished = 1;

	MQTTProperties_free(&opts.properties);
}


void flow_control_trace_callback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
	static char* msg = "Blocking on server receive maximum";

	if (strstr(message, msg) != NULL)
		test_flow_control_globals.blocking_found = 1;
}


int test_flow_control(struct Options options)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	int rc = 0;

	MyLog(LOGA_INFO, "Starting V5 test - flow control");
	fprintf(xml, "<testcase classname=\"test11\" name=\"flow control\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "flow_control",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	MQTTAsync_setTraceCallback(flow_control_trace_callback);
	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_MINIMUM);

	rc = MQTTAsync_setCallbacks(c, c, NULL, test_flow_control_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.MQTTVersion = options.MQTTVersion;
	opts.onSuccess5 = test_flow_control_onConnect;
	opts.context = c;
	opts.cleanstart = 1;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (test_flow_control_globals.test_finished == 0)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	assert("should have blocked", test_flow_control_globals.blocking_found == 1, "was %d\n",
			test_flow_control_globals.blocking_found);

	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST4: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}

struct
{
	char* test_topic;
	int test_finished;
	int messages_arrived;
} test_error_reporting_globals =
{
	"error reporting topic", 0, 0
};


void test_error_reporting_onUnsubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int i = 0;

	MyLog(LOGA_DEBUG, "Unsuback properties:");
	logProperties(&response->properties);

	assert("Reason code count should be 2", response->alt.unsub.reasonCodeCount == 2,
		   "Reason code count was %d\n", response->alt.unsub.reasonCodeCount);

	if (response->alt.unsub.reasonCodeCount == 1)
		MyLog(LOGA_DEBUG, "reason code %d", response->reasonCode);
	else if (response->alt.unsub.reasonCodeCount > 1)
	{
		for (i = 0; i < response->alt.unsub.reasonCodeCount; ++i)
		{
			MyLog(LOGA_DEBUG, "Unsubscribe reason code %d", response->alt.unsub.reasonCodes[i]);
		}
	}

	test_error_reporting_globals.test_finished = 1;
}


void test_error_reporting_onSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTProperty property;
	char* topics[2] = {test_error_reporting_globals.test_topic, "+"};
	int rc;
	int i = 0;

	MyLog(LOGA_DEBUG, "Suback properties:");
	logProperties(&response->properties);

	assert("Reason code count should be 2", response->alt.sub.reasonCodeCount == 2,
		   "Reason code count was %d\n", response->alt.sub.reasonCodeCount);

	if (response->alt.sub.reasonCodeCount == 1)
		MyLog(LOGA_DEBUG, "reason code %d", response->reasonCode);
	else if (response->alt.sub.reasonCodeCount > 1)
	{
		for (i = 0; i < response->alt.sub.reasonCodeCount; ++i)
		{
			MyLog(LOGA_DEBUG, "Subscribe reason code %d", response->alt.sub.reasonCodes[i]);
		}
	}

	opts.onSuccess5 = test_error_reporting_onUnsubscribe;
	opts.context = c;

	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "test user property";
	property.value.data.len = (int)strlen(property.value.data.data);
	property.value.value.data = "test user property value";
	property.value.value.len = (int)strlen(property.value.value.data);
	MQTTProperties_add(&opts.properties, &property);

	rc = MQTTAsync_unsubscribeMany(c, 2, topics, &opts);
	assert("Good rc from unsubscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_flow_control_globals.test_finished = 1;

	MQTTProperties_free(&opts.properties);
}

void test_error_reporting_onConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;
	char* topics[2] = {test_error_reporting_globals.test_topic, "+"};
	int qoss[2] = {2, 2};
	MQTTSubscribe_options subopts[2] = {MQTTSubscribe_options_initializer, MQTTSubscribe_options_initializer};
	MQTTProperty property;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_SUCCESS,
		   "Reason code was %d\n", response->reasonCode);

	MyLog(LOGA_DEBUG, "Connack properties:");
	logProperties(&response->properties);

	opts.onSuccess5 = test_error_reporting_onSubscribe;
	opts.context = c;

	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "test user property";
	property.value.data.len = (int)strlen(property.value.data.data);
	property.value.value.data = "test user property value";
	property.value.value.len = (int)strlen(property.value.value.data);
	MQTTProperties_add(&opts.properties, &property);

	opts.subscribeOptionsCount = 2;
	opts.subscribeOptionsList = subopts;

	rc = MQTTAsync_subscribeMany(c, 2, topics, qoss, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_flow_control_globals.test_finished = 1;

	MQTTProperties_free(&opts.properties);
}


int test_error_reporting(struct Options options)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	int rc = 0;

	MyLog(LOGA_INFO, "Starting V5 test - error reporting");
	fprintf(xml, "<testcase classname=\"test11\" name=\"error reporting\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "error reporting",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test_flow_control_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.MQTTVersion = options.MQTTVersion;
	opts.onSuccess5 = test_error_reporting_onConnect;
	opts.context = c;
	opts.cleanstart = 1;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (test_error_reporting_globals.test_finished == 0)
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


struct
{
	int test_finished;
	char* test_topic;
} test_qos_1_2_errors_globals =
{
	0, "test_qos_1_2_errors"
};


void test_qos_1_2_errors_onPublishSuccess(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_DEBUG, "Callback: publish success, reason code \"%s\" msgid: %d",
			MQTTReasonCode_toString(response->reasonCode), response->token);

	logProperties(&response->properties);

	test_qos_1_2_errors_globals.test_finished = 1;
}


void test_qos_1_2_errors_onPublishFailure3(void* context, MQTTAsync_failureData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

	MyLog(LOGA_DEBUG, "Callback: publish failure, reason code \"%s\" msgid: %d packet type: %d",
			MQTTReasonCode_toString(response->reasonCode), response->token, response->packet_type);

	logProperties(&response->properties);

	test_qos_1_2_errors_globals.test_finished = 1;
}


void test_qos_1_2_errors_onPublishFailure2(void* context, MQTTAsync_failureData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTProperty property;
	int rc;

	MyLog(LOGA_DEBUG, "Callback: publish failure, reason code \"%s\" msgid: %d packet type: %d",
			MQTTReasonCode_toString(response->reasonCode), response->token, response->packet_type);

	logProperties(&response->properties);

	opts.onSuccess5 = test_qos_1_2_errors_onPublishSuccess;
	opts.onFailure5 = test_qos_1_2_errors_onPublishFailure3;
	opts.context = c;

	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.qos = 2;
	pubmsg.retained = 0;

	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "pub user property";
	property.value.data.len = (int)strlen(property.value.data.data);
	property.value.value.data = "pub user property value";
	property.value.value.len = (int)strlen(property.value.value.data);
	MQTTProperties_add(&pubmsg.properties, &property);

	rc = MQTTAsync_sendMessage(c, "test_qos_1_2_errors_pubcomp", &pubmsg, &opts);
	assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTREASONCODE_SUCCESS)
		test_qos_1_2_errors_globals.test_finished = 1;

	MQTTProperties_free(&pubmsg.properties);
}


void test_qos_1_2_errors_onPublishFailure(void* context, MQTTAsync_failureData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTProperty property;
	int rc;

	MyLog(LOGA_DEBUG, "Callback: publish failure, reason code \"%s\" msgid: %d packet type: %d",
			MQTTReasonCode_toString(response->reasonCode), response->token, response->packet_type);

	logProperties(&response->properties);

	opts.onSuccess5 = test_qos_1_2_errors_onPublishSuccess;
	opts.onFailure5 = test_qos_1_2_errors_onPublishFailure2;
	opts.context = c;

	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.qos = 2;
	pubmsg.retained = 0;

	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "pub user property";
	property.value.data.len = (int)strlen(property.value.data.data);
	property.value.value.data = "pub user property value";
	property.value.value.len = (int)strlen(property.value.value.data);
	MQTTProperties_add(&pubmsg.properties, &property);

	rc = MQTTAsync_sendMessage(c, test_qos_1_2_errors_globals.test_topic, &pubmsg, &opts);
	assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTREASONCODE_SUCCESS)
		test_qos_1_2_errors_globals.test_finished = 1;

	MQTTProperties_free(&pubmsg.properties);
}


void test_qos_1_2_errors_onConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTProperty property;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_SUCCESS,
		   "Reason code was %d\n", response->reasonCode);

	MyLog(LOGA_DEBUG, "Connack properties:");
	logProperties(&response->properties);

	opts.onSuccess5 = test_qos_1_2_errors_onPublishSuccess;
	opts.onFailure5 = test_qos_1_2_errors_onPublishFailure;
	opts.context = c;

	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.qos = 1;
	pubmsg.retained = 0;

	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "pub user property";
	property.value.data.len = (int)strlen(property.value.data.data);
	property.value.value.data = "pub user property value";
	property.value.value.len = (int)strlen(property.value.value.data);
	MQTTProperties_add(&pubmsg.properties, &property);

	rc = MQTTAsync_sendMessage(c, test_qos_1_2_errors_globals.test_topic, &pubmsg, &opts);
	assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTREASONCODE_SUCCESS)
		test_qos_1_2_errors_globals.test_finished = 1;

	MQTTProperties_free(&pubmsg.properties);
}


int test_qos_1_2_errors(struct Options options)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	int rc = 0;

	MyLog(LOGA_INFO, "Starting V5 test - qos 1 and 2 errors");
	fprintf(xml, "<testcase classname=\"test11\" name=\"qos 1 and 2 errors\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "qos 1 and 2 errors",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test_flow_control_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.MQTTVersion = options.MQTTVersion;
	opts.onSuccess5 = test_qos_1_2_errors_onConnect;
	opts.context = c;
	opts.cleanstart = 1;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (test_qos_1_2_errors_globals.test_finished == 0)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	MQTTAsync_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST6: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}

struct
{
	int test_finished;
	char* response_topic;
	char* request_topic;
	char* correlation_id;
	int messages_arrived;
} test_request_response_globals =
{
	0,
	"test_7_request",
	"test_7_response",
	"test_7_correlation_id",
	0
};


int test_request_response_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;
	int rc;

	test_request_response_globals.messages_arrived++;

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	if (message->struct_version == 1)
		logProperties(&message->properties);

	if (test_request_response_globals.messages_arrived == 1)
	{
		/* this is the request */
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
		MQTTProperty *corr_prop = NULL, *response_topic_prop = NULL;
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		MQTTProperty property;
		char myTopicName[100];

		assert("Topic should be request",
				strcmp(test_request_response_globals.request_topic, topicName) == 0,
				"topic was %s\n", topicName);

		if (MQTTProperties_hasProperty(&message->properties, MQTTPROPERTY_CODE_RESPONSE_TOPIC))
			response_topic_prop = MQTTProperties_getProperty(&message->properties, MQTTPROPERTY_CODE_RESPONSE_TOPIC);

		assert("Topic should be response",
		strncmp(test_request_response_globals.response_topic, response_topic_prop->value.data.data,
				response_topic_prop->value.data.len) == 0,
			"topic was %.4s\n", response_topic_prop->value.data.data);

		if (MQTTProperties_hasProperty(&message->properties, MQTTPROPERTY_CODE_CORRELATION_DATA))
			corr_prop = MQTTProperties_getProperty(&message->properties, MQTTPROPERTY_CODE_CORRELATION_DATA);

		assert("Correlation data should be",
		strncmp(test_request_response_globals.correlation_id, corr_prop->value.data.data,
				corr_prop->value.data.len) == 0,
			"Correlation data was %.4s\n", corr_prop->value.data.data);

		/* send response */
		pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.retained = 0;
		pubmsg.qos = 1;

		property.identifier = MQTTPROPERTY_CODE_CORRELATION_DATA;
		property.value.data.data = test_request_response_globals.correlation_id;
		property.value.data.len = (int)strlen(property.value.data.data);
		MQTTProperties_add(&pubmsg.properties, &property);

		memcpy(myTopicName, response_topic_prop->value.data.data, response_topic_prop->value.data.len);
		myTopicName[response_topic_prop->value.data.len] = '\0';
		rc = MQTTAsync_sendMessage(c, myTopicName, &pubmsg, &opts);
		if (rc != MQTTREASONCODE_SUCCESS)
			test_request_response_globals.test_finished = 1;
		assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

		MQTTProperties_free(&pubmsg.properties);
	}
	else if (test_request_response_globals.messages_arrived == 2)
	{
		MQTTProperty *corr_prop = NULL;

		/* this should be response */
		assert("Topic should be response",
				strcmp(test_request_response_globals.response_topic, topicName) == 0,
				"topic was %s\n", topicName);

		if (MQTTProperties_hasProperty(&message->properties, MQTTPROPERTY_CODE_CORRELATION_DATA))
			corr_prop = MQTTProperties_getProperty(&message->properties, MQTTPROPERTY_CODE_CORRELATION_DATA);

		assert("Correlation data should be",
		strncmp(test_request_response_globals.correlation_id, corr_prop->value.data.data,
				corr_prop->value.data.len) == 0,
			"Correlation data was %.4s\n", corr_prop->value.data.data);

		test_request_response_globals.test_finished = 1;
	}

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


void test_request_response_onSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTProperty property;
	char* topics[2] = {test_error_reporting_globals.test_topic, "+"};
	int rc;
	int i = 0;

	MyLog(LOGA_DEBUG, "Suback properties:");
	logProperties(&response->properties);

	assert("Reason code count should be 2", response->alt.sub.reasonCodeCount == 2,
		   "Reason code count was %d\n", response->alt.sub.reasonCodeCount);

	if (response->alt.sub.reasonCodeCount == 1)
		MyLog(LOGA_DEBUG, "reason code %d", response->reasonCode);
	else if (response->alt.sub.reasonCodeCount > 1)
	{
		for (i = 0; i < response->alt.sub.reasonCodeCount; ++i)
		{
			MyLog(LOGA_DEBUG, "Subscribe reason code %d", response->alt.sub.reasonCodes[i]);
			assert("Reason code should be 2", response->alt.sub.reasonCodes[i] == MQTTREASONCODE_GRANTED_QOS_2,
				   "Reason code was %d\n", response->alt.sub.reasonCodes[i]);
		}
	}

	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.retained = 0;
	pubmsg.qos = 1;

	property.identifier = MQTTPROPERTY_CODE_RESPONSE_TOPIC;
	property.value.data.data = test_request_response_globals.response_topic;
	property.value.data.len = (int)strlen(property.value.data.data);
	MQTTProperties_add(&pubmsg.properties, &property);

	property.identifier = MQTTPROPERTY_CODE_CORRELATION_DATA;
	property.value.data.data = test_request_response_globals.correlation_id;
	property.value.data.len = (int)strlen(property.value.data.data);
	MQTTProperties_add(&pubmsg.properties, &property);

	rc = MQTTAsync_sendMessage(c, test_request_response_globals.request_topic, &pubmsg, &opts);
	if (rc != MQTTREASONCODE_SUCCESS)
		test_request_response_globals.test_finished = 1;
	assert("Good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MQTTProperties_free(&pubmsg.properties);
}


void test_request_response_onConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;
	char* topics[2] = {test_request_response_globals.request_topic,
					test_request_response_globals.response_topic};
	int qos[2] = {2, 2};

	MyLog(LOGA_DEBUG, "In request response connect onSuccess callback, context %p", context);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_SUCCESS,
		   "Reason code was %d\n", response->reasonCode);

	MyLog(LOGA_DEBUG, "Connack properties:");
	logProperties(&response->properties);

	opts.onSuccess5 = test_request_response_onSubscribe;
	opts.context = c;
	rc = MQTTAsync_subscribeMany(c, 2, topics, qos, &opts);
	if (rc != MQTTREASONCODE_SUCCESS)
		test_request_response_globals.test_finished = 1;
	assert("Good rc from subscribeMany", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


int test_request_response(struct Options options)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	int rc = 0;

	MyLog(LOGA_INFO, "Starting V5 test - request response");
	fprintf(xml, "<testcase classname=\"test11\" name=\"request response\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "request response",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test_request_response_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.MQTTVersion = options.MQTTVersion;
	opts.onSuccess5 = test_request_response_onConnect;
	opts.context = c;
	opts.cleanstart = 1;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (test_request_response_globals.test_finished == 0)
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


struct
{
	int test_finished;
	char* topic1;
	char* topic2;
	int messages_arrived;
} test_subscribeOptions_globals =
{
	0,
	"subscribe options topic",
	"+",
	0
};


int test_subscribeOptions_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;

	test_subscribeOptions_globals.messages_arrived++;

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	if (message->struct_version == 1)
		logProperties(&message->properties);

	if (test_subscribeOptions_globals.messages_arrived == 1)
	{
		int subsidcount, subsid;

		subsidcount = MQTTProperties_propertyCount(&message->properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER);
		assert("Subsidcount is i", subsidcount == 1, "subsidcount is not correct %d\n", subsidcount);

		subsid = (int)MQTTProperties_getNumericValueAt(&message->properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER, 0);
		assert("Subsid is 2", subsid == 2, "subsid is not correct %d\n", subsid);

		test_subscribeOptions_globals.test_finished = 1;
	}

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


void test_subscribeOptions_onSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;
	static int called = 0;

	MyLog(LOGA_DEBUG, "In subscribe options connect onSuccess callback, context %p", context);
	called++;

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_GRANTED_QOS_2,
		   "Reason code was %d\n", response->reasonCode);

	if (response->properties.count > 0)
	{
		MyLog(LOGA_DEBUG, "Suback properties:");
		logProperties(&response->properties);
	}

	if (called == 1)
	{
		MQTTProperty property;

		property.identifier = MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER;
		property.value.integer4 = 2;
		MQTTProperties_add(&opts.properties, &property);

		opts.onSuccess5 = test_subscribeOptions_onSubscribe;
		opts.context = c;
		opts.subscribeOptions.retainHandling = 2;
		opts.subscribeOptions.retainAsPublished = 1;
		rc = MQTTAsync_subscribe(c, test_subscribeOptions_globals.topic2, 2, &opts);
		if (rc != MQTTREASONCODE_SUCCESS)
			test_subscribeOptions_globals.test_finished = 1;
		assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

		MQTTProperties_free(&opts.properties);
	}
	else if (called == 2)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;

		pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.retained = 0;
		pubmsg.qos = 2;

		rc = MQTTAsync_sendMessage(c, test_subscribeOptions_globals.topic1, &pubmsg, &opts);
		assert("Good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		if (rc != MQTTASYNC_SUCCESS)
			test_subscribeOptions_globals.test_finished = 1;
	}
}


void test_subscribeOptions_onConnect(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTProperty property;
	int rc;

	MyLog(LOGA_DEBUG, "In subscribe options connect onSuccess callback, context %p", context);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_SUCCESS,
		   "Reason code was %d\n", response->reasonCode);

	MyLog(LOGA_DEBUG, "Connack properties:");
	logProperties(&response->properties);

	property.identifier = MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER;
	property.value.integer4 = 1;
	MQTTProperties_add(&opts.properties, &property);
	opts.subscribeOptions.noLocal = 1;

	opts.onSuccess5 = test_subscribeOptions_onSubscribe;
	opts.context = c;
	rc = MQTTAsync_subscribe(c, test_subscribeOptions_globals.topic1, 2, &opts);
	if (rc != MQTTREASONCODE_SUCCESS)
		test_request_response_globals.test_finished = 1;
	assert("Good rc from subscribeMany", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MQTTProperties_free(&opts.properties);
}


int test_subscribeOptions(struct Options options)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	int rc = 0;

	MyLog(LOGA_INFO, "Starting V5 test - subscribe options");
	fprintf(xml, "<testcase classname=\"test11\" name=\"subscribe options\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "subscribe options",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test_subscribeOptions_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.MQTTVersion = options.MQTTVersion;
	opts.onSuccess5 = test_subscribeOptions_onConnect;
	opts.context = c;
	opts.cleanstart = 1;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (test_subscribeOptions_globals.test_finished == 0)
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


struct
{
	char* shared_topic;
	char* topic;
	int messages_arrived;
	int test_finished;
	int message_count;
} test_shared_subscriptions_globals =
{
	"$share/share_test/any",
	"any",
	0,
	0,
	10,
};


int test_shared_subscriptions_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	MQTTAsync c = (MQTTAsync)context;

	test_shared_subscriptions_globals.messages_arrived++;

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	if (message->struct_version == 1)
		logProperties(&message->properties);

	if (test_shared_subscriptions_globals.message_count == test_shared_subscriptions_globals.messages_arrived)
		test_shared_subscriptions_globals.test_finished = 1;

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


void test_shared_subscriptions_onSubscribe(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;
	static int called = 0;

	called++;
	MyLog(LOGA_DEBUG, "In subscribe options connect onSuccess callback, context %p, called %d", context, called);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_GRANTED_QOS_2,
		   "Reason code was %d\n", response->reasonCode);

	if (response->properties.count > 0)
	{
		MyLog(LOGA_DEBUG, "Suback properties:");
		logProperties(&response->properties);
	}

	if (called == 2) /* both clients now connected and subscribed */
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		int i = 0;
		char buf[100];

		/* for each message we publish, only one of the subscribers should get it, not both */
		sprintf(buf, "shared subscriptions sequence number %d", i);
		pubmsg.payload = buf;
		pubmsg.payloadlen = (int)strlen(buf);
		pubmsg.retained = 0;
		pubmsg.qos = 2;

		for (i = 0; i < test_shared_subscriptions_globals.message_count; ++i)
		{
			rc = MQTTAsync_sendMessage(c, test_shared_subscriptions_globals.topic, &pubmsg, &opts);
			assert("Good rc from send", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
			if (rc != MQTTASYNC_SUCCESS)
			{
				test_subscribeOptions_globals.test_finished = 1;
				break;
			}
		}
	}
}


void test_shared_subscriptions_onConnectd(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync d = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In shared subscriptions connect d onSuccess callback, context %p", context);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_SUCCESS,
		   "Reason code was %d\n", response->reasonCode);

	opts.onSuccess5 = test_shared_subscriptions_onSubscribe;
	opts.context = d;
	rc = MQTTAsync_subscribe(d, test_shared_subscriptions_globals.shared_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTREASONCODE_SUCCESS, "rc was %d", rc);
	if (rc != MQTTREASONCODE_SUCCESS)
		test_shared_subscriptions_globals.test_finished = 1;
}


void test_shared_subscriptions_onConnectc(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In shared subscriptions connect c onSuccess callback, context %p", context);

	assert("Reason code should be 0", response->reasonCode == MQTTREASONCODE_SUCCESS,
		   "Reason code was %d\n", response->reasonCode);

	opts.onSuccess5 = test_shared_subscriptions_onSubscribe;
	opts.context = c;
	rc = MQTTAsync_subscribe(c, test_shared_subscriptions_globals.shared_topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTREASONCODE_SUCCESS, "rc was %d", rc);
	if (rc != MQTTREASONCODE_SUCCESS)
		test_shared_subscriptions_globals.test_finished = 1;
}


int test_shared_subscriptions(struct Options options)
{
	MQTTAsync c, d;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	int rc = 0, count = 0;

	MyLog(LOGA_INFO, "Starting V5 test - shared subscriptions");
	fprintf(xml, "<testcase classname=\"test11\" name=\"shared subscriptions\"");
	global_start_time = start_clock();

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&c, options.connection, "shared subscriptions c",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_createWithOptions(&d, options.connection, "shared subscriptions d",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, c, NULL, test_shared_subscriptions_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	rc = MQTTAsync_setCallbacks(d, d, NULL, test_shared_subscriptions_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	opts.MQTTVersion = options.MQTTVersion;
	opts.onSuccess5 = test_shared_subscriptions_onConnectc;
	opts.context = c;
	opts.cleanstart = 1;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	opts.onSuccess5 = test_shared_subscriptions_onConnectd;
	opts.context = d;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(d, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (test_shared_subscriptions_globals.test_finished == 0 && ++count < 1000)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	/* sleep a bit more to see if any other messages arrive */
	#if defined(_WIN32)
		Sleep(100);
	#else
		usleep(10000L);
	#endif
	assert("Correct number of messages arrived",
			test_shared_subscriptions_globals.message_count == test_shared_subscriptions_globals.messages_arrived,
			"Actual number of messages received %d\n", test_shared_subscriptions_globals.messages_arrived);

	MQTTAsync_destroy(&c);
	MQTTAsync_destroy(&d);

exit:
	MyLog(LOGA_INFO, "TEST9: test %s. %d tests run, %d failures.",
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
 	int (*tests[])(struct Options) = {NULL,
 		test_client_topic_aliases,
		test_server_topic_aliases,
		test_subscription_ids,
		test_flow_control,
		test_error_reporting,
		test_qos_1_2_errors,
		test_request_response,
		test_subscribeOptions,
		test_shared_subscriptions
 	}; /* indexed starting from 1 */
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
