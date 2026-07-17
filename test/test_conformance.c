/*******************************************************************************
 * Copyright (c) 2026 Ian Craggs
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
 *   Ian Craggs - initial implementation and documentation
 *******************************************************************************/

/**
 * @file
 * Black-box tests, using only the public MQTTClient API, whose purpose is to
 * make the paho.mqtt.testing broker (github.com/eclipse-paho/paho.mqtt.testing)
 * exercise specific MQTT 3.1.1 / 5.0 conformance-statement log lines in its own
 * source that the rest of the C client test suite doesn't otherwise reach:
 *
 *  - MQTT-3.8.3-1 / MQTT5-3.8.3-1   empty subscribe topic list
 *  - MQTT-3.12.4-1 / MQTT5-3.1.2-20 keepalive PINGREQ/PINGRESP
 *  - MQTT5-3.1.2-21                 server overriding the requested keepalive
 *  - MQTT5-3.1.3-6 / MQTT5-3.1.3-7  zero-length client id assigned by server
 *  - MQTT5-3.1.4-3                  second client with the same id disconnects the first
 *  - MQTT-3.3.1-11                  zero-byte retained publish deletes a retained message
 *  - MQTT-3.10.4-5                  unsubscribe from a non-subscribed topic still gets an unsuback
 *  - MQTT-2.1.2-6 / MQTT-2.1.2-10   retained flag on store, cleared flag on live delivery
 *  - MQTT-3.1.2-22 / MQTT5-3.1.2-22 password set without username
 *  - MQTT-3.1.3-10                  ordering of multiple user properties on a publish
 */

#include "MQTTClient.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#if !defined(_WINDOWS)
  #include <sys/time.h>
  #include <unistd.h>
#else
  #include <windows.h>
  #define setenv(a, b, c) _putenv_s(a, b)
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

void usage(void)
{
	printf("help!!\n");
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
		count++;
	}
}

#define LOGA_DEBUG 0
#define LOGA_INFO 1

void MyLog(int LOGA_level, char* format, ...)
{
	va_list args;

	if (LOGA_level == LOGA_DEBUG && options.verbose == 0)
		return;

	va_start(args, format);
	vprintf(format, args);
	printf("\n");
	va_end(args);
}

int tests = 0;
int failures = 0;

void myassert(char* filename, int lineno, char* description, int value, char* format, ...)
{
	++tests;
	if (!value)
	{
		va_list args;

		++failures;
		MyLog(LOGA_INFO, "Assertion failed, file %s, line %d, description: %s", filename, lineno, description);
		va_start(args, format);
		vprintf(format, args);
		va_end(args);
	}
	else
		MyLog(LOGA_DEBUG, "Assertion succeeded, file %s, line %d, description: %s", filename, lineno, description);
}

#define assert(a, b, c, d) myassert(__FILE__, __LINE__, a, b, c, d)

#if defined(_WIN32)
#define mysleep(ms) Sleep(ms)
#else
#define mysleep(ms) usleep((ms) * 1000L)
#endif


