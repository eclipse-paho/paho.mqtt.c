/*******************************************************************************
 * Copyright (c) 2011, 2020 IBM Corp. and others
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
 *******************************************************************************/


/**
 * @file
 * Async C client program for the MQTT v3 restart/recovery test suite.
 */

#include "MQTTAsync.h"
/*#define NO_HEAP_TRACKING 1
#include "Heap.h"*/
#include <string.h>
#include <stdlib.h>

#if !defined(_WINDOWS)
	#include <sys/time.h>
	#include <unistd.h>
  #include <signal.h>
#else
	#include <windows.h>
#endif

void usage(void)
{
	printf("help!!\n");
	exit(EXIT_FAILURE);
}

static char pub_topic[200];
static char sub_topic[200];

struct
{
	char* connection;         /**< connection to system under test. */
	char** connections;        /**< HA connection list */
	int connection_count;
	char* control_connection; /**< MQTT control connection, for test sync */
	char* topic;
	char* control_topic;
	char* clientid;
	int slot_no;
	int qos;
	int retained;
	char* username;
	char* password;
	int verbose;
	int persistence;
} opts =
{
	"tcp://localhost:1884",
	NULL,
	0,
	"tcp://localhost:7777",
	"Eclipse/Paho/restart_test",
	"Eclipse/Paho/restart_test/control",
	"C_broken_client",
	1,
	2,
	0,
	NULL,
	NULL,
	0,
	0,
};

