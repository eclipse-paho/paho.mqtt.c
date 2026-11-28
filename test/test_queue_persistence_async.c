/*******************************************************************************
 * Copyright (c) 2026 Roman Ondráček
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
 * AI Disclosure: This file was partly AI-generated. The AI-generated
 * portions are made available under CC0-1.0 and not subject to the
 * project's licence. The human contributor has reviewed and verified
 * that the code is correct.
 *
 * SPDX-License-Identifier: EPL-2.0 and CC0-1.0
 *
 * Contributors:
 *    Roman Ondráček - initial implementation
 *******************************************************************************/

/**
 * @file
 * Test that messages queued for delivery in the asynchronous client are restored
 * correctly from persistence. See test_queue_persistence.c for the synchronous
 * client, and the persisted format.
 * The asynchronous client queues a message, and persists it, when messageArrived
 * returns 0. The client subscribes and publishes messages to itself, which are
 * refused until they are all persisted in the message queue. Then it disconnects
 * and is destroyed, and recreated, which restores the queue. When it connects
 * again, the restored messages are delivered, and the test checks that they are
 * the ones which were sent. The client delivers one queued message each time
 * there is network activity, so the test publishes other messages to cause it.
 * The payload and topic lengths are not multiples of the size of an int, so the
 * ints persisted after them are not aligned. Run with -fsanitize=alignment to
 * detect reading them through int pointers on platforms which allow it.
 * The persistence callbacks and messageArrived are called on library threads, so
 * the in-memory store and the state shared with the callbacks are locked.
 * Test 1: MQTT 3.1.1
 * Test 2: MQTT 5, with a user property in each message, which is restored after
 *         the unaligned fields. MQTT 5 queue entries were never persisted before.
 */

#include "MQTTAsync.h"
#include "Thread.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#if !defined(_WINDOWS)
#include <sys/time.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

void usage(void)
{
	printf("Options:\n");
	printf("  --test_no <n>  1: MQTT 3.1.1; 2: MQTT 5 (default all)\n");
	printf("  --connection <uri>  broker to connect to (default tcp://localhost:1883)\n");
	printf("  --verbose\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	char* connection;
	int verbose;
	int test_no;
} options =
{
	"tcp://localhost:1883",
	0,
	0,
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
		else if (strcmp(argv[count], "--verbose") == 0)
			options.verbose = 1;
		else if (strcmp(argv[count], "--help") == 0 || strcmp(argv[count], "-h") == 0)
			usage();
		count++;
	}
}


#define LOGA_DEBUG 0
#define LOGA_INFO 1
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
#if defined(_WIN32) || defined(_WINDOWS)
	Sleep(milliseconds);
#else
	usleep(milliseconds*1000);
#endif
}

#define assert(a, b, c, ...) myassert(__FILE__, __LINE__, a, b, c, __VA_ARGS__)

int tests = 0;
int failures = 0;

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


/* Locks the store and the state shared with the callbacks, which are called on library threads */
static mutex_type lock = NULL;


/* A user persistence implementation which keeps the data in memory, so that it
 * survives the client being destroyed and recreated.
 * The library frees the buffers returned by get and keys, so they are allocated
 * with MQTTAsync_malloc, which works whether or not the library tracks its heap. */

#define STORE_SIZE 128

struct store_entry
{
	char* key;
	char* data;
	int len;
};

struct store
{
	struct store_entry entries[STORE_SIZE];
};

static struct store store;

static struct store_entry* store_find(struct store* s, const char* key)
{
	int i;

	for (i = 0; i < STORE_SIZE; ++i)
		if (s->entries[i].key && strcmp(s->entries[i].key, key) == 0)
			return &s->entries[i];
	return NULL;
}

static void store_remove_entry(struct store_entry* e)
{
	free(e->key);
	free(e->data);
	e->key = NULL;
	e->data = NULL;
	e->len = 0;
}

static void store_clear(struct store* s)
{
	int i;

	Paho_thread_lock_mutex(lock);
	for (i = 0; i < STORE_SIZE; ++i)
		if (s->entries[i].key)
			store_remove_entry(&s->entries[i]);
	Paho_thread_unlock_mutex(lock);
}

