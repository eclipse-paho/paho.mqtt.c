/*******************************************************************************
 * Copyright (c) 2026 IBM Corp. and Ian Craggs
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
 *.  Ian Craggs - initial implementation and documentation
 *******************************************************************************/

/**
 * @file
 * White-box unit tests for internal modules that don't need a live broker
 * connection: Base64, Tree, Proxy (parsing only), MQTTProperties and utf-8.
 * These call internal functions directly (not the public MQTTClient/MQTTAsync
 * API), targeting error/edge paths that the broker-driven integration tests
 * don't reach.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <time.h>
#endif

#include "MQTTAsync.h"
#include "Base64.h"
#include "Tree.h"
#include "Proxy.h"
#include "Socket.h"
#include "SocketBuffer.h"
#include "MQTTProperties.h"
#include "MQTTPacket.h"
#include "utf-8.h"
#include "Heap.h" /* redefines malloc/free as mymalloc/myfree, matching how Proxy.c allocates */

/* internal functions with external linkage that aren't declared in the public headers */
Node* TreeFindContentIndex(Tree* aTree, void* key, int index);
Node* TreeNextElementIndex(Tree* aTree, Node* curnode, int index);
void* TreeRemoveIndex(Tree* aTree, void* content, int index);
int MQTTProperty_write(char** pptr, MQTTProperty* prop);
int MQTTProperty_read(MQTTProperty* prop, char** pptr, char* enddata);

static unsigned int fails = 0u;

#define TEST_EXPECT(name, x) \
	if (!(x)) { fprintf(stderr, "failed test: %s (%s:%d)\n", name, __FILE__, __LINE__); ++fails; }


/* ---------------------------------------------------------------------- */
/* Base64 */
/* ---------------------------------------------------------------------- */

static void test_base64(void)
{
	char out[512];
	b64_size_t r;

	/* round trip, including edge lengths that leave 1 or 2 bytes of padding */
	const char* inputs[] = { "", "p", "pa", "pah", "paho", "paho with websockets." };
	int i;

	for (i = 0; i < (int)(sizeof(inputs) / sizeof(inputs[0])); ++i)
	{
		char encoded[512];
		char decoded[512];
		b64_size_t inlen = (b64_size_t)strlen(inputs[i]);
		b64_size_t enclen;

		enclen = Base64_encode(encoded, sizeof(encoded), (const b64_data_t*)inputs[i], inlen);
		TEST_EXPECT("base64 encode length matches Base64_encodeLength",
				enclen == Base64_encodeLength((const b64_data_t*)inputs[i], inlen));

		r = Base64_decode((b64_data_t*)decoded, sizeof(decoded), encoded, enclen);
		TEST_EXPECT("base64 decode length matches Base64_decodeLength",
				r == Base64_decodeLength(encoded, enclen));
		TEST_EXPECT("base64 round trip content matches",
				r == inlen && memcmp(decoded, inputs[i], inlen) == 0);
	}

#if !defined(_WIN32)
	/* decode: input length not a multiple of 4 - too short to decode anything */
	r = Base64_decode((b64_data_t*)out, sizeof(out), "cGE", 3);
	TEST_EXPECT("base64 decode with in_len < 4 returns 0", r == 0);

	/* decode: output buffer too small for the first byte only ("cGE=" -> "pa") */
	r = Base64_decode((b64_data_t*)out, 1, "cGE=", 4);
	TEST_EXPECT("base64 decode stops when out_len exhausted after 1st byte", r == 1);

	/* decode: output buffer holds exactly 2 of the 3 decoded bytes ("cGFo" -> "pah") */
	r = Base64_decode((b64_data_t*)out, 2, "cGFo", 4);
	TEST_EXPECT("base64 decode stops when out_len exhausted after 2nd byte", r == 2);

	/* decode: invalid character in the 3rd position of a quad */
	r = Base64_decode((b64_data_t*)out, sizeof(out), "AB!D", 4);
	TEST_EXPECT("base64 decode with invalid 3rd char stops at 1 byte", r == 1);

	/* decode: invalid character in the 4th position of a quad */
	r = Base64_decode((b64_data_t*)out, sizeof(out), "ABC!", 4);
	TEST_EXPECT("base64 decode with invalid 4th char stops at 2 bytes", r == 2);
#endif

	/* decodeLength / encodeLength edge cases (NULL / short inputs) */
	TEST_EXPECT("base64 decodeLength NULL input", Base64_decodeLength(NULL, 4) == 3);
	TEST_EXPECT("base64 decodeLength zero length", Base64_decodeLength("cGE=", 0) == 0);
	TEST_EXPECT("base64 decodeLength length 1, no padding check on 2nd-last byte",
			Base64_decodeLength("c", 1) == 0);
	TEST_EXPECT("base64 decodeLength one padding char", Base64_decodeLength("cGE=", 4) == 2);
	TEST_EXPECT("base64 decodeLength two padding chars", Base64_decodeLength("cA==", 4) == 1);
	TEST_EXPECT("base64 decodeLength no padding", Base64_decodeLength("cGFo", 4) == 3);
}