void getopts(int argc, char** argv)
{
	int count = 1;

	while (count < argc)
	{
		if (strcmp(argv[count], "--qos") == 0)
		{
			if (++count < argc)
			{
				if (strcmp(argv[count], "0") == 0)
					opts.qos = 0;
				else if (strcmp(argv[count], "1") == 0)
					opts.qos = 1;
				else if (strcmp(argv[count], "2") == 0)
					opts.qos = 2;
				else
					usage();
			}
			else
				usage();
		}
		else if (strcmp(argv[count], "--slot_no") == 0)
		{
			if (++count < argc)
				opts.slot_no = atoi(argv[count]);
			else
				usage();
		}
		else if (strcmp(argv[count], "--connection") == 0)
		{
			if (++count < argc)
				opts.connection = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--connections") == 0)
		{
			if (++count < argc)
			{
				opts.connection_count = 0;
				opts.connections = malloc(sizeof(char*) * 5);
				char* tok = strtok(argv[count], " ");
				while (tok)
				{
					opts.connections[opts.connection_count] = malloc(strlen(tok)+1);
					strcpy(opts.connections[opts.connection_count], tok);
					opts.connection_count++;
					tok = strtok(NULL, " ");
				}
			}
			else
				usage();
		}
		else if (strcmp(argv[count], "--control_connection") == 0)
		{
			if (++count < argc)
				opts.control_connection = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--clientid") == 0)
		{
			if (++count < argc)
				opts.clientid = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--username") == 0)
		{
			if (++count < argc)
				opts.username = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--password") == 0)
		{
			if (++count < argc)
				opts.password = argv[count];
			else
				usage();
		}
		else if (strcmp(argv[count], "--persistent") == 0)
			opts.persistence = 1;
		else if (strcmp(argv[count], "--verbose") == 0)
			opts.verbose = 1;
		count++;
	}
}

#define LOGA_DEBUG 0
#define LOGA_ALWAYS 1
#define LOGA_INFO 2
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

	if (LOGA_level == LOGA_DEBUG && opts.verbose == 0)
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
	return (res.tv_sec)*1000 + (res.tv_usec)/1000;
}
#endif

MQTTAsync control_client;
MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
MQTTAsync client;
int arrivedCount = 0;
int expectedCount = 0;
int measuring = 0;
long roundtrip_time = 0L;
int errors = 0;
int stopping = 0;
int connection_lost = 0; /* for use with the persistence option */
int recreated = 0;
int client_cleaned = 0;

char* wait_message = NULL;
char* wait_message2 = NULL;
int control_found = 0;
long last_completion_time = -1;
int test_count = 1000;

void control_connectionLost(void* context, char* cause)
{
	MyLog(LOGA_ALWAYS, "Control connection lost - stopping");

	stopping = 1;
}

/**-----------------------------------------------------------------------------
 * Callback which receives messages from the control connection
 * @param context
 * @param topicName the name of the topic on which the message is received
 * @param topicLen the length of the topic name (in case of embedded nulls)
 * @param m pointer to the message received
 * @return boolean
 */
int control_messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* m)
{
	MyLog(LOGA_ALWAYS, "Control message arrived: %.*s wait message: %s",
				m->payloadlen, m->payload, (wait_message == NULL) ? "None" : wait_message);
	if (strncmp(m->payload, "stop", 4) == 0)
	{
		MyLog(LOGA_ALWAYS, "Stop message arrived, stopping...");
		stopping = 1;
	}
	else if (wait_message != NULL && strncmp(wait_message, m->payload,
																					 strlen(wait_message)) == 0)
	{
		MyLog(LOGA_ALWAYS, "Wait message %s found", wait_message);
		control_found = 1;
		wait_message = NULL;
	}
	else if (wait_message2 != NULL && strncmp(wait_message2, m->payload,
																						strlen(wait_message2)) == 0)
	{
		MyLog(LOGA_ALWAYS, "Wait message2 %s found", wait_message);
		control_found = 2;
		wait_message2 = NULL;
	}

	MQTTAsync_free(topicName);
	MQTTAsync_freeMessage(&m);
	return 1;
}


int control_send(char* message)
{
	char buf[156];
	int rc = 0;
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;

	sprintf(buf, "%s: %s", opts.clientid, message);
	 MyLog(LOGA_ALWAYS, "Sending control message: %s", message);
	rc = MQTTAsync_send(control_client, pub_topic, (int)strlen(buf),
															buf, 1, 0, &ropts);
	MyLog(LOGA_DEBUG, "Control message sent: %s", buf);

	return rc;
}


/* wait for a specific message on the control topic. */
int control_wait(char* message)
{
	int count = 0;
	char buf[120];

	control_found = 0;
	wait_message = message;

	sprintf(buf, "waiting for: %s", message);
	control_send(buf);

	MyLog(LOGA_ALWAYS, "Waiting for: %s", message);
	while (control_found == 0 && stopping == 0)
	{
		if (++count == 300)
		{
			stopping = 1;
			MyLog(LOGA_ALWAYS, "Failed to receive message %s, stopping ", message);
			return 0; /* time out and tell the caller the message was not found */
		}
		MySleep(1000);
	}
	MyLog(LOGA_ALWAYS, "Control message found: %s, control_found %d", message, control_found);
	return control_found;
}


/* wait for a specific message on the control topic. */
int control_which(char* message1, char* message2)
{
	int count = 0;
	control_found = 0;
	wait_message = message1;
	wait_message2 = message2;

	while (control_found == 0)
	{
		if (++count == 300)
		  break; /* time out and tell the caller the message was not found */
		MySleep(1000);
	}
	return control_found;
}


START_TIME_TYPE global_start_time;

int messageArrived(void* context, char* topicName, int topicLen,
									 MQTTAsync_message* m)
{
	int seqno = -1;
	char* token = NULL;

	token = strtok(m->payload, " ");
	token = strtok(NULL, " ");
	token = strtok(NULL, " ");

	if (token)
		seqno = atoi(token);
	if (m->qos != opts.qos)
	{
		MyLog(LOGA_ALWAYS, "Error, expecting QoS %d but got %d", opts.qos,
				m->qos);
		errors++;
	} else if (seqno != arrivedCount + 1)
	{
		if (m->qos == 2 || (m->qos == 1 && seqno > arrivedCount + 1))
		{
			if (seqno == -1)
				MyLog(LOGA_ALWAYS,
					"Error, expecting sequence number %d but got message id %d, payload was %.*s",
							arrivedCount + 1, m->msgid, m->payloadlen, m->payload);
			else
				MyLog(LOGA_ALWAYS,
					"Error, expecting sequence number %d but got %d message id %d",
					arrivedCount + 1, seqno, m->msgid);
			errors++;
		}
	}
	arrivedCount++;
	MQTTAsync_free(topicName);
	MQTTAsync_freeMessage(&m);

	if (measuring && arrivedCount == test_count)
		roundtrip_time = elapsed(global_start_time);
	return 1;
}


void client_onReconnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_ALWAYS, "Successfully reconnected");
}


