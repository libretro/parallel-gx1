// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Network/SSL.h"

#include <cstdio>
#include <cstring>

#include <crypto/x509.h>
#include <lrc_hash.h>
#include <net/net_socket_ssl.h>
#include <retro_miscellaneous.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Network/Socket.h"
#include "Core/NetworkCaptureLogger.h"
#include "Core/System.h"

// The TLS client reports why a certificate was refused through these
// hooks; the last reason picks the error the guest is given.
static char s_verify_info[128];

extern "C" void ssl_socket_log_verify_fail(int mode_required, const char* domain,
                                           const char* verify_info)
{
  (void)mode_required;
  std::snprintf(s_verify_info, sizeof(s_verify_info), "%s", verify_info ? verify_info : "");
  ERROR_LOG(IOS_SSL, "Certificate for %s refused: %s", domain ? domain : "", s_verify_info);
}

extern "C" void ssl_socket_log_verify_disabled(const char* domain)
{
  (void)domain;
}

namespace IOS::HLE
{
WII_SSL NetSSLDevice::_SSL[NET_SSL_MAXINSTANCES];

// What the IOS client offers: the RSA key exchange and CBC suites its
// servers speak, after the ECDHE AEAD ones.
static const unsigned s_suites[] = {0x1301, 0x1302, 0x1303, 0xc02f, 0xc030, 0xc02b,
                                    0xc02c, 0xcca8, 0xcca9, 0xc013, 0xc014, 0x009c,
                                    0x009d, 0x003c, 0x003d, 0x002f, 0x0035};

static void ResetSSL(WII_SSL* ssl)
{
  // The socket belongs to the socket manager, so the connection is freed
  // without closing it.
  if (ssl->tls)
    ssl_socket_free(ssl->tls);
  ssl->tls = nullptr;
  ssl->rootca_count = 0;
  ssl->rootca_used = 0;
  ssl->clicert_ptr = nullptr;
  ssl->clicert_len = 0;
  ssl->have_client_cert = false;
  ssl->hostname[0] = '\0';
  ssl->sockfd = -1;
  ssl->hostfd = -1;
  ssl->active = false;
}

static int Base64Value(char c)
{
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

// The DER of the first PEM block in data, or data itself when it is not
// PEM, into out. Returns the DER's length, 0 when it does not fit or is
// malformed.
static size_t ToDER(const u8* data, size_t size, u8* out, size_t out_size)
{
  static const char begin[] = "-----BEGIN";
  const char* p = reinterpret_cast<const char*>(data);
  const char* end = p + size;
  size_t len = 0;
  u32 acc = 0;
  int bits = 0;

  if (size < sizeof(begin) - 1 || std::memcmp(p, begin, sizeof(begin) - 1) != 0)
  {
    if (size > out_size)
      return 0;
    std::memcpy(out, data, size);
    return size;
  }

  // the base64 starts on the line after the BEGIN line
  while (p < end && *p != '\n')
    p++;
  for (; p < end && *p != '-'; p++)
  {
    const int v = Base64Value(*p);
    if (v < 0)
      continue;
    acc = (acc << 6) | static_cast<u32>(v);
    bits += 6;
    if (bits >= 8)
    {
      bits -= 8;
      if (len == out_size)
        return 0;
      out[len++] = static_cast<u8>(acc >> bits);
    }
  }
  return p < end ? len : 0;
}

// Adds a trust anchor to the connection. Returns false when the store is full.
static bool AddRootCA(WII_SSL* ssl, const u8* data, size_t size)
{
  if (ssl->rootca_count == WII_SSL_MAX_ROOTCAS)
    return false;
  const size_t len = ToDER(data, size, ssl->rootca + ssl->rootca_used,
                           WII_SSL_ROOTCA_STORE - ssl->rootca_used);
  if (!len)
    return false;
  ssl->rootca_ptrs[ssl->rootca_count] = ssl->rootca + ssl->rootca_used;
  ssl->rootca_lens[ssl->rootca_count] = len;
  ssl->rootca_count++;
  ssl->rootca_used += static_cast<u32>(len);
  return true;
}

static s32 VerifyError()
{
  if (std::strstr(s_verify_info, "hostname"))
    return SSL_ERR_VCOMMONNAME;
  if (std::strstr(s_verify_info, "expired"))
    return SSL_ERR_VDATE;
  if (std::strstr(s_verify_info, "issuer") || std::strstr(s_verify_info, "trust"))
    return SSL_ERR_VROOTCA;
  return SSL_ERR_VCHAIN;
}

s32 WiiSSLHandshakeStep(WII_SSL* ssl)
{
  bool want_read;
  bool want_write;

  if (!ssl->tls)
  {
    struct ssl_socket_retro_config cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    // The Wii sends no server_name; the host is still checked.
    cfg.flags = SSL_RETRO_NO_SNI;
    if (!ssl->verify)
      cfg.flags |= SSL_RETRO_NO_VERIFY;
    // Without a root CA there is nothing the server could be checked against.
    else if (!ssl->rootca_count)
      return SSL_ERR_VROOTCA;
    cfg.trust = ssl->rootca_ptrs;
    cfg.trust_lens = ssl->rootca_lens;
    cfg.trust_count = ssl->rootca_count;
    if (ssl->have_client_cert)
    {
      cfg.chain = &ssl->clicert_ptr;
      cfg.chain_lens = &ssl->clicert_len;
      cfg.chain_count = 1;
      cfg.key = &ssl->key;
    }
    cfg.suites = s_suites;
    cfg.suite_count = sizeof(s_suites) / sizeof(s_suites[0]);

    ssl->tls = ssl_socket_init(ssl->hostfd, ssl->hostname);
    if (!ssl->tls)
      return SSL_ERR_FAILED;
    if (ssl_socket_retro_configure(ssl->tls, &cfg) != 0)
    {
      ssl_socket_free(ssl->tls);
      ssl->tls = nullptr;
      return SSL_ERR_FAILED;
    }
  }

  s_verify_info[0] = '\0';
  const int ret = ssl_socket_handshake_step(ssl->tls, &want_read, &want_write);
  if (ret > 0)
    return SSL_OK;
  if (ret == 0)
    return (want_write && !want_read) ? SSL_ERR_WAGAIN : SSL_ERR_RAGAIN;
  if (s_verify_info[0])
    return VerifyError();
  ERROR_LOG(IOS_SSL, "Handshake with %s failed (%d)", ssl->hostname,
            ssl_socket_last_error(ssl->tls));
  return SSL_ERR_FAILED;
}

s32 WiiSSLWrite(WII_SSL* ssl, const u8* data, u32 size)
{
  if (!ssl->tls)
    return SSL_ERR_FAILED;
  const ssize_t ret = ssl_socket_send_all_nonblocking(ssl->tls, data, size, true);
  if (ret < 0)
    return SSL_ERR_FAILED;
  if (ret == 0)
    return size ? SSL_ERR_WAGAIN : SSL_ERR_ZERO;
  return static_cast<s32>(ret);
}

s32 WiiSSLRead(WII_SSL* ssl, u8* data, u32 size)
{
  bool error = false;
  if (!ssl->tls)
    return SSL_ERR_FAILED;
  // a record the last send left unsent goes out first
  if (ssl_socket_flush_nonblocking(ssl->tls) < 0)
    return SSL_ERR_FAILED;
  const ssize_t ret = ssl_socket_receive_all_nonblocking(ssl->tls, &error, data, size);
  if (ret > 0)
    return static_cast<s32>(ret);
  if (!error)
    return SSL_ERR_RAGAIN;
  // the end of the stream, as a read of 0 was
  return ssl_socket_last_error(ssl->tls) ? SSL_ERR_FAILED : SSL_ERR_ZERO;
}

NetSSLDevice::NetSSLDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
  for (WII_SSL& ssl : _SSL)
  {
    ssl.tls = nullptr;
    ResetSSL(&ssl);
  }
}

NetSSLDevice::~NetSSLDevice()
{
  for (WII_SSL& ssl : _SSL)
  {
    if (ssl.active)
      ResetSSL(&ssl);
  }
}

int NetSSLDevice::GetSSLFreeID() const
{
  for (int i = 0; i < NET_SSL_MAXINSTANCES; i++)
  {
    if (!_SSL[i].active)
    {
      return i + 1;
    }
  }
  return 0;
}

std::optional<IPCReply> NetSSLDevice::IOCtl(const IOCtlRequest& request)
{
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_SSL, Common::Log::LogLevel::LINFO);
  return IPCReply(IPC_SUCCESS);
}

