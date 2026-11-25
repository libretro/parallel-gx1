// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/USB_VEN/VEN.h"

#include <algorithm>
#include <cstddef>
#include <memory>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/USB/Common.h"
#include "Core/System.h"

namespace IOS::HLE
{
USB_VEN::~USB_VEN() = default;

IPCReply USB_VEN::IOCtl(const IOCtlRequest& request)
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
  case USB::IOCTL_USBV5_SETALTERNATE:
  {
    USBV5Device* const device = GetIOCtlDevice(request);
    return device ? SetAlternateSetting(*device, request) : IPCReply(IPC_EINVAL);
  }
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

IPCReply USB_VEN::IOCtlV(const IOCtlVRequest& request)
{
  switch (request.request)
  {
  case USB::IOCTLV_USBV5_CTRLMSG:
  case USB::IOCTLV_USBV5_INTRMSG:
  case USB::IOCTLV_USBV5_BULKMSG:
  case USB::IOCTLV_USBV5_ISOMSG:
  {
    // isochronous messages take four vectors, the others two
    const size_t num_vectors = request.request == USB::IOCTLV_USBV5_ISOMSG ? 4 : 2;
    if (request.in_vectors.size() + request.io_vectors.size() != num_vectors)
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
    return TransferReply(*host_device, request.request, SubmitTransfer(*host_device, request));
  }
  default:
    return IPCReply(IPC_EINVAL);
  }
}

s32 USB_VEN::SubmitTransfer(USB::Device& device, const IOCtlVRequest& ioctlv)
{
  switch (ioctlv.request)
  {
  case USB::IOCTLV_USBV5_CTRLMSG:
    return device.SubmitTransfer(new USB::V5CtrlMessage(GetEmulationKernel(), ioctlv));
  case USB::IOCTLV_USBV5_INTRMSG:
    return device.SubmitTransfer(new USB::V5IntrMessage(GetEmulationKernel(), ioctlv));
  case USB::IOCTLV_USBV5_BULKMSG:
    return device.SubmitTransfer(new USB::V5BulkMessage(GetEmulationKernel(), ioctlv));
  case USB::IOCTLV_USBV5_ISOMSG:
    return device.SubmitTransfer(new USB::V5IsoMessage(GetEmulationKernel(), ioctlv));
  default:
    return IPC_EINVAL;
  }
}

IPCReply USB_VEN::CancelEndpoint(USBV5Device& device, const IOCtlRequest& request)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u8 endpoint = memory.Read_U8(request.buffer_in + 8);
  // IPC_EINVAL (-4) is returned when no transfer was cancelled.
  if (GetDeviceById(device.host_id)->CancelTransfer(endpoint) < 0)
    return IPCReply(IPC_EINVAL);
  return IPCReply(IPC_SUCCESS);
}

IPCReply USB_VEN::GetDeviceInfo(USBV5Device& device, const IOCtlRequest& request)
{
  if (request.buffer_out == 0 || request.buffer_out_size != 0xc0)
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
  memory.CopyToEmu(request.buffer_out + 20, &device_descriptor, sizeof(device_descriptor));

  // VEN only cares about the first configuration.
  USB::ConfigDescriptor config_descriptor;
  if (host_device->GetConfigurations(&config_descriptor, 1) == 0)
    return IPCReply(IPC_EINVAL);
  config_descriptor.Swap();
  memory.CopyToEmu(request.buffer_out + 40, &config_descriptor, sizeof(config_descriptor));

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
  memory.CopyToEmu(request.buffer_out + 52, it, sizeof(*it));

  USB::EndpointDescriptor endpoints[USB::MAX_ENDPOINTS];
  const size_t endpoint_count = host_device->GetEndpoints(
      0, it->bInterfaceNumber, it->bAlternateSetting, endpoints, USB::MAX_ENDPOINTS);
  for (size_t i = 0; i < endpoint_count; ++i)
  {
    endpoints[i].Swap();
    memory.CopyToEmu(request.buffer_out + 64 + 8 * static_cast<u8>(i), &endpoints[i],
                     sizeof(endpoints[i]));
  }

  return IPCReply(IPC_SUCCESS);
}
}  // namespace IOS::HLE
