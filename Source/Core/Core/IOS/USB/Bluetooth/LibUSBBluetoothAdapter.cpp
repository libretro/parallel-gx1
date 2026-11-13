// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/Bluetooth/LibUSBBluetoothAdapter.h"

#include <cassert>

#include <cstdlib>
#include <cstring>
#include <utility>

#include <libusb.h>

#include "Common/BitUtils.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"
#include "Core/IOS/USB/Bluetooth/RealtekFirmwareLoader.h"
#include "Core/IOS/USB/Bluetooth/hci.h"

namespace
{
constexpr std::size_t BUFFER_SIZE = 1024;

// An HCI command not acknowledged within this much emulated time is given up on.
constexpr u64 HCI_COMMAND_TIMEOUT_MS = 1000;

u64 EmulatedTicks()
{
  return Core::System::GetInstance().GetCoreTiming().GetTicks();
}

constexpr u8 REQUEST_TYPE =
    u8(u8(LIBUSB_ENDPOINT_OUT) | u8(LIBUSB_REQUEST_TYPE_CLASS)) | u8(LIBUSB_RECIPIENT_DEVICE);

constexpr u8 HCI_EVENT = 0x81;
constexpr u8 ACL_DATA_IN = 0x82;

template <auto MemFun>
constexpr libusb_transfer_cb_fn LibUSBMemFunCallback()
{
  return [](libusb_transfer* tr) {
    std::invoke(MemFun, static_cast<Common::ObjectType<MemFun>*>(tr->user_data), tr);
  };
}

}  // namespace

bool LibUSBBluetoothAdapter::IsBluetoothDevice(const libusb_device_descriptor& descriptor)
{
  constexpr u8 SUBCLASS = 0x01;
  constexpr u8 PROTOCOL_BLUETOOTH = 0x01;

  const bool is_bluetooth_protocol = descriptor.bDeviceClass == LIBUSB_CLASS_WIRELESS &&
                                     descriptor.bDeviceSubClass == SUBCLASS &&
                                     descriptor.bDeviceProtocol == PROTOCOL_BLUETOOTH;

  // Some devices misreport their class, so we avoid relying solely on descriptor checks and allow
  // users to specify their own VID/PID.
  return is_bluetooth_protocol ||
         LibUSBBluetoothAdapter::IsConfiguredBluetoothDevice(descriptor.idVendor,
                                                             descriptor.idProduct) ||
         IsKnownRealtekBluetoothDevice(descriptor.idVendor, descriptor.idProduct);
}

bool LibUSBBluetoothAdapter::IsWiiBTModule() const
{
  return m_device_vid == 0x57e && m_device_pid == 0x305;
}

bool LibUSBBluetoothAdapter::AreCommandsPendingResponse() const
{
  return !bt_pending_transfers_empty(&m_pending_hci_transfers) ||
         !bt_outstanding_commands_empty(&m_unacknowledged_commands);
}

bool LibUSBBluetoothAdapter::HasConfiguredBluetoothDevice()
{
  const int configured_vid = Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_VID);
  const int configured_pid = Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_PID);
  return configured_vid != -1 && configured_pid != -1;
}

bool LibUSBBluetoothAdapter::IsConfiguredBluetoothDevice(u16 vid, u16 pid)
{
  const int configured_vid = Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_VID);
  const int configured_pid = Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_PID);
  return configured_vid == vid && configured_pid == pid;
}