/* the number of messages in the persisted message queue */
static int store_queued(const struct store* s)
{
	int i, count = 0;

	Paho_thread_lock_mutex(lock);
	for (i = 0; i < STORE_SIZE; ++i)
		if (s->entries[i].key && (strncmp(s->entries[i].key, "q-", 2) == 0 ||
				strncmp(s->entries[i].key, "q5-", 3) == 0))
			++count;
	Paho_thread_unlock_mutex(lock);
	return count;
}

static int pers_open(void** handle, const char* clientID, const char* serverURI, void* context)
{
	*handle = context;
	return 0;
}

static int pers_close(void* handle)
{
	return 0;
}

static int pers_put(void* handle, char* key, int bufcount, char* buffers[], int buflens[])
{
	struct store* s = handle;
	struct store_entry* e;
	size_t keylen = strlen(key) + 1; /* keys are null terminated strings created by the library */
	int rc = MQTTCLIENT_PERSISTENCE_ERROR;
	int i, len = 0;
	char* ptr = NULL;

	MyLog(LOGA_DEBUG, "Persisting key %s", key);
	Paho_thread_lock_mutex(lock);
	if ((e = store_find(s, key)) != NULL)
		store_remove_entry(e);
	else
	{
		for (i = 0; i < STORE_SIZE && e == NULL; ++i)
			if (s->entries[i].key == NULL)
				e = &s->entries[i];
		if (e == NULL)
			goto exit;
	}

	for (i = 0; i < bufcount; ++i)
		len += buflens[i];
	e->key = malloc(keylen);
	e->data = malloc(len > 0 ? len : 1);
	if (e->key == NULL || e->data == NULL)
	{
		store_remove_entry(e);
		goto exit;
	}
	memcpy(e->key, key, keylen);
	ptr = e->data;
	for (i = 0; i < bufcount; ++i)
	{
		memcpy(ptr, buffers[i], buflens[i]);
		ptr += buflens[i];
	}
	e->len = len;
	rc = 0;
exit:
	Paho_thread_unlock_mutex(lock);
	return rc;
}

static int pers_get(void* handle, char* key, char** buffer, int* buflen)
{
	const struct store_entry* e;
	int rc = MQTTCLIENT_PERSISTENCE_ERROR;

	Paho_thread_lock_mutex(lock);
	e = store_find(handle, key);
	if (e && (*buffer = MQTTAsync_malloc(e->len > 0 ? e->len : 1)) != NULL)
	{
		memcpy(*buffer, e->data, e->len);
		*buflen = e->len;
		rc = 0;
	}
	Paho_thread_unlock_mutex(lock);
	return rc;
}

static int pers_remove(void* handle, char* key)
{
	struct store_entry* e;

	MyLog(LOGA_DEBUG, "Removing key %s", key);
	Paho_thread_lock_mutex(lock);
	if ((e = store_find(handle, key)) != NULL)
		store_remove_entry(e);
	Paho_thread_unlock_mutex(lock);
	return 0;
}

static int pers_keys(void* handle, char*** keys, int* nkeys)
{
	const struct store* s = handle;
	int rc = 0;
	int i, count = 0;

	*keys = NULL;
	*nkeys = 0;
	Paho_thread_lock_mutex(lock);
	for (i = 0; i < STORE_SIZE; ++i)
		if (s->entries[i].key)
			++count;
	if (count > 0 && (*keys = MQTTAsync_malloc(count * sizeof(char*))) == NULL)
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
	for (i = 0; rc == 0 && count > 0 && i < STORE_SIZE; ++i)
	{
		if (s->entries[i].key)
		{
			size_t keylen = strlen(s->entries[i].key) + 1; /* stored by pers_put, so null terminated */
			char* key = MQTTAsync_malloc(keylen);

			if (key == NULL)
				rc = MQTTCLIENT_PERSISTENCE_ERROR;
			else
			{
				memcpy(key, s->entries[i].key, keylen);
				(*keys)[(*nkeys)++] = key;
			}
		}
	}
	Paho_thread_unlock_mutex(lock);
	return rc;
}

static int pers_clear(void* handle)
{
	store_clear(handle);
	return 0;
}