void client_onReconnectFailure(void* context, MQTTAsync_failureData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	int rc;

	MyLog(LOGA_ALWAYS, "Failed to reconnect with return code %d", (response) ? response->code : -9999);

	conn_opts.context = context;
	conn_opts.keepAliveInterval = 10;
	conn_opts.username = opts.username;
	conn_opts.password = opts.password;
	conn_opts.cleansession = 0;
	conn_opts.onSuccess = client_onReconnect;
	conn_opts.onFailure = client_onReconnectFailure;
	rc = MQTTAsync_connect(c, &conn_opts);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MyLog(LOGA_ALWAYS, "Failed to start reconnect with return code %d", rc);
		stopping = 1;
	}
}


void connectionLost(void* context, char* cause)
{
	MQTTAsync c = (MQTTAsync)context;
	int rc = 0;

	MyLog(LOGA_ALWAYS, "Connection lost when %d messages arrived out of %d expected",
			arrivedCount, expectedCount);
	//dotrace = 1;

	if (opts.persistence)
		connection_lost = 1;
	else
	{
		conn_opts.context = context;
		conn_opts.keepAliveInterval = 10;
		conn_opts.username = opts.username;
		conn_opts.password = opts.password;
		conn_opts.cleansession = 0;
		conn_opts.onSuccess = client_onReconnect;
		conn_opts.onFailure = client_onReconnectFailure;
		if (opts.connections)
		{
			conn_opts.serverURIcount = opts.connection_count;
			conn_opts.serverURIs = opts.connections;
		}
		else
		{
			conn_opts.serverURIcount = 0;
			conn_opts.serverURIs = NULL;
		}
		//printf("reconnecting to first serverURI %s\n", conn_opts.serverURIs[0]);
		MyLog(LOGA_ALWAYS, "Starting reconnect attempt");
		rc = MQTTAsync_connect(context, &conn_opts);
		if (rc != MQTTASYNC_SUCCESS)
		{
			MyLog(LOGA_ALWAYS, "Failed to start reconnect with return code %d", rc);
			stopping = 1;
		}
	}
}