LibUSBBluetoothAdapter::LibUSBBluetoothAdapter()
{
  bt_pending_transfers_init(&m_pending_hci_transfers, 16);
  bt_outstanding_commands_init(&m_unacknowledged_commands, 16);
  retro_atomic_int_init(&m_run_input_transfers, 1);
  retro_atomic_int_init(&m_input_failed, 0);
  mpsc_stack_init(&m_transfers_to_free);
  retro_spsc_init_mirrored(&m_hci_event_queue, INPUT_RING_SIZE);
  retro_spsc_init_mirrored(&m_acl_data_queue, INPUT_RING_SIZE);

  if (!m_context.IsValid())
    return;

  m_last_open_error.clear();

  const bool has_configured_bt = HasConfiguredBluetoothDevice();

  const int ret = m_context.GetDeviceList([&](libusb_device* device) {
    libusb_device_descriptor device_descriptor;
    libusb_get_device_descriptor(device, &device_descriptor);
    auto [make_config_descriptor_ret, config_descriptor] =
        LibusbUtils::MakeConfigDescriptor(device);
    if (make_config_descriptor_ret != LIBUSB_SUCCESS || !config_descriptor)
    {
      ERROR_LOG(IOS_WIIMOTE, "Failed to get config descriptor for device %04x:%04x: %s (%d: %s)",
                (unsigned)device_descriptor.idVendor, (unsigned)device_descriptor.idProduct,
                LibusbUtils::ErrorWrap(make_config_descriptor_ret).GetStrError(),
                LibusbUtils::ErrorWrap(make_config_descriptor_ret).m_error,
                LibusbUtils::ErrorWrap(make_config_descriptor_ret).GetName());
      return true;
    }

    if (has_configured_bt &&
        !IsConfiguredBluetoothDevice(device_descriptor.idVendor, device_descriptor.idProduct))
    {
      return true;
    }

    if (IsBluetoothDevice(device_descriptor) && OpenDevice(device_descriptor, device))
    {
      const auto manufacturer =
          LibusbUtils::GetStringDescriptor(m_handle, device_descriptor.iManufacturer).value_or("?");
      const auto product =
          LibusbUtils::GetStringDescriptor(m_handle, device_descriptor.iProduct).value_or("?");
      const auto serial_number =
          LibusbUtils::GetStringDescriptor(m_handle, device_descriptor.iSerialNumber).value_or("?");
      NOTICE_LOG(IOS_WIIMOTE, "Using device %04x:%04x (rev %x) for Bluetooth: %s %s %s",
                 (unsigned)device_descriptor.idVendor, (unsigned)device_descriptor.idProduct,
                 (unsigned)device_descriptor.bcdDevice, manufacturer.c_str(), product.c_str(),
                 serial_number.c_str());

      m_device_vid = device_descriptor.idVendor;
      m_device_pid = device_descriptor.idProduct;
      return false;
    }
    return true;
  });
  if (ret != LIBUSB_SUCCESS)
  {
    const LibusbUtils::ErrorWrap error(ret);
    m_last_open_error = StringFromFormat("GetDeviceList failed: %s (%d: %s)", error.GetStrError(),
                                         error.m_error, error.GetName());
  }

  if (m_handle == nullptr)
  {
    if (m_last_open_error.empty())
    {
      CriticalAlert("Could not find any usable Bluetooth USB adapter for Bluetooth Passthrough.\n\n"
                    "The emulated console will now stop.");
    }
    else
    {
      CriticalAlert("Could not find any usable Bluetooth USB adapter for Bluetooth Passthrough.\n"
                    "The following error occurred when Dolphin tried to use an adapter:\n%s\n\n"
                    "The emulated console will now stop.",
                    m_last_open_error.c_str());
    }
    Core::QueueHostJob(&Core::Stop);
    return;
  }

  StartInputTransfers();

  if (IsRealtekVID(m_device_vid) || IsKnownRealtekBluetoothDevice(m_device_vid, m_device_pid))
  {
    INFO_LOG(IOS_WIIMOTE, "Initializing Realtek Bluetooth device: %04x:%04x",
             (unsigned)m_device_vid, (unsigned)m_device_pid);
    if (!InitializeRealtekBluetoothDevice(*this))
    {
      PanicAlert("Failed to initialize Realtek Bluetooth device.\n\n"
                 "Bluetooth passthrough will probably not work.");
    }
  }
}

LibUSBBluetoothAdapter::~LibUSBBluetoothAdapter()
{
  if (m_handle != nullptr)
    Close();

  retro_spsc_free(&m_acl_data_queue);
  retro_spsc_free(&m_hci_event_queue);
  bt_outstanding_commands_free(&m_unacknowledged_commands);
  bt_pending_transfers_free(&m_pending_hci_transfers);
}

