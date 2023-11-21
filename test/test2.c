/*******************************************************************************
 * Copyright (c) 2009, 2023 IBM Corp., Ian Craggs and others
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
 *    Ian Craggs - fix thread id display
 *******************************************************************************/

/**
 * @file
 * Multi-threaded tests for the Eclipse Paho MQTT C client
 */

#include "MQTTClient.h"
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
	int hacount;
	int verbose;
	int test_no;
	int MQTTVersion;
	int iterations;
} options =
{
	"tcp://localhost:1883",
	NULL,
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


#if defined(_WIN32) || defined(_WIN64)
mutex_type deliveryCompleted_mutex = NULL;
#else
pthread_mutex_t deliveryCompleted_mutex_store = PTHREAD_MUTEX_INITIALIZER;
mutex_type deliveryCompleted_mutex = &deliveryCompleted_mutex_store;
#endif

void lock_mutex(mutex_type amutex)
{
	int rc = Paho_thread_lock_mutex(amutex);
	if (rc != 0)
		MyLog(LOGA_INFO, "Error %s locking mutex", strerror(rc));
}

void unlock_mutex(mutex_type amutex)
{
	int rc = Paho_thread_unlock_mutex(amutex);
	if (rc != 0)
		MyLog(LOGA_INFO, "Error %s unlocking mutex", strerror(rc));
}


/*********************************************************************

Test1: multiple threads to single client object

*********************************************************************/
volatile int test1_arrivedcount = 0;
volatile int test1_arrivedcount_qos[3] = {0, 0, 0};
volatile int test1_deliveryCompleted = 0;
MQTTClient_message test1_pubmsg_check = MQTTClient_message_initializer;


void test1_deliveryComplete(void* context, MQTTClient_deliveryToken dt)
{
	lock_mutex(deliveryCompleted_mutex);
	MyLog(LOGA_DEBUG, "Delivery complete for token %d", dt);
	++test1_deliveryCompleted;
	unlock_mutex(deliveryCompleted_mutex);
}

int test1_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* m)
{
	++(test1_arrivedcount_qos[m->qos]);
	++test1_arrivedcount;

	MyLog(LOGA_DEBUG, "messageArrived: %d message received on topic %s is %.*s.",
					test1_arrivedcount, topicName, m->payloadlen, (char*)(m->payload));
	if (test1_pubmsg_check.payloadlen != m->payloadlen ||
					memcmp(m->payload, test1_pubmsg_check.payload, m->payloadlen) != 0)
	{
		failures++;
		MyLog(LOGA_INFO, "Error: wrong data received lengths %d %d\n", test1_pubmsg_check.payloadlen, m->payloadlen);
	}
	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&m);
	return 1;
}


struct thread_parms
{
	MQTTClient* c;
	int qos;
	char* test_topic;
};

static int iterations = 50;

thread_return_type WINAPI test1_sendAndReceive(void* n)
{
	MQTTClient_deliveryToken dt;
	int i = 0;
	int rc = 0;
	int wait_seconds = 30;
	MQTTClient_message test1_pubmsg = MQTTClient_message_initializer;
	int subsqos = 2;

	struct thread_parms *parms = n;
	MQTTClient* c = parms->c;
	int qos = parms->qos;
	char* test_topic = parms->test_topic;

	rc = MQTTClient_subscribe(c, test_topic, subsqos);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_INFO, "Thread %u, %d messages at QoS %d", Paho_thread_getid(), iterations, qos);
	test1_pubmsg.payload = test1_pubmsg_check.payload;
	test1_pubmsg.payloadlen = test1_pubmsg_check.payloadlen;
	test1_pubmsg.retained = 0;
	test1_pubmsg.qos = qos;

	for (i = 1; i <= iterations; ++i)
	{
		if (i % 10 == 0)
			rc = MQTTClient_publish(c, test_topic, test1_pubmsg.payloadlen, test1_pubmsg.payload,
                   qos, test1_pubmsg.retained, &dt);
		else
			rc = MQTTClient_publishMessage(c, test_topic, &test1_pubmsg, &dt);
		assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(100000L);
		#endif

		wait_seconds = 30;
		while ((test1_arrivedcount_qos[qos] < i) && (wait_seconds-- > 0))
		{
			MyLog(LOGA_DEBUG, "Arrived %d count %d", test1_arrivedcount_qos[qos], i);
			#if defined(_WIN32)
				Sleep(1000);
			#else
				usleep(1000000L);
			#endif
		}
		assert("Message Arrived", wait_seconds > 0,
				"Timed out waiting for message %d\n", i);
	}

