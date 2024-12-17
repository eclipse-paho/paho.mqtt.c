/*******************************************************************************
 * Copyright (c) 2012, 2017 IBM Corp.
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
 *******************************************************************************/

/**
 * @file
 * Test for issues 373, 385: Memory leak and segmentation fault during connection lost and reconnect
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
#include "Heap.h" // for Heap_get_info
// undefine macros from Heap.h:
#undef malloc
#undef realloc
#undef free

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
	unsigned int QoS;
	unsigned int iterrations;
} options =
{
	"localhost:1883",
	"localhost:1884",
	0,
	0,
	0,
	5
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

void MySleep(long milliseconds)
{
#if defined(_WIN32) || defined(_WIN64)
	Sleep(milliseconds);
#else
	usleep(milliseconds*1000);
#endif
}

#define assert(a, b, c, d) myassert(__FILE__, __LINE__, a, b, c, d)

int tests = 0;
int failures = 0;
int connected = 0;
int pendingMessageCnt = 0; /* counter of messages which are currently queued for publish */
int pendingMessageCntMax = 0;
int failedPublishCnt = 0;
int goodPublishCnt = 0;
int connectCnt = 0;
int connecting = 0;

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
	}
	else
		MyLog(LOGA_DEBUG, "Assertion succeeded, file %s, line %d, description: %s",
				filename, lineno, description);
}


void test1373OnFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_INFO, "In connect onFailure callback, context %p", context);
	connecting = 0;
}

void test373OnConnect(void* context, MQTTAsync_successData* response)
{
	connected = 1;
	connecting = 0;
	connectCnt++;
	MyLog(LOGA_INFO, "Established MQTT connection to %s",response->alt.connect.serverURI);
	char MqttVersion[40];
	switch (response->alt.connect.MQTTVersion)
	{
	case MQTTVERSION_3_1:
		sprintf(MqttVersion," MQTT version 3.1");
		break;
	case MQTTVERSION_3_1_1:
		sprintf(MqttVersion, " MQTT version 3.1.1");
		break;
	default:
		sprintf(MqttVersion, " MQTT version %d",response->alt.connect.MQTTVersion);
	}
	MyLog(LOGA_INFO, " %s\n",MqttVersion);
	MyLog(LOGA_INFO, "connectCnt %d\n",connectCnt);
}

void test373ConnectionLost(void* context, char* cause)
{
	connected = 0;
	MyLog(LOGA_INFO, "Disconnected from MQTT broker reason %s",cause);
}

void test373DeliveryComplete(void* context, MQTTAsync_token token)
{
}

void test373_onWriteSuccess(void* context, MQTTAsync_successData* response)
{
	pendingMessageCnt--;
	goodPublishCnt++;
}

void test373_onWriteFailure(void* context, MQTTAsync_failureData* response)
{
	pendingMessageCnt--;
	failedPublishCnt++;
}

int test373_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	return 0;
}

static char test373Payload[] = "No one is interested in this payload";

int test373SendPublishMessage(MQTTAsync handle,int id, const unsigned int QoS)
{
	int rc = 0;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	char topic[ sizeof(unique) + 40];

	sprintf(topic,"%s/test373/item_%03d",unique,id);
	opts.onFailure = test373_onWriteFailure;
	opts.onSuccess = test373_onWriteSuccess;

	pubmsg.payload = test373Payload;
	pubmsg.payloadlen = sizeof(test373Payload);
	pubmsg.qos = QoS;
	rc = MQTTAsync_sendMessage( handle, topic,&pubmsg,&opts);
	if (rc == MQTTASYNC_SUCCESS)
	{
		pendingMessageCnt++;
		if (pendingMessageCnt > pendingMessageCntMax) pendingMessageCntMax = pendingMessageCnt;
	}
	else
	{
		MyLog(LOGA_INFO, "Failed to queue message for send with retvalue %d",rc);
	}
	return rc;
}