/*********************************************************************

Test1: subscribe with an empty topic list is rejected by the broker
       [MQTT-3.8.3-1] / [MQTT5-3.8.3-1]

*********************************************************************/
int test_subscribe_empty(struct Options options)
{
	int rc;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	int MQTTVersions[] = { MQTTVERSION_3_1_1, MQTTVERSION_5 };
	size_t i;

	MyLog(LOGA_INFO, "Starting test - subscribe with empty topic list");
	failures = 0;

	for (i = 0; i < ARRAY_SIZE(MQTTVersions); ++i)
	{
		createOpts.MQTTVersion = MQTTVersions[i];
		rc = MQTTClient_createWithOptions(&c, options.connection, "conformance_sub_empty", MQTTCLIENT_PERSISTENCE_NONE,
				NULL, &createOpts);
		assert("Good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

		opts.keepAliveInterval = 20;
		opts.MQTTVersion = MQTTVersions[i];
		opts.cleansession = (MQTTVersions[i] >= MQTTVERSION_5) ? 0 : 1;
		opts.cleanstart = (MQTTVersions[i] >= MQTTVERSION_5) ? 1 : 0;

		if (MQTTVersions[i] >= MQTTVERSION_5)
		{
			MQTTResponse response = MQTTClient_connect5(c, &opts, NULL, NULL);
			rc = response.reasonCode;
		}
		else
			rc = MQTTClient_connect(c, &opts);
		assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

		/* deliberately empty: no topics/qos at all, count == 0 */
		if (MQTTVersions[i] >= MQTTVERSION_5)
		{
			MQTTResponse response = MQTTClient_subscribeMany5(c, 0, NULL, NULL, NULL, NULL);
			rc = response.reasonCode;
		}
		else
			rc = MQTTClient_subscribeMany(c, 0, NULL, NULL);
		MyLog(LOGA_INFO, "subscribeMany with 0 topics returned %d for MQTT version %d", rc, MQTTVersions[i]);

		/* the broker considers this a protocol violation and drops the connection without a suback,
		 * so any subsequent operation on this connection should reflect that */
		mysleep(500);
		assert("Connection dropped after empty subscribe", MQTTClient_isConnected(c) == 0, "isConnected was %d\n",
				MQTTClient_isConnected(c));

		MQTTClient_disconnect(c, 0);
		MQTTClient_destroy(&c);
	}

	MyLog(LOGA_INFO, "TEST: test %s. %d tests run, %d failures.", (failures == 0) ? "passed" : "failed", tests, failures);
	return failures;
}


/*********************************************************************

Test2: an idle connection with a short keepalive causes the client to send
       PINGREQ, and the broker to respond with PINGRESP
       [MQTT-3.12.4-1] / [MQTT5-3.1.2-20]

*********************************************************************/
int test_keepalive_ping(struct Options options)
{
	int rc;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	int MQTTVersions[] = { MQTTVERSION_3_1_1, MQTTVERSION_5 };
	size_t i;

	MyLog(LOGA_INFO, "Starting test - idle connection triggers keepalive ping");
	failures = 0;

	for (i = 0; i < ARRAY_SIZE(MQTTVersions); ++i)
	{
		rc = MQTTClient_create(&c, options.connection, "conformance_keepalive", MQTTCLIENT_PERSISTENCE_NONE, NULL);
		assert("Good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

		opts.keepAliveInterval = 2;
		opts.MQTTVersion = MQTTVersions[i];
		opts.cleansession = (MQTTVersions[i] >= MQTTVERSION_5) ? 0 : 1;
		opts.cleanstart = (MQTTVersions[i] >= MQTTVERSION_5) ? 1 : 0;

		rc = MQTTClient_connect(c, &opts);
		assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

		/* stay idle for a few keepalive intervals: this is a synchronous-mode client (no
		 * setCallbacks call), so per the library's documented usage it must call yield()
		 * or receive() periodically for the internal keepalive ping to actually be sent */
		{
			int count;
			for (count = 0; count < 60; ++count)
			{
				MQTTClient_yield();
				mysleep(100);
			}
		}

		assert("Still connected after idle period covering several keepalive intervals",
				MQTTClient_isConnected(c) == 1, "isConnected was %d\n", MQTTClient_isConnected(c));

		rc = MQTTClient_disconnect(c, 1000);
		assert("Good rc from disconnect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
		MQTTClient_destroy(&c);
	}

	MyLog(LOGA_INFO, "TEST: test %s. %d tests run, %d failures.", (failures == 0) ? "passed" : "failed", tests, failures);
	return failures;
}


/*********************************************************************

Test3: requesting a keepalive longer than the server's configured maximum
       causes the server to return a shorter SERVER_KEEP_ALIVE property
       [MQTT5-3.1.2-21]

*********************************************************************/
int test_server_keepalive_override(struct Options options)
{
	int rc;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	MQTTResponse response = MQTTResponse_initializer;

	MyLog(LOGA_INFO, "Starting test - server keepalive override");
	failures = 0;

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "conformance_server_ka", MQTTCLIENT_PERSISTENCE_NONE,
			NULL, &createOpts);
	assert("Good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	/* the test broker's configured serverKeepAlive is 60 seconds; requesting more than that
	 * should cause it to return SERVER_KEEP_ALIVE in the CONNACK properties */
	opts.keepAliveInterval = 120;
	opts.cleanstart = 1;
	opts.MQTTVersion = MQTTVERSION_5;

	response = MQTTClient_connect5(c, &opts, NULL, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d\n", response.reasonCode);

	if (response.properties != NULL)
	{
		int hasProp = MQTTProperties_hasProperty(response.properties, MQTTPROPERTY_CODE_SERVER_KEEP_ALIVE);
		assert("Server returned a keepalive override", hasProp == 1, "hasProperty was %d\n", hasProp);
		if (hasProp)
		{
			int64_t serverKA = MQTTProperties_getNumericValue(response.properties, MQTTPROPERTY_CODE_SERVER_KEEP_ALIVE);
			assert("Server keepalive is shorter than requested", serverKA > 0 && serverKA < 120,
					"serverKA was %lld\n", (long long)serverKA);
		}
	}
	else
		assert("CONNACK properties should be present", 0, "properties was NULL\n", 0);

	MQTTClient_disconnect(c, 1000);
	MQTTClient_destroy(&c);

	MyLog(LOGA_INFO, "TEST: test %s. %d tests run, %d failures.", (failures == 0) ? "passed" : "failed", tests, failures);
	return failures;
}


/*********************************************************************

Test4: connecting with an empty client id (MQTT 5.0, clean start) causes the
       server to assign one and return it in ASSIGNED_CLIENT_IDENTIFIER
       [MQTT5-3.1.3-6] / [MQTT5-3.1.3-7]

*********************************************************************/
int test_zero_length_clientid(struct Options options)
{
	int rc;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	MQTTResponse response = MQTTResponse_initializer;

	MyLog(LOGA_INFO, "Starting test - zero length client id");
	failures = 0;

	/* MQTTCLIENT_PERSISTENCE_NONE bypasses the C library's own client-side rejection
	 * of an empty client id, which only applies to the default (file) persistence */
	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "", MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
	assert("Good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = MQTTVERSION_5;

	response = MQTTClient_connect5(c, &opts, NULL, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d\n", response.reasonCode);

	if (response.properties != NULL)
	{
		MQTTProperty* prop = MQTTProperties_getProperty(response.properties, MQTTPROPERTY_CODE_ASSIGNED_CLIENT_IDENTIFIER);
		assert("Server assigned a client identifier", prop != NULL, "prop was %p\n", (void*)prop);
		if (prop != NULL)
			assert("Assigned client identifier is non-empty", prop->value.data.len > 0, "len was %d\n", prop->value.data.len);
	}
	else
		assert("CONNACK properties should be present", 0, "properties was NULL\n", 0);

	MQTTClient_disconnect(c, 1000);
	MQTTClient_destroy(&c);

	MyLog(LOGA_INFO, "TEST: test %s. %d tests run, %d failures.", (failures == 0) ? "passed" : "failed", tests, failures);
	return failures;
}


/*********************************************************************

Test5: a second client connecting with the same client id as an existing,
       still-connected client causes the server to disconnect the first
       [MQTT5-3.1.4-3]

*********************************************************************/
static int duplicate_clientid_a_disconnected = 0;

void duplicate_clientid_disconnected(void* context, MQTTProperties* properties, enum MQTTReasonCodes reasonCode)
{
	MyLog(LOGA_DEBUG, "Callback: disconnected, reason code %d", reasonCode);
	duplicate_clientid_a_disconnected = 1;
}

/* a is put into asynchronous mode (via setCallbacks) so that its background thread
 * actually processes the broker's unsolicited DISCONNECT when b takes over its session;
 * this messageArrived callback is otherwise unused here */
int duplicate_clientid_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&message);
	return 1;
}

int test_duplicate_clientid(struct Options options)
{
	int rc;
	MQTTClient a, b;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	MQTTResponse response = MQTTResponse_initializer;
	const char* clientId = "conformance_dup_clientid";

	MyLog(LOGA_INFO, "Starting test - duplicate client id disconnects the old client");
	failures = 0;

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&a, options.connection, clientId, MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
	assert("Good rc from create (a)", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	rc = MQTTClient_createWithOptions(&b, options.connection, clientId, MQTTCLIENT_PERSISTENCE_NONE, NULL, &createOpts);
	assert("Good rc from create (b)", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = MQTTVERSION_5;

	duplicate_clientid_a_disconnected = 0;
	rc = MQTTClient_setCallbacks(a, NULL, NULL, duplicate_clientid_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	rc = MQTTClient_setDisconnected(a, NULL, duplicate_clientid_disconnected);
	assert("Good rc from setDisconnected", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	response = MQTTClient_connect5(a, &opts, NULL, NULL);
	assert("Good rc from connect (a)", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d\n", response.reasonCode);
	assert("a is connected", MQTTClient_isConnected(a) == 1, "isConnected was %d\n", MQTTClient_isConnected(a));

	response = MQTTClient_connect5(b, &opts, NULL, NULL);
	assert("Good rc from connect (b)", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d\n", response.reasonCode);

	{
		int count = 0;
		while (duplicate_clientid_a_disconnected == 0 && ++count < 30)
			mysleep(100);
	}

	assert("a's disconnected callback fired when b connected with the same client id",
			duplicate_clientid_a_disconnected == 1, "a_disconnected was %d\n", duplicate_clientid_a_disconnected);
	assert("b is still connected", MQTTClient_isConnected(b) == 1, "isConnected(b) was %d\n", MQTTClient_isConnected(b));

	MQTTClient_disconnect(b, 1000);
	MQTTClient_destroy(&a);
	MQTTClient_destroy(&b);

	MyLog(LOGA_INFO, "TEST: test %s. %d tests run, %d failures.", (failures == 0) ? "passed" : "failed", tests, failures);
	return failures;
}


/*********************************************************************

Test6: publishing a zero-byte retained message deletes an existing retained
       message on that topic
       [MQTT-3.3.1-11]

*********************************************************************/
static int zero_byte_retained_messages_arrived = 0;

int zero_byte_retained_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s, %d bytes, retained %d",
			topicName, message->payloadlen, message->retained);
	++zero_byte_retained_messages_arrived;
	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&message);
	return 1;
}

int test_zero_byte_retained_delete(struct Options options)
{
	int rc;
	MQTTClient pub, sub;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	const char* topic = "conformance/zero_byte_retained";
	int count;

	MyLog(LOGA_INFO, "Starting test - zero byte retained message deletes retained message");
	failures = 0;
	zero_byte_retained_messages_arrived = 0;

	rc = MQTTClient_create(&pub, options.connection, "conformance_zbr_pub", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("Good rc from create (pub)", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	opts.keepAliveInterval = 20;
	opts.MQTTVersion = MQTTVERSION_3_1_1;
	opts.cleansession = 1;
	opts.cleanstart = 0;
	rc = MQTTClient_connect(pub, &opts);
	assert("Good rc from connect (pub)", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	/* store a real retained message */
	pubmsg.payload = "a retained message";
	pubmsg.payloadlen = (int)strlen(pubmsg.payload);
	pubmsg.qos = 1;
	pubmsg.retained = 1;
	rc = MQTTClient_publishMessage(pub, topic, &pubmsg, NULL);
	assert("Good rc from publish of retained message", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	mysleep(500);

	/* now delete it with a zero-byte retained publish */
	pubmsg.payload = NULL;
	pubmsg.payloadlen = 0;
	rc = MQTTClient_publishMessage(pub, topic, &pubmsg, NULL);
	assert("Good rc from publish of zero-byte retained message", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	mysleep(500);

	/* a fresh subscriber should now get nothing retained on this topic */
	rc = MQTTClient_create(&sub, options.connection, "conformance_zbr_sub", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("Good rc from create (sub)", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	rc = MQTTClient_setCallbacks(sub, NULL, NULL, zero_byte_retained_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	rc = MQTTClient_connect(sub, &opts);
	assert("Good rc from connect (sub)", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	rc = MQTTClient_subscribe(sub, topic, 1);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	count = 0;
	while (zero_byte_retained_messages_arrived == 0 && ++count < 20)
		mysleep(100);

	assert("No retained message should have been delivered after deletion",
			zero_byte_retained_messages_arrived == 0, "messages arrived %d\n", zero_byte_retained_messages_arrived);

	MQTTClient_disconnect(pub, 1000);
	MQTTClient_disconnect(sub, 1000);
	MQTTClient_destroy(&pub);
	MQTTClient_destroy(&sub);

	MyLog(LOGA_INFO, "TEST: test %s. %d tests run, %d failures.", (failures == 0) ? "passed" : "failed", tests, failures);
	return failures;
}


/*********************************************************************

Test7: unsubscribing from a topic the client was never subscribed to still
       results in a normal (non-error) unsuback
       [MQTT-3.10.4-5]

*********************************************************************/
int test_unsubscribe_no_match(struct Options options)
{
	int rc;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	MQTTResponse response = MQTTResponse_initializer;
	MQTTResponse unsub_response = MQTTResponse_initializer;

	MyLog(LOGA_INFO, "Starting test - unsubscribe with no matching subscription");
	failures = 0;

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "conformance_unsub_nomatch", MQTTCLIENT_PERSISTENCE_NONE,
			NULL, &createOpts);
	assert("Good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = MQTTVERSION_5;
	response = MQTTClient_connect5(c, &opts, NULL, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d\n", response.reasonCode);

	/* [MQTT-3.10.4-5] the server must respond to an unsubscribe of a topic with no matching
	 * subscription with an UNSUBACK, using reason code 0x11 (No subscription existed) */
	unsub_response = MQTTClient_unsubscribe5(c, "conformance/never_subscribed_to_this", NULL);
	assert("Reason code is no subscription existed", unsub_response.reasonCode == MQTTREASONCODE_NO_SUBSCRIPTION_FOUND,
			"reasonCode was %d\n", unsub_response.reasonCode);

	MQTTClient_disconnect(c, 1000);
	MQTTClient_destroy(&c);

	MyLog(LOGA_INFO, "TEST: test %s. %d tests run, %d failures.", (failures == 0) ? "passed" : "failed", tests, failures);
	return failures;
}


/*********************************************************************

Test8: a message published with the retained flag set, while a subscriber is
       already subscribed to that topic, is stored as the new retained message
       and delivered without the retained flag set on the live forward
       [MQTT-2.1.2-6] / [MQTT-2.1.2-10]

*********************************************************************/
static int retained_flag_messages_arrived = 0;

int retained_flag_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
	MyLog(LOGA_DEBUG, "Callback: message received on topic %s, retained %d", topicName, message->retained);
	++retained_flag_messages_arrived;
	MQTTClient_free(topicName);
	MQTTClient_freeMessage(&message);
	return 1;
}

int test_retained_flag_live_publish(struct Options options)
{
	int rc;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
	const char* topic = "conformance/retained_flag_live";
	int count;

	MyLog(LOGA_INFO, "Starting test - retain flag on a live publish to an existing subscriber");
	failures = 0;
	retained_flag_messages_arrived = 0;

	rc = MQTTClient_create(&c, options.connection, "conformance_retain_live", MQTTCLIENT_PERSISTENCE_NONE, NULL);
	assert("Good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	rc = MQTTClient_setCallbacks(c, NULL, NULL, retained_flag_messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
	opts.keepAliveInterval = 20;
	opts.MQTTVersion = MQTTVERSION_3_1_1;
	opts.cleansession = 1;
	opts.cleanstart = 0;
	rc = MQTTClient_connect(c, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	/* subscribe first ... */
	rc = MQTTClient_subscribe(c, topic, 1);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	/* ... then publish retained, while still subscribed: a live forward, not a retained replay */
	pubmsg.payload = "live retained publish";
	pubmsg.payloadlen = (int)strlen(pubmsg.payload);
	pubmsg.qos = 1;
	pubmsg.retained = 1;
	rc = MQTTClient_publishMessage(c, topic, &pubmsg, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	count = 0;
	while (retained_flag_messages_arrived == 0 && ++count < 50)
		mysleep(100);

	assert("The live retained publish was delivered", retained_flag_messages_arrived == 1,
			"messages arrived %d\n", retained_flag_messages_arrived);

	MQTTClient_disconnect(c, 1000);
	MQTTClient_destroy(&c);

	MyLog(LOGA_INFO, "TEST: test %s. %d tests run, %d failures.", (failures == 0) ? "passed" : "failed", tests, failures);
	return failures;
}


/*********************************************************************

Test9: a CONNECT with a password but no username is not client-side validated
       by this library, so it reaches the broker as a non-compliant packet
       [MQTT-3.1.2-22] / [MQTT5-3.1.2-22]

*********************************************************************/
int test_password_without_username(struct Options options)
{
	int rc;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	MQTTResponse response = MQTTResponse_initializer;
	int MQTTVersions[] = { MQTTVERSION_3_1_1, MQTTVERSION_5 };
	size_t i;

	MyLog(LOGA_INFO, "Starting test - password without username");
	failures = 0;

	for (i = 0; i < ARRAY_SIZE(MQTTVersions); ++i)
	{
		createOpts.MQTTVersion = MQTTVersions[i];
		rc = MQTTClient_createWithOptions(&c, options.connection, "conformance_pw_no_user", MQTTCLIENT_PERSISTENCE_NONE,
				NULL, &createOpts);
		assert("Good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

		opts.keepAliveInterval = 20;
		opts.MQTTVersion = MQTTVersions[i];
		opts.cleansession = (MQTTVersions[i] >= MQTTVERSION_5) ? 0 : 1;
		opts.cleanstart = (MQTTVersions[i] >= MQTTVERSION_5) ? 1 : 0;
		opts.username = NULL;
		opts.password = "somepassword";

		if (MQTTVersions[i] >= MQTTVERSION_5)
			response = MQTTClient_connect5(c, &opts, NULL, NULL);
		else
		{
			rc = MQTTClient_connect(c, &opts);
			response.reasonCode = (enum MQTTReasonCodes)rc;
		}
		/* the broker treats this as a malformed CONNECT and drops the connection without
		 * ever sending a CONNACK, so the client should report the connect as unsuccessful */
		MyLog(LOGA_INFO, "connect with password but no username returned reasonCode %d for MQTT version %d",
				response.reasonCode, MQTTVersions[i]);
		assert("Connect did not succeed", response.reasonCode != MQTTREASONCODE_SUCCESS,
				"reasonCode was %d\n", response.reasonCode);

		MQTTClient_disconnect(c, 0);
		MQTTClient_destroy(&c);
	}

	MyLog(LOGA_INFO, "TEST: test %s. %d tests run, %d failures.", (failures == 0) ? "passed" : "failed", tests, failures);
	return failures;
}


/*********************************************************************

Test10: a publish with more than one user property causes the broker to
        check that it must maintain the order of user properties
        [MQTT-3.1.3-10]

*********************************************************************/
int test_user_property_order(struct Options options)
{
	int rc;
	MQTTClient c;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer5;
	MQTTClient_createOptions createOpts = MQTTClient_createOptions_initializer;
	MQTTResponse response = MQTTResponse_initializer;
	MQTTProperties props = MQTTProperties_initializer;
	MQTTProperty property;

	MyLog(LOGA_INFO, "Starting test - user property order on publish");
	failures = 0;

	createOpts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTClient_createWithOptions(&c, options.connection, "conformance_user_props", MQTTCLIENT_PERSISTENCE_NONE,
			NULL, &createOpts);
	assert("Good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	opts.keepAliveInterval = 20;
	opts.cleanstart = 1;
	opts.MQTTVersion = MQTTVERSION_5;
	response = MQTTClient_connect5(c, &opts, NULL, NULL);
	assert("Good rc from connect", response.reasonCode == MQTTREASONCODE_SUCCESS, "rc was %d\n", response.reasonCode);

	property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
	property.value.data.data = "key1";
	property.value.data.len = (int)strlen("key1");
	property.value.value.data = "value1";
	property.value.value.len = (int)strlen("value1");
	MQTTProperties_add(&props, &property);

	property.value.data.data = "key2";
	property.value.value.data = "value2";
	MQTTProperties_add(&props, &property);

	response = MQTTClient_publish5(c, "conformance/user_property_order", (int)strlen("payload"), "payload", 0, 0, &props, NULL);
	assert("Good rc from publish with multiple user properties", response.reasonCode == MQTTREASONCODE_SUCCESS,
			"rc was %d\n", response.reasonCode);

	MQTTProperties_free(&props);
	MQTTClient_disconnect(c, 1000);
	MQTTClient_destroy(&c);

	MyLog(LOGA_INFO, "TEST: test %s. %d tests run, %d failures.", (failures == 0) ? "passed" : "failed", tests, failures);
	return failures;
}


int main(int argc, char** argv)
{
	int rc = 0, i;
	int (*testfns[])(struct Options) = { NULL,
		test_subscribe_empty,
		test_keepalive_ping,
		test_server_keepalive_override,
		test_zero_length_clientid,
		test_duplicate_clientid,
		test_zero_byte_retained_delete,
		test_unsubscribe_no_match,
		test_retained_flag_live_publish,
		test_password_without_username,
		test_user_property_order,
	};

	getopts(argc, argv);

	if (options.test_no == 0)
	{
		for (options.test_no = 1; options.test_no < (int)ARRAY_SIZE(testfns); ++options.test_no)
			rc += testfns[options.test_no](options);
	}
	else
		rc = testfns[options.test_no](options);

	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	return rc;
}
