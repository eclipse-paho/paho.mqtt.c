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
 * MQTT 3.1.1 Tests for the asynchronous Paho MQTT C client
 */


/*
#if !defined(_RTSHEADER)
	#include <rts.h>
#endif
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
	"localhost:1883",
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
int test_finished = 0;


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


void test1_onDisconnect3(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", c);
	test_finished = 1;
}


void test1_onConnect3(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	opts.onSuccess = test1_onDisconnect3;
	opts.context = c;

	assert("Correct serverURI returned", strstr(response->alt.connect.serverURI, options.connection) != NULL,
			"serverURI was %s", response->alt.connect.serverURI);
	assert("Correct MQTTVersion returned", response->alt.connect.MQTTVersion == 4,
			"MQTTVersion was %d", response->alt.connect.MQTTVersion);
	assert("Correct sessionPresent returned", response->alt.connect.sessionPresent == 1,
			"sessionPresent was %d", response->alt.connect.sessionPresent);

	rc = MQTTAsync_disconnect(c, &opts);
	assert("Disconnect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


void test1_onDisconnect2(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", c);

	opts.MQTTVersion = 4;
	opts.cleansession = 0;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}
	opts.onSuccess = test1_onConnect3;
	opts.onFailure = NULL;
	opts.context = c;

	opts.cleansession = 0;
	rc = MQTTAsync_connect(c, &opts);
	assert("Connect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


void test1_onConnect2(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	opts.onSuccess = test1_onDisconnect2;
	opts.context = c;

	assert("Correct serverURI returned", strcmp(response->alt.connect.serverURI, options.connection) == 0,
			"serverURI was %s", response->alt.connect.serverURI);
	assert("Correct MQTTVersion returned", response->alt.connect.MQTTVersion == 4,
			"MQTTVersion was %d", response->alt.connect.MQTTVersion);
	assert("Correct sessionPresent returned", response->alt.connect.sessionPresent == 0,
			"sessionPresent was %d", response->alt.connect.sessionPresent);

	rc = MQTTAsync_disconnect(c, &opts);
	assert("Disconnect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


void test1_onDisconnect1(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", c);

	opts.MQTTVersion = 4;
	opts.cleansession = 0;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}
	opts.onSuccess = test1_onConnect2;
	opts.onFailure = NULL;
	opts.context = c;

	opts.cleansession = 0;
	rc = MQTTAsync_connect(c, &opts);
	assert("Connect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


void test1_onConnect1(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback 1, context %p", context);
	opts.onSuccess = test1_onDisconnect1;
	opts.context = c;

	assert("Correct serverURI returned", strcmp(response->alt.connect.serverURI, options.connection) == 0,
			"serverURI was %s", response->alt.connect.serverURI);
	assert("Correct MQTTVersion returned", response->alt.connect.MQTTVersion == 4,
			"MQTTVersion was %d", response->alt.connect.MQTTVersion);
	assert("Correct sessionPresent returned", response->alt.connect.sessionPresent == 0,
			"sessionPresent was %d", response->alt.connect.sessionPresent);

	rc = MQTTAsync_disconnect(c, &opts);
	assert("Disconnect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


/*********************************************************************

Test1: sessionPresent

*********************************************************************/
int test1(struct Options options)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test1";

	fprintf(xml, "<testcase classname=\"test1\" name=\"sessionPresent\"");
	global_start_time = start_clock();
	test_finished = failures = 0;
	MyLog(LOGA_INFO, "Starting test 1 - sessionPresent");
	
	rc = MQTTAsync_create(&c, options.connection, "sesssionPresent",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);		
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.MQTTVersion = 4;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}
	opts.onSuccess = test1_onConnect1;
	opts.onFailure = NULL;
	opts.context = c;

	/* Connect cleansession */
	opts.cleansession = 1;
	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
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


void test2_onDisconnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In onDisconnect callback %p", c);
	test_finished = 1;
}


void test2_onSubscribe2(void* context, MQTTAsync_failureData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In subscribe onFailure callback, context %p", context);

	assert("Correct subscribe return code", response->code == MQTT_BAD_SUBSCRIBE,
			"qos was %d", response->code);

	opts.onSuccess = test2_onDisconnect;
	rc = MQTTAsync_disconnect(c, &opts);
	assert("Disconnect successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}


void test2_onSubscribe1(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback, context %p", context);

	assert("Correct subscribe return code", response->alt.qos == 2,
			"qos was %d", response->alt.qos);
}


void test2_onConnect1(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback 1, context %p", context);

	assert("Correct serverURI returned", strcmp(response->alt.connect.serverURI, options.connection) == 0,
			"serverURI was %s", response->alt.connect.serverURI);
	assert("Correct MQTTVersion returned", response->alt.connect.MQTTVersion == 4,
			"MQTTVersion was %d", response->alt.connect.MQTTVersion);
	assert("Correct sessionPresent returned", response->alt.connect.sessionPresent == 0,
			"sessionPresent was %d", response->alt.connect.sessionPresent);

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	opts.context = c;

	opts.onSuccess = test2_onSubscribe1;
	rc = MQTTAsync_subscribe(c, "a topic I can subscribe to", 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished = 1;

	opts.onSuccess = NULL;
	opts.onFailure = test2_onSubscribe2;
	rc = MQTTAsync_subscribe(c, "nosubscribe", 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		test_finished = 1;
}



/*********************************************************************

Test1: 0x80 from subscribe

*********************************************************************/
int test2(struct Options options)
{
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test1";

	fprintf(xml, "<testcase classname=\"test2\" name=\"bad subscribe\"");
	global_start_time = start_clock();
	test_finished = failures = 0;
	MyLog(LOGA_INFO, "Starting test 2 - bad subscribe");

	rc = MQTTAsync_create(&c, options.connection, "badSubscribe test",
			MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.MQTTVersion = 4;
	if (options.haconnections != NULL)
	{
		opts.serverURIs = options.haconnections;
		opts.serverURIcount = options.hacount;
	}
	opts.onSuccess = test2_onConnect1;
	opts.onFailure = NULL;
	opts.context = c;

	/* Connect cleansession */
	opts.cleansession = 1;
	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
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
	MyLog(LOGA_INFO, "TEST2: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", tests, failures);
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
