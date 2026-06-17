#ifndef COMPAT_H
#define COMPAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>

  typedef SOCKET socket_t;
  #define INVALID_SOCK INVALID_SOCKET
  #define SOCK_ERR_WOULDBLOCK WSAEWOULDBLOCK
  #define SOCK_ERR_INPROGRESS WSAEWOULDBLOCK
  #define COMPAT_POLL WSAPoll
  #define close_sock(s) closesocket(s)
  #define sock_errno() WSAGetLastError()
  #define MSG_NOSIGNAL_FLAG 0

  /* Windows doesn't have ssize_t in default headers we use */
  #ifndef _SSIZE_T_DEFINED
  typedef intptr_t ssize_t;
  #define _SSIZE_T_DEFINED
  #endif

#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <poll.h>
  #include <strings.h>

  typedef int socket_t;
  #define INVALID_SOCK (-1)
  #define SOCK_ERR_WOULDBLOCK EAGAIN
  #define SOCK_ERR_INPROGRESS EINPROGRESS
  #define COMPAT_POLL poll
  #define close_sock(s) close(s)
  #define sock_errno() errno
  #ifdef __linux__
    #define MSG_NOSIGNAL_FLAG MSG_NOSIGNAL
  #else
    #define MSG_NOSIGNAL_FLAG 0
  #endif
#endif

/* Initialize networking subsystem (WSAStartup on Windows, no-op elsewhere). */
int compat_init(void);
void compat_cleanup(void);

/* Set socket to non-blocking mode. Returns 0 on success, -1 on error. */
int compat_set_nonblock(socket_t s);

/* Portable memmem (Linux glibc-only otherwise). */
void *compat_memmem(const void *haystack, size_t hlen,
                    const void *needle, size_t nlen);

/* Portable case-insensitive substring search. */
char *compat_strcasestr(const char *haystack, const char *needle);

#endif
