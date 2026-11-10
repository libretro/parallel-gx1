// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include <crypto/pk.h>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"

namespace IOS::HLE
{
constexpr int NET_SSL_MAXINSTANCES = 4;

enum ssl_err_t : s32
{
  SSL_OK = 0,
  SSL_ERR_FAILED = -1,
  SSL_ERR_RAGAIN = -2,
  SSL_ERR_WAGAIN = -3,
  SSL_ERR_SYSCALL = -5,
  SSL_ERR_ZERO = -6,          // read or write returned 0
  SSL_ERR_CAGAIN = -7,        // BIO not connected
  SSL_ERR_ID = -8,            // invalid SSL id
  SSL_ERR_VCOMMONNAME = -9,   // verify failed: common name
  SSL_ERR_VROOTCA = -10,      // verify failed: root ca
  SSL_ERR_VCHAIN = -11,       // verify failed: certificate chain
  SSL_ERR_VDATE = -12,        // verify failed: date invalid
  SSL_ERR_SERVER_CERT = -13,  // certificate cert invalid
};

enum SSL_IOCTL
{
  IOCTLV_NET_SSL_NEW = 0x01,
  IOCTLV_NET_SSL_CONNECT = 0x02,
  IOCTLV_NET_SSL_DOHANDSHAKE = 0x03,
  IOCTLV_NET_SSL_READ = 0x04,
  IOCTLV_NET_SSL_WRITE = 0x05,
  IOCTLV_NET_SSL_SHUTDOWN = 0x06,
  IOCTLV_NET_SSL_SETCLIENTCERT = 0x07,
  IOCTLV_NET_SSL_SETCLIENTCERTDEFAULT = 0x08,
  IOCTLV_NET_SSL_REMOVECLIENTCERT = 0x09,
  IOCTLV_NET_SSL_SETROOTCA = 0x0A,
  IOCTLV_NET_SSL_SETROOTCADEFAULT = 0x0B,
  IOCTLV_NET_SSL_DOHANDSHAKEEX = 0x0C,
  IOCTLV_NET_SSL_SETBUILTINROOTCA = 0x0D,
  IOCTLV_NET_SSL_SETBUILTINCLIENTCERT = 0x0E,
  IOCTLV_NET_SSL_DISABLEVERIFYOPTIONFORDEBUG = 0x0F,
  IOCTLV_NET_SSL_DEBUGGETVERSION = 0x14,
  IOCTLV_NET_SSL_DEBUGGETTIME = 0x15,
};

// Root CAs a connection trusts, and the store their DER lives in.
constexpr u32 WII_SSL_MAX_ROOTCAS = 4;
constexpr u32 WII_SSL_ROOTCA_STORE = 8192;
constexpr u32 WII_SSL_CERT_STORE = 4096;

// One emulated connection. Its certificates and key are DER in fixed
// storage, which the TLS connection borrows from its first handshake step
// to its shutdown.
struct WII_SSL
{
  void* tls;  // ssl_socket state, made at the first handshake step
  u8 rootca[WII_SSL_ROOTCA_STORE];
  const u8* rootca_ptrs[WII_SSL_MAX_ROOTCAS];
  size_t rootca_lens[WII_SSL_MAX_ROOTCAS];
  u32 rootca_count;
  u32 rootca_used;
  u8 clicert[WII_SSL_CERT_STORE];
  const u8* clicert_ptr;
  size_t clicert_len;
  u8 clikey[WII_SSL_CERT_STORE];
  struct rsa_private_key key;
  bool have_client_cert;
  bool verify;
  int sockfd;
  int hostfd;
  char hostname[256];
  bool active;
};

class NetSSLDevice : public EmulationDevice
{
public:
  NetSSLDevice(EmulationKernel& ios, const std::string& device_name);

  ~NetSSLDevice() override;

  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

  int GetSSLFreeID() const;

  static WII_SSL _SSL[NET_SSL_MAXINSTANCES];

private:
  bool m_cert_error_shown = false;
};

constexpr bool IsSSLIDValid(int id)
{
  return (id >= 0 && id < NET_SSL_MAXINSTANCES && NetSSLDevice::_SSL[id].active);
}

// One step of the connection's handshake: SSL_OK when done, SSL_ERR_RAGAIN or
// SSL_ERR_WAGAIN while it runs, or a failure.
s32 WiiSSLHandshakeStep(WII_SSL* ssl);
// Bytes written or read, SSL_ERR_ZERO, SSL_ERR_RAGAIN or a failure.
s32 WiiSSLWrite(WII_SSL* ssl, const u8* data, u32 size);
s32 WiiSSLRead(WII_SSL* ssl, u8* data, u32 size);
}  // namespace IOS::HLE
