/*******************************************************************************
 * Copyright (c) 2009, 2022 IBM Corp., Ian Craggs, and others
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
 *    Ian Craggs - MQTT 3.1.1 support
 *    Ian Craggs - SNI support
 *    Ian Craggs - MQTT 5.0 support
 *    Sven Gambel - add generic proxy support
 *******************************************************************************/

#if !defined(MQTTPROTOCOLOUT_H)
#define MQTTPROTOCOLOUT_H

#include "LinkedList.h"
#include "MQTTPacket.h"
#include "Clients.h"
#include "Log.h"
#include "Messages.h"
#include "MQTTProtocol.h"
#include "MQTTProtocolClient.h"

#define MQTT_DEFAULT_PORT 1883
#define SECURE_MQTT_DEFAULT_PORT 8883
#define WS_DEFAULT_PORT 80
#define WSS_DEFAULT_PORT 443
#define PROXY_DEFAULT_PORT 8080

size_t MQTTProtocol_addressPort(const char* uri, int* port, const char **topic, int default_port);
void MQTTProtocol_reconnect(const char* ip_address, Clients* client);
#if defined(OPENSSL)
#if defined(__GNUC__) && defined(__linux__)
int MQTTProtocol_connect(const char* ip_address, Clients* acClients, int unixsock, int ssl, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties, long timeout);
#else
int MQTTProtocol_connect(const char* ip_address, Clients* acClients, int unixsock, int ssl, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties);
#endif
#else
#if defined(__GNUC__) && defined(__linux__)
int MQTTProtocol_connect(const char* ip_address, Clients* acClients, int unixsock, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties, long timeout);
#else
int MQTTProtocol_connect(const char* ip_address, Clients* acClients, int unixsock, int websocket, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties);
#endif
#endif
int MQTTProtocol_handlePingresps(void* pack, SOCKET sock);
int MQTTProtocol_subscribe(Clients* client, List* topics, List* qoss, int msgID,
		MQTTSubscribe_options* opts, MQTTProperties* props);
int MQTTProtocol_handleSubacks(void* pack, SOCKET sock);
int MQTTProtocol_unsubscribe(Clients* client, List* topics, int msgID, MQTTProperties* props);
int MQTTProtocol_handleUnsubacks(void* pack, SOCKET sock);
int MQTTProtocol_handleDisconnects(void* pack, SOCKET sock);

#endif