#if defined(_WINDOWS)
	return 0;
#else
	return NULL;
#endif
}


int test1(struct Options options)
{
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_willOptions wopts = MQTTClient_willOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test1";

	fprintf(xml, "<testcase classname=\"test1\" name=\"multiple threads using same client object\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test 1 - multiple threads using same client object");

	rc = MQTTClient_create(&c, options.connection, "single_object, multiple threads",
			MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
	{
		MQTTClient_destroy(&c);
		goto exit;
	}

	rc = MQTTClient_setCallbacks(c, NULL, NULL, test1_messageArrived, test1_deliveryComplete);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

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

	test1_pubmsg_check.payload = "a much longer message that we can shorten to the extent that we need to";
	test1_pubmsg_check.payloadlen = 27;
	test1_deliveryCompleted = test1_arrivedcount = 0;

	struct thread_parms parms0 = {c, 0, test_topic};
	Paho_thread_start(test1_sendAndReceive, (void*)&parms0);

	struct thread_parms parms1 = {c, 1, test_topic};
	Paho_thread_start(test1_sendAndReceive, (void*)&parms1);

	struct thread_parms parms2 = {c, 2, test_topic};
	Paho_thread_start(test1_sendAndReceive, (void*)&parms2);

	/* MQTT servers can send a message to a subscriber before the server has
	   completed the QoS 2 handshake with the publisher. For QoS 1 and 2,
	   allow time for the final delivery complete callback before checking
	   that all expected callbacks have been made */

	int wait_seconds = 90;
	while (((test1_arrivedcount < iterations*3) || (test1_deliveryCompleted < iterations*2)) && (wait_seconds-- > 0))
	{
		#if defined(_WIN32)
			Sleep(1000);
		#else
			usleep(1000000L);
		#endif
	}
	assert("Arrived count == 150", test1_arrivedcount == iterations*3, "arrivedcount was %d", test1_arrivedcount);
  assert("All Deliveries Complete", test1_deliveryCompleted == iterations*2,
			   "Number of deliveryCompleted callbacks was %d\n", test1_deliveryCompleted);

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

Test2: multiple client objects used from multiple threads

*********************************************************************/
volatile int test2_arrivedcount = 0;

volatile int test2_deliveryCompleted = 0;

MQTTClient_message test2_pubmsg = MQTTClient_message_initializer;

void test2_deliveryComplete(void* context, MQTTClient_deliveryToken dt)
{
	lock_mutex(deliveryCompleted_mutex);
	MyLog(LOGA_DEBUG, "Delivery complete for token %d", dt);
	++test2_deliveryCompleted;
	unlock_mutex(deliveryCompleted_mutex);
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
		wait_seconds = 40;
		while ((test2_deliveryCompleted < iterations) && (wait_seconds-- > 0))
		{
			MyLog(LOGA_DEBUG, "Delivery Completed %d count %d", test2_deliveryCompleted, i);
			#if defined(_WIN32)
				Sleep(1000);
			#else
				usleep(1000000L);
			#endif
		}
		assert("All Deliveries Complete", test2_deliveryCompleted == iterations,
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

	MQTTClient_create(&c, options.connection, "multi_threaded_sample", MQTTCLIENT_PERSISTENCE_NONE, NULL);

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.MQTTVersion = options.MQTTVersion;
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


int main(int argc, char** argv)
{
	int rc = 0;
 	int (*tests[])() = {NULL, test1};
	int i;

	#if defined(_WIN32) || defined(_WIN64)
	deliveryCompleted_mutex = CreateMutex(NULL, 0, NULL);
	#endif

	xml = fopen("TEST-test2.xml", "w");
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