static const u8 s_client_cert_hash[32] = {
    0x22, 0x9e, 0xc6, 0x78, 0x52, 0x5e, 0x06, 0x05, 0x88, 0xe8, 0xea,
    0x23, 0xe5, 0x45, 0x9e, 0xc1, 0x4a, 0xf3, 0xc2, 0xeb, 0xb7, 0xb9,
    0xe6, 0x9e, 0xc4, 0x6b, 0x0f, 0xaf, 0x01, 0x17, 0x30, 0xd9};
static const u8 s_client_key_hash[32] = {0x72, 0x3b, 0xe9, 0xb3, 0x2c, 0x3a, 0xfb, 0x83,
                                         0xa4, 0xa3, 0x75, 0x7a, 0xdf, 0x35, 0x25, 0x29,
                                         0xe9, 0x0c, 0x0a, 0xd6, 0xfa, 0xd5, 0x25, 0x09,
                                         0x96, 0x3b, 0xa8, 0x94, 0x2a, 0xe6, 0x25, 0xdf};
static const u8 s_root_ca_hash[32] = {0xc5, 0xb0, 0xf8, 0xdf, 0xce, 0xc6, 0xb9, 0xed,
                                      0x2a, 0xc3, 0x8b, 0x8b, 0xc6, 0x9a, 0x4d, 0xb7,
                                      0xc2, 0x09, 0xdc, 0x17, 0x7d, 0x24, 0x3c, 0x8d,
                                      0xf2, 0xbd, 0xdf, 0x9e, 0x39, 0x17, 0x1e, 0x5f};

