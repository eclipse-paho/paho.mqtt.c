/*******************************************************************************
 * Copyright (c) 2012, 2024 IBM Corp., Ian Craggs
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
 *    Allan Stockdill-Mander - initial API and implementation and/or initial documentation
 *    Ian Craggs - fix Windows includes
 *******************************************************************************/

/**
 * @file
 * SSL tests for the Eclipse Paho Asynchronous MQTT C client
 */

#include "MQTTAsync.h"
#include <string.h>
#include <stdlib.h>
#include "Thread.h"

#if defined(_WINDOWS)
#include <windows.h>
#include <openssl/applink.c>
#define MAXHOSTNAMELEN 256
#define snprintf _snprintf
#else
#include <sys/time.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

void usage(void)
{
	printf("Options:\n");
	printf("\t--test_no <test_no> - Run test number <test_no>\n");
	printf("\t--hostname <hostname> - Connect to <hostname> for tests\n");
	printf("\t--client_key <key_file> - Use <key_file> as the client certificate for SSL authentication\n");
	printf("\t--client_key_pass <password> - Use <password> to access the private key in the client certificate\n");
	printf("\t--server_key <key_file> - Use <key_file> as the trusted certificate for server\n");
	printf("\t--verbose - Enable verbose output \n");
	printf("\t--help - This help output\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	char connection[100]; /**< connection to system under test. */
	char mutual_auth_connection[100];   /**< connection to system under test. */
	char nocert_mutual_auth_connection[100];
	char server_auth_connection[100];
	char anon_connection[100];
	char psk_connection[100];
	char* client_key_file;
	char* client_key_pass;
	char* server_key_file;
	char* client_private_key_file;
	char* capath;
	int verbose;
	int test_no;
	int size;
	int websockets;
	int message_count;
	int start_port;
} options =
{
	"ssl://localhost:18883",
	"mqtts://localhost:18884",
	"ssl://localhost:18887",
	"mqtts://localhost:18885",
	"ssl://localhost:18886",
	"mqtts://localhost:18888",
	NULL, // "../../../test/ssl/client.pem",
	NULL,
	NULL, // "../../../test/ssl/test-root-ca.crt",
	NULL, // "../../../test/ssl/capath",
	NULL,
	0,
	0,
	5000000,
	0,
	3,
	18883,
};

typedef struct
{
	MQTTAsync client;
	char clientid[24];
	char topic[100];
	int maxmsgs;
	int rcvdmsgs[3];
	int sentmsgs[3];
	int testFinished;
	int subscribed;
} AsyncTestClient;

#define AsyncTestClient_initializer {NULL, "\0", "\0", 0, {0, 0, 0}, {0, 0, 0}, 0, 0}

void getopts(int argc, char** argv)
{
	int count = 1;

	while (count < argc)
	{
		if (strcmp(argv[count], "--help") == 0)
		{
			usage();
		}
		else if (strcmp(argv[count], "--test_no") == 0)
		{
			if (++count < argc)
				options.test_no = atoi(argv[count]);
			else
				usage();
		}
		else if (strcmp(argv[count], "--client_key") == 0)
		{
			if (++count < argc)
				options.client_key_file = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--client_key_pass") == 0)
		{
			if (++count < argc)
				options.client_key_pass = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--server_key") == 0)
		{
			if (++count < argc)
				options.server_key_file = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--capath") == 0)
		{
			if (++count < argc)
				options.capath = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--verbose") == 0)
		{
			options.verbose = 1;
			printf("\nSetting verbose on\n");
		}
		else if (strcmp(argv[count], "--hostname") == 0)
		{
			if (++count < argc)
			{
				char* prefix = (options.websockets) ? "wss" : "ssl";

				sprintf(options.connection, "%s://%s:%d", prefix, argv[count],
						options.start_port);
				printf("Setting connection to %s\n", options.connection);
				sprintf(options.mutual_auth_connection, "%s://%s:%d", prefix, argv[count],
						options.start_port+1);
				printf("Setting mutual_auth_connection to %s\n", options.mutual_auth_connection);
				sprintf(options.nocert_mutual_auth_connection, "%s://%s:%d", prefix,
						argv[count], options.start_port+4);
				printf("Setting nocert_mutual_auth_connection to %s\n",
					options.nocert_mutual_auth_connection);
				sprintf(options.server_auth_connection, "%s://%s:%d", prefix, argv[count],
						options.start_port+2);
				printf("Setting server_auth_connection to %s\n", options.server_auth_connection);
				sprintf(options.anon_connection, "%s://%s:%d", prefix, argv[count],
						options.start_port+3);
				printf("Setting anon_connection to %s\n", options.anon_connection);
				sprintf(options.psk_connection, "%s://%s:%d", prefix, argv[count],
						options.start_port+5);
				printf("Setting psk_connection to %s\n", options.psk_connection);
			}
			else
				usage();
		}
		else if (strcmp(argv[count], "--ws") == 0)
		{
			options.websockets = 1;
			printf("\nSetting websockets on\n");
		}
		else if (strcmp(argv[count], "--size") == 0)
		{
			if (++count < argc)
			{
				options.size = atoi(argv[count]);
				printf("\nSetting size to %d\n", options.size);
			}else
				usage();
		}
		else if (strcmp(argv[count], "--port") == 0)
		{
			if (++count < argc)
			{
				options.start_port = atoi(argv[count]);
				printf("\nSetting start_port to %d\n", options.start_port);
			}else
				usage();
		}
		else if (strcmp(argv[count], "--count") == 0)
		{
			if (++count < argc)
			{
				options.message_count = atoi(argv[count]);
				printf("\nSetting message count to %d\n", options.message_count);
			}else
				usage();
		}
		else
			printf("Unrecognized option %s\n", argv[count]);
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
		printf("Assertion failed, file %s, line %d, description: %s", filename,
				lineno, description);

		va_start(args, format);
		vprintf(format, args);
		va_end(args);

		cur_output += sprintf(cur_output, "<failure type=\"%s\">file %s, line %d </failure>\n",
                        description, filename, lineno);
	}
	else
		MyLog(LOGA_DEBUG,
				"Assertion succeeded, file %s, line %d, description: %s",
				filename, lineno, description);
}

/*********************************************************************

 Test: multi-threaded client using callbacks

 *********************************************************************/
volatile int multiThread_arrivedcount = 0;
int multiThread_deliveryCompleted = 0;
MQTTAsync_message multiThread_pubmsg = MQTTAsync_message_initializer;

void multiThread_deliveryComplete(void* context, MQTTAsync_token dt)
{
	++multiThread_deliveryCompleted;
}

int multiThread_messageArrived(void* context, char* topicName, int topicLen,
		MQTTAsync_message* m)
{
	++multiThread_arrivedcount;
	MyLog(LOGA_DEBUG, "Callback: %d message received on topic %s is %.*s.",
			multiThread_arrivedcount, topicName, m->payloadlen,
			(char*) (m->payload));
	if (multiThread_pubmsg.payloadlen != m->payloadlen || memcmp(m->payload,
			multiThread_pubmsg.payload, m->payloadlen) != 0)
	{
		failures++;
		MyLog(LOGA_INFO, "Error: wrong data received lengths %d %d\n",
				multiThread_pubmsg.payloadlen, m->payloadlen);
	}
	MQTTAsync_free(topicName);
	MQTTAsync_freeMessage(&m);
	return 1;
}

void sendAndReceive(MQTTAsync* c, int qos, char* test_topic)
{
	MQTTAsync_responseOptions ropts;
	int i = 0;
	int iterations = 50;
	int rc = 0;
	int wait_seconds = 0;

	multiThread_deliveryCompleted = 0;
	multiThread_arrivedcount = 0;

	MyLog(LOGA_DEBUG, "%d messages at QoS %d", iterations, qos);
	multiThread_pubmsg.payload
			= "a much longer message that we can shorten to the extent that we need to";
	multiThread_pubmsg.payloadlen = 27;
	multiThread_pubmsg.qos = qos;
	multiThread_pubmsg.retained = 0;

	for (i = 1; i <= iterations; ++i)
	{
		if (i % 10 == 0)
			rc = MQTTAsync_send(c, test_topic, multiThread_pubmsg.payloadlen,
					multiThread_pubmsg.payload, multiThread_pubmsg.qos,
					multiThread_pubmsg.retained, NULL);
		else
			rc = MQTTAsync_sendMessage(c, test_topic, &multiThread_pubmsg,
					&ropts);
		assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

#if defined(_WIN32)
		Sleep(100);
#else
		usleep(100000L);
#endif

		wait_seconds = 10;
		while ((multiThread_arrivedcount < i) && (wait_seconds-- > 0))
		{
			MyLog(LOGA_DEBUG, "Arrived %d count %d", multiThread_arrivedcount,
					i);
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
		while ((multiThread_deliveryCompleted < iterations) && (wait_seconds--
				> 0))
		{
			MyLog(LOGA_DEBUG, "Delivery Completed %d count %d",
					multiThread_deliveryCompleted, i);
#if defined(_WIN32)
			Sleep(1000);
#else
			usleep(1000000L);
#endif
		}
		assert("All Deliveries Complete", wait_seconds > 0,
				"Number of deliveryCompleted callbacks was %d\n",
				multiThread_deliveryCompleted);
	}
}

/*********************************************************************

 Async Callbacks - generic callbacks for send/receive tests

 *********************************************************************/

/*static mutex_type client_mutex = NULL;
static pthread_mutex_t client_mutex_store = PTHREAD_MUTEX_INITIALIZER;
static mutex_type client_mutex = &client_mutex_store;*/

void asyncTestOnDisconnect(void* context, MQTTAsync_successData* response)
{
	//int rc;

	AsyncTestClient* tc = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In asyncTestOnDisconnect callback, %s", tc->clientid);
	//rc = Thread_lock_mutex(client_mutex);
	tc->testFinished = 1;
	//rc = Thread_unlock_mutex(client_mutex);
}

void asyncTestOnSend(void* context, MQTTAsync_successData* response)
{
	AsyncTestClient* tc = (AsyncTestClient*) context;
	//int rc;
	int qos = response->alt.pub.message.qos;
	MyLog(LOGA_DEBUG, "In asyncTestOnSend callback, %s", tc->clientid);
	//rc = Thread_lock_mutex(client_mutex);
	tc->sentmsgs[qos]++;
	//rc = Thread_unlock_mutex(client_mutex);
}

void asyncTestOnSubscribeFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* tc = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In asyncTestOnSubscribeFailure callback, %s",
			tc->clientid);

	assert("There should be no failures in this test. ", 0, "asyncTestOnSubscribeFailure callback was called\n", 0);
}

void asyncTestOnUnsubscribe(void* context, MQTTAsync_successData* response)
{
	AsyncTestClient* tc = (AsyncTestClient*) context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In asyncTestOnUnsubscribe callback, %s", tc->clientid);
	opts.onSuccess = asyncTestOnDisconnect;
	opts.context = tc;
	opts.timeout = 1000;

	rc = MQTTAsync_disconnect(tc->client, &opts);
}

void asyncTestOnSubscribe(void* context, MQTTAsync_successData* response)
{
	AsyncTestClient* tc = (AsyncTestClient*) context;
	int rc, i;
	MyLog(LOGA_DEBUG, "In asyncTestOnSubscribe callback, %s", tc->clientid);
	//rc = Thread_lock_mutex(client_mutex);
	tc->subscribed = 1;
	//rc = Thread_unlock_mutex(client_mutex);
	for (i = 0; i < 3; i++)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;

		pubmsg.payload
				= "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.qos = i;
		pubmsg.retained = 0;

		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
		//opts.onSuccess = asyncTestOnSend;
		opts.context = &tc;

		rc = MQTTAsync_send(tc->client, tc->topic, pubmsg.payloadlen,
				pubmsg.payload, pubmsg.qos, pubmsg.retained, &opts);
		assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		tc->sentmsgs[i]++;
		MyLog(LOGA_DEBUG, "Maxmsgs %d", tc->maxmsgs);
	}
}

int asyncTestMessageArrived(void* context, char* topicName, int topicLen,
		MQTTAsync_message* m)
{
	AsyncTestClient* tc = (AsyncTestClient*) context;
	int rc;
	//rc = Thread_lock_mutex(client_mutex);
	tc->rcvdmsgs[m->qos]++;

	//printf("Received messages: %d\n", tc->rcvdmsgs[m->qos]);

	MyLog(LOGA_DEBUG,
			"In asyncTestMessageArrived callback, %s total to exit %d, total received %d,%d,%d",
			tc->clientid, (tc->maxmsgs * 3), tc->rcvdmsgs[0], tc->rcvdmsgs[1],
			tc->rcvdmsgs[2]);

	if (tc->sentmsgs[m->qos] < tc->maxmsgs)
	{
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		//opts.onSuccess = asyncTestOnSend;
		opts.context = tc;

		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		pubmsg.payload
				= "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.qos = m->qos;
		pubmsg.retained = 0;

		rc = MQTTAsync_send(tc->client, tc->topic, pubmsg.payloadlen,
				pubmsg.payload, pubmsg.qos, pubmsg.retained, &opts);
		assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d messages sent %d,%d,%d", rc);
		MyLog(LOGA_DEBUG, "Messages sent %d,%d,%d", tc->sentmsgs[0],
				tc->sentmsgs[1], tc->sentmsgs[2]);
		tc->sentmsgs[m->qos]++;
	}
	if ((tc->rcvdmsgs[0] + tc->rcvdmsgs[1] + tc->rcvdmsgs[2]) == (tc->maxmsgs* 3))
	{
		MyLog(LOGA_DEBUG, "Ready to unsubscribe");
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		opts.onSuccess = asyncTestOnUnsubscribe;
		opts.context = tc;
		rc = MQTTAsync_unsubscribe(tc->client, tc->topic, &opts);
		assert("Unsubscribe successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}
	//rc = Thread_unlock_mutex(client_mutex);
	MyLog(LOGA_DEBUG, "Leaving asyncTestMessageArrived callback");
	MQTTAsync_freeMessage(&m);
	MQTTAsync_free(topicName);
	return 1;
}

void asyncTestOnDeliveryComplete(void* context, MQTTAsync_token token)
{

}

void asyncTestOnConnect(void* context, MQTTAsync_successData* response)
{
	AsyncTestClient* tc = (AsyncTestClient*) context;
	int subsqos = 2;
	int rc;
	MyLog(LOGA_DEBUG, "In asyncTestOnConnect callback, %s", tc->clientid);

	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	opts.onSuccess = asyncTestOnSubscribe;
	opts.onFailure = asyncTestOnSubscribeFailure;
	opts.context = tc;

	rc = MQTTAsync_subscribe(tc->client, tc->topic, subsqos, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}

/*********************************************************************

 Test1: SSL connection to non SSL MQTT server

 *********************************************************************/

int test1Finished = 0;

int test1OnFailureCalled = 0;

void test1OnFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In connect onFailure callback, context %p", context);

	test1OnFailureCalled++;
	test1Finished = 1;
}

void test1OnConnect(void* context, MQTTAsync_successData* response)
{

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p\n", context);

	assert("Connect should not succeed", 0, "connect success callback was called", 0);

	test1Finished = 1;
}

int test1(struct Options options)
{
	char* testname = "test1";
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;
	char* test_topic = "C client SSL test1";
	int count = 0;

	test1Finished = 0;
	failures = 0;
	MyLog(LOGA_INFO, "Starting SSL test 1 - connection to nonSSL MQTT server");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, "rubbish://wrong", "test1", MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL);
	assert("bad rc from create", rc == MQTTASYNC_BAD_PROTOCOL, "rc was %d \n", rc);

	rc = MQTTAsync_create(&c, options.connection, "test1", MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d \n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = test1OnConnect;
	opts.onFailure = test1OnFailure;
	opts.context = c;

	rc = MQTTAsync_connect(c, &opts);
	assert("Bad rc from connect", rc == MQTTASYNC_NULL_PARAMETER, "rc was %d ", rc);

	opts.ssl = &sslopts;
	opts.ssl->enableServerCertAuth = 0;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d ", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	/* wait for success or failure callback */
	while (!test1Finished && ++count < 10000)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	exit: MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test2a: Mutual SSL Authentication - Certificates in place on client and server

 *********************************************************************/

void test2aOnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test2aOnConnectFailure callback, %s",
			client->clientid);

	assert("There should be no failures in this test. ", 0, "test2aOnConnectFailure callback was called\n", 0);
	client->testFinished = 1;
}

void test2aOnPublishFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test2aOnPublishFailure callback, %s",
			client->clientid);

	assert("There should be no failures in this test. ", 0, "test2aOnPublishFailure callback was called\n", 0);
}

int test2a(struct Options options)
{
	char* testname = "test2a";

	AsyncTestClient tc =
	AsyncTestClient_initializer;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 2a - Mutual SSL authentication");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, options.mutual_auth_connection, "test2a", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	tc.client = c;
	sprintf(tc.clientid, "%s", testname);
	sprintf(tc.topic, "C client SSL test2a");
	tc.maxmsgs = MAXMSGS;
	//tc.rcvdmsgs = 0;
	tc.subscribed = 0;
	tc.testFinished = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = asyncTestOnConnect;
	opts.onFailure = test2aOnConnectFailure;
	opts.context = &tc;

	opts.ssl = &sslopts;
	if (options.server_key_file != NULL)
		opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
	opts.ssl->keyStore = options.client_key_file; /*file of certificate for client to present to server*/
	if (options.client_key_pass != NULL)
		opts.ssl->privateKeyPassword = options.client_key_pass;
	//opts.ssl->enabledCipherSuites = "DEFAULT";
	//opts.ssl->enabledServerCertAuth = 1;
	opts.ssl->verify = 1;
	MyLog(LOGA_DEBUG, "enableServerCertAuth %d\n", opts.ssl->enableServerCertAuth);
	MyLog(LOGA_DEBUG, "verify %d\n", opts.ssl->verify);

	rc = MQTTAsync_setCallbacks(c, &tc, NULL, asyncTestMessageArrived,
			asyncTestOnDeliveryComplete);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!tc.subscribed && !tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	if (tc.testFinished)
		goto exit;

	while (!tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	MyLog(LOGA_DEBUG, "Stopping");

	exit: MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test2b: Mutual SSL Authentication - Server does not have Client cert

 *********************************************************************/

int test2bFinished;

void test2bOnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In test2bOnConnectFailure callback, context %p", context);

	assert("This test should call test2bOnConnectFailure. ", 1, "test2bOnConnectFailure callback was called\n", 1);
	test2bFinished = 1;
}

void test2bOnConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In test2bOnConnectFailure callback, context %p", context);

	assert("This connect should not succeed. ", 0, "test2bOnConnect callback was called\n", 0);
	test2bFinished = 1;
}

int test2b(struct Options options)
{
	char* testname = "test2b";
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;
	int count = 0;

	test2bFinished = 0;
	failures = 0;
	MyLog(LOGA_INFO,
			"Starting test 2b - connection to SSL MQTT server with clientauth=req but server does not have client cert");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, options.nocert_mutual_auth_connection,
            "test2b", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = test2bOnConnect;
	opts.onFailure = test2bOnConnectFailure;
	opts.context = c;

	opts.ssl = &sslopts;
	if (options.server_key_file != NULL)
		opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
	opts.ssl->keyStore = options.client_key_file; /*file of certificate for client to present to server*/
	if (options.client_key_pass != NULL)
		opts.ssl->privateKeyPassword = options.client_key_pass;
	//opts.ssl->enabledCipherSuites = "DEFAULT";
	//opts.ssl->enabledServerCertAuth = 0;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test2bFinished && ++count < 10000)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	exit: MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test2c: Mutual SSL Authentication - Client does not have Server cert

 *********************************************************************/

int test2cFinished;

void test2cOnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In test2cOnConnectFailure callback, context %p", context);

	assert("This test should call test2cOnConnectFailure. ", 1, "test2cOnConnectFailure callback was called\n", 0);
	test2cFinished = 1;
}

void test2cOnConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In test2cOnConnect callback, context %p", context);

	assert("This connect should not succeed. ", 0, "test2cOnConnect callback was called\n", 0);
	test2cFinished = 1;
}

int test2c(struct Options options)
{
	char* testname = "test2c";
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test2c";
	int count = 0;

	failures = 0;
	MyLog(
			LOGA_INFO,
			"Starting test 2c - connection to SSL MQTT server, server auth enabled but unknown cert");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, options.nocert_mutual_auth_connection,
            "test2c", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = test2cOnConnect;
	opts.onFailure = test2cOnConnectFailure;
	opts.context = c;

	opts.ssl = &sslopts;
	//if (options.server_key_file != NULL) opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
	opts.ssl->keyStore = options.client_key_file; /*file of certificate for client to present to server*/
	if (options.client_key_pass != NULL)
		opts.ssl->privateKeyPassword = options.client_key_pass;
	//opts.ssl->enabledCipherSuites = "DEFAULT";
	//opts.ssl->enabledServerCertAuth = 0;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		failures++;
		goto exit;
	}

	while (!test2cFinished && ++count < 10000)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	exit: MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}


