/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file posix_socket.h
 * @brief Interface for POSIX Socket operations.
 *
 * This header file provides an interface for creating, sending, receiving,
 * and managing POSIX Sockets.
 *
 * The functions defined in this header can be used in both C and C++
 * programs.
 */

#ifndef POSIX_SOCKET_COMMS_H
#define POSIX_SOCKET_COMMS_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
* @def SOCKET_PERMISSIONS
* @brief Default permissions for socket files (when applicable)
*/
#define SOCKET_PERMISSIONS 0660

/**
* @def MAX_PENDING_CONNECTIONS
* @brief Maximum number of pending connections in listen queue
*/
#define MAX_PENDING_CONNECTIONS 10

/**
* @def SOCKET_BUFFER_SIZE
* @brief Default buffer size for socket operations
*/
#define SOCKET_BUFFER_SIZE 4096

/**
* @enum NvPSFSocketType_t
* @brief Types of sockets supported
*/
typedef enum NvPSFSocketType_t {
    SOCK_STREAM_TYPE = SOCK_STREAM, /**< Stream socket (TCP) */
    SOCK_DGRAM_TYPE = SOCK_DGRAM /**< Datagram socket (UDP) */
} NvPSFSocketType;

/**
* @enum NvPSFSocketEndpointType_t
* @brief Role of the socket in communication
*/
typedef enum NvPSFSocketEndpointType_t {
    SOCKET_SERVER = 0, /**< Socket acting as server */
    SOCKET_CLIENT, /**< Socket acting as client */
    SOCKET_BIDIRECTIONAL /**< Socket used for bidirectional communication */
} NvPSFSocketEndpoint;

/**
* @enum NvPSFSocketBlockingMode_t
* @brief Blocking mode for socket operations
*/
typedef enum NvPSFSocketBlockingMode_t {
    SOCK_BLOCKING = 0, /**< Blocking mode */
    SOCK_NON_BLOCKING /**< Non-blocking mode */
} NvPSFSocketBlockingMode;

/**
* @enum NvPSFSocketErr_t
* @brief Error codes for socket operations
*/
typedef enum NvPSFSocketErr_t {
    NvPSFSOCK_SUCCESS = 0, /**< Operation successful */
    NvPSFSOCK_FAIL = 1  /**< Operation failed */
} NvPSFSocketErr;

/**
* @union NvPSFSocketRetCode_t
* @brief Return code from socket operations
*/
typedef union NvPSFSocketRetCode_t {
    int sockfd; /**< Socket file descriptor on success */
    int errCode; /**< Error code on failure */
    ssize_t bytesTransferred; /**< Number of bytes transferred in I/O operations */
} NvPSFSocketRetCode;

/**
* @struct NvPSFSocketStatus_t
* @brief Status structure returned by socket operations
*/
typedef struct NvPSFSocketStatus_t {
    NvPSFSocketErr err; /**< Error status */
    NvPSFSocketRetCode retCode; /**< Return code (descriptor, error, or bytes) */
} NvPSFSocketStatus;

/**
* @brief Create a new socket with specified parameters
*
* @param domain The protocol family (e.g., AF_INET, AF_INET6)
* @param type The socket type (SOCK_STREAM_TYPE or SOCK_DGRAM_TYPE)
* @param role The role determining default configurations (server/client/bidirectional)
* @param blocking Whether the socket should operate in blocking or non-blocking mode
*
* @return Status structure containing:
*         - Socket descriptor in retCode.sockfd on success
*         - Error code in retCode.errCode on failure
*/
NvPSFSocketStatus NvPSFSocketCreate(int domain, NvPSFSocketType type,
                    NvPSFSocketEndpoint role, NvPSFSocketBlockingMode blocking);

/**
* @brief Bind a socket to a local address
*
* @param sockfd Socket descriptor to bind
* @param addr Pointer to sockaddr structure containing address information
* @param addrlen Length of the address structure
*
* @return Status structure with operation result
*/
NvPSFSocketStatus NvPSFSocketBind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

/**
* @brief Start listening for incoming connections
*
* @param sockfd Socket descriptor to listen on
*
* @return Status structure with operation result
*/
NvPSFSocketStatus NvPSFSocketListen(int sockfd);

/**
* @brief Accept an incoming connection
*
* @param sockfd Listening socket descriptor
* @param addr Pointer to store client address (optional)
* @param addrlen Pointer to store client address length (optional)
*
* @return Status structure containing:
*         - New socket descriptor in retCode.sockfd on success
*         - Error code in retCode.errCode on failure
*/
NvPSFSocketStatus NvPSFSocketAccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

/**
* @brief Connect to a remote socket
*
* @param sockfd Socket descriptor to use for connection
* @param addr Pointer to sockaddr structure with target address
* @param addrlen Length of the address structure
*
* @return Status structure with operation result
*/
NvPSFSocketStatus NvPSFSocketConnect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

/**
* @brief Send data through a socket
*
* @param sockfd Connected socket descriptor
* @param buf Pointer to data buffer
* @param len Length of data to send
* @param flags Additional send flags
*
* @return Status structure containing:
*         - Number of bytes sent in retCode.bytesTransferred on success
*         - Error code in retCode.errCode on failure
*/
NvPSFSocketStatus NvPSFSocketSend(int sockfd, const void *buf, size_t len, int flags);

/**
* @brief Receive data from a socket
*
* @param sockfd Connected socket descriptor
* @param buf Pointer to receive buffer
* @param len Maximum length to receive
* @param flags Additional receive flags
*
* @return Status structure containing:
*         - Number of bytes received in retCode.bytesTransferred on success
*         - Error code in retCode.errCode on failure
*/
NvPSFSocketStatus NvPSFSocketReceive(int sockfd, void *buf, size_t len, int flags);

/**
* @brief Close a socket descriptor
*
* @param sockfd Socket descriptor to close
*
* @return Status structure with operation result
*/
NvPSFSocketStatus NvPSFSocketClose(int sockfd);

#ifdef __cplusplus
}
#endif

#endif // POSIX_SOCKET_COMMS_H
