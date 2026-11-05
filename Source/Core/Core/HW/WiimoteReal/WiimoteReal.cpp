// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteReal/WiimoteReal.h"

#include <algorithm>
#include <unordered_set>

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>

#include <SFML/Network/UdpSocket.hpp>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Quiesce.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Common/FrameClock.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/DataReport.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"
#include "Core/HW/WiimoteReal/IOAndroid.h"
#include "Core/HW/WiimoteReal/IOLinux.h"
#include "Core/HW/WiimoteReal/IOWin.h"
#include "Core/HW/WiimoteReal/IOhidapi.h"
#include "Core/System.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Wiimote/WiimoteController.h"

namespace WiimoteReal
{
using namespace WiimoteCommon;

static void TryToConnectBalanceBoard(std::unique_ptr<Wiimote>);
static bool TryToConnectWiimoteToSlot(std::unique_ptr<Wiimote>&, unsigned int);
static void HandleWiimoteDisconnect(int index);

static bool s_real_wiimotes_initialized = false;

// Slots and the pool are owned by one thread, the manager (the scanner's pool thread). Other
// threads hand it commands; the slots themselves are published pointers anyone may read.
// A slot changes only while the manager holds the CPU thread (CPUThreadGuard), so the
// emulated Bluetooth device, which keeps the pointer, never sees one go away under it.
retro_atomic_ptr_t g_wiimotes[MAX_BBMOTES];

Wiimote* GetWiimote(unsigned int index)
{
  return static_cast<Wiimote*>(retro_atomic_load_acquire_ptr(&g_wiimotes[index]));
}

// Connected Wiimotes' IDs, so we don't connect more than once to the same device. The scanner
// reads and adds to it, a Wiimote going away takes its ID out; each change swaps in a copy.
struct KnownIds
{
  quiesce_node_t node;  // first: the free callback casts it back
  std::unordered_set<std::string> ids;
};
static quiesce_t s_known_ids_quiesce;
static retro_atomic_ptr_t s_known_ids;

static void FreeKnownIds(quiesce_node_t* node)
{
  delete reinterpret_cast<KnownIds*>(node);
}

template <typename Edit>
static void EditKnownIds(Edit&& edit)
{
  quiesce_enter(&s_known_ids_quiesce);
  for (;;)
  {
    void* const cur = retro_atomic_load_acquire_ptr(&s_known_ids);
    auto* next = new KnownIds{{}, cur ? static_cast<const KnownIds*>(cur)->ids :
                                        std::unordered_set<std::string>{}};
    edit(next->ids);
    if (retro_atomic_cas_ptr(&s_known_ids, cur, next))
    {
      if (cur)
        quiesce_retire(&s_known_ids_quiesce, &static_cast<KnownIds*>(cur)->node, FreeKnownIds);
      break;
    }
    delete next;
  }
  quiesce_exit(&s_known_ids_quiesce);
}

struct WiimotePoolEntry
{
  std::unique_ptr<Wiimote> wiimote;
  u64 entry_frame = frame_clock_now();

