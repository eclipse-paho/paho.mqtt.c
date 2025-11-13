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
 *******************************************************************************/


/**
 * @file
 * MQTT 3.1.1 Tests for the synchronous Paho MQTT C client
 */


/*
#if !defined(_RTSHEADER)
	#include <rts.h>
#endif
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
#include <winsock2.h>
#include <ws2tcpip.h>
#define MAXHOSTNAMELEN 256
#define EAGAIN WSAEWOULDBLOCK
#define EINTR WSAEINTR
#define EINPROGRESS WSAEINPROGRESS
#define EWOULDBLOCK WSAEWOULDBLOCK
#define ENOTCONN WSAENOTCONN
#define ECONNRESET WSAECONNRESET
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
	int iterations;
} options =
{
	"tcp://localhost:1883",
	NULL,
	0,
	0,
	0,
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

Test1: sessionPresent

*********************************************************************/
int test1(struct Options options)
{
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_willOptions wopts = MQTTClient_willOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test1";

	fprintf(xml, "<testcase classname=\"test1\" name=\"sessionPresent\"");
	global_start_time = start_clock();
	failures = 0;
	MyLog(LOGA_INFO, "Starting test 1 - sessionPresent");
	
	rc = MQTTClient_create(&c, options.connection, "sesssionPresent",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);		
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
	{
		MQTTClient_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.username = "testuser";
	opts.password = "testpassword";
	opts.MQTTVersion = 4;
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

	/* Connect cleansession */
	opts.cleansession = 1;
	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTClient_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	assert("Correct serverURI returned", strcmp(opts.returned.serverURI, options.connection) == 0, "serverURI was %s",
		opts.returned.serverURI);
	assert("Correct MQTTVersion returned", opts.returned.MQTTVersion == 4, "MQTTVersion was %d",
		opts.returned.MQTTVersion);
	assert("Correct sessionPresent returned", opts.returned.sessionPresent == 0, "sessionPresent was %d",
		opts.returned.sessionPresent);

	rc = MQTTClient_disconnect(c, 0);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	/* Connect again, non-cleansession */
	opts.cleansession = 0;
	rc = MQTTClient_connect(c, &opts);
	assert("Connect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	assert("Correct serverURI returned", strcmp(opts.returned.serverURI, options.connection) == 0, "serverURI was %s",
		opts.returned.serverURI);
	assert("Correct MQTTVersion returned", opts.returned.MQTTVersion == 4, "MQTTVersion was %d",
		opts.returned.MQTTVersion);
	assert("Correct sessionPresent returned", opts.returned.sessionPresent == 0, "sessionPresent was %d",
		opts.returned.sessionPresent);

	rc = MQTTClient_disconnect(c, 0);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	/* Connect again, non-cleansession */
	opts.cleansession = 0;
	rc = MQTTClient_connect(c, &opts);
	assert("Connect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	assert("Correct serverURI returned", strcmp(opts.returned.serverURI, options.connection) == 0, "serverURI was %s",
		opts.returned.serverURI);
	assert("Correct MQTTVersion returned", opts.returned.MQTTVersion == 4, "MQTTVersion was %d",
		opts.returned.MQTTVersion);
	assert("Correct sessionPresent returned", opts.returned.sessionPresent == 1, "sessionPresent was %d",
		opts.returned.sessionPresent);
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

Test2: 0x80 return code from subscribe

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
	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&m);
	return 1;
}

int test2(struct Options options)
{
	char* testname = "test2";
	int subsqos = 2;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test2";
	char* topics[2] = {"test_topic", "nosubscribe"};
	int qoss[2] = {2, 2};

	fprintf(xml, "<testcase classname=\"test1\" name=\"bad return code from subscribe\"");
	MyLog(LOGA_INFO, "Starting test 2 - bad return code from subscribe");
	global_start_time = start_clock();
	failures = 0;

	MQTTClient_create(&c, options.connection, "multi_threaded_sample", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.MQTTVersion = 4;
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

	assert("Correct serverURI returned", strcmp(opts.returned.serverURI, options.connection) == 0, "serverURI was %s",
		opts.returned.serverURI);
	assert("Correct MQTTVersion returned", opts.returned.MQTTVersion == 4, "MQTTVersion was %d",
		opts.returned.MQTTVersion);
	assert("Correct sessionPresent returned", opts.returned.sessionPresent == 0, "sessionPresent was %d",
		opts.returned.sessionPresent);

	rc = MQTTClient_subscribe(c, test_topic, subsqos);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_subscribe(c, "nosubscribe", 2);
	assert("0x80 from subscribe", rc == 0x80, "rc was %d", rc);

	rc = MQTTClient_subscribeMany(c, 2, topics, qoss);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	assert("Correct returned qos from subscribe", qoss[0] == 2, "qos 0 was %d", qoss[0]);
	assert("Correct returned qos from subscribe", qoss[1] == 0x80, "qos 0 was %d", qoss[0]);

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
 	int (*tests[])(struct Options) = {NULL, test1, test2};
	int i;
	
	xml = fopen("TEST-MQTT4sync.xml", "w");
	fprintf(xml, "<testsuite name=\"test-mqtt4sync\" tests=\"%d\">\n", (int)(ARRAY_SIZE(tests) - 1));

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
