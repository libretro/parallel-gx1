// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/USB_HID/HIDv5.h"

#include <algorithm>
#include <memory>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/USB/Common.h"
#include "Core/System.h"

namespace IOS::HLE
{
USB_HIDv5::~USB_HIDv5() = default;

IPCReply USB_HIDv5::IOCtl(const IOCtlRequest& request)
{
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_USB);
  switch (request.request)
  {
  case USB::IOCTL_USBV5_GETVERSION:
    return GetUSBVersion(request);
  case USB::IOCTL_USBV5_GETDEVICECHANGE:
    return GetDeviceChange(request);
  case USB::IOCTL_USBV5_SHUTDOWN:
    return Shutdown(request);
  case USB::IOCTL_USBV5_GETDEVPARAMS:
  {
    USBV5Device* const device = GetIOCtlDevice(request);
    return device ? GetDeviceInfo(*device, request) : IPCReply(IPC_EINVAL);
  }
  case USB::IOCTL_USBV5_ATTACHFINISH:
    return IPCReply(IPC_SUCCESS);
  case USB::IOCTL_USBV5_SUSPEND_RESUME:
  {
    USBV5Device* const device = GetIOCtlDevice(request);
    return device ? SuspendResume(*device, request) : IPCReply(IPC_EINVAL);
  }
  case USB::IOCTL_USBV5_CANCELENDPOINT:
  {
    USBV5Device* const device = GetIOCtlDevice(request);
    return device ? CancelEndpoint(*device, request) : IPCReply(IPC_EINVAL);
  }
  default:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB,
                        Common::Log::LogLevel::LERROR);
    return IPCReply(IPC_SUCCESS);
  }
}

IPCReply USB_HIDv5::IOCtlV(const IOCtlVRequest& request)
{
  switch (request.request)
  {
  // TODO: HIDv5 seems to be able to queue transfers depending on the transfer length (unlike VEN).
  case USB::IOCTLV_USBV5_CTRLMSG:
  case USB::IOCTLV_USBV5_INTRMSG:
  {
    // IOS does not check the number of vectors, but let's do that to avoid out-of-bounds reads.
    if (request.in_vectors.size() + request.io_vectors.size() != 2)
      return IPCReply(IPC_EINVAL);
    USBV5Device* device = GetUSBV5Device(request.in_vectors[0].address);
    if (!device)
      return IPCReply(IPC_EINVAL);
    auto host_device = GetDeviceById(device->host_id);
    if (request.request == USB::IOCTLV_USBV5_CTRLMSG)
      host_device->Attach();
    else
      host_device->AttachAndChangeInterface(device->interface_number);
    if (!host_device)
      return IPCReply(IPC_ENOENT);
    return TransferReply(*host_device, request.request,
                         SubmitTransfer(*device, *host_device, request));
  }
  default:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB);
    return IPCReply(IPC_EINVAL);
  }
}

s32 USB_HIDv5::SubmitTransfer(USBV5Device& device, USB::Device& host_device,
                              const IOCtlVRequest& ioctlv)
{
  switch (ioctlv.request)
  {
  case USB::IOCTLV_USBV5_CTRLMSG:
    return host_device.SubmitTransfer(new USB::V5CtrlMessage(GetEmulationKernel(), ioctlv));
  case USB::IOCTLV_USBV5_INTRMSG:
  {
    USB::V5IntrMessage* const message = new USB::V5IntrMessage(GetEmulationKernel(), ioctlv);

    auto& system = GetSystem();
    auto& memory = system.GetMemory();

    // Unlike VEN, the endpoint is determined by the value at 8-12.
    // If it's non-zero, HID submits the request to the interrupt OUT endpoint.
    // Otherwise, the request is submitted to the IN endpoint.
    AdditionalDeviceData* data = &m_additional_device_data[&device - m_usbv5_devices];
    if (memory.Read_U32(ioctlv.in_vectors[0].address + 8) != 0)
      message->endpoint = data->interrupt_out_endpoint;
    else
      message->endpoint = data->interrupt_in_endpoint;

    return host_device.SubmitTransfer(message);
  }
  default:
    return IPC_EINVAL;
  }
}

