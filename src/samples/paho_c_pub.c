/*******************************************************************************
 * Copyright (c) 2012, 2020 IBM Corp., and others
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
 *    Guilherme Maciel Ferreira - add keep alive option
 *    Ian Craggs - add full capability
 *******************************************************************************/

#include "MQTTAsync.h"
#include "pubsub_opts.h"

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#define sleep Sleep
#else
#include <unistd.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#if defined(_WRS_KERNEL)
#include <OsWrapper.h>
#endif

volatile int toStop = 0;

struct pubsub_opts opts =
{
	1, 0, 0, 0, "\n", 100,  	/* debug/app options */
	NULL, NULL, 1, 0, 0, /* message options */
	MQTTVERSION_DEFAULT, NULL, "paho-c-pub", 0, 0, NULL, NULL, "localhost", "1883", NULL, 10, /* MQTT options */
	NULL, NULL, 0, 0, /* will options */
	0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* TLS options */
	0, {NULL, NULL}, /* MQTT V5 options */
	NULL, NULL, /* HTTP and HTTPS proxies */
};

MQTTAsync_responseOptions pub_opts = MQTTAsync_responseOptions_initializer;
MQTTProperty property;
MQTTProperties props = MQTTProperties_initializer;


void mysleep(int ms)
{
	#if defined(_WIN32)
		Sleep(ms);
	#else
		usleep(ms * 1000);
	#endif
}

void cfinish(int sig)
{
	signal(SIGINT, NULL);
	toStop = 1;
}


int messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* m)
{
	/* not expecting any messages */
	return 1;
}


static int disconnected = 0;

void onDisconnect5(void* context, MQTTAsync_successData5* response)
{
	disconnected = 1;
}

void onDisconnect(void* context, MQTTAsync_successData* response)
{
	disconnected = 1;
}


static int connected = 0;
void myconnect(MQTTAsync client);
int mypublish(MQTTAsync client, int datalen, char* data);

void onConnectFailure5(void* context, MQTTAsync_failureData5* response)
{
	fprintf(stderr, "Connect failed, rc %s reason code %s\n",
		MQTTAsync_strerror(response->code),
		MQTTReasonCode_toString(response->reasonCode));
	connected = -1;

	MQTTAsync client = (MQTTAsync)context;
}

void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	fprintf(stderr, "Connect failed, rc %s\n", response ? MQTTAsync_strerror(response->code) : "none");
	connected = -1;

	MQTTAsync client = (MQTTAsync)context;
}


void onConnect5(void* context, MQTTAsync_successData5* response)
{
	MQTTAsync client = (MQTTAsync)context;
	int rc = 0;

	if (opts.verbose)
		printf("Connected\n");

	if (opts.null_message == 1)
		rc = mypublish(client, 0, "");
	else if (opts.message)
		rc = mypublish(client, (int)strlen(opts.message), opts.message);
	else if (opts.filename)
	{
		int data_len = 0;
		char* buffer = readfile(&data_len, &opts);

		if (buffer == NULL)
			toStop = 1;
		else
		{
			rc = mypublish(client, data_len, buffer);
			free(buffer);
		}
	}

	connected = 1;
}

void onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync client = (MQTTAsync)context;
	int rc = 0;

	if (opts.verbose)
		printf("Connected\n");

	if (opts.null_message == 1)
		rc = mypublish(client, 0, "");
	else if (opts.message)
		rc = mypublish(client, (int)strlen(opts.message), opts.message);
	else if (opts.filename)
	{
		int data_len = 0;
		char* buffer = readfile(&data_len, &opts);

		if (buffer == NULL)
			toStop = 1;
		else
		{
			rc = mypublish(client, data_len, buffer);
			free(buffer);
		}
	}

	connected = 1;
}


static int published = 0;

void onPublishFailure5(void* context, MQTTAsync_failureData5* response)
{
	if (opts.verbose)
		fprintf(stderr, "Publish failed, rc %s reason code %s\n",
				MQTTAsync_strerror(response->code),
				MQTTReasonCode_toString(response->reasonCode));
	published = -1;
}

void onPublishFailure(void* context, MQTTAsync_failureData* response)
{
	if (opts.verbose)
		fprintf(stderr, "Publish failed, rc %s\n", MQTTAsync_strerror(response->code));
	published = -1;
}


void onPublish5(void* context, MQTTAsync_successData5* response)
{
	if (opts.verbose)
		printf("Publish succeeded, reason code %s\n",
				MQTTReasonCode_toString(response->reasonCode));

	if (opts.null_message || opts.message || opts.filename)
		toStop = 1;

	published = 1;
}


void onPublish(void* context, MQTTAsync_successData* response)
{
	if (opts.verbose)
		printf("Publish succeeded\n");

	if (opts.null_message || opts.message || opts.filename)
		toStop = 1;

	published = 1;
}


