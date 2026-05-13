/*******************************************************************************
 * Copyright (c) 2009, 2026 IBM Corp., Ian Craggs and others
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
 *    Sven Gambel - add generic proxy support
 *******************************************************************************/

#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
	#include <sys/time.h>
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
#include "Proxy.h"

#if defined(OPENSSL) && defined(LIBRESSL_VERSION_NUMBER)
	#include <openssl/err.h>
#endif


static int clientSockCompare(void* a, void* b);
static int MQTTAsync_checkConn(MQTTAsync_command* command, MQTTAsyncs* client, int was_connected);
#if !defined(NO_PERSISTENCE)
static int MQTTAsync_unpersistCommand(MQTTAsync_queuedCommand* qcmd);
static int MQTTAsync_persistCommand(MQTTAsync_queuedCommand* qcmd);
static MQTTAsync_queuedCommand* MQTTAsync_restoreCommand(char* buffer, int buflen, int MQTTVersion, MQTTAsync_queuedCommand*);
#endif
static void MQTTAsync_startConnectRetry(MQTTAsyncs* m);
static void MQTTAsync_checkDisconnect(MQTTAsync handle, MQTTAsync_command* command);
static void MQTTProtocol_checkPendingWrites(void);
static void MQTTAsync_freeCommand1(MQTTAsync_queuedCommand *command);
static void MQTTAsync_freeCommand(MQTTAsync_queuedCommand *command);
static int MQTTAsync_processCommand(void);
static void MQTTAsync_checkTimeouts(void);
static int MQTTAsync_completeConnection(MQTTAsyncs* m, Connack* connack);
static void MQTTAsync_stop(void);
static void MQTTAsync_closeOnly(Clients* client, enum MQTTReasonCodes reasonCode, MQTTProperties* props);
static int clientStructCompare(void* a, void* b);
static int MQTTAsync_cleanSession(Clients* client);
static int MQTTAsync_deliverMessage(MQTTAsyncs* m, char* topicName, size_t topicLen, MQTTAsync_message* mm);
static int MQTTAsync_disconnect_internal(MQTTAsync handle, int timeout);
static int cmdMessageIDCompare(void* a, void* b);
static void MQTTAsync_retry(void);
static MQTTPacket* MQTTAsync_cycle(SOCKET* sock, unsigned long timeout, int* rc);
static int MQTTAsync_connecting(MQTTAsyncs* m);

extern MQTTProtocol state; /* defined in MQTTAsync.c */
extern ClientStates* bstate; /* defined in MQTTAsync.c */

extern enum MQTTAsync_threadStates sendThread_state;
extern enum MQTTAsync_threadStates receiveThread_state;
extern thread_id_type sendThread_id,
               receiveThread_id;

extern volatile int global_initialized;
extern List* MQTTAsync_handles;
extern List* MQTTAsync_commands;
extern int MQTTAsync_tostop;

#if defined(_WIN32)
	#if defined(_MSC_VER) && _MSC_VER < 1900
		#define snprintf _snprintf
	#endif
extern mutex_type mqttasync_mutex;
extern mutex_type socket_mutex;
extern mutex_type mqttcommand_mutex;
#if !defined(NO_HEAP_TRACKING)
extern mutex_type stack_mutex;
extern mutex_type heap_mutex;
#endif
extern mutex_type log_mutex;
#else
extern mutex_type mqttasync_mutex;
extern mutex_type socket_mutex;
extern mutex_type mqttcommand_mutex;
#endif

extern evt_type send_evt;

#if !defined(min)
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

void MQTTAsync_sleep(long milliseconds)
{
	FUNC_ENTRY;
#if defined(_WIN32)
	Sleep(milliseconds);
#else
	usleep(milliseconds*1000);
#endif
	FUNC_EXIT;
}


/* Add random amount of jitter for exponential backoff on retry
   Jitter value will be +/- 20% of "base" interval, including max interval
   https://www.awsarchitectureblog.com/2015/03/backoff.html
   http://ee.lbl.gov/papers/sync_94.pdf */
int MQTTAsync_randomJitter(int currentIntervalBase, int minInterval, int maxInterval)
{
	const int max_sleep = (int)(min(maxInterval, currentIntervalBase) * 1.2); // (e.g. 72 if base > 60)
	const int min_sleep = (int)(max(minInterval, currentIntervalBase) / 1.2); // (e.g. 48 if base > 60)

	if (min_sleep >= max_sleep) // shouldn't happen, but just in case
	{
		return min_sleep;
	}

	{
		/* random_between(min_sleep, max_sleep)
		  http://stackoverflow.com/questions/2509679/how-to-generate-a-random-number-from-within-a-range */
		int r;
		int range = max_sleep - min_sleep + 1;
		const int buckets = RAND_MAX / range;
		const int limit = buckets * range;

		/* Create equal size buckets all in a row, then fire randomly towards
		 * the buckets until you land in one of them. All buckets are equally
		 * likely. If you land off the end of the line of buckets, try again. */
		do
		{
			r = rand();
		} while (r >= limit);

		{
			const int randResult = r / buckets;
			return min_sleep + randResult;
		}
	}
}


/**
 * List callback function for comparing clients by socket
 * @param a first integer value
 * @param b second integer value
 * @return boolean indicating whether a and b are equal
 */
static int clientSockCompare(void* a, void* b)
{
	MQTTAsyncs* m = (MQTTAsyncs*)a;
	return m->c->net.socket == *(int*)b;
}


void MQTTAsync_lock_mutex(mutex_type amutex)
{
	int rc = Paho_thread_lock_mutex(amutex);
	if (rc != 0)
		Log(LOG_ERROR, 0, "Error %s locking mutex", strerror(rc));
}


void MQTTAsync_unlock_mutex(mutex_type amutex)
{
	int rc = Paho_thread_unlock_mutex(amutex);
	if (rc != 0)
		Log(LOG_ERROR, 0, "Error %s unlocking mutex", strerror(rc));
}


/*
  Check whether there are any more connect options.  If not then we are finished
  with connect attempts.
  return 1 if more connect options left
*/
static int MQTTAsync_checkConn(MQTTAsync_command* command, MQTTAsyncs* client, int was_connected)
{
	int rc;

	FUNC_ENTRY;
	rc = command->details.conn.currentURI + 1 < client->serverURIcount ||
		(was_connected == 0 && command->details.conn.MQTTVersion == MQTTVERSION_3_1 && client->c->MQTTVersion == MQTTVERSION_DEFAULT);
	FUNC_EXIT_RC(rc);
	return rc;
}


void MQTTAsync_terminate(void)
{
	FUNC_ENTRY;
	MQTTAsync_stop();

	/* don't destroy global data if a new client was created while waiting for background threads to terminate */
	if (global_initialized && bstate->clients->count == 0)
	{
		ListElement* elem = NULL;
		ListFree(bstate->clients);
		ListFree(MQTTAsync_handles);
		while (ListNextElement(MQTTAsync_commands, &elem))
			MQTTAsync_freeCommand1((MQTTAsync_queuedCommand*)(elem->content));
		ListFree(MQTTAsync_commands);
		MQTTAsync_handles = NULL;
		WebSocket_terminate();
		#if !defined(NO_HEAP_TRACKING)
			Heap_terminate();
		#endif
		Log_terminate();
		global_initialized = 0;
	}
	FUNC_EXIT;
}


#if !defined(NO_PERSISTENCE)
static int MQTTAsync_unpersistCommand(MQTTAsync_queuedCommand* qcmd)
{
	int rc = 0;
	char key[PERSISTENCE_MAX_KEY_LENGTH + 1];
	int chars = 0;

	FUNC_ENTRY;
	if (qcmd->client->c->MQTTVersion >= MQTTVERSION_5)
		chars = snprintf(key, sizeof(key), "%s%u", PERSISTENCE_V5_COMMAND_KEY, qcmd->seqno);
	else
		chars = snprintf(key, sizeof(key), "%s%u", PERSISTENCE_COMMAND_KEY, qcmd->seqno);
	if (chars >= sizeof(key))
	{
		rc = MQTTASYNC_PERSISTENCE_ERROR;
		Log(LOG_ERROR, 0, "Error writing %d chars with snprintf", chars);
	}
	else if ((rc = qcmd->client->c->persistence->premove(qcmd->client->c->phandle, key)) != 0)
		Log(LOG_ERROR, 0, "Error %d removing command from persistence", rc);
	FUNC_EXIT_RC(rc);
	return rc;
}


static int MQTTAsync_persistCommand(MQTTAsync_queuedCommand* qcmd)
{
	int rc = 0;
	MQTTAsyncs* aclient = qcmd->client;
	MQTTAsync_command* command = &qcmd->command;
	int* lens = NULL;
	void** bufs = NULL;
	int bufindex = 0, i, nbufs = 0;
	char key[PERSISTENCE_MAX_KEY_LENGTH + 1];
	int chars = 0; /* number of chars from snprintf */
	int props_allocated = 0;
	int process = 1;
	int multiplier = 2; /* default value 2 for MQTTVERSION < 5 */

	FUNC_ENTRY;
	switch (command->type)
	{
		case SUBSCRIBE:
			multiplier = (aclient->c->MQTTVersion >= MQTTVERSION_5) ? 3 : 2;
			nbufs = ((aclient->c->MQTTVersion >= MQTTVERSION_5) ? 4 : 3) +
				(command->details.sub.count * multiplier);

			if (((lens = (int*)malloc(nbufs * sizeof(int))) == NULL) ||
					((bufs = malloc(nbufs * sizeof(char *))) == NULL))
			{
				rc = PAHO_MEMORY_ERROR;
				goto exit;
			}
			bufs[bufindex] = &command->type;
			lens[bufindex++] = sizeof(command->type);

			bufs[bufindex] = &command->token;
			lens[bufindex++] = sizeof(command->token);

			bufs[bufindex] = &command->details.sub.count;
			lens[bufindex++] = sizeof(command->details.sub.count);

			for (i = 0; i < command->details.sub.count; ++i)
			{
				bufs[bufindex] = command->details.sub.topics[i];
				lens[bufindex++] = (int)strlen(command->details.sub.topics[i]) + 1;

				bufs[bufindex] = &command->details.sub.qoss[i];
				lens[bufindex++] = sizeof(command->details.sub.qoss[i]);
				if (aclient->c->MQTTVersion >= MQTTVERSION_5)
				{
					if (command->details.sub.count == 1)
					{
						bufs[bufindex] = &command->details.sub.opts;
						lens[bufindex++] = sizeof(command->details.sub.opts);
					}
					else
					{
						bufs[bufindex] = &command->details.sub.optlist[i];
						lens[bufindex++] = sizeof(command->details.sub.optlist[i]);
					}
				}
			}
			break;

		case UNSUBSCRIBE:
			nbufs = ((aclient->c->MQTTVersion >= MQTTVERSION_5) ? 4 : 3) +
					command->details.unsub.count;

			if (((lens = (int*)malloc(nbufs * sizeof(int))) == NULL) ||
					((bufs = malloc(nbufs * sizeof(char *))) == NULL))
			{
				rc = PAHO_MEMORY_ERROR;
				goto exit;
			}

			bufs[bufindex] = &command->type;
			lens[bufindex++] = sizeof(command->type);

			bufs[bufindex] = &command->token;
			lens[bufindex++] = sizeof(command->token);

			bufs[bufindex] = &command->details.unsub.count;
			lens[bufindex++] = sizeof(command->details.unsub.count);

			for (i = 0; i < command->details.unsub.count; ++i)
			{
				bufs[bufindex] = command->details.unsub.topics[i];
				lens[bufindex++] = (int)strlen(command->details.unsub.topics[i]) + 1;
			}
			break;

		case PUBLISH:
			nbufs = (aclient->c->MQTTVersion >= MQTTVERSION_5) ? 8 : 7;

			if (((lens = (int*)malloc(nbufs * sizeof(int))) == NULL) ||
					((bufs = malloc(nbufs * sizeof(char *))) == NULL))
			{
				rc = PAHO_MEMORY_ERROR;
				goto exit;
			}

			bufs[bufindex] = &command->type;
			lens[bufindex++] = sizeof(command->type);

			bufs[bufindex] = &command->token;
			lens[bufindex++] = sizeof(command->token);

			bufs[bufindex] = command->details.pub.destinationName;
			lens[bufindex++] = (int)strlen(command->details.pub.destinationName) + 1;

			bufs[bufindex] = &command->details.pub.payloadlen;
			lens[bufindex++] = sizeof(command->details.pub.payloadlen);

			bufs[bufindex] = command->details.pub.payload;
			lens[bufindex++] = command->details.pub.payloadlen;

			bufs[bufindex] = &command->details.pub.qos;
			lens[bufindex++] = sizeof(command->details.pub.qos);

			bufs[bufindex] = &command->details.pub.retained;
			lens[bufindex++] = sizeof(command->details.pub.retained);
			break;

		default:
			process = 0;
			break;
	}

	/*
	 * Increment the command sequence number.  Don't exceed the maximum value allowed
	 * by the value PERSISTENCE_MAX_KEY_LENGTH minus the max prefix string length
	 */
	if (++aclient->command_seqno == PERSISTENCE_SEQNO_LIMIT)
		aclient->command_seqno = 0;

	if (aclient->c->MQTTVersion >= MQTTVERSION_5 && process) 	/* persist properties */
	{
		int temp_len = 0;
		char* ptr = NULL;

		temp_len = MQTTProperties_len(&command->properties);
		if ((ptr = bufs[bufindex] = malloc(temp_len)) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}
		props_allocated = bufindex;
		rc = MQTTProperties_write(&ptr, &command->properties);
		lens[bufindex++] = temp_len;
		chars = snprintf(key, sizeof(key), "%s%u", PERSISTENCE_V5_COMMAND_KEY, aclient->command_seqno);
	}
	else
		chars = snprintf(key, sizeof(key), "%s%u", PERSISTENCE_COMMAND_KEY, aclient->command_seqno);
	if (chars >= sizeof(key))
	{
		Log(LOG_ERROR, 0, "Error writing %d chars with snprintf", chars);
		goto exit;
	}

	if (nbufs > 0)
	{
		if (aclient->c->beforeWrite)
			rc = aclient->c->beforeWrite(aclient->c->beforeWrite_context, nbufs, (char**)bufs, lens);

		if ((rc = aclient->c->persistence->pput(aclient->c->phandle, key, nbufs, (char**)bufs, lens)) != 0)
			Log(LOG_ERROR, 0, "Error persisting command, rc %d", rc);
		qcmd->seqno = aclient->command_seqno;
	}
exit:
	if (props_allocated > 0)
		free(bufs[props_allocated]);
	if (lens)
		free(lens);
	if (bufs)
		free(bufs);
	FUNC_EXIT_RC(rc);
	return rc;
}