IPCReply USB_HIDv5::CancelEndpoint(USBV5Device& device, const IOCtlRequest& request)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u8 value = memory.Read_U8(request.buffer_in + 8);
  u8 endpoint = 0;
  switch (value)
  {
  case 0:
    // TODO: cancel all queued control transfers with return code -7022.
    endpoint = 0;
    break;
  case 1:
    // TODO: cancel all queued interrupt transfers with return code -7022.
    endpoint = m_additional_device_data[&device - m_usbv5_devices].interrupt_in_endpoint;
    break;
  case 2:
    // TODO: cancel all queued interrupt transfers with return code -7022.
    endpoint = m_additional_device_data[&device - m_usbv5_devices].interrupt_out_endpoint;
    break;
  }

  GetDeviceById(device.host_id)->CancelTransfer(endpoint);
  return IPCReply(IPC_SUCCESS);
}

IPCReply USB_HIDv5::GetDeviceInfo(USBV5Device& device, const IOCtlRequest& request)
{
  if (request.buffer_out == 0 || request.buffer_out_size != 0x60)
    return IPCReply(IPC_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  USB::Device* const host_device = GetDeviceById(device.host_id);
  const u8 alt_setting = memory.Read_U8(request.buffer_in + 8);

  memory.Memset(request.buffer_out, 0, request.buffer_out_size);
  memory.Write_U32(memory.Read_U32(request.buffer_in), request.buffer_out);
  memory.Write_U32(1, request.buffer_out + 4);

  USB::DeviceDescriptor device_descriptor = host_device->GetDeviceDescriptor();
  device_descriptor.Swap();
  memory.CopyToEmu(request.buffer_out + 36, &device_descriptor, sizeof(device_descriptor));

  // Just like VEN, HIDv5 only cares about the first configuration.
  USB::ConfigDescriptor config_descriptor;
  if (host_device->GetConfigurations(&config_descriptor, 1) == 0)
    return IPCReply(IPC_EINVAL);
  config_descriptor.Swap();
  memory.CopyToEmu(request.buffer_out + 56, &config_descriptor, sizeof(config_descriptor));

  USB::InterfaceDescriptor interfaces[USB::MAX_INTERFACES];
  const size_t interface_count = host_device->GetInterfaces(0, interfaces, USB::MAX_INTERFACES);
  USB::InterfaceDescriptor* it = nullptr;
  for (size_t i = 0; i < interface_count; i++)
  {
    if (interfaces[i].bInterfaceNumber == device.interface_number &&
        interfaces[i].bAlternateSetting == alt_setting)
    {
      it = &interfaces[i];
      break;
    }
  }
  if (!it)
    return IPCReply(IPC_EINVAL);
  it->Swap();
  memory.CopyToEmu(request.buffer_out + 68, it, sizeof(*it));

  USB::EndpointDescriptor endpoints[USB::MAX_ENDPOINTS];
  const size_t endpoint_count = host_device->GetEndpoints(
      0, it->bInterfaceNumber, it->bAlternateSetting, endpoints, USB::MAX_ENDPOINTS);
  for (size_t e = 0; e < endpoint_count; e++)
  {
    USB::EndpointDescriptor& endpoint = endpoints[e];
    constexpr u8 ENDPOINT_INTERRUPT = 0b11;
    constexpr u8 ENDPOINT_IN = 0x80;
    if (endpoint.bmAttributes == ENDPOINT_INTERRUPT)
    {
      const bool is_in_endpoint = (endpoint.bEndpointAddress & ENDPOINT_IN) != 0;

      AdditionalDeviceData* data = &m_additional_device_data[&device - m_usbv5_devices];
      if (is_in_endpoint)
        data->interrupt_in_endpoint = endpoint.bEndpointAddress;
      else
        data->interrupt_out_endpoint = endpoint.bEndpointAddress;

      const u32 offset = is_in_endpoint ? 80 : 88;
      endpoint.Swap();
      memory.CopyToEmu(request.buffer_out + offset, &endpoint, sizeof(endpoint));
    }
  }

  return IPCReply(IPC_SUCCESS);
}

bool USB_HIDv5::ShouldAddDevice(const USB::Device& device) const
{
  // XXX: HIDv5 opens /dev/usb/usb with mode 3 (which is likely HID_CLASS),
  //      unlike VEN (which opens it with mode 0xff). But is this really correct?
  constexpr u8 HID_CLASS = 0x03;
  return device.HasClass(HID_CLASS);
}
}  // namespace IOS::HLE
