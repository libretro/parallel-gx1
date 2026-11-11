// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/ES/ES.h"

#include <utility>
#include <vector>

#include "Common/Crypto/SHA1.h"
#include "Common/Crypto/ec.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"
#include "Core/System.h"

namespace IOS::HLE
{
ReturnCode ESCore::GetDeviceId(u32* device_id) const
{
  *device_id = m_ios.GetIOSC().GetDeviceId();
  INFO_LOG(IOS_ES, "GetDeviceId: %08X", *device_id);
  return IPC_SUCCESS;
}

IPCReply ESDevice::GetDeviceId(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != sizeof(u32))
    return IPCReply(ES_EINVAL);

  u32 device_id;
  const ReturnCode ret = m_core.GetDeviceId(&device_id);
  if (ret != IPC_SUCCESS)
    return IPCReply(ret);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  memory.Write_U32(device_id, request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::Encrypt(u32 uid, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 2))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  u32 keyIndex = memory.Read_U32(request.in_vectors[0].address);
  u32 size = request.in_vectors[2].size;
  u8* source = memory.GetPointerForRange(request.in_vectors[2].address, size);
  u8* iv = memory.GetPointerForRange(request.io_vectors[0].address, 16);
  u8* destination = memory.GetPointerForRange(request.io_vectors[1].address, size);

  // TODO: Check whether the active title is allowed to encrypt.

  const ReturnCode ret =
      GetEmulationKernel().GetIOSC().Encrypt(keyIndex, iv, source, size, destination, PID_ES);
  return IPCReply(ret);
}

IPCReply ESDevice::Decrypt(u32 uid, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 2))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  u32 keyIndex = memory.Read_U32(request.in_vectors[0].address);
  u32 size = request.in_vectors[2].size;
  u8* source = memory.GetPointerForRange(request.in_vectors[2].address, size);
  u8* iv = memory.GetPointerForRange(request.io_vectors[0].address, 16);
  u8* destination = memory.GetPointerForRange(request.io_vectors[1].address, size);

  // TODO: Check whether the active title is allowed to decrypt.

  const ReturnCode ret =
      GetEmulationKernel().GetIOSC().Decrypt(keyIndex, iv, source, size, destination, PID_ES);
  return IPCReply(ret);
}

IPCReply ESDevice::CheckKoreaRegion(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 0))
    return IPCReply(ES_EINVAL);

  // note by DacoTaco : name is unknown, I just tried to name it SOMETHING.
  // IOS70 has this to let system menu 4.2 check if the console is region changed. it returns
  // -1017
  // if the IOS didn't find the Korean keys and 0 if it does. 0 leads to a error 003
  INFO_LOG(IOS_ES, "IOCTL_ES_CHECKKOREAREGION: Title checked for Korean keys.");
  return IPCReply(ES_EINVAL);
}

IPCReply ESDevice::GetDeviceCertificate(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != 0x180)
    return IPCReply(ES_EINVAL);

  INFO_LOG(IOS_ES, "IOCTL_ES_GETDEVICECERT");

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const IOS::CertECC cert = GetEmulationKernel().GetIOSC().GetDeviceCertificate();
  memory.CopyToEmu(request.io_vectors[0].address, &cert, sizeof(cert));
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::Sign(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 2))
    return IPCReply(ES_EINVAL);

  INFO_LOG(IOS_ES, "IOCTL_ES_SIGN");
  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  u8* ap_cert_out = memory.GetPointerForRange(request.io_vectors[1].address, sizeof(CertECC));
  u32 data_size = request.in_vectors[0].size;
  u8* data = memory.GetPointerForRange(request.in_vectors[0].address, data_size);
  u8* sig_out =
      memory.GetPointerForRange(request.io_vectors[0].address, sizeof(Common::ec::Signature));

  if (!m_core.m_title_context.active)
    return IPCReply(ES_EINVAL);

  GetEmulationKernel().GetIOSC().Sign(sig_out, ap_cert_out, m_core.m_title_context.tmd.GetTitleId(),
                                      data, data_size);
  return IPCReply(IPC_SUCCESS);
}