/*********************************************************************

 Test2d: Mutual SSL Authentication - client has no certs

 *********************************************************************/

int test2dFinished;

void test2dOnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In test2dOnConnectFailure callback, context %p", context);

	assert("This test should call test2dOnConnectFailure. ", 1, "test2dOnConnectFailure callback was called\n", 0);
	test2dFinished = 1;
}

void test2dOnConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In test2dOnConnect callback, context %p", context);

	assert("This connect should not succeed. ", 0, "test2dOnConnect callback was called\n", 0);
	test2dFinished = 1;
}

int test2d(struct Options options)
{
	char* testname = "test2d";
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test2d";
	int count = 0;
	unsigned int iteration = 0;

	failures = 0;
	MyLog(
			LOGA_INFO,
			"Starting test 2d - connection to SSL MQTT server, server auth enabled but unknown cert");
	fprintf(xml, "<testcase classname=\"test2d\" name=\"%s\"", testname);
	global_start_time = start_clock();

	// As reported in https://github.com/eclipse/paho.mqtt.c/issues/190
	// there is/was some race condition, which caused _sometimes_ that the library failed to detect,
	// that the connect attempt has already failed.
	// Therefore we need to test this several times!
	for (iteration = 0; !failures && (iteration < 20) ; iteration++)
	{
		count = 0;
		MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);

		rc = MQTTAsync_create(&c, options.mutual_auth_connection,
				      "test2d", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
		assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);

		if (rc != MQTTASYNC_SUCCESS)
		{
			MQTTAsync_destroy(&c);
			failures++;
			break;
		}

		opts.keepAliveInterval = 60;
		opts.cleansession = 1;

		opts.will = &wopts;
		opts.will->message = "will message";
		opts.will->qos = 1;
		opts.will->retained = 0;
		opts.will->topicName = "will topic";
		opts.will = NULL;
		opts.onSuccess = test2dOnConnect;
		opts.onFailure = test2dOnConnectFailure;
		opts.context = c;

		opts.ssl = &sslopts;
		if (options.server_key_file != NULL) opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
		opts.ssl->keyStore = NULL; /*file of certificate for client to present to server - In this test the client has no certificate! */

		test2dFinished = 0;
		MyLog(LOGA_DEBUG, "Connecting");
		rc = MQTTAsync_connect(c, &opts);
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
		if (rc != MQTTASYNC_SUCCESS)
		{
			failures++;
			MyLog(LOGA_INFO, "Failed in iteration %d\n",iteration);
			MQTTAsync_destroy(&c);
			break;
		}
#define TEST2D_COUNT 1000
		while (!test2dFinished && ++count < TEST2D_COUNT)
		{
#if defined(_WIN32)
			Sleep(100);
#else
			usleep(10000L);
#endif
		}
		if (!test2dFinished && count >= TEST2D_COUNT)
		{
			MyLog(LOGA_INFO, "Failed in iteration %d\n",iteration);
			failures++;
		}
		MQTTAsync_destroy(&c);
	}
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test2e: Mutual SSL Authentication using serverURIs

 *********************************************************************/

