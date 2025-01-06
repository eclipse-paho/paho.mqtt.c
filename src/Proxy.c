/*******************************************************************************
 * Copyright (c) 2009, 2025 Diehl Metering, Ian Craggs and others
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
 *    Keith Holman - initial implementation and documentation
 *    Sven Gambel - move WebSocket proxy support to generic proxy support
 *******************************************************************************/

#include <stdio.h>
#include <string.h>
// for timeout process in Proxy_connect()
#include <time.h>
#if defined(_WIN32) || defined(_WIN64)
	#include <windows.h>
	/* Windows doesn't have strtok_r, so remap it to strtok_s */
	#define strtok_r strtok_s
	#if defined(_MSC_VER) && _MSC_VER < 1900
		#define snprintf _snprintf
	#endif
#else
	#include <unistd.h>
#endif

#include "Log.h"
#include "MQTTProtocolOut.h"
#include "StackTrace.h"
#include "Heap.h"

#if defined(OPENSSL)
#include "SSLSocket.h"
#include <openssl/rand.h>
#endif /* defined(OPENSSL) */
#include "Socket.h"
#include "Base64.h"
#include "ctype.h"


/**
 * Notify the IP address and port of the endpoint to proxy, and wait connection to endpoint.
 *
 * @param[in]  net               network connection to proxy.
 * @param[in]  ssl               enable ssl.
 * @param[in]  hostname          hostname of endpoint.
 *
 * @retval SOCKET_ERROR          failed to network connection
 * @retval 0                     connection to endpoint
 *
 */
