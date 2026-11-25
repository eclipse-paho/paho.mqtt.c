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

#if !defined(MQTTCLIENTOPTIONS_H)
#define MQTTCLIENTOPTIONS_H

#include "MQTTClient.h"

int MQTTClientOptions_copyALPN(const unsigned char* protos, unsigned int protos_len, unsigned char** copy);
int MQTTClientOptions_copySSL(const MQTTClient_SSLOptions* src, MQTTClient_SSLOptions** dest);
void MQTTClientOptions_freeSSL(MQTTClient_SSLOptions* opts);

#endif