void test2eOnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test2eOnConnectFailure callback, %s",
			client->clientid);

	assert("There should be no failures in this test. ", 0, "test2eOnConnectFailure callback was called\n", 0);
	client->testFinished = 1;
}

void test2eOnPublishFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test2eOnPublishFailure callback, %s",
			client->clientid);

	assert("There should be no failures in this test. ", 0, "test2eOnPublishFailure callback was called\n", 0);
}

int test2e(struct Options options)
{
	char* testname = "test2e";

	AsyncTestClient tc =
	AsyncTestClient_initializer;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	char* uris[2] = {"rubbish", options.mutual_auth_connection};
	int rc = 0;

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 2e - Mutual SSL authentication with serverURIs");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, "none", "test2e", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	tc.client = c;
	sprintf(tc.clientid, "%s", testname);
	sprintf(tc.topic, "C client SSL test2e");
	tc.maxmsgs = MAXMSGS;
	//tc.rcvdmsgs = 0;
	tc.subscribed = 0;
	tc.testFinished = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = asyncTestOnConnect;
	opts.onFailure = test2eOnConnectFailure;
	opts.context = &tc;
	opts.serverURIs = uris;
	opts.serverURIcount = 2;

	opts.ssl = &sslopts;
	if (options.server_key_file != NULL)
		opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
	opts.ssl->keyStore = options.client_key_file; /*file of certificate for client to present to server*/
	if (options.client_key_pass != NULL)
		opts.ssl->privateKeyPassword = options.client_key_pass;
	//opts.ssl->enabledCipherSuites = "DEFAULT";
	//opts.ssl->enabledServerCertAuth = 1;
	opts.ssl->verify = 1;
	MyLog(LOGA_DEBUG, "enableServerCertAuth %d\n", opts.ssl->enableServerCertAuth);
	MyLog(LOGA_DEBUG, "verify %d\n", opts.ssl->verify);

	rc = MQTTAsync_setCallbacks(c, &tc, NULL, asyncTestMessageArrived,
			asyncTestOnDeliveryComplete);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!tc.subscribed && !tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	if (tc.testFinished)
		goto exit;

	while (!tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	MyLog(LOGA_DEBUG, "Stopping");

	exit: MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test3a: Server Authentication - server certificate in client trust store

 *********************************************************************/

void test3aOnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test3aOnConnectFailure callback, context %p", context);

	assert("There should be no failures in this test. ", 0, "test3aOnConnectFailure callback was called\n", 0);
	client->testFinished = 1;
}

int test3a(struct Options options)
{
	char* testname = "test3a";
	int subsqos = 2;
	/* TODO - usused - remove ? MQTTAsync_deliveryToken* dt = NULL; */
	AsyncTestClient tc =
	AsyncTestClient_initializer;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;
	int i;

	failures = 0;

	MyLog(LOGA_INFO, "Starting test 3a - Server authentication");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	MQTTAsync_create(&c, options.server_auth_connection, "test3a", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);

	tc.client = c;
	sprintf(tc.clientid, "%s", testname);
	sprintf(tc.topic, "C client SSL test3a");
	tc.maxmsgs = MAXMSGS;
	//tc.rcvdmsgs = 0;
	tc.subscribed = 0;
	tc.testFinished = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = asyncTestOnConnect;
	opts.onFailure = test3aOnConnectFailure;
	opts.context = &tc;

	opts.ssl = &sslopts;
	if (options.server_key_file != NULL)
		opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
	//opts.ssl->keyStore = options.client_key_file;  /*file of certificate for client to present to server*/
	//if (options.client_key_pass != NULL) opts.ssl->privateKeyPassword = options.client_key_pass;
	//opts.ssl->enabledCipherSuites = "DEFAULT";
	//opts.ssl->enabledServerCertAuth = 1;

	rc = MQTTAsync_setCallbacks(c, &tc, NULL, asyncTestMessageArrived,
			asyncTestOnDeliveryComplete);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!tc.subscribed && !tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	if (tc.testFinished)
		goto exit;

	for (i = 0; i < 3; i++)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;

		pubmsg.payload
				= "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.qos = i;
		pubmsg.retained = 0;

		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
		opts.onSuccess = asyncTestOnSend;
		opts.context = &tc;

		rc = MQTTAsync_send(c, tc.topic, pubmsg.payloadlen, pubmsg.payload,
				pubmsg.qos, pubmsg.retained, &opts);
		assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}

	while (!tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	MyLog(LOGA_DEBUG, "Stopping");

	MQTTAsync_destroy(&c);

	exit: MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.", (failures
			== 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test3b: Server Authentication - Client does not have server cert

 *********************************************************************/

int test3bFinished;

void test3bOnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In test3bOnConnectFailure callback, context %p", context);

	assert("This test should call test3bOnConnectFailure. ", 1, "test3bOnConnectFailure callback was called\n", 1);
	test3bFinished = 1;
}

void test3bOnConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In test3bOnConnectFailure callback, context %p", context);

	assert("This connect should not succeed. ", 0, "test3bOnConnect callback was called\n", 0);
	test3bFinished = 1;
}

