/*******************************************************************************
 * Copyright (c) 2009, 2024 IBM Corp., Ian Craggs and others
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
 *    Ian Craggs, Allan Stockdill-Mander - SSL support
 *    Ian Craggs - multiple server connection support
 *    Ian Craggs - fix for bug 413429 - connectionLost not called
 *    Ian Craggs - fix for bug 415042 - using already freed structure
 *    Ian Craggs - fix for bug 419233 - mutexes not reporting errors
 *    Ian Craggs - fix for bug 420851
 *    Ian Craggs - fix for bug 432903 - queue persistence
 *    Ian Craggs - MQTT 3.1.1 support
 *    Rong Xiang, Ian Craggs - C++ compatibility
 *    Ian Craggs - fix for bug 442400: reconnecting after network cable unplugged
 *    Ian Craggs - fix for bug 444934 - incorrect free in freeCommand1
 *    Ian Craggs - fix for bug 445891 - assigning msgid is not thread safe
 *    Ian Craggs - fix for bug 465369 - longer latency than expected
 *    Ian Craggs - fix for bug 444103 - success/failure callbacks not invoked
 *    Ian Craggs - fix for bug 484363 - segfault in getReadySocket
 *    Ian Craggs - automatic reconnect and offline buffering (send while disconnected)
 *    Ian Craggs - fix for bug 472250
 *    Ian Craggs - fix for bug 486548
 *    Ian Craggs - SNI support
 *    Ian Craggs - auto reconnect timing fix #218
 *    Ian Craggs - fix for issue #190
 *    Ian Craggs - check for NULL SSL options #334
 *    Ian Craggs - allocate username/password buffers #431
 *    Ian Craggs - MQTT 5.0 support
 *    Ian Craggs - refactor to reduce module size
 *******************************************************************************/

#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32) && !defined(_WIN64)
	#include <sys/time.h>
#else
	#if defined(_MSC_VER) && _MSC_VER < 1900
		#define snprintf _snprintf
	#endif
#endif

#if !defined(NO_PERSISTENCE)
#include "MQTTPersistence.h"
#endif
#include "MQTTAsync.h"
#include "MQTTAsyncUtils.h"
#include "utf-8.h"
#include "MQTTProtocol.h"
#include "MQTTProtocolOut.h"
#include "Thread.h"
#include "SocketBuffer.h"
#include "StackTrace.h"
#include "Heap.h"
#include "OsWrapper.h"
#include "WebSocket.h"

static void MQTTAsync_freeServerURIs(MQTTAsyncs* m);

#include "VersionInfo.h"

const char *client_timestamp_eye = "MQTTAsyncV3_Timestamp " BUILD_TIMESTAMP;
const char *client_version_eye = "MQTTAsyncV3_Version " CLIENT_VERSION;

volatile int global_initialized = 0;
List* MQTTAsync_handles = NULL;
List* MQTTAsync_commands = NULL;
int MQTTAsync_tostop = 0;

static ClientStates ClientState =
{
	CLIENT_VERSION, /* version */
	NULL /* client list */
};

MQTTProtocol state;
ClientStates* bstate = &ClientState;

enum MQTTAsync_threadStates sendThread_state = STOPPED;
enum MQTTAsync_threadStates receiveThread_state = STOPPED;
thread_id_type sendThread_id = 0,
               receiveThread_id = 0;

// global objects init declaration
int MQTTAsync_init(void);

void MQTTAsync_global_init(MQTTAsync_init_options* inits)
{
	MQTTAsync_init();
#if defined(OPENSSL)
	SSLSocket_handleOpensslInit(inits->do_openssl_init);
#endif
}

#if !defined(min)
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#if defined(WIN32) || defined(WIN64)
void MQTTAsync_init_rand(void)
{
	START_TIME_TYPE now = MQTTTime_start_clock();
	srand((unsigned int)now);
}
#elif defined(AIX)
void MQTTAsync_init_rand(void)
{
	START_TIME_TYPE now = MQTTTime_start_clock();
	srand(now.tv_nsec);
}
#else
void MQTTAsync_init_rand(void)
{
	START_TIME_TYPE now = MQTTTime_start_clock();
	srand(now.tv_usec);
}
#endif

#if defined(_WIN32) || defined(_WIN64)
mutex_type mqttasync_mutex = NULL;
mutex_type socket_mutex = NULL;
mutex_type mqttcommand_mutex = NULL;
sem_type send_sem = NULL;
#if !defined(NO_HEAP_TRACKING)
extern mutex_type stack_mutex;
extern mutex_type heap_mutex;
#endif
extern mutex_type log_mutex;

int MQTTAsync_init(void)
{
	DWORD rc = 0;

	if (mqttasync_mutex == NULL)
	{
		if ((mqttasync_mutex = CreateMutex(NULL, 0, NULL)) == NULL)
		{
			rc = GetLastError();
			printf("mqttasync_mutex error %d\n", rc);
			goto exit;
		}
		if ((mqttcommand_mutex = CreateMutex(NULL, 0, NULL)) == NULL)
		{
			rc = GetLastError();
			printf("mqttcommand_mutex error %d\n", rc);
			goto exit;
		}
		if ((send_sem = CreateEvent(
				NULL,               /* default security attributes */
				FALSE,              /* manual-reset event? */
				FALSE,              /* initial state is nonsignaled */
				NULL                /* object name */
				)) == NULL)
		{
			rc = GetLastError();
			printf("send_sem error %d\n", rc);
			goto exit;
		}
#if !defined(NO_HEAP_TRACKING)
		if ((stack_mutex = CreateMutex(NULL, 0, NULL)) == NULL)
		{
			rc = GetLastError();
			printf("stack_mutex error %d\n", rc);
			goto exit;
		}
		if ((heap_mutex = CreateMutex(NULL, 0, NULL)) == NULL)
		{
			rc = GetLastError();
			printf("heap_mutex error %d\n", rc);
			goto exit;
		}
#endif
		if ((log_mutex = CreateMutex(NULL, 0, NULL)) == NULL)
		{
			rc = GetLastError();
			printf("log_mutex error %d\n", rc);
			goto exit;
		}
		if ((socket_mutex = CreateMutex(NULL, 0, NULL)) == NULL)
		{
			rc = GetLastError();
			printf("socket_mutex error %d\n", rc);
			goto exit;
		}
	}
	else
	{
		Log(TRACE_MAX, -1, "Library already initialized");
	}
exit:
	return rc;
}

void MQTTAsync_cleanup(void)
{
	if (send_sem)
		CloseHandle(send_sem);
#if !defined(NO_HEAP_TRACKING)
	if (stack_mutex)
		CloseHandle(stack_mutex);
	if (heap_mutex)
		CloseHandle(heap_mutex);
#endif
	if (log_mutex)
		CloseHandle(log_mutex);
	if (socket_mutex)
		CloseHandle(socket_mutex);
	if (mqttasync_mutex)
		CloseHandle(mqttasync_mutex);
}

#if defined(PAHO_MQTT_STATIC)
static INIT_ONCE g_InitOnce = INIT_ONCE_STATIC_INIT; /* Global for one time initialization */

