// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__LIBUSB__)
#include <string>

#include <retro_atomic.h>

#include "Common/CommonTypes.h"
#include "Common/Timer.h"

#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTBase.h"
#include "Core/IOS/USB/Bluetooth/LibUSBBluetoothAdapter.h"
#include "Core/IOS/USB/Bluetooth/hci.h"
#include "Core/IOS/USB/USBV0.h"

class PointerWrap;

namespace IOS::HLE
{
enum class SyncButtonState
{
  Unpressed,
  Held,
  Pressed,
  LongPressed,
  // On a real Wii, after a long press, the button release is ignored and doesn't trigger a sync.
  Ignored,
};

typedef u8 linkkey_t[16];

class BluetoothRealDevice final : public BluetoothBaseDevice
{
public:
  BluetoothRealDevice(EmulationKernel& ios, const char* device_name);
  ~BluetoothRealDevice() override;

  IPCReply Open(const OpenRequest& request) override;
  IPCReply Close(u32 fd) override;
  IPCReply IOCtlV(const IOCtlVRequest& request) override;
  void Update() override;

  void DoState(PointerWrap& p) override;
  void TriggerSyncButtonPressedEvent() override;

private:
  using BufferType = LibUSBBluetoothAdapter::BufferType;

  LibUSBBluetoothAdapter* m_lib_usb_bt_adapter = nullptr;  // owned

  static constexpr u32 SYNC_BUTTON_HOLD_MS_TO_RESET = 10000;

  retro_atomic_int_t m_sync_button_state{}; /* a SyncButtonState; 0 is Unpressed */
  SyncButtonState GetSyncButtonState() const
  {
    return static_cast<SyncButtonState>(retro_atomic_load_acquire_int(&m_sync_button_state));
  }
  void SetSyncButtonState(SyncButtonState state)
  {
    retro_atomic_store_release_int(&m_sync_button_state, static_cast<int>(state));
  }
  Common::Timer m_sync_button_held_timer;

  // Enqueued when we observe a command to which we need to fake a reply.
  // The vendor command opcodes still to fake a reply to, oldest first
  static constexpr u32 MAX_FAKE_REPLIES = 32;
  u16 m_fake_reply_opcodes[MAX_FAKE_REPLIES] = {};
  u32 m_fake_reply_head = 0;
  u32 m_fake_reply_count = 0;

  // This stores the address of paired devices and associated link keys, sorted by address.
  // It is needed because some adapters forget all stored link keys when they are reset,
  // which breaks pairings because the Wii relies on the Bluetooth module to remember them.
  struct LinkKeyEntry
  {
    bdaddr_t bdaddr;
    linkkey_t linkkey;
  };
  static constexpr std::size_t MAX_STORED_LINK_KEYS = 64;
  LinkKeyEntry m_link_keys[MAX_STORED_LINK_KEYS];
  std::size_t m_link_key_count = 0;

  void SetLinkKey(const bdaddr_t& bdaddr, const linkkey_t& linkkey);
  void EraseLinkKey(const bdaddr_t& bdaddr);

  // Endpoints provided by the emulated software.
  // Owned, the requests waiting for an HCI event and ACL data
  USB::V0IntrMessage* m_hci_endpoint = nullptr;
  USB::V0BulkMessage* m_acl_endpoint = nullptr;

  // Used for proper Bluetooth packet timing, especially for Wii remote speaker data.

  void TryToFillHCIEndpoint();
  void TryToFillACLEndpoint();

  [[nodiscard]] BufferType ProcessHCIEvent(BufferType buffer);

  void SendHCIResetCommand();
  void SendHCIDeleteLinkKeyCommand();
  bool SendHCIStoreLinkKeyCommand();

  void FakeVendorCommandReply(u16 opcode, USB::V0IntrMessage& ctrl);

  void FakeSyncButtonEvent(USB::V0IntrMessage& ctrl, const u8* payload, std::size_t size);
  void FakeSyncButtonPressedEvent(USB::V0IntrMessage& ctrl);
  void FakeSyncButtonHeldEvent(USB::V0IntrMessage& ctrl);

  void LoadLinkKeys();
  void SaveLinkKeys();
};
}  // namespace IOS::HLE

#else
#include "Core/IOS/USB/Bluetooth/BTStub.h"

namespace IOS::HLE
{
using BluetoothRealDevice = BluetoothStubDevice;
}  // namespace IOS::HLE
#endif
