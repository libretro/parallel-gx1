// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// PRELIMINARY - seems to fully work with libogc, writing has yet to be tested

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"

class PointerWrap;

namespace IOS::HLE
{
// The front SD slot
class SDIOSlot0Device : public EmulationDevice
{
public:
  SDIOSlot0Device(EmulationKernel& ios, const std::string& device_name);
  ~SDIOSlot0Device() override;

  void DoState(PointerWrap& p) override;

  IPCReply Open(const OpenRequest& request) override;
  IPCReply Close(u32 fd) override;
  IPCReply IOCtl(const IOCtlRequest& request) override;
  IPCReply IOCtlV(const IOCtlVRequest& request) override;

private:
  // SD Host Controller Registers
  enum
  {
    HCR_CLOCKCONTROL = 0x2C,
    HCR_SOFTWARERESET = 0x2F,
  };

  // IOCtl
  enum
  {
    IOCTL_WRITEHCR = 0x01,
    IOCTL_READHCR = 0x02,
    IOCTL_RESETCARD = 0x04,
    IOCTL_SETCLK = 0x06,
    IOCTL_SENDCMD = 0x07,
    IOCTL_GETSTATUS = 0x0B,
    IOCTL_GETOCR = 0x0C,
  };

  // IOCtlV
  enum
  {
    IOCTLV_SENDCMD = 0x07,
  };

  // ExecuteCommand
  enum
  {
    RET_OK,
    RET_FAIL,
    RET_LOCKED = -10,
    RET_EVENT_REGISTER,  // internal state only - not actually returned
  };

  // Status
  enum
  {
    CARD_NOT_EXIST = 0,
    CARD_INSERTED = 0x1,
    CARD_LOCKED = 0x4,
    CARD_INITIALIZED = 0x10000,
    CARD_SDHC = 0x100000,
  };

  // Commands
  enum
  {
    GO_IDLE_STATE = 0x00,
    ALL_SEND_CID = 0x02,
    SEND_RELATIVE_ADDR = 0x03,
    SELECT_CARD = 0x07,
    SEND_IF_COND = 0x08,
    SEND_CSD = 0x09,
    SEND_CID = 0x0A,
    SEND_STATUS = 0x0D,
    SET_BLOCKLEN = 0x10,
    READ_MULTIPLE_BLOCK = 0x12,
    WRITE_MULTIPLE_BLOCK = 0x19,
    APP_CMD_NEXT = 0x37,

    ACMD_SETBUSWIDTH = 0x06,
    ACMD_SENDOPCOND = 0x29,
    ACMD_SENDSCR = 0x33,

    EVENT_REGISTER = 0x40,
    EVENT_UNREGISTER = 0x41,
  };

  enum EventType
  {
    EVENT_INSERT = 1,
    EVENT_REMOVE = 2,
    // from unregister, i think it is just meant to be invalid
    EVENT_INVALID = 0xc210000
  };

  enum class SDProtocol
  {
    V1,
    V2,
  };

  // Maximum number of bytes in an SDSC card
  // Used to trigger using SDHC instead of SDSC
  static constexpr u64 SDSC_MAX_SIZE = 0x80000000;

  void RefreshConfig();

  void EventNotify();

  IPCReply WriteHCRegister(const IOCtlRequest& request);
  IPCReply ReadHCRegister(const IOCtlRequest& request);
  IPCReply ResetCard(const IOCtlRequest& request);
  IPCReply SetClk(const IOCtlRequest& request);
  IPCReply SendCommand(const IOCtlRequest& request);
  IPCReply GetStatus(const IOCtlRequest& request);
  IPCReply GetOCRegister(const IOCtlRequest& request);

  IPCReply SendCommand(const IOCtlVRequest& request);

  s32 ExecuteCommand(const Request& request, u32 buffer_in, u32 buffer_in_size, u32 rw_buffer,
                     u32 rw_buffer_size, u32 buffer_out, u32 buffer_out_size);
  void OpenInternal();

  u32 GetOCRegister() const;

  // csd: 4 words
  void GetCSDv1(u32* csd) const;
  void GetCSDv2(u32* csd) const;
  void InitSDHC();

  u64 GetAddressFromRequest(u32 arg) const;

  // The request waiting for an event, kept by address as the guest leaves it until the reply;
  // 0 for none. TODO: do we need more than one?
  u32 m_event_request_address = 0;
  EventType m_event_type = EVENT_INVALID;

  u32 m_status = CARD_NOT_EXIST;
  SDProtocol m_protocol = SDProtocol::V1;

  // Is SDHC supported by the IOS?
  // Other IOS requires manual SDHC initialization
  const bool m_sdhc_supported;

  u32 m_block_length = 0;
  u32 m_bus_width = 0;

  u32 m_registers[0x200 / sizeof(u32)]{};

  File::IOFile m_card;

  CPUThreadConfigCallback::ConfigChangedCallbackID m_config_callback_id;
  bool m_sd_card_inserted = false;
};
}  // namespace IOS::HLE
