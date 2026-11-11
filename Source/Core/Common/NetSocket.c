/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "Common/NetSocket.h"

#include <string.h>

#include <net/net_compat.h>
#include <net/net_socket.h>

static void net_addr(struct sockaddr_in *sin, uint32_t addr, uint16_t port)
{
   memset(sin, 0, sizeof(*sin));
   sin->sin_family      = AF_INET;
   sin->sin_addr.s_addr = addr;
   sin->sin_port        = htons(port);
}

static int net_open(int type, uint32_t addr, uint16_t port)
{
   struct sockaddr_in sin;
   int one = 1;
   int fd;

   if (!network_init())
      return -1;
   fd = (int)socket(AF_INET, type, 0);
   if (fd < 0)
      return -1;
   if (type == SOCK_STREAM)
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
   net_addr(&sin, addr, port);
   if (bind(fd, (const struct sockaddr*)&sin, sizeof(sin)) != 0)
   {
      socket_close(fd);
      return -1;
   }
   return fd;
}

int net_tcp_listen(uint16_t port)
{
   return net_tcp_listen_on(htonl(INADDR_ANY), port);
}

int net_tcp_listen_on(uint32_t addr, uint16_t port)
{
   int fd = net_open(SOCK_STREAM, addr, port);
   if (fd < 0)
      return -1;
   if (listen(fd, 4) != 0 || !socket_set_block(fd, false))
   {
      socket_close(fd);
      return -1;
   }
   return fd;
}

int net_tcp_accept(int listener)
{
   int fd = (int)accept(listener, NULL, NULL);
   if (fd < 0)
      return -1;
   /* some systems pass the listener's nonblocking mode on */
   socket_set_block(fd, true);
   return fd;
}

bool net_set_blocking(int fd, bool block)
{
   return socket_set_block(fd, block);
}

void net_close(int fd)
{
   if (fd >= 0)
      socket_close(fd);
}

int net_tcp_send(int fd, const void *data, size_t len)
{
   const char *p    = (const char*)data;
   size_t      sent = 0;
   while (sent < len)
   {
      int n = (int)send(fd, p + sent, (int)(len - sent), 0);
      if (n < 0)
      {
         if (isagain(n))
            break;
         return -1;
      }
      if (n == 0)
         return -1;
      sent += (size_t)n;
   }
   return (int)sent;
}

int net_tcp_recv(int fd, void *data, size_t len)
{
   int n = (int)recv(fd, (char*)data, (int)len, 0);
   if (n > 0)
      return n;
   if (n < 0 && isagain(n))
      return 0;
   return -1;
}

bool net_wait_readable(int fd, int timeout_ms)
{
   bool rd = true;
   bool wr = false;
   return socket_wait(fd, &rd, &wr, timeout_ms) && rd;
}

unsigned net_wait_readable_any(const int *fds, unsigned count, int timeout_ms, bool *ready)
{
   struct pollfd polls[32];
   unsigned      i;
   unsigned      n = 0;

   if (count > 32)
      count = 32;
   memset(polls, 0, sizeof(polls));
   for (i = 0; i < count; i++)
   {
      NET_POLL_FD(fds[i], &polls[i]);
      NET_POLL_EVENT(POLLIN, &polls[i]);
      ready[i] = false;
   }
   if (socket_poll(polls, count, timeout_ms) <= 0)
      return 0;
   for (i = 0; i < count; i++)
   {
      if (fds[i] >= 0 && NET_POLL_HAS_EVENT(POLLIN, &polls[i]))
      {
         ready[i] = true;
         n++;
      }
   }
   return n;
}

int net_udp_open(uint16_t port)
{
   return net_open(SOCK_DGRAM, htonl(INADDR_ANY), port);
}

int net_udp_sendto(int fd, const void *data, size_t len, uint32_t addr, uint16_t port)
{
   struct sockaddr_in sin;
   net_addr(&sin, addr, port);
   return (int)sendto(fd, (const char*)data, (int)len, 0,
         (const struct sockaddr*)&sin, sizeof(sin));
}

int net_udp_recvfrom(int fd, void *data, size_t len, uint32_t *addr, uint16_t *port)
{
   struct sockaddr_in sin;
   socklen_t          sin_len = sizeof(sin);
   int n = (int)recvfrom(fd, (char*)data, (int)len, 0, (struct sockaddr*)&sin, &sin_len);
   if (n < 0)
      return -1;
   if (addr)
      *addr = sin.sin_addr.s_addr;
   if (port)
      *port = ntohs(sin.sin_port);
   return n;
}

uint16_t net_local_port(int fd)
{
   struct sockaddr_in sin;
   socklen_t          sin_len = sizeof(sin);
   if (fd < 0 || getsockname(fd, (struct sockaddr*)&sin, &sin_len) != 0)
      return 0;
   return ntohs(sin.sin_port);
}

uint32_t net_loopback_ipv4(void)
{
   return htonl(INADDR_LOOPBACK);
}

bool net_local_ipv4(uint32_t *addr)
{
   struct sockaddr_in sin;
   socklen_t          sin_len = sizeof(sin);
   int                fd;
   bool               ok = false;

   if (!network_init())
      return false;
   fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
   if (fd < 0)
      return false;
   /* a UDP connect only picks the route */
   net_addr(&sin, htonl(0x01010101), 9);
   if (connect(fd, (const struct sockaddr*)&sin, sizeof(sin)) == 0
         && getsockname(fd, (struct sockaddr*)&sin, &sin_len) == 0)
   {
      *addr = sin.sin_addr.s_addr;
      ok    = true;
   }
   socket_close(fd);
   return ok;
}

bool net_resolve_ipv4(const char *host, uint32_t *addr)
{
   struct addrinfo  hints;
   struct addrinfo *res = NULL;

   if (!network_init())
      return false;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family   = AF_INET;
   hints.ai_socktype = SOCK_DGRAM;
   if (getaddrinfo_retro(host, NULL, &hints, &res) != 0 || !res)
      return false;
   *addr = ((const struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
   freeaddrinfo_retro(res);
   return true;
}
