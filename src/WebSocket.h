/*******************************************************************************
 * Copyright (c) 2018, 2022 Wind River Systems, Inc. and others. All Rights Reserved.
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

#if !defined(WEBSOCKET_H)
#define WEBSOCKET_H

#include "MQTTPacket.h"

/**
 * WebSocket op codes
 * @{
 */
#define WebSocket_OP_CONTINUE 0x0 /* 0000 - continue frame */
#define WebSocket_OP_TEXT     0x1 /* 0001 - text frame */
#define WebSocket_OP_BINARY   0x2 /* 0010 - binary frame */
#define WebSocket_OP_CLOSE    0x8 /* 1000 - close frame */
#define WebSocket_OP_PING     0x9 /* 1001 - ping frame */
#define WebSocket_OP_PONG     0xA /* 1010 - pong frame */
/** @} */

/**
 * Various close status codes
 * @{
 */
#define WebSocket_CLOSE_NORMAL          1000
#define WebSocket_CLOSE_GOING_AWAY      1001
#define WebSocket_CLOSE_PROTOCOL_ERROR  1002
#define WebSocket_CLOSE_UNKNOWN_DATA    1003
#define WebSocket_CLOSE_RESERVED        1004
#define WebSocket_CLOSE_NO_STATUS_CODE  1005 /* reserved: not to be used */
#define WebSocket_CLOSE_ABNORMAL        1006 /* reserved: not to be used */
#define WebSocket_CLOSE_BAD_DATA        1007
#define WebSocket_CLOSE_POLICY          1008
#define WebSocket_CLOSE_MSG_TOO_BIG     1009
#define WebSocket_CLOSE_NO_EXTENSION    1010
#define WebScoket_CLOSE_UNEXPECTED      1011
#define WebSocket_CLOSE_TLS_FAIL        1015 /* reserved: not be used */
/** @} */

/* closes a websocket connection */
void WebSocket_close(networkHandles *net, int status_code, const char *reason);

/* sends upgrade request */
int WebSocket_connect(networkHandles *net, int ssl, const char *uri);

/* obtain data from network socket */
int WebSocket_getch(networkHandles *net, char* c);
char *WebSocket_getdata(networkHandles *net, size_t bytes, size_t* actual_len);
size_t WebSocket_framePos();
void WebSocket_framePosSeekTo(size_t);

/* send data out, in websocket format only if required */
int WebSocket_putdatas(networkHandles* net, char** buf0, size_t* buf0len, PacketBuffers* bufs);

/* releases any resources used by the websocket system */
void WebSocket_terminate(void);

/* handles websocket upgrade request */
int WebSocket_upgrade(networkHandles *net);

#endif /* WEBSOCKET_H */