static int onSSLError(const char *str, size_t len, void *context)
{
	MQTTAsync client = (MQTTAsync)context;
	return fprintf(stderr, "SSL error: %s\n", str);
}

static unsigned int onPSKAuth(const char* hint,
                              char* identity,
                              unsigned int max_identity_len,
                              unsigned char* psk,
                              unsigned int max_psk_len,
                              void* context)
{
	int psk_len;
	int k, n;

	int rc = 0;
	struct pubsub_opts* opts = context;

	/* printf("Trying TLS-PSK auth with hint: %s\n", hint);*/

	if (opts->psk == NULL || opts->psk_identity == NULL)
	{
		/* printf("No PSK entered\n"); */
		goto exit;
	}

	/* psk should be array of bytes. This is a quick and dirty way to
	 * convert hex to bytes without input validation */
	psk_len = (int)strlen(opts->psk) / 2;
	if (psk_len > max_psk_len)
	{
		fprintf(stderr, "PSK too long\n");
		goto exit;
	}
	for (k=0, n=0; k < psk_len; k++, n += 2)
	{
		sscanf(&opts->psk[n], "%2hhx", &psk[k]);
	}

	/* identity should be NULL terminated string */
	strncpy(identity, opts->psk_identity, max_identity_len);
	if (identity[max_identity_len - 1] != '\0')
	{
		fprintf(stderr, "Identity too long\n");
		goto exit;
	}

	/* Function should return length of psk on success. */
	rc = psk_len;

exit:
	return rc;
}

void myconnect(MQTTAsync client)
{
	MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_SSLOptions ssl_opts = MQTTAsync_SSLOptions_initializer;
	MQTTAsync_willOptions will_opts = MQTTAsync_willOptions_initializer;
	int rc = 0;

	if (opts.verbose)
		printf("Connecting\n");
	if (opts.MQTTVersion == MQTTVERSION_5)
	{
		MQTTAsync_connectOptions conn_opts5 = MQTTAsync_connectOptions_initializer5;
		conn_opts = conn_opts5;
		conn_opts.onSuccess5 = onConnect5;
		conn_opts.onFailure5 = onConnectFailure5;
		conn_opts.cleanstart = 1;
	}
	else
	{
		conn_opts.onSuccess = onConnect;
		conn_opts.onFailure = onConnectFailure;
		conn_opts.cleansession = 1;
	}
	conn_opts.keepAliveInterval = opts.keepalive;
	conn_opts.username = opts.username;
	conn_opts.password = opts.password;
	conn_opts.MQTTVersion = opts.MQTTVersion;
	conn_opts.context = client;
	conn_opts.automaticReconnect = 1;
	conn_opts.httpProxy = opts.http_proxy;
	conn_opts.httpsProxy = opts.https_proxy;

	if (opts.will_topic) 	/* will options */
	{
		will_opts.message = opts.will_payload;
		will_opts.topicName = opts.will_topic;
		will_opts.qos = opts.will_qos;
		will_opts.retained = opts.will_retain;
		conn_opts.will = &will_opts;
	}

	if (opts.connection && (strncmp(opts.connection, "ssl://", 6) == 0 ||
			strncmp(opts.connection, "wss://", 6) == 0))
	{
		if (opts.insecure)
			ssl_opts.verify = 0;
		else
			ssl_opts.verify = 1;
		ssl_opts.CApath = opts.capath;
		ssl_opts.keyStore = opts.cert;
		ssl_opts.trustStore = opts.cafile;
		ssl_opts.privateKey = opts.key;
		ssl_opts.privateKeyPassword = opts.keypass;
		ssl_opts.enabledCipherSuites = opts.ciphers;
		ssl_opts.engine = opts.engine;
		ssl_opts.ssl_error_cb = onSSLError;
		ssl_opts.ssl_error_context = client;
		ssl_opts.ssl_psk_cb = onPSKAuth;
		ssl_opts.ssl_psk_context = &opts;
		conn_opts.ssl = &ssl_opts;
	}

	connected = 0;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
	{
		fprintf(stderr, "Failed to start connect, return code %s\n", MQTTAsync_strerror(rc));
		exit(EXIT_FAILURE);
	}
}


int mypublish(MQTTAsync client, int datalen, char* data)
{
	int rc;

	if (opts.verbose)
		printf("Publishing data of length %d\n", datalen);

	rc = MQTTAsync_send(client, opts.topic, datalen, data, opts.qos, opts.retained, &pub_opts);
	if (opts.verbose && rc != MQTTASYNC_SUCCESS && !opts.quiet)
		fprintf(stderr, "Error from MQTTAsync_send: %s\n", MQTTAsync_strerror(rc));

	return rc;
}