int test3b(struct Options options)
{
	char* testname = "test3b";
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;
	int count = 0;

	test3bFinished = 0;
	failures = 0;
	MyLog(
			LOGA_INFO,
			"Starting test 3b - connection to SSL MQTT server with clientauth=opt but client does not have server cert");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, options.server_auth_connection, "test3b", MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = test3bOnConnect;
	opts.onFailure = test3bOnConnectFailure;
	opts.context = c;

	opts.ssl = &sslopts;
	//if (options.server_key_file != NULL) opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
	//opts.ssl->keyStore = options.client_key_file;  /*file of certificate for client to present to server*/
	//if (options.client_key_pass != NULL) opts.ssl->privateKeyPassword = options.client_key_pass;
	//opts.ssl->enabledCipherSuites = "DEFAULT";
	//opts.ssl->enabledServerCertAuth = 0;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test3bFinished && ++count < 10000)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	exit: MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test4: Accept invalid server certificates

 *********************************************************************/

void test4OnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test4OnConnectFailure callback, context %p", context);

	assert("There should be no failures in this test. ", 0, "test4OnConnectFailure callback was called\n", 0);
	client->testFinished = 1;
}

void test4OnPublishFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In test4OnPublishFailure callback, context %p", context);

	assert("There should be no failures in this test. ", 0, "test4OnPublishFailure callback was called\n", 0);
}