/* ---------------------------------------------------------------------- */
/* Tree */
/* ---------------------------------------------------------------------- */

static void test_tree(void)
{
	Tree* t;
	Tree stackTree;
	Node* n;
	int values[] = { 5, 3, 8, 1, 4, 7, 9, 2, 6, 0 };
	int i, prev, count;
	const char* strs[] = { "banana", "apple", "cherry" };

	/* direct compare function checks (note: these return -1 when the first arg is
	 * the *greater* one - an inverted convention relative to strcmp, but self-consistent
	 * for how the tree uses it) */
	{
		int a = 1, b = 2;
		TEST_EXPECT("TreeIntCompare with a < b", TreeIntCompare(&a, &b, 0) == 1);
		TEST_EXPECT("TreeIntCompare with a > b", TreeIntCompare(&b, &a, 0) == -1);
		TEST_EXPECT("TreeIntCompare equal", TreeIntCompare(&a, &a, 0) == 0);
	}
	{
		int a = 1, b = 2;
		TEST_EXPECT("TreePtrCompare with a > b", TreePtrCompare(&a, &b, 0) == (&a > &b ? -1 : 1));
		TEST_EXPECT("TreePtrCompare equal", TreePtrCompare(&a, &a, 0) == 0);
	}
	TEST_EXPECT("TreeStringCompare less", TreeStringCompare("apple", "banana", 0) < 0);
	TEST_EXPECT("TreeStringCompare equal", TreeStringCompare("apple", "apple", 0) == 0);
	TEST_EXPECT("TreeStringCompare greater", TreeStringCompare("banana", "apple", 0) > 0);

	/* dynamically allocated tree, single index, iteration in sorted order */
	t = TreeInitialize(TreeIntCompare);
	TEST_EXPECT("TreeInitialize returns non-NULL", t != NULL);

	for (i = 0; i < (int)(sizeof(values) / sizeof(values[0])); ++i)
		TreeAdd(t, &values[i], sizeof(int));
	TEST_EXPECT("Tree count after adds", t->count == (int)(sizeof(values) / sizeof(values[0])));

	n = TreeFind(t, &values[0]);
	TEST_EXPECT("TreeFind locates an added value", n != NULL && *(int*)(n->content) == values[0]);

	n = NULL;
	prev = -1;
	count = 0;
	while ((n = TreeNextElement(t, n)) != NULL)
	{
		int v = *(int*)(n->content);
		TEST_EXPECT("TreeNextElement returns ascending order", v > prev);
		prev = v;
		++count;
	}
	TEST_EXPECT("TreeNextElement visited every node", count == (int)(sizeof(values) / sizeof(values[0])));

	{
		int key = 4;
		void* removed = TreeRemove(t, &key);
		TEST_EXPECT("TreeRemove finds and removes an existing key", removed != NULL);
		n = TreeFind(t, &key);
		TEST_EXPECT("TreeFind no longer finds removed key", n == NULL);
	}

	TreeFree(t);

	/* stack-allocated tree (no malloc), second index, both indexes ordering the
	 * same string content (content type must be consistent across all indexes
	 * added via TreeAddIndex, since TreeAdd inserts into every index) */
	TreeInitializeNoMalloc(&stackTree, TreeStringCompare);
	TreeAddIndex(&stackTree, TreeStringCompare);
	TEST_EXPECT("TreeAddIndex increments indexes", stackTree.indexes == 2);

	for (i = 0; i < (int)(sizeof(strs) / sizeof(strs[0])); ++i)
		TreeAdd(&stackTree, (void*)strs[i], strlen(strs[i]) + 1);

	n = TreeFindIndex(&stackTree, (void*)"apple", 1);
	TEST_EXPECT("TreeFindIndex on second index finds content",
			n != NULL && strcmp((char*)n->content, "apple") == 0);

	n = TreeFindContentIndex(&stackTree, (void*)"banana", 1);
	TEST_EXPECT("TreeFindContentIndex on second index finds content",
			n != NULL && strcmp((char*)n->content, "banana") == 0);

	n = NULL;
	count = 0;
	while ((n = TreeNextElementIndex(&stackTree, n, 1)) != NULL)
		++count;
	TEST_EXPECT("TreeNextElementIndex walks all nodes on second index",
			count == (int)(sizeof(strs) / sizeof(strs[0])));

	{
		void* removed = TreeRemoveIndex(&stackTree, (void*)"cherry", 1);
		TEST_EXPECT("TreeRemoveIndex removes via second index", removed != NULL);
	}

	/* allow_duplicates: when set, adding a key that already compares equal to an
	 * existing one is silently skipped (unlike the default, which replaces the
	 * existing node's content in place). Use a fresh, single-index int tree. */
	{
		Tree dupTree;
		int dupkey1 = 42, dupkey2 = 42;
		void* addResult;
		Node* found;

		TreeInitializeNoMalloc(&dupTree, TreeIntCompare);
		dupTree.allow_duplicates = 1;

		TreeAdd(&dupTree, &dupkey1, sizeof(int));
		addResult = TreeAdd(&dupTree, &dupkey2, sizeof(int));

		TEST_EXPECT("allow_duplicates skips inserting a second equal key",
				dupTree.count == 1 && addResult == NULL);

		found = TreeFind(&dupTree, &dupkey2);
		TEST_EXPECT("allow_duplicates leaves the original entry's content in place",
				found != NULL && found->content == &dupkey1);
	}
}