int recreateReconnect(void)
{
	int rc;

	if (recreated == 0)
	{
		MyLog(LOGA_ALWAYS, "Recreating client");

		MQTTAsync_destroy(&client); /* destroy the client object so that we force persistence to be read on recreate */
#if !defined(_WINDOWS)
		/*heap_info* mqtt_mem = 0;
		mqtt_mem = Heap_get_info();
		MyLog(LOGA_INFO, "MQTT mem current %ld, max %ld",mqtt_mem->current_size,mqtt_mem->max_size);
		if (mqtt_mem->current_size > 20)
		  HeapScan(5); */
#endif
		rc = MQTTAsync_create(&client, opts.connection, opts.clientid, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
		if (rc != MQTTASYNC_SUCCESS)
		{
			MyLog(LOGA_ALWAYS, "MQTTAsync_create failed, rc %d", rc);
			goto exit;
		}

		if ((rc = MQTTAsync_setCallbacks(client, client, connectionLost,
														messageArrived, NULL)) != MQTTASYNC_SUCCESS)
		{
			MyLog(LOGA_ALWAYS, "MQTTAsync_setCallbacks failed, rc %d", rc);
			goto exit;
		}
		recreated = 1;
	}

	MyLog(LOGA_ALWAYS, "Reconnecting client");
	conn_opts.keepAliveInterval = 10;
	conn_opts.username = opts.username;
	conn_opts.password = opts.password;
	conn_opts.cleansession = 0;
	conn_opts.context = client;
	conn_opts.onSuccess = client_onReconnect;
	conn_opts.onFailure = client_onReconnectFailure;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
		MyLog(LOGA_ALWAYS, "MQTTAsync_connect failed, rc %d", rc);
	else
		connection_lost = 0;

exit:
	return rc;
}


int success(int count)
{
	int rc = 1;

	if (errors)
	{
		MyLog(LOGA_ALWAYS, "Workload test failed because the callback had errors");
		rc = 0;
	}
	if (arrivedCount != count)
	{
		if (opts.qos == 2 || (opts.qos == 1 && arrivedCount < count))
		{
			MyLog(LOGA_ALWAYS,
					"Workload test failed because the wrong number of messages"
					" was received: %d whereas %d were expected",
					arrivedCount, count);
			rc = 0;
		}
	}
	if (rc == 1)
		control_send("verdict: pass");
	else
		control_send("verdict: fail");
	return rc;
}


int waitForCompletion(START_TIME_TYPE start_time)
{
	int lastreport = 0;
	int wait_count = 0;
	int limit = 120;

	MyLog(LOGA_ALWAYS, "Wait for completion");
  if (opts.qos == 0)
	  limit = 30; /* we aren't going to get back all QoS 0 messages anyway */
	MySleep(1000);
	while (arrivedCount < expectedCount)
	{
		if (arrivedCount > lastreport)
		{
			MyLog(LOGA_ALWAYS, "%d messages arrived out of %d expected, in %d seconds",
					arrivedCount, expectedCount, elapsed(start_time) / 1000);
			lastreport = arrivedCount;
		}
		MySleep(1000);
		if (opts.persistence && connection_lost)
				recreateReconnect();
		if (++wait_count > limit || stopping)
			break;
	}
	last_completion_time = elapsed(start_time) / 1000;
	if (opts.qos > 0)
	{
	  MyLog(LOGA_ALWAYS, "Extra wait to see if any duplicates arrive");
	  MySleep(10000);            /* check if any duplicate messages arrive */
	}
	MyLog(LOGA_ALWAYS, "%d messages arrived out of %d expected, in %d seconds",
				arrivedCount, expectedCount, elapsed(start_time) / 1000);
	return success(expectedCount);
}

int messagesSent = 0;

void messageSent(void* context, MQTTAsync_successData* response)
{
	messagesSent++;
}


void one_iteration(void)
{
	int interval = 0;
	int i = 0;
	int seqno = 0;
	int rc = 0;
	START_TIME_TYPE start_time;
	int last_expected_count = expectedCount;
	int test_interval = 30;

	if (control_wait("start_measuring") == 0)
		goto exit;

	connection_lost = 0;
	recreated = 0;

	/* find the time for evaluation_count round-trip messages */
	MyLog(LOGA_INFO, "Evaluating how many messages needed");
	expectedCount = arrivedCount = 0;
	measuring = 1;
	global_start_time = start_clock();
	for (i = 1; i <= test_count; ++i)
	{
		char payload[128];

		sprintf(payload, "message number %d", i);

		rc = MQTTAsync_send(client, opts.topic, (int)(strlen(payload)+1), payload,
				        opts.qos, opts.retained, NULL);
		while (rc != MQTTASYNC_SUCCESS)
		{
			if (opts.persistence && (connection_lost || rc == MQTTASYNC_DISCONNECTED))
				recreateReconnect();
			if (stopping)
				goto exit;
			MySleep(1000);
			rc = MQTTAsync_send(client, opts.topic, (int)(strlen(payload)+1), payload,
					opts.qos, opts.retained, NULL);
			while (seqno - messagesSent > 2000)
				MySleep(1000);
		}
	}
	MyLog(LOGA_INFO, "Messages sent... waiting for echoes");
	while (arrivedCount < test_count)
	{
		if (opts.persistence && connection_lost)
			recreateReconnect();
		if (stopping)
			goto exit;
		MySleep(1000);
		MyLog(LOGA_ALWAYS, "arrivedCount %d", arrivedCount);
	}
	measuring = 0;

	/* Now set a target of 30 seconds total round trip */
	if (1) //last_completion_time == -1)
	{
		MyLog(LOGA_ALWAYS, "Round trip time for %d messages is %d ms", test_count, roundtrip_time);
		// test_count messages in 3039 ms: (test_interval * 1000) / roundtrip_time * test_count
		//expectedCount = 1000 * test_count * test_interval / roundtrip_time / 2;
		expectedCount = (test_interval * 1000) / roundtrip_time * test_count;
	}
	else
	{
		MyLog(LOGA_ALWAYS, "Last time, %d messages took %d s.", last_expected_count, last_completion_time);
		expectedCount = last_expected_count * test_interval / last_completion_time;
	}
	MyLog(LOGA_ALWAYS, "Therefore %d messages needed for 30 seconds", expectedCount);

	if (control_wait("start_test") == 0) /* now synchronize the test interval */
		goto exit;

	MyLog(LOGA_ALWAYS, "Starting 30 second test run with %d messages", expectedCount);
	arrivedCount = 0;
	messagesSent = 0;
	start_time = start_clock();
	while (seqno < expectedCount)
	{
		MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
		char payload[128];

		ropts.onSuccess = messageSent;
		seqno++;
		sprintf(payload, "message number %d", seqno);
		rc = MQTTAsync_send(client, opts.topic, (int)(strlen(payload)+1), payload,
				opts.qos, opts.retained, &ropts);
		while (rc != MQTTASYNC_SUCCESS)
		{
			MyLog(LOGA_INFO, "Rc %d from publish with payload %s, retrying", rc, payload);
			if (opts.persistence && (connection_lost || rc == MQTTASYNC_DISCONNECTED))
				recreateReconnect();
			if (stopping)
				goto exit;
			MySleep(1000);
			rc = MQTTAsync_send(client, opts.topic, (int)(strlen(payload)+1), payload,
				opts.qos, opts.retained, &ropts);
		}
		//MyLog(LOGA_DEBUG, "Successful publish with payload %s", payload);
		//while (seqno - messagesSent > 2000)
		//{
		//if (opts.persistence && (connection_lost || rc == MQTTASYNC_DISCONNECTED))
		//	recreateReconnect();
		//}
		//	MySleep(1000);
	}
	MyLog(LOGA_ALWAYS, "%d messages sent in %d seconds", expectedCount, elapsed(start_time) / 1000);

	waitForCompletion(start_time);
	control_wait("test finished");
exit:
	; /* dummy statement for target of exit */
}


static int client_subscribed = 0;

void client_onSubscribe(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In client subscribe onSuccess callback %p granted qos %d", c, response->alt.qos);

	client_subscribed = 1;
}

void client_onFailure(void* context, MQTTAsync_failureData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MyLog(LOGA_INFO, "In failure callback");

	client_subscribed = -1;
}


void client_onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
	int rc;

	sprintf(sub_topic, "%s/send", opts.control_topic);
	sprintf(pub_topic, "%s/receive", opts.control_topic);
	ropts.context = context;
	ropts.onSuccess = client_onSubscribe;
	ropts.onFailure = client_onFailure;
	ropts.context = c;
	if ((rc = MQTTAsync_subscribe(c, opts.topic, opts.qos, &ropts)) != MQTTASYNC_SUCCESS)
	{
		MyLog(LOGA_ALWAYS, "client MQTTAsync_subscribe failed, rc %d", rc);
		client_subscribed = -1;
	}
}


