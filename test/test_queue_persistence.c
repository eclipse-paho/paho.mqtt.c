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
 * Test that messages queued for delivery in the synchronous client are restored
 * correctly from persistence.
 * A queued message is persisted as one buffer: the payload length, the payload,
 * qos, retained, dup, msgid, the topic name with its terminating null, the topic
 * length and, for MQTT 5, the properties. The payload and topic lengths used here
 * are not multiples of the size of an int, so the ints after them are not aligned.
 * They used to be read through int pointers, which is undefined behavior, and
 * fails on platforms which require aligned access. Run with
 * -fsanitize=alignment to detect that on platforms which don't.
 * The client subscribes, publishes messages to itself and waits until they are
 * persisted in the message queue, without receiving them. Then it disconnects
 * and is destroyed, and recreated, which restores the queue. The test checks
 * that the restored messages are the ones which were sent.
 * A user persistence implementation in memory is used, so that the test can
 * see when the messages have been queued, and nothing is left on disk.
 * Test 1: MQTT 3.1.1
 * Test 2: MQTT 5, with a user property in each message, which is restored after
 *         the unaligned fields
 * Test 3: corrupt queue entries in persistence, which are too short or have a
 *         topic name which isn't null terminated, must not be read past their end,
 *         and must not stop a valid entry being restored
 */

#include "MQTTClient.h"
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
	printf("  --test_no <n>  1: MQTT 3.1.1; 2: MQTT 5; 3: corrupt entries (default all)\n");
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


/* A user persistence implementation which keeps the data in memory, so that it
 * survives the client being destroyed and recreated.
 * The library frees the buffers returned by get and keys, so they are allocated
 * with MQTTClient_malloc, which works whether the library tracks its heap. */

#define STORE_SIZE 64

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
	e->key = e->data = NULL;
	e->len = 0;
}

static void store_clear(struct store* s)
{
	int i;

	for (i = 0; i < STORE_SIZE; ++i)
		if (s->entries[i].key)
			store_remove_entry(&s->entries[i]);
}

/* the number of messages in the persisted message queue */
static int store_queued(const struct store* s)
{
	int i, count = 0;

	for (i = 0; i < STORE_SIZE; ++i)
		if (s->entries[i].key && (strncmp(s->entries[i].key, "q-", 2) == 0 ||
				strncmp(s->entries[i].key, "q5-", 3) == 0))
			++count;
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
	struct store_entry* e = store_find(s, key);
	size_t keylen = strlen(key) + 1; /* keys are null terminated strings created by the library */
	int i, len = 0;
	char* ptr;

	MyLog(LOGA_DEBUG, "Persisting key %s", key);
	if (e)
		store_remove_entry(e);
	else
	{
		for (i = 0; i < STORE_SIZE && e == NULL; ++i)
			if (s->entries[i].key == NULL)
				e = &s->entries[i];
		if (e == NULL)
			return MQTTCLIENT_PERSISTENCE_ERROR;
	}

	for (i = 0; i < bufcount; ++i)
		len += buflens[i];
	if ((e->key = malloc(keylen)) == NULL || (e->data = malloc(len > 0 ? len : 1)) == NULL)
	{
		store_remove_entry(e);
		return MQTTCLIENT_PERSISTENCE_ERROR;
	}
	memcpy(e->key, key, keylen);
	ptr = e->data;
	for (i = 0; i < bufcount; ++i)
	{
		memcpy(ptr, buffers[i], buflens[i]);
		ptr += buflens[i];
	}
	e->len = len;
	return 0;
}

static int pers_get(void* handle, char* key, char** buffer, int* buflen)
{
	const struct store_entry* e = store_find(handle, key);

	if (e == NULL || (*buffer = MQTTClient_malloc(e->len > 0 ? e->len : 1)) == NULL)
		return MQTTCLIENT_PERSISTENCE_ERROR;
	memcpy(*buffer, e->data, e->len);
	*buflen = e->len;
	return 0;
}