static MQTTAsync_queuedCommand* MQTTAsync_restoreCommand(char* buffer, int buflen, int MQTTVersion, MQTTAsync_queuedCommand* qcommand)
{
	MQTTAsync_command* command = NULL;
	char* ptr = buffer;
	int i;
	size_t data_size;
	char* endpos = &buffer[buflen];

	FUNC_ENTRY;
	if (buflen == 0)
	{
		qcommand = NULL;
		goto exit;
	}

	if (qcommand == NULL)
	{
		if ((qcommand = malloc(sizeof(MQTTAsync_queuedCommand))) == NULL)
			goto exit;
		memset(qcommand, '\0', sizeof(MQTTAsync_queuedCommand));
		qcommand->not_restored = 1; /* don't restore all the command on the first call */
	}
	else
		qcommand->not_restored = 0;

	command = &qcommand->command;

	if (&ptr[sizeof(int) + sizeof(MQTTAsync_token)] > endpos)
		goto error_exit;
	memcpy(&command->type, ptr, sizeof(int));
	ptr += sizeof(int);
	memcpy(&command->token, ptr, sizeof(MQTTAsync_token));
	ptr += sizeof(MQTTAsync_token);

	switch (command->type)
	{
		case SUBSCRIBE:
			if (qcommand->not_restored == 1)
				break;
			if (&ptr[sizeof(int)] > endpos)
				goto error_exit;
			memcpy(&command->details.sub.count, ptr, sizeof(int));
			ptr += sizeof(int);

			if (command->details.sub.count > 0)
			{
				if ((command->details.sub.topics = (char **)malloc(sizeof(char *) * command->details.sub.count)) == NULL)
					goto error_exit;
				if ((command->details.sub.qoss = (int *)malloc(sizeof(int) * command->details.sub.count)) == NULL)
					goto error_exit;

				if ((MQTTVersion >= MQTTVERSION_5))
				{
					if (command->details.sub.count > 1)
					{
						command->details.sub.optlist = (MQTTSubscribe_options*)malloc(sizeof(MQTTSubscribe_options) * command->details.sub.count);
						if (command->details.sub.optlist == NULL)
							goto error_exit;
					}
				}
			}

			for (i = 0; i < command->details.sub.count; ++i)
			{
				data_size = strnlen(ptr, endpos - ptr) + 1;
				if (data_size == endpos - ptr)
					goto error_exit; /* no null found */

				if ((command->details.sub.topics[i] = malloc(data_size)) == NULL)
					goto error_exit;
				strcpy(command->details.sub.topics[i], ptr);
				ptr += data_size;

				if (&ptr[sizeof(int)] > endpos)
					goto error_exit;
				memcpy(&command->details.sub.qoss[i], ptr, sizeof(int));
				ptr += sizeof(int);

				if (MQTTVersion >= MQTTVERSION_5)
				{
					if (&ptr[sizeof(MQTTSubscribe_options)] > endpos)
						goto error_exit;
					if (command->details.sub.count == 1)
					{
						memcpy(&command->details.sub.opts, ptr, sizeof(MQTTSubscribe_options));
						ptr += sizeof(MQTTSubscribe_options);
					}
					else
					{
						memcpy(&command->details.sub.optlist[i], ptr, sizeof(MQTTSubscribe_options));
						ptr += sizeof(MQTTSubscribe_options);
					}
				}
			}
			break;

		case UNSUBSCRIBE:
			if (qcommand->not_restored == 1)
				break;

			if (&ptr[sizeof(int)] > endpos)
				goto error_exit;
			memcpy(&command->details.unsub.count, ptr, sizeof(int));
			ptr += sizeof(int);

			if (command->details.unsub.count > 0)
			{
				command->details.unsub.topics = (char **)malloc(sizeof(char *) * command->details.unsub.count);
				if (command->details.unsub.topics == NULL)
					goto error_exit;
			}

			for (i = 0; i < command->details.unsub.count; ++i)
			{
				data_size = strnlen(ptr, endpos - ptr) + 1;
				if (data_size == endpos - ptr)
					goto error_exit; /* no null found */

				if ((command->details.unsub.topics[i] = malloc(data_size)) == NULL)
					goto error_exit;
				strcpy(command->details.unsub.topics[i], ptr);
				ptr += data_size;
			}
			break;

		case PUBLISH:
			data_size = strnlen(ptr, endpos - ptr) + 1;
			if (data_size == endpos - ptr)
				goto error_exit; /* no null found */

			if (qcommand->not_restored == 0)
			{
				if ((command->details.pub.destinationName = malloc(data_size)) == NULL)
					goto error_exit;
				strcpy(command->details.pub.destinationName, ptr);
			}
			ptr += data_size;

			if (&ptr[sizeof(int)] > endpos)
				goto error_exit;
			memcpy(&command->details.pub.payloadlen, ptr, sizeof(int));
			ptr += sizeof(int);

			data_size = command->details.pub.payloadlen;
			if (&ptr[data_size] > endpos)
				goto error_exit;
			if (qcommand->not_restored == 0)
			{
				if ((command->details.pub.payload = malloc(data_size)) == NULL)
					goto error_exit;
				memcpy(command->details.pub.payload, ptr, data_size);
			}
			ptr += data_size;

			if (&ptr[sizeof(int)*2] > endpos)
				goto error_exit;
			memcpy(&command->details.pub.qos, ptr, sizeof(int));
			ptr += sizeof(int);

			memcpy(&command->details.pub.retained, ptr, sizeof(int));
			ptr += sizeof(int);
			break;

		default:
			goto error_exit;

	}
	if (qcommand != NULL && qcommand->not_restored == 0 && MQTTVersion >= MQTTVERSION_5 &&
			MQTTProperties_read(&command->properties, &ptr, buffer + buflen) != 1)
	{
			Log(LOG_ERROR, -1, "Error restoring properties from persistence");
			free(qcommand);
			qcommand = NULL;
	}
	goto exit;
error_exit:
	free(qcommand);
	qcommand = NULL;
exit:
	FUNC_EXIT;
	return qcommand;
}


static int cmpkeys(const void *p1, const void *p2)
{
   int key1 = atoi(strchr(*(char * const *)p1, '-') + 1);
   int key2 = atoi(strchr(*(char * const *)p2, '-') + 1);

   return (key1 == key2) ? 0 : ((key1 < key2) ? -1 : 1);
}


int MQTTAsync_restoreCommands(MQTTAsyncs* client)
{
	int rc = 0;
	char **msgkeys;
	int nkeys;
	int i = 0;
	Clients* c = client->c;
	int commands_restored = 0;

	FUNC_ENTRY;
	if (c->persistence && (rc = c->persistence->pkeys(c->phandle, &msgkeys, &nkeys)) == 0 && nkeys > 0)
	{
		/* let's have the sequence number array sorted */
		qsort(msgkeys, (size_t)nkeys, sizeof(char*), cmpkeys);

		while (rc == 0 && i < nkeys)
		{
			char *buffer = NULL;
			int buflen;

			if (strncmp(msgkeys[i], PERSISTENCE_COMMAND_KEY, strlen(PERSISTENCE_COMMAND_KEY)) != 0 &&
				strncmp(msgkeys[i], PERSISTENCE_V5_COMMAND_KEY, strlen(PERSISTENCE_V5_COMMAND_KEY)) != 0)
			{
				;
			}
			else
			{
				MQTTAsync_queuedCommand* cmd = NULL;
				if ((rc = c->persistence->pget(c->phandle, msgkeys[i], &buffer, &buflen)) == 0 &&
					(c->afterRead == NULL || (rc = c->afterRead(c->afterRead_context, &buffer, &buflen)) == 0))
				{
					int MQTTVersion = (strncmp(msgkeys[i], PERSISTENCE_V5_COMMAND_KEY, strlen(PERSISTENCE_V5_COMMAND_KEY)) == 0)
										? MQTTVERSION_5 : MQTTVERSION_3_1_1;
					cmd = MQTTAsync_restoreCommand(buffer, buflen, MQTTVersion, NULL);
				}

				if (cmd)
				{
					/* As the entire command is not restored on the first read to save memory, we temporarily store
					 * the filename of the persisted command to be used when restoreCommand is called the second time.
					 */
					cmd->key = malloc(strlen(msgkeys[i])+1);
					strcpy(cmd->key, msgkeys[i]);

					cmd->client = client;
					cmd->seqno = atoi(strchr(msgkeys[i], '-')+1); /* key format is tag'-'seqno */
					/* we can just append the commands to the list as they've already been sorted */
					ListAppend(MQTTAsync_commands, cmd, sizeof(MQTTAsync_queuedCommand));
					client->command_seqno = max(client->command_seqno, cmd->seqno);
					commands_restored++;
					if (cmd->command.type == PUBLISH)
						client->noBufferedMessages++;
				}
			}
			if (buffer)
				free(buffer);
			if (msgkeys[i])
				free(msgkeys[i]);
			i++;
		}
		if (msgkeys != NULL)
			free(msgkeys);
	}
	Log(TRACE_MINIMUM, -1, "%d commands restored for client %s", commands_restored, c->clientID);
	FUNC_EXIT_RC(rc);
	return rc;
}


int MQTTAsync_unpersistCommandsAndMessages(Clients* c)
{
	int rc = 0;
	char **msgkeys;
	int nkeys;
	int i = 0;
	int messages_deleted = 0;

	FUNC_ENTRY;
	if (c->persistence && (rc = c->persistence->pkeys(c->phandle, &msgkeys, &nkeys)) == 0)
	{
		while (rc == 0 && i < nkeys)
		{
			if (strncmp(msgkeys[i], PERSISTENCE_COMMAND_KEY, strlen(PERSISTENCE_COMMAND_KEY)) == 0 ||
				strncmp(msgkeys[i], PERSISTENCE_V5_COMMAND_KEY, strlen(PERSISTENCE_V5_COMMAND_KEY)) == 0 ||
				strncmp(msgkeys[i], PERSISTENCE_QUEUE_KEY, strlen(PERSISTENCE_QUEUE_KEY)) == 0 ||
				strncmp(msgkeys[i], PERSISTENCE_V5_QUEUE_KEY, strlen(PERSISTENCE_V5_QUEUE_KEY)) == 0)
			{
				if ((rc = c->persistence->premove(c->phandle, msgkeys[i])) == 0)
					messages_deleted++;
				else
					Log(LOG_ERROR, 0, "Error %d removing queued message from persistence", rc);
			}
			if (msgkeys[i])
				free(msgkeys[i]);
			i++;
		}
		if (msgkeys != NULL)
			free(msgkeys);
	}
	Log(TRACE_MINIMUM, -1, "%d queued messages deleted for client %s", messages_deleted, c->clientID);
	FUNC_EXIT_RC(rc);
	return rc;
}


static int MQTTAsync_unpersistInflightMessages(Clients* c)
{
	int rc = 0;
	char **msgkeys;
	int nkeys;
	int i = 0;
	int messages_deleted = 0;

	FUNC_ENTRY;
	if (c->persistence && (rc = c->persistence->pkeys(c->phandle, &msgkeys, &nkeys)) == 0)
	{
		while (rc == 0 && i < nkeys)
		{
			if (strncmp(msgkeys[i], PERSISTENCE_PUBLISH_SENT, strlen(PERSISTENCE_PUBLISH_SENT)) == 0 ||
				strncmp(msgkeys[i], PERSISTENCE_V5_PUBLISH_SENT, strlen(PERSISTENCE_V5_PUBLISH_SENT)) == 0 ||
				strncmp(msgkeys[i], PERSISTENCE_PUBREL, strlen(PERSISTENCE_PUBREL)) == 0 ||
				strncmp(msgkeys[i], PERSISTENCE_V5_PUBREL, strlen(PERSISTENCE_V5_PUBREL)) == 0 ||
				strncmp(msgkeys[i], PERSISTENCE_PUBLISH_RECEIVED, strlen(PERSISTENCE_PUBLISH_RECEIVED)) == 0 ||
				strncmp(msgkeys[i], PERSISTENCE_V5_PUBLISH_RECEIVED, strlen(PERSISTENCE_V5_PUBLISH_RECEIVED)) == 0)
			{
				if ((rc = c->persistence->premove(c->phandle, msgkeys[i])) == 0)
					messages_deleted++;
				else
					Log(LOG_ERROR, 0, "Error %d removing inflight message from persistence", rc);
			}
			if (msgkeys[i])
				free(msgkeys[i]);
			i++;
		}
		if (msgkeys != NULL)
			free(msgkeys);
	}
	Log(TRACE_MINIMUM, -1, "%d inflight messages deleted for client %s", messages_deleted, c->clientID);
	FUNC_EXIT_RC(rc);
	return rc;
}
#endif

#if 0
/**
 * List callback function for comparing client handles and command types being CONNECT or DISCONNECT
 * @param a first MQTTAsync_queuedCommand pointer
 * @param b second MQTTAsync_queuedCommand pointer
 * @return boolean indicating whether a and b are equal
 */
static int clientCompareConnectCommand(void* a, void* b)
{
	MQTTAsync_queuedCommand* cmd1 = (MQTTAsync_queuedCommand*)a;
	MQTTAsync_queuedCommand* cmd2 = (MQTTAsync_queuedCommand*)b;
	if (cmd1->client == cmd2->client)
	{
		if (cmd1->command.type == cmd2->command.type)
		{
			if (cmd1->command.type == CONNECT || cmd1->command.type == DISCONNECT)
			{
				return 1; //Item found in the list
			}
		}
	}
	return 0;	//Item NOT found in the list
}
#endif


