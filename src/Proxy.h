/*******************************************************************************
 * Copyright (c) 2009, 2021 Diehl Metering.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    https://www.eclipse.org/legal/epl-v20.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php
 *
 * Contributors:
 *    Sven Gambel - move WebSocket proxy support to generic proxy support
 *******************************************************************************/

#if !defined(PROXY_H)
#define PROXY_H

#include "Clients.h"

/* Notify the IP address and port of the endpoint to proxy, and wait connection to endpoint */
int Proxy_connect(networkHandles *net, int ssl, const char *hostname );

#endif /* PROXY_H */