void LibUSBBluetoothAdapter::Close()
{
  // Commands still awaiting a reply are abandoned with their transfers below.
  bt_pending_transfers_clear(&m_pending_hci_transfers);
  bt_outstanding_commands_clear(&m_unacknowledged_commands);

  // Stop the repeating input transfers. A callback resubmitting meanwhile sees the flag after
  // its submit and cancels its own transfer (see HandleInputTransfer).
  retro_atomic_exchange_int(&m_run_input_transfers, 0);
  retro_atomic_thread_fence_seq_cst();

  // Cancel all LibUSB transfers, wait for their callbacks and clean up all the buffers. A
  // callback that read the run flag before it was cleared may resubmit its transfer after the
  // cancel, so whatever is still in flight is cancelled again on every pass.
  while (m_live_transfers)
  {
    for (FreeNode* node = m_live_transfers; node; node = node->next)
      libusb_cancel_transfer(node->transfer);
    const int key = retro_eventcount_prepare_wait(&m_adapter_event.ec);
    if (!mpsc_stack_empty(&m_transfers_to_free))
      retro_eventcount_cancel_wait(&m_adapter_event.ec);
    else
      retro_eventcount_commit_wait(&m_adapter_event.ec, key);
    CleanCompletedTransfers();
  }

  const int ret = libusb_release_interface(m_handle, 0);
  if (ret != LIBUSB_SUCCESS)
    WARN_LOG(IOS_WIIMOTE, "libusb_release_interface failed: %s (%d: %s)",
             LibusbUtils::ErrorWrap(ret).GetStrError(), LibusbUtils::ErrorWrap(ret).m_error,
             LibusbUtils::ErrorWrap(ret).GetName());

  libusb_close(std::exchange(m_handle, nullptr));
}

void LibUSBBluetoothAdapter::Update()
{
  // Remove timed out commands. Emulated time stands still outside emulation, where the
  // blocking callers wait for the reply itself.
  const u64 timeout_ticks = Core::System::GetInstance().GetSystemTimers().GetTicksPerSecond() *
                            HCI_COMMAND_TIMEOUT_MS / 1000;
  const u64 now = EmulatedTicks();
  while (!bt_outstanding_commands_empty(&m_unacknowledged_commands) &&
         now - bt_outstanding_commands_front(&m_unacknowledged_commands)->submit_ticks >
             timeout_ticks)
  {
    WARN_LOG(IOS_WIIMOTE, "HCI command 0x%04x timed out.",
             (unsigned)bt_outstanding_commands_front(&m_unacknowledged_commands)->opcode);
    bt_outstanding_commands_pop_front(&m_unacknowledged_commands);
  }

  // Allow sending commands if none are pending acknowledgement.
  if (bt_outstanding_commands_empty(&m_unacknowledged_commands))
    m_num_hci_command_packets = 1;

  // Push queued commands when the controller is ready for them.
  if (!bt_pending_transfers_empty(&m_pending_hci_transfers) && IsControllerReadyForCommand())
  {
    libusb_transfer* const transfer = *bt_pending_transfers_front(&m_pending_hci_transfers);
    DEBUG_LOG(IOS_WIIMOTE, "Submitting queued HCI command.");

    bt_pending_transfers_pop_front(&m_pending_hci_transfers);
    PushHCICommand(transfer);
  }

  CleanCompletedTransfers();
}

// Consumer side of an input ring: the length of the whole packet at its front, or 0.
static u16 PeekPacketLength(const retro_spsc_t* queue)
{
  const std::size_t avail = retro_spsc_read_avail(queue);
  u16 length;
  if (avail < sizeof(length))
    return 0;
  retro_spsc_peek(queue, &length, sizeof(length));
  return avail >= sizeof(length) + length ? length : 0;
}