void trace_callback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
	fprintf(stderr, "Trace : %d, %s\n", level, message);
}


int main(int argc, char** argv)
{
	MQTTAsync_disconnectOptions disc_opts = MQTTAsync_disconnectOptions_initializer;
	MQTTAsync_createOptions create_opts = MQTTAsync_createOptions_initializer;
	MQTTAsync client;
	char* buffer = NULL;
	char* url = NULL;
	int url_allocated = 0;
	int rc = 0;
	const char* version = NULL;
	const char* program_name = "paho_c_pub";
	MQTTAsync_nameValue* infos = MQTTAsync_getVersionInfo();
#if !defined(_WIN32)
    struct sigaction sa;
#endif

	if (argc < 2)
		usage(&opts, (pubsub_opts_nameValue*)infos, program_name);

	if (getopts(argc, argv, &opts) != 0)
		usage(&opts, (pubsub_opts_nameValue*)infos, program_name);

	if (opts.connection)
		url = opts.connection;
	else
	{
		url = malloc(100);
		url_allocated = 1;
		sprintf(url, "%s:%s", opts.host, opts.port);
	}
	if (opts.verbose)
		printf("URL is %s\n", url);

	if (opts.tracelevel > 0)
	{
		MQTTAsync_setTraceCallback(trace_callback);
		MQTTAsync_setTraceLevel(opts.tracelevel);
	}

	create_opts.sendWhileDisconnected = 1;
	if (opts.MQTTVersion >= MQTTVERSION_5)
		create_opts.MQTTVersion = MQTTVERSION_5;
	rc = MQTTAsync_createWithOptions(&client, url, opts.clientid, MQTTCLIENT_PERSISTENCE_NONE, NULL, &create_opts);
	if (rc != MQTTASYNC_SUCCESS)
	{
		if (!opts.quiet)
			fprintf(stderr, "Failed to create client, return code: %s\n", MQTTAsync_strerror(rc));
		exit(EXIT_FAILURE);
	}

#if defined(_WIN32)
	signal(SIGINT, cfinish);
	signal(SIGTERM, cfinish);
#else
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = cfinish;
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif

	rc = MQTTAsync_setCallbacks(client, client, NULL, messageArrived, NULL);
	if (rc != MQTTASYNC_SUCCESS)
	{
		if (!opts.quiet)
			fprintf(stderr, "Failed to set callbacks, return code: %s\n", MQTTAsync_strerror(rc));
		exit(EXIT_FAILURE);
	}

	if (opts.MQTTVersion >= MQTTVERSION_5)
	{
		pub_opts.onSuccess5 = onPublish5;
		pub_opts.onFailure5 = onPublishFailure5;

		if (opts.message_expiry > 0)
		{
			property.identifier = MQTTPROPERTY_CODE_MESSAGE_EXPIRY_INTERVAL;
			property.value.integer4 = opts.message_expiry;
			MQTTProperties_add(&props, &property);
		}
		if (opts.user_property.name)
		{
			property.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
			property.value.data.data = opts.user_property.name;
			property.value.data.len = (int)strlen(opts.user_property.name);
			property.value.value.data = opts.user_property.value;
			property.value.value.len = (int)strlen(opts.user_property.value);
			MQTTProperties_add(&props, &property);
		}
		pub_opts.properties = props;
	}
	else
	{
		pub_opts.onSuccess = onPublish;
		pub_opts.onFailure = onPublishFailure;
	}

	myconnect(client);

	while (!toStop)
	{
		int data_len = 0;
		int delim_len = 0;

		if (opts.stdin_lines)
		{
			buffer = malloc(opts.maxdatalen);

			delim_len = (int)strlen(opts.delimiter);
			do
			{
				buffer[data_len++] = getchar();
				if (data_len > delim_len)
				{
					if (strncmp(opts.delimiter, &buffer[data_len - delim_len], delim_len) == 0)
						break;
				}
			} while (data_len < opts.maxdatalen);

			rc = mypublish(client, data_len, buffer);
		}
		else
			mysleep(100);
	}

	if (opts.message == 0 && opts.null_message == 0 && opts.filename == 0)
		free(buffer);

	if (opts.MQTTVersion >= MQTTVERSION_5)
		disc_opts.onSuccess5 = onDisconnect5;
	else
		disc_opts.onSuccess = onDisconnect;
	if ((rc = MQTTAsync_disconnect(client, &disc_opts)) != MQTTASYNC_SUCCESS)
	{
		if (!opts.quiet)
			fprintf(stderr, "Failed to start disconnect, return code: %s\n", MQTTAsync_strerror(rc));
		exit(EXIT_FAILURE);
	}

	while (!disconnected)
		mysleep(100);

	MQTTAsync_destroy(&client);

	if (url_allocated)
		free(url);

	return EXIT_SUCCESS;
}