  bool IsExpired() const
  {
    // Keep wii remotes in the pool for a bit, about five seconds, before disconnecting them.
    constexpr u64 POOL_FRAMES = 5 * 60;

    return frame_clock_now() - entry_frame > POOL_FRAMES;
  }
};

// Connected wii remotes are placed here when no open slot is set to "Real".
// They are then automatically disconnected after some time. Manager only.
static std::vector<WiimotePoolEntry> s_wiimote_pool;

// What other threads ask of the manager.
struct ManagerCommand
{
  enum class Kind
  {
    AddToPool,
    ConnectBalanceBoard,
    SourceChange,
    ProcessPool,
    EmuStop,
    EmuResume,
    EmuPause,
  };
  mpsc_stack_node_t link;  // first: the drained chain is cast back
  Kind kind;
  std::unique_ptr<Wiimote> wiimote;
  unsigned int index;
};
// The manager waits on the frame clock, so posting a command wakes it through that.
static mpsc_stack_t s_manager_commands;
static retro_atomic_int_t s_manager_running;
static thread_local bool t_is_manager = false;

static void PostToManager(ManagerCommand::Kind kind, std::unique_ptr<Wiimote> wiimote = {},
                          unsigned int index = 0)
{
  auto* command = new ManagerCommand{{}, kind, std::move(wiimote), index};
  mpsc_stack_push(&s_manager_commands, &command->link);
  frame_clock_wake_all();
}

static void PutWiimoteInSlot(unsigned int index, std::unique_ptr<Wiimote> wiimote)
{
  delete static_cast<Wiimote*>(retro_atomic_exchange_ptr(&g_wiimotes[index], wiimote.release()));
}

static std::unique_ptr<Wiimote> TakeWiimoteFromSlot(unsigned int index)
{
  return std::unique_ptr<Wiimote>(
      static_cast<Wiimote*>(retro_atomic_exchange_ptr(&g_wiimotes[index], nullptr)));
}

static WiimoteScanner s_wiimote_scanner;

// Attempt to fill a real wiimote slot from the pool or by stealing from ControllerInterface.
// Manager only.
static void TryToFillWiimoteSlot(u32 index)
{
  if (GetWiimote(index) ||
      Config::Get(Config::GetInfoForWiimoteSource(index)) != WiimoteSource::Real)
  {
    return;
  }

  // If the pool is empty, attempt to steal from ControllerInterface.
  if (s_wiimote_pool.empty())
  {
    ciface::WiimoteController::ReleaseDevices(1);

    // Still empty?
    if (s_wiimote_pool.empty())
      return;
  }

  if (TryToConnectWiimoteToSlot(s_wiimote_pool.front().wiimote, index))
    s_wiimote_pool.erase(s_wiimote_pool.begin());
}

void PopulateDevices()
{
  // There is a very small chance of deadlocks if we didn't do it in another thread
  s_wiimote_scanner.PopulateDevices();
}

// Attempts to fill enabled real wiimote slots.
// Push/pull wiimotes to/from ControllerInterface as needed. Manager only.
static void ProcessWiimotePool()
{
  for (u32 index = 0; index != MAX_WIIMOTES; ++index)
    TryToFillWiimoteSlot(index);

  if (Config::Get(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE))
  {
    for (auto& entry : s_wiimote_pool)
      ciface::WiimoteController::AddDevice(std::move(entry.wiimote));

    s_wiimote_pool.clear();
  }
  else
  {
    ciface::WiimoteController::ReleaseDevices();
  }
}

static void AddWiimoteToPoolNow(std::unique_ptr<Wiimote> wiimote)
{
  // Our real wiimote class requires an index.
  // Within the pool it's only going to be used for logging purposes.
  static constexpr int POOL_WIIMOTE_INDEX = 99;

  if (!wiimote->Connect(POOL_WIIMOTE_INDEX))
  {
    ERROR_LOG_FMT(WIIMOTE, "Failed to connect real wiimote.");
    return;
  }

  wiimote->EmuStop();

  s_wiimote_pool.emplace_back(WiimotePoolEntry{std::move(wiimote)});
}

void AddWiimoteToPool(std::unique_ptr<Wiimote> wiimote)
{
  // ControllerInterface hands remotes back from inside the manager's own pool processing,
  // which expects them in the pool on return.
  if (t_is_manager)
    AddWiimoteToPoolNow(std::move(wiimote));
  else if (retro_atomic_load_acquire_int(&s_manager_running))
    PostToManager(ManagerCommand::Kind::AddToPool, std::move(wiimote));
  // With no manager the remote is simply let go, which disconnects it.
}

// Records each ring holds: 256 reports is over a second of input at the Wiimote's 200 Hz.
static constexpr size_t REPORT_RING_RECORDS = 256;

Wiimote::Wiimote()
{
  retro_spsc_init(&m_read_reports, REPORT_RING_RECORDS * sizeof(ReadRecord));
  retro_spsc_init(&m_write_reports, REPORT_RING_RECORDS * sizeof(WriteRecord));
  retro_eventcount_init(&m_write_event);
  m_config_changed_callback_id = Config::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();
}

Wiimote::~Wiimote()
{
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
  retro_eventcount_free(&m_write_event);
  retro_spsc_free(&m_write_reports);
  retro_spsc_free(&m_read_reports);
}

// Producer side of m_write_reports. A ring this deep is only full when the device has stopped
// taking reports, and the write thread is about to drop the connection then.
void Wiimote::QueueWrite(u64 frame, const u8* data, size_t size)
{
  ASSERT(size <= MAX_PAYLOAD);
  WriteRecord record;
  record.frame = frame;
  record.size = static_cast<u8>(size);
  std::copy_n(data, size, record.data);
  retro_spsc_write_frames(&m_write_reports, &record, 1, sizeof(record));
  retro_eventcount_notify(&m_write_event);
}

void Wiimote::Shutdown()
{
  EditKnownIds([this](std::unordered_set<std::string>& ids) { ids.erase(GetId()); });

  StopThread();
  ClearReadQueue();
}

// to be called from CPU thread
void Wiimote::WriteReport(Report rpt)
{
  if (rpt.size() >= 3)
  {
    bool const new_rumble_state = (rpt[2] & 0x1) != 0;

    switch (WiimoteCommon::OutputReportID(rpt[1]))
    {
    case OutputReportID::Rumble:
      // If this is a rumble report and the rumble state didn't change, we can drop this report.
      if (new_rumble_state == m_rumble_state)
        return;
      break;

    case OutputReportID::SpeakerEnable:
      m_speaker_enable = (rpt[2] & 0x4) != 0;
      break;

    case OutputReportID::SpeakerMute:
      m_speaker_mute = (rpt[2] & 0x4) != 0;
      break;

    default:
      break;
    }

    m_rumble_state = new_rumble_state;
  }

  // Sent as soon as the write thread gets to it.
  QueueWrite(0, rpt.data(), rpt.size());
}

// to be called from CPU thread
void Wiimote::QueueReport(WiimoteCommon::OutputReportID rpt_id, const void* data, unsigned int size)
{
  auto const queue_data = static_cast<const u8*>(data);

  Report rpt(size + 2);
  rpt[0] = WR_SET_REPORT | BT_OUTPUT;
  rpt[1] = u8(rpt_id);
  std::copy_n(queue_data, size, rpt.begin() + 2);
  WriteReport(std::move(rpt));
}

void Wiimote::ResetDataReporting()
{
  m_last_input_report.clear();

  // "core" reporting in non-continuous mode is a wiimote's initial state.
  // FYI: This also disables rumble.
  OutputReportMode rpt = {};
  rpt.mode = InputReportID::ReportCore;
  rpt.continuous = 0;
  QueueReport(rpt);
}

void Wiimote::ClearReadQueue()
{
  // Consumer side: discards what the read thread has delivered so far.
  retro_spsc_skip(&m_read_reports, retro_spsc_read_avail(&m_read_reports));
}

void Wiimote::EventLinked()
{
  m_is_linked = true;

  ClearReadQueue();
  ResetDataReporting();
  EnablePowerAssertionInternal();
}

void Wiimote::EventUnlinked()
{
  if (m_really_disconnect)
    DisconnectInternal();
  else
    EmuStop();
}

void Wiimote::InterruptDataOutput(const u8* data, const u32 size)
{
  Report rpt(size + REPORT_HID_HEADER_SIZE);
  std::copy_n(data, size, rpt.data() + REPORT_HID_HEADER_SIZE);

  // Convert output DATA packets to SET_REPORT packets.
  // Nintendo Wiimotes work without this translation, but 3rd
  // party ones don't.
  rpt[0] = WR_SET_REPORT | BT_OUTPUT;

  // Disallow games from turning off all of the LEDs.
  // It makes Wiimote connection status confusing.
  if (rpt[1] == u8(OutputReportID::LED))
  {
    auto& leds_rpt = *reinterpret_cast<OutputReportLeds*>(&rpt[2]);
    if (0 == leds_rpt.leds)
    {
      // Turn on ALL of the LEDs.
      leds_rpt.leds = 0xf;
    }
  }
  else if (rpt[1] == u8(OutputReportID::SpeakerData) &&
           (!m_speaker_enabled_in_dolphin_config || !m_speaker_enable || m_speaker_mute))
  {
    rpt.resize(3);
    // Translate undesired speaker data reports into rumble reports.
    rpt[1] = u8(OutputReportID::Rumble);
    // Keep only the rumble bit.
    rpt[2] &= 0x1;
  }

  WriteReport(std::move(rpt));
}

bool Wiimote::Read()
{
  // Receive straight into the next ring record. With the ring full the core has stopped taking
  // reports; receive into a scratch record and drop it.
  ReadRecord scratch;
  ReadRecord* record = &scratch;
  void* span;
  const bool reserved = retro_spsc_write_begin(&m_read_reports, &span) >= sizeof(ReadRecord);
  if (reserved)
    record = static_cast<ReadRecord*>(span);

  auto const result = IORead(record->data);

  // Drop the report if not connected.
  const bool keep = m_is_linked && result > 0;
  if (keep && m_balance_board_dump_port > 0 && m_index == WIIMOTE_BALANCE_BOARD)
  {
    static sf::UdpSocket Socket;
    (void)Socket.send(record->data, MAX_PAYLOAD, sf::IpAddress::LocalHost,
                      m_balance_board_dump_port);
  }

  if (reserved)
  {
    record->size = static_cast<u8>(result);
    retro_spsc_write_end(&m_read_reports, keep ? sizeof(ReadRecord) : 0);
  }

  return result != 0;
}

// Write thread: a report is queued and its frame has come.
bool Wiimote::IsWriteDue() const
{
  if (retro_spsc_read_avail(&m_write_reports) < sizeof(WriteRecord))
    return false;
  u64 frame;
  retro_spsc_peek(&m_write_reports, &frame, sizeof(frame));
  return frame <= frame_clock_now();
}

void Wiimote::Poke()
{
  retro_eventcount_notify(&m_write_event);
}

bool Wiimote::Write(const WriteRecord& record)
{
  if (m_balance_board_dump_port > 0 && m_index == WIIMOTE_BALANCE_BOARD)
  {
    static sf::UdpSocket Socket;
    (void)Socket.send(record.data, record.size, sf::IpAddress::LocalHost,
                      m_balance_board_dump_port);
  }

  int ret = IOWrite(record.data, record.size);

  return ret != 0;
}

bool Wiimote::IsBalanceBoard()
{
  if (!ConnectInternal())
    return false;
  // Initialise the extension by writing 0x55 to 0xa400f0, then writing 0x00 to 0xa400fb.
  // TODO: Use the structs for building these reports..
  static const u8 init_extension_rpt1[MAX_PAYLOAD] = {
      WR_SET_REPORT | BT_OUTPUT, u8(OutputReportID::WriteData), 0x04, 0xa4, 0x00, 0xf0, 0x01, 0x55};
  static const u8 init_extension_rpt2[MAX_PAYLOAD] = {
      WR_SET_REPORT | BT_OUTPUT, u8(OutputReportID::WriteData), 0x04, 0xa4, 0x00, 0xfb, 0x01, 0x00};
  static const u8 status_report[] = {WR_SET_REPORT | BT_OUTPUT, u8(OutputReportID::RequestStatus),
                                     0};
  if (!IOWrite(init_extension_rpt1, sizeof(init_extension_rpt1)) ||
      !IOWrite(init_extension_rpt2, sizeof(init_extension_rpt2)))
  {
    ERROR_LOG_FMT(WIIMOTE, "IsBalanceBoard(): Failed to initialise extension.");
    return false;
  }

  int ret = IOWrite(status_report, sizeof(status_report));
  u8 buf[MAX_PAYLOAD];
  while (ret != 0)
  {
    ret = IORead(buf);
    if (ret == -1)
      continue;

    switch (InputReportID(buf[1]))
    {
    case InputReportID::Status:
    {
      const auto* status = reinterpret_cast<InputReportStatus*>(&buf[2]);
      // A Balance Board has a Balance Board extension.
      if (!status->extension)
        return false;
      // Read two bytes from 0xa400fe to identify the extension.
      static const u8 identify_ext_rpt[] = {WR_SET_REPORT | BT_OUTPUT,
                                            u8(OutputReportID::ReadData),
                                            0x04,
                                            0xa4,
                                            0x00,
                                            0xfe,
                                            0x02,
                                            0x00};
      ret = IOWrite(identify_ext_rpt, sizeof(identify_ext_rpt));
      break;
    }
    case InputReportID::ReadDataReply:
    {
      const auto* reply = reinterpret_cast<InputReportReadDataReply*>(&buf[2]);
      if (Common::swap16(reply->address) != 0x00fe)
      {
        ERROR_LOG_FMT(WIIMOTE, "IsBalanceBoard(): Received unexpected data reply for address {:X}",
                      Common::swap16(reply->address));
        return false;
      }
      // A Balance Board ext can be identified by checking for 0x0402.
      return reply->data[0] == 0x04 && reply->data[1] == 0x02;
    }
    case InputReportID::Ack:
    {
      const auto* ack = reinterpret_cast<InputReportAck*>(&buf[2]);
      if (ack->rpt_id == OutputReportID::ReadData && ack->error_code != ErrorCode::Success)
      {
        WARN_LOG_FMT(WIIMOTE,
                     "Failed to read from 0xa400fe, assuming Wiimote is not a Balance Board.");
        return false;
      }
      break;
    }
    default:
      break;
    }
  }
  return false;
}

static bool IsDataReport(const Report& rpt)
{
  return rpt.size() >= 2 && rpt[1] >= u8(InputReportID::ReportCore);
}

bool Wiimote::GetNextReport(Report* report)
{
  const void* span;
  if (retro_spsc_read_begin(&m_read_reports, &span) < sizeof(ReadRecord))
    return false;
  const ReadRecord* const record = static_cast<const ReadRecord*>(span);
  report->assign(record->data, record->data + record->size);
  retro_spsc_read_end(&m_read_reports, sizeof(ReadRecord));
  return true;
}

// Returns the next report that should be sent
Report& Wiimote::ProcessReadQueue(bool repeat_last_data_report)
{
  // If we're not repeating data reports or had a non-data report, any old report is irrelevant.
  if (!repeat_last_data_report || !IsDataReport(m_last_input_report))
    m_last_input_report.clear();

  // Step through the read queue.
  while (GetNextReport(&m_last_input_report))
  {
    // Stop on a non-data report.
    if (!IsDataReport(m_last_input_report))
      break;
  }

  return m_last_input_report;
}

u8 Wiimote::GetWiimoteDeviceIndex() const
{
  return m_bt_device_index;
}

void Wiimote::SetWiimoteDeviceIndex(u8 index)
{
  m_bt_device_index = index;
}

void Wiimote::PrepareInput(WiimoteEmu::DesiredWiimoteState* target_state,
                           SensorBarState sensor_bar_state)
{
  // Nothing to do here on real Wiimotes.
}

void Wiimote::Update(const WiimoteEmu::DesiredWiimoteState& target_state)
{
  // Wii remotes send input at 200hz once a Wii enables "sniff mode" on the connection.
  // PC bluetooth stacks do not enable sniff mode causing remotes to send input at only 100hz.
  //
  // Commercial games do not send speaker data unless input rates approach 200hz.
  // Some games will also present input issues.
  // e.g. Super Mario Galaxy's star cursor drops in and out.
  //
  // If we want speaker data we must pretend input is at 200hz.
  // We duplicate data reports to accomplish this.
  // Unfortunately this breaks detection of motion gestures in some games.
  // e.g. Sonic and the Secret Rings, Jett Rocket
  const bool repeat_reports_to_maintain_200hz =
      Config::Get(Config::MAIN_REAL_WII_REMOTE_REPEAT_REPORTS);

  const Report& rpt = ProcessReadQueue(repeat_reports_to_maintain_200hz);

  if (rpt.empty())
    return;

  InterruptCallback(rpt.front(), rpt.data() + REPORT_HID_HEADER_SIZE,
                    u32(rpt.size() - REPORT_HID_HEADER_SIZE));
}

ButtonData Wiimote::GetCurrentlyPressedButtons()
{
  Report& rpt = m_last_input_report;
  if (rpt.size() >= 4)
  {
    const auto mode = InputReportID(rpt[1]);

    // TODO: Button data could also be pulled out of non-data reports if really wanted.
    if (DataReportBuilder::IsValidMode(mode))
    {
      auto builder = MakeDataReportManipulator(mode, rpt.data() + 2);
      ButtonData buttons = {};
      builder->GetCoreData(&buttons);

      return buttons;
    }
  }
  return ButtonData{};
}

void Wiimote::Prepare()
{
  const u64 now = frame_clock_now();

  // Set reporting mode to non-continuous core buttons and turn on rumble.
  const u8 mode_report[] = {WR_SET_REPORT | BT_OUTPUT, u8(OutputReportID::ReportMode), 1,
                            u8(InputReportID::ReportCore)};
  QueueWrite(now, mode_report, sizeof(mode_report));

  // Request status and turn off rumble a dozen frames, about 200 ms, later.
  const u8 req_status_report[] = {WR_SET_REPORT | BT_OUTPUT, u8(OutputReportID::RequestStatus), 0};
  QueueWrite(now + 12, req_status_report, sizeof(req_status_report));
}

void Wiimote::EmuStop()
{
  m_is_linked = false;

  ResetDataReporting();
  DisablePowerAssertionInternal();
}

void Wiimote::EmuResume()
{
  EnablePowerAssertionInternal();
}

void Wiimote::EmuPause()
{
  DisablePowerAssertionInternal();
}

static unsigned int CalculateWantedWiimotes()
{
  // Figure out how many real Wiimotes are required
  unsigned int wanted_wiimotes = 0;
  for (int i = 0; i < MAX_WIIMOTES; ++i)
  {
    if (Config::Get(Config::GetInfoForWiimoteSource(i)) == WiimoteSource::Real && !GetWiimote(i))
      ++wanted_wiimotes;
  }
  return wanted_wiimotes;
}

static unsigned int CalculateWantedBB()
{
  unsigned int wanted_bb = 0;
  if (Config::Get(Config::WIIMOTE_BB_SOURCE) == WiimoteSource::Real &&
      !GetWiimote(WIIMOTE_BALANCE_BOARD))
  {
    ++wanted_bb;
  }
  return wanted_bb;
}

void WiimoteScanner::StartThread()
{
  if (m_scan_thread_running.IsSet())
    return;
  m_scan_thread_running.Set();
  m_scan_thread = sthread_create(Common::ThreadEntry<&WiimoteScanner::ThreadFunc>, this);
}

void WiimoteScanner::StopThread()
{
  if (m_scan_thread_running.IsSet())
  {
    SetScanMode(WiimoteScanMode::DO_NOT_SCAN);

    // The scanner thread frees its backends only after it sees the running flag drop, which
    // happens after this, so the list stays valid here.
    if (const auto* backends = static_cast<const std::vector<WiimoteScannerBackend*>*>(
            retro_atomic_load_acquire_ptr(&m_backends_published)))
    {
      for (WiimoteScannerBackend* backend : *backends)
        backend->RequestStopSearching();
    }

    m_scan_thread_running.Clear();
    frame_clock_wake_all();
    Common::JoinThread(m_scan_thread);
  }
}

void WiimoteScanner::SetScanMode(WiimoteScanMode scan_mode)
{
  m_scan_mode.store(scan_mode);
  retro_atomic_store_release_int(&m_scan_requested, 1);
  frame_clock_wake_all();
}

// Manager only.
static void CheckForDisconnectedWiimotes()
{
  for (unsigned int i = 0; i < MAX_BBMOTES; ++i)
    if (const Wiimote* wiimote = GetWiimote(i); wiimote && !wiimote->IsConnected())
      HandleWiimoteDisconnect(i);
}

static void RunManagerCommand(ManagerCommand& command)
{
  switch (command.kind)
  {
  case ManagerCommand::Kind::AddToPool:
    AddWiimoteToPoolNow(std::move(command.wiimote));
    break;
  case ManagerCommand::Kind::ConnectBalanceBoard:
    TryToConnectBalanceBoard(std::move(command.wiimote));
    break;
  case ManagerCommand::Kind::SourceChange:
  {
    std::unique_ptr<Wiimote> removed_wiimote;
    {
      const Core::CPUThreadGuard guard(Core::System::GetInstance());
      removed_wiimote = TakeWiimoteFromSlot(command.index);
      WiimoteCommon::UpdateSource(command.index);
    }

    // The Wiimote pool isn't currently designed to handle Balance Boards.
    // Refactoring will be needed to differentiate between remotes/boards.
    const bool is_balance_board = command.index == WIIMOTE_BALANCE_BOARD;

    if (removed_wiimote != nullptr && !is_balance_board)
      AddWiimoteToPoolNow(std::move(removed_wiimote));
    g_controller_interface.PlatformPopulateDevices([] { ProcessWiimotePool(); });
    break;
  }
  case ManagerCommand::Kind::ProcessPool:
    g_controller_interface.PlatformPopulateDevices([] { ProcessWiimotePool(); });
    break;
  case ManagerCommand::Kind::EmuStop:
  case ManagerCommand::Kind::EmuResume:
  case ManagerCommand::Kind::EmuPause:
    for (unsigned int i = 0; i < MAX_BBMOTES; ++i)
    {
      Wiimote* const wiimote = GetWiimote(i);
      if (!wiimote || !wiimote->IsConnected())
        continue;
      if (command.kind == ManagerCommand::Kind::EmuStop)
        wiimote->EmuStop();
      else if (command.kind == ManagerCommand::Kind::EmuResume)
        wiimote->EmuResume();
      else
        wiimote->EmuPause();
    }
    break;
  }
}

static void RunManagerCommands()
{
  // The stack gives the newest first; run them in the order they were posted.
  for (mpsc_stack_node_t* node =
           mpsc_stack_reverse(mpsc_stack_drain(&s_manager_commands));
       node;)
  {
    auto* command = reinterpret_cast<ManagerCommand*>(node);
    node = node->next;
    RunManagerCommand(*command);
    delete command;
  }
}

void WiimoteScanner::PoolThreadFunc()
{
  Common::SetCurrentThreadName("Wiimote Pool Thread");
  t_is_manager = true;

  // Toggle between 1010 and 0101.
  u8 led_value = 0b1010;

  // About a quarter of a second of frontend frames.
  constexpr u64 TICK_FRAMES = 15;
  u64 next_tick = frame_clock_now();

  while (m_scan_thread_running.IsSet())
  {
    RunManagerCommands();

    if (frame_clock_now() < next_tick)
    {
      // Wait for the next tick or a command, whichever comes first.
      const int key = frame_clock_prepare_wait();
      if (!mpsc_stack_empty(&s_manager_commands) || !m_scan_thread_running.IsSet() ||
          frame_clock_now() >= next_tick)
      {
        frame_clock_cancel_wait();
        continue;
      }
      frame_clock_commit_wait(key);
      continue;
    }
    next_tick = frame_clock_now() + TICK_FRAMES;

    CheckForDisconnectedWiimotes();

    // Let the write threads look at their idle time and delayed reports.
    for (unsigned int i = 0; i < MAX_BBMOTES; ++i)
      if (Wiimote* const wiimote = GetWiimote(i))
        wiimote->Poke();
    for (auto& entry : s_wiimote_pool)
      entry.wiimote->Poke();

    // Remove stale pool entries.
    for (auto it = s_wiimote_pool.begin(); it != s_wiimote_pool.end();)
    {
      if (!it->wiimote->IsConnected())
      {
        INFO_LOG_FMT(WIIMOTE, "Removing disconnected wiimote pool entry.");
        it = s_wiimote_pool.erase(it);
      }
      else if (it->IsExpired())
      {
        INFO_LOG_FMT(WIIMOTE, "Removing expired wiimote pool entry.");
        it = s_wiimote_pool.erase(it);
      }
      else
      {
        ++it;
      }
    }

    // Make wiimote pool LEDs dance.
    for (auto& wiimote : s_wiimote_pool)
    {
      OutputReportLeds leds = {};
      leds.leds = led_value;
      wiimote.wiimote->QueueReport(leds);
    }

    led_value ^= 0b1111;
  }

  // What was posted before the stop still runs, so no remote handed over is lost.
  RunManagerCommands();
  t_is_manager = false;
}

void WiimoteScanner::PopulateDevices()
{
  PostToManager(ManagerCommand::Kind::ProcessPool);
}

void WiimoteScanner::ThreadFunc()
{
#if defined(__LIBRETRO__) && defined(SKIP_WIMOTESCANNER_THREAD)
  return;
#endif
  retro_atomic_store_release_int(&s_manager_running, 1);
  sthread_t* pool_thread =
      sthread_create(Common::ThreadEntry<&WiimoteScanner::PoolThreadFunc>, this);

  Common::SetCurrentThreadName("Wiimote Scanning Thread");

  NOTICE_LOG_FMT(WIIMOTE, "Wiimote scanning thread has started.");

  // Create and destroy scanner backends here to ensure all operations stay on the same thread. The
  // HIDAPI backend on macOS has an error condition when IOHIDManagerCreate and IOHIDManagerClose
  // are called on different threads (and so reference different CFRunLoops) which can cause an
  // EXC_BAD_ACCES crash.
  std::vector<std::unique_ptr<WiimoteScannerBackend>> backends;
  backends.emplace_back(std::make_unique<WiimoteScannerLinux>());
  backends.emplace_back(std::make_unique<WiimoteScannerAndroid>());
  backends.emplace_back(std::make_unique<WiimoteScannerWindows>());
  backends.emplace_back(std::make_unique<WiimoteScannerHidapi>());

  std::vector<WiimoteScannerBackend*> published;
  for (const auto& backend : backends)
    published.push_back(backend.get());
  retro_atomic_store_release_ptr(&m_backends_published, &published);

  // About half a second of frontend frames between looks for remotes.
  constexpr u64 SCAN_FRAMES = 30;
  while (m_scan_thread_running.IsSet())
  {
    const u64 next_scan = frame_clock_now() + SCAN_FRAMES;
    for (;;)
    {
      const int key = frame_clock_prepare_wait();
      if (!m_scan_thread_running.IsSet() || frame_clock_now() >= next_scan ||
          retro_atomic_exchange_int(&m_scan_requested, 0))
      {
        frame_clock_cancel_wait();
        break;
      }
      frame_clock_commit_wait(key);
    }
    if (!m_scan_thread_running.IsSet())
      break;

    // Currently does nothing. To be removed.
    for (const auto& backend : backends)
      backend->Update();

    // If we don't want Wiimotes in ControllerInterface, we may not need them at all.
    if (!Config::Get(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE))
    {
      auto& system = Core::System::GetInstance();
      // We don't want any remotes in passthrough mode or running in GC mode.
      const bool core_running = Core::GetState(system) != Core::State::Uninitialized;
      if (Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED) ||
          (core_running && !system.IsWii()))
      {
        continue;
      }

      // We don't want any remotes if we already connected everything we need.
      if (0 == CalculateWantedWiimotes() && 0 == CalculateWantedBB())
        continue;
    }

    // Stop scanning if not in continuous mode.
    auto scan_mode = WiimoteScanMode::SCAN_ONCE;
    m_scan_mode.compare_exchange_strong(scan_mode, WiimoteScanMode::DO_NOT_SCAN);

    // When not scanning we still look for already attached devices.
    // This allows Windows, hidapi, and DolphinBar remotes to be quickly discovered.
    const bool should_perform_inquiry = scan_mode != WiimoteScanMode::DO_NOT_SCAN;

    for (const auto& backend : backends)
    {
      auto results =
          should_perform_inquiry ? backend->FindNewWiimotes() : backend->FindAttachedWiimotes();

      for (auto& wiimote : results.wii_remotes)
      {
        EditKnownIds([&wiimote](std::unordered_set<std::string>& ids) {
          ids.insert(wiimote->GetId());
        });
        PostToManager(ManagerCommand::Kind::AddToPool, std::move(wiimote));
        PostToManager(ManagerCommand::Kind::ProcessPool);
      }

      for (auto& bboard : results.balance_boards)
      {
        EditKnownIds([&bboard](std::unordered_set<std::string>& ids) {
          ids.insert(bboard->GetId());
        });
        PostToManager(ManagerCommand::Kind::ConnectBalanceBoard, std::move(bboard));
      }
    }
  }