int test_373(struct Options options)
{
	char* testname = "test373";
	MQTTAsync mqttasyncContext;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	int rc = 0;
	char clientid[30 + sizeof(unique)];
	heap_info* mqtt_mem = 0;

	MyLog(LOGA_INFO, "Running test373 with QoS=%u, iterrations=%u\n",options.QoS,options.iterrations);
	sprintf(clientid, "paho-test373-%s", unique);
	connectCnt = 0;
	rc = MQTTAsync_create(&mqttasyncContext, options.proxy_connection, clientid,
			      MQTTCLIENT_PERSISTENCE_NONE,
			      NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		goto exit;
	}
	opts.connectTimeout = 2;
	opts.keepAliveInterval = 20;
	opts.cleansession = 0;
	opts.MQTTVersion = MQTTVERSION_DEFAULT;
	opts.onSuccess = test373OnConnect;
	opts.onFailure = test1373OnFailure;
	opts.context = mqttasyncContext;

	rc = MQTTAsync_setCallbacks(mqttasyncContext,mqttasyncContext,
				    test373ConnectionLost,
				    test373_messageArrived,
				    test373DeliveryComplete);
	if (rc != MQTTASYNC_SUCCESS)
	{
		goto exit;
	}
	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
	while (connectCnt < (int)options.iterrations)
	{
		if (!connected)
		{
			MyLog(LOGA_INFO, "Connected %d connectCnt %d\n",connected,connectCnt);
			MyLog(LOGA_INFO, "PublishCnt %d, FailedCnt %d, Pending %d maxPending %d",
			      goodPublishCnt,failedPublishCnt,pendingMessageCnt,pendingMessageCntMax);
#if !defined(_WINDOWS)
			mqtt_mem = Heap_get_info();
			MyLog(LOGA_INFO, "MQTT mem current %ld, max %ld",mqtt_mem->current_size,mqtt_mem->max_size);
#endif
			/* (re)connect to the broker */
			if (connecting)
			{
				MySleep((1+opts.connectTimeout) * 1000); /* but wait for all pending connect attempts to timeout */
			}
			else
			{
				rc = MQTTAsync_connect(mqttasyncContext, &opts);
				if (rc != MQTTASYNC_SUCCESS)
				{
					failures++;
					goto exit;
				}
				connecting = 1;
			}
		}
		else
		{
			/* while connected send 100 message per second */
			int topicId;
			for(topicId=0; topicId < 100; topicId++)
			{
				rc = test373SendPublishMessage(mqttasyncContext,topicId,options.QoS);
				if (rc != MQTTASYNC_SUCCESS) break;
			}
			MySleep(100);
		}
	}
	MySleep(5000);
	MyLog(LOGA_INFO, "PublishCnt %d, FailedCnt %d, Pending %d maxPending %d",
	      goodPublishCnt,failedPublishCnt,pendingMessageCnt,pendingMessageCntMax);
#if !defined(_WINDOWS)
	mqtt_mem = Heap_get_info();
	MyLog(LOGA_INFO, "MQTT mem current %ld, max %ld",mqtt_mem->current_size,mqtt_mem->max_size);
#endif
	MQTTAsync_disconnect(mqttasyncContext, NULL);
	connected = 0;
	MyLog(LOGA_INFO, "PublishCnt %d, FailedCnt %d, Pending %d maxPending %d",
	      goodPublishCnt,failedPublishCnt,pendingMessageCnt,pendingMessageCntMax);
#if !defined(_WINDOWS)
	mqtt_mem = Heap_get_info();
	MyLog(LOGA_INFO, "MQTT mem current %ld, max %ld",mqtt_mem->current_size,mqtt_mem->max_size);
#endif
exit:
	MQTTAsync_destroy(&mqttasyncContext);
#if !defined(_WINDOWS)
	mqtt_mem = Heap_get_info();
	MyLog(LOGA_INFO, "MQTT mem current %ld, max %ld",mqtt_mem->current_size,mqtt_mem->max_size);
	if (mqtt_mem->current_size > 0) failures++; /* consider any not freed memory as failure */
#endif
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
	int (*tests[])() = { NULL, test_373};
	unsigned int QoS;

	sprintf(unique, "%u", rand());
	MyLog(LOGA_INFO, "Random prefix/suffix is %s", unique);

	MQTTAsync_setTraceCallback(handleTrace);
	getopts(argc, argv);

	if (options.test_no == 0)
	{ /* run all the tests */
		for (options.test_no = 1; options.test_no < ARRAY_SIZE(tests); ++options.test_no)
		{
			/* test with QoS 0, 1 and 2 and just 5 iterrations */
			for (QoS = 0; QoS < 3; QoS++)
			{
				failures = 0;
				options.QoS = QoS;
				options.iterrations = 5;
				MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
				rc += tests[options.test_no](options); /* return number of failures.  0 = test succeeded */
			}
			if (rc == 0)
			{
				/* Test with much more iterrations for QoS = 0 */
				failures = 0;
				options.QoS = 0;
				options.iterrations = 100;
				MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
				rc += tests[options.test_no](options); /* return number of failures.  0 = test succeeded */
			}
		}
	}
	else
	{
		MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
		rc = tests[options.test_no](options); /* run just the selected test */
	}

	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	return rc;
}


/* Local Variables: */
/* indent-tabs-mode: t */
/* c-basic-offset: 8 */
/* End: */
