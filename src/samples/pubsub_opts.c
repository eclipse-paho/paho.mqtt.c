/*******************************************************************************
 * Copyright (c) 2012, 2018 IBM Corp.
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
 *******************************************************************************/

#include "pubsub_opts.h"

#include <string.h>
#include <stdlib.h>


int printVersionInfo(pubsub_opts_nameValue* info)
{
	int rc = 0;

	printf("\nLibrary information:\n");
	while (info->name)
	{
		printf("%s: %s\n", info->name, info->value);
		info++;
		rc = 1;  /* at least one value printed */
	}
	if (rc == 1)
		printf("\n");
	return rc;
}


void usage(struct pubsub_opts* opts, pubsub_opts_nameValue* name_values, const char* program_name)
{
	printf("Eclipse Paho MQTT C %s\n", opts->publisher ? "publisher" : "subscriber");
	printVersionInfo(name_values);

	printf("Usage: %s [topicname] [-t topic] [-c connection] [-h host] [-p port]\n"
		   "       [-q qos] [-i clientid] [-u username] [-P password] [-k keepalive_timeout]\n"
			, program_name);
	printf("       [-V MQTT-version] [--quiet] [--trace trace-level]\n");
	if (opts->publisher)
	{
		printf("       [-r] [-n] [-m message] [-f filename]\n");
		printf("       [--maxdatalen len] [--message-expiry seconds] [--user-property name value]\n");
	}
	else
		printf("       [-R] [--no-delimiter]\n");
	printf("       [--will-topic topic] [--will-payload message] [--will-qos qos] [--will-retain]\n");
	printf("       [--cafile filename] [--capath dirname] [--cert filename] [--key filename]\n"
		   "       [--keypass string] [--ciphers string] [--insecure]");

	printf(
	"\n\n  -t (--topic)        : MQTT topic to %s to\n"
	"  -c (--connection)   : connection string, overrides host/port e.g wss://hostname:port/ws.  Use this option\n"
	"                        rather than host/port to connect with TLS and/or web sockets. No default.\n"
	"  -h (--host)         : host to connect to.  Default is %s.\n"
	"  -p (--port)         : network port to connect to. Default is %s.\n"
	"  -q (--qos)          : MQTT QoS to %s with (0, 1 or 2). Default is %d.\n"
	"  -V (--MQTTversion)  : MQTT version (31, 311, or 5).  Default is 311.\n"
	"  --quiet             : do not print error messages.\n"
	"  --trace             : print internal trace (\"error\", \"min\", \"max\" or \"protocol\").\n",
			opts->publisher ? "publish" : "subscribe", opts->host, opts->port,
			opts->publisher ? "publish" : "subscribe", opts->qos);

	if (opts->publisher)
	{
		printf("  -r (--retained)   : use MQTT retain option.  Default is off.\n");
		printf("  -n (--null-message) : send 0-length message.\n");
		printf("  -m (--message)    : the payload to send.\n");
		printf("  -f (--filename)   : use the contents of the named file as the payload.\n");
	}

	printf(
	"  -i (--clientid)     : MQTT client id. Default is %s.\n"
	"  -u (--username)     : MQTT username. No default.\n"
	"  -P (--password)     : MQTT password. No default.\n"
	"  -k (--keepalive)    : MQTT keepalive timeout value. Default is %d seconds.\n"
	"  --delimiter         : delimiter string.  Default is \\n.\n",
	opts->clientid,  opts->keepalive);

	if (opts->publisher)
	{
		printf("  --maxdatalen        : maximum length of data to read when publishing strings (default is %d)\n",
				opts->maxdatalen);
		printf("  --message-expiry    : MQTT 5 only.  Sets the message expiry property in seconds.\n");
		printf("  --user-property     : MQTT 5 only.  Sets a user property.\n");
	}
	else
	{
		printf("  --no-delimiter      : do not use a delimiter string between messages.\n");
		printf("  -R (--no-retained)  : do not print retained messages.\n");
	}

	printf(
	"  --will-topic        : will topic on connect.  No default.\n"
	"  --will-payload      : will message.  If the will topic is set, but not payload, a null message will be set.\n"
	"  --will-retain       : set the retained flag on the will message.  The default is off.\n"
	"  --will-qos          : the will message QoS.  The default is 0.\n"
	);

	printf(
	"  --cafile            : a filename for the TLS truststore.\n"
	"  --capath            : a directory name containing TLS trusted server certificates.\n"
	"  --cert              : a filename for the TLS keystore containing client certificates.\n"
	"  --key               : client private key file.\n"
	"  --keypass           : password for the client private key file.\n"
	"  --ciphers           : the list of cipher suites that the client will present to the server during\n"
	"                        the TLS handshake.\n"
	"  --insecure          : don't check that the server certificate common name matches the hostname.\n"
	"  --psk               : pre-shared-key in hexadecimal (no leading 0x) \n"
	"  --psk-identity      : client identity string for TLS-PSK mode.\n"
	);

	printf(
	"  --http-proxy        : HTTP proxy string.\n"
	"  --https-proxy       : HTTPS proxy string.\n"
	);

	printf("\nSee http://eclipse.org/paho for more information about the Eclipse Paho project.\n");
	exit(EXIT_FAILURE);
}