int MQTTAsync_addCommand(MQTTAsync_queuedCommand* command, int command_size)
{
	int rc = MQTTASYNC_SUCCESS;
	int rc1 = 0;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttcommand_mutex);
	/* Don't set start time if the connect command is already in process #218 */
	if ((command->command.type != CONNECT) || (command->client->c->connect_state == NOT_IN_PROGRESS))
		command->command.start_time = MQTTTime_start_clock();

	if (command->command.type == CONNECT ||
		(command->command.type == DISCONNECT && command->command.details.dis.internal))
	{
		MQTTAsync_queuedCommand* head = NULL;
		ListElement* current = MQTTAsync_commands->first;

		/* Look for any connect or disconnect command belonging to this client.  All the connects/disconnects
		 * are at the head of the list, so we don't search any further if we meet anything other than a
		 * connect or disconnect for any client.
		 */
		while (current)
		{
			MQTTAsync_queuedCommand* cur_cmd = (MQTTAsync_queuedCommand*)(current->content);
			if (cur_cmd->command.type != CONNECT && cur_cmd->command.type != DISCONNECT)
				break; /* end search if we meet anything other than connect or disconnect */
			if (cur_cmd->client == command->client)
			{
				head = cur_cmd;
				break;
			}
			current = current->prev;
		}

		if (head)
		{
			MQTTAsync_freeCommand(command); /* ignore duplicate connect or disconnect command */
			rc = MQTTASYNC_COMMAND_IGNORED;
		}
		else
		{
			ListElement* result = ListInsert(MQTTAsync_commands, command, command_size, MQTTAsync_commands->first); /* add to the head of the list */
			if (result == NULL)
				rc = PAHO_MEMORY_ERROR;
		}
	}
	else
	{
		if (ListAppend(MQTTAsync_commands, command, command_size) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}
#if !defined(NO_PERSISTENCE)
		if (command->client->c->persistence)
		{
			if (command->command.type == PUBLISH &&
				command->client->createOptions && command->client->createOptions->struct_version >= 2 &&
				command->client->createOptions->persistQoS0 == 0 && command->command.details.pub.qos == 0)
				; /* don't persist QoS0 if that create option is set to 0 */
			else
			{
				rc = MQTTAsync_persistCommand(command);
				if (command->command.type == PUBLISH && rc == 0)
				{
					char key[PERSISTENCE_MAX_KEY_LENGTH + 1];
					int chars = 0;

					command->not_restored = 1;
					if (command->client->c->MQTTVersion >= MQTTVERSION_5)
						chars = snprintf(key, sizeof(key), "%s%u", PERSISTENCE_V5_COMMAND_KEY, command->seqno);
					else
						chars = snprintf(key, sizeof(key), "%s%u", PERSISTENCE_COMMAND_KEY, command->seqno);
					if (chars >= sizeof(key))
					{
						rc = MQTTASYNC_PERSISTENCE_ERROR;
						Log(LOG_ERROR, 0, "Error writing %d chars with snprintf", chars);
						goto exit;
					}
					command->key = malloc(strlen(key)+1);
					strcpy(command->key, key);

					free(command->command.details.pub.payload);
					command->command.details.pub.payload = NULL;
					free(command->command.details.pub.destinationName);
					command->command.details.pub.destinationName = NULL;
					MQTTProperties_free(&command->command.properties);
				}
			}
		}
#endif
		if (command->command.type == PUBLISH)
		{
			/* delete oldest message if buffer is full.  We wouldn't be here if delete newest was in operation */
			if (command->client->createOptions && (command->client->noBufferedMessages >= command->client->createOptions->maxBufferedMessages))
			{
				MQTTAsync_queuedCommand* first_publish = NULL;
				ListElement* current = NULL;

				/* Find first publish command for this client and detach it */
				while (ListNextElement(MQTTAsync_commands, &current))
				{
					MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)(current->content);

					if (cmd->client == command->client && cmd->command.type == PUBLISH)
					{
						first_publish = cmd;
						break;
					}
				}
				if (first_publish)
				{
					ListDetach(MQTTAsync_commands, first_publish);

	#if !defined(NO_PERSISTENCE)
					if (command->client->c->persistence)
						MQTTAsync_unpersistCommand(first_publish);
#endif
					if (first_publish->command.onFailure)
					{
						MQTTAsync_failureData data;

						data.token = first_publish->command.token;
						data.code = MQTTASYNC_MAX_BUFFERED_MESSAGES;
						data.message = NULL;
						Log(TRACE_MIN, -1, "Calling connect failure for client %s, rc %d", command->client->c->clientID, data.code);
						(*(first_publish->command.onFailure))(first_publish->command.context, &data);
					} else if (first_publish->command.onFailure5)
					{
						MQTTAsync_failureData5 data;

						data.token = first_publish->command.token;
						data.code = MQTTASYNC_MAX_BUFFERED_MESSAGES;
						data.message = NULL;
						data.packet_type = PUBLISH;
						Log(TRACE_MIN, -1, "Calling connect failure for client %s, rc %d", command->client->c->clientID, data.code);
						(*(first_publish->command.onFailure5))(first_publish->command.context, &data);
					}
					MQTTAsync_freeCommand(first_publish);
				}
			}
			else
				command->client->noBufferedMessages++;
		}
	}
exit:
	MQTTAsync_unlock_mutex(mqttcommand_mutex);
	if ((rc1 = Thread_signal_evt(send_evt)) != 0)
		Log(LOG_ERROR, 0, "Error %d from signal event", rc1);
	FUNC_EXIT_RC(rc);
	return rc;
}


void MQTTAsync_startConnectRetry(MQTTAsyncs* m)
{
	if (m->automaticReconnect && m->shouldBeConnected)
	{
		m->lastConnectionFailedTime = MQTTTime_start_clock();
		if (m->retrying)
		{
			m->currentIntervalBase = min(m->currentIntervalBase * 2, m->maxRetryInterval);
		}
		else
		{
			m->currentIntervalBase = m->minRetryInterval;
			m->retrying = 1;
		}
		m->currentInterval = MQTTAsync_randomJitter(m->currentIntervalBase, m->minRetryInterval, m->maxRetryInterval);
	}
}


void MQTTAsync_checkDisconnect(MQTTAsync handle, MQTTAsync_command* command)
{
	MQTTAsyncs* m = handle;

	FUNC_ENTRY;
	/* wait for all inflight message flows to finish, up to timeout */;
	if (m->c->outboundMsgs->count == 0 || MQTTTime_elapsed(command->start_time) >= (ELAPSED_TIME_TYPE)command->details.dis.timeout)
	{
		int was_connected = m->c->connected;
		MQTTAsync_closeSession(m->c, command->details.dis.reasonCode, &command->properties);
		if (command->details.dis.internal)
		{
			if (m->cl && was_connected)
			{
				Log(TRACE_MIN, -1, "Calling connectionLost for client %s", m->c->clientID);
				(*(m->cl))(m->clContext, NULL);
			}
			MQTTAsync_startConnectRetry(m);
		}
		else if (command->onSuccess)
		{
			MQTTAsync_successData data;

			memset(&data, '\0', sizeof(data));
			Log(TRACE_MIN, -1, "Calling disconnect complete for client %s", m->c->clientID);
			(*(command->onSuccess))(command->context, &data);
		}
		else if (command->onSuccess5)
		{
			MQTTAsync_successData5 data = MQTTAsync_successData5_initializer;

			data.reasonCode = MQTTASYNC_SUCCESS;
			Log(TRACE_MIN, -1, "Calling disconnect complete for client %s", m->c->clientID);
			(*(command->onSuccess5))(command->context, &data);
		}
	}
	FUNC_EXIT;
}


/**
 * Call Socket_noPendingWrites(int socket) with protection by socket_mutex, see https://github.com/eclipse/paho.mqtt.c/issues/385
 */
static int MQTTAsync_Socket_noPendingWrites(SOCKET socket)
{
    int rc;
    MQTTAsync_lock_mutex(socket_mutex);
    rc = Socket_noPendingWrites(socket);
    MQTTAsync_unlock_mutex(socket_mutex);
    return rc;
}

/**
 * See if any pending writes have been completed, and cleanup if so.
 * Cleaning up means removing any publication data that was stored because the write did
 * not originally complete.
 */
static void MQTTProtocol_checkPendingWrites(void)
{
	FUNC_ENTRY;
	if (state.pending_writes.count > 0)
	{
		ListElement* le = state.pending_writes.first;
		while (le)
		{
			if (MQTTAsync_Socket_noPendingWrites(((pending_write*)(le->content))->socket))
			{
				MQTTProtocol_removePublication(((pending_write*)(le->content))->p);
				state.pending_writes.current = le;
				ListRemove(&(state.pending_writes), le->content); /* does NextElement itself */
				le = state.pending_writes.current;
			}
			else
				ListNextElement(&(state.pending_writes), &le);
		}
	}
	FUNC_EXIT;
}


static void MQTTAsync_freeCommand1(MQTTAsync_queuedCommand *command)
{
	if (command->command.type == SUBSCRIBE)
	{
		int i;

		for (i = 0; i < command->command.details.sub.count; i++)
			free(command->command.details.sub.topics[i]);

		free(command->command.details.sub.topics);
		command->command.details.sub.topics = NULL;
		free(command->command.details.sub.qoss);
		command->command.details.sub.qoss = NULL;
	}
	else if (command->command.type == UNSUBSCRIBE)
	{
		int i;

		for (i = 0; i < command->command.details.unsub.count; i++)
			free(command->command.details.unsub.topics[i]);

		free(command->command.details.unsub.topics);
		command->command.details.unsub.topics = NULL;
	}
	else if (command->command.type == PUBLISH)
	{
		/* qos 1 and 2 topics are freed in the protocol code when the flows are completed */
		if (command->command.details.pub.destinationName)
			free(command->command.details.pub.destinationName);
		command->command.details.pub.destinationName = NULL;
		if (command->command.details.pub.payload)
			free(command->command.details.pub.payload);
		command->command.details.pub.payload = NULL;
	}
	MQTTProperties_free(&command->command.properties);
	if (command->not_restored && command->key)
		free(command->key);
}


static void MQTTAsync_freeCommand(MQTTAsync_queuedCommand *command)
{
	MQTTAsync_freeCommand1(command);
	free(command);
}


void MQTTAsync_writeContinue(SOCKET socket)
{
	ListElement* found = NULL;

	if ((found = ListFindItem(MQTTAsync_handles, &socket, clientSockCompare)) != NULL)
	{
		MQTTAsyncs* m = (MQTTAsyncs*)(found->content);

		m->c->net.lastSent = MQTTTime_now();
	}
}


void MQTTAsync_writeComplete(SOCKET socket, int rc)
{
	ListElement* found = NULL;

	FUNC_ENTRY;

	/* a partial write is now complete for a socket - this will be on a publish*/
	MQTTAsync_lock_mutex(mqttasync_mutex);
	MQTTProtocol_checkPendingWrites();

	/* find the client using this socket */
	if ((found = ListFindItem(MQTTAsync_handles, &socket, clientSockCompare)) != NULL)
	{
		MQTTAsyncs* m = (MQTTAsyncs*)(found->content);

		m->c->net.lastSent = MQTTTime_now();

		/* see if there is a pending write flagged */
		if (m->pending_write)
		{
			ListElement* cur_response = NULL;
			MQTTAsync_command* command = m->pending_write;
			MQTTAsync_queuedCommand* com = NULL;

			cur_response = NULL;
			while (ListNextElement(m->responses, &cur_response))
			{
				com = (MQTTAsync_queuedCommand*)(cur_response->content);
				if (&com->command == m->pending_write)
					break;
			}

			if (cur_response) /* we found a response */
			{
				if (command->type == PUBLISH)
				{
					if (rc == 1 && command->details.pub.qos == 0)
					{
						if (command->onSuccess)
						{
							MQTTAsync_successData data;

							data.token = command->token;
							data.alt.pub.destinationName = command->details.pub.destinationName;
							data.alt.pub.message.payload = command->details.pub.payload;
							data.alt.pub.message.payloadlen = command->details.pub.payloadlen;
							data.alt.pub.message.qos = command->details.pub.qos;
							data.alt.pub.message.retained = command->details.pub.retained;
							Log(TRACE_MIN, -1, "Calling publish success for client %s", m->c->clientID);
							(*(command->onSuccess))(command->context, &data);
						}
						else if (command->onSuccess5)
						{
							MQTTAsync_successData5 data = MQTTAsync_successData5_initializer;

							data.token = command->token;
							data.alt.pub.destinationName = command->details.pub.destinationName;
							data.alt.pub.message.payload = command->details.pub.payload;
							data.alt.pub.message.payloadlen = command->details.pub.payloadlen;
							data.alt.pub.message.qos = command->details.pub.qos;
							data.alt.pub.message.retained = command->details.pub.retained;
							data.properties = command->properties;
							Log(TRACE_MIN, -1, "Calling publish success for client %s", m->c->clientID);
							(*(command->onSuccess5))(command->context, &data);
						}
					}
					else if (rc == -1)
					{
						if (command->onFailure)
						{
							MQTTAsync_failureData data;

							data.token = command->token;
							data.code = rc;
							data.message = NULL;
							Log(TRACE_MIN, -1, "Calling publish failure for client %s", m->c->clientID);
							(*(command->onFailure))(command->context, &data);
						}
						else if (command->onFailure5)
						{
							MQTTAsync_failureData5 data;

							data.token = command->token;
							data.code = rc;
							data.message = NULL;
							data.packet_type = PUBLISH;
							Log(TRACE_MIN, -1, "Calling publish failure for client %s", m->c->clientID);
							(*(command->onFailure5))(command->context, &data);
						}
					}
					else
						com = NULL; /* Don't delete response we haven't acknowledged */
					/* QoS 0 payloads are freed elsewhere after a write complete,
					 * so we should indicate that.
					 */
					if (command->details.pub.qos == 0)
						command->details.pub.payload = NULL;
				}
				if (com)
				{
					Log(TRACE_PROTOCOL, -1, "writeComplete: Removing response for msgid %d", com->command.token);
					ListDetach(m->responses, com);
					MQTTAsync_freeCommand(com);
				}
			} /* if cur_response */
			m->pending_write = NULL;
		} /* if pending_write */
	}
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT;
}


