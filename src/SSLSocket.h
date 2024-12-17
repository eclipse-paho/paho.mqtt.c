/*******************************************************************************
 * Copyright (c) 2009, 2022 IBM Corp., Ian Craggs
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
 *    Ian Craggs, Allan Stockdill-Mander - initial implementation 
 *    Ian Craggs - SNI support
 *    Ian Craggs - post connect checks and CApath
 *******************************************************************************/
#if !defined(SSLSOCKET_H)
#define SSLSOCKET_H

#if defined(_WIN32) || defined(_WIN64)
	#define ssl_mutex_type HANDLE
#else
	#include <pthread.h>
	#include <semaphore.h>
	#define ssl_mutex_type pthread_mutex_t
#endif

#include <openssl/ssl.h>
#include "SocketBuffer.h"
#include "Clients.h"

#define URI_SSL   "ssl://"
#define URI_MQTTS "mqtts://"

/** if we should handle openssl initialization (bool_value == 1) or depend on it to be initalized externally (bool_value == 0) */
void SSLSocket_handleOpensslInit(int bool_value);

int SSLSocket_initialize(void);
void SSLSocket_terminate(void);
int SSLSocket_setSocketForSSL(networkHandles* net, MQTTClient_SSLOptions* opts, const char* hostname, size_t hostname_len);

int SSLSocket_getch(SSL* ssl, SOCKET socket, char* c);
char *SSLSocket_getdata(SSL* ssl, SOCKET socket, size_t bytes, size_t* actual_len, int* rc);

int SSLSocket_close(networkHandles* net);
int SSLSocket_putdatas(SSL* ssl, SOCKET socket, char* buf0, size_t buf0len, PacketBuffers bufs);
int SSLSocket_connect(SSL* ssl, SOCKET sock, const char* hostname, int verify, int (*cb)(const char *str, size_t len, void *u), void* u);

SOCKET SSLSocket_getPendingRead(void);
int SSLSocket_continueWrite(pending_writes* pw);
int SSLSocket_abortWrite(pending_writes* pw);

#endif