int getopts(int argc, char** argv, struct pubsub_opts* opts)
{
	int count = 1;

	if (argv[1][0] != '-')
	{
		opts->topic = argv[1];
		count = 2;
	}

	while (count < argc)
	{
		if (strcmp(argv[count], "--verbose") == 0 || strcmp(argv[count], "-v") == 0)
			opts->verbose = 1;
		else if (strcmp(argv[count], "--quiet") == 0)
			opts->quiet = 1;
		else if (strcmp(argv[count], "--qos") == 0 || strcmp(argv[count], "-q") == 0)
		{
			if (++count < argc)
			{
				if (strcmp(argv[count], "0") == 0)
					opts->qos = 0;
				else if (strcmp(argv[count], "1") == 0)
					opts->qos = 1;
				else if (strcmp(argv[count], "2") == 0)
					opts->qos = 2;
				else
					return 1;
			}
			else
				return 1;
		}
		else if (strcmp(argv[count], "--connection") == 0 || strcmp(argv[count], "-c") == 0)
		{
			if (++count < argc)
				opts->connection = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--host") == 0 || strcmp(argv[count], "-h") == 0)
		{
			if (++count < argc)
				opts->host = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--port") == 0 || strcmp(argv[count], "-p") == 0)
		{
			if (++count < argc)
				opts->port = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--http-proxy") == 0)
		{
			if (++count < argc)
				opts->http_proxy = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--https-proxy") == 0)
		{
			if (++count < argc)
				opts->https_proxy = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--clientid") == 0 || strcmp(argv[count], "-i") == 0)
		{
			if (++count < argc)
				opts->clientid = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--username") == 0 || strcmp(argv[count], "-u") == 0)
		{
			if (++count < argc)
				opts->username = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--password") == 0 || strcmp(argv[count], "-P") == 0)
		{
			if (++count < argc)
				opts->password = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--maxdatalen") == 0)
		{
			if (++count < argc)
				opts->maxdatalen = atoi(argv[count]);
			else
				return 1;
		}
		else if (strcmp(argv[count], "--delimiter") == 0)
		{
			if (++count < argc)
				opts->delimiter = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--no-delimiter") == 0)
			opts->delimiter = NULL;
		else if (strcmp(argv[count], "--keepalive") == 0 || strcmp(argv[count], "-k") == 0)
		{
			if (++count < argc)
				opts->keepalive = atoi(argv[count]);
			else
				return 1;
		}
		else if (strcmp(argv[count], "--topic") == 0 || strcmp(argv[count], "-t") == 0)
		{
			if (++count < argc)
				opts->topic = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--will-topic") == 0)
		{
			if (++count < argc)
				opts->will_topic = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--will-payload") == 0)
		{
			if (++count < argc)
				opts->will_payload = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--will-qos") == 0)
		{
			if (++count < argc)
				opts->will_qos = atoi(argv[count]);
			else
				return 1;
		}
		else if (strcmp(argv[count], "--will-retain") == 0)
		{
			if (++count < argc)
				opts->will_retain = 1;
			else
				return 1;
		}
		else if (strcmp(argv[count], "--insecure") == 0)
			opts->insecure = 1;
		else if (strcmp(argv[count], "--capath") == 0)
		{
			if (++count < argc)
				opts->capath = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--cafile") == 0)
		{
			if (++count < argc)
				opts->cafile = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--cert") == 0)
		{
			if (++count < argc)
				opts->cert = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--key") == 0)
		{
			if (++count < argc)
				opts->key = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--keypass") == 0)
		{
			if (++count < argc)
				opts->keypass = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--ciphers") == 0)
		{
			if (++count < argc)
				opts->ciphers = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--psk") == 0)
		{
			if (++count < argc)
				opts->psk = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "--psk-identity") == 0)
		{
			if (++count < argc)
				opts->psk_identity = argv[count];
			else
				return 1;
		}
		else if (strcmp(argv[count], "-V") == 0)
		{
			if (++count < argc)
			{
				if (strcmp(argv[count], "mqttv31") == 0 || strcmp(argv[count], "31") == 0)
					opts->MQTTVersion = MQTTVERSION_3_1;
				else if (strcmp(argv[count], "mqttv311") == 0 || strcmp(argv[count], "311") == 0)
					opts->MQTTVersion = MQTTVERSION_3_1_1;
				else if (strcmp(argv[count], "mqttv5") == 0 || strcmp(argv[count], "5") == 0)
					opts->MQTTVersion = MQTTVERSION_5;
				else
					return 1;
			}
			else
				return 1;
		}
		else if (strcmp(argv[count], "--trace") == 0)
		{
			if (++count < argc)
			{
				if (strcmp(argv[count], "error") == 0)
					opts->tracelevel = MQTTASYNC_TRACE_ERROR;
				else if (strcmp(argv[count], "protocol") == 0)
					opts->tracelevel = MQTTASYNC_TRACE_PROTOCOL;
				else if (strcmp(argv[count], "min") == 0 || strcmp(argv[count], "on") == 0)
					opts->tracelevel = MQTTASYNC_TRACE_MINIMUM;
				else if (strcmp(argv[count], "max") == 0)
					opts->tracelevel = MQTTASYNC_TRACE_MAXIMUM;
				else
					return 1;
			}
			else
				return 1;
		}
		else if (opts->publisher == 0)
		{
			if (strcmp(argv[count], "--no-retained") == 0 || strcmp(argv[count], "-R") == 0)
				opts->retained = 1;
			else
			{
				fprintf(stderr, "Unknown option %s\n", argv[count]);
				return 1;
			}
		}
		else if (opts->publisher == 1)
		{
			if (strcmp(argv[count], "--retained") == 0 || strcmp(argv[count], "-r") == 0)
				opts->retained = 1;
			else if (strcmp(argv[count], "--user-property") == 0)
			{
				if (count + 2 < argc)
				{
					opts->user_property.name = argv[++count];
					opts->user_property.value = argv[++count];
				}
				else
					return 1;
			}
			else if (strcmp(argv[count], "--message-expiry") == 0)
			{
				if (++count < argc)
					opts->message_expiry = atoi(argv[count]);
				else
					return 1;
			}
			else if (strcmp(argv[count], "-m") == 0 || strcmp(argv[count], "--message") == 0)
			{
				if (++count < argc)
				{
					opts->stdin_lines = 0;
					opts->message = argv[count];
				}
				else
					return 1;
			}
			else if (strcmp(argv[count], "-f") == 0 || strcmp(argv[count], "--filename") == 0)
			{
				if (++count < argc)
				{
					opts->stdin_lines = 0;
					opts->filename = argv[count];
				}
				else
					return 1;
			}
			else if (strcmp(argv[count], "-n") == 0 || strcmp(argv[count], "--null-message") == 0)
			{
				opts->stdin_lines = 0;
				opts->null_message = 1;
			}
			else
			{
				fprintf(stderr, "Unknown option %s\n", argv[count]);
				return 1;
			}
		}
		else
		{
			fprintf(stderr, "Unknown option %s\n", argv[count]);
			return 1;
		}

		count++;
	}

	if (opts->topic == NULL)
		return 1;

	return 0;
}


char* readfile(int* data_len, struct pubsub_opts* opts)
{
	char* buffer = NULL;
	long filesize = 0L;
	FILE* infile = fopen(opts->filename, "rb");

	if (infile == NULL)
	{
		fprintf(stderr, "Can't open file %s\n", opts->filename);
		return NULL;
	}
	fseek(infile, 0, SEEK_END);
	filesize = ftell(infile);
	rewind(infile);

	buffer = malloc(sizeof(char)*filesize);
	if (buffer == NULL)
	{
		fprintf(stderr, "Can't allocate buffer to read file %s\n", opts->filename);
		fclose(infile);
		return NULL;
	}
	*data_len = (int)fread(buffer, 1, filesize, infile);
	if (*data_len != filesize)
	{
		fprintf(stderr, "%d bytes read of %ld expected for file %s\n", *data_len, filesize, opts->filename);
		fclose(infile);
		free(buffer);
		return NULL;
	}

	fclose(infile);
	return buffer;
}


void logProperties(MQTTProperties *props)
{
	int i = 0;

	for (i = 0; i < props->count; ++i)
	{
		int id = props->array[i].identifier;
		const char* name = MQTTPropertyName(id);
		char* intformat = "Property name %s value %d\n";

		switch (MQTTProperty_getType(id))
		{
		case MQTTPROPERTY_TYPE_BYTE:
		  printf(intformat, name, props->array[i].value.byte);
		  break;
		case MQTTPROPERTY_TYPE_TWO_BYTE_INTEGER:
		  printf(intformat, name, props->array[i].value.integer2);
		  break;
		case MQTTPROPERTY_TYPE_FOUR_BYTE_INTEGER:
		  printf(intformat, name, props->array[i].value.integer4);
		  break;
		case MQTTPROPERTY_TYPE_VARIABLE_BYTE_INTEGER:
		  printf(intformat, name, props->array[i].value.integer4);
		  break;
		case MQTTPROPERTY_TYPE_BINARY_DATA:
		case MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING:
		  printf("Property name %s value len %.*s\n", name,
				  props->array[i].value.data.len, props->array[i].value.data.data);
		  break;
		case MQTTPROPERTY_TYPE_UTF_8_STRING_PAIR:
		  printf("Property name %s key %.*s value %.*s\n", name,
			  props->array[i].value.data.len, props->array[i].value.data.data,
		  	  props->array[i].value.value.len, props->array[i].value.value.data);
		  break;
		}
	}
}
