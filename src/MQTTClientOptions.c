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
 * Contributors:
 *    Roman Ondráček - initial implementation
 *******************************************************************************/

/**
 * @file
 * \brief Copying of application supplied client options into library-owned memory
 *
 * The SSL options of MQTTClient and MQTTAsync have the same layout, so both
 * clients share these functions.
 */

#include <stdlib.h>
#include <string.h>

#include "MQTTClientOptions.h"
#include "MQTTProtocolClient.h"
#include "StackTrace.h"

#include "Heap.h" /* must always be the last header as malloc, free are redefined */


/**
 * Validate an ALPN protocol name list and copy it into library-owned memory.
 *
 * The list is in the TLS wire format of RFC 7301, section 3.1: a sequence of
 * protocol names, each prefixed by a single length byte.  Empty names are not
 * allowed, a name must not extend past the end of the list and the whole list
 * must fit the 16-bit length of the ProtocolNameList.  The names are opaque
 * bytes, so the list is not treated as a NUL-terminated string.
 * @param protos the protocol name list, or NULL if ALPN is not used
 * @param protos_len the length of the list in bytes, 0 if ALPN is not used
 * @param copy set to a newly allocated copy of the list, or to NULL if ALPN is not used
 * @return MQTTCLIENT_SUCCESS, MQTTCLIENT_BAD_STRUCTURE if the list is not valid
 * or PAHO_MEMORY_ERROR if the copy cannot be allocated
 */
int MQTTClientOptions_copyALPN(const unsigned char* protos, unsigned int protos_len, unsigned char** copy)
{
	unsigned int pos = 0;
	int rc = MQTTCLIENT_SUCCESS;

	FUNC_ENTRY;
	*copy = NULL;
	if (protos == NULL || protos_len == 0)
		goto exit; /* ALPN not used */

	if (protos_len > 0xFFFF)
	{
		rc = MQTTCLIENT_BAD_STRUCTURE;
		goto exit;
	}
	while (pos < protos_len)
	{
		unsigned int name_len = protos[pos];

		if (name_len == 0 || name_len > protos_len - pos - 1)
		{
			rc = MQTTCLIENT_BAD_STRUCTURE;
			goto exit;
		}
		pos += 1 + name_len;
	}

	if ((*copy = malloc(protos_len)) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	memcpy(*copy, protos, protos_len);
exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * Free SSL options and all the data they own.
 * @param opts the options to free, may be NULL
 */
void MQTTClientOptions_freeSSL(MQTTClient_SSLOptions* opts)
{
	FUNC_ENTRY;
	if (opts == NULL)
		goto exit;
	if (opts->trustStore)
		free((void*)opts->trustStore);
	if (opts->keyStore)
		free((void*)opts->keyStore);
	if (opts->privateKey)
		free((void*)opts->privateKey);
	if (opts->privateKeyPassword)
		free((void*)opts->privateKeyPassword);
	if (opts->enabledCipherSuites)
		free((void*)opts->enabledCipherSuites);
	if (opts->struct_version >= 2 && opts->CApath)
		free((void*)opts->CApath);
	if (opts->struct_version >= 5 && opts->protos)
		free((void*)opts->protos);
	free(opts);
exit:
	FUNC_EXIT;
}


/**
 * Duplicate an optional string.
 * @param src the string to copy, may be NULL
 * @param dest set to the copy, left unchanged if src is NULL
 * @return MQTTCLIENT_SUCCESS or PAHO_MEMORY_ERROR
 */
static int MQTTClientOptions_strdup(const char* src, const char** dest)
{
	if (src == NULL)
		return MQTTCLIENT_SUCCESS;
	return ((*dest = MQTTStrdup(src)) == NULL) ? PAHO_MEMORY_ERROR : MQTTCLIENT_SUCCESS;
}


/**
 * Make a deep copy of application supplied SSL options, so that the library
 * does not depend on the application's memory after connect returns.
 * Only the fields present in src->struct_version are copied.
 * @param src the options supplied by the application
 * @param dest set to the newly allocated copy, or to NULL on failure
 * @return MQTTCLIENT_SUCCESS, MQTTCLIENT_BAD_STRUCTURE if the ALPN list is not valid
 * or PAHO_MEMORY_ERROR if memory cannot be allocated
 */
int MQTTClientOptions_copySSL(const MQTTClient_SSLOptions* src, MQTTClient_SSLOptions** dest)
{
	MQTTClient_SSLOptions* opts = NULL;
	int rc = MQTTCLIENT_SUCCESS;

	FUNC_ENTRY;
	*dest = NULL;
	if ((opts = malloc(sizeof(MQTTClient_SSLOptions))) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	memset(opts, '\0', sizeof(MQTTClient_SSLOptions));
	memcpy(opts->struct_id, src->struct_id, sizeof(opts->struct_id));
	opts->struct_version = src->struct_version;
	opts->enableServerCertAuth = src->enableServerCertAuth;

	if ((rc = MQTTClientOptions_strdup(src->trustStore, &opts->trustStore)) != MQTTCLIENT_SUCCESS ||
		(rc = MQTTClientOptions_strdup(src->keyStore, &opts->keyStore)) != MQTTCLIENT_SUCCESS ||
		(rc = MQTTClientOptions_strdup(src->privateKey, &opts->privateKey)) != MQTTCLIENT_SUCCESS ||
		(rc = MQTTClientOptions_strdup(src->privateKeyPassword, &opts->privateKeyPassword)) != MQTTCLIENT_SUCCESS ||
		(rc = MQTTClientOptions_strdup(src->enabledCipherSuites, &opts->enabledCipherSuites)) != MQTTCLIENT_SUCCESS)
		goto exit;

	if (opts->struct_version >= 1)
		opts->sslVersion = src->sslVersion;
	if (opts->struct_version >= 2)
	{
		opts->verify = src->verify;
		if ((rc = MQTTClientOptions_strdup(src->CApath, &opts->CApath)) != MQTTCLIENT_SUCCESS)
			goto exit;
	}
	if (opts->struct_version >= 3)
	{
		opts->ssl_error_cb = src->ssl_error_cb;
		opts->ssl_error_context = src->ssl_error_context;
	}
	if (opts->struct_version >= 4)
	{
		opts->ssl_psk_cb = src->ssl_psk_cb;
		opts->ssl_psk_context = src->ssl_psk_context;
		opts->disableDefaultTrustStore = src->disableDefaultTrustStore;
	}
	if (opts->struct_version >= 5)
	{
		unsigned char* protos = NULL;

		if ((rc = MQTTClientOptions_copyALPN(src->protos, src->protos_len, &protos)) != MQTTCLIENT_SUCCESS)
			goto exit;
		opts->protos = protos;
		opts->protos_len = protos ? src->protos_len : 0;
	}
exit:
	if (rc == MQTTCLIENT_SUCCESS)
		*dest = opts;
	else
		MQTTClientOptions_freeSSL(opts);
	FUNC_EXIT_RC(rc);
	return rc;
}