auto LibUSBBluetoothAdapter::ReceiveHCIEvent() -> BufferType
{
  const u16 length = PeekPacketLength(&m_hci_event_queue);
  if (length == 0)
  {
    Update();
    return {};
  }

  BufferType buffer(length);
  retro_spsc_skip(&m_hci_event_queue, sizeof(length));
  retro_spsc_read(&m_hci_event_queue, buffer.data(), length);

  // We only care about "COMMAND_COMPL" and "COMMAND_STATUS" events here.
  // The controller will reply with one of those for every command.
  // We track which commands are still "in flight" to properly obey Num_HCI_Command_Packets.
  const auto event = buffer[0];
  if (event == HCI_EVENT_COMMAND_COMPL)
  {
    AcknowledgeCommand<hci_command_compl_ep>(buffer.data(), buffer.size());
  }
  else if (event == HCI_EVENT_COMMAND_STATUS)
  {
    AcknowledgeCommand<hci_command_status_ep>(buffer.data(), buffer.size());
  }

  Update();
  return buffer;
}

const u8* LibUSBBluetoothAdapter::PeekACLData(std::size_t* size)
{
  const u16 length = PeekPacketLength(&m_acl_data_queue);
  *size = length;
  if (length == 0)
    return nullptr;

  m_acl_peeked = sizeof(length) + length;
  const void* ring;
  if (retro_spsc_read_begin(&m_acl_data_queue, &ring) >= m_acl_peeked)
    return static_cast<const u8*>(ring) + sizeof(length);

  // Only a ring without a mirror splits a packet at its end.
  m_acl_bounce.resize(m_acl_peeked);
  retro_spsc_peek(&m_acl_data_queue, m_acl_bounce.data(), m_acl_peeked);
  return m_acl_bounce.data() + sizeof(length);
}

void LibUSBBluetoothAdapter::PopACLData()
{
  retro_spsc_skip(&m_acl_data_queue, std::exchange(m_acl_peeked, 0));
}

bool LibUSBBluetoothAdapter::IsControllerReadyForCommand() const
{
  return m_num_hci_command_packets > bt_outstanding_commands_count(&m_unacknowledged_commands);
}

template <typename EventType>
void LibUSBBluetoothAdapter::AcknowledgeCommand(const u8* buffer, std::size_t size)
{
  if (size < sizeof(hci_event_hdr_t) + sizeof(EventType)) [[unlikely]]
  {
    WARN_LOG(IOS_WIIMOTE, "Undersized HCI event");
    return;
  }

  EventType ev;
  std::memcpy(&ev, buffer + sizeof(hci_event_hdr_t), sizeof(ev));

  const std::size_t count = bt_outstanding_commands_count(&m_unacknowledged_commands);
  std::size_t index = 0;
  while (index < count &&
         bt_outstanding_commands_at(&m_unacknowledged_commands, index)->opcode != ev.opcode)
  {
    index++;
  }
  if (index < count)
  {
    DEBUG_LOG(IOS_WIIMOTE, "HCI command acknowledged: 0x%04x", (unsigned)ev.opcode);
    bt_outstanding_commands_erase(&m_unacknowledged_commands, index);
  }
  else if (ev.opcode != 0x0000)
  {
    WARN_LOG(IOS_WIIMOTE, "Unexpected opcode acknowledgement: 0x%04x", (unsigned)ev.opcode);
  }

  m_num_hci_command_packets = ev.num_cmd_pkts;
}

libusb_transfer* LibUSBBluetoothAdapter::AllocateTransfer(std::size_t buffer_size)
{
  // One block: the free-list node and live-list links, then the transfer's bytes.
  u8* const block = static_cast<u8*>(std::malloc(BUFFER_HEADER_SIZE + buffer_size));
  libusb_transfer* const transfer = block ? libusb_alloc_transfer(0) : nullptr;
  if (!transfer)
  {
    std::free(block);
    return nullptr;
  }

  FreeNode* const node = reinterpret_cast<FreeNode*>(block);
  node->transfer = transfer;
  node->prev = nullptr;
  node->next = m_live_transfers;
  if (m_live_transfers)
    m_live_transfers->prev = node;
  m_live_transfers = node;

  transfer->buffer = block + BUFFER_HEADER_SIZE;
  return transfer;
}