static int pers_remove(void* handle, char* key)
{
	struct store_entry* e = store_find(handle, key);

	MyLog(LOGA_DEBUG, "Removing key %s", key);
	if (e)
		store_remove_entry(e);
	return 0;
}

static int pers_keys(void* handle, char*** keys, int* nkeys)
{
	const struct store* s = handle;
	int i, count = 0;

	*keys = NULL;
	*nkeys = 0;
	for (i = 0; i < STORE_SIZE; ++i)
		if (s->entries[i].key)
			++count;
	if (count == 0)
		return 0;

	if ((*keys = MQTTClient_malloc(count * sizeof(char*))) == NULL)
		return MQTTCLIENT_PERSISTENCE_ERROR;
	for (i = 0; i < STORE_SIZE; ++i)
	{
		if (s->entries[i].key)
		{
			size_t keylen = strlen(s->entries[i].key) + 1; /* stored by pers_put, so null terminated */
			char* key = MQTTClient_malloc(keylen);

			if (key == NULL)
				return MQTTCLIENT_PERSISTENCE_ERROR;
			memcpy(key, s->entries[i].key, keylen);
			(*keys)[(*nkeys)++] = key;
		}
	}
	return 0;
}

static int pers_clear(void* handle)
{
	store_clear(handle);
	return 0;
}

