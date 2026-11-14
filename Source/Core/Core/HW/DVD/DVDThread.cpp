// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/DVD/DVDThread.h"

#include <cinttypes>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Timer.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/DVD/FileMonitor.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/System.h"

#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"

namespace DVD
{
DVDThread::DVDThread(Core::System& system) : m_system(system)
{
  mpsc_stack_init(&m_result_queue);
  mpsc_stack_init(&m_free_results);
  retro_eventcount_init(&m_result_ready);
}

DVDThread::~DVDThread()
{
  m_dvd_thread.Shutdown();
  FreeResults();
  retro_eventcount_free(&m_result_ready);
  DiscIO::DestroyVolume(m_disc);
}

void DVDThread::FreeResultList(mpsc_stack_node_t* node)
{
  while (node)
  {
    mpsc_stack_node_t* const next = node->next;
    std::free(reinterpret_cast<ResultNode*>(node)->data);
    std::free(node);
    node = next;
  }
}

void DVDThread::FreeResults()
{
  FreeResultList(m_result_chain);
  m_result_chain = nullptr;
  FreeResultList(mpsc_stack_drain(&m_result_queue));
  FreeResultList(mpsc_stack_drain(&m_free_results));
  FreeResultList(m_worker_free);
  m_worker_free = nullptr;
}

// CPU thread. Unlinks and returns the result with this id, or every result taken so far when
// id is ~0, without waiting; nullptr when none has arrived.
DVDThread::ResultNode* DVDThread::TakeResult(u64 id)
{
  if (!mpsc_stack_empty(&m_result_queue))
  {
    mpsc_stack_node_t* const arrived = mpsc_stack_reverse(mpsc_stack_drain(&m_result_queue));
    mpsc_stack_node_t** tail = &m_result_chain;
    while (*tail)
      tail = &(*tail)->next;
    *tail = arrived;
  }

  for (mpsc_stack_node_t** link = &m_result_chain; *link; link = &(*link)->next)
  {
    ResultNode* const node = reinterpret_cast<ResultNode*>(*link);
    if (id == ~u64{0} || node->request.id == id)
    {
      *link = node->link.next;
      return node;
    }
  }
  return nullptr;
}

void DVDThread::Start()
{
  m_finish_read = m_system.GetCoreTiming().RegisterEvent("FinishReadDVDThread", GlobalFinishRead);

  // This is reset on every launch for determinism, but it doesn't matter
  // much, because this will never get exposed to the emulated game.
  m_next_id = 0;

  m_dvd_thread.Reset("DVD thread", std::bind_front(&DVDThread::ProcessReadRequest, this));
}

void DVDThread::Stop()
{
  m_dvd_thread.Cancel();
  m_dvd_thread.Shutdown();

  FreeResults();
  m_result_map.clear();
  m_num_lent_reads = 0;

  DiscIO::DestroyVolume(m_disc);
  m_disc = nullptr;
}

void DVDThread::DoState(PointerWrap& p)
{
  // Ensure all requests are completed with results Push'd to m_result_queue.
  WaitUntilIdle();

  // Move all results from result_queue to result_map because
  // PointerWrap::Do supports std::map but not the queue.
  // This won't affect the behavior of FinishRead.
  while (ResultNode* const node = TakeResult(~u64{0}))
  {
    m_result_map.emplace(node->request.id,
                         ReadResult(node->request, std::vector<u8>(node->data,
                                                                   node->data + node->size)));
    mpsc_stack_push(&m_free_results, &node->link);
  }
  // Lent reads are saved as the results the DVD thread would have made; a state loaded has
  // only those.
  if (p.IsReadMode())
    m_num_lent_reads = 0;
  else
    MaterializeLentReads();

  p.Do(m_result_map);
  p.Do(m_next_id);

  // m_disc isn't savestated (because it points to files on the
  // local system). Instead, we check that the status of the disc
  // is the same as when the savestate was made. This won't catch
  // cases of having the wrong disc inserted, though.
  // TODO: Check the game ID, disc number, revision?
  bool had_disc = HasDisc();
  p.Do(had_disc);
  if (had_disc != HasDisc())
  {
    if (had_disc)
      PanicAlert("An inserted disc was expected but not found.");
    else
    {
      DiscIO::DestroyVolume(m_disc);
      m_disc = nullptr;
    }
  }

  // TODO: Savestates can be smaller if the buffers of results aren't saved,
  // but instead get re-read from the disc when loading the savestate.

  // TODO: It would be possible to create a savestate faster by stopping
  // the DVD thread regardless of whether there are pending requests.

  // After loading a savestate, the debug log in FinishRead will report
  // screwed up times for requests that were submitted before the savestate
  // was made. Handling that properly may be more effort than it's worth.
}

void DVDThread::SetDisc(DiscIO::Volume* disc)
{
  WaitUntilIdle();
  // what was read from the disc going away stays read
  MaterializeLentReads();
  DiscIO::DestroyVolume(m_disc);
  m_disc = disc;
}

bool DVDThread::HasDisc() const
{
  return m_disc != nullptr;
}

bool DVDThread::HasWiiHashes() const
{
  // HasWiiHashes is thread-safe, so calling WaitUntilIdle isn't necessary.
  return m_disc->HasWiiHashes();
}

DiscIO::Platform DVDThread::GetDiscType() const
{
  // GetVolumeType is thread-safe, so calling WaitUntilIdle isn't necessary.
  return m_disc->GetVolumeType();
}

u64 DVDThread::PartitionOffsetToRawOffset(u64 offset, const DiscIO::Partition& partition)
{
  // PartitionOffsetToRawOffset is thread-safe, so calling WaitUntilIdle isn't necessary.
  return m_disc->PartitionOffsetToRawOffset(offset, partition);
}

IOS::ES::TMDReader DVDThread::GetTMD(const DiscIO::Partition& partition)
{
  WaitUntilIdle();
  return m_disc->GetTMD(partition);
}

IOS::ES::TicketReader DVDThread::GetTicket(const DiscIO::Partition& partition)
{
  WaitUntilIdle();
  return m_disc->GetTicket(partition);
}

bool DVDThread::IsInsertedDiscRunning()
{
  if (!m_disc)
    return false;

  WaitUntilIdle();

  return SConfig::GetInstance().GetGameID() == m_disc->GetGameID();
}

bool DVDThread::UpdateRunningGameMetadata(const DiscIO::Partition& partition,
                                          std::optional<u64> title_id)
{
  if (!m_disc)
    return false;

  WaitUntilIdle();

  if (title_id)
  {
    const std::optional<u64> volume_title_id = m_disc->GetTitleID(partition);
    if (!volume_title_id || *volume_title_id != *title_id)
      return false;
  }

  SConfig::GetInstance().SetRunningGameMetadata(*m_disc, partition);
  return true;
}

void DVDThread::WaitUntilIdle()
{
  ASSERT(Core::IsCPUThread());

  m_dvd_thread.WaitForCompletion();
}

void DVDThread::StartRead(u64 dvd_offset, u32 length, const DiscIO::Partition& partition,
                          DVD::ReplyType reply_type, s64 ticks_until_completion)
{
  StartReadInternal(false, 0, dvd_offset, length, partition, reply_type, ticks_until_completion);
}

void DVDThread::StartReadToEmulatedRAM(u32 output_address, u64 dvd_offset, u32 length,
                                       const DiscIO::Partition& partition,
                                       DVD::ReplyType reply_type, s64 ticks_until_completion)
{
  StartReadInternal(true, output_address, dvd_offset, length, partition, reply_type,
                    ticks_until_completion);
}

void DVDThread::StartReadInternal(bool copy_to_ram, u32 output_address, u64 dvd_offset, u32 length,
                                  const DiscIO::Partition& partition, DVD::ReplyType reply_type,
                                  s64 ticks_until_completion)
{
  ASSERT(Core::IsCPUThread());

  auto& core_timing = m_system.GetCoreTiming();

  ReadRequest request;

  request.copy_to_ram = copy_to_ram;
  request.output_address = output_address;
  request.dvd_offset = dvd_offset;
  request.length = length;
  request.partition = partition;
  request.reply_type = reply_type;

  u64 id = m_next_id++;
  request.id = id;

  request.time_started_ticks = core_timing.GetTicks();

  if (m_num_lent_reads < MAX_LENT_READS && m_disc && m_disc->View(dvd_offset, length, partition))
  {
    m_file_logger.Log(*m_disc, partition, dvd_offset);
    m_disc->Prefetch(dvd_offset, length, partition);
    m_lent_reads[m_num_lent_reads++] = request;
  }
  else
  {
    m_dvd_thread.Push(std::move(request));
  }
  core_timing.ScheduleEvent(ticks_until_completion, m_finish_read, id);
}

void DVDThread::MaterializeLentReads()
{
  for (int i = 0; i < m_num_lent_reads; i++)
  {
    const ReadRequest& request = m_lent_reads[i];
    const u8* data = m_disc ? m_disc->View(request.dvd_offset, request.length, request.partition) :
                              nullptr;
    std::vector<u8> buffer;
    if (data)
      buffer.assign(data, data + request.length);
    m_result_map.emplace(request.id, ReadResult(request, std::move(buffer)));
  }
  m_num_lent_reads = 0;
}

void DVDThread::GlobalFinishRead(Core::System& system, u64 id, s64 cycles_late)
{
  system.GetDVDThread().FinishRead(id, cycles_late);
}

void DVDThread::FinishRead(u64 id, s64 cycles_late)
{
  for (int i = 0; i < m_num_lent_reads; i++)
  {
    if (m_lent_reads[i].id != id)
      continue;

    const ReadRequest request = m_lent_reads[i];
    m_lent_reads[i] = m_lent_reads[--m_num_lent_reads];

    // the disc is the one the read started on: a change materializes lent reads first
    const u8* data = m_disc->View(request.dvd_offset, request.length, request.partition);
    if (request.copy_to_ram)
      m_system.GetMemory().CopyToEmu(request.output_address, data, request.length);
    m_system.GetDVDInterface().FinishExecutingCommand(
        request.reply_type, DVD::DIInterruptType::TCINT, cycles_late, data, request.length);
    return;
  }

  // The DVD thread may finish reads in a different order than they are wanted, so results
  // are looked up by id: in the map a savestate load filled, else among those that arrived.
  ReadResult result;
  ResultNode* node = nullptr;
  auto it = m_result_map.find(id);
  if (it != m_result_map.end())
  {
    result = std::move(it->second);
    m_result_map.erase(it);
  }
  else
  {
    while (!(node = TakeResult(id)))
    {
      const int key = retro_eventcount_prepare_wait(&m_result_ready);
      if (!mpsc_stack_empty(&m_result_queue))
      {
        retro_eventcount_cancel_wait(&m_result_ready);
        continue;
      }
      retro_eventcount_commit_wait(&m_result_ready, key);
    }
  }
  // We have now obtained the right ReadResult.

  const ReadRequest& request = node ? node->request : result.first;
  const u8* const data = node ? node->data : result.second.data();
  const size_t size = node ? node->size : result.second.size();

  DEBUG_LOG(DVDINTERFACE, "Disc has been read. Emulated time including delay: %" PRIu64 " us.",
            (u64)((m_system.GetCoreTiming().GetTicks() - request.time_started_ticks) /
                  (m_system.GetSystemTimers().GetTicksPerSecond() / 1000000)));

  auto& dvd_interface = m_system.GetDVDInterface();
  DVD::DIInterruptType interrupt;
  if (size != request.length)
  {
    PanicAlert("The disc could not be read (at %#" PRIx64 " - %#" PRIx64 ").",
               (u64)request.dvd_offset, (u64)(request.dvd_offset + request.length));

    dvd_interface.SetDriveError(DVD::DriveError::ReadError);
    interrupt = DVD::DIInterruptType::DEINT;
  }
  else
  {
    if (request.copy_to_ram)
    {
      auto& memory = m_system.GetMemory();
      memory.CopyToEmu(request.output_address, data, request.length);
    }

    interrupt = DVD::DIInterruptType::TCINT;
  }

  // Notify the emulated software that the command has been executed
  dvd_interface.FinishExecutingCommand(request.reply_type, interrupt, cycles_late, data, size);

  if (node)
    mpsc_stack_push(&m_free_results, &node->link);
}

// DVD thread. A result to fill: one handed back, or a new one while none has been.
DVDThread::ResultNode* DVDThread::TakeFreeResult()
{
  mpsc_stack_node_t* node;
  if (!m_worker_free)
    m_worker_free = mpsc_stack_drain(&m_free_results);
  node = m_worker_free;
  if (!node)
    return static_cast<ResultNode*>(std::calloc(1, sizeof(ResultNode)));
  m_worker_free = node->next;
  return reinterpret_cast<ResultNode*>(node);
}

void DVDThread::ProcessReadRequest(ReadRequest&& request)
{
  m_file_logger.Log(*m_disc, request.partition, request.dvd_offset);

  ResultNode* const node = TakeFreeResult();
  node->request = request;
  node->size = 0;
  if (node->capacity < request.length)
  {
    // grows to the largest read, then stays
    u8* const grown = static_cast<u8*>(std::realloc(node->data, request.length));
    if (grown)
    {
      node->data = grown;
      node->capacity = request.length;
    }
  }
  if (node->capacity >= request.length &&
      m_disc->Read(request.dvd_offset, request.length, node->data, request.partition))
  {
    node->size = request.length;
  }

  mpsc_stack_push(&m_result_queue, &node->link);
  retro_eventcount_notify(&m_result_ready);
}
}  // namespace DVD
