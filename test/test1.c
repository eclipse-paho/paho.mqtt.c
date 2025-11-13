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
 *    Ian Craggs - change will message test back to using proxy
 *******************************************************************************/


/**
 * @file
 * Tests for the MQ Telemetry MQTT C client
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


/*********************************************************************

Test1: single-threaded client

*********************************************************************/
void test1_sendAndReceive(MQTTClient* c, int qos, char* test_topic)
{
	MQTTClient_deliveryToken dt;
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	MQTTClient_message* m = NULL;
	char* topicName = NULL;
	int topicLen;
	int i = 0;
	int iterations = 50;
	int rc;

	MyLog(LOGA_DEBUG, "%d messages at QoS %d", iterations, qos);
	pubmsg.payload = "a much longer message that we can shorten to the extent that we need to payload up to 11";
	pubmsg.payloadlen = 11;
	pubmsg.qos = qos;
	pubmsg.retained = 0;

	for (i = 0; i< iterations; ++i)
	{
		if (i % 10 == 0)
			rc = MQTTClient_publish(c, test_topic, pubmsg.payloadlen, pubmsg.payload, pubmsg.qos, pubmsg.retained, &dt);
		else
			rc = MQTTClient_publishMessage(c, test_topic, &pubmsg, &dt);
		assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

		if (qos > 0)
		{
			rc = MQTTClient_waitForCompletion(c, dt, 5000L);
			assert("Good rc from waitforCompletion", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
		}

		rc = MQTTClient_receive(c, &topicName, &topicLen, &m, 5000);
		assert("Good rc from receive", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
		if (topicName)
		{
			MyLog(LOGA_DEBUG, "Message received on topic %s is %.*s", topicName, m->payloadlen, (char*)(m->payload));
			if (pubmsg.payloadlen != m->payloadlen ||
					memcmp(m->payload, pubmsg.payload, m->payloadlen) != 0)
			{
				failures++;
				MyLog(LOGA_INFO, "Error: wrong data - received lengths %d %d", pubmsg.payloadlen, m->payloadlen);
				break;
			}
			MQTTClient_free(topicName);
			MQTTClient_freeMessage(&m);
		}
		else
			printf("No message received within timeout period\n");
	}

	/* receive any outstanding messages */
	MQTTClient_receive(c, &topicName, &topicLen, &m, 2000);
	while (topicName)
	{
		printf("Message received on topic %s is %.*s.\n", topicName, m->payloadlen, (char*)(m->payload));
		MQTTClient_free(topicName);
		MQTTClient_freeMessage(&m);
		MQTTClient_receive(c, &topicName, &topicLen, &m, 2000);
	}
}


int test1(struct Options options)
{
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_willOptions wopts = MQTTClient_willOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test1";

	fprintf(xml, "<testcase classname=\"test1\" name=\"single threaded client using receive\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test 1 - single threaded client using receive");

	rc = MQTTClient_create(&c, options.connection, "single_threaded_test",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
	{
		MQTTClient_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";
	opts.MQTTVersion = options.MQTTVersion;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTClient_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	rc = MQTTClient_subscribe(c, test_topic, subsqos);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	test1_sendAndReceive(c, 0, test_topic);
	test1_sendAndReceive(c, 1, test_topic);
	test1_sendAndReceive(c, 2, test_topic);

	MyLog(LOGA_DEBUG, "Stopping\n");

	rc = MQTTClient_unsubscribe(c, test_topic);
	assert("Unsubscribe successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	rc = MQTTClient_disconnect(c, 0);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	/* Just to make sure we can connect again */
	rc = MQTTClient_connect(c, &opts);
	assert("Connect successful",  rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	rc = MQTTClient_disconnect(c, 0);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MQTTClient_destroy(&c);

exit:
	MyLog(LOGA_INFO, "TEST1: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
	write_test_result();
	return failures;
}


/*********************************************************************

Test2: multi-threaded client using callbacks

*********************************************************************/
volatile int test2_arrivedcount = 0;
int test2_deliveryCompleted = 0;
MQTTClient_message test2_pubmsg = MQTTClient_message_initializer;

void test2_deliveryComplete(void* context, MQTTClient_deliveryToken dt)
{
	++test2_deliveryCompleted;
}

int test2_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* m)
{
	++test2_arrivedcount;
	MyLog(LOGA_DEBUG, "Callback: %d message received on topic %s is %.*s.",
					test2_arrivedcount, topicName, m->payloadlen, (char*)(m->payload));
	if (test2_pubmsg.payloadlen != m->payloadlen ||
					memcmp(m->payload, test2_pubmsg.payload, m->payloadlen) != 0)
	{
		failures++;
		MyLog(LOGA_INFO, "Error: wrong data received lengths %d %d\n", test2_pubmsg.payloadlen, m->payloadlen);
	}
	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&m);
	return 1;
}


void test2_sendAndReceive(MQTTClient* c, int qos, char* test_topic)
{
	MQTTClient_deliveryToken dt;
	int i = 0;
	int iterations = 50;
	int rc = 0;
	int wait_seconds = 0;

	test2_deliveryCompleted = 0;

	MyLog(LOGA_INFO, "%d messages at QoS %d", iterations, qos);
	test2_pubmsg.payload = "a much longer message that we can shorten to the extent that we need to";
	test2_pubmsg.payloadlen = 27;
	test2_pubmsg.qos = qos;
	test2_pubmsg.retained = 0;

	for (i = 1; i <= iterations; ++i)
	{
		if (i % 10 == 0)
			rc = MQTTClient_publish(c, test_topic, test2_pubmsg.payloadlen, test2_pubmsg.payload,
                   test2_pubmsg.qos, test2_pubmsg.retained, NULL);
		else
			rc = MQTTClient_publishMessage(c, test_topic, &test2_pubmsg, &dt);
		assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(100000L);
		#endif

		wait_seconds = 10;
		while ((test2_arrivedcount < i) && (wait_seconds-- > 0))
		{
			MyLog(LOGA_DEBUG, "Arrived %d count %d", test2_arrivedcount, i);
			#if defined(_WIN32)
				Sleep(1000);
			#else
				usleep(1000000L);
			#endif
		}
		assert("Message Arrived", wait_seconds > 0,
				"Time out waiting for message %d\n", i );
	}
	if (qos > 0)
	{
		/* MQ Telemetry can send a message to a subscriber before the server has
		   completed the QoS 2 handshake with the publisher. For QoS 1 and 2,
		   allow time for the final delivery complete callback before checking
		   that all expected callbacks have been made */
		wait_seconds = 10;
		while ((test2_deliveryCompleted < iterations) && (wait_seconds-- > 0))
		{
			MyLog(LOGA_DEBUG, "Delivery Completed %d count %d", test2_deliveryCompleted, i);
			#if defined(_WIN32)
				Sleep(1000);
			#else
				usleep(1000000L);
			#endif
		}
		assert("All Deliveries Complete", wait_seconds > 0,
			   "Number of deliveryCompleted callbacks was %d\n",
			   test2_deliveryCompleted);
	}
}


int test2(struct Options options)
{
	char* testname = "test2";
	int subsqos = 2;
	/* TODO - usused - remove ? MQTTClient_deliveryToken* dt = NULL; */
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test2";

	fprintf(xml, "<testcase classname=\"test1\" name=\"multi-threaded client using callbacks\"");
	MyLog(LOGA_INFO, "Starting test 2 - multi-threaded client using callbacks");
	global_start_time = start_clock();
	failures = 0;

	MQTTClient_create(&c, options.connection, "multi_threaded_sample", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.MQTTVersion = options.MQTTVersion;
	opts.username = "testuser";
	opts.binarypwd.data = "testpassword";
	opts.binarypwd.len = (int)strlen(opts.binarypwd.data);
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	rc = MQTTClient_setCallbacks(c, NULL, NULL, test2_messageArrived, test2_deliveryComplete);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTClient_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	rc = MQTTClient_subscribe(c, test_topic, subsqos);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	test2_sendAndReceive(c, 0, test_topic);
	test2_sendAndReceive(c, 1, test_topic);
	test2_sendAndReceive(c, 2, test_topic);

	MyLog(LOGA_DEBUG, "Stopping");

	rc = MQTTClient_unsubscribe(c, test_topic);
	assert("Unsubscribe successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	rc = MQTTClient_disconnect(c, 0);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MQTTClient_destroy(&c);

exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}


/*********************************************************************

Test 3: connack return codes

for AMQTDD, needs an amqtdd.cfg of:

	allow_anonymous false
  password_file passwords

and a passwords file of:

	Admin:Admin

*********************************************************************/
int test3(struct Options options)
{
	char* testname = "test3";
	int rc;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_willOptions wopts = MQTTClient_willOptions_initializer;

	fprintf(xml, "<testcase classname=\"test1\" name=\"connack return codes\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test 3 - connack return codes");

#if 0
	/* clientid too long (RC = 2) */
	rc = MQTTClient_create(&c, options.connection, "client_ID_too_long_for_MQTT_protocol_version_3",
		MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	rc = MQTTClient_connect(c, &opts);
	assert("identifier rejected", rc == 2, "rc was %d\n", rc);
	MQTTClient_destroy(&c);
#endif
	/* broker unavailable (RC = 3)  - TDD when allow_anonymous not set*/
	rc = MQTTClient_create(&c, options.connection, "The C Client", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
#if 0
	rc = MQTTClient_connect(c, &opts);
	assert("broker unavailable", rc == 3, "rc was %d\n", rc);

	/* authentication failure (RC = 4) */
	opts.username = "Admin";
	opts.password = "fred";
	rc = MQTTClient_connect(c, &opts);
	assert("Bad user name or password", rc == 4, "rc was %d\n", rc);
#endif

	/* authorization failure (RC = 5) */
	opts.username = "Admin";
	opts.password = "Admin";
	/*opts.will = &wopts;    "Admin" not authorized to publish to Will topic by default
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";*/
	rc = MQTTClient_connect(c, &opts);
	//assert("Not authorized", rc == 5, "rc was %d\n", rc);

#if 0
	/* successful connection (RC = 0) */
	opts.username = "Admin";
	opts.password = "Admin";
  opts.will = NULL;
	rc = MQTTClient_connect(c, &opts);
	assert("successful connection", rc == MQTTCLIENT_SUCCESS,  "rc was %d\n", rc);
	MQTTClient_disconnect(c, 0);
	MQTTClient_destroy(&c);
#endif

/* TODO - unused - remove ? exit: */
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}


/*********************************************************************

Test 4: client persistence 1


*********************************************************************/
int test4_run(int qos)
{
	char* testname = "test 4";
	char* topic = "Persistence test 1";
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_message* m = NULL;
	char* topicName = NULL;
	int topicLen;
	MQTTClient_deliveryToken* tokens = NULL;
	int mytoken = -99;
	char buffer[100];
	int count = 3;
	int i, rc;

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 4 - persistence, qos %d", qos);

	MQTTClient_create(&c, options.connection, "xrctest1_test_4", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);

	opts.keepAliveInterval = 20;
	opts.reliable = 0;
	opts.MQTTVersion = options.MQTTVersion;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	MyLog(LOGA_DEBUG, "Cleanup by connecting clean session\n");
	opts.cleansession = 1;
	if ((rc = MQTTClient_connect(c, &opts)) != 0)
	{
  		assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
		return -1;
	}
	opts.cleansession = 0;
	MQTTClient_disconnect(c, 0);

	MyLog(LOGA_DEBUG, "Connecting\n");
	if ((rc = MQTTClient_connect(c, &opts)) != 0)
	{
  		assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
		return -1;
	}

	/* subscribe so we can get messages back */
	rc = MQTTClient_subscribe(c, topic, subsqos);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	/* send messages so that we can receive the same ones */
	for (i = 0; i < count; ++i)
	{
		sprintf(buffer, "Message sequence no %d", i);
		rc = MQTTClient_publish(c, topic, 10, buffer, qos, 0, NULL);
		assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	}

	/* disconnect immediately without receiving the incoming messages */
	MQTTClient_disconnect(c, 0); /* now there should be "orphaned" publications */

	rc = MQTTClient_getPendingDeliveryTokens(c, &tokens);
 	assert("getPendingDeliveryTokens rc == 0", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	assert("should get some tokens back", tokens != NULL, "tokens was %p", tokens);
	if (tokens)
	{
		int i = 0;

		while (tokens[i] != -1)
			MyLog(LOGA_DEBUG, "Pending delivery token %d", tokens[i++]);
		assert1("no of tokens should be count", i == count, "no of tokens %d count %d", i, count);
		mytoken = tokens[0];
		MQTTClient_free(tokens);
	}

	MQTTClient_destroy(&c); /* force re-reading persistence on create */

	MQTTClient_create(&c, options.connection, "xrctest1_test_4", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);

	rc = MQTTClient_getPendingDeliveryTokens(c, &tokens);
	assert("getPendingDeliveryTokens rc == 0", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	assert("should get some tokens back", tokens != NULL, "tokens was %p", tokens);
	if (tokens)
	{
		int i = 0;
		while (tokens[i] != -1)
			MyLog(LOGA_DEBUG, "Pending delivery token %d", tokens[i++]);
		MQTTClient_free(tokens);
		assert1("no of tokens should be count", i == count, "no of tokens %d count %d", i, count);
	}

	MyLog(LOGA_DEBUG, "Reconnecting");
	if (MQTTClient_connect(c, &opts) != 0)
	{
		assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
		return -1;
	}

	for (i = 0; i < count; ++i)
	{
  		int dup = 0;
		do
		{
    		dup = 0;
			MQTTClient_receive(c, &topicName, &topicLen, &m, 5000);
			if (m && m->dup)
			{
				assert("No duplicates should be received for qos 2", qos == 1, "qos is %d", qos);
				MyLog(LOGA_DEBUG, "Duplicate message id %d", m->msgid);
				MQTTClient_freeMessage(&m);
				MQTTClient_free(topicName);
  	    		dup = 1;
			}
		} while (dup == 1);
		assert("should get a message", m != NULL, "m was %p", m);
		if (m)
		{
			MyLog(LOGA_DEBUG, "Received message id %d", m->msgid);
			assert("topicName is correct", strcmp(topicName, topic) == 0, "topicName is %s", topicName);
			MQTTClient_freeMessage(&m);
			MQTTClient_free(topicName);
		}
	}

	/* call yield a few times until unfinished protocol exchanges are finished */
	count = 0;
	do
	{
		MQTTClient_yield();
		rc = MQTTClient_getPendingDeliveryTokens(c, &tokens);
		assert("getPendingDeliveryTokens rc == 0", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	} while (tokens != NULL && ++count < 10);

	rc = MQTTClient_getPendingDeliveryTokens(c, &tokens);
	assert("getPendingDeliveryTokens rc == 0", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	assert("should get no tokens back", tokens == NULL, "tokens was %p", tokens);

	MQTTClient_disconnect(c, 0);

	MQTTClient_destroy(&c);

/* TODO - unused -remove? exit: */
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);

	return failures;
}


int test4(struct Options options)
{
	int rc = 0;
	fprintf(xml, "<testcase classname=\"test1\" name=\"persistence\"");
	global_start_time = start_clock();
	rc = test4_run(1) + test4_run(2);
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

Test 5: disconnect with quiesce timeout should allow exchanges to complete

*********************************************************************/
int test5(struct Options options)
{
	char* testname = "test 5";
	char* topic = "Persistence test 2";
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_deliveryToken* tokens = NULL;
	char buffer[100];
	int count = 5;
	int i, rc;

	fprintf(xml, "<testcase classname=\"test1\" name=\"disconnect with quiesce timeout should allow exchanges to complete\"");
	global_start_time = start_clock();
	failures = 0;
 	MyLog(LOGA_INFO, "Starting test 5 - disconnect with quiesce timeout should allow exchanges to complete");

	MQTTClient_create(&c, options.connection, "xrctest1_test_5", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);

	opts.keepAliveInterval = 20;
	opts.cleansession = 0;
	opts.reliable = 0;
	opts.MQTTVersion = options.MQTTVersion;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	MyLog(LOGA_DEBUG, "Connecting");
	if ((rc = MQTTClient_connect(c, &opts)) != 0)
	{
		assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
		MQTTClient_destroy(&c);
		goto exit;
	}

	rc = MQTTClient_subscribe(c, topic, subsqos);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	for (i = 0; i < count; ++i)
	{
		sprintf(buffer, "Message sequence no %d", i);
		rc = MQTTClient_publish(c, topic, 10, buffer, 1, 0, NULL);
		assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	}

	MQTTClient_disconnect(c, 1000); /* now there should be no "orphaned" publications */
	MyLog(LOGA_DEBUG, "Disconnected");

	rc = MQTTClient_getPendingDeliveryTokens(c, &tokens);
 	assert("getPendingDeliveryTokens rc == 0", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	assert("should get no tokens back", tokens == NULL, "tokens was %p", tokens);

	MQTTClient_destroy(&c);

exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}


/*********************************************************************

Test 6: connectionLost and will message

*********************************************************************/
MQTTClient test6_c1, test6_c2;
volatile int test6_will_message_arrived = 0;
volatile int test6_connection_lost_called = 0;

void test6_connectionLost(void* context, char* cause)
{
	MQTTClient c = (MQTTClient)context;
	printf("%s -> Callback: connection lost\n", (c == test6_c1) ? "Client-1" : "Client-2");
	test6_connection_lost_called = 1;
}

void test6_deliveryComplete(void* context, MQTTClient_deliveryToken token)
{
	printf("Client-2 -> Callback: publish complete for token %d\n", token);
}

char* test6_will_topic = "C Test 2: will topic";
char* test6_will_message = "will message from Client-1";

int test6_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* m)
{
	MQTTClient c = (MQTTClient)context;
	printf("%s -> Callback: message received on topic '%s' is '%.*s'.\n",
			 (c == test6_c1) ? "Client-1" : "Client-2", topicName, m->payloadlen, (char*)(m->payload));
	if (c == test6_c2 && strcmp(topicName, test6_will_topic) == 0 && memcmp(m->payload, test6_will_message, m->payloadlen) == 0)
		test6_will_message_arrived = 1;
	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&m);
	return 1;
}


int test6(struct Options options)
{
	char* testname = "test6";
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_willOptions wopts =  MQTTClient_willOptions_initializer;
	MQTTClient_connectOptions opts2 = MQTTClient_connectOptions_initializer;
	int rc, count;
	char* mqttsas_topic = "MQTTSAS topic";

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 6 - connectionLost and will messages");
	fprintf(xml, "<testcase classname=\"test1\" name=\"connectionLost and will messages\"");
	global_start_time = start_clock();

	opts.keepAliveInterval = 2;
	opts.cleansession = 1;
	opts.MQTTVersion = MQTTVERSION_3_1_1;
	opts.will = &wopts;
	opts.will->message = test6_will_message;
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = test6_will_topic;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	/* Client-1 with Will options */
	rc = MQTTClient_create(&test6_c1, options.proxy_connection, "Client_1", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	rc = MQTTClient_setCallbacks(test6_c1, (void*)test6_c1, test6_connectionLost, test6_messageArrived, test6_deliveryComplete);
	assert("good rc from setCallbacks",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	/* Connect to the broker */
	rc = MQTTClient_connect(test6_c1, &opts);
	assert("good rc from connect",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	/* Client - 2 (multi-threaded) */
	rc = MQTTClient_create(&test6_c2, options.connection, "Client_2", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	/* Set the callback functions for the client */
	rc = MQTTClient_setCallbacks(test6_c2, (void*)test6_c2, test6_connectionLost, test6_messageArrived, test6_deliveryComplete);
	assert("good rc from setCallbacks",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	/* Connect to the broker */
	opts2.keepAliveInterval = 20;
	opts2.cleansession = 1;
	MyLog(LOGA_INFO, "Connecting Client_2 ...");
	rc = MQTTClient_connect(test6_c2, &opts2);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_subscribe(test6_c2, test6_will_topic, 2);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	/* now send the command which will break the connection and cause the will message to be sent */
	rc = MQTTClient_publish(test6_c1, mqttsas_topic, (int)strlen("TERMINATE"), "TERMINATE", 0, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	MyLog(LOGA_INFO, "Waiting to receive the will message");
	count = 0;
	while (++count < 40)
	{
		#if defined(_WIN32)
			Sleep(1000L);
		#else
			sleep(1);
		#endif
		if (test6_will_message_arrived == 1 && test6_connection_lost_called == 1)
			break;
	}
	assert("will message arrived", test6_will_message_arrived == 1,
							"will_message_arrived was %d\n", test6_will_message_arrived);
	assert("connection lost called", test6_connection_lost_called == 1,
			         "connection_lost_called %d\n", test6_connection_lost_called);

	rc = MQTTClient_unsubscribe(test6_c2, test6_will_topic);
	assert("Good rc from unsubscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_isConnected(test6_c2);
	assert("Client-2 still connected", rc == 1, "isconnected is %d", rc);

	rc = MQTTClient_isConnected(test6_c1);
	assert("Client-1 not connected", rc == 0, "isconnected is %d", rc);

	rc = MQTTClient_disconnect(test6_c2, 100L);
	assert("Good rc from disconnect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MQTTClient_destroy(&test6_c1);
	MQTTClient_destroy(&test6_c2);

exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.\n",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}


int test6a(struct Options options)
{
	char* testname = "test6a";
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_willOptions wopts =  MQTTClient_willOptions_initializer;
	MQTTClient_connectOptions opts2 = MQTTClient_connectOptions_initializer;
	int rc, count;
	char* mqttsas_topic = "MQTTSAS topic";

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 6 - connectionLost and binary will messages");
	fprintf(xml, "<testcase classname=\"test1\" name=\"connectionLost and binary will messages\"");
	global_start_time = start_clock();

	opts.keepAliveInterval = 2;
	opts.cleansession = 1;
	opts.MQTTVersion = MQTTVERSION_3_1_1;
	opts.will = &wopts;
	opts.will->payload.data = test6_will_message;
	opts.will->payload.len = (int)strlen(test6_will_message) + 1;
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = test6_will_topic;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}

	/* Client-1 with Will options */
	rc = MQTTClient_create(&test6_c1, options.proxy_connection, "Client_1", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	rc = MQTTClient_setCallbacks(test6_c1, (void*)test6_c1, test6_connectionLost, test6_messageArrived, test6_deliveryComplete);
	assert("good rc from setCallbacks",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	/* Connect to the broker */
	rc = MQTTClient_connect(test6_c1, &opts);
	assert("good rc from connect",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	/* Client - 2 (multi-threaded) */
	rc = MQTTClient_create(&test6_c2, options.connection, "Client_2", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	/* Set the callback functions for the client */
	rc = MQTTClient_setCallbacks(test6_c2, (void*)test6_c2, test6_connectionLost, test6_messageArrived, test6_deliveryComplete);
	assert("good rc from setCallbacks",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	/* Connect to the broker */
	opts2.keepAliveInterval = 20;
	opts2.cleansession = 1;
	MyLog(LOGA_INFO, "Connecting Client_2 ...");
	rc = MQTTClient_connect(test6_c2, &opts2);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_subscribe(test6_c2, test6_will_topic, 2);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	/* now send the command which will break the connection and cause the will message to be sent */
	rc = MQTTClient_publish(test6_c1, mqttsas_topic, (int)strlen("TERMINATE"), "TERMINATE", 0, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	MyLog(LOGA_INFO, "Waiting to receive the will message");
	count = 0;
	while (++count < 40)
	{
		#if defined(_WIN32)
			Sleep(1000L);
		#else
			sleep(1);
		#endif
		if (test6_will_message_arrived == 1 && test6_connection_lost_called == 1)
			break;
	}
	assert("will message arrived", test6_will_message_arrived == 1,
							"will_message_arrived was %d\n", test6_will_message_arrived);
	assert("connection lost called", test6_connection_lost_called == 1,
			         "connection_lost_called %d\n", test6_connection_lost_called);

	rc = MQTTClient_unsubscribe(test6_c2, test6_will_topic);
	assert("Good rc from unsubscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_isConnected(test6_c2);
	assert("Client-2 still connected", rc == 1, "isconnected is %d", rc);

	rc = MQTTClient_isConnected(test6_c1);
	assert("Client-1 not connected", rc == 0, "isconnected is %d", rc);

	rc = MQTTClient_disconnect(test6_c2, 100L);
	assert("Good rc from disconnect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MQTTClient_destroy(&test6_c1);
	MQTTClient_destroy(&test6_c2);

exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.\n",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

int main(int argc, char** argv)
{
	int rc = 0;
 	int (*tests[])(struct Options) = {NULL, test1, test2, test3, test4, test5, test6, test6a};
	int i;

	xml = fopen("TEST-test1.xml", "w");
	fprintf(xml, "<testsuite name=\"test1\" tests=\"%d\">\n", (int)(ARRAY_SIZE(tests) - 1));

	setenv("MQTT_C_CLIENT_TRACE", "ON", 1);
	setenv("MQTT_C_CLIENT_TRACE_LEVEL", "ERROR", 1);

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
