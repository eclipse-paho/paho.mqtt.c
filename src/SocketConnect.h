/*******************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 *******************************************************************************/
#ifndef SOCKETCONNECT_H
#define SOCKETCONNECT_H

#include "Socket.h"
#include "MQTTTime.h"

typedef struct SocketConnect SocketConnect;

/* All calls are serialized by the owning MQTT library's client mutex.
 * The socket layer never calls back into this module while holding its mutex.
 */
int SocketConnect_start(SocketConnect** attempt, const char* host, size_t length, int port,
                        START_TIME_TYPE start, ELAPSED_TIME_TYPE timeout);
void SocketConnect_process(SOCKET ready, uint64_t registration_id);
int SocketConnect_timeout(int timeout);
int SocketConnect_takeResult(SocketConnect** attempt, SOCKET* winner, int* error);
void SocketConnect_cancel(SocketConnect** attempt);

#endif