static int pers_containskey(void* handle, char* key)
{
	int rc;

	Paho_thread_lock_mutex(lock);
	rc = store_find(handle, key) ? 0 : MQTTCLIENT_PERSISTENCE_ERROR;
	Paho_thread_unlock_mutex(lock);
	return rc;
}


/* The payload and topic lengths aren't multiples of the size of an int, so that
 * the ints persisted after them aren't aligned */
static const struct
{
	char* subtopic;
	char* payload;
	int qos;
} messages[] =
{
	{"a", "1", 1},
	{"bc", "22", 2},
	{"def", "333", 1},
	{"ghijk", "55555", 2},
	{"lmnopqr", "7777777", 1},
};

#define MESSAGE_COUNT (int)(sizeof(messages) / sizeof(messages[0]))


/* A copy of a message received by messageArrived, which the main thread checks */
struct received_message
{
	char topic[100];
	int topicLen;
	char payload[20];
	int payloadlen;
	int qos;
	int retained;
	int dup;
	int msgid;
	char user_property[20]; /* the value of the user property, for MQTT 5 */
	int has_user_property;
};

/* The state shared with the callbacks, locked by lock */
static struct
{
	int accept; /* whether messageArrived accepts messages, or refuses them so that they're queued */
	int received_count;
	struct received_message received[MESSAGE_COUNT];
	int unexpected; /* messages received after all the expected ones */
	int success; /* the number of operations which succeeded */
	int failure; /* the number of operations which failed */
} state;

static int get_state(const int* value)
{
	int rc;

	Paho_thread_lock_mutex(lock);
	rc = *value;
	Paho_thread_unlock_mutex(lock);
	return rc;
}

static void increment_state(int* value)
{
	Paho_thread_lock_mutex(lock);
	++(*value);
	Paho_thread_unlock_mutex(lock);
}

static void reset_state(int accept)
{
	Paho_thread_lock_mutex(lock);
	memset(&state, '\0', sizeof(state));
	state.accept = accept;
	Paho_thread_unlock_mutex(lock);
}

static void copy_string(char* dest, size_t size, const char* src, int len)
{
	if (len < 0)
		len = 0;
	if ((size_t)len >= size)
		len = (int)size - 1;
	memcpy(dest, src, len);
	dest[len] = '\0';
}

static int messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
	int accept;

	Paho_thread_lock_mutex(lock);
	accept = state.accept;
	if (accept)
	{
		if (state.received_count < MESSAGE_COUNT)
		{
			struct received_message* r = &state.received[state.received_count++];
			const MQTTProperty* property = MQTTProperties_getProperty(&message->properties,
					MQTTPROPERTY_CODE_USER_PROPERTY);

			copy_string(r->topic, sizeof(r->topic), topicName, (int)strlen(topicName));
			r->topicLen = topicLen;
			copy_string(r->payload, sizeof(r->payload), message->payload, message->payloadlen);
			r->payloadlen = message->payloadlen;
			r->qos = message->qos;
			r->retained = message->retained;
			r->dup = message->dup;
			r->msgid = message->msgid;
			if (property)
			{
				copy_string(r->user_property, sizeof(r->user_property), property->value.value.data,
						property->value.value.len);
				r->has_user_property = 1;
			}
		}
		else
			++state.unexpected;
	}
	Paho_thread_unlock_mutex(lock);

	if (!accept)
		return 0; /* the message stays queued, and persisted */
	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);
	return 1;
}

static void onSuccess(void* context, MQTTAsync_successData* response)
{
	increment_state(&state.success);
}

static void onFailure(void* context, MQTTAsync_failureData* response)
{
	increment_state(&state.failure);
}

static void onSuccess5(void* context, MQTTAsync_successData5* response)
{
	increment_state(&state.success);
}

static void onFailure5(void* context, MQTTAsync_failureData5* response)
{
	increment_state(&state.failure);
}

/* wait for an operation to succeed or fail, with the success count it had before it was started */
static int wait_for(int successes)
{
	int i;

	for (i = 0; i < 500 && get_state(&state.success) <= successes && get_state(&state.failure) == 0; ++i)
		MySleep(10);
	return get_state(&state.success) > successes && get_state(&state.failure) == 0;
}

