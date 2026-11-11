/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DOLPHIN_COMMON_NETSOCKET_H
#define DOLPHIN_COMMON_NETSOCKET_H

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The TCP and UDP sockets the core's links use (the GBA link, USB Gecko,
 * the broadband adapters, the DSU client), as plain descriptors, over
 * libretro-common's net_compat. Addresses are IPv4, in network byte order;
 * ports in host order. -1 is no socket. */

/* Listens on @port on every address, nonblocking. */
int net_tcp_listen(uint16_t port);
/* Listens on @port on @addr only, nonblocking. */
int net_tcp_listen_on(uint32_t addr, uint16_t port);
/* An accepted connection, blocking, or -1 when none is waiting. */
int net_tcp_accept(int listener);

bool net_set_blocking(int fd, bool block);
void net_close(int fd);

/* Bytes sent, 0 when a nonblocking socket would block, -1 when the
 * connection is gone. A blocking socket sends all of @len. */
int net_tcp_send(int fd, const void *data, size_t len);
/* Bytes received, 0 when a nonblocking socket has none, -1 when the
 * connection is gone. */
int net_tcp_recv(int fd, void *data, size_t len);

/* Whether @fd became readable within @timeout_ms. */
bool net_wait_readable(int fd, int timeout_ms);
/* Waits up to @timeout_ms for any of @count sockets (at most 32) to become
 * readable; @ready[i] tells which. Returns how many are. */
unsigned net_wait_readable_any(const int *fds, unsigned count, int timeout_ms, bool *ready);

/* A UDP socket bound to @port on every address (0: any port), blocking. */
int net_udp_open(uint16_t port);
/* Bytes sent, or -1. */
int net_udp_sendto(int fd, const void *data, size_t len, uint32_t addr, uint16_t port);
/* Bytes received, or -1 (none waiting on a nonblocking socket, or an
 * error); @addr and @port name the sender. */
int net_udp_recvfrom(int fd, void *data, size_t len, uint32_t *addr, uint16_t *port);

/* The port @fd is bound to, 0 when none. */
uint16_t net_local_port(int fd);

/* 127.0.0.1, in network order. */
uint32_t net_loopback_ipv4(void);

/* The address this host reaches the network from, by its route to a
 * public address (no packet is sent). */
bool net_local_ipv4(uint32_t *addr);

/* Resolves @host (a name or a dotted address) to an IPv4 address. */
bool net_resolve_ipv4(const char *host, uint32_t *addr);

#ifdef __cplusplus
}
#endif

#endif