void LibUSBBluetoothAdapter::FreeTransfer(libusb_transfer* transfer)
{
  FreeNode* const node = reinterpret_cast<FreeNode*>(transfer->buffer - BUFFER_HEADER_SIZE);
  if (node->prev)
    node->prev->next = node->next;
  else
    m_live_transfers = node->next;
  if (node->next)
    node->next->prev = node->prev;
  libusb_free_transfer(transfer);
  std::free(node);
}

// Any thread.
void LibUSBBluetoothAdapter::QueueFree(libusb_transfer* transfer)
{
  // node->transfer was set at allocation; only the free-list link is written here.
  FreeNode* const node = reinterpret_cast<FreeNode*>(transfer->buffer - BUFFER_HEADER_SIZE);
  mpsc_stack_push(&m_transfers_to_free, &node->link);
  retro_eventcount_notify(&m_adapter_event.ec);
}

void LibUSBBluetoothAdapter::CleanCompletedTransfers()
{
  mpsc_stack_node_t* node = mpsc_stack_drain(&m_transfers_to_free);
  while (node)
  {
    // The node is freed with its transfer's buffer.
    mpsc_stack_node_t* const next = node->next;
    FreeTransfer(reinterpret_cast<FreeNode*>(node)->transfer);
    node = next;
  }
}

void LibUSBBluetoothAdapter::PushHCICommand(libusb_transfer* transfer)
{
  hci_cmd_hdr_t cmd;
  std::memcpy(&cmd, libusb_control_transfer_get_data(transfer), sizeof(cmd));

  // Timed from now, not from when it was queued, so a delayed command gets its full time.
  const BTOutstandingCommand outstanding = {cmd.opcode, EmulatedTicks()};
  bt_outstanding_commands_push_back_growing(&m_unacknowledged_commands, &outstanding);

  // This function is only invoked when this value is at least 1.
  assert(m_num_hci_command_packets >= 1);
  --m_num_hci_command_packets;

  SubmitTransfer(transfer);
}

void LibUSBBluetoothAdapter::SubmitTransfer(libusb_transfer* transfer)
{
  const int ret = libusb_submit_transfer(transfer);

  if (ret == LIBUSB_SUCCESS)
    return;

  ERROR_LOG(IOS_WIIMOTE, "libusb_submit_transfer: %s (%d: %s)",
            LibusbUtils::ErrorWrap(ret).GetStrError(), LibusbUtils::ErrorWrap(ret).m_error,
            LibusbUtils::ErrorWrap(ret).GetName());

  // Failed transers will not invoke callbacks so we must mark the buffer to be free'd here.
  QueueFree(transfer);
}

void LibUSBBluetoothAdapter::SendBulkTransfer(u8 endpoint, const u8* data, std::size_t size)
{
  constexpr auto callback = LibUSBMemFunCallback<&LibUSBBluetoothAdapter::HandleOutputTransfer>();

  auto* const transfer = AllocateTransfer(size);
  if (!transfer)
    return;
  libusb_fill_bulk_transfer(transfer, m_handle, endpoint, transfer->buffer, int(size), callback,
                            this, 0);

  std::memcpy(transfer->buffer, data, size);

  SubmitTransfer(transfer);
}

void LibUSBBluetoothAdapter::SendControlTransfer(u8 type, u8 request, u8 value, u8 index,
                                                 const u8* data, std::size_t size)
{
  constexpr auto callback = LibUSBMemFunCallback<&LibUSBBluetoothAdapter::HandleOutputTransfer>();

  auto* const transfer = AllocateTransfer(size + LIBUSB_CONTROL_SETUP_SIZE);
  if (!transfer)
    return;

  libusb_fill_control_setup(transfer->buffer, type, request, value, index, u16(size));
  libusb_fill_control_transfer(transfer, m_handle, transfer->buffer, callback, this, 0);

  std::memcpy(transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE, data, size);

  if (IsControllerReadyForCommand())
  {
    PushHCICommand(transfer);
  }
  else
  {
    DEBUG_LOG(IOS_WIIMOTE, "Queueing HCI command while controller is busy.");
    bt_pending_transfers_push_back_growing(&m_pending_hci_transfers, &transfer);
  }
}