static void set_callbacks(MQTTAsync_responseOptions* ropts, int MQTTVersion)
{
	if (MQTTVersion >= MQTTVERSION_5)
	{
		ropts->onSuccess5 = onSuccess5;
		ropts->onFailure5 = onFailure5;
	}
	else
	{
		ropts->onSuccess = onSuccess;
		ropts->onFailure = onFailure;
	}
}


static int connect_client(MQTTAsync c, int MQTTVersion, int clean)
{
	MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_connectOptions opts5 = MQTTAsync_connectOptions_initializer5;
	const MQTTAsync_connectOptions* popts = &opts;
	int successes = get_state(&state.success);
	int rc;

	if (MQTTVersion >= MQTTVERSION_5)
	{
		opts5.cleanstart = clean;
		opts5.onSuccess5 = onSuccess5;
		opts5.onFailure5 = onFailure5;
		popts = &opts5;
	}
	else
	{
		opts.cleansession = clean;
		opts.MQTTVersion = MQTTVersion;
		opts.onSuccess = onSuccess;
		opts.onFailure = onFailure;
	}
	rc = MQTTAsync_connect(c, popts);
	assert("good rc from connect", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc == MQTTASYNC_SUCCESS)
	{
		rc = wait_for(successes) ? MQTTASYNC_SUCCESS : MQTTASYNC_FAILURE;
		assert("connected", rc == MQTTASYNC_SUCCESS, "failures %d\n", get_state(&state.failure));
	}
	return rc;
}


static void disconnect_client(MQTTAsync c, int MQTTVersion)
{
	MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
	int successes = get_state(&state.success);
	int rc;

	if (MQTTVersion >= MQTTVERSION_5)
	{
		opts.onSuccess5 = onSuccess5;
		opts.onFailure5 = onFailure5;
	}
	else
	{
		opts.onSuccess = onSuccess;
		opts.onFailure = onFailure;
	}
	opts.timeout = 1000;
	rc = MQTTAsync_disconnect(c, &opts);
	if (rc == MQTTASYNC_SUCCESS)
		wait_for(successes);
}


static int subscribe(MQTTAsync c, int MQTTVersion, const char* topic_filter)
{
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
	int successes = get_state(&state.success);
	int rc;

	set_callbacks(&ropts, MQTTVersion);
	rc = MQTTAsync_subscribe(c, topic_filter, 2, &ropts);
	assert("good rc from subscribe", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc == MQTTASYNC_SUCCESS)
	{
		rc = wait_for(successes) ? MQTTASYNC_SUCCESS : MQTTASYNC_FAILURE;
		assert("subscribed", rc == MQTTASYNC_SUCCESS, "failures %d\n", get_state(&state.failure));
	}
	return rc;
}


static int publish_message(MQTTAsync c, int MQTTVersion, const char* topic, int index)
{
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
	int successes = get_state(&state.success);
	int rc;

	pubmsg.payload = messages[index].payload;
	pubmsg.payloadlen = (int)strlen(messages[index].payload);
	pubmsg.qos = messages[index].qos;
	if (MQTTVersion >= MQTTVERSION_5)
	{
		MQTTProperty property;

		property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
		property.value.data.data = "subtopic";
		property.value.data.len = (int)strlen(property.value.data.data);
		property.value.value.data = messages[index].subtopic;
		property.value.value.len = (int)strlen(messages[index].subtopic);
		MQTTProperties_add(&pubmsg.properties, &property);
	}
	set_callbacks(&ropts, MQTTVersion);
	rc = MQTTAsync_sendMessage(c, topic, &pubmsg, &ropts);
	MQTTProperties_free(&pubmsg.properties);
	assert("good rc from sendMessage", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc == MQTTASYNC_SUCCESS)
	{
		rc = wait_for(successes) ? MQTTASYNC_SUCCESS : MQTTASYNC_FAILURE;
		assert("publish completed", rc == MQTTASYNC_SUCCESS, "failures %d\n", get_state(&state.failure));
	}
	return rc;
}


