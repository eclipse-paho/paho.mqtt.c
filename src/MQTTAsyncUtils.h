/*******************************************************************************
 * Copyright (c) 2009, 2022 IBM Corp. and others
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
 *    Ian Craggs - initial implementation and documentation
 *******************************************************************************/

#if !defined(MQTTASYNCUTILS_H_)
#define MQTTASYNCUTILS_H_

#include "MQTTPacket.h"
#include "Thread.h"

#define URI_TCP  "tcp://"
#define URI_MQTT "mqtt://"
#define URI_WS   "ws://"
#define URI_WSS  "wss://"

enum MQTTAsync_threadStates
{
	STOPPED, STARTING, RUNNING, STOPPING
};

typedef struct
{
	MQTTAsync_message* msg;
	char* topicName;
	int topicLen;
	unsigned int seqno; /* only used on restore */
} qEntry;

typedef struct
{
	int type;
	MQTTAsync_onSuccess* onSuccess;
	MQTTAsync_onFailure* onFailure;
	MQTTAsync_onSuccess5* onSuccess5;
	MQTTAsync_onFailure5* onFailure5;
	MQTTAsync_token token;
	void* context;
	START_TIME_TYPE start_time;
	MQTTProperties properties;
	union
	{
		struct
		{
			int count;
			char** topics;
			int* qoss;
			MQTTSubscribe_options opts;
			MQTTSubscribe_options* optlist;
		} sub;
		struct
		{
			int count;
			char** topics;
		} unsub;
		struct
		{
			char* destinationName;
			int payloadlen;
			void* payload;
			int qos;
			int retained;
		} pub;
		struct
		{
			int internal;
			int timeout;
			enum MQTTReasonCodes reasonCode;
		} dis;
		struct
		{
			int currentURI;
			int MQTTVersion; /**< current MQTT version being used to connect */
		} conn;
	} details;
} MQTTAsync_command;

typedef struct MQTTAsync_struct
{
	char* serverURI;
	int ssl;
	int websocket;
	Clients* c;

	/* "Global", to the client, callback definitions */
	MQTTAsync_connectionLost* cl;
	MQTTAsync_messageArrived* ma;
	MQTTAsync_deliveryComplete* dc;
    MQTTAsync_messageDeleted* md;
	void* clContext; /* the context to be associated with the conn lost callback*/
	void* maContext; /* the context to be associated with the msg arrived callback*/
	void* dcContext; /* the context to be associated with the deliv complete callback*/
    void* mdContext; /* the context to be associated with the message deleted callback*/

	MQTTAsync_connected* connected;
	void* connected_context; /* the context to be associated with the connected callback*/

	MQTTAsync_disconnected* disconnected;
	void* disconnected_context; /* the context to be associated with the disconnected callback*/

	MQTTAsync_updateConnectOptions* updateConnectOptions;
	void* updateConnectOptions_context;

	/* Each time connect is called, we store the options that were used.  These are reused in
	   any call to reconnect, or an automatic reconnect attempt */
	MQTTAsync_command connect;		/* Connect operation properties */
	MQTTAsync_command disconnect;		/* Disconnect operation properties */
	MQTTAsync_command* pending_write;       /* Is there a socket write pending? */

	List* responses;
	unsigned int command_seqno;

	MQTTPacket* pack;

	/* added for offline buffering */
	MQTTAsync_createOptions* createOptions;
	int shouldBeConnected;
	int noBufferedMessages; /* the current number of buffered (publish) messages for this client */

	/* added for automatic reconnect */
	int automaticReconnect;
	int minRetryInterval;
	int maxRetryInterval;
	int serverURIcount;
	char** serverURIs;
	int connectTimeout;

	int currentInterval;
	int currentIntervalBase;
	START_TIME_TYPE lastConnectionFailedTime;
	int retrying;
	int reconnectNow;

	/* MQTT V5 properties */
	MQTTProperties* connectProps;
	MQTTProperties* willProps;

} MQTTAsyncs;

typedef struct
{
	MQTTAsync_command command;
	MQTTAsyncs* client;
	unsigned int seqno; /* only used on restore */
	int not_restored;
	char* key; /* if not_restored, this holds the key */
} MQTTAsync_queuedCommand;

void MQTTAsync_lock_mutex(mutex_type amutex);
void MQTTAsync_unlock_mutex(mutex_type amutex);
void MQTTAsync_terminate(void);
#if !defined(NO_PERSISTENCE)
int MQTTAsync_restoreCommands(MQTTAsyncs* client);
#endif
int MQTTAsync_addCommand(MQTTAsync_queuedCommand* command, int command_size);
void MQTTAsync_emptyMessageQueue(Clients* client);
void MQTTAsync_freeResponses(MQTTAsyncs* m);
void MQTTAsync_freeCommands(MQTTAsyncs* m);
int MQTTAsync_unpersistCommandsAndMessages(Clients* c);
void MQTTAsync_closeSession(Clients* client, enum MQTTReasonCodes reasonCode, MQTTProperties* props);
int MQTTAsync_disconnect1(MQTTAsync handle, const MQTTAsync_disconnectOptions* options, int internal);
int MQTTAsync_assignMsgId(MQTTAsyncs* m);
int MQTTAsync_getNoBufferedMessages(MQTTAsyncs* m);
void MQTTAsync_writeContinue(SOCKET socket);
void MQTTAsync_writeComplete(SOCKET socket, int rc);
void setRetryLoopInterval(int keepalive);
void MQTTAsync_NULLPublishResponses(MQTTAsyncs* m);
void MQTTAsync_NULLPublishCommands(MQTTAsyncs* m);

#if defined(_WIN32) || defined(_WIN64)
#else
#define WINAPI
#endif

thread_return_type WINAPI MQTTAsync_sendThread(void* n);
thread_return_type WINAPI MQTTAsync_receiveThread(void* n);

#endif /* MQTTASYNCUTILS_H_ */