ReturnCode ESCore::VerifySign(const u8* hash, size_t hash_size, const u8* ecc_signature,
                              const u8* certs_bytes, size_t certs_size)
{
  const std::map<std::string, ES::CertReader> certs = ES::ParseCertChain(certs_bytes, certs_size);
  if (certs.empty())
    return ES_EINVAL;

  auto ap_iterator = certs.begin();
  for (; ap_iterator != certs.end(); ++ap_iterator)
  {
    if (ap_iterator->first.length() > 2 && ap_iterator->first.compare(0, 2, "AP") == 0)
      break;
  }
  if (ap_iterator == certs.end())
    return ES_UNKNOWN_ISSUER;
  const ES::CertReader& ap = ap_iterator->second;

  const auto ap_issuers = SplitString(ap.GetIssuer(), '-');
  const auto ng_iterator = ap_issuers.size() > 1 ? certs.find(*ap_issuers.rbegin()) : certs.end();
  if (ng_iterator == certs.end())
    return ES_UNKNOWN_ISSUER;
  const ES::CertReader& ng = ng_iterator->second;

  IOSC& iosc = m_ios.GetIOSC();
  IOSC::Handle ng_cert;
  ReturnCode ret =
      iosc.CreateObject(&ng_cert, IOSC::TYPE_PUBLIC_KEY, IOSC::ObjectSubType::ECC233, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;
  Common::ScopeGuard handle_guard{[&] { iosc.DeleteObject(ng_cert, PID_ES); }};

  ret = VerifyContainer(VerifyContainerType::Device, VerifyMode::DoNotUpdateCertStore, ng,
                        certs_bytes, certs_size, ng_cert);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG(IOS_ES, "VerifySign: VerifyContainer(ng) failed with error %d",
              std::to_underlying(ret));
    return ret;
  }

  size_t ap_signature_size;
  const u8* const ap_signature = ap.GetSignatureData(&ap_signature_size);
  ret = iosc.VerifyPublicKeySign(ap.GetSha1(), ng_cert, ap_signature, ap_signature_size, PID_ES);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG(IOS_ES, "VerifySign: IOSC_VerifyPublicKeySign(ap) failed with error %d",
              std::to_underlying(ret));
    return ret;
  }

  IOSC::Handle ap_cert;
  ret = iosc.CreateObject(&ap_cert, IOSC::TYPE_PUBLIC_KEY, IOSC::ObjectSubType::ECC233, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;
  Common::ScopeGuard handle2_guard{[&] { iosc.DeleteObject(ap_cert, PID_ES); }};

  size_t ap_key_size;
  ret = iosc.ImportPublicKey(ap_cert, ap.GetPublicKey(&ap_key_size), nullptr, PID_ES);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG(IOS_ES, "VerifySign: IOSC_ImportPublicKey(ap) failed with error %d",
              std::to_underlying(ret));
    return ret;
  }

  const auto hash_digest = Common::SHA1::CalculateDigest(hash, hash_size);
  ret = iosc.VerifyPublicKeySign(hash_digest, ap_cert, ecc_signature,
                                 sizeof(Common::ec::Signature), PID_ES);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG(IOS_ES, "VerifySign: IOSC_VerifyPublicKeySign(data) failed with error %d",
              std::to_underlying(ret));
    return ret;
  }

  INFO_LOG(IOS_ES, "VerifySign: all checks passed");
  return IPC_SUCCESS;
}

IPCReply ESDevice::VerifySign(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 0))
    return IPCReply(ES_EINVAL);
  if (request.in_vectors[1].size != sizeof(Common::ec::Signature))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  // all three read where they lie in guest memory
  const u8* const hash =
      memory.GetPointerForRange(request.in_vectors[0].address, request.in_vectors[0].size);
  const u8* const ecc_signature =
      memory.GetPointerForRange(request.in_vectors[1].address, request.in_vectors[1].size);
  const u8* const certs =
      memory.GetPointerForRange(request.in_vectors[2].address, request.in_vectors[2].size);
  if ((!hash && request.in_vectors[0].size) || !ecc_signature ||
      (!certs && request.in_vectors[2].size))
  {
    return IPCReply(ES_EINVAL);
  }

  return IPCReply(m_core.VerifySign(hash, request.in_vectors[0].size, ecc_signature, certs,
                                    request.in_vectors[2].size));
}
}  // namespace IOS::HLE