/* This runs at most once */
BOOL CALLBACK InitMutexesOnce (
    PINIT_ONCE InitOnce,        /* Pointer to one-time initialization structure */
    PVOID Parameter,            /* Optional parameter */
    PVOID *lpContext)           /* Return data, if any */
{
	int rc = MQTTAsync_init();
    return rc == 0;
}
#else
BOOL APIENTRY DllMain(HANDLE hModule,
					  DWORD  ul_reason_for_call,
					  LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
		case DLL_PROCESS_ATTACH:
			MQTTAsync_init();
			break;
		case DLL_THREAD_ATTACH:
			break;
		case DLL_THREAD_DETACH:
			break;
		case DLL_PROCESS_DETACH:
			if (lpReserved)
				MQTTAsync_cleanup();
		break;
	}
	return TRUE;
}
#endif


#else
static pthread_mutex_t mqttasync_mutex_store = PTHREAD_MUTEX_INITIALIZER;
mutex_type mqttasync_mutex = &mqttasync_mutex_store;

static pthread_mutex_t socket_mutex_store = PTHREAD_MUTEX_INITIALIZER;
mutex_type socket_mutex = &socket_mutex_store;

static pthread_mutex_t mqttcommand_mutex_store = PTHREAD_MUTEX_INITIALIZER;
mutex_type mqttcommand_mutex = &mqttcommand_mutex_store;

static cond_type_struct send_cond_store = { PTHREAD_COND_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };
cond_type send_cond = &send_cond_store;

int MQTTAsync_init(void)
{
	pthread_mutexattr_t attr;
	int rc;

	pthread_mutexattr_init(&attr);
#if !defined(_WRS_KERNEL)
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
#else
	/* #warning "no pthread_mutexattr_settype" */
#endif
	if ((rc = pthread_mutex_init(mqttasync_mutex, &attr)) != 0)
		printf("MQTTAsync: error %d initializing async_mutex\n", rc);
	else if ((rc = pthread_mutex_init(mqttcommand_mutex, &attr)) != 0)
		printf("MQTTAsync: error %d initializing command_mutex\n", rc);
	else if ((rc = pthread_mutex_init(socket_mutex, &attr)) != 0)
		printf("MQTTClient: error %d initializing socket_mutex\n", rc);
	else if ((rc = pthread_cond_init(&send_cond->cond, NULL)) != 0)
		printf("MQTTAsync: error %d initializing send_cond cond\n", rc);
	else if ((rc = pthread_mutex_init(&send_cond->mutex, &attr)) != 0)
		printf("MQTTAsync: error %d initializing send_cond mutex\n", rc);

	return rc;
}
#endif