void client_onCleanedDisconnected(void* context, MQTTAsync_successData* response)
{
	client_cleaned = 1;
}


void client_onCleaned(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	dopts.context = context;
	dopts.onSuccess = client_onCleanedDisconnected;
	dopts.onFailure = client_onFailure;
	dopts.context = c;
	if ((rc = MQTTAsync_disconnect(c, &dopts)) != MQTTASYNC_SUCCESS)
	{
		MyLog(LOGA_ALWAYS, "client MQTTAsync_disconnect failed, rc %d", rc);
		stopping = 1;
	}
}


int sendAndReceive(void)
{
	int rc = 0;
	int persistence = MQTTCLIENT_PERSISTENCE_NONE;

	MyLog(LOGA_ALWAYS, "v3 async C client topic workload using QoS %d", opts.qos);
	MyLog(LOGA_DEBUG, "Connecting to %s", opts.connection);

	if (opts.persistence)
		persistence = MQTTCLIENT_PERSISTENCE_DEFAULT;

	rc = MQTTAsync_create(&client, opts.connection, opts.clientid, persistence, NULL);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MyLog(LOGA_ALWAYS, "MQTTAsync_create failed, rc %d", rc);
		rc = 99;
		goto exit;
	}

	if ((rc = MQTTAsync_setCallbacks(client, client, connectionLost,
                messageArrived, NULL)) != MQTTASYNC_SUCCESS)
	{
		MyLog(LOGA_ALWAYS, "MQTTAsync_setCallbacks failed, rc %d", rc);
		rc = 99;
		goto destroy_exit;
	}

	/* wait to know that the controlling process is running before connecting to the SUT */
	if (control_wait("who is ready?") == 0)
	{
		MyLog(LOGA_ALWAYS, "Wait for controller failed");
		goto exit;
	}

	/* connect cleansession, and then disconnect, to clean up */
	conn_opts.keepAliveInterval = 10;
	conn_opts.username = opts.username;
	conn_opts.password = opts.password;
	conn_opts.cleansession = 1;
	conn_opts.context = client;
	conn_opts.onSuccess = client_onCleaned;
	conn_opts.onFailure = client_onFailure;
	if (opts.connections)
	{
		conn_opts.serverURIcount = opts.connection_count;
		conn_opts.serverURIs = opts.connections;
	}
	else
	{
		conn_opts.serverURIcount = 0;
		conn_opts.serverURIs = NULL;
	}
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
	{
		MyLog(LOGA_ALWAYS, "MQTTAsync_connect failed, rc %d", rc);
		rc = 99;
		goto destroy_exit;
	}

	while (client_cleaned == 0)
		MySleep(1000);

	MyLog(LOGA_ALWAYS, "Client state cleaned up");

	conn_opts.cleansession = 0;
	conn_opts.context = client;
	conn_opts.onSuccess = client_onConnect;
	conn_opts.onFailure = client_onFailure;
	conn_opts.retryInterval = 1;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
	{
		MyLog(LOGA_ALWAYS, "MQTTAsync_connect failed, rc %d", rc);
		rc = 99;
		goto destroy_exit;
	}

	/* wait until subscribed */
	while (client_subscribed == 0)
		MySleep(1000);

	if (client_subscribed != 1)
		goto disconnect_exit;

	while (1)
	{
		control_send("Ready");
		if (control_which("who is ready?", "continue") == 2)
			break;
		control_send("Ready");
	}

	while (!stopping)
	{
		one_iteration();
	}