static int MQTTAsync_processCommand(void)
{
	int rc = 0;
	MQTTAsync_queuedCommand* command = NULL;
	ListElement* cur_command = NULL;
	List* ignored_clients = NULL;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);
	MQTTAsync_lock_mutex(mqttcommand_mutex);

	/* only the first command in the list must be processed for any particular client, so if we skip
	   a command for a client, we must skip all following commands for that client.  Use a list of
	   ignored clients to keep track
	*/
	ignored_clients = ListInitialize();

	/* don't try a command until there isn't a pending write for that client, and we are not connecting */
	while (ListNextElement(MQTTAsync_commands, &cur_command))
	{
		MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)(cur_command->content);

		if (ListFind(ignored_clients, cmd->client))
			continue;

		if (cmd->command.type == CONNECT || cmd->command.type == DISCONNECT || (cmd->client->c->connected &&
			cmd->client->c->connect_state == NOT_IN_PROGRESS && MQTTAsync_Socket_noPendingWrites(cmd->client->c->net.socket)))
		{
			if ((cmd->command.type == PUBLISH || cmd->command.type == SUBSCRIBE || cmd->command.type == UNSUBSCRIBE) &&
				cmd->client->c->outboundMsgs->count >= MAX_MSG_ID - 1)
			{
				; /* no more message ids available */
			}
			else if (((cmd->command.type == PUBLISH && cmd->command.details.pub.qos > 0) ||
						cmd->command.type == SUBSCRIBE || cmd->command.type == UNSUBSCRIBE) &&
				(cmd->client->c->outboundMsgs->count >= cmd->client->c->maxInflightMessages))
			{
				Log(TRACE_MIN, -1, "Blocking on server receive maximum for client %s",
						cmd->client->c->clientID); /* flow control */
			}
			else
			{
				command = cmd;
				break;
			}
		}
		ListAppend(ignored_clients, cmd->client, sizeof(cmd->client));
	}
	ListFreeNoContent(ignored_clients);
	if (command)
	{
		if (command->command.type == PUBLISH)
			command->client->noBufferedMessages--;
		ListDetach(MQTTAsync_commands, command);
#if !defined(NO_PERSISTENCE)
		/*printf("outboundmsgs count %d max inflight %d qos %d %d %d\n", command->client->c->outboundMsgs->count, command->client->c->maxInflightMessages,
				command->command.details.pub.qos, command->client->c->MQTTVersion, command->command.type);*/
		if (command->client->c->persistence)
		{
			if (command->not_restored)
			{
				char* buffer = NULL;
				int buflen = 0;

				if ((rc = command->client->c->persistence->pget(command->client->c->phandle, command->key, &buffer, &buflen)) == 0
						&& (command->client->c->afterRead == NULL ||
					(rc = command->client->c->afterRead(command->client->c->afterRead_context, &buffer, &buflen)) == 0))
				{
					int MQTTVersion = (strncmp(command->key, PERSISTENCE_V5_COMMAND_KEY, strlen(PERSISTENCE_V5_COMMAND_KEY)) == 0)
									? MQTTVERSION_5 : MQTTVERSION_3_1_1;
					free(command->key);
					command->key = NULL;
					command = MQTTAsync_restoreCommand(buffer, buflen, MQTTVersion, command);
				}
				else
				{
					Log(LOG_ERROR, -1, "Error restoring command: rc %d from pget\n", rc);
					command = NULL;
				}
				if (buffer)
					free(buffer);
			}
			if (command)
				MQTTAsync_unpersistCommand(command);
		}
#endif
	}
	MQTTAsync_unlock_mutex(mqttcommand_mutex);

	if (!command)
		goto exit; /* nothing to do */

	if (command->command.type == CONNECT)
	{
		if (command->client->c->connect_state != NOT_IN_PROGRESS || command->client->c->connected)
			rc = 0;
		else
		{
			char* serverURI = command->client->serverURI;

			if (command->client->serverURIcount > 0)
			{
				if (command->command.details.conn.currentURI < command->client->serverURIcount)
				{
					serverURI = command->client->serverURIs[command->command.details.conn.currentURI];

					if (strncmp(URI_TCP, serverURI, strlen(URI_TCP)) == 0)
						serverURI += strlen(URI_TCP);
					else if (strncmp(URI_MQTT, serverURI, strlen(URI_MQTT)) == 0)
						serverURI += strlen(URI_MQTT);
#if defined(UNIXSOCK)
					else if (strncmp(URI_UNIX, serverURI, strlen(URI_UNIX)) == 0)
					{
						serverURI += strlen(URI_UNIX);
						command->client->unixsock = 1;
					}
#endif
					else if (strncmp(URI_WS, serverURI, strlen(URI_WS)) == 0)
					{
						serverURI += strlen(URI_WS);
						command->client->websocket = 1;
					}
#if defined(OPENSSL)
					else if (strncmp(URI_SSL, serverURI, strlen(URI_SSL)) == 0)
					{
						serverURI += strlen(URI_SSL);
						command->client->ssl = 1;
					}
					else if (strncmp(URI_TLS, serverURI, strlen(URI_TLS)) == 0)
					{
						serverURI += strlen(URI_TLS);
						command->client->ssl = 1;
					}
					else if (strncmp(URI_MQTTS, serverURI, strlen(URI_MQTTS)) == 0)
					{
						serverURI += strlen(URI_MQTTS);
						command->client->ssl = 1;
					}
					else if (strncmp(URI_WSS, serverURI, strlen(URI_WSS)) == 0)
					{
						serverURI += strlen(URI_WSS);
						command->client->ssl = 1;
						command->client->websocket = 1;
					}
#endif
				}
			}

			if (command->client->c->MQTTVersion == MQTTVERSION_DEFAULT)
			{
				if (command->command.details.conn.MQTTVersion == MQTTVERSION_DEFAULT)
					command->command.details.conn.MQTTVersion = MQTTVERSION_3_1_1;
				else if (command->command.details.conn.MQTTVersion == MQTTVERSION_3_1_1)
					command->command.details.conn.MQTTVersion = MQTTVERSION_3_1;
			}
			else
				command->command.details.conn.MQTTVersion = command->client->c->MQTTVersion;

			Log(TRACE_PROTOCOL, -1, "Connecting to serverURI %s with MQTT version %d", serverURI, command->command.details.conn.MQTTVersion);
#if defined(OPENSSL)
#if defined(__GNUC__) && defined(__linux__)
			rc = MQTTProtocol_connect(serverURI, command->client->c, command->client->unixsock, command->client->ssl, command->client->websocket,
					command->command.details.conn.MQTTVersion, command->client->connectProps, command->client->willProps, 100);
#else
			rc = MQTTProtocol_connect(serverURI, command->client->c, command->client->unixsock, command->client->ssl, command->client->websocket,
					command->command.details.conn.MQTTVersion, command->client->connectProps, command->client->willProps);
#endif
#else
#if defined(__GNUC__) && defined(__linux__)
			rc = MQTTProtocol_connect(serverURI, command->client->c, command->client->unixsock, command->client->websocket,
					command->command.details.conn.MQTTVersion, command->client->connectProps, command->client->willProps, 100);
#else
			rc = MQTTProtocol_connect(serverURI, command->client->c, command->client->unixsock, command->client->websocket,
					command->command.details.conn.MQTTVersion, command->client->connectProps, command->client->willProps);
#endif
#endif

			if (command->client->c->connect_state == NOT_IN_PROGRESS)
				rc = SOCKET_ERROR;

			/* if the TCP connect is pending, then we must call select to determine when the connect has completed,
			which is indicated by the socket being ready *either* for reading *or* writing.  The next couple of lines
			make sure we check for writeability as well as readability, otherwise we wait around longer than we need to
			in Socket_getReadySocket() */
			if (rc == EINPROGRESS)
				Socket_addPendingWrite(command->client->c->net.socket);
		}
	}
	else if (command->command.type == SUBSCRIBE)
	{
		List* topics = ListInitialize();
		List* qoss = ListInitialize();
		MQTTProperties* props = NULL;
		MQTTSubscribe_options* subopts = NULL;
		int i;

		for (i = 0; i < command->command.details.sub.count; i++)
		{
			ListAppend(topics, command->command.details.sub.topics[i], strlen(command->command.details.sub.topics[i]));
			ListAppend(qoss, &command->command.details.sub.qoss[i], sizeof(int));
		}
		if (command->client->c->MQTTVersion >= MQTTVERSION_5)
		{
			props = &command->command.properties;
			if (command->command.details.sub.count > 1)
				subopts = command->command.details.sub.optlist;
			else
				subopts = &command->command.details.sub.opts;
		}
		rc = MQTTProtocol_subscribe(command->client->c, topics, qoss, command->command.token, subopts, props);
		ListFreeNoContent(topics);
		ListFreeNoContent(qoss);
		if (command->client->c->MQTTVersion >= MQTTVERSION_5 && command->command.details.sub.count > 1)
			free(command->command.details.sub.optlist);
	}
	else if (command->command.type == UNSUBSCRIBE)
	{
		List* topics = ListInitialize();
		MQTTProperties* props = NULL;
		int i;

		for (i = 0; i < command->command.details.unsub.count; i++)
			ListAppend(topics, command->command.details.unsub.topics[i], strlen(command->command.details.unsub.topics[i]));

		if (command->client->c->MQTTVersion >= MQTTVERSION_5)
			props = &command->command.properties;

		rc = MQTTProtocol_unsubscribe(command->client->c, topics, command->command.token, props);
		ListFreeNoContent(topics);
	}
	else if (command->command.type == PUBLISH)
	{
		Messages* msg = NULL;
		Publish* p = NULL;
		MQTTProperties initialized = MQTTProperties_initializer;

		if ((p = malloc(sizeof(Publish))) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}

		/* Initialize the mask */
		memset(p->mask, 0, sizeof(p->mask));

		p->payload = command->command.details.pub.payload;
		p->payloadlen = command->command.details.pub.payloadlen;
		p->topic = command->command.details.pub.destinationName;
		p->msgId = command->command.token;
		p->MQTTVersion = command->client->c->MQTTVersion;
		p->properties = initialized;
		if (p->MQTTVersion >= MQTTVERSION_5)
			p->properties = command->command.properties;

		rc = MQTTProtocol_startPublish(command->client->c, p, command->command.details.pub.qos, command->command.details.pub.retained, &msg);

		if (command->command.details.pub.qos == 0)
		{
			if (rc == TCPSOCKET_COMPLETE)
			{
				if (command->command.onSuccess)
				{
					MQTTAsync_successData data;

					data.token = command->command.token;
					data.alt.pub.destinationName = command->command.details.pub.destinationName;
					data.alt.pub.message.payload = command->command.details.pub.payload;
					data.alt.pub.message.payloadlen = command->command.details.pub.payloadlen;
					data.alt.pub.message.qos = command->command.details.pub.qos;
					data.alt.pub.message.retained = command->command.details.pub.retained;
					Log(TRACE_MIN, -1, "Calling publish success for client %s", command->client->c->clientID);
					(*(command->command.onSuccess))(command->command.context, &data);
				}
				else if (command->command.onSuccess5)
				{
					MQTTAsync_successData5 data = MQTTAsync_successData5_initializer;

					data.token = command->command.token;
					data.alt.pub.destinationName = command->command.details.pub.destinationName;
					data.alt.pub.message.payload = command->command.details.pub.payload;
					data.alt.pub.message.payloadlen = command->command.details.pub.payloadlen;
					data.alt.pub.message.qos = command->command.details.pub.qos;
					data.alt.pub.message.retained = command->command.details.pub.retained;
					data.properties = command->command.properties;
					Log(TRACE_MIN, -1, "Calling publish success for client %s", command->client->c->clientID);
					(*(command->command.onSuccess5))(command->command.context, &data);
				}
			}
			else
			{
				if (rc != SOCKET_ERROR)
				{
					command->command.details.pub.payload = NULL; /* this will be freed by the protocol code */
					command->command.details.pub.destinationName = NULL; /* this will be freed by the protocol code */
				}
				command->client->pending_write = &command->command;
			}
		}
		free(p); /* should this be done if the write isn't complete? */
	}
	else if (command->command.type == DISCONNECT)
	{
		if (command->client->c->connect_state != NOT_IN_PROGRESS || command->client->c->connected != 0)
		{
			if (command->client->c->connect_state != NOT_IN_PROGRESS)
			{
				if (command->client->connect.onFailure)
				{
					MQTTAsync_failureData data;

					data.token = 0;
					data.code = MQTTASYNC_OPERATION_INCOMPLETE;
					data.message = NULL;
					Log(TRACE_MIN, -1, "Calling connect failure for client %s", command->client->c->clientID);
					(*(command->client->connect.onFailure))(command->client->connect.context, &data);
					/* Null out callback pointers so they aren't accidentally called again */
					command->client->connect.onFailure = NULL;
					command->client->connect.onSuccess = NULL;
				}
				else if (command->client->connect.onFailure5)
				{
					MQTTAsync_failureData5 data;

					data.token = 0;
					data.code = MQTTASYNC_OPERATION_INCOMPLETE;
					data.message = NULL;
					Log(TRACE_MIN, -1, "Calling connect failure for client %s", command->client->c->clientID);
					(*(command->client->connect.onFailure5))(command->client->connect.context, &data);
					/* Null out callback pointers so they aren't accidentally called again */
					command->client->connect.onFailure5 = NULL;
					command->client->connect.onSuccess5 = NULL;
				}
			}
			command->client->c->connect_state = DISCONNECTING;
			MQTTAsync_checkDisconnect(command->client, &command->command);
		}
	}

	if (command->command.type == CONNECT && rc != SOCKET_ERROR && rc != MQTTASYNC_PERSISTENCE_ERROR)
	{
		command->client->connect = command->command;
		MQTTAsync_freeCommand(command);
	}
	else if (command->command.type == DISCONNECT)
	{
		command->client->disconnect = command->command;
		MQTTAsync_freeCommand(command);
	}
	else if (command->command.type == PUBLISH && command->command.details.pub.qos == 0 &&
			rc != SOCKET_ERROR && rc != MQTTASYNC_PERSISTENCE_ERROR)
	{
		if (rc == TCPSOCKET_INTERRUPTED)
			ListAppend(command->client->responses, command, sizeof(command));
		else
			MQTTAsync_freeCommand(command);
	}
	else if (rc == SOCKET_ERROR || rc == MQTTASYNC_PERSISTENCE_ERROR)
	{
		if (command->command.type == CONNECT)
		{
			MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
			MQTTAsync_disconnect(command->client, &opts); /* not "internal" because we don't want to call connection lost */
			command->client->shouldBeConnected = 1; /* as above call is not "internal" we need to reset this */
		}
		else
			MQTTAsync_disconnect_internal(command->client, 0);

		if (command->command.type == CONNECT
				&& MQTTAsync_checkConn(&command->command, command->client, 0))
		{
			Log(TRACE_MIN, -1, "Connect failed, more to try");

			if (command->client->c->MQTTVersion == MQTTVERSION_DEFAULT)
			{
				if (command->command.details.conn.MQTTVersion == MQTTVERSION_3_1)
				{
					command->command.details.conn.currentURI++;
					command->command.details.conn.MQTTVersion = 	MQTTVERSION_DEFAULT;
				}
			} else
				command->command.details.conn.currentURI++; /* Here currentURI becomes larger than command->client->serverURIcount. This needs to be handled to avoid segmentation faults! */

			/* put the connect command back to the head of the command queue, using the next serverURI */
			rc = MQTTAsync_addCommand(command,
					sizeof(command->command.details.conn));
		} else
		{
			if (command->command.onFailure)
			{
				MQTTAsync_failureData data;

				data.token = 0;
				data.code = rc;
				data.message = NULL;
				Log(TRACE_MIN, -1, "Calling command failure for client %s", command->client->c->clientID);
				(*(command->command.onFailure))(command->command.context, &data);
			}
			else if (command->command.onFailure5)
			{
				MQTTAsync_failureData5 data = MQTTAsync_failureData5_initializer;

				data.code = rc;
				Log(TRACE_MIN, -1, "Calling command failure for client %s", command->client->c->clientID);
				(*(command->command.onFailure5))(command->command.context, &data);
			}
			if (command->command.type == CONNECT)
			{
				command->client->connect = command->command;
				MQTTAsync_startConnectRetry(command->client);
			}
			MQTTAsync_freeCommand(command);  /* free up the command if necessary */
		}
	}
	else /* put the command into a waiting for response queue for each client, indexed by msgid */
		ListAppend(command->client->responses, command, sizeof(command));

exit:
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	rc = (command != NULL);
	FUNC_EXIT_RC(rc);
	return rc;
}