/* ---------------------------------------------------------------------- */
/* Proxy (pure parsing functions only - no live connection) */
/* ---------------------------------------------------------------------- */

static void test_proxy(void)
{
	char no_proxy_buf[256];

	strcpy(no_proxy_buf, "example.com");
	TEST_EXPECT("Proxy_noProxy exact match returns 0 (don't use proxy)",
			Proxy_noProxy("example.com", no_proxy_buf) == 0);

	strcpy(no_proxy_buf, "example.com");
	TEST_EXPECT("Proxy_noProxy suffix match returns 0",
			Proxy_noProxy("sub.example.com", no_proxy_buf) == 0);

	strcpy(no_proxy_buf, "example.com");
	TEST_EXPECT("Proxy_noProxy non-matching host returns 1 (use proxy)",
			Proxy_noProxy("notexample.com", no_proxy_buf) == 1);

	strcpy(no_proxy_buf, "*");
	TEST_EXPECT("Proxy_noProxy wildcard matches anything",
			Proxy_noProxy("anything.example.org", no_proxy_buf) == 0);

	strcpy(no_proxy_buf, "foo.com,example.com");
	TEST_EXPECT("Proxy_noProxy matches second entry in comma list",
			Proxy_noProxy("example.com", no_proxy_buf) == 0);

	strcpy(no_proxy_buf, "foo.com,bar.com");
	TEST_EXPECT("Proxy_noProxy with no match at all returns 1",
			Proxy_noProxy("example.com", no_proxy_buf) == 1);

	/* Proxy_setHTTPProxy: no auth */
	{
		char source[] = "http://myproxy.example.com:8080/";
		char* dest = NULL;
		char* auth_dest = NULL;
		int rc = Proxy_setHTTPProxy(NULL, source, &dest, &auth_dest, "http://");

		TEST_EXPECT("Proxy_setHTTPProxy without auth succeeds", rc == 0);
		TEST_EXPECT("Proxy_setHTTPProxy strips the prefix",
				dest != NULL && strcmp(dest, "myproxy.example.com:8080/") == 0);
		TEST_EXPECT("Proxy_setHTTPProxy leaves auth NULL when none is given", auth_dest == NULL);
	}

	/* Proxy_setHTTPProxy: with user:pass auth, verify the encoded auth matches
	 * an independently computed Base64 encoding of "user:pass" */
	{
		char source[] = "http://user:pass@myproxy.example.com:8080/";
		char* dest = NULL;
		char* auth_dest = NULL;
		char expected[64];
		b64_size_t explen;
		int rc;

		explen = Base64_encode(expected, sizeof(expected), (const b64_data_t*)"user:pass", 9);
		expected[explen] = '\0';

		rc = Proxy_setHTTPProxy(NULL, source, &dest, &auth_dest, "http://");

		TEST_EXPECT("Proxy_setHTTPProxy with auth succeeds", rc == 0);
		TEST_EXPECT("Proxy_setHTTPProxy dest starts after the '@'",
				dest != NULL && strcmp(dest, "myproxy.example.com:8080/") == 0);
		TEST_EXPECT("Proxy_setHTTPProxy encodes auth as expected",
				auth_dest != NULL && strcmp(auth_dest, expected) == 0);
		free(auth_dest);
	}

	/* Proxy_setHTTPProxy: NULL source is a safe no-op */
	{
		char* dest = (char*)0x1; /* sentinel: should remain untouched */
		char* auth_dest = NULL;
		int rc = Proxy_setHTTPProxy(NULL, NULL, &dest, &auth_dest, "http://");

		TEST_EXPECT("Proxy_setHTTPProxy with NULL source returns success", rc == 0);
		TEST_EXPECT("Proxy_setHTTPProxy with NULL source doesn't touch dest",
				dest == (char*)0x1);
	}
}