/* name is appended to the session Wii root; data stays NULL and size 0
 * when the file is missing, unreadable or not the expected one */
static bool LoadCertFile(File::FileLoan* cert, const char* name, const u8* correct_hash,
                         bool silent)
{
  char path[PATH_MAX_LENGTH];
  u8 hash[32];
  struct sha256_state sha;

  std::snprintf(path, sizeof(path), "%s/%s", File::GetUserPath(D_SESSION_WIIROOT_IDX).c_str(),
                name);

  if (!File::LoanFile(cert, path))
  {
    ERROR_LOG(IOS_SSL, "Failed to read %s", path);
    if (!silent)
    {
      PanicAlert("IOS: Could not read a file required for SSL services (%s). Please refer to "
                 "https://dolphin-emu.org/docs/guides/wii-network-guide/ for "
                 "instructions on setting up Wii networking.",
                 path);
    }
    return false;
  }

  sha256_stream_init(&sha, 0);
  sha256_stream_update(&sha, cert->data, size_t(cert->size));
  sha256_stream_final(&sha, hash);
  if (std::memcmp(hash, correct_hash, sizeof(hash)) != 0)
  {
    ERROR_LOG(IOS_SSL, "Wrong hash for %s", path);
    if (!silent)
    {
      PanicAlert("IOS: A file required for SSL services (%s) is invalid. Please refer to "
                 "https://dolphin-emu.org/docs/guides/wii-network-guide/ for "
                 "instructions on setting up Wii networking.",
                 path);
    }
    File::EndLoan(cert);
    return false;
  }
  return true;
}