int MQTTAsync_createWithOptions(MQTTAsync* handle, const char* serverURI, const char* clientId,
		int persistence_type, void* persistence_context,  MQTTAsync_createOptions* options)
{
	int rc = 0;
	MQTTAsyncs *m = NULL;

#if (defined(_WIN32) || defined(_WIN64)) && defined(PAHO_MQTT_STATIC)
	 /* intializes mutexes once.  Must come before FUNC_ENTRY */
	BOOL bStatus = InitOnceExecuteOnce(&g_InitOnce, InitMutexesOnce, NULL, NULL);
#endif
	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (serverURI == NULL || clientId == NULL)
	{
		rc = MQTTASYNC_NULL_PARAMETER;
		goto exit;
	}

	if (!UTF8_validateString(clientId))
	{
		rc = MQTTASYNC_BAD_UTF8_STRING;
		goto exit;
	}

	if (strlen(clientId) == 0 && persistence_type == MQTTCLIENT_PERSISTENCE_DEFAULT)
	{
		rc = MQTTASYNC_PERSISTENCE_ERROR;
		goto exit;
	}

	if (strstr(serverURI, "://") != NULL)
	{
		if (strncmp(URI_TCP, serverURI, strlen(URI_TCP)) != 0
		 && strncmp(URI_MQTT, serverURI, strlen(URI_MQTT)) != 0
		 && strncmp(URI_WS, serverURI, strlen(URI_WS)) != 0
#if defined(OPENSSL)
		 && strncmp(URI_SSL, serverURI, strlen(URI_SSL)) != 0
		 && strncmp(URI_MQTTS, serverURI, strlen(URI_MQTTS)) != 0
		 && strncmp(URI_WSS, serverURI, strlen(URI_WSS)) != 0
#endif
			)
		{
			rc = MQTTASYNC_BAD_PROTOCOL;
			goto exit;
		}
	}

	if (options && options->maxBufferedMessages <= 0)
	{
		rc = MQTTASYNC_MAX_BUFFERED;
		goto exit;
	}

	if (options && (strncmp(options->struct_id, "MQCO", 4) != 0 ||
					options->struct_version < 0 || options->struct_version > 2))
	{
		rc = MQTTASYNC_BAD_STRUCTURE;
		goto exit;
	}

	if (!global_initialized)
	{
		#if !defined(NO_HEAP_TRACKING)
			Heap_initialize();
		#endif
		Log_initialize((Log_nameValue*)MQTTAsync_getVersionInfo());
		bstate->clients = ListInitialize();
		Socket_outInitialize();
		Socket_setWriteContinueCallback(MQTTAsync_writeContinue);
		Socket_setWriteCompleteCallback(MQTTAsync_writeComplete);
		Socket_setWriteAvailableCallback(MQTTProtocol_writeAvailable);
		MQTTAsync_handles = ListInitialize();
		MQTTAsync_commands = ListInitialize();
#if defined(OPENSSL)
		SSLSocket_initialize();
#endif
		global_initialized = 1;
	}
	if ((m = malloc(sizeof(MQTTAsyncs))) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	*handle = m;
	memset(m, '\0', sizeof(MQTTAsyncs));
	if (strncmp(URI_TCP, serverURI, strlen(URI_TCP)) == 0)
		serverURI += strlen(URI_TCP);
	else if (strncmp(URI_MQTT, serverURI, strlen(URI_MQTT)) == 0)
		serverURI += strlen(URI_MQTT);
	else if (strncmp(URI_WS, serverURI, strlen(URI_WS)) == 0)
	{
		serverURI += strlen(URI_WS);
		m->websocket = 1;
	}
#if defined(OPENSSL)
	else if (strncmp(URI_SSL, serverURI, strlen(URI_SSL)) == 0)
	{
		serverURI += strlen(URI_SSL);
		m->ssl = 1;
	}
	else if (strncmp(URI_MQTTS, serverURI, strlen(URI_MQTTS)) == 0)
	{
		serverURI += strlen(URI_MQTTS);
		m->ssl = 1;
	}
	else if (strncmp(URI_WSS, serverURI, strlen(URI_WSS)) == 0)
	{
		serverURI += strlen(URI_WSS);
		m->ssl = 1;
		m->websocket = 1;
	}
#endif
	if ((m->serverURI = MQTTStrdup(serverURI)) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	m->responses = ListInitialize();
	ListAppend(MQTTAsync_handles, m, sizeof(MQTTAsyncs));

	if ((m->c = malloc(sizeof(Clients))) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	memset(m->c, '\0', sizeof(Clients));
	m->c->context = m;
	m->c->outboundMsgs = ListInitialize();
	m->c->inboundMsgs = ListInitialize();
	m->c->messageQueue = ListInitialize();
	m->c->outboundQueue = ListInitialize();
	m->c->clientID = MQTTStrdup(clientId);
	if (m->c->context == NULL || m->c->outboundMsgs == NULL || m->c->inboundMsgs == NULL ||
			m->c->messageQueue == NULL || m->c->outboundQueue == NULL || m->c->clientID == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	m->c->MQTTVersion = MQTTVERSION_DEFAULT;

	m->shouldBeConnected = 0;
	if (options)
	{
		if ((m->createOptions = malloc(sizeof(MQTTAsync_createOptions))) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}
		memcpy(m->createOptions, options, sizeof(MQTTAsync_createOptions));
		if (options->struct_version > 0)
			m->c->MQTTVersion = options->MQTTVersion;
	}

#if !defined(NO_PERSISTENCE)
	rc = MQTTPersistence_create(&(m->c->persistence), persistence_type, persistence_context);
	if (rc == 0)
	{
		rc = MQTTPersistence_initialize(m->c, m->serverURI); /* inflight messages restored here */
		if (rc == 0)
		{
			if (m->createOptions && m->createOptions->struct_version >= 2 && m->createOptions->restoreMessages == 0)
				MQTTAsync_unpersistCommandsAndMessages(m->c);
			else
			{
				MQTTAsync_restoreCommands(m);
				MQTTPersistence_restoreMessageQueue(m->c);
			}
		}
	}
#endif
	ListAppend(bstate->clients, m->c, sizeof(Clients) + 3*sizeof(List));

exit:
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_create(MQTTAsync* handle, const char* serverURI, const char* clientId,
		int persistence_type, void* persistence_context)
{
	MQTTAsync_init_rand();

	return MQTTAsync_createWithOptions(handle, serverURI, clientId, persistence_type,
		persistence_context, NULL);
}


void MQTTAsync_destroy(MQTTAsync* handle)
{
	MQTTAsyncs* m = *handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL)
		goto exit;

	MQTTAsync_closeSession(m->c, MQTTREASONCODE_SUCCESS, NULL);

	MQTTAsync_NULLPublishResponses(m);
	MQTTAsync_freeResponses(m);
	MQTTAsync_freeCommands(m);
	ListFree(m->responses);

	if (m->c)
	{
		SOCKET saved_socket = m->c->net.socket;
		char* saved_clientid = MQTTStrdup(m->c->clientID);
#if !defined(NO_PERSISTENCE)
		MQTTPersistence_close(m->c);
#endif
		MQTTAsync_emptyMessageQueue(m->c);
		MQTTProtocol_freeClient(m->c);
		if (!ListRemove(bstate->clients, m->c))
			Log(LOG_ERROR, 0, NULL);
		else
			Log(TRACE_MIN, 1, NULL, saved_clientid, saved_socket);
		free(saved_clientid);
	}

	if (m->serverURI)
		free(m->serverURI);
	if (m->createOptions)
		free(m->createOptions);
	MQTTAsync_freeServerURIs(m);
	if (m->connectProps)
	{
		MQTTProperties_free(m->connectProps);
		free(m->connectProps);
		m->connectProps = NULL;
	}
	if (m->willProps)
	{
		MQTTProperties_free(m->willProps);
		free(m->willProps);
		m->willProps = NULL;
	}
	if (!ListRemove(MQTTAsync_handles, m))
		Log(LOG_ERROR, -1, "free error");
	*handle = NULL;
	if (bstate->clients->count == 0)
		MQTTAsync_terminate();

exit:
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT;
}


int MQTTAsync_connect(MQTTAsync handle, const MQTTAsync_connectOptions* options)
{
	MQTTAsyncs* m = handle;
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsync_queuedCommand* conn;
	thread_id_type thread_id = 0;
	int locked = 0;

	FUNC_ENTRY;
	if (options == NULL)
	{
		rc = MQTTASYNC_NULL_PARAMETER;
		goto exit;
	}

	if (strncmp(options->struct_id, "MQTC", 4) != 0 || options->struct_version < 0 || options->struct_version > 8)
	{
		rc = MQTTASYNC_BAD_STRUCTURE;
		goto exit;
	}

#if defined(OPENSSL)
	if (m->ssl && options->ssl == NULL)
	{
		rc = MQTTASYNC_NULL_PARAMETER;
		goto exit;
	}
#endif

	if (options->will) /* check validity of will options structure */
	{
		if (strncmp(options->will->struct_id, "MQTW", 4) != 0 || (options->will->struct_version != 0 && options->will->struct_version != 1))
		{
			rc = MQTTASYNC_BAD_STRUCTURE;
			goto exit;
		}
		if (options->will->qos < 0 || options->will->qos > 2)
		{
			rc = MQTTASYNC_BAD_QOS;
			goto exit;
		}
		if (options->will->topicName == NULL)
		{
			rc = MQTTASYNC_NULL_PARAMETER;
			goto exit;
		} else if (strlen(options->will->topicName) == 0)
		{
			rc = MQTTASYNC_0_LEN_WILL_TOPIC;
			goto exit;
		}
	}
	if (options->struct_version != 0 && options->ssl) /* check validity of SSL options structure */
	{
		if (strncmp(options->ssl->struct_id, "MQTS", 4) != 0 || options->ssl->struct_version < 0 || options->ssl->struct_version > 6)
		{
			rc = MQTTASYNC_BAD_STRUCTURE;
			goto exit;
		}
	}
	if (options->MQTTVersion >= MQTTVERSION_5 && m->c->MQTTVersion < MQTTVERSION_5)
	{
		rc = MQTTASYNC_WRONG_MQTT_VERSION;
		goto exit;
	}
	if ((options->username && !UTF8_validateString(options->username)) ||
		(options->password && !UTF8_validateString(options->password)))
	{
		rc = MQTTASYNC_BAD_UTF8_STRING;
		goto exit;
	}
	if (options->MQTTVersion >= MQTTVERSION_5 && options->struct_version < 6)
	{
		rc = MQTTASYNC_BAD_STRUCTURE;
		goto exit;
	}
	if (options->MQTTVersion >= MQTTVERSION_5 && options->cleansession != 0)
	{
		rc = MQTTASYNC_BAD_MQTT_OPTION;
		goto exit;
	}
	if (options->MQTTVersion < MQTTVERSION_5 && options->struct_version >= 6)
	{
		if (options->cleanstart != 0 || options->onFailure5 || options->onSuccess5 ||
				options->connectProperties || options->willProperties)
		{
			rc = MQTTASYNC_BAD_MQTT_OPTION;
			goto exit;
		}
	}

	m->connect.onSuccess = options->onSuccess;
	m->connect.onFailure = options->onFailure;
	if (options->struct_version >= 6)
	{
		m->connect.onSuccess5 = options->onSuccess5;
		m->connect.onFailure5 = options->onFailure5;
	}
	m->connect.context = options->context;
	m->connectTimeout = options->connectTimeout;

	/* don't lock async mutex if we are being called from a callback */
	thread_id = Paho_thread_getid();
	if (thread_id != sendThread_id && thread_id != receiveThread_id)
	{
		MQTTAsync_lock_mutex(mqttasync_mutex);
		locked = 1;
	}
	MQTTAsync_tostop = 0;
	if (sendThread_state != STARTING && sendThread_state != RUNNING)
	{
		sendThread_state = STARTING;
		Paho_thread_start(MQTTAsync_sendThread, NULL);
	}
	if (receiveThread_state != STARTING && receiveThread_state != RUNNING)
	{
		receiveThread_state = STARTING;
		Paho_thread_start(MQTTAsync_receiveThread, handle);
	}
	if (locked)
		MQTTAsync_unlock_mutex(mqttasync_mutex);

	m->c->keepAliveInterval = m->c->savedKeepAliveInterval = options->keepAliveInterval;
	setRetryLoopInterval(options->keepAliveInterval);
	m->c->cleansession = options->cleansession;
	m->c->maxInflightMessages = options->maxInflight;
	if (options->struct_version >= 3)
		m->c->MQTTVersion = options->MQTTVersion;
	else
		m->c->MQTTVersion = MQTTVERSION_DEFAULT;
	if (options->struct_version >= 4)
	{
		m->automaticReconnect = options->automaticReconnect;
		m->minRetryInterval = options->minRetryInterval;
		m->maxRetryInterval = options->maxRetryInterval;
	}
	if (options->struct_version >= 7)
	{
		m->c->net.httpHeaders = (const MQTTClient_nameValue *) options->httpHeaders;
	}
	if (options->struct_version >= 8)
	{
		if (options->httpProxy)
			m->c->httpProxy = MQTTStrdup(options->httpProxy);
		if (options->httpsProxy)
			m->c->httpsProxy = MQTTStrdup(options->httpsProxy);
	}

	if (m->c->will)
	{
		free(m->c->will->payload);
		free(m->c->will->topic);
		free(m->c->will);
		m->c->will = NULL;
	}

	if (options->will && (options->will->struct_version == 0 || options->will->struct_version == 1))
	{
		const void* source = NULL;

		if ((m->c->will = malloc(sizeof(willMessages))) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}
		if (options->will->message || (options->will->struct_version == 1 && options->will->payload.data))
		{
			if (options->will->struct_version == 1 && options->will->payload.data)
			{
				m->c->will->payloadlen = options->will->payload.len;
				source = options->will->payload.data;
			}
			else
			{
				m->c->will->payloadlen = (int)strlen(options->will->message);
				source = (void*)options->will->message;
			}
			if ((m->c->will->payload = malloc(m->c->will->payloadlen)) == NULL)
			{
				rc = PAHO_MEMORY_ERROR;
				goto exit;
			}
			memcpy(m->c->will->payload, source, m->c->will->payloadlen);
		}
		else
		{
			m->c->will->payload = NULL;
			m->c->will->payloadlen = 0;
		}
		m->c->will->qos = options->will->qos;
		m->c->will->retained = options->will->retained;
		m->c->will->topic = MQTTStrdup(options->will->topicName);
	}

#if defined(OPENSSL)
	if (m->c->sslopts)
	{
		if (m->c->sslopts->trustStore)
			free((void*)m->c->sslopts->trustStore);
		if (m->c->sslopts->keyStore)
			free((void*)m->c->sslopts->keyStore);
		if (m->c->sslopts->privateKey)
			free((void*)m->c->sslopts->privateKey);
		if (m->c->sslopts->privateKeyPassword)
			free((void*)m->c->sslopts->privateKeyPassword);
		if (m->c->sslopts->enabledCipherSuites)
			free((void*)m->c->sslopts->enabledCipherSuites);
		if (m->c->sslopts->struct_version >= 2)
		{
			if (m->c->sslopts->CApath)
				free((void*)m->c->sslopts->CApath);
		}
		if (m->c->sslopts->struct_version >= 6)
		{
			if (m->c->sslopts->providerName)
				free((void*)m->c->sslopts->providerName);
		}
		free((void*)m->c->sslopts);
		m->c->sslopts = NULL;
	}

	if (options->struct_version != 0 && options->ssl)
	{
		if ((m->c->sslopts = malloc(sizeof(MQTTClient_SSLOptions))) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}
		memset(m->c->sslopts, '\0', sizeof(MQTTClient_SSLOptions));
		m->c->sslopts->struct_version = options->ssl->struct_version;
		if (options->ssl->trustStore)
			m->c->sslopts->trustStore = MQTTStrdup(options->ssl->trustStore);
		if (options->ssl->keyStore)
			m->c->sslopts->keyStore = MQTTStrdup(options->ssl->keyStore);
		if (options->ssl->privateKey)
			m->c->sslopts->privateKey = MQTTStrdup(options->ssl->privateKey);
		if (options->ssl->privateKeyPassword)
			m->c->sslopts->privateKeyPassword = MQTTStrdup(options->ssl->privateKeyPassword);
		if (options->ssl->enabledCipherSuites)
			m->c->sslopts->enabledCipherSuites = MQTTStrdup(options->ssl->enabledCipherSuites);
		m->c->sslopts->enableServerCertAuth = options->ssl->enableServerCertAuth;
		if (m->c->sslopts->struct_version >= 1)
			m->c->sslopts->sslVersion = options->ssl->sslVersion;
		if (m->c->sslopts->struct_version >= 2)
		{
			m->c->sslopts->verify = options->ssl->verify;
			if (options->ssl->CApath)
				m->c->sslopts->CApath = MQTTStrdup(options->ssl->CApath);
		}
		if (m->c->sslopts->struct_version >= 3)
		{
			m->c->sslopts->ssl_error_cb = options->ssl->ssl_error_cb;
			m->c->sslopts->ssl_error_context = options->ssl->ssl_error_context;
		}
		if (m->c->sslopts->struct_version >= 4)
		{
			m->c->sslopts->ssl_psk_cb = options->ssl->ssl_psk_cb;
			m->c->sslopts->ssl_psk_context = options->ssl->ssl_psk_context;
			m->c->sslopts->disableDefaultTrustStore = options->ssl->disableDefaultTrustStore;
		}
		if (m->c->sslopts->struct_version >= 5)
		{
			if (options->ssl->protos)
				m->c->sslopts->protos = (const unsigned char*)MQTTStrdup((const char*)options->ssl->protos);
			m->c->sslopts->protos_len = options->ssl->protos_len;
		}
		if (m->c->sslopts->struct_version >= 6)
		{
			if (options->ssl->providerName) {
# if OPENSSL_PROVIDERS
				m->c->sslopts->providerName = MQTTStrdup(options->ssl->providerName);
# else // OPENSSL_PROVIDERS
				rc = MQTTASYNC_SSL_NOT_SUPPORTED;
				goto exit;
# endif // OPENSSL_PROVIDERS
			}
		}
	}
#else
	if (options->struct_version != 0 && options->ssl)
	{
		rc = MQTTASYNC_SSL_NOT_SUPPORTED;
		goto exit;
	}
#endif

	if (m->c->username)
	{
		free((void*)m->c->username);
		m->c->username = NULL;
	}
	if (options->username)
		m->c->username = MQTTStrdup(options->username);
	if (m->c->password)
	{
		free((void*)m->c->password);
		m->c->password = NULL;
	}
	if (options->password)
	{
		m->c->password = MQTTStrdup(options->password);
		m->c->passwordlen = (int)strlen(options->password);
	}
	else if (options->struct_version >= 5 && options->binarypwd.data)
	{
		m->c->passwordlen = options->binarypwd.len;
		if ((m->c->password = malloc(m->c->passwordlen)) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}
		memcpy((void*)m->c->password, options->binarypwd.data, m->c->passwordlen);
	}

	m->c->retryInterval = options->retryInterval;
	m->shouldBeConnected = 1;

	m->connectTimeout = options->connectTimeout;

	MQTTAsync_freeServerURIs(m);
	if (options->struct_version >= 2 && options->serverURIcount > 0)
	{
		int i;

		m->serverURIcount = options->serverURIcount;
		if ((m->serverURIs = malloc(options->serverURIcount * sizeof(char*))) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}
		for (i = 0; i < options->serverURIcount; ++i)
			m->serverURIs[i] = MQTTStrdup(options->serverURIs[i]);
	}

	if (m->connectProps)
	{
		MQTTProperties_free(m->connectProps);
		free(m->connectProps);
		m->connectProps = NULL;
	}
	if (m->willProps)
	{
		MQTTProperties_free(m->willProps);
		free(m->willProps);
		m->willProps = NULL;
	}
	if (options->struct_version >=6)
	{
		if (options->connectProperties)
		{
			MQTTProperties initialized = MQTTProperties_initializer;

			if ((m->connectProps = malloc(sizeof(MQTTProperties))) == NULL)
			{
				rc = PAHO_MEMORY_ERROR;
				goto exit;
			}
			*m->connectProps = initialized;
			*m->connectProps = MQTTProperties_copy(options->connectProperties);

			if (MQTTProperties_hasProperty(options->connectProperties, MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL))
				m->c->sessionExpiry = MQTTProperties_getNumericValue(options->connectProperties,
						MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL);

		}
		if (options->willProperties)
		{
			MQTTProperties initialized = MQTTProperties_initializer;

			if ((m->willProps = malloc(sizeof(MQTTProperties))) == NULL)
			{
				rc = PAHO_MEMORY_ERROR;
				goto exit;
			}
			*m->willProps = initialized;
			*m->willProps = MQTTProperties_copy(options->willProperties);
		}
		m->c->cleanstart = options->cleanstart;
	}

	/* Add connect request to operation queue */
	if ((conn = malloc(sizeof(MQTTAsync_queuedCommand))) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	memset(conn, '\0', sizeof(MQTTAsync_queuedCommand));
	conn->client = m;
	if (options)
	{
		conn->command.onSuccess = options->onSuccess;
		conn->command.onFailure = options->onFailure;
		conn->command.onSuccess5 = options->onSuccess5;
		conn->command.onFailure5 = options->onFailure5;
		conn->command.context = options->context;
	}
	conn->command.type = CONNECT;
	conn->command.details.conn.currentURI = 0;
	rc = MQTTAsync_addCommand(conn, sizeof(conn));

exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_reconnect(MQTTAsync handle)
{
	int rc = MQTTASYNC_FAILURE;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m->automaticReconnect)
	{
		if (m->shouldBeConnected)
		{
			m->reconnectNow = 1;
			m->currentIntervalBase = m->minRetryInterval;
			m->currentInterval = m->minRetryInterval;
			m->retrying = 1;
			rc = MQTTASYNC_SUCCESS;
		}
	}
	else
	{
		/* to reconnect, put the connect command to the head of the command queue */
		MQTTAsync_queuedCommand* conn = malloc(sizeof(MQTTAsync_queuedCommand));
		if (!conn)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}
		memset(conn, '\0', sizeof(MQTTAsync_queuedCommand));
		conn->client = m;
		conn->command = m->connect;
		/* make sure that the version attempts are restarted */
		if (m->c->MQTTVersion == MQTTVERSION_DEFAULT)
			conn->command.details.conn.MQTTVersion = 0;
		rc = MQTTAsync_addCommand(conn, sizeof(m->connect));
	}

exit:
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_inCallback()
{
	thread_id_type thread_id = Paho_thread_getid();
	return thread_id == sendThread_id || thread_id == receiveThread_id;
}


int MQTTAsync_subscribeMany(MQTTAsync handle, int count, char* const* topic, const int* qos, MQTTAsync_responseOptions* response)
{
	MQTTAsyncs* m = handle;
	int i = 0;
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsync_queuedCommand* sub;
	int msgid = 0;

	FUNC_ENTRY;
	if (!MQTTAsync_inCallback())
		MQTTAsync_lock_mutex(mqttasync_mutex);
	if (m == NULL || m->c == NULL)
		rc = MQTTASYNC_FAILURE;
	else if (m->c->connected == 0)
		rc = MQTTASYNC_DISCONNECTED;
	else for (i = 0; i < count; i++)
	{
		if (!UTF8_validateString(topic[i]))
		{
			rc = MQTTASYNC_BAD_UTF8_STRING;
			break;
		}
		if (qos[i] < 0 || qos[i] > 2)
		{
			rc = MQTTASYNC_BAD_QOS;
			break;
		}
	}
	if (rc != MQTTASYNC_SUCCESS)
		; /* don't overwrite a previous error code */
	else if ((msgid = MQTTAsync_assignMsgId(m)) == 0)
		rc = MQTTASYNC_NO_MORE_MSGIDS;
	else if (m->c->MQTTVersion >= MQTTVERSION_5 && count > 1 && (count != response->subscribeOptionsCount
			&& response->subscribeOptionsCount != 0))
		rc = MQTTASYNC_BAD_MQTT_OPTION;
	else if (response)
	{
		if (m->c->MQTTVersion >= MQTTVERSION_5)
		{
			if (response->struct_version == 0 || response->onFailure || response->onSuccess)
				rc = MQTTASYNC_BAD_MQTT_OPTION;
		}
		else if (m->c->MQTTVersion < MQTTVERSION_5)
		{
			if (response->struct_version >= 1 && (response->onFailure5 || response->onSuccess5))
				rc = MQTTASYNC_BAD_MQTT_OPTION;
		}
	}
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	/* Add subscribe request to operation queue */
	if ((sub = malloc(sizeof(MQTTAsync_queuedCommand))) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	memset(sub, '\0', sizeof(MQTTAsync_queuedCommand));
	sub->client = m;
	sub->command.token = msgid;
	if (response)
	{
		sub->command.onSuccess = response->onSuccess;
		sub->command.onFailure = response->onFailure;
		sub->command.onSuccess5 = response->onSuccess5;
		sub->command.onFailure5 = response->onFailure5;
		sub->command.context = response->context;
		response->token = sub->command.token;
		if (m->c->MQTTVersion >= MQTTVERSION_5)
		{
			sub->command.properties = MQTTProperties_copy(&response->properties);
			sub->command.details.sub.opts = response->subscribeOptions;
			if (count > 1)
			{
				if ((sub->command.details.sub.optlist = malloc(sizeof(MQTTSubscribe_options) * count)) == NULL)
				{
					rc = PAHO_MEMORY_ERROR;
					goto exit;
				}
				if (response->subscribeOptionsCount == 0)
				{
					MQTTSubscribe_options initialized = MQTTSubscribe_options_initializer;
					for (i = 0; i < count; ++i)
						sub->command.details.sub.optlist[i] = initialized;
				}
				else
				{
					for (i = 0; i < count; ++i)
						sub->command.details.sub.optlist[i] = response->subscribeOptionsList[i];
				}
			}
		}
	}
	sub->command.type = SUBSCRIBE;
	sub->command.details.sub.count = count;
	sub->command.details.sub.topics = malloc(sizeof(char*) * count);
	sub->command.details.sub.qoss = malloc(sizeof(int) * count);
	if (sub->command.details.sub.topics && sub->command.details.sub.qoss)
	{
		for (i = 0; i < count; ++i)
		{
			if ((sub->command.details.sub.topics[i] = MQTTStrdup(topic[i])) == NULL)
			{
				rc = PAHO_MEMORY_ERROR;
				goto exit;
			}
			sub->command.details.sub.qoss[i] = qos[i];
		}
		rc = MQTTAsync_addCommand(sub, sizeof(sub));
	}
	else
		rc = PAHO_MEMORY_ERROR;

exit:
	if (!MQTTAsync_inCallback())
		MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_subscribe(MQTTAsync handle, const char* topic, int qos, MQTTAsync_responseOptions* response)
{
	int rc = 0;
	FUNC_ENTRY;
	rc = MQTTAsync_subscribeMany(handle, 1, (char * const *)(&topic), &qos, response);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_unsubscribeMany(MQTTAsync handle, int count, char* const* topic, MQTTAsync_responseOptions* response)
{
	MQTTAsyncs* m = handle;
	int i = 0;
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsync_queuedCommand* unsub;
	int msgid = 0;

	FUNC_ENTRY;
	if (!MQTTAsync_inCallback())
		MQTTAsync_lock_mutex(mqttasync_mutex);
	if (m == NULL || m->c == NULL)
		rc = MQTTASYNC_FAILURE;
	else if (m->c->connected == 0)
		rc = MQTTASYNC_DISCONNECTED;
	else for (i = 0; i < count; i++)
	{
		if (!UTF8_validateString(topic[i]))
		{
			rc = MQTTASYNC_BAD_UTF8_STRING;
			break;
		}
	}
	if (rc != MQTTASYNC_SUCCESS)
		; /* don't overwrite a previous error code */
	else if ((msgid = MQTTAsync_assignMsgId(m)) == 0)
		rc = MQTTASYNC_NO_MORE_MSGIDS;
	else if (response)
	{
		if (m->c->MQTTVersion >= MQTTVERSION_5)
		{
			if (response->struct_version == 0 || response->onFailure || response->onSuccess)
				rc = MQTTASYNC_BAD_MQTT_OPTION;
		}
		else if (m->c->MQTTVersion < MQTTVERSION_5)
		{
			if (response->struct_version >= 1 && (response->onFailure5 || response->onSuccess5))
				rc = MQTTASYNC_BAD_MQTT_OPTION;
		}
	}
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	/* Add unsubscribe request to operation queue */
	if ((unsub = malloc(sizeof(MQTTAsync_queuedCommand))) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	memset(unsub, '\0', sizeof(MQTTAsync_queuedCommand));
	unsub->client = m;
	unsub->command.type = UNSUBSCRIBE;
	unsub->command.token = msgid;
	if (response)
	{
		unsub->command.onSuccess = response->onSuccess;
		unsub->command.onFailure = response->onFailure;
		unsub->command.onSuccess5 = response->onSuccess5;
		unsub->command.onFailure5 = response->onFailure5;
		unsub->command.context = response->context;
		response->token = unsub->command.token;
		if (m->c->MQTTVersion >= MQTTVERSION_5)
			unsub->command.properties = MQTTProperties_copy(&response->properties);
	}
	unsub->command.details.unsub.count = count;
	if ((unsub->command.details.unsub.topics = malloc(sizeof(char*) * count)) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	for (i = 0; i < count; ++i)
		unsub->command.details.unsub.topics[i] = MQTTStrdup(topic[i]);
	rc = MQTTAsync_addCommand(unsub, sizeof(unsub));

exit:
	if (!MQTTAsync_inCallback())
		MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_unsubscribe(MQTTAsync handle, const char* topic, MQTTAsync_responseOptions* response)
{
	int rc = 0;
	FUNC_ENTRY;
	rc = MQTTAsync_unsubscribeMany(handle, 1, (char * const *)(&topic), response);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_send(MQTTAsync handle, const char* destinationName, int payloadlen, const void* payload,
							 int qos, int retained, MQTTAsync_responseOptions* response)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;
	MQTTAsync_queuedCommand* pub;
	int msgid = 0;

	FUNC_ENTRY;
	if (!MQTTAsync_inCallback())
		MQTTAsync_lock_mutex(mqttasync_mutex);
	if (m == NULL || m->c == NULL)
		rc = MQTTASYNC_FAILURE;
	else if (m->c->connected == 0)
	{
		if (m->createOptions == NULL)
			rc = MQTTASYNC_DISCONNECTED;
		else if (m->createOptions->sendWhileDisconnected == 0)
			rc = MQTTASYNC_DISCONNECTED;
		else if (m->shouldBeConnected == 0 && (m->createOptions->struct_version < 2 || m->createOptions->allowDisconnectedSendAtAnyTime == 0))
			rc = MQTTASYNC_DISCONNECTED;
	}

	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	if (!UTF8_validateString(destinationName))
		rc = MQTTASYNC_BAD_UTF8_STRING;
	else if (qos < 0 || qos > 2)
		rc = MQTTASYNC_BAD_QOS;
	else if (qos > 0 && (msgid = MQTTAsync_assignMsgId(m)) == 0)
		rc = MQTTASYNC_NO_MORE_MSGIDS;
	else if (m->createOptions &&
			(m->createOptions->struct_version < 2 || m->createOptions->deleteOldestMessages == 0) &&
			(MQTTAsync_getNoBufferedMessages(m) >= m->createOptions->maxBufferedMessages))
		rc = MQTTASYNC_MAX_BUFFERED_MESSAGES;
	else if (response)
	{
		if (m->c->MQTTVersion >= MQTTVERSION_5)
		{
			if (response->struct_version == 0 || response->onFailure || response->onSuccess)
				rc = MQTTASYNC_BAD_MQTT_OPTION;
		}
		else if (m->c->MQTTVersion < MQTTVERSION_5)
		{
			if (response->struct_version >= 1 && (response->onFailure5 || response->onSuccess5))
				rc = MQTTASYNC_BAD_MQTT_OPTION;
		}
	}

	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

	/* Add publish request to operation queue */
	if ((pub = malloc(sizeof(MQTTAsync_queuedCommand))) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	memset(pub, '\0', sizeof(MQTTAsync_queuedCommand));
	pub->client = m;
	pub->command.type = PUBLISH;
	pub->command.token = msgid;
	if (response)
	{
		pub->command.onSuccess = response->onSuccess;
		pub->command.onFailure = response->onFailure;
		pub->command.onSuccess5 = response->onSuccess5;
		pub->command.onFailure5 = response->onFailure5;
		pub->command.context = response->context;
		response->token = pub->command.token;
		if (m->c->MQTTVersion >= MQTTVERSION_5)
			pub->command.properties = MQTTProperties_copy(&response->properties);
	}
	if ((pub->command.details.pub.destinationName = MQTTStrdup(destinationName)) == NULL)
	{
		free(pub);
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	pub->command.details.pub.payloadlen = payloadlen;
	if ((pub->command.details.pub.payload = malloc(payloadlen)) == NULL)
	{
		free(pub->command.details.pub.destinationName);
		free(pub);
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	memcpy(pub->command.details.pub.payload, payload, payloadlen);
	pub->command.details.pub.qos = qos;
	pub->command.details.pub.retained = retained;
	rc = MQTTAsync_addCommand(pub, sizeof(pub));

exit:
	if (!MQTTAsync_inCallback())
		MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_sendMessage(MQTTAsync handle, const char* destinationName, const MQTTAsync_message* message,
													 MQTTAsync_responseOptions* response)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	if (message == NULL)
	{
		rc = MQTTASYNC_NULL_PARAMETER;
		goto exit;
	}
	if (strncmp(message->struct_id, "MQTM", 4) != 0 ||
			(message->struct_version != 0 && message->struct_version != 1))
	{
		rc = MQTTASYNC_BAD_STRUCTURE;
		goto exit;
	}

	if (m->c->MQTTVersion >= MQTTVERSION_5 && response)
		response->properties = message->properties;

	rc = MQTTAsync_send(handle, destinationName, message->payloadlen, message->payload,
								message->qos, message->retained, response);
exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_disconnect(MQTTAsync handle, const MQTTAsync_disconnectOptions* options)
{
	int rc = 0;

	FUNC_ENTRY;
	if (!MQTTAsync_inCallback())
		MQTTAsync_lock_mutex(mqttasync_mutex);
	if (options != NULL && (strncmp(options->struct_id, "MQTD", 4) != 0 || options->struct_version < 0 || options->struct_version > 1))
		rc = MQTTASYNC_BAD_STRUCTURE;
	else
		rc = MQTTAsync_disconnect1(handle, options, 0);
	if (!MQTTAsync_inCallback())
		MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_isConnected(MQTTAsync handle)
{
	MQTTAsyncs* m = handle;
	int rc = 0;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);
	if (m && m->c)
		rc = m->c->connected;
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_isComplete(MQTTAsync handle, MQTTAsync_token dt)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;
	ListElement* current = NULL;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL)
	{
		rc = MQTTASYNC_FAILURE;
		goto exit;
	}

	/* First check unprocessed commands */
	current = NULL;
	while (ListNextElement(MQTTAsync_commands, &current))
	{
		MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)(current->content);

		if (cmd->client == m && cmd->command.token == dt)
			goto exit;
	}

	/* Now check the inflight messages */
	if (m->c && m->c->outboundMsgs->count > 0)
	{
		current = NULL;
		while (ListNextElement(m->c->outboundMsgs, &current))
		{
			Messages* m = (Messages*)(current->content);
			if (m->msgid == dt)
				goto exit;
		}
	}
	rc = MQTTASYNC_TRUE; /* Can't find it, so it must be complete */

exit:
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_waitForCompletion(MQTTAsync handle, MQTTAsync_token dt, unsigned long timeout)
{
	int rc = MQTTASYNC_FAILURE;
	START_TIME_TYPE start = MQTTTime_start_clock();
	ELAPSED_TIME_TYPE elapsed = 0L;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL || m->c == NULL)
	{
		MQTTAsync_unlock_mutex(mqttasync_mutex);
		rc = MQTTASYNC_FAILURE;
		goto exit;
	}
	if (m->c->connected == 0)
	{
		MQTTAsync_unlock_mutex(mqttasync_mutex);
		rc = MQTTASYNC_DISCONNECTED;
		goto exit;
	}
	MQTTAsync_unlock_mutex(mqttasync_mutex);

	if (MQTTAsync_isComplete(handle, dt) == 1)
	{
		rc = MQTTASYNC_SUCCESS; /* well we couldn't find it */
		goto exit;
	}

	elapsed = MQTTTime_elapsed(start);
	while (elapsed < timeout && rc == MQTTASYNC_FAILURE)
	{
		MQTTTime_sleep(100);
		if (MQTTAsync_isComplete(handle, dt) == 1)
			rc = MQTTASYNC_SUCCESS; /* well we couldn't find it */
		MQTTAsync_lock_mutex(mqttasync_mutex);
		if (m->c->connected == 0)
			rc = MQTTASYNC_DISCONNECTED;
		MQTTAsync_unlock_mutex(mqttasync_mutex);
		elapsed = MQTTTime_elapsed(start);
	}
exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_getPendingTokens(MQTTAsync handle, MQTTAsync_token **tokens)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;
	ListElement* current = NULL;
	int count = 0;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);
	MQTTAsync_lock_mutex(mqttcommand_mutex);
	*tokens = NULL;

	if (m == NULL)
	{
		rc = MQTTASYNC_FAILURE;
		goto exit;
	}

	/* calculate the number of pending tokens - commands plus inflight */
	while (ListNextElement(MQTTAsync_commands, &current))
	{
		MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)(current->content);

		if (cmd->client == m && cmd->command.type == PUBLISH)
			count++;
	}
	if (m->c)
		count += m->c->outboundMsgs->count;
	if (count == 0)
		goto exit; /* no tokens to return */
	*tokens = malloc(sizeof(MQTTAsync_token) * (count + 1));  /* add space for sentinel at end of list */
	if (!*tokens)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}

	/* First add the unprocessed commands to the pending tokens */
	current = NULL;
	count = 0;
	while (ListNextElement(MQTTAsync_commands, &current))
	{
		MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)(current->content);

		if (cmd->client == m  && cmd->command.type == PUBLISH)
			(*tokens)[count++] = cmd->command.token;
	}

	/* Now add the inflight messages */
	if (m->c && m->c->outboundMsgs->count > 0)
	{
		current = NULL;
		while (ListNextElement(m->c->outboundMsgs, &current))
		{
			Messages* m = (Messages*)(current->content);
			(*tokens)[count++] = m->msgid;
		}
	}
	(*tokens)[count] = -1; /* indicate end of list */

exit:
	MQTTAsync_unlock_mutex(mqttcommand_mutex);
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_setCallbacks(MQTTAsync handle, void* context,
									MQTTAsync_connectionLost* cl,
									MQTTAsync_messageArrived* ma,
									MQTTAsync_deliveryComplete* dc)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL || ma == NULL || m->c == NULL || m->c->connect_state != NOT_IN_PROGRESS)
		rc = MQTTASYNC_FAILURE;
	else
	{
		m->clContext = m->maContext = m->dcContext = context;
		m->cl = cl;
		m->ma = ma;
		m->dc = dc;
	}

	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}

int MQTTAsync_setConnectionLostCallback(MQTTAsync handle, void* context,
										MQTTAsync_connectionLost* cl)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL || m->c->connect_state != 0)
		rc = MQTTASYNC_FAILURE;
	else
	{
		m->clContext = context;
		m->cl = cl;
	}

	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_setMessageArrivedCallback(MQTTAsync handle, void* context,
										MQTTAsync_messageArrived* ma)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL || ma == NULL || m->c->connect_state != 0)
		rc = MQTTASYNC_FAILURE;
	else
	{
		m->maContext = context;
		m->ma = ma;
	}

	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}

int MQTTAsync_setDeliveryCompleteCallback(MQTTAsync handle, void* context,
										  MQTTAsync_deliveryComplete* dc)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL || m->c->connect_state != 0)
		rc = MQTTASYNC_FAILURE;
	else
	{
		m->dcContext = context;
		m->dc = dc;
	}

	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_setDisconnected(MQTTAsync handle, void* context, MQTTAsync_disconnected* disconnected)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL || m->c->connect_state != NOT_IN_PROGRESS)
		rc = MQTTASYNC_FAILURE;
	else
	{
		m->disconnected_context = context;
		m->disconnected = disconnected;
	}

	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_setConnected(MQTTAsync handle, void* context, MQTTAsync_connected* connected)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL || m->c->connect_state != NOT_IN_PROGRESS)
		rc = MQTTASYNC_FAILURE;
	else
	{
		m->connected_context = context;
		m->connected = connected;
	}

	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_setUpdateConnectOptions(MQTTAsync handle, void* context, MQTTAsync_updateConnectOptions* updateOptions)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL)
		rc = MQTTASYNC_FAILURE;
	else
	{
		m->updateConnectOptions_context = context;
		m->updateConnectOptions = updateOptions;
	}

	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


#if !defined(NO_PERSISTENCE)
int MQTTAsync_setBeforePersistenceWrite(MQTTAsync handle, void* context, MQTTPersistence_beforeWrite* co)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL)
		rc = MQTTASYNC_FAILURE;
	else
	{
		m->c->beforeWrite = co;
		m->c->beforeWrite_context = context;
	}

	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_setAfterPersistenceRead(MQTTAsync handle, void* context, MQTTPersistence_afterRead* co)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);

	if (m == NULL)
		rc = MQTTASYNC_FAILURE;
	else
	{
		m->c->afterRead = co;
		m->c->afterRead_context = context;
	}

	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}