static void nextOrClose(MQTTAsyncs* m, int rc, char* message)
{
	int was_connected = m->c->connected;
	int more_to_try = 0;
	int connectionLost_called = 0;
	FUNC_ENTRY;

	more_to_try = MQTTAsync_checkConn(&m->connect, m, was_connected);
	if (more_to_try)
	{
		MQTTAsync_queuedCommand* conn;

		MQTTAsync_closeOnly(m->c, MQTTREASONCODE_SUCCESS, NULL);
		if (m->cl && was_connected)
		{
			Log(TRACE_MIN, -1, "Calling connectionLost for client %s", m->c->clientID);
				(*(m->cl))(m->clContext, NULL);
			connectionLost_called = 1;
		}
		/* put the connect command back to the head of the command queue, using the next serverURI */
		if ((conn = malloc(sizeof(MQTTAsync_queuedCommand))) == NULL)
			goto exit;
		memset(conn, '\0', sizeof(MQTTAsync_queuedCommand));
		conn->client = m;
		conn->command = m->connect;
		Log(TRACE_MIN, -1, "Connect failed, more to try");

		if (conn->client->c->MQTTVersion == MQTTVERSION_DEFAULT)
		{
			/* if last attempt successfully connected and we are using the DEFAULT option, don't fallback to MQTT 3.1 */
			if (was_connected == 0 || conn->command.details.conn.MQTTVersion == MQTTVERSION_3_1)
			{
				conn->command.details.conn.currentURI++;
				conn->command.details.conn.MQTTVersion = MQTTVERSION_DEFAULT;
			}
		}
		else
			conn->command.details.conn.currentURI++;

		if (MQTTAsync_addCommand(conn, sizeof(m->connect)) != MQTTASYNC_SUCCESS)
			more_to_try = 0; /* go into retry mode if CONNECT command add fails */
	}

	if (!more_to_try)
	{
		MQTTAsync_closeSession(m->c, MQTTREASONCODE_SUCCESS, NULL);
		if (m->connect.onFailure)
		{
			MQTTAsync_failureData data;

			data.token = 0;
			data.code = rc;
			data.message = message;
			Log(TRACE_MIN, -1, "Calling connect failure for client %s", m->c->clientID);
			(*(m->connect.onFailure))(m->connect.context, &data);
			/* Null out callback pointers so they aren't accidentally called again */
			m->connect.onFailure = NULL;
			m->connect.onSuccess = NULL;
		}
		else if (m->connect.onFailure5)
		{
			MQTTAsync_failureData5 data = MQTTAsync_failureData5_initializer;

			data.token = 0;
			if (rc > 0) /* MQTT Reason Codes are > 0; C client return codes are < 0 */
			{
				/* MQTT 5 reason codes >= 0x00 and < 0x80 are successful,
				 * but in that case we should not get here but be calling
				 * onSuccess instead. */
				data.reasonCode = rc;
				data.code = MQTTASYNC_FAILURE;
			} else
				data.code = rc;
			data.message = message;
			Log(TRACE_MIN, -1, "Calling connect failure for client %s", m->c->clientID);
			(*(m->connect.onFailure5))(m->connect.context, &data);
			/* Null out callback pointers so they aren't accidentally called again */
			m->connect.onFailure5 = NULL;
			m->connect.onSuccess5 = NULL;
		}
		if (connectionLost_called == 0 && m->cl && was_connected)
		{
			Log(TRACE_MIN, -1, "Calling connectionLost for client %s", m->c->clientID);
				(*(m->cl))(m->clContext, NULL);
		}
		MQTTAsync_startConnectRetry(m);
	}
exit:
	FUNC_EXIT;
}


static void MQTTAsync_checkTimeouts(void)
{
	ListElement* current = NULL;
	static START_TIME_TYPE last = START_TIME_ZERO;
	START_TIME_TYPE now;

	FUNC_ENTRY;
	MQTTAsync_lock_mutex(mqttasync_mutex);
	now = MQTTTime_now();
	if (MQTTTime_difftime(now, last) < (DIFF_TIME_TYPE)3000)
		goto exit;
	last = now;
	while (ListNextElement(MQTTAsync_handles, &current))		/* for each client */
	{
		MQTTAsyncs* m = (MQTTAsyncs*)(current->content);

		/* check disconnect timeout */
		if (m->c->connect_state == DISCONNECTING)
			MQTTAsync_checkDisconnect(m, &m->disconnect);

		/* check connect timeout */
		if (m->c->connect_state != NOT_IN_PROGRESS && MQTTTime_elapsed(m->connect.start_time) > (ELAPSED_TIME_TYPE)(m->connectTimeout * 1000))
		{
			nextOrClose(m, MQTTASYNC_FAILURE, "TCP connect timeout");
			continue;
		}

		/* There was a section here that removed timed-out responses.  But if the command had completed and
		 * there was a response, then we may as well report it, no?
		 *
		 * In any case, that section was disabled when automatic reconnect was implemented.
		 */

		if (m->automaticReconnect && m->retrying)
		{
			if (m->reconnectNow || MQTTTime_elapsed(m->lastConnectionFailedTime) > (ELAPSED_TIME_TYPE)(m->currentInterval * 1000))
			{
				/* to reconnect put the connect command to the head of the command queue */
				MQTTAsync_queuedCommand* conn = malloc(sizeof(MQTTAsync_queuedCommand));
				if (!conn)
					goto exit;
				memset(conn, '\0', sizeof(MQTTAsync_queuedCommand));
				conn->client = m;
				conn->command = m->connect;
	  			/* make sure that the version attempts are restarted */
				if (m->c->MQTTVersion == MQTTVERSION_DEFAULT)
					conn->command.details.conn.MQTTVersion = 0;
				if (m->updateConnectOptions)
				{
					MQTTAsync_connectData connectData = MQTTAsync_connectData_initializer;
					int callback_rc = MQTTASYNC_SUCCESS;

					connectData.username = m->c->username;
					connectData.binarypwd.data = m->c->password;
					connectData.binarypwd.len = m->c->passwordlen;
					Log(TRACE_MIN, -1, "Calling updateConnectOptions for client %s", m->c->clientID);
					callback_rc = (*(m->updateConnectOptions))(m->updateConnectOptions_context, &connectData);

					if (callback_rc)
					{
						if (connectData.username != m->c->username)
						{
							if (m->c->username)
								free((void*)m->c->username);
							if (connectData.username)
								m->c->username = connectData.username; /* must be allocated by MQTTAsync_malloc in the callback */
							else
								m->c->username = NULL;
						}
						if (connectData.binarypwd.data != m->c->password)
						{
							if (m->c->password)
								free((void*)m->c->password);
							if (connectData.binarypwd.data)
							{
								m->c->passwordlen = connectData.binarypwd.len;
								m->c->password = connectData.binarypwd.data; /* must be allocated by MQTTAsync_malloc in the callback */
							}
							else
							{
								m->c->password = NULL;
								m->c->passwordlen = 0;
							}
						}
					}
				}
				Log(TRACE_MIN, -1, "Automatically attempting to reconnect");
				MQTTAsync_addCommand(conn, sizeof(m->connect));
				m->reconnectNow = 0;
			}
		}
	}
exit:
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT;
}


thread_return_type WINAPI MQTTAsync_sendThread(void* n)
{
	int timeout = 10; /* first time in we have a small timeout.  Gets things started more quickly */

	FUNC_ENTRY;
	Thread_set_name("MQTTAsync_send");
	MQTTAsync_lock_mutex(mqttasync_mutex);
	sendThread_state = RUNNING;
	sendThread_id = Paho_thread_getid();
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	while (!MQTTAsync_tostop)
	{
		int rc;
		int command_count = 0;

		MQTTAsync_lock_mutex(mqttcommand_mutex);
		command_count = MQTTAsync_commands->count;
		MQTTAsync_unlock_mutex(mqttcommand_mutex);
		while (command_count > 0)
		{
			if (MQTTAsync_processCommand() == 0)
				break;  /* no commands were processed, so go into a wait */
			MQTTAsync_lock_mutex(mqttcommand_mutex);
			command_count = MQTTAsync_commands->count;
			MQTTAsync_unlock_mutex(mqttcommand_mutex);
		}
		if ((rc = Thread_wait_evt(send_evt, timeout)) != 0 && rc != ETIMEDOUT)
			Log(LOG_ERROR, -1, "Error %d waiting for send event", rc);
		timeout = 1000; /* 1 second for follow on waits */
		MQTTAsync_checkTimeouts();
	}
	sendThread_state = STOPPING;
	MQTTAsync_lock_mutex(mqttasync_mutex);
	sendThread_state = STOPPED;
	sendThread_id = 0;
	MQTTAsync_unlock_mutex(mqttasync_mutex);

#if defined(OPENSSL)
#if ((OPENSSL_VERSION_NUMBER < 0x1010000fL) || defined(LIBRESSL_VERSION_NUMBER)) || defined(OPENSSL_IS_BORINGSSL)
	ERR_remove_state(0);
#else
	OPENSSL_thread_stop();
#endif
#endif

	FUNC_EXIT;
#if defined(_WIN32)
	ExitThread(0);
#endif
	return 0;
}


void MQTTAsync_emptyMessageQueue(Clients* client)
{
	FUNC_ENTRY;
	/* empty message queue */
	if (client->messageQueue->count > 0)
	{
		ListElement* current = NULL;
		while (ListNextElement(client->messageQueue, &current))
		{
			qEntry* qe = (qEntry*)(current->content);
			free(qe->topicName);
			free(qe->msg->payload);
			free(qe->msg);
		}
		ListEmpty(client->messageQueue);
	}
	FUNC_EXIT;
}


void MQTTAsync_freeResponses(MQTTAsyncs* m)
{
	int count = 0;

	FUNC_ENTRY;
	if (m->responses)
	{
		ListElement* cur_response = NULL;

		while (ListNextElement(m->responses, &cur_response))
		{
			MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(cur_response->content);

			if (command->command.onFailure)
			{
				MQTTAsync_failureData data;

				data.token = command->command.token;
				data.code = MQTTASYNC_OPERATION_INCOMPLETE; /* interrupted return code */
				data.message = NULL;

				Log(TRACE_MIN, -1, "Calling %s failure for client %s",
						MQTTPacket_name(command->command.type), m->c->clientID);
				(*(command->command.onFailure))(command->command.context, &data);
			}
			else if (command->command.onFailure5)
			{
				MQTTAsync_failureData5 data = MQTTAsync_failureData5_initializer;

				data.token = command->command.token;
				data.code = MQTTASYNC_OPERATION_INCOMPLETE; /* interrupted return code */
				data.message = NULL;

				Log(TRACE_MIN, -1, "Calling %s failure for client %s",
						MQTTPacket_name(command->command.type), m->c->clientID);
				(*(command->command.onFailure5))(command->command.context, &data);
			}

			MQTTAsync_freeCommand1(command);
			count++;
		}
		ListEmpty(m->responses);
	}
	Log(TRACE_MINIMUM, -1, "%d responses removed for client %s", count, m->c->clientID);
	FUNC_EXIT;
}


void MQTTAsync_freeCommands(MQTTAsyncs* m)
{
	int count = 0;
	ListElement* current = NULL;
	ListElement *next = NULL;

	FUNC_ENTRY;
	/* remove commands in the command queue relating to this client */
	current = ListNextElement(MQTTAsync_commands, &next);
	ListNextElement(MQTTAsync_commands, &next);
	while (current)
	{
		MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(current->content);

		if (command->client == m)
		{
			ListDetach(MQTTAsync_commands, command);

			if (command->command.onFailure)
			{
				MQTTAsync_failureData data;

				data.token = command->command.token;
				data.code = MQTTASYNC_OPERATION_INCOMPLETE; /* interrupted return code */
				data.message = NULL;

				Log(TRACE_MIN, -1, "Calling %s failure for client %s",
							MQTTPacket_name(command->command.type), m->c->clientID);
					(*(command->command.onFailure))(command->command.context, &data);
			}
			else if (command->command.onFailure5)
			{
				MQTTAsync_failureData5 data = MQTTAsync_failureData5_initializer;

				data.token = command->command.token;
				data.code = MQTTASYNC_OPERATION_INCOMPLETE; /* interrupted return code */
				data.message = NULL;

				Log(TRACE_MIN, -1, "Calling %s failure for client %s",
							MQTTPacket_name(command->command.type), m->c->clientID);
					(*(command->command.onFailure5))(command->command.context, &data);
			}

			MQTTAsync_freeCommand(command);
			count++;
		}
		current = next;
		ListNextElement(MQTTAsync_commands, &next);
	}
	Log(TRACE_MINIMUM, -1, "%d commands removed for client %s", count, m->c->clientID);
	FUNC_EXIT;
}


static int MQTTAsync_completeConnection(MQTTAsyncs* m, Connack* connack)
{
	int rc = MQTTASYNC_FAILURE;

	FUNC_ENTRY;
	if (m->c->connect_state == WAIT_FOR_CONNACK) /* MQTT connect sent - wait for CONNACK */
	{
		Log(LOG_PROTOCOL, 1, NULL, m->c->net.socket, m->c->clientID, connack->rc);
		if ((rc = connack->rc) == MQTTASYNC_SUCCESS)
		{
			m->retrying = 0;
			m->c->connected = 1;
			m->c->good = 1;
			m->c->connect_state = NOT_IN_PROGRESS;
			if (m->c->cleansession || m->c->cleanstart)
				rc = MQTTAsync_cleanSession(m->c);
			else if (m->c->MQTTVersion >= MQTTVERSION_3_1_1 && connack->flags.bits.sessionPresent == 0)
			{
				Log(LOG_PROTOCOL, -1, "Cleaning session state on connect because sessionPresent is 0");
				rc = MQTTAsync_cleanSession(m->c);
			}
			if (m->c->outboundMsgs->count > 0)
			{
				ListElement* outcurrent = NULL;
				START_TIME_TYPE zero = START_TIME_ZERO;

				while (ListNextElement(m->c->outboundMsgs, &outcurrent))
				{
					Messages* messages = (Messages*)(outcurrent->content);
					memset(&messages->lastTouch, '\0', sizeof(messages->lastTouch));
				}
				MQTTProtocol_retry(zero, 1, 1);
				if (m->c->connected != 1)
					rc = MQTTASYNC_DISCONNECTED;
			}
			if (m->c->MQTTVersion == MQTTVERSION_5)
			{
				if (MQTTProperties_hasProperty(&connack->properties, MQTTPROPERTY_CODE_SERVER_KEEP_ALIVE))
				{
					/* update the keep alive from the server keep alive */
					int server_keep_alive = (int)MQTTProperties_getNumericValue(&connack->properties, MQTTPROPERTY_CODE_SERVER_KEEP_ALIVE);
					if (server_keep_alive != -999999)
					{
						Log(LOG_PROTOCOL, -1, "Setting keep alive interval to server keep alive %d", server_keep_alive);
						m->c->keepAliveInterval = server_keep_alive;
					}
				}
				else if (m->c->keepAliveInterval != m->c->savedKeepAliveInterval)
				{
					/* if the keep alive has been previously updated with a server keep alive, but there is no server keep alive
					on this connect, reset it to the value requested in the original connect API */
					Log(LOG_PROTOCOL, -1, "Resetting keep alive interval to %d", m->c->savedKeepAliveInterval);
					m->c->keepAliveInterval = m->c->savedKeepAliveInterval;
				}
			}
		}
		m->pack = NULL;
		Thread_signal_evt(send_evt);
	}
	FUNC_EXIT_RC(rc);
	return rc;
}