/* ---------------------------------------------------------------------- */
/* Proxy_connect over a real socket pair, with the response fragmented */
/* ---------------------------------------------------------------------- */

#if !defined(_WIN32)

/** Response the fake proxy sends back, followed by a byte of tunnelled data.
 * The trailing marker must survive Proxy_connect untouched: it belongs to the
 * protocol being tunnelled, not to the proxy. */
#define PROXY_REPLY "HTTP/1.1 200 Connection established\r\nX-Pad: abc\r\n\r\n"
#define PROXY_MARKER 0x42

struct proxy_writer_args
{
	int fd;
	size_t first_chunk;	/**< bytes to send before Proxy_connect can see the rest */
};

/** Sends the tail of the response after a delay, so that the first read in
 * Proxy_connect is necessarily short. */
static void* proxy_writer(void* context)
{
	struct proxy_writer_args* args = (struct proxy_writer_args*)context;
	static const char reply[] = PROXY_REPLY;
	const char marker = (char)PROXY_MARKER;
	size_t i;

	/* 100ms, comfortably inside the 250ms retry interval in Proxy_connect */
	usleep(100000);
	for (i = args->first_chunk; i < sizeof(reply) - 1; ++i)
	{
		if (write(args->fd, &reply[i], 1) != 1)
			break;
		usleep(1000);
	}
	if (write(args->fd, &marker, 1) != 1)
		fprintf(stderr, "proxy_writer: failed to write the marker\n");
	return NULL;
}

struct proxy_connect_args
{
	networkHandles* net;
	int rc;
	int done;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

/** Runs the call under test, so that a version of Proxy_connect that fails to
 * terminate shows up as a test failure rather than hanging the test run. */
static void* proxy_connect_runner(void* context)
{
	struct proxy_connect_args* args = (struct proxy_connect_args*)context;
	int rc = Proxy_connect(args->net, 0, "localhost:1883");

	pthread_mutex_lock(&args->mutex);
	args->rc = rc;
	args->done = 1;
	pthread_cond_signal(&args->cond);
	pthread_mutex_unlock(&args->mutex);
	return NULL;
}

static void test_proxy_connect_fragmented(void)
{
	static const char reply[] = PROXY_REPLY;
	/* short enough that the status line cannot be complete on the first read */
	const size_t first_chunk = 5;
	int fds[2];
	networkHandles net;
	struct proxy_writer_args args;
	struct proxy_connect_args call;
	struct timespec deadline;
	pthread_t writer;
	pthread_t runner;
	char leftover = 0;
	ssize_t got;
	int waitrc = 0;
	int done;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
	{
		TEST_EXPECT("socketpair for Proxy_connect test", 0);
		return;
	}

	/* non-blocking, as a connecting socket is in the library, so that a read
	 * with nothing available returns instead of blocking */
	if (fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK) != 0)
	{
		TEST_EXPECT("set Proxy_connect test socket non-blocking", 0);
		close(fds[0]);
		close(fds[1]);
		return;
	}