  retro_atomic_store_release_ptr(&m_backends_published, nullptr);
  backends.clear();

  Common::JoinThread(pool_thread);
  retro_atomic_store_release_int(&s_manager_running, 0);

  NOTICE_LOG_FMT(WIIMOTE, "Wiimote scanning thread has stopped.");
}

bool Wiimote::Connect(int index)
{
  m_index = index;
  StartThread();
  return IsConnected();
}

void Wiimote::StartThread()
{
  if (m_write_thread)
    return;

  m_run_thread.Set();
  // Note that the write thread starts the read thread.
  m_write_thread = sthread_create(Common::ThreadEntry<&Wiimote::WriteThreadFunc>, this);

  m_thread_ready_event.Wait();
}

void Wiimote::StopThread()
{
  if (!m_write_thread)
    return;

  m_run_thread.Clear();
  retro_eventcount_notify(&m_write_event);

  // Note that the write thread stops the read thread.
  Common::JoinThread(m_write_thread);
}

void Wiimote::ReadThreadFunc()
{
  Common::SetCurrentThreadName("Wiimote Read Thread");

  while (m_run_thread.IsSet())
  {
    if (!Read())
    {
      WARN_LOG_FMT(WIIMOTE, "Wiimote::Read failed on Wiimote {}.", m_index + 1);
      m_run_thread.Clear();
      retro_eventcount_notify(&m_write_event);
      break;
    }
  }
}