#endif


void MQTTAsync_setTraceLevel(enum MQTTASYNC_TRACE_LEVELS level)
{
	Log_setTraceLevel((enum LOG_LEVELS)level);
}


void MQTTAsync_setTraceCallback(MQTTAsync_traceCallback* callback)
{
	Log_setTraceCallback((Log_traceCallback*)callback);
}


MQTTAsync_nameValue* MQTTAsync_getVersionInfo(void)
{
	#define MAX_INFO_STRINGS 8
	static MQTTAsync_nameValue libinfo[MAX_INFO_STRINGS + 1];
	int i = 0;

	libinfo[i].name = "Product name";
	libinfo[i++].value = "Eclipse Paho Asynchronous MQTT C Client Library";

	libinfo[i].name = "Version";
	libinfo[i++].value = CLIENT_VERSION;

	libinfo[i].name = "Build level";
	libinfo[i++].value = BUILD_TIMESTAMP;
#if defined(OPENSSL)
	libinfo[i].name = "OpenSSL version";
	libinfo[i++].value = SSLeay_version(SSLEAY_VERSION);

	libinfo[i].name = "OpenSSL flags";
	libinfo[i++].value = SSLeay_version(SSLEAY_CFLAGS);

	libinfo[i].name = "OpenSSL build timestamp";
	libinfo[i++].value = SSLeay_version(SSLEAY_BUILT_ON);

	libinfo[i].name = "OpenSSL platform";
	libinfo[i++].value = SSLeay_version(SSLEAY_PLATFORM);

	libinfo[i].name = "OpenSSL directory";
	libinfo[i++].value = SSLeay_version(SSLEAY_DIR);
#endif
	libinfo[i].name = NULL;
	libinfo[i].value = NULL;
	return libinfo;
}