int test4(struct Options options)
{
	char* testname = "test4";
	int subsqos = 2;
	/* TODO - usused - remove ? MQTTAsync_deliveryToken* dt = NULL; */
	AsyncTestClient tc =
	AsyncTestClient_initializer;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;
	int i;

	failures = 0;

	MyLog(LOGA_INFO, "Starting test 4 - accept invalid server certificates");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	MQTTAsync_create(&c, options.server_auth_connection, "test4", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);

	tc.client = c;
	sprintf(tc.clientid, "%s", testname);
	sprintf(tc.topic, "C client SSL test4");
	tc.maxmsgs = MAXMSGS;
	//tc.rcvdmsgs = 0;
	tc.subscribed = 0;
	tc.testFinished = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = asyncTestOnConnect;
	opts.onFailure = test4OnConnectFailure;
	opts.context = &tc;

	opts.ssl = &sslopts;
	//if (options.server_key_file != NULL) opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
	//opts.ssl->keyStore = options.client_key_file;  /*file of certificate for client to present to server*/
	//if (options.client_key_pass != NULL) opts.ssl->privateKeyPassword = options.client_key_pass;
	//opts.ssl->enabledCipherSuites = "DEFAULT";
	opts.ssl->enableServerCertAuth = 0;

	rc = MQTTAsync_setCallbacks(c, &tc, NULL, asyncTestMessageArrived,
			asyncTestOnDeliveryComplete);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!tc.subscribed && !tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	if (tc.testFinished)
		goto exit;

	while (!tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	MyLog(LOGA_DEBUG, "Stopping");

	exit: MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test5a: Anonymous ciphers - server auth disabled

 *********************************************************************/

void test5aOnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test5aOnConnectFailure callback, context %p", context);

	assert("There should be no failures in this test. ", 0, "test5aOnConnectFailure callback was called\n", 0);
	client->testFinished = 1;
}

void test5aOnPublishFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In test5aOnPublishFailure callback, context %p", context);

	assert("There should be no failures in this test. ", 0, "test5aOnPublishFailure callback was called\n", 0);
}

int test5a(struct Options options)
{
	char* testname = "test5a";

	AsyncTestClient tc =
	AsyncTestClient_initializer;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;
	int i;

	failures = 0;

	MyLog(LOGA_INFO,
			"Starting SSL test 5a - Anonymous ciphers - server authentication disabled");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, options.anon_connection, "test5a", MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	tc.client = c;
	sprintf(tc.clientid, "%s", testname);
	sprintf(tc.topic, "C client SSL test5a");
	tc.maxmsgs = MAXMSGS;
	//tc.rcvdmsgs = 0;
	tc.subscribed = 0;
	tc.testFinished = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = asyncTestOnConnect;
	opts.onFailure = test5aOnConnectFailure;
	opts.context = &tc;

	opts.ssl = &sslopts;
	//opts.ssl->trustStore = /*file of certificates trusted by client*/
	//opts.ssl->keyStore = options.client_key_file;  /*file of certificate for client to present to server*/
	//if (options.client_key_pass != NULL) opts.ssl->privateKeyPassword = options.client_key_pass;
	opts.ssl->enabledCipherSuites = "aNULL";
	opts.ssl->enableServerCertAuth = 0;

	rc = MQTTAsync_setCallbacks(c, &tc, NULL, asyncTestMessageArrived,
			asyncTestOnDeliveryComplete);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!tc.subscribed && !tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	if (tc.testFinished)
		goto exit;

	for (i = 0; i < 3; i++)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;

		pubmsg.payload
				= "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.qos = i;
		pubmsg.retained = 0;

		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
		opts.onSuccess = asyncTestOnSend;
		opts.context = &tc;

		rc = MQTTAsync_send(c, tc.topic, pubmsg.payloadlen, pubmsg.payload,
				pubmsg.qos, pubmsg.retained, &opts);
		assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}

	while (!tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	MyLog(LOGA_DEBUG, "Stopping");

	exit: MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test5b: Anonymous ciphers - server auth enabled

 ********************************************************************/

void test5bOnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test5bOnConnectFailure callback, context %p", context);

	assert("There should be no failures in this test. ", 0, "test5bOnConnectFailure callback was called\n", 0);
	client->testFinished = 1;
}

void test5bOnPublishFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In test5bOnPublishFailure callback, context %p", context);

	assert("There should be no failures in this test. ", 0, "test5bOnPublishFailure callback was called\n", 0);
}