disconnect_exit:
	MQTTAsync_disconnect(client, 0);

destroy_exit:
 	MQTTAsync_destroy(&client);

exit:
	return rc;
}


static int control_subscribed = 0;

void control_onSubscribe(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	MyLog(LOGA_DEBUG, "In control subscribe onSuccess callback %p granted qos %d", c, response->alt.qos);

	control_subscribed = 1;
	MyLog(LOGA_ALWAYS, "Connected and subscribed to control connection");
}

void control_onFailure(void* context, MQTTAsync_failureData* response)
{
	MQTTAsync c = (MQTTAsync)context;

	control_subscribed = -1;
}


void control_onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync c = (MQTTAsync)context;
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
	int rc;

	sprintf(sub_topic, "%s/send", opts.control_topic);
	sprintf(pub_topic, "%s/receive", opts.control_topic);
	ropts.onSuccess = control_onSubscribe;
	ropts.onFailure = control_onFailure;
	ropts.context = c;
	MyLog(LOGA_ALWAYS, "Subscribing to control topic %s", sub_topic);
	if ((rc = MQTTAsync_subscribe(c, sub_topic, 2, &ropts)) != MQTTASYNC_SUCCESS)
	{
		MyLog(LOGA_ALWAYS, "control MQTTAsync_subscribe failed, rc %d", rc);
		control_subscribed = -1;
	}
}

