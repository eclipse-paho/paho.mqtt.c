/*******************************************************************************
 * Copyright (c) 2009, 2023 IBM Corp., Ian Craggs and others
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
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *    Ian Craggs, Allan Stockdill-Mander - SSL updates
 *    Ian Craggs - fix for buffer overflow in addressPort bug #433290
 *    Ian Craggs - MQTT 3.1.1 support
 *    Rong Xiang, Ian Craggs - C++ compatibility
 *    Ian Craggs - fix for bug 479376
 *    Ian Craggs - SNI support
 *    Ian Craggs - fix for issue #164
 *    Ian Craggs - fix for issue #179
 *    Ian Craggs - MQTT 5.0 support
 *    Sven Gambel - add generic proxy support
 *******************************************************************************/

/**
 * @file
 * \brief Functions dealing with the MQTT protocol exchanges
 *
 * Some other related functions are in the MQTTProtocolClient module
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "MQTTProtocolOut.h"
#include "StackTrace.h"
#include "Heap.h"
#include "WebSocket.h"
#include "Proxy.h"
#include "Base64.h"

extern ClientStates* bstate;



/**
 * Separates an address:port into two separate values
 * @param[in] uri the input string - hostname:port
 * @param[out] port the returned port integer
 * @param[out] topic optional topic portion of the address starting with '/'
 * @return the address string
 */
size_t MQTTProtocol_addressPort(const char* uri, int* port, const char **topic, int default_port)
{
	char* buf = (char*)uri;
	char* colon_pos;
	size_t len;
	char* topic_pos;

	FUNC_ENTRY;
	colon_pos = strrchr(uri, ':'); /* reverse find to allow for ':' in IPv6 addresses */

	if (uri[0] == '[')
	{  /* ip v6 */
		if (colon_pos < strrchr(uri, ']'))
			colon_pos = NULL;  /* means it was an IPv6 separator, not for host:port */
	}

	if (colon_pos) /* have to strip off the port */
	{
		len = colon_pos - uri;
		*port = atoi(colon_pos + 1);
	}
	else
	{
		len = strlen(buf);
		*port = default_port;
	}

	/* find any topic portion */
	topic_pos = (char*)uri;
	if (colon_pos)
		topic_pos = colon_pos;
	topic_pos = strchr(topic_pos, '/');
	if (topic_pos)
	{
		if (topic)
			*topic = topic_pos;
		if (!colon_pos)
			len = topic_pos - uri;
	}

	if (buf[len - 1] == ']')
	{
		/* we are stripping off the final ], so length is 1 shorter */
		--len;
	}
	FUNC_EXIT;
	return len;
}


/**
 * Allow user or password characters to be expressed in the form of %XX, XX being the
 * hexadecimal value of the character. This will avoid problems when a user code or a password
 * contains a '@' or another special character ('%' included)
 * @param p0 output string
 * @param p1 input string
 * @param basic_auth_in_len
 */
void MQTTProtocol_specialChars(char* p0, char* p1, b64_size_t *basic_auth_in_len)
{
	while (*p1 != '@')
	{
		if (*p1 != '%')
		{
			*p0++ = *p1++;
		}
		else if (isxdigit(*(p1 + 1)) && isxdigit(*(p1 + 2)))
		{
			/* next 2 characters are hexa digits */
			char hex[3];
			p1++;
			hex[0] = *p1++;
			hex[1] = *p1++;
			hex[2] = '\0';
			*p0++ = (char)strtol(hex, 0, 16);
			/* 3 input char => 1 output char */
			*basic_auth_in_len -= 2;
		}
	}
	*p0 = 0x0;
}


/*
 * Examples of proxy settings:
 *   http://your.proxy.server:8080/
 *   http://user:pass@my.proxy.server:8080/
 */
