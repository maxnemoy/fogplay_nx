#pragma once
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <netdb.h>
#ifndef CMSG_ALIGN
#define CMSG_ALIGN(len) _ALIGN(len)
#endif
#include <unistd.h>
#ifdef __cplusplus
extern "C" {
#endif
int rtc_switch_pipe(int fds[2]);
int rtc_switch_poll(struct pollfd *fds, nfds_t count, int timeout);
int rtc_switch_getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags);
#ifdef __cplusplus
}
#endif
#define pipe rtc_switch_pipe

#define poll rtc_switch_poll

#define getnameinfo rtc_switch_getnameinfo