static void check_message(int MQTTVersion, const char* topic_base, int index, const struct received_message* r)
{
	char expected_topic[100];
	int payloadlen = (int)strlen(messages[index].payload);

	snprintf(expected_topic, sizeof(expected_topic), "%s%s", topic_base, messages[index].subtopic);
	assert("topic restored", strcmp(r->topic, expected_topic) == 0,
		"topic was %s, expected %s\n", r->topic, expected_topic);
	/* the asynchronous client passes 0 if the topic contains no nulls */
	assert("topic length restored", r->topicLen == 0 || r->topicLen == (int)strlen(expected_topic),
		"topic length was %d, expected %d\n", r->topicLen, (int)strlen(expected_topic));
	assert("payload length restored", r->payloadlen == payloadlen,
		"payload length was %d, expected %d\n", r->payloadlen, payloadlen);
	assert("payload restored", strcmp(r->payload, messages[index].payload) == 0,
		"payload was %s, expected %s\n", r->payload, messages[index].payload);
	assert("qos restored", r->qos == messages[index].qos,
		"qos was %d, expected %d\n", r->qos, messages[index].qos);
	assert("retained restored", r->retained == 0, "retained was %d\n", r->retained);
	assert("dup restored", r->dup == 0, "dup was %d\n", r->dup);
	assert("msgid restored", r->msgid > 0 && r->msgid <= 65535, "msgid was %d\n", r->msgid);
	if (MQTTVersion >= MQTTVERSION_5)
	{
		assert("user property restored", r->has_user_property, "%s\n", "no user property");
		assert("user property value restored", strcmp(r->user_property, messages[index].subtopic) == 0,
			"value was %s, expected %s\n", r->user_property, messages[index].subtopic);
	}
}


/* The client delivers one queued message each time there is network activity, so
 * while waiting for the restored messages, publish QoS 1 messages to a topic which
 * isn't subscribed to, whose PUBACKs cause that activity */
static void nudge(MQTTAsync c)
{
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;

	pubmsg.payload = "nudge";
	pubmsg.payloadlen = 5;
	pubmsg.qos = 1;
	MQTTAsync_sendMessage(c, "async_queue_persistence_nudge", &pubmsg, NULL);
}


/* wait for a condition on the state or the store, polling every 10ms for up to 5s */
#define WAIT_UNTIL(condition) \
	do { int wait_i; for (wait_i = 0; wait_i < 500 && !(condition); ++wait_i) MySleep(10); } while (0)