/* This is the thread function that handles the calling of callback functions if set */
thread_return_type WINAPI MQTTAsync_receiveThread(void* n)
{
	long timeout = 10L; /* first time in we have a small timeout.  Gets things started more quickly */

	FUNC_ENTRY;
	Thread_set_name("MQTTAsync_rcv");
	MQTTAsync_lock_mutex(mqttasync_mutex);
	receiveThread_state = RUNNING;
	receiveThread_id = Paho_thread_getid();
	while (!MQTTAsync_tostop)
	{
		int rc = SOCKET_ERROR;
		SOCKET sock = -1;
		MQTTAsyncs* m = NULL;
		MQTTPacket* pack = NULL;

		MQTTAsync_unlock_mutex(mqttasync_mutex);
		Log(TRACE_MINIMUM, -1, "MQTTAsync_cycle call timeout %lu", timeout);
		pack = MQTTAsync_cycle(&sock, timeout, &rc);
		Log(TRACE_MINIMUM, -1, "MQTTAsync_cycle returns pack %p, sock %d, timeout %lu, rc %d", pack, sock, timeout, rc);
		MQTTAsync_lock_mutex(mqttasync_mutex);
		if (MQTTAsync_tostop)
			break;

		if (sock == 0)
			continue;
		timeout = 1000L;

		/* find client corresponding to socket */
		if (ListFindItem(MQTTAsync_handles, &sock, clientSockCompare) == NULL)
		{
			Log(TRACE_MINIMUM, -1, "Could not find client corresponding to socket %d", sock);
			/* Socket_close(sock); - removing socket in this case is not necessary (Bug 442400) */
			continue;
		}
		m = (MQTTAsyncs*)(MQTTAsync_handles->current->content);
		if (m == NULL)
		{
			Log(LOG_ERROR, -1, "Client structure was NULL for socket %d - removing socket", sock);
			Socket_close(sock);
			continue;
		}
		if (rc == SOCKET_ERROR)
		{
			Log(TRACE_MINIMUM, -1, "Error from MQTTAsync_cycle() - removing socket %d", sock);
			nextOrClose(m, rc, "socket error");
		}
		else
		{
			if (m->c->messageQueue->count > 0 && m->ma)
			{
				qEntry* qe = (qEntry*)(m->c->messageQueue->first->content);
				int topicLen = qe->topicLen;

				if (strlen(qe->topicName) == topicLen)
					topicLen = 0;

				if (MQTTAsync_deliverMessage(m, qe->topicName, topicLen, qe->msg))
				{
#if !defined(NO_PERSISTENCE)
					if (m->c->persistence)
						MQTTPersistence_unpersistQueueEntry(m->c, (MQTTPersistence_qEntry*)qe);
#endif
					ListRemove(m->c->messageQueue, qe); /* qe is freed here */
				}
				else
					Log(TRACE_MIN, -1, "False returned from messageArrived for client %s, message remains on queue",
						m->c->clientID);
			}
			if (pack)
			{
				if (pack->header.bits.type == CONNACK)
				{
					Connack* connack = (Connack*)pack;
					int sessionPresent = connack->flags.bits.sessionPresent;

					rc = MQTTAsync_completeConnection(m, connack);
					if (rc == MQTTASYNC_SUCCESS)
					{
						int onSuccess = 0;
						if ((m->serverURIcount > 0)
						    && (m->connect.details.conn.currentURI < m->serverURIcount))
						{
							Log(TRACE_MIN, -1, "Connect succeeded to %s",
								m->serverURIs[m->connect.details.conn.currentURI]);
						}
						onSuccess = (m->connect.onSuccess != NULL ||
								m->connect.onSuccess5 != NULL); /* save setting of onSuccess callback */
						if (m->connect.onSuccess)
						{
							MQTTAsync_successData data;
							memset(&data, '\0', sizeof(data));
							Log(TRACE_MIN, -1, "Calling connect success for client %s", m->c->clientID);
							if ((m->serverURIcount > 0)
							    && (m->connect.details.conn.currentURI < m->serverURIcount))
								data.alt.connect.serverURI = m->serverURIs[m->connect.details.conn.currentURI];
							else
								data.alt.connect.serverURI = m->serverURI;
							data.alt.connect.MQTTVersion = m->connect.details.conn.MQTTVersion;
							data.alt.connect.sessionPresent = sessionPresent;
							(*(m->connect.onSuccess))(m->connect.context, &data);
							/* Null out callback pointers so they aren't accidentally called again */
							m->connect.onSuccess = NULL;
							m->connect.onFailure = NULL;
						}
						else if (m->connect.onSuccess5)
						{
							MQTTAsync_successData5 data = MQTTAsync_successData5_initializer;
							Log(TRACE_MIN, -1, "Calling connect success for client %s", m->c->clientID);
							if (m->serverURIcount > 0)
								data.alt.connect.serverURI = m->serverURIs[m->connect.details.conn.currentURI];
							else
								data.alt.connect.serverURI = m->serverURI;
							data.alt.connect.MQTTVersion = m->connect.details.conn.MQTTVersion;
							data.alt.connect.sessionPresent = sessionPresent;
							data.properties = connack->properties;
							data.reasonCode = connack->rc;
							(*(m->connect.onSuccess5))(m->connect.context, &data);
							/* Null out callback pointers so they aren't accidentally called again */
							m->connect.onSuccess5 = NULL;
							m->connect.onFailure5 = NULL;
						}
						if (m->connected)
						{
							char* reason = (onSuccess) ? "connect onSuccess called" : "automatic reconnect";
							Log(TRACE_MIN, -1, "Calling connected for client %s", m->c->clientID);
							(*(m->connected))(m->connected_context, reason);
						}
						if (m->c->MQTTVersion >= MQTTVERSION_5)
						{
							if (MQTTProperties_hasProperty(&connack->properties, MQTTPROPERTY_CODE_RECEIVE_MAXIMUM))
							{
								int recv_max = (int)MQTTProperties_getNumericValue(&connack->properties, MQTTPROPERTY_CODE_RECEIVE_MAXIMUM);
								if (m->c->maxInflightMessages > recv_max)
									m->c->maxInflightMessages = recv_max;
							}
						}
					}
					else
					{
					    nextOrClose(m, rc, "CONNACK return code");
					}
					MQTTPacket_freeConnack(connack);
				}
				else if (pack->header.bits.type == SUBACK)
				{
					ListElement* current = NULL;

					/* use the msgid to find the callback to be called */
					while (ListNextElement(m->responses, &current))
					{
						MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(current->content);
						if (command->command.token == ((Suback*)pack)->msgId)
						{
							Suback* sub = (Suback*)pack;
							if (!ListDetach(m->responses, command)) /* remove the response from the list */
								Log(LOG_ERROR, -1, "Subscribe command not removed from command list");

							/* Call the failure callback if there is one subscribe in the MQTT packet and
							 * the return code is 0x80 (failure).  If the MQTT packet contains >1 subscription
							 * request, then we call onSuccess with the list of returned QoSs, which inelegantly,
							 * could include some failures, or worse, the whole list could have failed.
							 */
							if (m->c->MQTTVersion >= MQTTVERSION_5)
							{
								if (sub->qoss->count == 1 && *(int*)(sub->qoss->first->content) >= MQTTREASONCODE_UNSPECIFIED_ERROR)
								{
									if (command->command.onFailure5)
									{
										MQTTAsync_failureData5 data = MQTTAsync_failureData5_initializer;

										data.token = command->command.token;
										data.reasonCode = *(int*)(sub->qoss->first->content);
										data.message = NULL;
										data.properties = sub->properties;
										Log(TRACE_MIN, -1, "Calling subscribe failure for client %s", m->c->clientID);
										(*(command->command.onFailure5))(command->command.context, &data);
									}
								}
								else if (command->command.onSuccess5)
								{
									MQTTAsync_successData5 data;
									enum MQTTReasonCodes* array = NULL;

									data.reasonCode = *(int*)(sub->qoss->first->content);
									data.alt.sub.reasonCodeCount = sub->qoss->count;
									if (sub->qoss->count > 1)
									{
										ListElement* cur_qos = NULL;
										enum MQTTReasonCodes* element = array = data.alt.sub.reasonCodes = malloc(sub->qoss->count * sizeof(enum MQTTReasonCodes));
										if (array)
											while (ListNextElement(sub->qoss, &cur_qos))
												*element++ = *(int*)(cur_qos->content);
									}
									data.token = command->command.token;
									data.properties = sub->properties;
									Log(TRACE_MIN, -1, "Calling subscribe success for client %s", m->c->clientID);
									(*(command->command.onSuccess5))(command->command.context, &data);
									if (array)
										free(array);
								}
							}
							else if (sub->qoss->count == 1 && *(int*)(sub->qoss->first->content) == MQTT_BAD_SUBSCRIBE)
							{
								if (command->command.onFailure)
								{
									MQTTAsync_failureData data;

									data.token = command->command.token;
									data.code = *(int*)(sub->qoss->first->content);
									data.message = NULL;
									Log(TRACE_MIN, -1, "Calling subscribe failure for client %s", m->c->clientID);
									(*(command->command.onFailure))(command->command.context, &data);
								}
							}
							else if (command->command.onSuccess)
							{
								MQTTAsync_successData data;
								int* array = NULL;

								if (sub->qoss->count == 1)
									data.alt.qos = *(int*)(sub->qoss->first->content);
								else if (sub->qoss->count > 1)
								{
									ListElement* cur_qos = NULL;
									int* element = array = data.alt.qosList = malloc(sub->qoss->count * sizeof(int));
									if (array)
										while (ListNextElement(sub->qoss, &cur_qos))
											*element++ = *(int*)(cur_qos->content);
								}
								data.token = command->command.token;
								Log(TRACE_MIN, -1, "Calling subscribe success for client %s", m->c->clientID);
								(*(command->command.onSuccess))(command->command.context, &data);
								if (array)
									free(array);
							}
							MQTTAsync_freeCommand(command);
							break;
						}
					}
					rc = MQTTProtocol_handleSubacks(pack, m->c->net.socket);
				}
				else if (pack->header.bits.type == UNSUBACK)
				{
					ListElement* current = NULL;
					Unsuback* unsub = (Unsuback*)pack;

					/* use the msgid to find the callback to be called */
					while (ListNextElement(m->responses, &current))
					{
						MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(current->content);
						if (command->command.token == ((Unsuback*)pack)->msgId)
						{
							if (!ListDetach(m->responses, command)) /* remove the response from the list */
								Log(LOG_ERROR, -1, "Unsubscribe command not removed from command list");
							if (command->command.onSuccess || command->command.onSuccess5)
							{
								Log(TRACE_MIN, -1, "Calling unsubscribe success for client %s", m->c->clientID);
								if (command->command.onSuccess)
								{
									MQTTAsync_successData data;

									memset(&data, '\0', sizeof(data));
									data.token = command->command.token;
									(*(command->command.onSuccess))(command->command.context, &data);
								}
								else
								{
									MQTTAsync_successData5 data = MQTTAsync_successData5_initializer;
									enum MQTTReasonCodes* array = NULL;

									data.reasonCode = *(enum MQTTReasonCodes*)(unsub->reasonCodes->first->content);
									data.alt.unsub.reasonCodeCount = unsub->reasonCodes->count;
									if (unsub->reasonCodes->count > 1)
									{
										ListElement* cur_rc = NULL;
										enum MQTTReasonCodes* element = array = data.alt.unsub.reasonCodes = malloc(unsub->reasonCodes->count * sizeof(enum MQTTReasonCodes));
										if (array)
											while (ListNextElement(unsub->reasonCodes, &cur_rc))
												*element++ = *(enum MQTTReasonCodes*)(cur_rc->content);
									}
									data.token = command->command.token;
									data.properties = unsub->properties;
									Log(TRACE_MIN, -1, "Calling unsubscribe success for client %s", m->c->clientID);
									(*(command->command.onSuccess5))(command->command.context, &data);
									if (array)
										free(array);
								}
							}
							MQTTAsync_freeCommand(command);
							break;
						}
					}
					rc = MQTTProtocol_handleUnsubacks(pack, m->c->net.socket);
				}
				else if (pack->header.bits.type == DISCONNECT)
				{
					Ack* disc = (Ack*)pack;
					int discrc = 0;

					discrc = disc->rc;
					if (m->disconnected)
					{
						Log(TRACE_MIN, -1, "Calling disconnected for client %s", m->c->clientID);
						(*(m->disconnected))(m->disconnected_context, &disc->properties, disc->rc);
					}
					rc = MQTTProtocol_handleDisconnects(pack, m->c->net.socket);
					m->c->connected = 0; /* don't send disconnect packet back */
					nextOrClose(m, discrc, "Received disconnect");
				}
				else
				{
					Log(LOG_ERROR, -1, "An unexpected packet type %u has been received", pack->header.bits.type);
				}
			}
		}
	}
	receiveThread_state = STOPPED;
	receiveThread_id = 0;
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	if (sendThread_state != STOPPED)
		Thread_signal_evt(send_evt);

#if defined(OPENSSL)
#if ((OPENSSL_VERSION_NUMBER < 0x1010000fL) || defined(LIBRESSL_VERSION_NUMBER)) || defined(OPENSSL_IS_BORINGSSL)
	ERR_remove_state(0);
#else
	OPENSSL_thread_stop();
#endif
#endif

	FUNC_EXIT;
#if defined(_WIN32)
	ExitThread(0);
#endif
	return 0;
}


static void MQTTAsync_stop(void)
{
#if !defined(NOSTACKTRACE)
	int rc = 0;
#endif

	FUNC_ENTRY;
	if (sendThread_state != STOPPED || receiveThread_state != STOPPED)
	{
		int conn_count = 0;
		ListElement* current = NULL;

		if (MQTTAsync_handles != NULL)
		{
			/* find out how many handles are still connected */
			while (ListNextElement(MQTTAsync_handles, &current))
			{
				if (((MQTTAsyncs*)(current->content))->c->connect_state > NOT_IN_PROGRESS ||
						((MQTTAsyncs*)(current->content))->c->connected)
					++conn_count;
			}
		}
		Log(TRACE_MIN, -1, "Conn_count is %d", conn_count);
		/* stop the background thread, if we are the last one to be using it */
		if (conn_count == 0)
		{
			int count = 0;
			MQTTAsync_tostop = 1;
			while ((sendThread_state != STOPPED || receiveThread_state != STOPPED) && MQTTAsync_tostop != 0 && ++count < 100)
			{
				MQTTAsync_unlock_mutex(mqttasync_mutex);
				Log(TRACE_MIN, -1, "sleeping");
				MQTTAsync_sleep(100L);
				MQTTAsync_lock_mutex(mqttasync_mutex);
			}
#if !defined(NOSTACKTRACE)
			rc = 1;
#endif
			MQTTAsync_tostop = 0;
		}
	}
	FUNC_EXIT_RC(rc);
}


