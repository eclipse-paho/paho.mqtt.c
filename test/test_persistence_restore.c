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
 * Test restoring the synchronous client's state from a persistence store which
 * is damaged, as files can be after a power loss or a full disk. A user
 * persistence implementation in memory returns the entries, so no broker is
 * needed, and each entry is returned in a buffer of exactly its length, so that
 * reading past its end is detected when the test is built with AddressSanitizer.
 * Test 1: persisted packets which are empty, one byte long, or whose remaining
 *         length runs past the end of the data, or past 4 bytes, are skipped,
 *         and a valid one is still restored
 * Test 2: when reading a queued message fails, the buffer and the remaining
 *         keys are freed. Without PAHO_HIGH_PERFORMANCE, the library's heap
 *         tracking shows any memory left allocated; with it, LeakSanitizer
 *         reports leaks when the test is built with it.
 */

#include "MQTTClient.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#if !defined(_WINDOWS)
#include <sys/time.h>
#else
#include <windows.h>
#endif

#if !defined(HIGH_PERFORMANCE)
#include "Heap.h"
#endif

void usage(void)
{
	printf("Options:\n");
	printf("  --test_no <n>  run test number n (default all)\n");
	printf("  --verbose\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	int verbose;
	int test_no;
} options =
{
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


/* A user persistence implementation which returns fixed entries */

struct entry
{
	const char* key;
	const unsigned char* data;
	int len;
	int fail; /* pget allocates the buffer and then fails */
};

struct store
{
	const struct entry* entries;
	int count;
};

static const struct entry* store_find(const struct store* s, const char* key)
{
	int i;

	for (i = 0; i < s->count; ++i)
		if (strcmp(s->entries[i].key, key) == 0)
			return &s->entries[i];
	return NULL;
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
	return 0;
}

static int pers_get(void* handle, char* key, char** buffer, int* buflen)
{
	const struct entry* e = store_find(handle, key);

	/* the library frees the buffer, so it's allocated with MQTTClient_malloc, of exactly the entry's length */
	if (e == NULL || (*buffer = MQTTClient_malloc(e->len > 0 ? e->len : 1)) == NULL)
		return MQTTCLIENT_PERSISTENCE_ERROR;
	memcpy(*buffer, e->data, e->len);
	*buflen = e->len;
	return e->fail ? MQTTCLIENT_PERSISTENCE_ERROR : 0;
}

static int pers_remove(void* handle, char* key)
{
	return 0;
}

static int pers_keys(void* handle, char*** keys, int* nkeys)
{
	const struct store* s = handle;
	int i;

	*keys = NULL;
	*nkeys = 0;
	if (s->count == 0)
		return 0;
	if ((*keys = MQTTClient_malloc(s->count * sizeof(char*))) == NULL)
		return MQTTCLIENT_PERSISTENCE_ERROR;
	for (i = 0; i < s->count; ++i)
	{
		size_t len = strlen(s->entries[i].key) + 1;

		if (((*keys)[i] = MQTTClient_malloc(len)) == NULL)
			return MQTTCLIENT_PERSISTENCE_ERROR;
		memcpy((*keys)[i], s->entries[i].key, len);
		++*nkeys;
	}
	return 0;
}

static int pers_clear(void* handle)
{
	return 0;
}

static int pers_containskey(void* handle, char* key)
{
	return store_find(handle, key) ? 0 : MQTTCLIENT_PERSISTENCE_ERROR;
}

#define PERSISTENCE(store) {store, pers_open, pers_close, pers_put, pers_get, \
	pers_remove, pers_keys, pers_clear, pers_containskey}

/* the library keeps a pointer to the persistence, so it must last as long as the client */
static int create_client(MQTTClient* c, MQTTClient_persistence* persistence)
{
	int rc = MQTTClient_create(c, "tcp://localhost:1883", "persistence restore", MQTTCLIENT_PERSISTENCE_USER,
			persistence);

	assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	return rc;
}

/* the size of the library's heap, or 0 when it isn't tracked */
static size_t heap_size(void)
{
#if !defined(HIGH_PERFORMANCE)
	return Heap_get_info()->current_size;
#else
	return 0;
#endif
}


/* the remaining length of each is wrong, or runs past the end of the data, or past 4 bytes */
static const unsigned char empty[] = {0};
static const unsigned char one_byte[] = {0x32};
static const unsigned char length_past_end[] = {0x32, 0xff};
static const unsigned char length_too_long[] = {0x32, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00};
static const unsigned char length_wrong[] = {0x32, 0x05, 0x00, 0x03, 'a', '/', 'b', 0x00, 0x07};
/* a QoS 1 PUBLISH to a/b, with packet identifier 7 */
static const unsigned char valid_publish[] = {0x32, 0x09, 0x00, 0x03, 'a', '/', 'b', 0x00, 0x07, 'h', 'i'};

int test1(struct Options options)
{
	const char* testname = "test_persistence_restore-1 - damaged persisted packets";
	static const struct entry entries[] =
	{
		{"s-1", empty, 0, 0},
		{"s-2", one_byte, sizeof(one_byte), 0},
		{"s-3", length_past_end, sizeof(length_past_end), 0},
		{"s-4", length_too_long, sizeof(length_too_long), 0},
		{"s-5", length_wrong, sizeof(length_wrong), 0},
		{"s-7", valid_publish, sizeof(valid_publish), 0},
	};
	struct store s = {entries, sizeof(entries) / sizeof(entries[0])};
	MQTTClient_persistence persistence = PERSISTENCE(&s);
	MQTTClient_deliveryToken* tokens = NULL;
	MQTTClient c = NULL;
	int rc, count = 0;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;

	if (create_client(&c, &persistence) != MQTTCLIENT_SUCCESS)
		goto exit;
	rc = MQTTClient_getPendingDeliveryTokens(c, &tokens);
	assert("good rc from getPendingDeliveryTokens", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	while (tokens && tokens[count] != -1)
		++count;
	assert("only the valid publication restored", count == 1 && tokens[0] == 7,
		"%d tokens restored, the first %d\n", count, count > 0 ? tokens[0] : -1);
	if (tokens)
		MQTTClient_free(tokens);
	MQTTClient_destroy(&c);
exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}


int test2(struct Options options)
{
	const char* testname = "test_persistence_restore-2 - failure reading a queued message";
	static const unsigned char data[] = {0x01, 0x02, 0x03, 0x04};
	static const struct entry entries[] =
	{
		{"q-1", data, sizeof(data), 1},
		{"q-2", data, sizeof(data), 0},
		{"q-3", data, sizeof(data), 0},
	};
	struct store s = {entries, sizeof(entries) / sizeof(entries[0])};
	MQTTClient keeper = NULL;
	MQTTClient c = NULL;
	struct store none = {NULL, 0};
	MQTTClient_persistence persistence = PERSISTENCE(&s);
	MQTTClient_persistence keeper_persistence = PERSISTENCE(&none);
	size_t before;

	MyLog(LOGA_INFO, "Starting %s", testname);
	tests = failures = 0;

	/* another client keeps the library initialized, so the heap can be compared before and after */
	if (create_client(&keeper, &keeper_persistence) != MQTTCLIENT_SUCCESS)
		goto exit;
	before = heap_size();
	if (create_client(&c, &persistence) == MQTTCLIENT_SUCCESS)
		MQTTClient_destroy(&c);
	assert("nothing left allocated", heap_size() == before, "%d bytes left allocated\n",
		(int)(heap_size() - before));
	MQTTClient_destroy(&keeper);
exit:
	MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	return failures;
}


int main(int argc, char** argv)
{
	int rc = 0;
	int (*test_funcs[])(struct Options) = {NULL, test1, test2};
	int i;

	getopts(argc, argv);

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
