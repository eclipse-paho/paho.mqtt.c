/*******************************************************************************
 * Copyright (c) 2012, 2023 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution. 
 *
 * The Eclipse Public License is available at 
 *   https://www.eclipse.org/legal/epl-2.0/
 * and the Eclipse Distribution License is available at 
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial contribution
 *    Frank Pagliughi - loop to repeatedly read and sent time values.
 *******************************************************************************/

// This is a somewhat contrived example to show an application that publishes
// continuously, like a data acquisition app might do. In this case, though,
// we don't have a sensor to read, so we use the system time as the number
// of milliseconds since the epoch to simulate a data input.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "MQTTAsync.h"

#if !defined(_WIN32)
#include <unistd.h>
#else
#include <windows.h>
#include <Minwinbase.h>
#endif

#if defined(_WRS_KERNEL)
#include <OsWrapper.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#define snprintf _snprintf
#endif


// Better not to flood a public broker. Test against localhost.
#define ADDRESS         "mqtt://localhost:1883"

#define CLIENTID        "ExampleClientTimePub"
#define TOPIC           "data/time"
#define QOS             1
#define TIMEOUT         10000L
#define SAMPLE_PERIOD   10L    // in ms

volatile int finished = 0;
volatile int connected = 0;

void connlost(void *context, char *cause)
{
	MQTTAsync client = (MQTTAsync)context;
	MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
	int rc;

	printf("\nConnection lost\n");
	if (cause)
		printf("     cause: %s\n", cause);

	printf("Reconnecting\n");
	conn_opts.keepAliveInterval = 20;
	conn_opts.cleansession = 1;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start connect, return code %d\n", rc);
 		finished = 1;
	}
}

void onDisconnectFailure(void* context, MQTTAsync_failureData* response)
{
	printf("Disconnect failed\n");
	finished = 1;
}

void onDisconnect(void* context, MQTTAsync_successData* response)
{
	printf("Successful disconnection\n");
	finished = 1;
}

void onSendFailure(void* context, MQTTAsync_failureData* response)
{
	MQTTAsync client = (MQTTAsync)context;
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int rc;

	printf("Message send failed token %d error code %d\n", response->token, response->code);
	opts.onSuccess = onDisconnect;
	opts.onFailure = onDisconnectFailure;
	opts.context = client;
	if ((rc = MQTTAsync_disconnect(client, &opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start disconnect, return code %d\n", rc);
		exit(EXIT_FAILURE);
	}
}

void onSend(void* context, MQTTAsync_successData* response)
{
	// This gets called when a message is acknowledged successfully.
}


void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	printf("Connect failed, rc %d\n", response ? response->code : 0);
	finished = 1;
}


void onConnect(void* context, MQTTAsync_successData* response)
{
	printf("Successful connection\n");
	connected = 1;
}

int messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* m)
{
	/* not expecting any messages */
	return 1;
}

int64_t getTime(void)
{
	#if defined(_WIN32)
		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);
		return ((((int64_t) ft.dwHighDateTime) << 8) + ft.dwLowDateTime) / 10000;
	#else
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		return ((int64_t) ts.tv_sec * 1000) + ((int64_t) ts.tv_nsec / 1000000);
	#endif
}

int main(int argc, char* argv[])
{
	MQTTAsync client;
	MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;

	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTAsync_responseOptions pub_opts = MQTTAsync_responseOptions_initializer;

	int rc;

	if ((rc = MQTTAsync_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to create client object, return code %d\n", rc);
		exit(EXIT_FAILURE);
	}

	if ((rc = MQTTAsync_setCallbacks(client, client, connlost, messageArrived, NULL)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to set callback, return code %d\n", rc);
		exit(EXIT_FAILURE);
	}

	conn_opts.keepAliveInterval = 20;
	conn_opts.cleansession = 1;
	conn_opts.onSuccess = onConnect;
	conn_opts.onFailure = onConnectFailure;
	conn_opts.context = client;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start connect, return code %d\n", rc);
		exit(EXIT_FAILURE);
	}

	while (!connected) {
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(100000L);
		#endif
	}

	while (!finished) {
		int64_t t = getTime();

		char buf[256];
		int n = snprintf(buf, sizeof(buf), "%lld", (long long) t);
		printf("%s\n", buf);

		pub_opts.onSuccess = onSend;
		pub_opts.onFailure = onSendFailure;
		pub_opts.context = client;

		pubmsg.payload = buf;
		pubmsg.payloadlen = n;
		pubmsg.qos = QOS;
		pubmsg.retained = 0;

		if ((rc = MQTTAsync_sendMessage(client, TOPIC, &pubmsg, &pub_opts)) != MQTTASYNC_SUCCESS)
		{
			printf("Failed to start sendMessage, return code %d\n", rc);
			exit(EXIT_FAILURE);
		}

		#if defined(_WIN32)
			Sleep(SAMPLE_PERIOD);
		#else
			usleep(SAMPLE_PERIOD * 1000);
		#endif
	}

	MQTTAsync_destroy(&client);
 	return rc;
}