int test5b(struct Options options)
{
	char* testname = "test5b";

	AsyncTestClient tc =
	AsyncTestClient_initializer;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;
	int i;

	failures = 0;

	MyLog(LOGA_INFO,
			"Starting SSL test 5b - Anonymous ciphers - server authentication enabled");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, options.anon_connection, "test5b", MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	tc.client = c;
	sprintf(tc.clientid, "%s", testname);
	sprintf(tc.topic, "C client SSL test5b");
	tc.maxmsgs = MAXMSGS;
	//tc.rcvdmsgs = 0;
	tc.subscribed = 0;
	tc.testFinished = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = asyncTestOnConnect;
	opts.onFailure = test5bOnConnectFailure;
	opts.context = &tc;

	opts.ssl = &sslopts;
	//opts.ssl->trustStore = /*file of certificates trusted by client*/
	//opts.ssl->keyStore = options.client_key_file;  /*file of certificate for client to present to server*/
	//if (options.client_key_pass != NULL) opts.ssl->privateKeyPassword = options.client_key_pass;
	opts.ssl->enabledCipherSuites = "aNULL";
	opts.ssl->enableServerCertAuth = 1;

	rc = MQTTAsync_setCallbacks(c, &tc, NULL, asyncTestMessageArrived,
			asyncTestOnDeliveryComplete);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!tc.subscribed && !tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	if (tc.testFinished)
		goto exit;

	for (i = 0; i < 3; i++)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;

		pubmsg.payload
				= "a much longer message that we can shorten to the extent that we need to payload up to 11";
		pubmsg.payloadlen = 11;
		pubmsg.qos = i;
		pubmsg.retained = 0;

		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
		opts.onSuccess = asyncTestOnSend;
		opts.context = &tc;

		rc = MQTTAsync_send(c, tc.topic, pubmsg.payloadlen, pubmsg.payload,
				pubmsg.qos, pubmsg.retained, &opts);
		assert("Good rc from publish", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}

	while (!tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	MyLog(LOGA_DEBUG, "Stopping");

	exit: MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test5c: Anonymous ciphers - client not using anonymous ciphers

 *********************************************************************/

int test5cFinished;

void test5cOnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In test5cOnConnectFailure callback, context %p", context);

	assert("This test should call test5cOnConnectFailure. ", 1, "test5cOnConnectFailure callback was called\n", 1);
	test5cFinished = 1;
}

void test5cOnConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In test5cOnConnectFailure callback, context %p", context);

	assert("This connect should not succeed. ", 0, "test5cOnConnect callback was called\n", 0);
	test5cFinished = 1;
}

int test5c(struct Options options)
{
	char* testname = "test5c";
	int subsqos = 2;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;
	int count = 0;

	test5cFinished = 0;
	failures = 0;
	MyLog(LOGA_INFO,
			"Starting SSL test 5c - Anonymous ciphers - client not using anonymous cipher");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, options.anon_connection, "test5c", MQTTCLIENT_PERSISTENCE_DEFAULT,
			NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = test5cOnConnect;
	opts.onFailure = test5cOnConnectFailure;
	opts.context = c;

	opts.ssl = &sslopts;
	//opts.ssl->trustStore = /*file of certificates trusted by client*/
	//opts.ssl->keyStore = options.client_key_file;  /*file of certificate for client to present to server*/
	//if (options.client_key_pass != NULL) opts.ssl->privateKeyPassword = options.client_key_pass;
	//opts.ssl->enabledCipherSuites = "DEFAULT";
	opts.ssl->enableServerCertAuth = 0;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test5cFinished && ++count < 10000)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

exit:
	MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test6: More than one client object - simultaneous working.

 *********************************************************************/

void test6OnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test6OnConnectFailure callback, context %p", context);

	assert("There should be no failures in this test. ", 0, "test6OnConnectFailure callback was called\n", 0);
	client->testFinished = 1;
}

void test6OnPublishFailure(void* context, MQTTAsync_failureData* response)
{
	MyLog(LOGA_DEBUG, "In test6OnPublishFailure callback, context %p", context);

	assert("There should be no failures in this test. ", 0, "test6OnPublishFailure callback was called\n", 0);
}

int test6(struct Options options)
{
	char* testname = "test6";
#define num_clients 10
	int subsqos = 2;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;
	int i;
	AsyncTestClient tc[num_clients];
	int test6finished = 0;

	MyLog(LOGA_INFO, "Starting test 6 - multiple connections");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	for (i = 0; i < num_clients; ++i)
	{
		tc[i].maxmsgs = MAXMSGS;
		tc[i].rcvdmsgs[0] = 0;
		tc[i].rcvdmsgs[1] = 0;
		tc[i].rcvdmsgs[2] = 0;
		tc[i].sentmsgs[0] = 0;
		tc[i].sentmsgs[1] = 0;
		tc[i].sentmsgs[2] = 0;
		tc[i].testFinished = 0;
		sprintf(tc[i].clientid, "sslasync_test6_num_%d", i);
		sprintf(tc[i].topic, "sslasync test6 topic num %d", i);

		rc = MQTTAsync_create(&(tc[i].client), options.server_auth_connection, tc[i].clientid,
				MQTTCLIENT_PERSISTENCE_NONE, NULL);
		assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);

		rc = MQTTAsync_setCallbacks(tc[i].client, &tc[i], NULL,
				asyncTestMessageArrived, NULL);
		assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

		opts.keepAliveInterval = 20;
		opts.cleansession = 1;
		opts.username = "testuser";
		opts.password = "testpassword";

		opts.will = &wopts;
		opts.will->message = "will message";
		opts.will->qos = 1;
		opts.will->retained = 0;
		opts.will->topicName = "will topic";
		opts.onSuccess = asyncTestOnConnect;
		opts.onFailure = test6OnConnectFailure;
		opts.context = &tc[i];

		opts.ssl = &sslopts;
		if (options.server_key_file != NULL)
			opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
		opts.ssl->keyStore = options.client_key_file; /*file of certificate for client to present to server*/
		if (options.client_key_pass != NULL)
			opts.ssl->privateKeyPassword = options.client_key_pass;
		//opts.ssl->enabledCipherSuites = "DEFAULT";
		//opts.ssl->enabledServerCertAuth = 1;

		MyLog(LOGA_DEBUG, "Connecting");
		rc = MQTTAsync_connect(tc[i].client, &opts);
		assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}

	while (test6finished < num_clients)
	{
		MyLog(LOGA_DEBUG, "num_clients %d test_finished %d\n", num_clients,
				test6finished);
#if defined(_WIN32)
		Sleep(100);

#else
		usleep(10000L);
#endif
		for (i = 0; i < num_clients; ++i)
		{
			if (tc[i].testFinished)
			{
				test6finished++;
				tc[i].testFinished = 0;
			}
		}
	}

	MyLog(LOGA_DEBUG, "test6: destroying clients");

	for (i = 0; i < num_clients; ++i)
		MQTTAsync_destroy(&tc[i].client);

//exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
		(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test7: Send and receive big messages

 *********************************************************************/

void* test7_payload = NULL;
int test7_payloadlen = 0;

void test7OnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test7OnConnectFailure callback, %s", client->clientid);

	assert("There should be no failures in this test. ", 0, "test7OnConnectFailure callback was called\n", 0);
	client->testFinished = 1;
}

int test7OnPublishSuccessCount = 0;
int test7OnUnsubscribed = 0;

void test7OnPublishSuccess(void* context, MQTTAsync_successData* response)
{
	AsyncTestClient* tc = (AsyncTestClient*) context;

	MyLog(LOGA_DEBUG, "In test7OnPublishSuccess callback, %s, qos %d", tc->clientid,
				response->alt.pub.message.qos);

	test7OnPublishSuccessCount++;
}