void trace_callback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
	//if (level == MQTTASYNC_TRACE_ERROR || strstr(message, "Connect") || strstr(message, "failed"))
		printf("Trace : %d, %s\n", level, message);
}

int main(int argc, char** argv)
{
	MQTTAsync_connectOptions control_conn_opts = MQTTAsync_connectOptions_initializer;
	int rc = 0;
	static char topic_buf[200];
	static char clientid[40];

#if !defined(_WIN32)
	signal(SIGPIPE, SIG_IGN);
#endif

	MQTTAsync_nameValue* info = MQTTAsync_getVersionInfo();

	while (info->name)
	{
	  MyLog(LOGA_ALWAYS, "%s: %s\n", info->name, info->value);
	  info++;
	}

	getopts(argc, argv);

	sprintf(topic_buf, "%s_%d", opts.topic, opts.slot_no);
	opts.topic = topic_buf;

	sprintf(clientid, "%s_%d", opts.clientid, opts.slot_no);
	opts.clientid = clientid;

	MyLog(LOGA_ALWAYS, "Starting with clientid %s", opts.clientid);

	MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
	MQTTAsync_setTraceCallback(trace_callback);

	rc = MQTTAsync_create(&control_client, opts.control_connection,
	                                     opts.clientid, MQTTCLIENT_PERSISTENCE_NONE, NULL);
	if (rc != MQTTASYNC_SUCCESS)
	{
		MyLog(LOGA_ALWAYS, "control MQTTAsync_create failed, rc %d", rc);
		rc = 99;
		goto exit;
	}

	if ((rc = MQTTAsync_setCallbacks(control_client, control_client, control_connectionLost,
						control_messageArrived, NULL)) != MQTTASYNC_SUCCESS)
	{
		MyLog(LOGA_ALWAYS, "control MQTTAsync_setCallbacks failed, rc %d", rc);
		rc = 99;
		goto destroy_exit;
	}

	control_subscribed = 0;
	control_conn_opts.context = control_client;
	control_conn_opts.keepAliveInterval = 10;
	control_conn_opts.onSuccess = control_onConnect;
	control_conn_opts.onFailure = control_onFailure;
	if ((rc = MQTTAsync_connect(control_client, &control_conn_opts))
			!= MQTTASYNC_SUCCESS)
	{
		MyLog(LOGA_ALWAYS, "control MQTTAsync_connect failed, rc %d", rc);
		rc = 99;
		goto destroy_exit;
	}

	while (control_subscribed == 0)
		MySleep(1000);

	if (control_subscribed != 1)
		goto destroy_exit;

	sendAndReceive();

exit:
	MQTTAsync_disconnect(control_client, 0);

destroy_exit:
	MQTTAsync_destroy(&control_client);

	return 0;
}
