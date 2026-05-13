/*******************************************************************************
 * Copyright (c) 2009, 2026 IBM Corp., Ian Craggs and others
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
 *    Ian Craggs - initial implementation and documentation
 *    Ian Craggs - async client updates
 *******************************************************************************/

#if !defined(SOCKET_H)
#define SOCKET_H

#include <stdint.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <errno.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define MAXHOSTNAMELEN 256
#define poll WSAPoll
#if !defined(SSLSOCKET_H)
#undef EAGAIN
#define EAGAIN WSAEWOULDBLOCK
#undef EINTR
#define EINTR WSAEINTR
#undef EINPROGRESS
#define EINPROGRESS WSAEINPROGRESS
#undef EWOULDBLOCK
#define EWOULDBLOCK WSAEWOULDBLOCK
#undef ENOTCONN
#define ENOTCONN WSAENOTCONN
#undef ECONNRESET
#define ECONNRESET WSAECONNRESET
#undef ETIMEDOUT
#define ETIMEDOUT WAIT_TIMEOUT
#endif
#define ioctl ioctlsocket
#define socklen_t int
#else
#define INVALID_SOCKET SOCKET_ERROR
#include <sys/socket.h>
#if !defined(_WRS_KERNEL)
#include <sys/param.h>
#include <sys/time.h>
#include <sys/select.h>
#include <poll.h>
#include <sys/uio.h>
#else
#include <selectLib.h>
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef __QNXNTO__
#define AI_ADDRCONFIG 0
#endif
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#define ULONG size_t
#define SOCKET int
#endif

#include "Thread.h" /* Needed for mutex_type */

/** socket operation completed successfully */
#define TCPSOCKET_COMPLETE 0
#if !defined(SOCKET_ERROR)
	/** error in socket operation */
	#define SOCKET_ERROR -1
#endif
/** must be the same as SOCKETBUFFER_INTERRUPTED */
#define TCPSOCKET_INTERRUPTED -22
#define SSL_FATAL -3

#if !defined(INET6_ADDRSTRLEN)
#define INET6_ADDRSTRLEN 46 /** only needed for gcc/cygwin on windows */
#endif


#if !defined(max)
#define max(A,B) ( (A) > (B) ? (A):(B))
#endif

#include "LinkedList.h"

/*
 * Network write buffers for an MQTT packet
 */
typedef struct
{
	int count;         /**> number of buffers/buflens/frees */
	char** buffers;    /**> array of byte buffers */
	size_t* buflens;   /**> array of lengths of buffers */
	int* frees;        /**> array of flags indicating whether each buffer needs to be freed */
	uint8_t mask[4];   /**> websocket mask used to mask the buffer data, if any */
} PacketBuffers;


/**
 * Structure to hold all socket data for the module
 */
typedef struct
{
	List* connect_pending; /**< list of sockets for which a connect is pending */
	List* write_pending; /**< list of sockets for which a write is pending */

#if defined(USE_SELECT)
	fd_set rset, /**< socket read set (see select doc) */
		rset_saved; /**< saved socket read set */
	int maxfdp1; /**< max descriptor used +1 (again see select doc) */
	List* clientsds; /**< list of client socket descriptors */
	ListElement* cur_clientsds; /**< current client socket descriptor (iterator) */
	fd_set pending_wset; /**< socket pending write set for select */
#else
	unsigned int nfds;         /**< no of file descriptors for poll */
	struct pollfd* fds_read;        /**< poll read file descriptors */
	struct pollfd* fds_write;

	struct {
		int cur_fd;            /**< index into the fds_saved array */
		unsigned int nfds;	   /**< number of fds in the fds_saved array */
		struct pollfd* fds_write;
		struct pollfd* fds_read;
	} saved;
#endif
} Sockets;


void Socket_outInitialize(void);
void Socket_outTerminate(void);
SOCKET Socket_getReadySocket(int more_work, int timeout, mutex_type mutex, int* rc, int* interrupted);
int Socket_getch(SOCKET socket, char* c);
char *Socket_getdata(SOCKET socket, size_t bytes, size_t* actual_len, int* rc);
int Socket_putdatas(SOCKET socket, char* buf0, size_t buf0len, PacketBuffers bufs);
int Socket_close(SOCKET socket);
#if defined(__GNUC__) && defined(__linux__)
/* able to use GNU's getaddrinfo_a to make timeouts possible */
int Socket_new(const char* addr, size_t addr_len, int port, SOCKET* socket, long timeout);
#else
int Socket_new(const char* addr, size_t addr_len, int port, SOCKET* socket);
#endif
int Socket_unix_new(const char* addr, size_t addr_len, SOCKET* sock);

int Socket_noPendingWrites(SOCKET socket);
char* Socket_getpeer(SOCKET sock);

void Socket_addPendingWrite(SOCKET socket);
void Socket_clearPendingWrite(SOCKET socket);

typedef void Socket_writeContinue(SOCKET socket);
void Socket_setWriteContinueCallback(Socket_writeContinue*);

typedef void Socket_writeComplete(SOCKET socket, int rc);
void Socket_setWriteCompleteCallback(Socket_writeComplete*);

typedef void Socket_writeAvailable(SOCKET socket);
void Socket_setWriteAvailableCallback(Socket_writeAvailable*);

int Socket_interrupt();

#endif /* SOCKET_H */
