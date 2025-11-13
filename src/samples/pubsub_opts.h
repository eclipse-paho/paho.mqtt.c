/*******************************************************************************
 * Copyright (c) 2012, 2020 IBM Corp.
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
 *******************************************************************************/

#if !defined(PUBSUB_OPTS_H)
#define PUBSUB_OPTS_H

#include "MQTTAsync.h"
#include "MQTTClientPersistence.h"

struct pubsub_opts
{
	/* debug app options */
	int publisher;  /* publisher app? */
	int quiet;
	int verbose;
	int tracelevel;
	char* delimiter;
	int maxdatalen;
	/* message options */
	char* message;
	char* filename;
	int stdin_lines;
	int stdlin_complete;
	int null_message;
	/* MQTT options */
	int MQTTVersion;
	char* topic;
	char* clientid;
	int qos;
	int retained;
	char* username;
	char* password;
	char* host;
	char* port;
	char* connection;
	int keepalive;
	/* will options */
	char* will_topic;
	char* will_payload;
	int will_qos;
	int will_retain;
	/* TLS options */
	int insecure;
	char* capath;
	char* cert;
	char* cafile;
	char* key;
	char* keypass;
	char* ciphers;
	char* psk_identity;
	char* psk;
	/* MQTT V5 options */
	int message_expiry;
	struct {
		char *name;
		char *value;
	} user_property;
	/* HTTP proxies */
	char* http_proxy;
	char* https_proxy;
};

typedef struct
{
	const char* name;
	const char* value;
} pubsub_opts_nameValue;

//void usage(struct pubsub_opts* opts, const char* version, const char* program_name);
void usage(struct pubsub_opts* opts, pubsub_opts_nameValue* name_values, const char* program_name);
int getopts(int argc, char** argv, struct pubsub_opts* opts);
char* readfile(int* data_len, struct pubsub_opts* opts);
void logProperties(MQTTProperties *props);

#endif