const char* MQTTAsync_strerror(int code)
{
  static char buf[30];
  int chars = 0;

  switch (code) {
    case MQTTASYNC_SUCCESS:
      return "Success";
    case MQTTASYNC_FAILURE:
      return "Failure";
    case MQTTASYNC_PERSISTENCE_ERROR:
      return "Persistence error";
    case MQTTASYNC_DISCONNECTED:
      return "Disconnected";
    case MQTTASYNC_MAX_MESSAGES_INFLIGHT:
      return "Maximum in-flight messages amount reached";
    case MQTTASYNC_BAD_UTF8_STRING:
      return "Invalid UTF8 string";
    case MQTTASYNC_NULL_PARAMETER:
      return "Invalid (NULL) parameter";
    case MQTTASYNC_TOPICNAME_TRUNCATED:
      return "Topic containing NULL characters has been truncated";
    case MQTTASYNC_BAD_STRUCTURE:
      return "Bad structure";
    case MQTTASYNC_BAD_QOS:
      return "Invalid QoS value";
    case MQTTASYNC_NO_MORE_MSGIDS:
      return "Too many pending commands";
    case MQTTASYNC_OPERATION_INCOMPLETE:
      return "Operation discarded before completion";
    case MQTTASYNC_MAX_BUFFERED_MESSAGES:
      return "No more messages can be buffered";
    case MQTTASYNC_SSL_NOT_SUPPORTED:
      return "SSL is not supported";
    case MQTTASYNC_BAD_PROTOCOL:
      return "Invalid protocol scheme";
    case MQTTASYNC_BAD_MQTT_OPTION:
      return "Options for wrong MQTT version";
    case MQTTASYNC_WRONG_MQTT_VERSION:
      return "Client created for another version of MQTT";
    case MQTTASYNC_0_LEN_WILL_TOPIC:
      return "Zero length will topic on connect";
    case MQTTASYNC_COMMAND_IGNORED:
      return "Connect or disconnect command ignored";
    case MQTTASYNC_MAX_BUFFERED:
      return "maxBufferedMessages in the connect options must be >= 0";
  }

  chars = snprintf(buf, sizeof(buf), "Unknown error code %d", code);
  if (chars >= sizeof(buf))
  {
	buf[sizeof(buf)-1] = '\0';
	Log(LOG_ERROR, 0, "Error writing %d chars with snprintf", chars);
  }
  return buf;
}


void MQTTAsync_freeMessage(MQTTAsync_message** message)
{
	FUNC_ENTRY;
	MQTTProperties_free(&(*message)->properties);
	free((*message)->payload);
	free(*message);
	*message = NULL;
	FUNC_EXIT;
}


void MQTTAsync_free(void* memory)
{
	FUNC_ENTRY;
	free(memory);
	FUNC_EXIT;
}


void* MQTTAsync_malloc(size_t size)
{
	void* val;
	int rc = 0;

	FUNC_ENTRY;
	val = malloc(size);
	rc = (val != NULL);
	FUNC_EXIT_RC(rc);
	return val;
}


static void MQTTAsync_freeServerURIs(MQTTAsyncs* m)
{
	int i;

	for (i = 0; i < m->serverURIcount; ++i)
		free(m->serverURIs[i]);
	m->serverURIcount = 0;
	if (m->serverURIs)
		free(m->serverURIs);
	m->serverURIs = NULL;
}