static int create_client(MQTTAsync* c, const char* clientid, int MQTTVersion, MQTTClient_persistence* persistence)
{
	MQTTAsync_createOptions createOpts = MQTTAsync_createOptions_initializer;
	int rc;

	createOpts.MQTTVersion = MQTTVersion;
	rc = MQTTAsync_createWithOptions(c, options.connection, clientid, MQTTCLIENT_PERSISTENCE_USER,
			persistence, &createOpts);
	assert("good rc from create", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	if (rc == MQTTASYNC_SUCCESS)
	{
		rc = MQTTAsync_setCallbacks(*c, NULL, NULL, messageArrived, NULL);
		assert("good rc from setCallbacks", rc == MQTTASYNC_SUCCESS, "rc was %d\n", rc);
	}
	return rc;
}


/* subscribe, and publish the messages, which are refused, so they're queued and persisted */
static int queue_messages(MQTTAsync c, int MQTTVersion, const char* topic_base)
{
	char topic_filter[100];
	char topic[100];
	int i;

	snprintf(topic_filter, sizeof(topic_filter), "%s#", topic_base);
	if (subscribe(c, MQTTVersion, topic_filter) != MQTTASYNC_SUCCESS)
		return MQTTASYNC_FAILURE;
	for (i = 0; i < MESSAGE_COUNT; ++i)
	{
		snprintf(topic, sizeof(topic), "%s%s", topic_base, messages[i].subtopic);
		if (publish_message(c, MQTTVersion, topic, i) != MQTTASYNC_SUCCESS)
			return MQTTASYNC_FAILURE;
	}
	WAIT_UNTIL(store_queued(&store) == MESSAGE_COUNT);
	assert("all messages persisted in the queue", store_queued(&store) == MESSAGE_COUNT,
		"%d messages were persisted, expected %d\n", store_queued(&store), MESSAGE_COUNT);
	return MQTTASYNC_SUCCESS;
}


static int run_test(const char* testname, int MQTTVersion)
{
	MQTTClient_persistence persistence = {&store, pers_open, pers_close, pers_put, pers_get,
		pers_remove, pers_keys, pers_clear, pers_containskey};
	MQTTAsync c = NULL;
	const char* clientid = (MQTTVersion >= MQTTVERSION_5) ? "async queue persistence v5" : "async queue persistence v3";
	const char* topic_base = (MQTTVersion >= MQTTVERSION_5) ? "async_queue_persistence/v5/" : "async_queue_persistence/v3/";
	int i;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;
	store_clear(&store);
	reset_state(0);

	if (create_client(&c, clientid, MQTTVersion, &persistence) != MQTTASYNC_SUCCESS)
		goto exit;

	/* remove any session left on the server by an earlier run */
	if (connect_client(c, MQTTVersion, 1) != MQTTASYNC_SUCCESS)
		goto destroy;
	disconnect_client(c, MQTTVersion);

	/* a non-clean session, so that the client keeps the queued messages when it disconnects */
	if (connect_client(c, MQTTVersion, 0) != MQTTASYNC_SUCCESS)
		goto destroy;
	if (queue_messages(c, MQTTVersion, topic_base) != MQTTASYNC_SUCCESS)
		goto disconnect;
	disconnect_client(c, MQTTVersion);
	MQTTAsync_destroy(&c);

	/* recreating the client restores the queued messages, which are delivered when it connects */
	reset_state(1);
	if (create_client(&c, clientid, MQTTVersion, &persistence) != MQTTASYNC_SUCCESS)
		goto exit;
	if (connect_client(c, MQTTVersion, 0) != MQTTASYNC_SUCCESS)
		goto destroy;

	for (i = 0; i < 500 && (get_state(&state.received_count) < MESSAGE_COUNT || store_queued(&store) > 0); ++i)
	{
		if (i % 5 == 0)
			nudge(c);
		MySleep(10);
	}
	assert("all messages received", get_state(&state.received_count) == MESSAGE_COUNT,
		"%d messages were received, expected %d\n", get_state(&state.received_count), MESSAGE_COUNT);
	Paho_thread_lock_mutex(lock);
	for (i = 0; i < state.received_count; ++i)
		check_message(MQTTVersion, topic_base, i, &state.received[i]);
	Paho_thread_unlock_mutex(lock);
	assert("queued messages removed from persistence", store_queued(&store) == 0,
		"%d messages left\n", store_queued(&store));
	MySleep(100);
	assert("no other messages received", get_state(&state.unexpected) == 0,
		"%d other messages\n", get_state(&state.unexpected));

disconnect:
	disconnect_client(c, MQTTVersion);
	/* don't leave a session on the server */
	if (connect_client(c, MQTTVersion, 1) == MQTTASYNC_SUCCESS)
		disconnect_client(c, MQTTVersion);
destroy:
	MQTTAsync_destroy(&c);
exit:
	store_clear(&store);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}

int test1(struct Options options)
{
	return run_test("test_queue_persistence_async-1 - MQTT 3.1.1, restore queued messages", MQTTVERSION_3_1_1);
}

int test2(struct Options options)
{
	return run_test("test_queue_persistence_async-2 - MQTT 5, restore queued messages with properties", MQTTVERSION_5);
}


int main(int argc, char** argv)
{
	int rc = 0;
	int (*test_funcs[])(struct Options) = {NULL, test1, test2};
	int i;

	getopts(argc, argv);
	lock = Paho_thread_create_mutex(&rc);
	if (lock == NULL)
	{
		MyLog(LOGA_INFO, "Failed to create mutex, rc %d", rc);
		return 1;
	}
	rc = 0;

	if (options.test_no == 0)
	{
		for (i = 1; i < (int)(sizeof(test_funcs) / sizeof(test_funcs[0])); ++i)
			rc += test_funcs[i](options);
	}
	else if (options.test_no < (int)(sizeof(test_funcs) / sizeof(test_funcs[0])))
		rc = test_funcs[options.test_no](options);
	else
		usage();

	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	return rc;
}