int MQTTProtocol_setHTTPProxy(Clients* aClient, char* source, char** dest, char** auth_dest, char* prefix)
{
	b64_size_t basic_auth_in_len, basic_auth_out_len;
	b64_data_t *basic_auth;
	char *p1;
	int rc = 0;

	if (*auth_dest)
	{
		free(*auth_dest);
		*auth_dest = NULL;
	}

	if (source)
	{
		if ((p1 = strstr(source, prefix)) != NULL) /* skip http:// prefix, if any */
			source += strlen(prefix);
		*dest = source;
		if ((p1 = strchr(source, '@')) != NULL) /* find user.pass separator */
			*dest = p1 + 1;

		if (p1)
		{
			/* basic auth len is string between http:// and @ */
			basic_auth_in_len = (b64_size_t)(p1 - source);
			if (basic_auth_in_len > 0)
			{
				basic_auth = (b64_data_t *)malloc(sizeof(char)*(basic_auth_in_len+1));
				if (!basic_auth)
				{
					rc = PAHO_MEMORY_ERROR;
					goto exit;
				}
				MQTTProtocol_specialChars((char*)basic_auth, source, &basic_auth_in_len);
				basic_auth_out_len = Base64_encodeLength(basic_auth, basic_auth_in_len) + 1; /* add 1 for trailing NULL */
				if ((*auth_dest = (char *)malloc(sizeof(char)*basic_auth_out_len)) == NULL)
				{
					free(basic_auth);
					rc = PAHO_MEMORY_ERROR;
					goto exit;
				}
				Base64_encode(*auth_dest, basic_auth_out_len, basic_auth, basic_auth_in_len);
				free(basic_auth);
			}
		}
	}
exit:
	return rc;
}


/**
 * MQTT outgoing connect processing for a client
 * @param ip_address the TCP address:port to connect to
 * @param aClient a structure with all MQTT data needed
 * @param int ssl
 * @param int MQTTVersion the MQTT version to connect with (3 or 4)
 * @param long timeout how long to wait for a new socket to be created
 * @return return code
 */