void Wiimote::WriteThreadFunc()
{
  Common::SetCurrentThreadName("Wiimote Write Thread");

  bool ok = ConnectInternal();

  if (!ok)
  {
    // try again a few frames later, it might take a moment to settle
    if (frame_clock_wait_until(frame_clock_now() + 6, m_run_thread.Raw()))
      ok = ConnectInternal();
  }

  m_thread_ready_event.Set();

  if (!ok)
  {
    return;
  }

  sthread_t* read_thread = sthread_create(Common::ThreadEntry<&Wiimote::ReadThreadFunc>, this);

  // Windows and also the DolphinBar require performing a write
  //  to detect disconnections in a timely manner
  // If we haven't written a report in some time, attempt a rumble-off report.
  // This also has a minor benefit of preventing rumble from being stuck on.
  // About a second of frontend frames. The pool manager pokes this thread every few frames,
  // so it notices the time passing without a timed wait.
  constexpr u64 WRITE_TEST_FRAMES = 60;

  u64 last_write_frame = frame_clock_now();

  while (m_run_thread.IsSet())
  {
    bool write_success = false;

    if (IsWriteDue())
    {
      // Send a normal report.
      WriteRecord record;
      retro_spsc_read(&m_write_reports, &record, sizeof(record));
      write_success = Write(record);
    }
    else if (frame_clock_now() - last_write_frame >= WRITE_TEST_FRAMES)
    {
      // We haven't written in a while, test a write so we can check for a disconnect.
      DEBUG_LOG_FMT(WIIMOTE, "Sending periodic write test for Wiimote {}.", m_index + 1);
      const u8 rumble_off[] = {WR_SET_REPORT | BT_OUTPUT, u8(OutputReportID::Rumble), 0x00};

      write_success = IOWrite(std::data(rumble_off), std::size(rumble_off)) > 0;
    }
    else
    {
      // Nothing to do. Wait for a report or a poke.
      const int key = retro_eventcount_prepare_wait(&m_write_event);
      if (IsWriteDue() || !m_run_thread.IsSet())
        retro_eventcount_cancel_wait(&m_write_event);
      else
        retro_eventcount_commit_wait(&m_write_event, key);
      continue;
    }

    last_write_frame = frame_clock_now();

    if (!write_success)
    {
      WARN_LOG_FMT(WIIMOTE, "Wiimote::Write failed on Wiimote {}.", m_index + 1);
      m_run_thread.Clear();
      break;
    }
  }

  IOWakeup();
  Common::JoinThread(read_thread);

  NOTICE_LOG_FMT(WIIMOTE, "Disconnecting Wiimote {}.", m_index + 1);
  DisconnectInternal();
}

