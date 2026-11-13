// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/Buffer.h"

#include "Core/IOS/USB/Bluetooth/hci.h"
#include "Core/LibusbUtils.h"

#include <queues/mpsc_stack.h>
#include <queues/retro_deque.h>
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <rthreads/retro_eventcount.h>

struct libusb_device_handle;
struct libusb_device_descriptor;
struct libusb_transfer;

// A sent HCI command yet to be acknowledged
struct BTOutstandingCommand
{
  u16 opcode;
  u64 submit_ticks;  // emulated time
};
RETRO_DEQUE_DEFINE(bt_outstanding_commands, BTOutstandingCommand);
RETRO_DEQUE_DEFINE(bt_pending_transfers, libusb_transfer*);

template <u16 Opcode, typename CommandType>
struct HCICommandPayload
{
  hci_cmd_hdr_t header{Opcode, sizeof(CommandType)};
  CommandType command{};
};

class LibUSBBluetoothAdapter
{
public:
  using BufferType = Common::UniqueBuffer<u8>;

  static bool IsConfiguredBluetoothDevice(u16 vid, u16 pid);
  static bool IsBluetoothDevice(const libusb_device_descriptor& descriptor);
  static bool HasConfiguredBluetoothDevice();

  // Public interface is intended to be used by a single thread.

  LibUSBBluetoothAdapter();
  ~LibUSBBluetoothAdapter();

  // Return a packet at the front of the queue, or an empty buffer when nothing is queued.
  // ReceiveHCIEvent is expected to be called regularly to do some internal processing.
  [[nodiscard]] BufferType ReceiveHCIEvent();

  // The ACL packet at the front of the queue, read in place; nullptr when nothing is queued.
  // PopACLData releases it.
  [[nodiscard]] const u8* PeekACLData(std::size_t* size);
  void PopACLData();

  // Submit a transfer now; libusb completes it asynchronously.
  void SendBulkTransfer(u8 endpoint, const u8* data, std::size_t size);
  void SendControlTransfer(u8 type, u8 request, u8 value, u8 index, const u8* data,
                           std::size_t size);
  void SendControlTransfer(const void* data, std::size_t size);

  // Blocks and eats events until a command complete event of the correct opcode is received.
  // Returns true on success or false on error or when the device stops responding.
  // The written response does not include the event header or command complete data.
  bool SendBlockingCommand(const void* data, std::size_t size, void* response,
                           std::size_t response_size);

  bool IsWiiBTModule() const;

private:
  // Bytes each input ring holds; inputs are dropped when it is full.
  static constexpr std::size_t INPUT_RING_SIZE = 64 * 1024;

  static constexpr u8 INTERFACE = 0x00;

  std::string m_last_open_error;

  // Notified when a transfer is freed or an input transfer completes. Callbacks on the libusb
  // event thread notify this until that thread is joined, which happens when m_context goes, so
  // it is declared first to be released last.
  struct AdapterEvent
  {
    AdapterEvent() { retro_eventcount_init(&ec); }
    ~AdapterEvent() { retro_eventcount_free(&ec); }
    retro_eventcount_t ec;
  } m_adapter_event;

  LibusbUtils::Context m_context;
  libusb_device_handle* m_handle = nullptr;

  retro_atomic_int_t m_run_input_transfers;
  // Set once an input transfer stops repeating, as when the device is gone.
  retro_atomic_int_t m_input_failed;

  // Callback and worker threads push transfers here, lock-free, when they are ready for clean up.
  // The node lives in the front of the transfer's own buffer, so this allocates nothing; the
  // same header links every live transfer for Close to cancel.
  struct FreeNode
  {
    mpsc_stack_node_t link;
    libusb_transfer* transfer;
    FreeNode* prev;
    FreeNode* next;
  };
  static constexpr std::size_t BUFFER_HEADER_SIZE = (sizeof(FreeNode) + 15) & ~std::size_t{15};
  void QueueFree(libusb_transfer* transfer);
  mpsc_stack_t m_transfers_to_free;

  // Incoming packets from the libusb callback thread, each a 16-bit length and the payload.
  retro_spsc_t m_hci_event_queue;
  retro_spsc_t m_acl_data_queue;
  std::size_t m_acl_peeked = 0;
  std::vector<u8> m_acl_bounce;

  // All transfers are alloc'd and linked here until complete.
  FreeNode* m_live_transfers = nullptr;

  // Set to reduce OSD and log spam.
  bool m_showed_failed_transfer = false;
  bool m_showed_long_queue_drop = false;

  u16 m_device_vid = 0;
  u16 m_device_pid = 0;

  // Bluetooth spec's Num_HCI_Command_Packets.
  // This is the number of hci commands that the host is allowed to send.
  // We track this to send commands only when the controller is ready.
  u8 m_num_hci_command_packets = 1;

  // HCI commands are queued when the controller isn't ready for them.
  // They will be sent after COMMAND_COMPL or COMMAND_STATUS signal readiness.
  bt_pending_transfers_t m_pending_hci_transfers;

  // Sent HCI commands that have yet to receive a COMMAND_COMPL or COMMAND_STATUS response.
  // Container size will be small, around 2.
  bt_outstanding_commands_t m_unacknowledged_commands;

  bool IsControllerReadyForCommand() const;
  bool AreCommandsPendingResponse() const;

  // Give the transfer to the worker and track the command appropriately.
  // This should only be used when IsControllerReadyForCommand is true.
  void PushHCICommand(libusb_transfer*);

  template <typename EventType>
  void AcknowledgeCommand(const u8* buffer, std::size_t size);

  libusb_transfer* AllocateTransfer(std::size_t buffer_size);
  void Close();
  void FreeTransfer(libusb_transfer*);
  void CleanCompletedTransfers();

  // Do some HCI logic and clean up completed transfers.
  void Update();

  // Immediately submit transfer with libusb.
  void SubmitTransfer(libusb_transfer*);

  // Start repetitive transfers to fill the hci-event and acl-data queues.
  void StartInputTransfers();

  // LibUSB callbacks. Invoked from the LibUSB context thread.
  void HandleOutputTransfer(libusb_transfer*);
  void HandleInputTransfer(libusb_transfer*);

  // Error logging.
  void HandleTransferError(libusb_transfer*);

  bool OpenDevice(const libusb_device_descriptor& device_descriptor, libusb_device* device);
};