	memset(&net, '\0', sizeof(net));
	net.socket = fds[0];

	/* Proxy_connect writes through Socket_putdatas and reads through
	 * Socket_getdata, so the socket module's queues have to exist. */
	Socket_outInitialize();
	SocketBuffer_initialize();

	/* the beginning of the status line only - fewer than the 12 bytes that
	 * Proxy_connect asks for */
	if (write(fds[1], reply, first_chunk) != (ssize_t)first_chunk)
	{
		TEST_EXPECT("write first chunk of proxy reply", 0);
		close(fds[0]);
		close(fds[1]);
		return;
	}

	args.fd = fds[1];
	args.first_chunk = first_chunk;
	if (pthread_create(&writer, NULL, proxy_writer, &args) != 0)
	{
		TEST_EXPECT("start proxy reply writer thread", 0);
		close(fds[0]);
		close(fds[1]);
		return;
	}

	call.net = &net;
	call.rc = SOCKET_ERROR;
	call.done = 0;
	pthread_mutex_init(&call.mutex, NULL);
	pthread_cond_init(&call.cond, NULL);

	if (pthread_create(&runner, NULL, proxy_connect_runner, &call) != 0)
	{
		TEST_EXPECT("start Proxy_connect runner thread", 0);
		pthread_join(writer, NULL);
		close(fds[0]);
		close(fds[1]);
		return;
	}

	/* Proxy_connect bounds its own wait at 10 seconds, so anything past that
	 * means it is not going to return at all. */
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 30;
	pthread_mutex_lock(&call.mutex);
	while (!call.done && waitrc == 0)
		waitrc = pthread_cond_timedwait(&call.cond, &call.mutex, &deadline);
	done = call.done;
	pthread_mutex_unlock(&call.mutex);

	TEST_EXPECT("Proxy_connect returns rather than looping forever", done);
	if (!done)
	{
		/* The call is spinning inside the library and still using the socket
		 * buffers, so there is no safe way to carry on in this process. Report
		 * what has been found and stop here rather than letting a stuck thread
		 * corrupt the remaining tests. */
		pthread_detach(runner);
		fprintf(stderr, "%u tests failed\n", fails);
		fflush(NULL);
		_exit((int)fails);
	}

	pthread_join(runner, NULL);
	pthread_join(writer, NULL);

	TEST_EXPECT("Proxy_connect accepts a status line split across reads",
			call.rc != SOCKET_ERROR);

	/* Proxy_connect must stop at the blank line: the tunnelled byte after it
	 * has to still be there, and nothing of the proxy response before it. */
	got = recv(fds[0], &leftover, 1, 0);
	TEST_EXPECT("Proxy_connect leaves the tunnelled data unread",
			got == 1 && leftover == (char)PROXY_MARKER);

	/* and nothing of the proxy response beyond it */
	got = recv(fds[0], &leftover, 1, 0);
	TEST_EXPECT("Proxy_connect consumes the whole proxy response", got <= 0);

	pthread_mutex_destroy(&call.mutex);
	pthread_cond_destroy(&call.cond);
	close(fds[0]);
	close(fds[1]);
}

#endif /* !_WIN32 */


/* ---------------------------------------------------------------------- */
/* utf-8 */
/* ---------------------------------------------------------------------- */

