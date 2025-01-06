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
 *    Sven Gambel - move WebSocket proxy support to generic proxy support
 *******************************************************************************/

#if !defined(PROXY_H)
#define PROXY_H

#include "Clients.h"

/* Notify the IP address and port of the endpoint to proxy, and wait connection to endpoint */
int Proxy_connect(networkHandles *net, int ssl, const char *hostname);

int Proxy_noProxy(const char* dest, char* no_proxy);

int Proxy_setHTTPProxy(Clients* aClient, char* source, char** dest, char** auth_dest, char* prefix);

#endif /* PROXY_H */
