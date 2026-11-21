// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/EXI_DeviceModem.h"

namespace ExpansionInterface
{

CEXIModem::TAPServerNetworkInterface::TAPServerNetworkInterface(CEXIModem* modem_ref,
                                                                const char* destination)
    : NetworkInterface(modem_ref),
      m_tapserver_if(destination, &TAPServerNetworkInterface::HandleReceivedFrame, this,
                     MODEM_RECV_SIZE)
{
}

bool CEXIModem::TAPServerNetworkInterface::Activate()
{
  return m_tapserver_if.Activate();
}

void CEXIModem::TAPServerNetworkInterface::Deactivate()
{
  m_tapserver_if.Deactivate();
}

bool CEXIModem::TAPServerNetworkInterface::IsActivated()
{
  return m_tapserver_if.IsActivated();
}

bool CEXIModem::TAPServerNetworkInterface::SendAndRemoveAllHDLCFrames(u8* send_buffer,
                                                                      std::size_t* send_size)
{
  const std::size_t orig_size = *send_size;
  const bool send_succeeded = m_tapserver_if.SendAndRemoveAllHDLCFrames(send_buffer, send_size);
  if (send_succeeded && (*send_size < orig_size))
    m_modem_ref->SendComplete();
  return send_succeeded;
}

bool CEXIModem::TAPServerNetworkInterface::RecvInit()
{
  return m_tapserver_if.RecvInit();
}

void CEXIModem::TAPServerNetworkInterface::RecvStart()
{
  m_tapserver_if.RecvStart();
}

void CEXIModem::TAPServerNetworkInterface::RecvStop()
{
  m_tapserver_if.RecvStop();
}

void CEXIModem::TAPServerNetworkInterface::HandleReceivedFrame(void* user, const u8* data,
                                                               std::size_t size)
{
  static_cast<TAPServerNetworkInterface*>(user)->m_modem_ref->AddToReceiveBuffer(data, size);
}

}  // namespace ExpansionInterface