static void MQTTAsync_closeOnly(Clients* client, enum MQTTReasonCodes reasonCode, MQTTProperties* props)
{
	FUNC_ENTRY;
	client->good = 0;
	client->ping_outstanding = 0;
	client->ping_due = 0;
	if (client->net.socket > 0)
	{
		MQTTProtocol_checkPendingWrites();
		if (client->connected && MQTTAsync_Socket_noPendingWrites(client->net.socket))
			MQTTPacket_send_disconnect(client, reasonCode, props);
		MQTTAsync_lock_mutex(socket_mutex);
		WebSocket_close(&client->net, WebSocket_CLOSE_NORMAL, NULL);
#if defined(OPENSSL)
		SSL_SESSION_free(client->session); /* is a no-op if session is NULL */
		client->session = NULL; /* show the session has been freed */
		SSLSocket_close(&client->net);
#endif
		MQTTAsync_unlock_mutex(socket_mutex);
		Socket_close(client->net.socket); /* Socket_close locks socket mutex itself */
		client->net.socket = 0;
#if defined(OPENSSL)
		client->net.ssl = NULL;
#endif
	}
	client->connected = 0;
	client->connect_state = NOT_IN_PROGRESS;
	FUNC_EXIT;
}


void MQTTAsync_closeSession(Clients* client, enum MQTTReasonCodes reasonCode, MQTTProperties* props)
{
	FUNC_ENTRY;
	MQTTAsync_closeOnly(client, reasonCode, props);

	if (client->cleansession ||
			(client->MQTTVersion >= MQTTVERSION_5 && client->sessionExpiry == 0))
		MQTTAsync_cleanSession(client);

	FUNC_EXIT;
}


/**
 * List callback function for comparing clients by client structure
 * @param a Async structure
 * @param b Client structure
 * @return boolean indicating whether a and b are equal
 */
static int clientStructCompare(void* a, void* b)
{
	MQTTAsyncs* m = (MQTTAsyncs*)a;
	return m->c == (Clients*)b;
}


/*
 * Set destinationName and payload to NULL in all responses
 * for a client, so that these memory locations aren't freed twice as they
 * are also stored by MQTTProtocol_storePublication.
 * @param m the client to process
 */
void MQTTAsync_NULLPublishResponses(MQTTAsyncs* m)
{
	FUNC_ENTRY;
	if (m->responses)
	{
		ListElement* cur_response = NULL;

		while (ListNextElement(m->responses, &cur_response))
		{
			MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(cur_response->content);
			if (command->command.type == PUBLISH)
			{
				/* these values are going to be freed in RemovePublication */
				command->command.details.pub.destinationName = NULL;
				command->command.details.pub.payload = NULL;
			}
		}
	}
	FUNC_EXIT;
}


#if 0 /* removed as part of fix for issue 1474 */
/*
 * Set destinationName and payload to NULL in all commands
 * for a client, so that these memory locations aren't freed twice as they
 * are also stored by MQTTProtocol_storePublication.
 * @param m the client to process
 */
void MQTTAsync_NULLPublishCommands(MQTTAsyncs* m)
{
	ListElement* current = NULL;
	ListElement *next = NULL;

	FUNC_ENTRY;
	current = ListNextElement(MQTTAsync_commands, &next);
	ListNextElement(MQTTAsync_commands, &next);
	while (current)
	{
		MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(current->content);

		if (command->client == m && command->command.type == PUBLISH)
		{
			/* these values are going to be freed in RemovePublication */
			command->command.details.pub.destinationName = NULL;
			command->command.details.pub.payload = NULL;
		}
		current = next;
		ListNextElement(MQTTAsync_commands, &next);
	}
	FUNC_EXIT;
}
#endif


/**
 * Clean the MQTT session data.  This includes the MQTT inflight messages, because
 * that is part of the MQTT state that will be cleared by the MQTT broker too.
 * However, queued up messages, outgoing or incoming, need (should?) not be cleared
 * as they are outside the scope of the MQTT session.
 */
static int MQTTAsync_cleanSession(Clients* client)
{
	int rc = 0;
	ListElement* found = NULL;

	FUNC_ENTRY;
#if !defined(NO_PERSISTENCE)
	rc = MQTTAsync_unpersistInflightMessages(client);
#endif
	MQTTProtocol_emptyMessageList(client->inboundMsgs);
	MQTTProtocol_emptyMessageList(client->outboundMsgs);
	client->msgID = 0;
	if ((found = ListFindItem(MQTTAsync_handles, client, clientStructCompare)) != NULL)
	{
		MQTTAsyncs* m = (MQTTAsyncs*)(found->content);
		MQTTAsync_NULLPublishResponses(m);
		MQTTAsync_freeResponses(m);
	}
	else
		Log(LOG_ERROR, -1, "cleanSession: did not find client structure in handles list");
	FUNC_EXIT_RC(rc);
	return rc;
}


/*
* Deliver a message to the messageArrived callback
* @param m a client structure
* @param topicName the name of the topic on which the message is being delivered
* @param topicLen the length of the topic name string
* @param mm the message to be delivered
* @return boolean 1 means message has been delivered, 0 that it has not
*/
static int MQTTAsync_deliverMessage(MQTTAsyncs* m, char* topicName, size_t topicLen, MQTTAsync_message* mm)
{
	int rc;

	Log(TRACE_MIN, -1, "Calling messageArrived for client %s, queue depth %d",
					m->c->clientID, m->c->messageQueue->count);
	rc = (*(m->ma))(m->maContext, topicName, (int)topicLen, mm);
	/* if 0 (false) is returned by the callback then it failed, so we don't remove the message from
	 * the queue, and it will be retried later.  If 1 is returned then the message data may have been freed,
	 * so we must be careful how we use it.
	 */
	return rc;
}


void Protocol_processPublication(Publish* publish, Clients* client, int allocatePayload)
{
	MQTTAsync_message* mm = NULL;
	MQTTAsync_message initialized = MQTTAsync_message_initializer;
	int rc = 0;

	FUNC_ENTRY;
	if ((mm = malloc(sizeof(MQTTAsync_message))) == NULL)
		goto exit;
	memcpy(mm, &initialized, sizeof(MQTTAsync_message));

	if (allocatePayload)
	{
		if ((mm->payload = malloc(publish->payloadlen)) == NULL)
		{
			free(mm);
			goto exit;
		}
		memcpy(mm->payload, publish->payload, publish->payloadlen);
	} else
		mm->payload = publish->payload;
	mm->payloadlen = publish->payloadlen;
	mm->qos = publish->header.bits.qos;
	mm->retained = publish->header.bits.retain;
	if (publish->header.bits.qos == 2)
		mm->dup = 0;  /* ensure that a QoS2 message is not passed to the application with dup = 1 */
	else
		mm->dup = publish->header.bits.dup;
	mm->msgid = publish->msgId;

	if (publish->MQTTVersion >= MQTTVERSION_5)
		mm->properties = MQTTProperties_copy(&publish->properties);

	if (client->messageQueue->count == 0 && client->connected)
	{
		ListElement* found = NULL;

		if ((found = ListFindItem(MQTTAsync_handles, client, clientStructCompare)) == NULL)
			Log(LOG_ERROR, -1, "processPublication: did not find client structure in handles list");
		else
		{
			MQTTAsyncs* m = (MQTTAsyncs*)(found->content);

			if (m->ma)
				rc = MQTTAsync_deliverMessage(m, publish->topic, publish->topiclen, mm);
			else
				Log(LOG_ERROR, -1, "Message arrived for client %s but can't deliver it. No messageArrived callback",
						m->c->clientID);
		}
	}

	if (rc == 0) /* if message was not delivered, queue it up */
	{
		qEntry* qe = malloc(sizeof(qEntry));

		if (!qe)
			goto exit;
		qe->msg = mm;
		qe->topicName = publish->topic;
		qe->topicLen = publish->topiclen;
		ListAppend(client->messageQueue, qe, sizeof(qe) + sizeof(mm) + mm->payloadlen + strlen(qe->topicName)+1);
#if !defined(NO_PERSISTENCE)
		if (client->persistence)
			MQTTPersistence_persistQueueEntry(client, (MQTTPersistence_qEntry*)qe);
#endif
	}
exit:
	publish->topic = NULL;
	FUNC_EXIT;
}


static int retryLoopIntervalms = 5000;

void setRetryLoopInterval(int keepalive)
{
	retryLoopIntervalms = (keepalive*1000) / 10;

	if (retryLoopIntervalms < 100)
		retryLoopIntervalms = 100;
	else if (retryLoopIntervalms  > 5000)
		retryLoopIntervalms = 5000;
}