std::optional<IPCReply> NetSSLDevice::IOCtlV(const IOCtlVRequest& request)
{
  u32 BufferIn = 0, BufferIn2 = 0, BufferIn3 = 0;
  u32 BufferInSize = 0, BufferInSize2 = 0, BufferInSize3 = 0;

  u32 BufferOut = 0, BufferOut2 = 0, BufferOut3 = 0;
  u32 BufferOutSize = 0, BufferOutSize2 = 0, BufferOutSize3 = 0;

  if (!request.in_vectors.empty())
  {
    BufferIn = request.in_vectors.at(0).address;
    BufferInSize = request.in_vectors.at(0).size;
  }
  if (request.in_vectors.size() > 1)
  {
    BufferIn2 = request.in_vectors.at(1).address;
    BufferInSize2 = request.in_vectors.at(1).size;
  }
  if (request.in_vectors.size() > 2)
  {
    BufferIn3 = request.in_vectors.at(2).address;
    BufferInSize3 = request.in_vectors.at(2).size;
  }

  if (!request.io_vectors.empty())
  {
    BufferOut = request.io_vectors.at(0).address;
    BufferOutSize = request.io_vectors.at(0).size;
  }
  if (request.io_vectors.size() > 1)
  {
    BufferOut2 = request.io_vectors.at(1).address;
    BufferOutSize2 = request.io_vectors.at(1).size;
  }
  if (request.io_vectors.size() > 2)
  {
    BufferOut3 = request.io_vectors.at(2).address;
    BufferOutSize3 = request.io_vectors.at(2).size;
  }

  // I don't trust SSL to be deterministic, and this is never going to sync
  // as such (as opposed to forwarding IPC results or whatever), so -
  if (Core::WantsDeterminism())
    return IPCReply(IPC_EACCES);

  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();

  switch (request.request)
  {
  case IOCTLV_NET_SSL_NEW:
  {
    int verifyOption = memory.Read_U32(BufferOut);
    std::string hostname = memory.GetString(BufferOut2, BufferOutSize2);

    int freeSSL = GetSSLFreeID();
    if (freeSSL && hostname.size() < sizeof(_SSL[0].hostname))
    {
      int sslID = freeSSL - 1;
      WII_SSL* ssl = &_SSL[sslID];
      ResetSSL(ssl);
      ssl->verify = Config::Get(Config::MAIN_NETWORK_SSL_VERIFY_CERTIFICATES) && verifyOption;
      std::memcpy(ssl->hostname, hostname.c_str(), hostname.size() + 1);
      ssl->active = true;
      WriteReturnValue(memory, freeSSL, BufferIn);
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_FAILED, BufferIn);
    }

    INFO_LOG(IOS_SSL,
             "IOCTLV_NET_SSL_NEW (%d, %s) "
             "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
             "BufferIn3: (%08x, %u), BufferOut: (%08x, %u), "
             "BufferOut2: (%08x, %u), BufferOut3: (%08x, %u)",
             verifyOption, hostname.c_str(), BufferIn, BufferInSize, BufferIn2, BufferInSize2,
             BufferIn3, BufferInSize3, BufferOut, BufferOutSize, BufferOut2, BufferOutSize2,
             BufferOut3, BufferOutSize3);
    break;
  }
  case IOCTLV_NET_SSL_SHUTDOWN:
  {
    int sslID = memory.Read_U32(BufferOut) - 1;
    if (IsSSLIDValid(sslID))
    {
      ResetSSL(&_SSL[sslID]);
      WriteReturnValue(memory, SSL_OK, BufferIn);
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
    }
    INFO_LOG(IOS_SSL,
             "IOCTLV_NET_SSL_SHUTDOWN "
             "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
             "BufferIn3: (%08x, %u), BufferOut: (%08x, %u), "
             "BufferOut2: (%08x, %u), BufferOut3: (%08x, %u)",
             BufferIn, BufferInSize, BufferIn2, BufferInSize2, BufferIn3, BufferInSize3, BufferOut,
             BufferOutSize, BufferOut2, BufferOutSize2, BufferOut3, BufferOutSize3);
    break;
  }
  case IOCTLV_NET_SSL_SETROOTCA:
  {
    INFO_LOG(IOS_SSL,
             "IOCTLV_NET_SSL_SETROOTCA "
             "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
             "BufferIn3: (%08x, %u), BufferOut: (%08x, %u), "
             "BufferOut2: (%08x, %u), BufferOut3: (%08x, %u)",
             BufferIn, BufferInSize, BufferIn2, BufferInSize2, BufferIn3, BufferInSize3, BufferOut,
             BufferOutSize, BufferOut2, BufferOutSize2, BufferOut3, BufferOutSize3);

    int sslID = memory.Read_U32(BufferOut) - 1;
    if (IsSSLIDValid(sslID))
    {
      WII_SSL* ssl = &_SSL[sslID];
      const u8* const der = memory.GetPointerForRange(BufferOut2, BufferOutSize2);
      // the root CA as the guest gives it, DER
      const bool ok = der && BufferOutSize2 &&
                      std::memcmp(der, "-----BEGIN", BufferOutSize2 < 10 ? BufferOutSize2 : 10) &&
                      AddRootCA(ssl, der, BufferOutSize2);

      if (der && Config::Get(Config::MAIN_NETWORK_SSL_DUMP_ROOT_CA))
      {
        std::string filename = File::GetUserPath(D_DUMPSSL_IDX) + ssl->hostname + "_rootca.der";
        File::IOFile(filename, "wb").WriteBytes(der, BufferOutSize2);
      }

      WriteReturnValue(memory, ok ? SSL_OK : SSL_ERR_FAILED, BufferIn);
      INFO_LOG(IOS_SSL, "IOCTLV_NET_SSL_SETROOTCA = %s", ok ? "true" : "false");
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
    }
    break;
  }
  case IOCTLV_NET_SSL_SETBUILTINCLIENTCERT:
  {
    INFO_LOG(IOS_SSL,
             "IOCTLV_NET_SSL_SETBUILTINCLIENTCERT "
             "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
             "BufferIn3: (%08x, %u), BufferOut: (%08x, %u), "
             "BufferOut2: (%08x, %u), BufferOut3: (%08x, %u)",
             BufferIn, BufferInSize, BufferIn2, BufferInSize2, BufferIn3, BufferInSize3, BufferOut,
             BufferOutSize, BufferOut2, BufferOutSize2, BufferOut3, BufferOutSize3);

    int sslID = memory.Read_U32(BufferOut) - 1;
    if (IsSSLIDValid(sslID))
    {
      WII_SSL* ssl = &_SSL[sslID];
      File::FileLoan client_cert;
      File::FileLoan client_key;
      const bool have_cert =
          LoadCertFile(&client_cert, "clientca.pem", s_client_cert_hash, m_cert_error_shown);
      const bool have_key =
          LoadCertFile(&client_key, "clientcakey.pem", s_client_key_hash, m_cert_error_shown);
      // If any of the required files fail to load, show a panic alert, but only once
      // per IOS instance (usually once per emulation session).
      if (!have_cert || !have_key)
        m_cert_error_shown = true;

      size_t cert_len = 0;
      size_t key_len = 0;
      if (have_cert && have_key)
      {
        cert_len = ToDER(client_cert.data, size_t(client_cert.size), ssl->clicert,
                         sizeof(ssl->clicert));
        key_len =
            ToDER(client_key.data, size_t(client_key.size), ssl->clikey, sizeof(ssl->clikey));
      }
      File::EndLoan(&client_cert);
      File::EndLoan(&client_key);

      // the key's members point into clikey, which stays until shutdown
      ssl->have_client_cert =
          cert_len && key_len && x509_rsa_private_key_parse(&ssl->key, ssl->clikey, key_len) == 0;
      ssl->clicert_ptr = ssl->have_client_cert ? ssl->clicert : nullptr;
      ssl->clicert_len = ssl->have_client_cert ? cert_len : 0;
      WriteReturnValue(memory, ssl->have_client_cert ? SSL_OK : SSL_ERR_FAILED, BufferIn);

      INFO_LOG(IOS_SSL, "IOCTLV_NET_SSL_SETBUILTINCLIENTCERT = %s",
               ssl->have_client_cert ? "true" : "false");
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
      INFO_LOG(IOS_SSL, "IOCTLV_NET_SSL_SETBUILTINCLIENTCERT invalid sslID = %d", sslID);
    }
    break;
  }
  case IOCTLV_NET_SSL_REMOVECLIENTCERT:
  {
    INFO_LOG(IOS_SSL,
             "IOCTLV_NET_SSL_REMOVECLIENTCERT "
             "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
             "BufferIn3: (%08x, %u), BufferOut: (%08x, %u), "
             "BufferOut2: (%08x, %u), BufferOut3: (%08x, %u)",
             BufferIn, BufferInSize, BufferIn2, BufferInSize2, BufferIn3, BufferInSize3, BufferOut,
             BufferOutSize, BufferOut2, BufferOutSize2, BufferOut3, BufferOutSize3);

    int sslID = memory.Read_U32(BufferOut) - 1;
    if (IsSSLIDValid(sslID))
    {
      WII_SSL* ssl = &_SSL[sslID];
      ssl->have_client_cert = false;
      ssl->clicert_ptr = nullptr;
      ssl->clicert_len = 0;
      WriteReturnValue(memory, SSL_OK, BufferIn);
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
      INFO_LOG(IOS_SSL, "IOCTLV_NET_SSL_SETBUILTINCLIENTCERT invalid sslID = %d", sslID);
    }
    break;
  }
  case IOCTLV_NET_SSL_SETBUILTINROOTCA:
  {
    int sslID = memory.Read_U32(BufferOut) - 1;
    if (IsSSLIDValid(sslID))
    {
      WII_SSL* ssl = &_SSL[sslID];
      File::FileLoan root_ca;
      bool ok = false;
      if (!LoadCertFile(&root_ca, "rootca.pem", s_root_ca_hash, m_cert_error_shown))
        m_cert_error_shown = true;
      else
        ok = AddRootCA(ssl, root_ca.data, size_t(root_ca.size));
      File::EndLoan(&root_ca);
      WriteReturnValue(memory, ok ? SSL_OK : SSL_ERR_FAILED, BufferIn);
      INFO_LOG(IOS_SSL, "IOCTLV_NET_SSL_SETBUILTINROOTCA = %s", ok ? "true" : "false");
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
    }
    INFO_LOG(IOS_SSL,
             "IOCTLV_NET_SSL_SETBUILTINROOTCA "
             "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
             "BufferIn3: (%08x, %u), BufferOut: (%08x, %u), "
             "BufferOut2: (%08x, %u), BufferOut3: (%08x, %u)",
             BufferIn, BufferInSize, BufferIn2, BufferInSize2, BufferIn3, BufferInSize3, BufferOut,
             BufferOutSize, BufferOut2, BufferOutSize2, BufferOut3, BufferOutSize3);
    break;
  }
  case IOCTLV_NET_SSL_CONNECT:
  {
    int sslID = memory.Read_U32(BufferOut) - 1;
    if (IsSSLIDValid(sslID))
    {
      WII_SSL* ssl = &_SSL[sslID];
      ssl->sockfd = memory.Read_U32(BufferOut2);
      ssl->hostfd = GetEmulationKernel().GetSocketManager()->GetHostSocket(ssl->sockfd);
      INFO_LOG(IOS_SSL, "IOCTLV_NET_SSL_CONNECT socket = %d", ssl->sockfd);
      WriteReturnValue(memory, SSL_OK, BufferIn);
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
    }
    INFO_LOG(IOS_SSL,
             "IOCTLV_NET_SSL_CONNECT "
             "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
             "BufferIn3: (%08x, %u), BufferOut: (%08x, %u), "
             "BufferOut2: (%08x, %u), BufferOut3: (%08x, %u)",
             BufferIn, BufferInSize, BufferIn2, BufferInSize2, BufferIn3, BufferInSize3, BufferOut,
             BufferOutSize, BufferOut2, BufferOutSize2, BufferOut3, BufferOutSize3);
    break;
  }
  case IOCTLV_NET_SSL_DOHANDSHAKE:
  {
    int sslID = memory.Read_U32(BufferOut) - 1;
    if (IsSSLIDValid(sslID))
    {
      GetEmulationKernel().GetSocketManager()->DoSock(_SSL[sslID].sockfd, request,
                                                      IOCTLV_NET_SSL_DOHANDSHAKE);
      return std::nullopt;
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
    }
    break;
  }
  case IOCTLV_NET_SSL_WRITE:
  {
    const int sslID = memory.Read_U32(BufferOut) - 1;
    if (IsSSLIDValid(sslID))
    {
      GetEmulationKernel().GetSocketManager()->DoSock(_SSL[sslID].sockfd, request,
                                                      IOCTLV_NET_SSL_WRITE);
      return std::nullopt;
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
    }
    INFO_LOG(IOS_SSL,
             "IOCTLV_NET_SSL_WRITE "
             "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
             "BufferIn3: (%08x, %u), BufferOut: (%08x, %u), "
             "BufferOut2: (%08x, %u), BufferOut3: (%08x, %u)",
             BufferIn, BufferInSize, BufferIn2, BufferInSize2, BufferIn3, BufferInSize3, BufferOut,
             BufferOutSize, BufferOut2, BufferOutSize2, BufferOut3, BufferOutSize3);
    INFO_LOG(IOS_SSL, "%s", memory.GetString(BufferOut2).c_str());
    break;
  }
  case IOCTLV_NET_SSL_READ:
  {
    int ret = 0;
    int sslID = memory.Read_U32(BufferOut) - 1;
    if (IsSSLIDValid(sslID))
    {
      GetEmulationKernel().GetSocketManager()->DoSock(_SSL[sslID].sockfd, request,
                                                      IOCTLV_NET_SSL_READ);
      return std::nullopt;
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
    }

    INFO_LOG(IOS_SSL,
             "IOCTLV_NET_SSL_READ(%d)"
             "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
             "BufferIn3: (%08x, %u), BufferOut: (%08x, %u), "
             "BufferOut2: (%08x, %u), BufferOut3: (%08x, %u)",
             ret, BufferIn, BufferInSize, BufferIn2, BufferInSize2, BufferIn3, BufferInSize3,
             BufferOut, BufferOutSize, BufferOut2, BufferOutSize2, BufferOut3, BufferOutSize3);
    break;
  }
  case IOCTLV_NET_SSL_SETROOTCADEFAULT:
  {
    int sslID = memory.Read_U32(BufferOut) - 1;
    if (IsSSLIDValid(sslID))
    {
      WriteReturnValue(memory, SSL_OK, BufferIn);
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
    }
    INFO_LOG(IOS_SSL,
             "IOCTLV_NET_SSL_SETROOTCADEFAULT "
             "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
             "BufferIn3: (%08x, %u), BufferOut: (%08x, %u), "
             "BufferOut2: (%08x, %u), BufferOut3: (%08x, %u)",
             BufferIn, BufferInSize, BufferIn2, BufferInSize2, BufferIn3, BufferInSize3, BufferOut,
             BufferOutSize, BufferOut2, BufferOutSize2, BufferOut3, BufferOutSize3);
    break;
  }
  case IOCTLV_NET_SSL_SETCLIENTCERTDEFAULT:
  {
    INFO_LOG(IOS_SSL,
             "IOCTLV_NET_SSL_SETCLIENTCERTDEFAULT "
             "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
             "BufferIn3: (%08x, %u), BufferOut: (%08x, %u), "
             "BufferOut2: (%08x, %u), BufferOut3: (%08x, %u)",
             BufferIn, BufferInSize, BufferIn2, BufferInSize2, BufferIn3, BufferInSize3, BufferOut,
             BufferOutSize, BufferOut2, BufferOutSize2, BufferOut3, BufferOutSize3);

    int sslID = memory.Read_U32(BufferOut) - 1;
    if (IsSSLIDValid(sslID))
    {
      WriteReturnValue(memory, SSL_OK, BufferIn);
    }
    else
    {
      WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
    }
    break;
  }
  default:
    request.DumpUnknown(system, GetDeviceName(), Common::Log::LogType::IOS_SSL);
  }

  // SSL return codes are written to BufferIn
  return IPCReply(IPC_SUCCESS);
}
}  // namespace IOS::HLE