static void test_utf8(void)
{
	/* 1-byte ASCII */
	TEST_EXPECT("utf8 1-byte ascii valid", UTF8_validate(1, "A") == 1);

	/* 2-byte: (c) copyright sign, 0xC2 0xA9 */
	TEST_EXPECT("utf8 2-byte valid", UTF8_validate(2, "\xC2\xA9") == 1);

	/* 3-byte, each of the four sub-ranges in valid_ranges */
	TEST_EXPECT("utf8 3-byte valid (E0 sub-range)", UTF8_validate(3, "\xE0\xA0\x80") == 1);
	TEST_EXPECT("utf8 3-byte valid (E1-EC sub-range)", UTF8_validate(3, "\xE1\x80\x80") == 1);
	TEST_EXPECT("utf8 3-byte valid (ED sub-range, excludes surrogates)",
			UTF8_validate(3, "\xED\x80\x80") == 1);
	TEST_EXPECT("utf8 3-byte valid (EE-EF sub-range)", UTF8_validate(3, "\xEE\x80\x80") == 1);

	/* 4-byte, each of the three sub-ranges */
	TEST_EXPECT("utf8 4-byte valid (F0 sub-range)", UTF8_validate(4, "\xF0\x90\x80\x80") == 1);
	TEST_EXPECT("utf8 4-byte valid (F1-F3 sub-range)", UTF8_validate(4, "\xF1\x80\x80\x80") == 1);
	TEST_EXPECT("utf8 4-byte valid (F4 sub-range)", UTF8_validate(4, "\xF4\x80\x80\x80") == 1);

	/* truncated multi-byte sequences: not enough bytes for the declared length */
	TEST_EXPECT("utf8 truncated 3-byte sequence is invalid", UTF8_validate(2, "\xE0\xA0") == 0);
	TEST_EXPECT("utf8 truncated 4-byte sequence is invalid", UTF8_validate(3, "\xF0\x90\x80") == 0);

	/* invalid continuation byte within a valid lead byte's range */
	TEST_EXPECT("utf8 invalid 2-byte continuation is invalid", UTF8_validate(2, "\xC2\x20") == 0);
	TEST_EXPECT("utf8 invalid 3-byte continuation is invalid", UTF8_validate(3, "\xE0\x20\x80") == 0);

	/* whole-string validation, embedding a valid multi-byte character */
	TEST_EXPECT("UTF8_validateString accepts a valid multi-byte string",
			UTF8_validateString("caf\xC3\xA9") == 1);
	TEST_EXPECT("UTF8_validateString rejects an invalid byte sequence",
			UTF8_validateString("caf\xFF\xFF") == 0);
}


/* ---------------------------------------------------------------------- */
/* MQTTProperties */
/* ---------------------------------------------------------------------- */