void Wiimote::RefreshConfig()
{
  m_speaker_enabled_in_dolphin_config = Config::Get(Config::MAIN_WIIMOTE_ENABLE_SPEAKER);
  m_balance_board_dump_port = Config::Get(Config::MAIN_BB_DUMP_PORT);
}

int Wiimote::GetIndex() const
{
  return m_index;
}

// config dialog calls this when some settings change
void Initialize(::Wiimote::InitializeMode init_mode)
{
  if (!s_real_wiimotes_initialized)
  {
    s_wiimote_scanner.StartThread();
  }

  if (Config::Get(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING))
    s_wiimote_scanner.SetScanMode(WiimoteScanMode::CONTINUOUSLY_SCAN);
  else
    s_wiimote_scanner.SetScanMode(WiimoteScanMode::DO_NOT_SCAN);

  if (s_real_wiimotes_initialized)
    return;

  NOTICE_LOG_FMT(WIIMOTE, "WiimoteReal::Initialize");

  s_real_wiimotes_initialized = true;
}

// called on emulation shutdown
void Stop()
{
  PostToManager(ManagerCommand::Kind::EmuStop);
}

// called when the Dolphin app exits
void Shutdown()
{
  s_real_wiimotes_initialized = false;
  s_wiimote_scanner.StopThread();

  NOTICE_LOG_FMT(WIIMOTE, "WiimoteReal::Shutdown");

  // The manager has stopped, so this thread now owns the slots and the pool.
  t_is_manager = true;
  for (unsigned int i = 0; i < MAX_BBMOTES; ++i)
    HandleWiimoteDisconnect(i);

  // Release remotes from ControllerInterface and empty the pool.
  ciface::WiimoteController::ReleaseDevices();
  s_wiimote_pool.clear();
  t_is_manager = false;

  // Commands nobody will run now.
  for (mpsc_stack_node_t* node = mpsc_stack_drain(&s_manager_commands); node;)
  {
    auto* command = reinterpret_cast<ManagerCommand*>(node);
    node = node->next;
    delete command;
  }
}

