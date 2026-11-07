// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

#include "Common/WorkQueueThread.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/DVD/FileMonitor.h"

#include "DiscIO/Volume.h"

#include <queues/mpsc_stack.h>
#include <rthreads/retro_eventcount.h>

class PointerWrap;
namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
}
namespace DiscIO
{
struct Partition;
}

namespace DiscIO
{
enum class Platform;
class Volume;
}  // namespace DiscIO

namespace IOS::ES
{
class TMDReader;
class TicketReader;
}  // namespace IOS::ES

namespace DVD
{
enum class ReplyType : u32;

class DVDThread
{
public:
  explicit DVDThread(Core::System& system);
  DVDThread(const DVDThread&) = delete;
  DVDThread(DVDThread&&) = delete;
  DVDThread& operator=(const DVDThread&) = delete;
  DVDThread& operator=(DVDThread&&) = delete;
  ~DVDThread();

  void Start();
  void Stop();
  void DoState(PointerWrap& p);

  void SetDisc(std::unique_ptr<DiscIO::Volume> disc);
  bool HasDisc() const;

  bool HasWiiHashes() const;
  DiscIO::Platform GetDiscType() const;
  u64 PartitionOffsetToRawOffset(u64 offset, const DiscIO::Partition& partition);
  IOS::ES::TMDReader GetTMD(const DiscIO::Partition& partition);
  IOS::ES::TicketReader GetTicket(const DiscIO::Partition& partition);
  bool IsInsertedDiscRunning();
  // This function returns true and calls SConfig::SetRunningGameMetadata(Volume&, Partition&)
  // if both of the following conditions are true:
  // - A disc is inserted
  // - The title_id argument doesn't contain a value, or its value matches the disc's title ID
  bool UpdateRunningGameMetadata(const DiscIO::Partition& partition,
                                 std::optional<u64> title_id = {});

  void StartRead(u64 dvd_offset, u32 length, const DiscIO::Partition& partition,
                 DVD::ReplyType reply_type, s64 ticks_until_completion);
  void StartReadToEmulatedRAM(u32 output_address, u64 dvd_offset, u32 length,
                              const DiscIO::Partition& partition, DVD::ReplyType reply_type,
                              s64 ticks_until_completion);

private:
  void WaitUntilIdle();

  void StartReadInternal(bool copy_to_ram, u32 output_address, u64 dvd_offset, u32 length,
                         const DiscIO::Partition& partition, DVD::ReplyType reply_type,
                         s64 ticks_until_completion);

  static void GlobalFinishRead(Core::System& system, u64 id, s64 cycles_late);
  void FinishRead(u64 id, s64 cycles_late);

  struct ReadRequest
  {
    bool copy_to_ram = false;
    u32 output_address = 0;
    u64 dvd_offset = 0;
    u32 length = 0;
    DiscIO::Partition partition{};

    // This determines which code DVDInterface will run to reply
    // to the emulated software. We can't use callbacks,
    // because function pointers can't be stored in savestates.
    DVD::ReplyType reply_type = DVD::ReplyType::NoReply;

    // IDs are used to uniquely identify a request. They must not be
    // identical to IDs of any other requests that currently exist, but
    // it's fine to re-use IDs of requests that have existed in the past.
    u64 id = 0;

    // Only used for logging
    u64 time_started_ticks = 0;
  };

  void ProcessReadRequest(ReadRequest&& read_request);

  using ReadResult = std::pair<ReadRequest, std::vector<u8>>;

  CoreTiming::EventType* m_finish_read = nullptr;

  u64 m_next_id = 0;

  Common::WorkQueueThreadSP<ReadRequest> m_dvd_thread;

  // Results travel from the DVD thread to the CPU thread through a lock-free stack; the CPU thread
  // takes them all at once into m_result_chain, oldest first, and wakes on m_result_ready.
  struct ResultNode
  {
    mpsc_stack_node_t link;
    ReadResult result;
  };
  ResultNode* TakeResult(u64 id);
  void FreeResults();

  // Results go back for the DVD thread to fill again, buffers and all.
  ResultNode* TakeFreeResult();
  mpsc_stack_t m_free_results;
  mpsc_stack_node_t* m_worker_free = nullptr;  // DVD thread only
  mpsc_stack_t m_result_queue;
  retro_eventcount_t m_result_ready;
  mpsc_stack_node_t* m_result_chain = nullptr;  // CPU thread only
  std::map<u64, ReadResult> m_result_map;

  // Reads the disc image can lend: no DVD thread and no buffer, the bytes copied straight
  // from the image when they are due. Their pages are prefetched when the read starts.
  void MaterializeLentReads();
  std::vector<ReadRequest> m_lent_reads;  // CPU thread only

  std::unique_ptr<DiscIO::Volume> m_disc;

  FileMonitor::FileLogger m_file_logger;

  Core::System& m_system;
};
}  // namespace DVD