void test7OnPublishFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test7OnPublishFailure callback, %s %d", client->clientid);

	assert("There should be no failures in this test. ", 0, "test7OnPublishFailure callback was called\n", 0);
	client->testFinished = 1;
}

void test7OnUnsubscribe(void* context, MQTTAsync_successData* response)
{
	AsyncTestClient* tc = (AsyncTestClient*) context;
	int rc;

	MyLog(LOGA_DEBUG, "In test7OnUnsubscribe callback, %s %d %d", tc->clientid,
			test7OnUnsubscribed, test7OnPublishSuccessCount);

	test7OnUnsubscribed++;
}

int test7MessageArrived(void* context, char* topicName, int topicLen,
		MQTTAsync_message* message)
{
	AsyncTestClient* tc = (AsyncTestClient*) context;
	static int message_count = 0;
	int rc, i;

	MyLog(LOGA_DEBUG, "In messageArrived callback %p", tc);

	assert("Message size correct", message->payloadlen == test7_payloadlen,
			"message size was %d", message->payloadlen);

	for (i = 0; i < options.size; ++i)
	{
		if (((char*) test7_payload)[i] != ((char*) message->payload)[i])
		{
			assert("Message contents correct", ((char*)test7_payload)[i] != ((char*)message->payload)[i],
					"message content was %c", ((char*)message->payload)[i]);
			break;
		}
	}

	if (++message_count == 1)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		pubmsg.payload = test7_payload;
		pubmsg.payloadlen = test7_payloadlen;
		pubmsg.qos = 1;
		pubmsg.retained = 0;
		opts.onSuccess = test7OnPublishSuccess;
		opts.onFailure = test7OnPublishFailure;
		opts.context = tc;

		rc = MQTTAsync_sendMessage(tc->client, tc->topic, &pubmsg, &opts);
		assert("Publish successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}
	else if (message_count < options.message_count)
	{
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		pubmsg.payload = test7_payload;
		pubmsg.payloadlen = test7_payloadlen;
		pubmsg.qos = 0;
		pubmsg.retained = 0;
		opts.onSuccess = test7OnPublishSuccess;
		opts.onFailure = test7OnPublishFailure;
		opts.context = tc;
		rc = MQTTAsync_sendMessage(tc->client, tc->topic, &pubmsg, &opts);
		assert("Publish successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}
	else
	{
		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		opts.onSuccess = test7OnUnsubscribe;
		opts.context = tc;
		rc = MQTTAsync_unsubscribe(tc->client, tc->topic, &opts);
		assert("Unsubscribe successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	}

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	return 1;
}


void test7OnSubscribe(void* context, MQTTAsync_successData* response)
{
	AsyncTestClient* tc = (AsyncTestClient*) context;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc, i;

	MyLog(LOGA_DEBUG, "In subscribe onSuccess callback %p", tc);

	pubmsg.payload = test7_payload = malloc(options.size);
	pubmsg.payloadlen = test7_payloadlen = options.size;

	srand(33);
	for (i = 0; i < options.size; ++i)
		((char*) pubmsg.payload)[i] = rand() % 256;

	pubmsg.qos = 2;
	pubmsg.retained = 0;

	pubmsg.payload = test7_payload;
	pubmsg.payloadlen = test7_payloadlen;
	opts.onSuccess = test7OnPublishSuccess;
	opts.onFailure = test7OnPublishFailure;
	opts.context = tc;

	rc = MQTTAsync_send(tc->client, tc->topic, pubmsg.payloadlen, pubmsg.payload,
			pubmsg.qos, pubmsg.retained, &opts);
	assert("Publish successful", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
}

void test7OnConnect(void* context, MQTTAsync_successData* response)
{
	AsyncTestClient* tc = (AsyncTestClient*) context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	MyLog(LOGA_DEBUG, "In connect onSuccess callback, context %p", context);
	opts.onSuccess = test7OnSubscribe;
	opts.context = tc;

	rc = MQTTAsync_subscribe(tc->client, tc->topic, 2, &opts);
	assert("Good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		tc->testFinished = 1;
}

int test7(struct Options options)
{
	char* testname = "test7";
	int subsqos = 2;
	AsyncTestClient tc = AsyncTestClient_initializer;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
	int rc = 0;
	char* test_topic = "C client test7";
	int test_finished;

	test_finished = failures = 0;

	MyLog(LOGA_INFO, "Starting test 7 - big messages");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	rc = MQTTAsync_create(&c, options.server_auth_connection, "async_test_7", MQTTCLIENT_PERSISTENCE_NONE,
			NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MQTTAsync_destroy(&c);
		goto exit;
	}

	rc = MQTTAsync_setCallbacks(c, &tc, NULL, test7MessageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	tc.client = c;
	sprintf(tc.clientid, "%s", testname);
	sprintf(tc.topic, "C client SSL test7");
	tc.maxmsgs = MAXMSGS;
	//tc.rcvdmsgs = 0;
	tc.subscribed = 0;
	tc.testFinished = 0;

	opts.keepAliveInterval = 60;
	opts.cleansession = 1;
	//opts.username = "testuser";
	//opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = test7OnConnect;
	opts.onFailure = test7OnConnectFailure;
	opts.context = &tc;

	opts.ssl = &sslopts;
	if (options.server_key_file != NULL)
		opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
	if (options.client_key_file != NULL)
		opts.ssl->keyStore = options.client_key_file; /*file of certificate for client to present to server*/
	if (options.client_key_pass != NULL)
		opts.ssl->privateKeyPassword = options.client_key_pass;
	//opts.ssl->enabledCipherSuites = "DEFAULT";
	//opts.ssl->enabledServerCertAuth = 1;

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	rc = 0;
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (tc.testFinished == 0 && test7OnUnsubscribed == 0 && test7OnPublishSuccessCount < options.message_count)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(1000L);
#endif

	dopts.onSuccess = asyncTestOnDisconnect;
	dopts.context = &tc;
	rc = MQTTAsync_disconnect(c, &dopts);

	while (!tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(1000L);
#endif

	MQTTAsync_destroy(&c);

	if (test7_payload)
	{
		free(test7_payload);
		test7_payload = NULL;
	}

	exit: MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

Test8: TLS-PSK - client and server has a common pre-shared key

*********************************************************************/

static unsigned int onPSKAuth(const char* hint,
                              char* identity,
                              unsigned int max_identity_len,
                              unsigned char* psk,
                              unsigned int max_psk_len,
                              void* context)
{
	unsigned char test_psk[] = {0x50, 0x53, 0x4B, 0x00}; /* {'P', 'S', 'K', '\0' } */
	MyLog(LOGA_DEBUG, "PSK auth callback");

	assert("Good application context in onPSKAuth", context == (void *) 42, "context was %d\n", context);

	strncpy(identity, "id", max_identity_len);
	memcpy(psk, test_psk, sizeof(test_psk));
	return sizeof(test_psk);
}


int test8(struct Options options)
{
	char* testname = "test8";

	AsyncTestClient tc =
	AsyncTestClient_initializer;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 8 - TLS-PSK - client and server has a common pre-shared key");
	fprintf(xml, "<testcase classname=\"test8\" name=\"%s\"", testname);
	global_start_time = start_clock();

	MQTTAsync_create(&c, options.psk_connection, "test8", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	tc.client = c;
	sprintf(tc.clientid, "%s", testname);
	sprintf(tc.topic, "C client SSL test8");
	tc.maxmsgs = MAXMSGS;
	tc.subscribed = 0;
	tc.testFinished = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.onSuccess = asyncTestOnConnect;
	opts.onFailure = asyncTestOnSubscribeFailure;
	opts.context = &tc;

	opts.ssl = &sslopts;
	opts.ssl->ssl_psk_cb = onPSKAuth;
	opts.ssl->ssl_psk_context = (void *) 42;
	opts.ssl->enabledCipherSuites = "PSK-AES128-CBC-SHA";

	rc = MQTTAsync_setCallbacks(c, &tc, NULL, asyncTestMessageArrived,
			asyncTestOnDeliveryComplete);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!tc.subscribed && !tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	if (tc.testFinished)
		goto exit;

	while (!tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	MyLog(LOGA_DEBUG, "Stopping");

exit:
	MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}


/*********************************************************************

 Test9: Mutual SSL Authentication - Testing CApath

 *********************************************************************/

void test9OnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test9OnConnectFailure callback, %s",
			client->clientid);

	assert("There should be no failures in this test. ", 0, "test9OnConnectFailure callback was called\n", 0);
	client->testFinished = 1;
}

int test9(struct Options options)
{
	char* testname = "test9";

	AsyncTestClient tc =
	AsyncTestClient_initializer;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;

	failures = 0;
	MyLog(LOGA_INFO, "Starting test 9 - Mutual SSL authentication with CApath");
	fprintf(xml, "<testcase classname=\"test5\" name=\"%s\"", testname);
	global_start_time = start_clock();

	MQTTAsync_create(&c, options.mutual_auth_connection, "test9", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	tc.client = c;
	sprintf(tc.clientid, "%s", testname);
	sprintf(tc.topic, "C client SSL test9");
	tc.maxmsgs = MAXMSGS;
	//tc.rcvdmsgs = 0;
	tc.subscribed = 0;
	tc.testFinished = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = asyncTestOnConnect;
	opts.onFailure = test9OnConnectFailure;
	opts.context = &tc;

	opts.ssl = &sslopts;
	if (options.server_key_file != NULL)
		opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
	opts.ssl->keyStore = options.client_key_file; /*file of certificate for client to present to server*/
	if (options.client_key_pass != NULL)
		opts.ssl->privateKeyPassword = options.client_key_pass;
	opts.ssl->CApath = options.capath;
	opts.ssl->enableServerCertAuth = 1;
	opts.ssl->verify = 1;
	MyLog(LOGA_DEBUG, "enableServerCertAuth %d\n", opts.ssl->enableServerCertAuth);
	MyLog(LOGA_DEBUG, "verify %d\n", opts.ssl->verify);

	rc = MQTTAsync_setCallbacks(c, &tc, NULL, asyncTestMessageArrived,
			asyncTestOnDeliveryComplete);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!tc.subscribed && !tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	if (tc.testFinished)
		goto exit;

	while (!tc.testFinished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif

	MyLog(LOGA_DEBUG, "Stopping");

	exit: MQTTAsync_destroy(&c);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();
	return failures;
}

/*********************************************************************

 Test10: Mutual SSL Authentication - Testing CApath

 *********************************************************************/

int test10Finished;

void test10OnConnectFailure(void* context, MQTTAsync_failureData* response)
{
	AsyncTestClient* client = (AsyncTestClient*) context;
	MyLog(LOGA_DEBUG, "In test10OnConnectFailure callback, %s",
			client->clientid);

	assert("This test should call test10OnConnectFailure. ", 1, "test10OnConnectFailure callback was called\n", 1);
	test10Finished = 1;
}

void test10OnConnect(void* context, MQTTAsync_successData* response)
{
	MyLog(LOGA_DEBUG, "In test10OnConnect callback, context %p", context);

	assert("This connect should not succeed. ", 0, "test10OnConnect callback was called\n", 0);
	test10Finished = 1;
}

int test10(struct Options options)
{
	char* testname = "test10";

	AsyncTestClient tc =
	AsyncTestClient_initializer;
	MQTTAsync c;
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_willOptions wopts = MQTTAsync_willOptions_initializer;
	MQTTAsync_SSLOptions sslopts = MQTTAsync_SSLOptions_initializer;
	int rc = 0;

	failures = 0;
	test10Finished = 0;
	MyLog(LOGA_INFO, "Starting test 10 - dummy CApath");
	fprintf(xml, "<testcase classname=\"test10\" name=\"%s\"", testname);
	global_start_time = start_clock();

	MQTTAsync_create(&c, options.mutual_auth_connection, "test10", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;


	tc.client = c;
	sprintf(tc.clientid, "%s", testname);
	sprintf(tc.topic, "C client SSL test10");
	tc.maxmsgs = MAXMSGS;
	//tc.rcvdmsgs = 0;
	tc.subscribed = 0;
	tc.testFinished = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = "testuser";
	opts.password = "testpassword";

	opts.will = &wopts;
	opts.will->message = "will message";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = "will topic";
	opts.will = NULL;
	opts.onSuccess = test10OnConnect;
	opts.onFailure = test10OnConnectFailure;
	opts.context = &tc;

	opts.ssl = &sslopts;
	if (options.server_key_file != NULL)
		opts.ssl->trustStore = options.server_key_file; /*file of certificates trusted by client*/
	opts.ssl->keyStore = options.client_key_file; /*file of certificate for client to present to server*/
	if (options.client_key_pass != NULL)
		opts.ssl->privateKeyPassword = options.client_key_pass;
	opts.ssl->CApath = "DUMMY";
	opts.ssl->enableServerCertAuth = 1;
	opts.ssl->verify = 1;
	MyLog(LOGA_DEBUG, "enableServerCertAuth %d\n", opts.ssl->enableServerCertAuth);
	MyLog(LOGA_DEBUG, "verify %d\n", opts.ssl->verify);

	rc = MQTTAsync_setCallbacks(c, &tc, NULL, asyncTestMessageArrived,
			asyncTestOnDeliveryComplete);
	assert("Good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Connecting");
	rc = MQTTAsync_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d", rc);
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	while (!test10Finished)
#if defined(_WIN32)
		Sleep(100);
#else
		usleep(10000L);
#endif
	MyLog(LOGA_DEBUG, "Stopping");

	exit: MQTTAsync_destroy(&c);
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
	int (*tests[])(struct Options) =
            { NULL, test1, test2a, test2b, test2c, test2d, test3a, test3b, test4, /* test5a,
			test5b, test5c, */ test6, test7, test8, test9, test10, test2e };

	xml = fopen("TEST-test5.xml", "w");
	fprintf(xml, "<testsuite name=\"test5\" tests=\"%d\">\n", (int)ARRAY_SIZE(tests) - 1);

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

/* Local Variables: */
/* indent-tabs-mode: t */
/* c-basic-offset: 8 */
/* End: */