void Resume()
{
  PostToManager(ManagerCommand::Kind::EmuResume);
}

void Pause()
{
  PostToManager(ManagerCommand::Kind::EmuPause);
}

// Manager only.
static bool TryToConnectWiimoteToSlot(std::unique_ptr<Wiimote>& wm, unsigned int i)
{
  if (Config::Get(Config::GetInfoForWiimoteSource(i)) != WiimoteSource::Real || GetWiimote(i))
    return false;

  if (!wm->Connect(i))
  {
    ERROR_LOG_FMT(WIIMOTE, "Failed to connect real wiimote.");
    return false;
  }

  wm->Prepare();

  // Set LEDs.
  OutputReportLeds led_report = {};
  led_report.leds = u8(1 << (i % WIIMOTE_BALANCE_BOARD));
  wm->QueueReport(led_report);

  {
    const Core::CPUThreadGuard guard(Core::System::GetInstance());
    PutWiimoteInSlot(i, std::move(wm));
    WiimoteCommon::UpdateSource(i);
  }

  NOTICE_LOG_FMT(WIIMOTE, "Connected real wiimote to slot {}.", i + 1);

  return true;
}

static void TryToConnectBalanceBoard(std::unique_ptr<Wiimote> wm)
{
  if (TryToConnectWiimoteToSlot(wm, WIIMOTE_BALANCE_BOARD))
    return;

  NOTICE_LOG_FMT(WIIMOTE, "No open slot for real balance board.");
}