static void test_mqtt_properties(void)
{
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperty prop;
	int rc;

	/* invalid property identifier is rejected */
	memset(&prop, 0, sizeof(prop));
	prop.identifier = (enum MQTTPropertyCodes)9999;
	rc = MQTTProperties_add(&props, &prop);
	TEST_EXPECT("MQTTProperties_add rejects an unknown property id", rc == MQTT_INVALID_PROPERTY_ID);

	/* BYTE-type property: write then read back */
	{
		char buf[16];
		char* wptr = buf;
		char* rptr = buf;
		MQTTProperty byteProp;
		MQTTProperty readProp;

		memset(&byteProp, 0, sizeof(byteProp));
		byteProp.identifier = MQTTPROPERTY_CODE_MAXIMUM_QOS;
		byteProp.value.byte = 2;

		MQTTProperty_write(&wptr, &byteProp);
		rc = MQTTProperty_read(&readProp, &rptr, buf + sizeof(buf));
		TEST_EXPECT("MQTTProperty_read of a BYTE property succeeds", rc > 0);
		TEST_EXPECT("MQTTProperty_read of a BYTE property returns the value written",
				readProp.identifier == MQTTPROPERTY_CODE_MAXIMUM_QOS && readProp.value.byte == 2);
	}

	/* truncated buffers: not enough data for each property type */
	{
		char buf[8];
		char* pptr;
		MQTTProperty readProp;

		/* BYTE: identifier present, but no value byte */
		pptr = buf;
		buf[0] = (char)MQTTPROPERTY_CODE_MAXIMUM_QOS;
		rc = MQTTProperty_read(&readProp, &pptr, buf + 1);
		TEST_EXPECT("MQTTProperty_read truncated BYTE value fails", rc == -1);

		/* TWO_BYTE_INTEGER: identifier present, only 1 of 2 value bytes */
		pptr = buf;
		buf[0] = (char)MQTTPROPERTY_CODE_TOPIC_ALIAS;
		buf[1] = 0;
		rc = MQTTProperty_read(&readProp, &pptr, buf + 2);
		TEST_EXPECT("MQTTProperty_read truncated TWO_BYTE_INTEGER fails", rc == -1);

		/* FOUR_BYTE_INTEGER: identifier present, only 2 of 4 value bytes */
		pptr = buf;
		buf[0] = (char)MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
		buf[1] = 0;
		buf[2] = 0;
		rc = MQTTProperty_read(&readProp, &pptr, buf + 3);
		TEST_EXPECT("MQTTProperty_read truncated FOUR_BYTE_INTEGER fails", rc == -1);

		/* UTF-8 string: identifier present, length prefix says 5 bytes but none follow */
		pptr = buf;
		buf[0] = (char)MQTTPROPERTY_CODE_CONTENT_TYPE;
		buf[1] = 0;
		buf[2] = 5;
		rc = MQTTProperty_read(&readProp, &pptr, buf + 3);
		TEST_EXPECT("MQTTProperty_read truncated UTF-8 string fails", rc == -1);

		/* just the 1-byte identifier, nothing else in the buffer at all */
		pptr = buf;
		buf[0] = (char)MQTTPROPERTY_CODE_MAXIMUM_QOS;
		rc = MQTTProperty_read(&readProp, &pptr, buf);
		TEST_EXPECT("MQTTProperty_read with zero remaining data fails", rc == -1);
	}

	/* MQTTProperties_read: declared remaining length longer than the buffer supplied */
	{
		char buf[8];
		char* pptr = buf;
		MQTTProperties readProps = MQTTProperties_initializer;

		buf[0] = 100; /* VBI: claims 100 bytes of properties follow, buffer is much shorter */
		rc = MQTTProperties_read(&readProps, &pptr, buf + 1);
		TEST_EXPECT("MQTTProperties_read with an over-long declared length fails",
				rc == MQTTPACKET_BUFFER_TOO_SHORT);
	}

	/* MQTTProperties_read: more than 10 properties, to exercise array growth (realloc) */
	{
		char buf[256];
		char* wptr;
		char* pptr;
		MQTTProperties writeProps = MQTTProperties_initializer;
		MQTTProperties readProps = MQTTProperties_initializer;
		int i;
		int numProps = 15;

		for (i = 0; i < numProps; ++i)
		{
			MQTTProperty p;
			memset(&p, 0, sizeof(p));
			p.identifier = MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER;
			p.value.integer4 = (unsigned int)(i + 1);
			rc = MQTTProperties_add(&writeProps, &p);
			TEST_EXPECT("MQTTProperties_add succeeds while building the growth test list", rc == 0);
		}

		wptr = buf;
		rc = MQTTProperties_write(&wptr, &writeProps);
		TEST_EXPECT("MQTTProperties_write of >10 properties succeeds", rc > 0);

		/* MQTTProperties_read decodes its own leading VBI length prefix, so pptr
		 * must point at the very start of the buffer written by MQTTProperties_write */
		pptr = buf;
		rc = MQTTProperties_read(&readProps, &pptr, buf + sizeof(buf));
		TEST_EXPECT("MQTTProperties_read of >10 properties succeeds", rc == 1);
		TEST_EXPECT("MQTTProperties_read grows the array to hold all properties",
				readProps.count == numProps && readProps.max_count >= numProps);

		MQTTProperties_free(&writeProps);
		MQTTProperties_free(&readProps);
	}

	/* MQTTProperties_read: a corrupt property (unknown identifier) mid-buffer -> MQTTPACKET_BAD */
	{
		char buf[8];
		char* pptr = buf;
		MQTTProperties readProps = MQTTProperties_initializer;

		buf[0] = 1;    /* VBI: 1 byte of properties follows */
		buf[1] = (char)0x7F; /* an identifier that isn't in the known property table */
		rc = MQTTProperties_read(&readProps, &pptr, buf + 2);
		TEST_EXPECT("MQTTProperties_read with an unknown property id fails", rc == MQTTPACKET_BAD);
	}

	/* MQTTProperties_copy: source list has a manually-injected invalid identifier,
	 * which should be skipped/logged rather than crashing the copy */
	{
		MQTTProperties badProps = MQTTProperties_initializer;
		MQTTProperties copied;
		MQTTProperty p;

		memset(&p, 0, sizeof(p));
		p.identifier = MQTTPROPERTY_CODE_MAXIMUM_QOS;
		p.value.byte = 1;
		MQTTProperties_add(&badProps, &p);

		/* bypass validation: hand-corrupt the stored identifier after adding it */
		badProps.array[0].identifier = (enum MQTTPropertyCodes)9999;

		copied = MQTTProperties_copy(&badProps);
		TEST_EXPECT("MQTTProperties_copy with a corrupt source entry doesn't crash and copies nothing",
				copied.count == 0);

		badProps.array[0].identifier = MQTTPROPERTY_CODE_MAXIMUM_QOS; /* restore so free() is safe */
		MQTTProperties_free(&badProps);
		MQTTProperties_free(&copied);
	}

	/* MQTTProperties_getNumericValueAt: BYTE type, and the "unsupported type" default */
	{
		MQTTProperties numProps = MQTTProperties_initializer;
		MQTTProperty p;
		int64_t v;

		memset(&p, 0, sizeof(p));
		p.identifier = MQTTPROPERTY_CODE_MAXIMUM_QOS; /* BYTE type */
		p.value.byte = 7;
		MQTTProperties_add(&numProps, &p);

		v = MQTTProperties_getNumericValue(&numProps, MQTTPROPERTY_CODE_MAXIMUM_QOS);
		TEST_EXPECT("MQTTProperties_getNumericValue reads a BYTE property", v == 7);

		memset(&p, 0, sizeof(p));
		p.identifier = MQTTPROPERTY_CODE_CONTENT_TYPE; /* UTF-8 string: unsupported for numeric read */
		p.value.data.data = (char*)"text/plain";
		p.value.data.len = (int)strlen("text/plain");
		MQTTProperties_add(&numProps, &p);

		v = MQTTProperties_getNumericValue(&numProps, MQTTPROPERTY_CODE_CONTENT_TYPE);
		TEST_EXPECT("MQTTProperties_getNumericValue on a non-numeric type returns the default", v == -999999);

		MQTTProperties_free(&numProps);
	}

	/* MQTTProperties_getPropertyAt: index > 0 to find the second occurrence */
	{
		MQTTProperties userProps = MQTTProperties_initializer;
		MQTTProperty p;
		MQTTProperty* found;

		memset(&p, 0, sizeof(p));
		p.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
		p.value.data.data = (char*)"k1";
		p.value.data.len = 2;
		p.value.value.data = (char*)"v1";
		p.value.value.len = 2;
		MQTTProperties_add(&userProps, &p);

		p.value.data.data = (char*)"k2";
		p.value.value.data = (char*)"v2";
		MQTTProperties_add(&userProps, &p);

		found = MQTTProperties_getPropertyAt(&userProps, MQTTPROPERTY_CODE_USER_PROPERTY, 1);
		TEST_EXPECT("MQTTProperties_getPropertyAt(index=1) finds the second occurrence",
				found != NULL && strncmp(found->value.data.data, "k2", 2) == 0);

		MQTTProperties_free(&userProps);
	}

	MQTTProperties_free(&props);
}


int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;

	/* initialize heap tracking / mutexes used internally by these modules,
	 * without needing an actual network connection. MQTTAsync_global_init()
	 * alone isn't enough: Heap_initialize() is normally only called lazily from
	 * MQTTAsync_createWithOptions, so it's called explicitly here instead. */
	MQTTAsync_global_init(NULL);
#if !defined(HIGH_PERFORMANCE)
	Heap_initialize();
#endif

	test_base64();
	test_tree();
	test_proxy();
	test_utf8();
	test_mqtt_properties();
#if !defined(_WIN32)
	/* last: on failure this one stops the process, see the comment there */
	test_proxy_connect_fragmented();
#endif

	if (fails)
		printf("%u tests failed\n", fails);
	else
		printf("all tests passed\n");

	return (int)fails;
}
