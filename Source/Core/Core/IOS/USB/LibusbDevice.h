// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__LIBUSB__)
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <array>

#include <retro_atomic.h>

#include "Common/CallGate.h"
#include "Common/CommonTypes.h"
#include "Core/IOS/USB/Common.h"
#include "Core/LibusbUtils.h"

struct libusb_device;
struct libusb_device_descriptor;
struct libusb_device_handle;
struct libusb_transfer;

namespace IOS::HLE::USB
{
class LibusbDevice final : public Device
{
public:
  LibusbDevice(libusb_device* device, const libusb_device_descriptor& device_descriptor);
  ~LibusbDevice() override;
  DeviceDescriptor GetDeviceDescriptor() const override;
  std::vector<ConfigDescriptor> GetConfigurations() const override;
  std::vector<InterfaceDescriptor> GetInterfaces(u8 config) const override;
  std::vector<EndpointDescriptor> GetEndpoints(u8 config, u8 iface, u8 alt) const override;
  std::string GetErrorName(int error_code) const override;
  bool Attach() override;
  bool AttachAndChangeInterface(u8 iface) override;
  int CancelTransfer(u8 endpoint) override;
  int ChangeInterface(u8 iface) override;
  int GetNumberOfAltSettings(u8 iface) override;
  int SetAltSetting(u8 alt_setting) override;
  int SubmitTransfer(std::unique_ptr<CtrlMessage> message) override;
  int SubmitTransfer(std::unique_ptr<BulkMessage> message) override;
  int SubmitTransfer(std::unique_ptr<IntrMessage> message) override;
  int SubmitTransfer(std::unique_ptr<IsoMessage> message) override;

private:
  std::vector<LibusbUtils::ConfigDescriptor> m_config_descriptors;
  u16 m_vid = 0;
  u16 m_pid = 0;
  u16 m_spoofed_vid = 0;
  u16 m_spoofed_pid = 0;
  u8 m_active_interface = 0;
  bool m_device_attached = false;
  bool m_needs_playstation_rock_band_3_instrument_control_transfer = false;

  libusb_device* m_device = nullptr;
  libusb_device_handle* m_handle = nullptr;

  class TransferEndpoint final
  {
  public:
    void AddTransfer(std::unique_ptr<TransferCommand> command, libusb_transfer* transfer);
    void HandleTransfer(libusb_transfer* tr,
                        const std::function<s32(const TransferCommand&)>& function);
    void CancelTransfers();
    // Whether any transfer was ever queued here.
    bool IsUsed() const { return m_used; }

    TransferEndpoint() = default;
    TransferEndpoint(const TransferEndpoint&) = delete;
    TransferEndpoint& operator=(const TransferEndpoint&) = delete;
    ~TransferEndpoint();

  private:
    // A transfer in flight. Queued by the emulation thread, completed (and freed) by libusb's
    // event thread, cancelled by the emulation thread: each slot is claimed with a CAS and
    // given up with an exchange, and a completion waits out a cancel loop that may still be
    // looking at its transfer before libusb frees it.
    struct InFlight
    {
      libusb_transfer* transfer;
      std::unique_ptr<TransferCommand> command;
    };
    static constexpr size_t MAX_IN_FLIGHT = 64;
    std::array<retro_atomic_ptr_t, MAX_IN_FLIGHT> m_in_flight{};
    call_gate_t m_cancel_gate{};
    bool m_used = false;  // emulation thread
  };
  // Indexed by endpoint address, so the event thread never looks one up in a changing map.
  std::array<TransferEndpoint, 256> m_transfer_endpoints;
  static void CtrlTransferCallback(libusb_transfer* transfer);
  static void TransferCallback(libusb_transfer* transfer);

  void DisguisePlayStationDevice();
  int SubmitPlayStationRockBand3InstrumentControlTransfer();

  int ClaimAllInterfaces(u8 config_num) const;
  int ReleaseAllInterfaces(u8 config_num) const;
  int ReleaseAllInterfacesForCurrentConfig() const;
};
}  // namespace IOS::HLE::USB
#endif