#if defined(OPENSSL)
#if defined(__GNUC__) && defined(__linux__)
int MQTTProtocol_connect(const char* ip_address, Clients* aClient, int ssl, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties, long timeout)
#else
int MQTTProtocol_connect(const char* ip_address, Clients* aClient, int ssl, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties)
#endif
#else
#if defined(__GNUC__) && defined(__linux__)
int MQTTProtocol_connect(const char* ip_address, Clients* aClient, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties, long timeout)
#else
int MQTTProtocol_connect(const char* ip_address, Clients* aClient, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties)
#endif
#endif
{
	int rc = 0,
		port;
	size_t addr_len;
	char* p0;

	FUNC_ENTRY;
	aClient->good = 1;

	if (aClient->httpProxy)
		p0 = aClient->httpProxy;
	else
		p0 = getenv("http_proxy");

	if (p0)
	{
		if ((rc = MQTTProtocol_setHTTPProxy(aClient, p0, &aClient->net.http_proxy, &aClient->net.http_proxy_auth, "http://")) != 0)
			goto exit;
		Log(TRACE_PROTOCOL, -1, "Setting http proxy to %s", aClient->net.http_proxy);
		if (aClient->net.http_proxy_auth)
			Log(TRACE_PROTOCOL, -1, "Setting http proxy auth to %s", aClient->net.http_proxy_auth);
	}

#if defined(OPENSSL)
	if (aClient->httpsProxy)
		p0 = aClient->httpsProxy;
	else
		p0 = getenv("https_proxy");

	if (p0)
	{
		if ((rc = MQTTProtocol_setHTTPProxy(aClient, p0, &aClient->net.https_proxy, &aClient->net.https_proxy_auth, "https://")) != 0)
			goto exit;
		Log(TRACE_PROTOCOL, -1, "Setting https proxy to %s", aClient->net.https_proxy);
		if (aClient->net.https_proxy_auth)
			Log(TRACE_PROTOCOL, -1, "Setting https proxy auth to %s", aClient->net.https_proxy_auth);
	}

	if (!ssl && aClient->net.http_proxy) {
#else
	if (aClient->net.http_proxy) {
#endif
		addr_len = MQTTProtocol_addressPort(aClient->net.http_proxy, &port, NULL, PROXY_DEFAULT_PORT);
#if defined(__GNUC__) && defined(__linux__)
		if (timeout < 0)
			rc = -1;
		else
			rc = Socket_new(aClient->net.http_proxy, addr_len, port, &(aClient->net.socket), timeout);
#else
		rc = Socket_new(aClient->net.http_proxy, addr_len, port, &(aClient->net.socket));
#endif
	}
#if defined(OPENSSL)
	else if (ssl && aClient->net.https_proxy) {
		addr_len = MQTTProtocol_addressPort(aClient->net.https_proxy, &port, NULL, PROXY_DEFAULT_PORT);
#if defined(__GNUC__) && defined(__linux__)
		if (timeout < 0)
			rc = -1;
		else
			rc = Socket_new(aClient->net.https_proxy, addr_len, port, &(aClient->net.socket), timeout);
#else
		rc = Socket_new(aClient->net.https_proxy, addr_len, port, &(aClient->net.socket));
#endif
	}
#endif
	else {
#if defined(OPENSSL)
		addr_len = MQTTProtocol_addressPort(ip_address, &port, NULL, ssl ?
				(websocket ? WSS_DEFAULT_PORT : SECURE_MQTT_DEFAULT_PORT) :
				(websocket ? WS_DEFAULT_PORT : MQTT_DEFAULT_PORT) );
#else
		addr_len = MQTTProtocol_addressPort(ip_address, &port, NULL, websocket ? WS_DEFAULT_PORT : MQTT_DEFAULT_PORT);
#endif
#if defined(__GNUC__) && defined(__linux__)
		if (timeout < 0)
			rc = -1;
		else
			rc = Socket_new(ip_address, addr_len, port, &(aClient->net.socket), timeout);
#else
		rc = Socket_new(ip_address, addr_len, port, &(aClient->net.socket));
#endif
	}
	if (rc == EINPROGRESS || rc == EWOULDBLOCK)
		aClient->connect_state = TCP_IN_PROGRESS; /* TCP connect called - wait for connect completion */
	else if (rc == 0)
	{	/* TCP connect completed. If SSL, send SSL connect */
#if defined(OPENSSL)
		if (ssl)
		{
			if (aClient->net.https_proxy) {
				aClient->connect_state = PROXY_CONNECT_IN_PROGRESS;
				rc = Proxy_connect( &aClient->net, 1, ip_address);
			}
			if (rc == 0 && SSLSocket_setSocketForSSL(&aClient->net, aClient->sslopts, ip_address, addr_len) == 1)
			{
				rc = aClient->sslopts->struct_version >= 3 ?
					SSLSocket_connect(aClient->net.ssl, aClient->net.socket, ip_address,
						aClient->sslopts->verify, aClient->sslopts->ssl_error_cb, aClient->sslopts->ssl_error_context) :
					SSLSocket_connect(aClient->net.ssl, aClient->net.socket, ip_address,
						aClient->sslopts->verify, NULL, NULL);
				if (rc == TCPSOCKET_INTERRUPTED)
					aClient->connect_state = SSL_IN_PROGRESS; /* SSL connect called - wait for completion */
			}
			else
				rc = SOCKET_ERROR;
		}
		else if (aClient->net.http_proxy) {
#else
		if (aClient->net.http_proxy) {
#endif
			aClient->connect_state = PROXY_CONNECT_IN_PROGRESS;
			rc = Proxy_connect( &aClient->net, 0, ip_address);
		}
		if ( websocket )
		{
#if defined(OPENSSL)
			rc = WebSocket_connect(&aClient->net, ssl, ip_address);
#else
			rc = WebSocket_connect(&aClient->net, 0, ip_address);
#endif
			if ( rc == TCPSOCKET_INTERRUPTED )
				aClient->connect_state = WEBSOCKET_IN_PROGRESS; /* Websocket connect called - wait for completion */
		}
		if (rc == 0)
		{
			/* Now send the MQTT connect packet */
			if ((rc = MQTTPacket_send_connect(aClient, MQTTVersion, connectProperties, willProperties)) == 0)
				aClient->connect_state = WAIT_FOR_CONNACK; /* MQTT Connect sent - wait for CONNACK */
			else
				aClient->connect_state = NOT_IN_PROGRESS;
		}
	}

exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * Process an incoming pingresp packet for a socket
 * @param pack pointer to the publish packet
 * @param sock the socket on which the packet was received
 * @return completion code
 */
int MQTTProtocol_handlePingresps(void* pack, SOCKET sock)
{
	Clients* client = NULL;
	int rc = TCPSOCKET_COMPLETE;

	FUNC_ENTRY;
	client = (Clients*)(ListFindItem(bstate->clients, &sock, clientSocketCompare)->content);
	Log(LOG_PROTOCOL, 21, NULL, sock, client->clientID);
	client->ping_outstanding = 0;
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * MQTT outgoing subscribe processing for a client
 * @param client the client structure
 * @param topics list of topics
 * @param qoss corresponding list of QoSs
 * @param opts MQTT 5.0 subscribe options
 * @param props MQTT 5.0 subscribe properties
 * @return completion code
 */
int MQTTProtocol_subscribe(Clients* client, List* topics, List* qoss, int msgID,
		MQTTSubscribe_options* opts, MQTTProperties* props)
{
	int rc = 0;

	FUNC_ENTRY;
	rc = MQTTPacket_send_subscribe(topics, qoss, opts, props, msgID, 0, client);
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * Process an incoming suback packet for a socket
 * @param pack pointer to the publish packet
 * @param sock the socket on which the packet was received
 * @return completion code
 */
int MQTTProtocol_handleSubacks(void* pack, SOCKET sock)
{
	Suback* suback = (Suback*)pack;
	Clients* client = NULL;
	int rc = TCPSOCKET_COMPLETE;

	FUNC_ENTRY;
	client = (Clients*)(ListFindItem(bstate->clients, &sock, clientSocketCompare)->content);
	Log(LOG_PROTOCOL, 23, NULL, sock, client->clientID, suback->msgId);
	MQTTPacket_freeSuback(suback);
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * MQTT outgoing unsubscribe processing for a client
 * @param client the client structure
 * @param topics list of topics
 * @return completion code
 */
int MQTTProtocol_unsubscribe(Clients* client, List* topics, int msgID, MQTTProperties* props)
{
	int rc = 0;

	FUNC_ENTRY;
	rc = MQTTPacket_send_unsubscribe(topics, props, msgID, 0, client);
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * Process an incoming unsuback packet for a socket
 * @param pack pointer to the publish packet
 * @param sock the socket on which the packet was received
 * @return completion code
 */
int MQTTProtocol_handleUnsubacks(void* pack, SOCKET sock)
{
	Unsuback* unsuback = (Unsuback*)pack;
	Clients* client = NULL;
	int rc = TCPSOCKET_COMPLETE;

	FUNC_ENTRY;
	client = (Clients*)(ListFindItem(bstate->clients, &sock, clientSocketCompare)->content);
	Log(LOG_PROTOCOL, 24, NULL, sock, client->clientID, unsuback->msgId);
	MQTTPacket_freeUnsuback(unsuback);
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * Process an incoming disconnect packet for a socket
 * @param pack pointer to the disconnect packet
 * @param sock the socket on which the packet was received
 * @return completion code
 */
int MQTTProtocol_handleDisconnects(void* pack, SOCKET sock)
{
	Ack* disconnect = (Ack*)pack;
	Clients* client = NULL;
	int rc = TCPSOCKET_COMPLETE;

	FUNC_ENTRY;
	client = (Clients*)(ListFindItem(bstate->clients, &sock, clientSocketCompare)->content);
	Log(LOG_PROTOCOL, 30, NULL, sock, client->clientID, disconnect->rc);
	MQTTPacket_freeAck(disconnect);
	FUNC_EXIT_RC(rc);
	return rc;
}