void LibUSBBluetoothAdapter::SendControlTransfer(const void* data, std::size_t size)
{
  SendControlTransfer(REQUEST_TYPE, 0, 0, 0, static_cast<const u8*>(data), size);
}

bool LibUSBBluetoothAdapter::SendBlockingCommand(const void* data, std::size_t size,
                                                 void* response, std::size_t response_size)
{
  SendControlTransfer(data, size);

  hci_cmd_hdr_t cmd;
  std::memcpy(&cmd, data, sizeof(cmd));

  while (AreCommandsPendingResponse())
  {
    // Waits for the input callback to queue an event or to see the device fail.
    const int key = retro_eventcount_prepare_wait(&m_adapter_event.ec);
    if (PeekPacketLength(&m_hci_event_queue) == 0 &&
        !retro_atomic_load_acquire_int(&m_input_failed))
    {
      retro_eventcount_commit_wait(&m_adapter_event.ec, key);
      continue;
    }
    retro_eventcount_cancel_wait(&m_adapter_event.ec);

    const auto event = ReceiveHCIEvent();

    if (event.empty())
    {
      if (retro_atomic_load_acquire_int(&m_input_failed))
      {
        ERROR_LOG(IOS_WIIMOTE, "Bluetooth adapter stopped answering HCI commands.");
        return false;
      }
      continue;
    }

    if (event[0] != HCI_EVENT_COMMAND_COMPL)
      continue;

    if (event.size() < sizeof(hci_event_hdr_t) + sizeof(hci_command_compl_ep))
    {
      ERROR_LOG(IOS_WIIMOTE, "Undersized hci_command_compl_ep");
      continue;
    }

    const hci_command_compl_ep compl_event =
        Common::BitCastPtr<hci_command_compl_ep>(event.data() + sizeof(hci_event_hdr_t));
    if (compl_event.opcode != cmd.opcode)
      continue;

    if (event.size() < sizeof(hci_event_hdr_t) + sizeof(hci_command_compl_ep) + response_size)
    {
      ERROR_LOG(IOS_WIIMOTE, "Undersized command result");
      break;
    }

    std::memcpy(response, event.data() + sizeof(hci_event_hdr_t) + sizeof(hci_command_compl_ep),
                response_size);
    return true;
  }

  return false;
}

void LibUSBBluetoothAdapter::StartInputTransfers()
{
  constexpr auto callback = LibUSBMemFunCallback<&LibUSBBluetoothAdapter::HandleInputTransfer>();

  // Incoming HCI events.
  {
    auto* const transfer = AllocateTransfer(BUFFER_SIZE);
    libusb_fill_interrupt_transfer(transfer, m_handle, HCI_EVENT, transfer->buffer, BUFFER_SIZE,
                                   callback, this, 0);
    SubmitTransfer(transfer);
  }

  // Incoming ACL data.
  {
    auto* const transfer = AllocateTransfer(BUFFER_SIZE);
    libusb_fill_bulk_transfer(transfer, m_handle, ACL_DATA_IN, transfer->buffer, BUFFER_SIZE,
                              callback, this, 0);
    SubmitTransfer(transfer);
  }
}

bool LibUSBBluetoothAdapter::OpenDevice(const libusb_device_descriptor& device_descriptor,
                                        libusb_device* device)
{
  const int ret = libusb_open(device, &m_handle);
  if (ret != LIBUSB_SUCCESS)
  {
    const LibusbUtils::ErrorWrap error(ret);
    m_last_open_error = StringFromFormat("Failed to open Bluetooth device %04x:%04x: %s (%d: %s)",
                                         device_descriptor.idVendor, device_descriptor.idProduct,
                                         error.GetStrError(), error.m_error, error.GetName());
    return false;
  }

// Detaching always fails as a regular user on FreeBSD
// https://lists.freebsd.org/pipermail/freebsd-usb/2016-March/014161.html
#ifndef __FreeBSD__
  int result = libusb_set_auto_detach_kernel_driver(m_handle, 1);
  if (result != LIBUSB_SUCCESS)
  {
    result = libusb_detach_kernel_driver(m_handle, INTERFACE);
    if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_NOT_FOUND &&
        result != LIBUSB_ERROR_NOT_SUPPORTED)
    {
      const LibusbUtils::ErrorWrap error(result);
      m_last_open_error =
          StringFromFormat("Failed to detach kernel driver for BT passthrough: %s (%d: %s)",
                           error.GetStrError(), error.m_error, error.GetName());
      return false;
    }
  }
