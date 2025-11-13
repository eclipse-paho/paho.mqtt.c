/*******************************************************************************
 * Copyright (c) 2009, 2025 IBM Corp., Ian Craggs and others
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
 * MQTT outgoing connect processing for a client
 * @param address The address of the server. For TCP this is in the form
 *  			  'address:port; for a UNIX socket it's the path to the
 *  			  socket file, etc.
 * @param aClient a structure with all MQTT data needed
 * @param unixsock Whether the address if for a UNIX-domain socket
 * @param ssl Whether we're connecting with SSL/TLS
 * @param websocket Whether we should use a websocket for the connection
 * @param MQTTVersion the MQTT version to connect with (3, 4, or 5)
 * @param connectProperties The connection properties
 * @param willProperties Properties for the LWT
 * @param timeout how long to wait for a new socket to be created
 * @return return code
 */
#if defined(OPENSSL)
#if defined(__GNUC__) && defined(__linux__)
int MQTTProtocol_connect(const char* address, Clients* aClient, int unixsock, int ssl, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties, long timeout)
#else
int MQTTProtocol_connect(const char* address, Clients* aClient, int unixsock, int ssl, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties)
#endif
#else
#if defined(__GNUC__) && defined(__linux__)
int MQTTProtocol_connect(const char* address, Clients* aClient, int unixsock, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties, long timeout)
#else
int MQTTProtocol_connect(const char* address, Clients* aClient, int unixsock, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties)
#endif
#endif
{
	int rc = 0,
		port;
	size_t addr_len;
	char* p0 = NULL;

	FUNC_ENTRY;
	aClient->good = 1;

	if (!unixsock)
	{
		if (aClient->httpProxy)
			p0 = aClient->httpProxy;
		else /* if the proxy isn't set in the API, then we can look in the environment */
		{
			/* Don't use the environment HTTP proxy settings by default - for backwards compatibility */
			char* use_proxy = getenv("PAHO_C_CLIENT_USE_HTTP_PROXY");
			if (use_proxy)
			{
				if (strncmp(use_proxy, "TRUE", strlen("TRUE")) == 0)
				{
					char* http_proxy = getenv("http_proxy");
					if (http_proxy)
					{
						char* no_proxy = getenv("no_proxy");
						if (no_proxy)
						{
							if (Proxy_noProxy(address, no_proxy))
								p0 = http_proxy;
						}
						else
							p0 = http_proxy; /* no no_proxy set */
					}
				}
			}
		}

		if (p0)
		{
			if ((rc = Proxy_setHTTPProxy(aClient, p0, &aClient->net.http_proxy, &aClient->net.http_proxy_auth, "http://")) != 0)
				goto exit;
			Log(TRACE_PROTOCOL, -1, "Setting http proxy to %s", aClient->net.http_proxy);
			if (aClient->net.http_proxy_auth)
				Log(TRACE_PROTOCOL, -1, "Setting http proxy auth to %s", aClient->net.http_proxy_auth);
		}
	}

#if defined(OPENSSL)
	if (!unixsock)
	{
		if (aClient->httpsProxy)
			p0 = aClient->httpsProxy;
		else /* if the proxy isn't set in the API then we can look in the environment */
		{
			/* Don't use the environment HTTP proxy settings by default - for backwards compatibility */
			char* use_proxy = getenv("PAHO_C_CLIENT_USE_HTTP_PROXY");
			if (use_proxy)
			{
				if (strncmp(use_proxy, "TRUE", strlen("TRUE")) == 0)
				{
					char* https_proxy = getenv("https_proxy");
					if (https_proxy)
					{
						char* no_proxy = getenv("no_proxy");
						if (no_proxy)
						{
							if (Proxy_noProxy(address, no_proxy))
								p0 = https_proxy;
						}
						else
							p0 = https_proxy; /* no no_proxy set */
					}
				}
			}
		}

		if (p0)
		{
			char* prefix = NULL;

			if (memcmp(p0, "http://", 7) == 0)
				prefix = "http://";
			else if (memcmp(p0, "https://", 8) == 0)
				prefix = "https://";
			else
			{
				rc = -1;
				goto exit;
			}

			if ((rc = Proxy_setHTTPProxy(aClient, p0, &aClient->net.https_proxy, &aClient->net.https_proxy_auth, prefix)) != 0)
				goto exit;
			Log(TRACE_PROTOCOL, -1, "Setting https proxy to %s", aClient->net.https_proxy);
			if (aClient->net.https_proxy_auth)
				Log(TRACE_PROTOCOL, -1, "Setting https proxy auth to %s", aClient->net.https_proxy_auth);
		}
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
#if defined(UNIXSOCK)
	else if (unixsock) {
		addr_len = strlen(address);
		rc = Socket_unix_new(address, addr_len, &(aClient->net.socket));
	}
#endif
	else {
#if defined(OPENSSL)
		addr_len = MQTTProtocol_addressPort(address, &port, NULL, ssl ?
				(websocket ? WSS_DEFAULT_PORT : SECURE_MQTT_DEFAULT_PORT) :
				(websocket ? WS_DEFAULT_PORT : MQTT_DEFAULT_PORT) );
#else
		addr_len = MQTTProtocol_addressPort(address, &port, NULL, websocket ? WS_DEFAULT_PORT : MQTT_DEFAULT_PORT);
#endif
#if defined(__GNUC__) && defined(__linux__)
		if (timeout < 0)
			rc = -1;
		else
			rc = Socket_new(address, addr_len, port, &(aClient->net.socket), timeout);
#else
		rc = Socket_new(address, addr_len, port, &(aClient->net.socket));
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
				rc = Proxy_connect( &aClient->net, 1, address);
			}
			if (rc == 0 && SSLSocket_setSocketForSSL(&aClient->net, aClient->sslopts, address, addr_len) == 1)
			{
				rc = aClient->sslopts->struct_version >= 3 ?
					SSLSocket_connect(aClient->net.ssl, aClient->net.socket, address,
						aClient->sslopts->verify, aClient->sslopts->ssl_error_cb, aClient->sslopts->ssl_error_context) :
					SSLSocket_connect(aClient->net.ssl, aClient->net.socket, address,
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
			rc = Proxy_connect( &aClient->net, 0, address);
		}
		if ( websocket )
		{
#if defined(OPENSSL)
			rc = WebSocket_connect(&aClient->net, ssl, address);
#else
			rc = WebSocket_connect(&aClient->net, 0, address);
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
	ListElement* result = NULL;
	int rc = TCPSOCKET_COMPLETE;

	FUNC_ENTRY;
	result = ListFindItem(bstate->clients, &sock, clientSocketCompare);
	if (result)
	{
		client = (Clients*)(result->content);
		Log(LOG_PROTOCOL, 21, NULL, sock, client->clientID);
	}
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
	ListElement* result = NULL;
	int rc = TCPSOCKET_COMPLETE;

	FUNC_ENTRY;
	result = ListFindItem(bstate->clients, &sock, clientSocketCompare);
	if (result)
	{
		client = (Clients*)(result->content);
		Log(LOG_PROTOCOL, 23, NULL, sock, client->clientID, suback->msgId);
	}
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
	ListElement* result = NULL;
	int rc = TCPSOCKET_COMPLETE;

	FUNC_ENTRY;
	result = ListFindItem(bstate->clients, &sock, clientSocketCompare);
	if (result)
	{
		client = (Clients*)(result->content);
		Log(LOG_PROTOCOL, 24, NULL, sock, client->clientID, unsuback->msgId);
	}
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
	ListElement* result = NULL;
	int rc = TCPSOCKET_COMPLETE;

	FUNC_ENTRY;
	result = ListFindItem(bstate->clients, &sock, clientSocketCompare);
	if (result)
	{
		client = (Clients*)(result->content);
		Log(LOG_PROTOCOL, 30, NULL, sock, client->clientID, disconnect->rc);
	}
	MQTTPacket_freeAck(disconnect);
	FUNC_EXIT_RC(rc);
	return rc;
}