// Manager only.
static void HandleWiimoteDisconnect(int index)
{
  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  // The Wii Remote object must exist through the call to UpdateSource
  // to prevent WiimoteDevice from having a dangling HIDWiimote pointer.
  const auto temp_real_wiimote = TakeWiimoteFromSlot(index);
  WiimoteCommon::UpdateSource(index);
}

// This is called from the GUI thread
void Refresh()
{
  if (!Config::Get(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING))
    s_wiimote_scanner.SetScanMode(WiimoteScanMode::SCAN_ONCE);
}

bool IsValidDeviceName(std::string_view name)
{
  return IsWiimoteName(name) || IsBalanceBoardName(name);
}

bool IsWiimoteName(std::string_view name)
{
  // Wii software similarly checks just the start of the name.
  return name.starts_with("Nintendo RVL-CNT");
}

bool IsBalanceBoardName(std::string_view name)
{
  // Wii software similarly checks just the start of the name.
  return name.starts_with("Nintendo RVL-WBC");
}

// This is called from the scanner backends (currently on the scanner thread).
bool IsNewWiimote(const std::string& identifier)
{
  quiesce_enter(&s_known_ids_quiesce);
  const auto* known = static_cast<const KnownIds*>(retro_atomic_load_acquire_ptr(&s_known_ids));
  const bool is_new = !known || !known->ids.contains(identifier);
  quiesce_exit(&s_known_ids_quiesce);
  return is_new;
}

bool IsKnownDeviceId(const USBUtils::DeviceInfo& device_info)
{
  return device_info.vid == 0x057e && (device_info.pid == 0x0306 || device_info.pid == 0x0330);
}

void HandleWiimoteSourceChange(unsigned int index)
{
  PostToManager(ManagerCommand::Kind::SourceChange, {}, index);
}

void HandleWiimotesInControllerInterfaceSettingChange()
{
  PostToManager(ManagerCommand::Kind::ProcessPool);
}

}  // namespace WiimoteReal