int Proxy_connect(networkHandles *net, int ssl, const char *hostname)
{
	int port, i, rc = 0, buf_len=0;
	char *buf = NULL;
	size_t hostname_len, actual_len = 0;
	time_t current, timeout;
	PacketBuffers nulbufs = {0, NULL, NULL, NULL, {0, 0, 0, 0}};

	FUNC_ENTRY;
	hostname_len = MQTTProtocol_addressPort(hostname, &port, NULL, PROXY_DEFAULT_PORT);
	for ( i = 0; i < 2; ++i ) {
#if defined(OPENSSL)
		if(ssl) {
			if (net->https_proxy_auth) {
				buf_len = snprintf( buf, (size_t)buf_len, "CONNECT %.*s:%d HTTP/1.1\r\n"
					"Host: %.*s\r\n"
					"Proxy-authorization: Basic %s\r\n"
					"\r\n",
					(int)hostname_len, hostname, port,
					(int)hostname_len, hostname, net->https_proxy_auth);
			}
			else {
				buf_len = snprintf( buf, (size_t)buf_len, "CONNECT %.*s:%d HTTP/1.1\r\n"
					"Host: %.*s\r\n"
					"\r\n",
					(int)hostname_len, hostname, port,
					(int)hostname_len, hostname);
			}
		}
		else {
#endif
			if (net->http_proxy_auth) {
				buf_len = snprintf( buf, (size_t)buf_len, "CONNECT %.*s:%d HTTP/1.1\r\n"
					"Host: %.*s\r\n"
					"Proxy-authorization: Basic %s\r\n"
					"\r\n",
					(int)hostname_len, hostname, port,
					(int)hostname_len, hostname, net->http_proxy_auth);
			}
			else {
				buf_len = snprintf( buf, (size_t)buf_len, "CONNECT %.*s:%d HTTP/1.1\r\n"
					"Host: %.*s\r\n"
					"\r\n",
					(int)hostname_len, hostname, port,
					(int)hostname_len, hostname);
			}
#if defined(OPENSSL)
		}
#endif
		if ( i==0 && buf_len > 0 ) {
			++buf_len;
			if ((buf = malloc( buf_len )) == NULL)
			{
				rc = PAHO_MEMORY_ERROR;
				goto exit;
			}

		}
	}
	Log(TRACE_PROTOCOL, -1, "Proxy_connect: \"%s\"", buf);

	Socket_putdatas(net->socket, buf, buf_len, nulbufs);
	free(buf);
	buf = NULL;

	time(&timeout);
	timeout += (time_t)10;

	while(1) {
		buf = Socket_getdata(net->socket, (size_t)12, &actual_len, &rc);
		if(actual_len) {
			if ( (strncmp( buf, "HTTP/1.0 200", 12 ) != 0) &&  (strncmp( buf, "HTTP/1.1 200", 12 ) != 0) )
				rc = SOCKET_ERROR;
			break;
		}
		else {
			time(&current);
			if(current > timeout) {
				rc = SOCKET_ERROR;
				break;
			}
#if defined(_WIN32) || defined(_WIN64)
			Sleep(250);
#else
			usleep(250000);
#endif
		}
	}

	/* flush the SocketBuffer */
	actual_len = 1;
	while (actual_len)
	{
		int rc1;

		buf = Socket_getdata(net->socket, (size_t)1, &actual_len, &rc1);
	}

exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * Check the dest parameter against the no_proxy blacklist
 *
 * no_proxy:
 *  1. Use lowercase form.
 *  2. Use comma-separated hostname:port values.
 *  3. IP addresses are okay, but hostnames are never resolved.
 *  4. Suffixes are always matched (e.g. example.com will match test.example.com).
 *  5. If top-level domains need to be matched, leading dots are accepted e.g. .com
 *
 * @param dest the destination hostname/ip address
 * @param no_proxy the no_proxy list, probably from the environment
 * @return 1 - use the proxy, 0 - don't use the proxy
 */
int Proxy_noProxy(const char* dest, char* no_proxy)
{
	char* saveptr = NULL;
	char* curtok = NULL;
	int port = 0;
	int destport = 0;
	int port_matches = 0;
	size_t hostlen = 0;
	size_t desthostlen = 0;
	int rc = 1;
	char* no_proxy_list = NULL;

	if ((no_proxy_list = MQTTStrdup(no_proxy)) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}

	curtok = strtok_r(no_proxy_list, ",", &saveptr);
	while (curtok)
	{
		char* host = curtok;
		int matched = 0;
		int pos = 1;
		const char* topic;

		if (curtok == NULL)
			break;
		if (host[0] == '.')
			host++;

		hostlen = MQTTProtocol_addressPort(host, &port, &topic, -99);

		desthostlen = MQTTProtocol_addressPort(dest, &destport, &topic, -99);
		if (dest[desthostlen] == '/')
			desthostlen--;

		/* check if port matches */
		if (port == -99)
			port_matches = 1; /* no_proxy port absence matches any */
		else if (destport != -99)
		{
			if (destport == port)
				port_matches = 1;
		}
		if (memcmp(host, "*", 1) == 0) /* * matches anything */
		{
			/* match any host or address */
			if (port_matches == 1)
			{
				rc = 0;
				break;
			}
		}
		/* now see if .host matches the end of the dest string */
		/* match backwards from the end */
		while (host[hostlen - pos] == dest[desthostlen - pos])
		{
			if (pos == hostlen) /* reached the beginning of the no_proxy definition */
			{
				if ((pos == desthostlen || dest[desthostlen - pos - 1] == '.') && port_matches)
					matched = 1;
				break;
			}
			else if (pos == desthostlen) /* reached the beginning of the destination string */
				break;
			pos++;
		}
		if (matched)
		{
			rc = 0;
			break;
		}
		curtok = strtok_r(NULL, ",", &saveptr);
	}
	if (rc == 0)
		Log(TRACE_PROTOCOL, -1, "Matched destination %s against no_proxy %s. Don't use proxy.", dest, curtok);
	free(no_proxy_list);
exit:
	return rc;
}


/**
 * Allow user or password characters to be expressed in the form of %XX, XX being the
 * hexadecimal value of the character. This will avoid problems when a user code or a password
 * contains a '@' or another special character ('%' included)
 * @param p0 output string
 * @param p1 input string
 * @param basic_auth_in_len
 */
void Proxy_specialChars(char* p0, char* p1, b64_size_t *basic_auth_in_len)
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


/**
 * Set the HTTP proxy for connecting
 * Examples of proxy settings:
 *   http://your.proxy.server:8080/
 *   http://user:pass@my.proxy.server:8080/
 *
 * @param aClient pointer to Clients object
 * @param source the proxy setting from environment or API
 * @param [out] dest pointer to output proxy info
 * @param [out] auth_dest pointer to output authentication material
 * @param prefix expected URI prefix: http:// or https://
 * @return 0 on success, non-zero otherwise
 */
int Proxy_setHTTPProxy(Clients* aClient, char* source, char** dest, char** auth_dest, char* prefix)
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
				Proxy_specialChars((char*)basic_auth, source, &basic_auth_in_len);
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