static int pers_containskey(void* handle, char* key)
{
	return store_find(handle, key) ? 0 : MQTTCLIENT_PERSISTENCE_ERROR;
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


static int connect_client(MQTTClient c, int MQTTVersion, int clean)
{
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_connectOptions opts5 = MQTTClient_connectOptions_initializer5;
	int rc;

	if (MQTTVersion >= MQTTVERSION_5)
	{
		opts5.cleanstart = clean;
		MQTTResponse response = MQTTClient_connect5(c, &opts5, NULL, NULL);

		rc = response.reasonCode;
		MQTTResponse_free(response);
	}
	else
	{
		opts.cleansession = clean;
		opts.MQTTVersion = MQTTVersion;
		rc = MQTTClient_connect(c, &opts);
	}
	assert("good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	return rc;
}


static int publish_message(MQTTClient c, int MQTTVersion, const char* topic, int index)
{
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	MQTTClient_deliveryToken token;
	int rc;

	pubmsg.payload = messages[index].payload;
	pubmsg.payloadlen = (int)strlen(messages[index].payload);
	pubmsg.qos = messages[index].qos;
	if (MQTTVersion >= MQTTVERSION_5)
	{
		MQTTResponse response;
		MQTTProperty property;

		property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
		property.value.data.data = "subtopic";
		property.value.data.len = (int)strlen(property.value.data.data);
		property.value.value.data = messages[index].subtopic;
		property.value.value.len = (int)strlen(messages[index].subtopic);
		MQTTProperties_add(&pubmsg.properties, &property);
		response = MQTTClient_publishMessage5(c, topic, &pubmsg, &token);
		rc = response.reasonCode;
		MQTTResponse_free(response);
		MQTTProperties_free(&pubmsg.properties);
	}
	else
		rc = MQTTClient_publishMessage(c, topic, &pubmsg, &token);
	assert("good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc == MQTTCLIENT_SUCCESS)
	{
		rc = MQTTClient_waitForCompletion(c, token, 5000L);
		assert("publish completed", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	}
	return rc;
}


static void check_message(int MQTTVersion, const char* topic_base, int index, const char* topicName, int topicLen,
		const MQTTClient_message* msg)
{
	char expected_topic[100];
	int payloadlen = (int)strlen(messages[index].payload);

	snprintf(expected_topic, sizeof(expected_topic), "%s%s", topic_base, messages[index].subtopic);
	assert("topic restored", strcmp(topicName, expected_topic) == 0,
		"topic was %s, expected %s\n", topicName, expected_topic);
	assert("topic length restored", topicLen == (int)strlen(expected_topic),
		"topic length was %d, expected %d\n", topicLen, (int)strlen(expected_topic));
	assert("payload length restored", msg->payloadlen == payloadlen,
		"payload length was %d, expected %d\n", msg->payloadlen, payloadlen);
	if (msg->payloadlen == payloadlen)
		assert("payload restored", memcmp(msg->payload, messages[index].payload, payloadlen) == 0,
			"payload was %.*s, expected %s\n", msg->payloadlen, (char*)msg->payload, messages[index].payload);
	assert("qos restored", msg->qos == messages[index].qos,
		"qos was %d, expected %d\n", msg->qos, messages[index].qos);
	assert("retained restored", msg->retained == 0, "retained was %d\n", msg->retained);
	assert("dup restored", msg->dup == 0, "dup was %d\n", msg->dup);
	assert("msgid restored", msg->msgid > 0 && msg->msgid <= 65535, "msgid was %d\n", msg->msgid);

	if (MQTTVersion >= MQTTVERSION_5)
	{
		const MQTTProperty* property = MQTTProperties_getProperty(&msg->properties, MQTTPROPERTY_CODE_USER_PROPERTY);
		int len = (int)strlen(messages[index].subtopic);

		assert("user property restored", property != NULL, "properties count was %d\n", msg->properties.count);
		if (property)
			assert("user property value restored", property->value.value.len == len &&
				memcmp(property->value.value.data, messages[index].subtopic, len) == 0,
				"value was %.*s, expected %s\n", property->value.value.len, property->value.value.data,
				messages[index].subtopic);
	}
}


static int subscribe(MQTTClient c, int MQTTVersion, char* topic_filter)
{
	int rc;

	if (MQTTVersion >= MQTTVERSION_5)
	{
		MQTTSubscribe_options subopts = MQTTSubscribe_options_initializer;
		MQTTResponse response = MQTTClient_subscribe5(c, topic_filter, 2, &subopts, NULL);

		rc = (response.reasonCode == MQTTREASONCODE_GRANTED_QOS_2) ? MQTTCLIENT_SUCCESS : response.reasonCode;
		MQTTResponse_free(response);
	}
	else
		rc = MQTTClient_subscribe(c, topic_filter, 2);
	assert("good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	return rc;
}


/* receive the restored messages, which must be the ones sent, in the same order */
static void receive_messages(MQTTClient c, int MQTTVersion, const char* topic_base)
{
	int i;

	for (i = 0; i < MESSAGE_COUNT; ++i)
	{
		MQTTClient_message* msg = NULL;
		char* topicName = NULL;
		int topicLen = 0;
		int rc = MQTTClient_receive(c, &topicName, &topicLen, &msg, 1000L);

		assert("message received", rc == MQTTCLIENT_SUCCESS && msg != NULL,
			"rc was %d, message %p, for message %d\n", rc, (void*)msg, i);
		if (msg == NULL)
			break;
		MyLog(LOGA_DEBUG, "Received message on topic %s", topicName);
		check_message(MQTTVersion, topic_base, i, topicName, topicLen, msg);
		MQTTClient_freeMessage(&msg);
		MQTTClient_free(topicName);
	}
}


static int run_test(char* testname, int MQTTVersion)
{
	MQTTClient_persistence persistence = {&store, pers_open, pers_close, pers_put, pers_get,
		pers_remove, pers_keys, pers_clear, pers_containskey};
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	MQTTClient c = NULL;
	char* clientid = (MQTTVersion >= MQTTVERSION_5) ? "queue persistence v5" : "queue persistence v3";
	char* topic_base = (MQTTVersion >= MQTTVERSION_5) ? "queue_persistence/v5/" : "queue_persistence/v3/";
	char topic_filter[100];
	char topic[100];
	int rc, i;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;
	store_clear(&store);
	snprintf(topic_filter, sizeof(topic_filter), "%s#", topic_base);

	createOpts.MQTTVersion = MQTTVersion;
	rc = MQTTClient_createWithOptions(&c, options.connection, clientid, MQTTCLIENT_PERSISTENCE_USER,
			&persistence, &createOpts);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	/* remove any session left on the server by an earlier run */
	if (connect_client(c, MQTTVersion, 1) != MQTTCLIENT_SUCCESS)
		goto destroy;
	MQTTClient_disconnect(c, 1000);

	/* a non-clean session, so that the client keeps the queued messages when it disconnects */
	if (connect_client(c, MQTTVersion, 0) != MQTTCLIENT_SUCCESS)
		goto destroy;
	if (subscribe(c, MQTTVersion, topic_filter) != MQTTCLIENT_SUCCESS)
		goto disconnect;

	/* the messages are queued for delivery, and persisted, because they aren't received */
	for (i = 0; i < MESSAGE_COUNT; ++i)
	{
		snprintf(topic, sizeof(topic), "%s%s", topic_base, messages[i].subtopic);
		if (publish_message(c, MQTTVersion, topic, i) != MQTTCLIENT_SUCCESS)
			goto disconnect;
	}
	for (i = 0; i < 100 && store_queued(&store) < MESSAGE_COUNT; ++i)
		MQTTClient_yield();
	assert("all messages persisted in the queue", store_queued(&store) == MESSAGE_COUNT,
		"%d messages were persisted, expected %d\n", store_queued(&store), MESSAGE_COUNT);

	MQTTClient_disconnect(c, 1000);
	MQTTClient_destroy(&c);

	/* recreating the client restores the queued messages from persistence */
	rc = MQTTClient_createWithOptions(&c, options.connection, clientid, MQTTCLIENT_PERSISTENCE_USER,
			&persistence, &createOpts);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;
	if (connect_client(c, MQTTVersion, 0) != MQTTCLIENT_SUCCESS)
		goto destroy;

	receive_messages(c, MQTTVersion, topic_base);
	assert("queued messages removed from persistence", store_queued(&store) == 0,
		"%d messages left\n", store_queued(&store));

disconnect:
	MQTTClient_disconnect(c, 1000);
	/* don't leave a session on the server */
	if (connect_client(c, MQTTVersion, 1) == MQTTCLIENT_SUCCESS)
		MQTTClient_disconnect(c, 1000);
destroy:
	MQTTClient_destroy(&c);
exit:
	store_clear(&store);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}

int test1(struct Options options)
{
	return run_test("test_queue_persistence-1 - MQTT 3.1.1, restore queued messages", MQTTVERSION_3_1_1);
}

int test2(struct Options options)
{
	return run_test("test_queue_persistence-2 - MQTT 5, restore queued messages with properties", MQTTVERSION_5);
}


/* Builds a persisted MQTT 3.1.1 queue entry. If the topic is NULL, the buffer ends after the msgid.
 * If terminate is 0, the topic name isn't null terminated and the buffer ends after it. */
static int build_entry(char* buf, int payloadlen, const char* payload, const char* topic, int terminate)
{
	const int ints[4] = {1, 0, 0, 1}; /* qos, retained, dup, msgid */
	int len = 0;

	memcpy(&buf[len], &payloadlen, sizeof(int));
	len += sizeof(int);
	memcpy(&buf[len], payload, strlen(payload));
	len += (int)strlen(payload);
	memcpy(&buf[len], ints, sizeof(ints));
	len += sizeof(ints);
	if (topic)
	{
		int topiclen = (int)strlen(topic);

		memcpy(&buf[len], topic, topiclen + (terminate ? 1 : 0));
		len += topiclen + (terminate ? 1 : 0);
		if (terminate)
		{
			memcpy(&buf[len], &topiclen, sizeof(int));
			len += sizeof(int);
		}
	}
	return len;
}

static void put_entry(char* key, char* buf, int len)
{
	char* buffers[1];
	int buflens[1];
	int rc;

	buffers[0] = buf;
	buflens[0] = len;
	rc = pers_put(&store, key, 1, buffers, buflens);
	assert("entry stored", rc == 0, "rc was %d for key %s\n", rc, key);
}

int test3(struct Options options)
{
	char* testname = "test_queue_persistence-3 - corrupt queue entries";
	MQTTClient_persistence persistence = {&store, pers_open, pers_close, pers_put, pers_get,
		pers_remove, pers_keys, pers_clear, pers_containskey};
	MQTTClient c = NULL;
	MQTTClient_message* msg = NULL;
	char* topic = "queue_persistence/v3/valid";
	char* topicName = NULL;
	char buf[200];
	int topicLen = 0;
	int len, rc;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;
	store_clear(&store);

	/* too short for the payload length */
	put_entry("q-1", "\x01\x00", 2);
	/* the payload length is longer than the buffer */
	len = build_entry(buf, 1000, "abc", topic, 1);
	put_entry("q-2", buf, len);
	/* a negative payload length */
	len = build_entry(buf, -1, "abc", topic, 1);
	put_entry("q-3", buf, len);
	/* the buffer ends before the topic name */
	len = build_entry(buf, 3, "abc", NULL, 1);
	put_entry("q-4", buf, len);
	/* the topic name isn't null terminated */
	len = build_entry(buf, 3, "abc", topic, 0);
	put_entry("q-5", buf, len);
	/* the buffer ends before the topic length */
	len = build_entry(buf, 3, "abc", topic, 1) - (int)sizeof(int);
	put_entry("q-6", buf, len);
	/* a valid entry */
	len = build_entry(buf, 5, "valid", topic, 1);
	put_entry("q-7", buf, len);

	rc = MQTTClient_create(&c, options.connection, "queue persistence corrupt", MQTTCLIENT_PERSISTENCE_USER,
			&persistence);
	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	if (rc != MQTTCLIENT_SUCCESS)
		goto exit;

	/* MQTTClient_receive needs a connection. A clean session would remove the queue. */
	if (connect_client(c, MQTTVERSION_3_1_1, 0) != MQTTCLIENT_SUCCESS)
		goto destroy;

	rc = MQTTClient_receive(c, &topicName, &topicLen, &msg, 1000L);
	assert("valid message received", rc == MQTTCLIENT_SUCCESS && msg != NULL, "rc was %d, message %p\n",
		rc, (void*)msg);
	if (msg)
	{
		assert("topic restored", strcmp(topicName, topic) == 0, "topic was %s\n", topicName);
		assert("payload restored", msg->payloadlen == 5 && memcmp(msg->payload, "valid", 5) == 0,
			"payload length was %d\n", msg->payloadlen);
		MQTTClient_freeMessage(&msg);
		MQTTClient_free(topicName);
		topicName = NULL;
	}

	rc = MQTTClient_receive(c, &topicName, &topicLen, &msg, 100L);
	assert("no other messages restored", rc == MQTTCLIENT_SUCCESS && msg == NULL,
		"rc was %d, received a message on topic %s\n", rc, topicName ? topicName : "(null)");
	if (msg)
	{
		MQTTClient_freeMessage(&msg);
		MQTTClient_free(topicName);
	}

	MQTTClient_disconnect(c, 1000);
	/* don't leave a session on the server */
	if (connect_client(c, MQTTVERSION_3_1_1, 1) == MQTTCLIENT_SUCCESS)
		MQTTClient_disconnect(c, 1000);
destroy:
	MQTTClient_destroy(&c);
exit:
	store_clear(&store);
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}


int main(int argc, char** argv)
{
	int rc = 0;
	int (*test_funcs[])(struct Options) = {NULL, test1, test2, test3};

	getopts(argc, argv);

	if (options.test_no == 0)
	{
		for (int i = 1; i < (int)(sizeof(test_funcs) / sizeof(test_funcs[0])); ++i)
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