int MQTTAsync_disconnect1(MQTTAsync handle, const MQTTAsync_disconnectOptions* options, int internal)
{
	MQTTAsyncs* m = handle;
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsync_queuedCommand* dis;

	FUNC_ENTRY;
	if (m == NULL || m->c == NULL)
	{
		rc = MQTTASYNC_FAILURE;
		goto exit;
	}
	if (!internal)
		m->shouldBeConnected = 0;
	if (m->c->connected == 0)
	{
		rc = MQTTASYNC_DISCONNECTED;
		goto exit;
	}

	/* Add disconnect request to operation queue */
	if ((dis = malloc(sizeof(MQTTAsync_queuedCommand))) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	memset(dis, '\0', sizeof(MQTTAsync_queuedCommand));
	dis->client = m;
	if (options)
	{
		dis->command.onSuccess = options->onSuccess;
		dis->command.onFailure = options->onFailure;
		dis->command.onSuccess5 = options->onSuccess5;
		dis->command.onFailure5 = options->onFailure5;
		dis->command.context = options->context;
		dis->command.details.dis.timeout = options->timeout;
		if (m->c->MQTTVersion >= MQTTVERSION_5 && options->struct_version >= 1)
		{
			dis->command.properties = MQTTProperties_copy(&options->properties);
			dis->command.details.dis.reasonCode = options->reasonCode;
		}
	}
	dis->command.type = DISCONNECT;
	dis->command.details.dis.internal = internal;
	rc = MQTTAsync_addCommand(dis, sizeof(dis));

exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


static int MQTTAsync_disconnect_internal(MQTTAsync handle, int timeout)
{
	MQTTAsync_disconnectOptions options = MQTTAsync_disconnectOptions_initializer;

	options.timeout = timeout;
	return MQTTAsync_disconnect1(handle, &options, 1);
}


void MQTTProtocol_closeSession(Clients* c, int sendwill)
{
	nextOrClose((MQTTAsync)c->context, MQTTASYNC_DISCONNECTED, "MQTTProtocol_closeSession");
}


static int cmdMessageIDCompare(void* a, void* b)
{
	MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)a;
	return cmd->command.token == *(int*)b;
}


/**
 * Assign a new message id for a client.  Make sure it isn't already being used and does
 * not exceed the maximum.
 * @param m a client structure
 * @return the next message id to use, or 0 if none available
 */
int MQTTAsync_assignMsgId(MQTTAsyncs* m)
{
	int start_msgid;
	int msgid;

	/* need to check: commands list and response list for a client */
	FUNC_ENTRY;
	/* Fetch last message ID in locked state */
	start_msgid = m->c->msgID;
	msgid = start_msgid;

	MQTTAsync_lock_mutex(mqttcommand_mutex);
	msgid = (msgid == MAX_MSG_ID) ? 1 : msgid + 1;
	while (ListFindItem(MQTTAsync_commands, &msgid, cmdMessageIDCompare) ||
			ListFindItem(m->c->outboundMsgs, &msgid, messageIDCompare) ||
			ListFindItem(m->responses, &msgid, cmdMessageIDCompare))
	{
		msgid = (msgid == MAX_MSG_ID) ? 1 : msgid + 1;
		if (msgid == start_msgid)
		{ /* we've tried them all - none free */
			msgid = 0;
			break;
		}
	}
	MQTTAsync_unlock_mutex(mqttcommand_mutex);
	if (msgid != 0)
		m->c->msgID = msgid;
	FUNC_EXIT_RC(msgid);
	return msgid;
}


static void MQTTAsync_retry(void)
{
	static START_TIME_TYPE last = START_TIME_ZERO;
	START_TIME_TYPE now;

	FUNC_ENTRY;
	now = MQTTTime_now();
	if (MQTTTime_difftime(now, last) >= (DIFF_TIME_TYPE)(retryLoopIntervalms))
	{
		last = MQTTTime_now();
		MQTTProtocol_keepalive(now);
		MQTTProtocol_retry(now, 1, 0);
	}
	else
		MQTTProtocol_retry(now, 0, 0);
	FUNC_EXIT;
}


static int MQTTAsync_connecting(MQTTAsyncs* m)
{
	int rc = -1;
	char* serverURI = m->serverURI;
#if defined(OPENSSL)
	int default_port = MQTT_DEFAULT_PORT;
#endif

	FUNC_ENTRY;

	/* This was reported in #1007, but I've not been able to reproduce it.  It feels like this is
	 * covering up the issue, if it exists.  If the error message is ever seen, please consider
	 * reporting the circumstances so that more debugging can occur.  Thanks - IGC.
	 */
	if (m->connect.details.conn.MQTTVersion == MQTTVERSION_DEFAULT) /* should not happen - #1007 */
	{
		Log(LOG_ERROR, -1, "MQTT version is 0 in MQTTAsync_connecting");
		m->connect.details.conn.MQTTVersion = (m->c->MQTTVersion == MQTTVERSION_DEFAULT) ? MQTTVERSION_3_1_1 : m->c->MQTTVersion;
	}
	/* End of #1007 avoiding code */

	if (m->serverURIcount > 0)
	{
		serverURI = m->serverURIs[m->connect.details.conn.currentURI];

		/* skip URI scheme */
		if (strncmp(URI_TCP, serverURI, strlen(URI_TCP)) == 0)
			serverURI += strlen(URI_TCP);
		else if (strncmp(URI_MQTT, serverURI, strlen(URI_MQTT)) == 0)
			serverURI += strlen(URI_MQTT);
		else if (strncmp(URI_WS, serverURI, strlen(URI_WS)) == 0)
		{
			serverURI += strlen(URI_WS);
#if defined(OPENSSL)
			default_port = WS_DEFAULT_PORT;
#endif
		}
#if defined(OPENSSL)
		else if (strncmp(URI_SSL, serverURI, strlen(URI_SSL)) == 0)
		{
			serverURI += strlen(URI_SSL);
			default_port = SECURE_MQTT_DEFAULT_PORT;
		}
		else if (strncmp(URI_TLS, serverURI, strlen(URI_TLS)) == 0)
		{
			serverURI += strlen(URI_TLS);
			default_port = SECURE_MQTT_DEFAULT_PORT;
		}
		else if (strncmp(URI_MQTTS, serverURI, strlen(URI_MQTTS)) == 0)
		{
			serverURI += strlen(URI_MQTTS);
			default_port = SECURE_MQTT_DEFAULT_PORT;
		}
		else if (strncmp(URI_WSS, serverURI, strlen(URI_WSS)) == 0)
		{
			serverURI += strlen(URI_WSS);
			default_port = WSS_DEFAULT_PORT;
		}
#endif
	}

	if (m->c->connect_state == TCP_IN_PROGRESS) /* TCP connect started - check for completion */
	{
		int error;
		socklen_t len = sizeof(error);

		if ((rc = getsockopt(m->c->net.socket, SOL_SOCKET, SO_ERROR, (char*)&error, &len)) == 0)
			rc = error;

		if (rc != 0)
			goto exit;

		Socket_clearPendingWrite(m->c->net.socket);

#if defined(OPENSSL)
		if (m->ssl)
		{
			int port;
			size_t hostname_len;
			int setSocketForSSLrc = 0;

			if (m->c->net.https_proxy) {
				m->c->connect_state = PROXY_CONNECT_IN_PROGRESS;
				if ((rc = Proxy_connect( &m->c->net, 1, serverURI)) == SOCKET_ERROR )
					goto exit;
			}

			hostname_len = MQTTProtocol_addressPort(serverURI, &port, NULL, default_port);
			setSocketForSSLrc = SSLSocket_setSocketForSSL(&m->c->net, m->c->sslopts,
					serverURI, hostname_len);

			if (setSocketForSSLrc == 1)
			{
				if (m->c->session != NULL)
					if ((rc = SSL_set_session(m->c->net.ssl, m->c->session)) != 1)
						Log(TRACE_MIN, -1, "Failed to set SSL session with stored data, non critical");
				rc = m->c->sslopts->struct_version >= 3 ?
					SSLSocket_connect(m->c->net.ssl, m->c->net.socket, serverURI,
						m->c->sslopts->verify, m->c->sslopts->ssl_error_cb, m->c->sslopts->ssl_error_context) :
					SSLSocket_connect(m->c->net.ssl, m->c->net.socket, serverURI,
						m->c->sslopts->verify, NULL, NULL);
				if (rc == TCPSOCKET_INTERRUPTED)
				{
					rc = MQTTCLIENT_SUCCESS; /* the connect is still in progress */
					m->c->connect_state = SSL_IN_PROGRESS;
				}
				else if (rc == SSL_FATAL)
				{
					rc = SOCKET_ERROR;
					goto exit;
				}
				else if (rc == 1)
				{
					if ( m->websocket )
					{
						m->c->connect_state = WEBSOCKET_IN_PROGRESS;
						if ((rc = WebSocket_connect(&m->c->net, m->ssl, serverURI)) == SOCKET_ERROR )
							goto exit;
					}
					else
					{
						rc = MQTTCLIENT_SUCCESS;
						m->c->connect_state = WAIT_FOR_CONNACK;
						if (MQTTPacket_send_connect(m->c, m->connect.details.conn.MQTTVersion,
								m->connectProps, m->willProps) == SOCKET_ERROR)
						{
							rc = SOCKET_ERROR;
							goto exit;
						}
					}
					if (!m->c->cleansession && m->c->session == NULL)
						m->c->session = SSL_get1_session(m->c->net.ssl);
				}
			}
			else
			{
				rc = SOCKET_ERROR;
				goto exit;
			}
		}
		else
		{
#endif
			if (m->c->net.http_proxy) {
				m->c->connect_state = PROXY_CONNECT_IN_PROGRESS;
				if ((rc = Proxy_connect( &m->c->net, 0, serverURI)) == SOCKET_ERROR )
					goto exit;
			}

			if ( m->websocket )
			{
				m->c->connect_state = WEBSOCKET_IN_PROGRESS;
				if ((rc = WebSocket_connect(&m->c->net, 0, serverURI)) == SOCKET_ERROR )
					goto exit;
			}
			else
			{
				m->c->connect_state = WAIT_FOR_CONNACK; /* TCP/SSL connect completed, in which case send the MQTT connect packet */
				if ((rc = MQTTPacket_send_connect(m->c, m->connect.details.conn.MQTTVersion,
						m->connectProps, m->willProps)) == SOCKET_ERROR)
					goto exit;
			}
#if defined(OPENSSL)
		}
#endif
	}
#if defined(OPENSSL)
	else if (m->c->connect_state == SSL_IN_PROGRESS) /* SSL connect sent - wait for completion */
	{
		rc = m->c->sslopts->struct_version >= 3 ?
			SSLSocket_connect(m->c->net.ssl, m->c->net.socket, serverURI,
				m->c->sslopts->verify, m->c->sslopts->ssl_error_cb, m->c->sslopts->ssl_error_context) :
			SSLSocket_connect(m->c->net.ssl, m->c->net.socket, serverURI,
				m->c->sslopts->verify, NULL, NULL);
		if (rc != 1)
			goto exit;

		if(!m->c->cleansession && m->c->session == NULL)
			m->c->session = SSL_get1_session(m->c->net.ssl);

		if ( m->websocket )
		{
			m->c->connect_state = WEBSOCKET_IN_PROGRESS;
			if ((rc = WebSocket_connect(&m->c->net, 1, serverURI)) == SOCKET_ERROR )
				goto exit;
		}
		else
		{
			m->c->connect_state = WAIT_FOR_CONNACK; /* SSL connect completed, in which case send the MQTT connect packet */
			if ((rc = MQTTPacket_send_connect(m->c, m->connect.details.conn.MQTTVersion,
					m->connectProps, m->willProps)) == SOCKET_ERROR)
				goto exit;
		}
	}
#endif
	else if (m->c->connect_state == WEBSOCKET_IN_PROGRESS) /* Websocket connect sent - wait for completion */
	{
		if ((rc = WebSocket_upgrade( &m->c->net ) ) == SOCKET_ERROR )
			goto exit;
		else if (rc != TCPSOCKET_INTERRUPTED)
		{
			m->c->connect_state = WAIT_FOR_CONNACK; /* Websocket upgrade completed, in which case send the MQTT connect packet */
			if ((rc = MQTTPacket_send_connect(m->c, m->connect.details.conn.MQTTVersion, m->connectProps, m->willProps)) == SOCKET_ERROR)
				goto exit;
		}
	}

exit:
	if ((rc != 0 && rc != TCPSOCKET_INTERRUPTED && (m->c->connect_state != SSL_IN_PROGRESS && m->c->connect_state != WEBSOCKET_IN_PROGRESS)) || (rc == SSL_FATAL))
		nextOrClose(m, MQTTASYNC_FAILURE, "TCP/TLS connect failure");

	FUNC_EXIT_RC(rc);
	return rc;
}


static MQTTPacket* MQTTAsync_cycle(SOCKET* sock, unsigned long timeout, int* rc)
{
	MQTTPacket* pack = NULL;
	int rc1 = 0;

	FUNC_ENTRY;
#if defined(OPENSSL)
	if ((*sock = SSLSocket_getPendingRead()) == -1)
	{
#endif
		/*int should_stop = 0;*/
		int interrupted = 0;

		/* 0 from getReadySocket indicates no work to do, rc -1 == error */
		*sock = Socket_getReadySocket(0, (int)timeout, socket_mutex, &rc1, &interrupted);
		*rc = rc1;
		/*MQTTAsync_lock_mutex(mqttasync_mutex);
		should_stop = MQTTAsync_tostop;
		MQTTAsync_unlock_mutex(mqttasync_mutex);*/
		/* Frank deleted this small delay on the suggestion of Claude.
		 I want to preserve it as a comment in case we need it in the future.
		 The preceeding 3 lines of code are only needed if the following line
		 is reinstated - Ian
		 if (!should_stop && *sock == 0 && (timeout > 0L) && (interrupted == 0))
			MQTTAsync_sleep(50L);*/
#if defined(OPENSSL)
	}
#endif
	MQTTAsync_lock_mutex(mqttasync_mutex);
	if (*sock > 0 && rc1 == 0)
	{
		MQTTAsyncs* m = NULL;
		if (ListFindItem(MQTTAsync_handles, sock, clientSockCompare) != NULL)
			m = (MQTTAsync)(MQTTAsync_handles->current->content);
		if (m != NULL)
		{
			Log(TRACE_MINIMUM, -1, "m->c->connect_state = %d", m->c->connect_state);
			if (m->c->connect_state == TCP_IN_PROGRESS || m->c->connect_state == SSL_IN_PROGRESS || m->c->connect_state == WEBSOCKET_IN_PROGRESS)
				*rc = MQTTAsync_connecting(m);
			else
				pack = MQTTPacket_Factory(m->c->MQTTVersion, &m->c->net, rc);
			if (m->c->connect_state == WAIT_FOR_CONNACK && *rc == SOCKET_ERROR)
			{
				Log(TRACE_MINIMUM, -1, "CONNECT sent but MQTTPacket_Factory has returned SOCKET_ERROR");
				nextOrClose(m, MQTTASYNC_FAILURE, "TCP connect completion failure");
			}
		}
		if (pack)
		{
			int freed = 1;

			/* Note that these handle... functions free the packet structure that they are dealing with */
			if (pack->header.bits.type == PUBLISH)
				*rc = MQTTProtocol_handlePublishes(pack, *sock);
			else if (pack->header.bits.type == PUBACK || pack->header.bits.type == PUBCOMP ||
					pack->header.bits.type == PUBREC)
			{
				int msgid = 0,
					mqttversion = 0;
				unsigned int msgtype = 0,
					ackrc = 0;
				MQTTProperties msgprops = MQTTProperties_initializer;
				Publications* pubToRemove = NULL;

				/* This block is so that the ack variable is local and isn't accidentally reused */
				{
					static Ack ack;
					ack = *(Ack*)pack;
					/* these values are stored because the packet structure is freed in the handle functions */
					msgid = ack.msgId;
					msgtype = pack->header.bits.type;
					if (ack.MQTTVersion >= MQTTVERSION_5)
					{
						ackrc = ack.rc;
						msgprops = MQTTProperties_copy(&ack.properties);
						mqttversion = ack.MQTTVersion;
					}
				}

				if (msgtype == PUBCOMP)
				{
					*rc = MQTTProtocol_handlePubcomps(pack, *sock, &pubToRemove);
					if (sendThread_state != STOPPED)
						Thread_signal_evt(send_evt);
				}
				else if (msgtype == PUBREC)
					*rc = MQTTProtocol_handlePubrecs(pack, *sock, &pubToRemove);
				else if (msgtype == PUBACK)
				{
					*rc = MQTTProtocol_handlePubacks(pack, *sock, &pubToRemove);
					if (sendThread_state != STOPPED)
						Thread_signal_evt(send_evt);
				}
				if (!m)
					Log(LOG_ERROR, -1, "PUBCOMP, PUBACK or PUBREC received for no client, msgid %d", msgid);
				if (m && *rc == 0 && (msgtype != PUBREC || ackrc >= MQTTREASONCODE_UNSPECIFIED_ERROR))
				{
					ListElement* current = NULL;

					if (m->dc)
					{
						Log(TRACE_MIN, -1, "Calling deliveryComplete for client %s, msgid %d", m->c->clientID, msgid);
						(*(m->dc))(m->dcContext, msgid);
					}
					/* use the msgid to find the callback to be called */
					while (ListNextElement(m->responses, &current))
					{
						MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(current->content);
						if (command->command.token == msgid)
						{
							if (!ListDetach(m->responses, command)) /* then remove the response from the list */
								Log(LOG_ERROR, -1, "Publish command not removed from command list");
							if (command->command.onSuccess)
							{
								MQTTAsync_successData data;

								data.token = command->command.token;
								data.alt.pub.destinationName = command->command.details.pub.destinationName;
								data.alt.pub.message.payload = command->command.details.pub.payload;
								data.alt.pub.message.payloadlen = command->command.details.pub.payloadlen;
								data.alt.pub.message.qos = command->command.details.pub.qos;
								data.alt.pub.message.retained = command->command.details.pub.retained;
								Log(TRACE_MIN, -1, "Calling publish success for client %s", m->c->clientID);
								(*(command->command.onSuccess))(command->command.context, &data);
							}
							else if (command->command.onSuccess5 && ackrc < MQTTREASONCODE_UNSPECIFIED_ERROR)
							{
								MQTTAsync_successData5 data = MQTTAsync_successData5_initializer;

								data.token = command->command.token;
								data.alt.pub.destinationName = command->command.details.pub.destinationName;
								data.alt.pub.message.payload = command->command.details.pub.payload;
								data.alt.pub.message.payloadlen = command->command.details.pub.payloadlen;
								data.alt.pub.message.qos = command->command.details.pub.qos;
								data.alt.pub.message.retained = command->command.details.pub.retained;
								data.properties = command->command.properties;
								Log(TRACE_MIN, -1, "Calling publish success for client %s", m->c->clientID);
								(*(command->command.onSuccess5))(command->command.context, &data);
							}
							else if (command->command.onFailure5 && ackrc >= MQTTREASONCODE_UNSPECIFIED_ERROR)
							{
								MQTTAsync_failureData5 data = MQTTAsync_failureData5_initializer;

								data.token = command->command.token;
								data.reasonCode = ackrc;
								data.properties = msgprops;
								data.packet_type = msgtype;
								Log(TRACE_MIN, -1, "Calling publish failure for client %s", m->c->clientID);
								(*(command->command.onFailure5))(command->command.context, &data);
							}
							if (pubToRemove != NULL)
							{
								MQTTProtocol_removePublication(pubToRemove);
								pubToRemove = NULL;
								/* removePublication has freed the topic and payload memory, so here we indicate that
								 * so freeCommand doesn't try to free them again.
								 */
								command->command.details.pub.destinationName = NULL;
								command->command.details.pub.payload = NULL;
							}
							MQTTAsync_freeCommand(command);
							break;
						}
					}
					if (mqttversion >= MQTTVERSION_5)
						MQTTProperties_free(&msgprops);
				}
				if (pubToRemove != NULL)
					MQTTProtocol_removePublication(pubToRemove);
			}
			else if (pack->header.bits.type == PUBREL)
				*rc = MQTTProtocol_handlePubrels(pack, *sock);
			else if (pack->header.bits.type == PINGRESP)
				*rc = MQTTProtocol_handlePingresps(pack, *sock);
			else
				freed = 0;
			if (freed)
				pack = NULL;
		}
	}
	MQTTAsync_retry();
	MQTTAsync_unlock_mutex(mqttasync_mutex);
	FUNC_EXIT_RC(*rc);
	return pack;
}


int MQTTAsync_getNoBufferedMessages(MQTTAsyncs* m)
{
	int count = 0;

	MQTTAsync_lock_mutex(mqttcommand_mutex);
	count = m->noBufferedMessages;
	MQTTAsync_unlock_mutex(mqttcommand_mutex);
	return count;
}
