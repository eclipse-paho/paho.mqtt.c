/*******************************************************************************
 * Copyright (c) 2009, 2023 IBM Corp. and others
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
 *    Ian Craggs - MQTT 5.0 support
 *******************************************************************************/


/**
 * @file
 * MQTT V5 specific tests for the MQTT C client
 *
 *  - topic aliases
 *  - subscription ids
 *  - session expiry
 *  - payload format
 *  - flow control
 *  - QoS 2 exchange termination
 *  - request/response
 *  - shared subscriptions
 *  - server initiated disconnect
 *  - auth packets
 *  - server assigned clientid returned in a property
 *  - server defined keepalive
 *  - subscribe failure
 */


#include "MQTTClient.h"
#include <string.h>
#include <stdlib.h>

#if !defined(_WINDOWS)
  #include <sys/time.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #include <errno.h>
#else
  #include <windows.h>
  #define setenv(a, b, c) _putenv_s(a, b)
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
	char** haconnections;
	char* proxy_connection;
	int hacount;
	int verbose;
	int test_no;
	int MQTTVersion;
	int iterations;
} options =
{
	"tcp://localhost:1883",
	NULL,
	"tcp://localhost:1884",
	0,
	0,
	0,
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
		else if (strcmp(argv[count], "--connection") == 0)
		{
			if (++count < argc)
			{
				options.connection = argv[count];
				printf("\nSetting connection to %s\n", options.connection);
			}
			else
				usage();
		}
		else if (strcmp(argv[count], "--haconnections") == 0)
		{
			if (++count < argc)
			{
				char* tok = strtok(argv[count], " ");
				options.hacount = 0;
				options.haconnections = malloc(sizeof(char*) * 5);
				while (tok)
				{
					options.haconnections[options.hacount] = malloc(strlen(tok) + 1);
					strcpy(options.haconnections[options.hacount], tok);
					options.hacount++;
					tok = strtok(NULL, " ");
				}
			}
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
		{
			options.verbose = 1;
			printf("\nSetting verbose on\n");
		}
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
		MyLog(LOGA_INFO, "Assertion failed, file %s, line %d, description: %s\n", filename, lineno, description);

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
		  MyLog(LOGA_INFO, "Property name value %s %.*s", name,
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

struct
{
	int disconnected;
} test_topic_aliases_globals =
{
	0,
};

void disconnected(void* context, MQTTProperties* props, enum MQTTReasonCodes rc)
{
	MQTTClient c = (MQTTClient)context;
	MyLog(LOGA_INFO, "Callback: disconnected, reason code \"%s\"", MQTTReasonCode_toString(rc));
	logProperties(props);
	test_topic_aliases_globals.disconnected = 1;
}

static int messages_arrived = 0;

int messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	if (message->struct_version == 1)
	{
		const int props_count = 0;

		assert("Properties count should be 0", message->properties.count == props_count,
			"Properties count was %d\n", message->properties.count);
		logProperties(&message->properties);
	}
	messages_arrived++;

	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&message);
	return 1;
}


int test_client_topic_aliases(struct Options options)
{
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTClient_willOptions wopts = MQTTClient_willOptions_initializer;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperties connect_props = MQTTProperties_initializer;
	MQTTProperty property;
	MQTTSubscribe_options subopts = MQTTSubscribe_options_initializer;
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	MQTTResponse response = MQTTResponse_initializer;
	MQTTClient_deliveryToken dt;
	int rc = 0;
	int count = 0;
	char* test_topic = "test_client_topic_aliases";
	int topicAliasMaximum = 0;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;

	fprintf(xml, "<testcase classname=\"test_client_topic_aliases\" name=\"client topic aliases\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test 1 - client topic aliases");

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "client_topic_alias_test",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
	{
		MQTTClient_destroy(&c);
		goto exit;
	}

	rc = MQTTClient_setCallbacks(c, NULL, NULL, messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_setDisconnected(c, c, disconnected);
	assert("Good rc from setDisconnected", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = options.MQTTVersion;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	MyLog(LOGA_DEBUG, "Connecting");
	response = MQTTClient_connect5(c, &opts, NULL, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	if (response.properties)
	{
		logProperties(response.properties);
		MQTTResponse_free(response);
	}

	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.qos = 1;
	pubmsg.retained = 0;

	/* a Topic Alias of 0 is not allowed, so we should be disconnected */
	property.identifier = MQTTPROPERTY_CODE_TOPIC_ALIAS;
	property.value.integer2 = 0;
	MQTTProperties_add(&pubmsg.properties, &property);

	response = MQTTClient_publishMessage5(c, test_topic, &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);

	/* Now we expect to receive a disconnect packet telling us why */
	count = 0;
	while (test_topic_aliases_globals.disconnected == 0 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("Disconnected should be called", test_topic_aliases_globals.disconnected == 1,
			"was %d", test_topic_aliases_globals.disconnected);

	property.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
	property.value.integer4 = 30;
	MQTTProperties_add(&connect_props, &property);

	/* Now try a valid topic alias */
	response = MQTTClient_connect5(c, &opts, &connect_props, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	if (response.properties)
	{
		if (MQTTProperties_hasProperty(response.properties, MQTTPROPERTY_CODE_TOPIC_ALIAS_MAXIMUM))
			topicAliasMaximum = MQTTProperties_getNumericValue(response.properties, MQTTPROPERTY_CODE_TOPIC_ALIAS_MAXIMUM);

		logProperties(response.properties);
		MQTTResponse_free(response);
	}
	assert("topicAliasMaximum > 0", topicAliasMaximum > 0, "topicAliasMaximum was %d", topicAliasMaximum);

	/* subscribe to a topic */
	response = MQTTClient_subscribe5(c, test_topic, 2, NULL, NULL);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);

	/* then publish to the topic */
	MQTTProperties_free(&pubmsg.properties);
	property.identifier = MQTTPROPERTY_CODE_TOPIC_ALIAS;
	property.value.integer2 = 1;
	MQTTProperties_add(&pubmsg.properties, &property);

	messages_arrived = 0;
	response = MQTTClient_publishMessage5(c, test_topic, &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);

	/* should get a response */
	while (messages_arrived == 0 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("1 message should have arrived", messages_arrived == 1, "was %d", messages_arrived);

	/* now publish to the topic alias only */
	messages_arrived = 0;
	response = MQTTClient_publishMessage5(c, "", &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);

	/* should get a response */
	while (messages_arrived == 0 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("1 message should have arrived", messages_arrived == 1, "was %d", messages_arrived);

	rc = MQTTClient_disconnect5(c, 1000, MQTTREASONCODE_SUCCESS, NULL);

	/* Reconnect.  Topic aliases should be deleted, but not subscription */
	opts.cleanstart = 0;
	response = MQTTClient_connect5(c, &opts, NULL, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	MQTTResponse_free(response);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	/* then publish to the topic */
	MQTTProperties_free(&pubmsg.properties);
	messages_arrived = 0;
	response = MQTTClient_publishMessage5(c, test_topic, &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);

	/* should get a response */
	while (messages_arrived == 0 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("1 message should have arrived", messages_arrived == 1, "was %d", messages_arrived);

	/* now publish to the topic alias only */
	test_topic_aliases_globals.disconnected = 0;
	messages_arrived = 0;
	property.identifier = MQTTPROPERTY_CODE_TOPIC_ALIAS;
	property.value.integer2 = 1;
	MQTTProperties_add(&pubmsg.properties, &property);
	response = MQTTClient_publishMessage5(c, "", &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);

	/* should not get a response */
	while (messages_arrived == 0 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("No message should have arrived", messages_arrived == 0, "was %d", messages_arrived);

	/* Now we expect to receive a disconnect packet telling us why */
	count = 0;
	while (test_topic_aliases_globals.disconnected == 0 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("Disconnected should be called", test_topic_aliases_globals.disconnected == 1,
			"was %d", test_topic_aliases_globals.disconnected);

	MQTTProperties_free(&pubmsg.properties);
	MQTTProperties_free(&props);
	MQTTProperties_free(&connect_props);
	MQTTClient_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST1: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}



int test2_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	static int received = 0;
	static int first_topic_alias = 0;
	int topicAlias = 0;

	received++;
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	if (message->struct_version == 1)
	{
		const int props_count = 0;

		if (MQTTProperties_hasProperty(&message->properties, MQTTPROPERTY_CODE_TOPIC_ALIAS))
			topicAlias = MQTTProperties_getNumericValue(&message->properties, MQTTPROPERTY_CODE_TOPIC_ALIAS);

		if (received == 1)
			first_topic_alias = topicAlias;
		else
			assert("All topic aliases should be the same", topicAlias == first_topic_alias,
					"Topic alias was %d\n", topicAlias);

		assert("topicAlias should not be 0", topicAlias > 0, "Topic alias was %d\n", topicAlias);
		logProperties(&message->properties);
	}
	messages_arrived++;

	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&message);
	return 1;
}


int test_server_topic_aliases(struct Options options)
{
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTClient_willOptions wopts = MQTTClient_willOptions_initializer;
	MQTTProperties connect_props = MQTTProperties_initializer;
	MQTTProperty property;
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	MQTTResponse response = MQTTResponse_initializer;
	MQTTClient_deliveryToken dt;
	int rc = 0;
	int count = 0;
	char* test_topic = "test_server_topic_aliases";
	int topicAliasMaximum = 0;
	int qos = 0;
	const int msg_count = 3;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;

	fprintf(xml, "<testcase classname=\"test_server_topic_aliases\" name=\"server topic aliases\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test 2 - server topic aliases");

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "server_topic_alias_test",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
	{
		MQTTClient_destroy(&c);
		goto exit;
	}

	rc = MQTTClient_setCallbacks(c, NULL, NULL, test2_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = options.MQTTVersion;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	/* Allow at least one server topic alias */
	property.identifier = MQTTPROPERTY_CODE_TOPIC_ALIAS_MAXIMUM;
	property.value.integer2 = 1;
	MQTTProperties_add(&connect_props, &property);

	MyLog(LOGA_DEBUG, "Connecting");
	response = MQTTClient_connect5(c, &opts, &connect_props, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	if (response.properties)
	{
		if (MQTTProperties_hasProperty(response.properties, MQTTPROPERTY_CODE_TOPIC_ALIAS_MAXIMUM))
			topicAliasMaximum = MQTTProperties_getNumericValue(response.properties, MQTTPROPERTY_CODE_TOPIC_ALIAS_MAXIMUM);

		logProperties(response.properties);
		MQTTResponse_free(response);
	}

	/* subscribe to a topic */
	response = MQTTClient_subscribe5(c, test_topic, 2, NULL, NULL);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);

	messages_arrived = 0;
	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.retained = 0;
	for (qos = 0; qos < msg_count; ++qos)
	{
		pubmsg.qos = qos;
		response = MQTTClient_publishMessage5(c, test_topic, &pubmsg, &dt);
		assert("Good rc from publish", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	}

	/* should get responses */
	while (messages_arrived < msg_count && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("3 messages should have arrived", messages_arrived == msg_count, "was %d", messages_arrived);

	rc = MQTTClient_disconnect5(c, 1000, MQTTREASONCODE_SUCCESS, NULL);

	MQTTProperties_free(&pubmsg.properties);
	MQTTProperties_free(&connect_props);
	MQTTClient_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST2: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}



int test_subscription_ids_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	static int received = 0;
	static int first_topic_alias = 0;
	int topicAlias = 0;

	received++;
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	if (message->struct_version == 1)
	{
		int subsidcount = 0, i = 0;

		subsidcount = MQTTProperties_propertyCount(&message->properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER);

		for (i = 0; i < subsidcount; ++i)
		{
			int subsid = MQTTProperties_getNumericValueAt(&message->properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER, i);
			assert("Subsid is i+1", subsid == i+1, "subsid is not correct %d\n", subsid);
		}
		logProperties(&message->properties);
	}
	messages_arrived++;

	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&message);
	return 1;
}


int test_subscription_ids(struct Options options)
{
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTProperties connect_props = MQTTProperties_initializer;
	MQTTProperties subs_props = MQTTProperties_initializer;
	MQTTProperty property;
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	MQTTResponse response = MQTTResponse_initializer;
	MQTTClient_deliveryToken dt;
	int rc = 0;
	int count = 0;
	char* test_topic = "test_subscription_ids";
	const int msg_count = 1;
	int subsids = 1;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;

	fprintf(xml, "<testcase classname=\"test_subscription_ids\" name=\"subscription ids\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test 3 - subscription ids");

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "subscription_ids",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
	{
		MQTTClient_destroy(&c);
		goto exit;
	}

	rc = MQTTClient_setCallbacks(c, NULL, NULL, test_subscription_ids_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = options.MQTTVersion;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	MyLog(LOGA_DEBUG, "Connecting");
	response = MQTTClient_connect5(c, &opts, &connect_props, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	if (response.properties)
	{
		if (MQTTProperties_hasProperty(response.properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIERS_AVAILABLE))
			subsids = MQTTProperties_getNumericValue(response.properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIERS_AVAILABLE);

		logProperties(response.properties);
		MQTTResponse_free(response);
	}
	assert("Subscription ids must be available", subsids == 1, "subsids is %d", subsids);

	/* subscribe to the test topic */
	property.identifier = MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER;
	property.value.integer4 = 1;
	MQTTProperties_add(&subs_props, &property);
	response = MQTTClient_subscribe5(c, test_topic, 2, NULL, &subs_props);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);

	/* now to an overlapping topic */
	property.value.integer4 = 2;
	subs_props.array[0].value.integer4 = 2;
	response = MQTTClient_subscribe5(c, "+", 2, NULL, &subs_props);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);

	messages_arrived = 0;
	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.retained = 0;
	pubmsg.qos = 2;

	response = MQTTClient_publishMessage5(c, test_topic, &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);

	/* should get responses */
	while (messages_arrived < msg_count && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("1 message should have arrived", messages_arrived == msg_count, "was %d", messages_arrived);

	rc = MQTTClient_disconnect5(c, 1000, MQTTREASONCODE_SUCCESS, NULL);

	MQTTProperties_free(&pubmsg.properties);
	MQTTProperties_free(&subs_props);
	MQTTProperties_free(&connect_props);
	MQTTClient_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST3: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}


int test_flow_control_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	static int received = 0;
	static int first_topic_alias = 0;
	int topicAlias = 0;

	received++;
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	messages_arrived++;

	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&message);
	return 1;
}

static int blocking_found = 0;

void test_flow_control_trace_callback(enum MQTTCLIENT_TRACE_LEVELS level, char* message)
{
	static char* msg = "Blocking publish on queue full";

	if (strstr(message, msg) != NULL)
		blocking_found = 1;
}


int test_flow_control(struct Options options)
{
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTProperties connect_props = MQTTProperties_initializer;
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	MQTTResponse response = MQTTResponse_initializer;
	MQTTClient_deliveryToken dt;
	int rc = 0, i = 0, count = 0;
	char* test_topic = "test_flow_control";
	int receive_maximum = 65535;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;

	fprintf(xml, "<testcase classname=\"test_flow_control\" name=\"flow control\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test - flow control");

	MQTTClient_setTraceLevel(MQTTCLIENT_TRACE_MINIMUM); /* to get the blocking trace message */

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "flow_control",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	rc = MQTTClient_setCallbacks(c, NULL, NULL, test_flow_control_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = options.MQTTVersion;
	opts.reliable = 0;
	opts.maxInflightMessages = 100;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	MyLog(LOGA_DEBUG, "Connecting");
	response = MQTTClient_connect5(c, &opts, &connect_props, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	if (response.properties)
	{
		if (MQTTProperties_hasProperty(response.properties, MQTTPROPERTY_CODE_RECEIVE_MAXIMUM))
			receive_maximum = MQTTProperties_getNumericValue(response.properties, MQTTPROPERTY_CODE_RECEIVE_MAXIMUM);

		logProperties(response.properties);
		MQTTResponse_free(response);
	}

	response = MQTTClient_subscribe5(c, test_topic, 2, NULL, NULL);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);

	messages_arrived = 0;
	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.retained = 0;
	pubmsg.qos = 2;
	for (i = 0; i < receive_maximum + 2; ++i)
	{
		response = MQTTClient_publishMessage5(c, test_topic, &pubmsg, &dt);
		assert("Good rc from publish", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	}

	/* should get responses */
	while (messages_arrived < receive_maximum + 2 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("messages should have arrived", messages_arrived == receive_maximum + 2, "was %d", messages_arrived);
	assert("should have blocked", blocking_found == 1, "was %d\n", blocking_found);

	rc = MQTTClient_disconnect5(c, 1000, MQTTREASONCODE_SUCCESS, NULL);

exit:
	MQTTClient_setTraceCallback(NULL);
	MQTTProperties_free(&pubmsg.properties);
	MQTTProperties_free(&connect_props);
	MQTTClient_destroy(&c);

	MyLog(LOGA_INFO, "TEST3: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}


int test_error_reporting(struct Options options)
{
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperty property;
	MQTTResponse response = MQTTResponse_initializer;
	int rc = 0, i = 0, count = 0;
	char* test_topic = "test_error_reporting";
	int receive_maximum = 65535;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;

	fprintf(xml, "<testcase classname=\"test_error_reporting\" name=\"error reporting\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test - error reporting");

	//MQTTClient_setTraceCallback(test_flow_control_trace_callback);

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "error_reporting",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	rc = MQTTClient_setCallbacks(c, NULL, NULL, test_flow_control_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	opts.MQTTVersion = options.MQTTVersion;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	MyLog(LOGA_DEBUG, "Connecting");
	response = MQTTClient_connect5(c, &opts, NULL, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	if (response.properties)
	{
		if (MQTTProperties_hasProperty(response.properties, MQTTPROPERTY_CODE_RECEIVE_MAXIMUM))
			receive_maximum = MQTTProperties_getNumericValue(response.properties, MQTTPROPERTY_CODE_RECEIVE_MAXIMUM);

		logProperties(response.properties);
		MQTTResponse_free(response);
	}

	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "unsub user property";
	property.value.data.len = (int)strlen(property.value.data.data);
	property.value.value.data = "unsub user property value";
	property.value.value.len = (int)strlen(property.value.value.data);
	MQTTProperties_add(&props, &property);
	response = MQTTClient_subscribe5(c, test_topic, 2, NULL, &props);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);
	assert("Properties should exist", response.properties != NULL, "props was %p", response.properties);
	if (response.properties)
	{
		logProperties(response.properties);
		MQTTResponse_free(response);
	}

	response = MQTTClient_unsubscribe5(c, test_topic, &props);
	assert("Good rc from unsubscribe", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	assert("Properties should exist", response.properties != NULL, "props was %p", response.properties);
	if (response.properties)
	{
		logProperties(response.properties);
		MQTTResponse_free(response);
	}

	rc = MQTTClient_disconnect5(c, 1000, MQTTREASONCODE_SUCCESS, NULL);

exit:
	MQTTClient_setTraceCallback(NULL);
	MQTTProperties_free(&props);
	MQTTClient_destroy(&c);

	MyLog(LOGA_INFO, "TEST3: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}

struct
{
	int published;
	int packet_type;
	enum MQTTReasonCodes rc;
} test_qos_1_2_errors_globals =
{
	0, -1, MQTTREASONCODE_SUCCESS
};


void published(void* context, int msgid, int packet_type, MQTTProperties* props, enum MQTTReasonCodes rc)
{
	MQTTClient c = (MQTTClient)context;
	MyLog(LOGA_INFO, "Callback: published, reason code \"%s\" msgid: %d packet type: %d",
			MQTTReasonCode_toString(rc), msgid, packet_type);
	test_qos_1_2_errors_globals.packet_type = packet_type;
	test_qos_1_2_errors_globals.rc = rc;
	if (props)
	{
		MyLog(LOGA_INFO, "Callback: published, properties:");
		logProperties(props);
	}
	test_qos_1_2_errors_globals.published = 1;
}

void test_trace_callback(enum MQTTCLIENT_TRACE_LEVELS level, char* message)
{
	printf("%s\n", message);
}

enum msgTypes
{
	CONNECT = 1, CONNACK, PUBLISH, PUBACK, PUBREC, PUBREL,
	PUBCOMP, SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK,
	PINGREQ, PINGRESP, DISCONNECT, AUTH
};


int test_qos_1_2_errors(struct Options options)
{
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperty property;
	MQTTResponse response = MQTTResponse_initializer;
	MQTTClient_deliveryToken dt;
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	int rc = 0, i = 0, count = 0;
	char* test_topic = "test_qos_1_2_errors";
	int receive_maximum = 65535;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;

	fprintf(xml, "<testcase classname=\"test_qos_1_2_errors\" name=\"qos 1 2 errors\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test - qos 1 and 2 errors");

	//MQTTClient_setTraceCallback(test_trace_callback);

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "error_reporting",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	rc = MQTTClient_setCallbacks(c, NULL, NULL, test_flow_control_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_setPublished(c, c, published);
	assert("Good rc from setPublished", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	opts.MQTTVersion = options.MQTTVersion;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	MyLog(LOGA_DEBUG, "Connecting");
	response = MQTTClient_connect5(c, &opts, NULL, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	if (response.properties)
	{
		if (MQTTProperties_hasProperty(response.properties, MQTTPROPERTY_CODE_RECEIVE_MAXIMUM))
			receive_maximum = MQTTProperties_getNumericValue(response.properties, MQTTPROPERTY_CODE_RECEIVE_MAXIMUM);

		logProperties(response.properties);
		MQTTResponse_free(response);
	}

	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.qos = 1;
	pubmsg.retained = 0;

	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "unsub user property";
	property.value.data.len = (int)strlen(property.value.data.data);
	property.value.value.data = "unsub user property value";
	property.value.value.len = (int)strlen(property.value.value.data);
	MQTTProperties_add(&pubmsg.properties, &property);

	response = MQTTClient_publishMessage5(c, test_topic, &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d", response.reasonCode);

	count = 0;
	while (test_qos_1_2_errors_globals.published == 0 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("Published called", test_qos_1_2_errors_globals.published == 1,
			"published was %d", test_qos_1_2_errors_globals.published);
	assert("Reason code was packet identifier not found",
			test_qos_1_2_errors_globals.rc == MQTTREASONCODE_NOT_AUTHORIZED,
			"Reason code was %d", test_qos_1_2_errors_globals.rc);
	assert("Packet type was PUBACK", test_qos_1_2_errors_globals.packet_type == PUBACK,
			"packet type was %d", test_qos_1_2_errors_globals.packet_type);

	test_qos_1_2_errors_globals.published = 0;
	pubmsg.qos = 2;
	response = MQTTClient_publishMessage5(c, test_topic, &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);

	count = 0;
	while (test_qos_1_2_errors_globals.published == 0 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("Published called", test_qos_1_2_errors_globals.published == 1,
				"published was %d", test_qos_1_2_errors_globals.published);
	assert("Reason code was packet identifier not found",
			test_qos_1_2_errors_globals.rc == MQTTREASONCODE_NOT_AUTHORIZED,
			"Reason code was %d", test_qos_1_2_errors_globals.rc);
	assert("Packet type was PUBREC", test_qos_1_2_errors_globals.packet_type == PUBREC,
			"packet type was %d", test_qos_1_2_errors_globals.packet_type);

	test_qos_1_2_errors_globals.published = 0;
	response = MQTTClient_publishMessage5(c, "test_qos_1_2_errors_pubcomp", &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d", response.reasonCode);

	count = 0;
	while (test_qos_1_2_errors_globals.published == 0 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("Published called", test_qos_1_2_errors_globals.published == 1,
				"published was %d", test_qos_1_2_errors_globals.published);
	assert("Reason code was packet identifier not found",
			test_qos_1_2_errors_globals.rc == MQTTREASONCODE_PACKET_IDENTIFIER_NOT_FOUND,
			"Reason code was %d", test_qos_1_2_errors_globals.rc);
	assert("Packet type was PUBCOMP", test_qos_1_2_errors_globals.packet_type == PUBCOMP,
			"packet type was %d", test_qos_1_2_errors_globals.packet_type);

	rc = MQTTClient_disconnect5(c, 1000, MQTTREASONCODE_SUCCESS, NULL);

exit:
	MQTTClient_setTraceCallback(NULL);
	MQTTProperties_free(&props);
	MQTTProperties_free(&pubmsg.properties);
	MQTTClient_destroy(&c);

	MyLog(LOGA_INFO, "TEST6: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}


struct
{
	char* response_topic;
	char* request_topic;
	int messages_arrived;
	char* correlation_id;
} test_request_response_globals =
{
	"my response topic",
	"my request topic",
	0,
	"request no 1",
};


int test_request_response_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	if (message->struct_version == 1)
	{
		const int props_count = 0;

		MyLog(LOGA_INFO, "Message properties:");
		logProperties(&message->properties);
	}
	test_request_response_globals.messages_arrived++;

	if (test_request_response_globals.messages_arrived == 1)
	{
		MQTTProperty *prop;

		assert("Topic should be request",
				strcmp(test_request_response_globals.request_topic, topicName) == 0,
				"topic was %s\n", topicName);

		if (MQTTProperties_hasProperty(&message->properties, MQTTPROPERTY_CODE_RESPONSE_TOPIC))
			prop = MQTTProperties_getProperty(&message->properties, MQTTPROPERTY_CODE_RESPONSE_TOPIC);

		assert("Topic should be response",
		strncmp(test_request_response_globals.response_topic, prop->value.data.data, prop->value.data.len) == 0,
			"topic was %.4s\n", prop->value.data.data);

		if (MQTTProperties_hasProperty(&message->properties, MQTTPROPERTY_CODE_CORRELATION_DATA))
			prop = MQTTProperties_getProperty(&message->properties, MQTTPROPERTY_CODE_CORRELATION_DATA);

		assert("Correlation data should be",
		strncmp(test_request_response_globals.correlation_id, prop->value.data.data, prop->value.data.len) == 0,
			"Correlation data was %.4s\n", prop->value.data.data);

	}
	else if (test_request_response_globals.messages_arrived == 2)
	{
		assert("Topic should be response",
				strcmp(test_request_response_globals.response_topic, topicName) == 0,
				"topic was %s\n", topicName);
	}

	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&message);
	return 1;
}


int test_request_response(struct Options options)
{
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTProperties connect_props = MQTTProperties_initializer;
	MQTTProperties subs_props = MQTTProperties_initializer;
	MQTTProperty property;
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	MQTTResponse response = MQTTResponse_initializer;
	MQTTClient_deliveryToken dt;
	int rc = 0;
	int count = 0;
	char* test_topic = "test_request_response";
	const int msg_count = 1;
	int subsids = 1;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;

	fprintf(xml, "<testcase classname=\"test_request_response\" name=\"request/response\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test 7 - request response");

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "request_response",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
	{
		MQTTClient_destroy(&c);
		goto exit;
	}

	rc = MQTTClient_setCallbacks(c, NULL, NULL, test_request_response_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = options.MQTTVersion;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	MyLog(LOGA_DEBUG, "Connecting");
	response = MQTTClient_connect5(c, &opts, &connect_props, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	if (response.properties)
	{
		if (MQTTProperties_hasProperty(response.properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIERS_AVAILABLE))
			subsids = MQTTProperties_getNumericValue(response.properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIERS_AVAILABLE);

		MyLog(LOGA_INFO, "Connack properties:");
		logProperties(response.properties);
		MQTTResponse_free(response);
	}

	response = MQTTClient_subscribe5(c, test_request_response_globals.response_topic, 2, NULL, NULL);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);

	response = MQTTClient_subscribe5(c, test_request_response_globals.request_topic, 2, NULL, NULL);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);

	messages_arrived = 0;
	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.retained = 0;
	pubmsg.qos = 2;

	property.identifier = MQTTPROPERTY_CODE_RESPONSE_TOPIC;
	property.value.data.data = test_request_response_globals.response_topic;
	property.value.data.len = (int)strlen(property.value.data.data);
	MQTTProperties_add(&pubmsg.properties, &property);

	property.identifier = MQTTPROPERTY_CODE_CORRELATION_DATA;
	property.value.data.data = test_request_response_globals.correlation_id;
	property.value.data.len = (int)strlen(property.value.data.data);
	MQTTProperties_add(&pubmsg.properties, &property);

	response = MQTTClient_publishMessage5(c, test_request_response_globals.request_topic, &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d", response.reasonCode);

	/* should get the request */
	while (test_request_response_globals.messages_arrived < 1 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("1 message should have arrived", test_request_response_globals.messages_arrived == 1, "was %d",
			test_request_response_globals.messages_arrived);

	MQTTProperties_free(&pubmsg.properties);
	property.identifier = MQTTPROPERTY_CODE_CORRELATION_DATA;
	property.value.data.data = "request no 1";
	property.value.data.len = (int)strlen(property.value.data.data);
	MQTTProperties_add(&pubmsg.properties, &property);

	response = MQTTClient_publishMessage5(c, test_request_response_globals.response_topic, &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d", response.reasonCode);

	/* should get the response */
	while (test_request_response_globals.messages_arrived < 1 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("1 message should have arrived", test_request_response_globals.messages_arrived == 1, "was %d",
			test_request_response_globals.messages_arrived);

	rc = MQTTClient_disconnect5(c, 1000, MQTTREASONCODE_SUCCESS, NULL);

	MQTTProperties_free(&pubmsg.properties);
	MQTTProperties_free(&subs_props);
	MQTTProperties_free(&connect_props);
	MQTTClient_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST7: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}


struct
{
	char* topic;
	int messages_arrived;
} test_subscribe_options_globals =
{
	"my response topic",
	0,
};


int test_subscribe_options_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	int subsidcount = 0, i = 0, subsid = -1;

	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	if (message->struct_version == 1)
	{
		const int props_count = 0;

		MyLog(LOGA_INFO, "Message properties:");
		logProperties(&message->properties);
	}
	test_subscribe_options_globals.messages_arrived++;

	if (test_subscribe_options_globals.messages_arrived == 1)
	{
		subsidcount = MQTTProperties_propertyCount(&message->properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER);
		assert("Subsidcount is i", subsidcount == 1, "subsidcount is not correct %d\n", subsidcount);

		subsid = MQTTProperties_getNumericValueAt(&message->properties, MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER, 0);
		assert("Subsid is 2", subsid == 2, "subsid is not correct %d\n", subsid);
	}

	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&message);
	return 1;
}


int test_subscribe_options(struct Options options)
{
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTProperties connect_props = MQTTProperties_initializer;
	MQTTProperties subs_props = MQTTProperties_initializer;
	MQTTProperty property;
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	MQTTResponse response = MQTTResponse_initializer;
	MQTTClient_deliveryToken dt;
	int rc = 0;
	int count = 0;
	const int msg_count = 1;
	MQTTSubscribe_options subopts = MQTTSubscribe_options_initializer;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;

	fprintf(xml, "<testcase classname=\"test_subscribe_options\" name=\"subscribe options\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test 8 - subscribe options");

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "subscribe_options",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
	{
		MQTTClient_destroy(&c);
		goto exit;
	}

	rc = MQTTClient_setCallbacks(c, NULL, NULL, test_subscribe_options_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = options.MQTTVersion;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	MyLog(LOGA_DEBUG, "Connecting");
	response = MQTTClient_connect5(c, &opts, &connect_props, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	if (response.properties)
	{
		MyLog(LOGA_INFO, "Connack properties:");
		logProperties(response.properties);
		MQTTResponse_free(response);
	}

	property.identifier = MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER;
	property.value.integer4 = 1;
	MQTTProperties_add(&subs_props, &property);
	subopts.noLocal = 1;
	response = MQTTClient_subscribe5(c, test_subscribe_options_globals.topic, 2, &subopts, &subs_props);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);

	subs_props.array[0].value.integer4 = 2;
	subopts.noLocal = 0;
	subopts.retainHandling = 1;
	response = MQTTClient_subscribe5(c, "#", 2, &subopts, &subs_props);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);

	messages_arrived = 0;
	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.retained = 0;
	pubmsg.qos = 2;

	response = MQTTClient_publishMessage5(c, test_subscribe_options_globals.topic, &pubmsg, &dt);
	assert("Good rc from publish", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d", response.reasonCode);

	/* should get the request */
	while (test_subscribe_options_globals.messages_arrived < 1 && ++count < 10)
	{
#if defined(_WIN32)
		Sleep(1000);
#else
		usleep(1000000L);
#endif
	}
	assert("1 message should have arrived", test_subscribe_options_globals.messages_arrived == 1, "was %d",
			test_subscribe_options_globals.messages_arrived);

	rc = MQTTClient_disconnect5(c, 1000, MQTTREASONCODE_SUCCESS, NULL);

	MQTTProperties_free(&pubmsg.properties);
	MQTTProperties_free(&subs_props);
	MQTTProperties_free(&connect_props);
	MQTTClient_destroy(&c);

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
} test_shared_subscriptions_globals =
{
	"$share/share_test/#",
	"a",
	0,
};

MQTTClient c, d;

int test_shared_subscriptions_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	int subsidcount = 0, i = 0, subsid = -1;

	MyLog(LOGA_DEBUG, "Callback: message received on topic %s is %.*s.",
				 topicName, message->payloadlen, (char*)(message->payload));

	assert("Message structure version should be 1", message->struct_version == 1,
				"message->struct_version was %d", message->struct_version);
	if (message->struct_version == 1)
	{
		const int props_count = 0;

		if (message->properties.count > 0)
		{
			MyLog(LOGA_INFO, "Message properties:");
			logProperties(&message->properties);
		}
	}
	test_shared_subscriptions_globals.messages_arrived++;

	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&message);
	return 1;
}


int test_shared_subscriptions(struct Options options)
{
	int subsqos = 2;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTProperties connect_props = MQTTProperties_initializer;
	MQTTProperties subs_props = MQTTProperties_initializer;
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	MQTTResponse response = MQTTResponse_initializer;
	MQTTClient_deliveryToken dt;
	int rc = 0;
	int count = 0;
	const int msg_count = 1;
	MQTTSubscribe_options subopts = MQTTSubscribe_options_initializer;
	int i;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;

	fprintf(xml, "<testcase classname=\"test_shared_subscriptions\" name=\"shared subscriptions\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test 8 - shared subscriptions");

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "shared_subscriptions",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
	{
		MQTTClient_destroy(&c);
		goto exit;
	}

	rc = MQTTClient_createWithOptions(&d, options.connection, "shared_subscriptions_1",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL, &createOpts);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
	{
		MQTTClient_destroy(&d);
		goto exit;
	}

	rc = MQTTClient_setCallbacks(c, c, NULL, test_shared_subscriptions_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_setCallbacks(d, d, NULL, test_shared_subscriptions_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = options.MQTTVersion;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	MyLog(LOGA_DEBUG, "Connecting");
	response = MQTTClient_connect5(c, &opts, &connect_props, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	if (response.properties)
	{
		MyLog(LOGA_INFO, "Connack properties:");
		logProperties(response.properties);
		MQTTResponse_free(response);
	}

	MyLog(LOGA_DEBUG, "Connecting");
	response = MQTTClient_connect5(d, &opts, &connect_props, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTCLIENT_SUCCESS, "rc was %d", response.reasonCode);
	if (response.reasonCode != MQTTCLIENT_SUCCESS)
		goto exit;

	if (response.properties)
	{
		MyLog(LOGA_INFO, "Connack properties:");
		logProperties(response.properties);
		MQTTResponse_free(response);
	}

	response = MQTTClient_subscribe5(c, test_shared_subscriptions_globals.shared_topic, 2, &subopts, &subs_props);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);

	response = MQTTClient_subscribe5(d, test_shared_subscriptions_globals.shared_topic, 2, &subopts, &subs_props);
	assert("Good rc from subscribe", response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2, "rc was %d", response.reasonCode);

	messages_arrived = 0;
	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.retained = 0;
	pubmsg.qos = 2;

	test_shared_subscriptions_globals.messages_arrived = 0;
	for (i = 0; i < 10; ++i)
	{
		response = MQTTClient_publishMessage5(c, test_shared_subscriptions_globals.topic, &pubmsg, &dt);
		assert("Good rc from publish", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d", response.reasonCode);

		/* should get the request */
		while (test_shared_subscriptions_globals.messages_arrived < i+1 && ++count < 100)
		{
#if defined(_WIN32)
			Sleep(100);
#else
			usleep(100000L);
#endif
		}
		assert("1 message should have arrived", test_shared_subscriptions_globals.messages_arrived == i+1, "was %d",
			test_shared_subscriptions_globals.messages_arrived);
	}

	rc = MQTTClient_disconnect5(c, 1000, MQTTREASONCODE_SUCCESS, NULL);

	MQTTProperties_free(&pubmsg.properties);
	MQTTProperties_free(&subs_props);
	MQTTProperties_free(&connect_props);
	MQTTClient_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST9: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}


int main(int argc, char** argv)
{
	int rc = 0,
		i;
 	int (*tests[])() = {NULL,
 		test_client_topic_aliases,
		test_server_topic_aliases,
 		test_subscription_ids,
 		test_flow_control,
		test_error_reporting,
		test_qos_1_2_errors,
		test_request_response,
		test_subscribe_options,
		test_shared_subscriptions
 	};

	xml = fopen("TEST-test1.xml", "w");
	fprintf(xml, "<testsuite name=\"test1\" tests=\"%d\">\n", (int)(ARRAY_SIZE(tests) - 1));

	MQTTClient_setTraceCallback(test_flow_control_trace_callback);

	getopts(argc, argv);

	for (i = 0; i < options.iterations; ++i)
	{
	 	if (options.test_no == 0)
		{ /* run all the tests */
 		   	for (options.test_no = 1; options.test_no < ARRAY_SIZE(tests); ++options.test_no)
				rc += tests[options.test_no](options); /* return number of failures.  0 = test succeeded */
		}
		else
 		   	rc = tests[options.test_no](options); /* run just the selected test */
	}

 	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	fprintf(xml, "</testsuite>\n");
	fclose(xml);
	return rc;
}