#endif
  if (const int result2 = libusb_claim_interface(m_handle, INTERFACE); result2 != LIBUSB_SUCCESS)
  {
    const LibusbUtils::ErrorWrap error(result2);
    m_last_open_error =
        StringFromFormat("Failed to claim interface for BT passthrough: %s (%d: %s)",
                         error.GetStrError(), error.m_error, error.GetName());
    return false;
  }

  return true;
}

void LibUSBBluetoothAdapter::HandleOutputTransfer(libusb_transfer* tr)
{
  HandleTransferError(tr);
  QueueFree(tr);
}

void LibUSBBluetoothAdapter::HandleInputTransfer(libusb_transfer* tr)
{
  if (tr->status == LIBUSB_TRANSFER_COMPLETED)
  {
    const bool is_hci_event = tr->endpoint == HCI_EVENT;
    retro_spsc_t* const queue = is_hci_event ? &m_hci_event_queue : &m_acl_data_queue;
    const u16 length = static_cast<u16>(tr->actual_length);

    if (retro_spsc_write_avail(queue) >= sizeof(length) + length)
    {
      if (length != 0)
      {
        // The consumer only takes a packet once all of it is there.
        retro_spsc_write(queue, &length, sizeof(length));
        retro_spsc_write(queue, tr->buffer, length);
      }

      m_showed_long_queue_drop = false;
    }
    else if (!std::exchange(m_showed_long_queue_drop, true))
    {
      // This will happen when pausing the emulator.
      WARN_LOG(IOS_WIIMOTE, "%s queue is too long. Packets will be dropped.",
               (is_hci_event ? "HCI" : "ACL"));
    }
  }

  HandleTransferError(tr);

  if (retro_atomic_load_acquire_int(&m_run_input_transfers))
  {
    // Continuously repeat this input transfer so long as we're still running.
    const auto ret = libusb_submit_transfer(tr);
    if (ret == LIBUSB_SUCCESS)
    {
      // Close cleared the flag before cancelling; if that was after the check above, this
      // submit may have missed its cancel, so it cancels itself.
      retro_atomic_thread_fence_seq_cst();
      if (!retro_atomic_load_acquire_int(&m_run_input_transfers))
        libusb_cancel_transfer(tr);
      retro_eventcount_notify(&m_adapter_event.ec);
      return;
    }

    ERROR_LOG(IOS_WIIMOTE, "HandleInputTransfer libusb_submit_transfer: %s (%d: %s)",
              LibusbUtils::ErrorWrap(ret).GetStrError(), LibusbUtils::ErrorWrap(ret).m_error,
              LibusbUtils::ErrorWrap(ret).GetName());
  }

  // No longer listening: nothing more will arrive for a blocking command to wait on.
  retro_atomic_store_release_int(&m_input_failed, 1);
  QueueFree(tr);
}

void LibUSBBluetoothAdapter::HandleTransferError(libusb_transfer* transfer)
{
  if (transfer->status != LIBUSB_TRANSFER_COMPLETED &&
      transfer->status != LIBUSB_TRANSFER_CANCELLED)
  {
    ERROR_LOG(IOS_WIIMOTE, "libusb transfer failed: %s (%d: %s)",
              LibusbUtils::ErrorWrap(transfer->status).GetStrError(),
              LibusbUtils::ErrorWrap(transfer->status).m_error,
              LibusbUtils::ErrorWrap(transfer->status).GetName());
    if (!std::exchange(m_showed_failed_transfer, true))
    {
      Core::DisplayMessage("Failed to send a command to the Bluetooth adapter.");
      Core::DisplayMessage("It may not be compatible with passthrough mode.");
    }
  }
  else
  {
    m_showed_failed_transfer = false;
  }
}
