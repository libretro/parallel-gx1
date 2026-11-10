// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CoreTiming.h"

#include <stdlib.h>
#include <string.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"

#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "VideoCommon/Fifo.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

namespace CoreTiming
{
static constexpr int MAX_SLICE_LENGTH = 20000;

static void EmptyTimedCallback(Core::System& system, u64 userdata, s64 cyclesLate)
{
}

/* the queued events the timing starts with room for: the hardware's, a few each */
#define CORE_TIMING_FIRST_EVENTS 64

CoreTimingManager::CoreTimingManager(Core::System& system) : m_system(system)
{
  mpsc_stack_init(&m_ts_queue);
}

/* Earlier: sooner, or as soon and queued first. */
static bool EventBefore(const Event& a, const Event& b)
{
  return a.time < b.time || (a.time == b.time && a.fifo_order < b.fifo_order);
}

bool CoreTimingManager::ReserveEvents(u32 count)
{
  u32 capacity;
  Event* grown;
  if (count <= m_event_capacity)
    return true;
  capacity = m_event_capacity ? m_event_capacity : CORE_TIMING_FIRST_EVENTS;
  while (capacity < count)
    capacity *= 2;
  grown = static_cast<Event*>(realloc(m_event_queue, capacity * sizeof(Event)));
  if (!grown)
    return false;
  m_event_queue = grown;
  m_event_capacity = capacity;
  return true;
}

/* Up from the end to where it is no earlier than its parent. */
void CoreTimingManager::PushEvent(const Event& ev)
{
  u32 i;
  if (!ReserveEvents(m_num_events + 1))
  {
    ERROR_LOG_FMT(POWERPC, "Lost a \"{}\" event: out of memory", ev.type->name);
    return;
  }
  i = m_num_events++;
  while (i > 0)
  {
    const u32 parent = (i - 1) / 2;
    if (!EventBefore(ev, m_event_queue[parent]))
      break;
    m_event_queue[i] = m_event_queue[parent];
    i = parent;
  }
  m_event_queue[i] = ev;
}

/* The earliest out: the last goes down from the top. */
void CoreTimingManager::PopEvent()
{
  Event last;
  u32 i = 0;
  if (--m_num_events == 0)
    return;
  last = m_event_queue[m_num_events];
  for (;;)
  {
    u32 child = 2 * i + 1;
    if (child >= m_num_events)
      break;
    if (child + 1 < m_num_events && EventBefore(m_event_queue[child + 1], m_event_queue[child]))
      child++;
    if (!EventBefore(m_event_queue[child], last))
      break;
    m_event_queue[i] = m_event_queue[child];
    i = child;
  }
  m_event_queue[i] = last;
}

/* The heap order, from any: each parent down from the last. */
void CoreTimingManager::MakeHeap()
{
  u32 start = m_num_events / 2;
  while (start-- > 0)
  {
    const Event ev = m_event_queue[start];
    u32 i = start;
    for (;;)
    {
      u32 child = 2 * i + 1;
      if (child >= m_num_events)
        break;
      if (child + 1 < m_num_events && EventBefore(m_event_queue[child + 1], m_event_queue[child]))
        child++;
      if (!EventBefore(m_event_queue[child], ev))
        break;
      m_event_queue[i] = m_event_queue[child];
      i = child;
    }
    m_event_queue[i] = ev;
  }
}

EventType* CoreTimingManager::FindEventType(const char* name)
{
  u32 i;
  for (i = 0; i < m_num_event_types; i++)
  {
    if (strcmp(m_event_types[i].name, name) == 0)
      return &m_event_types[i];
  }
  return nullptr;
}

// Changing the CPU speed in Dolphin isn't actually done by changing the physical clock rate,
// but by changing the amount of work done in a particular amount of time. This tends to be more
// compatible because it stops the games from actually knowing directly that the clock rate has
// changed, and ensures that anything based on waiting a specific number of cycles still works.
//
// Technically it might be more accurate to call this changing the IPC instead of the CPU speed,
// but the effect is largely the same.
int CoreTimingManager::DowncountToCycles(int downcount) const
{
  return static_cast<int>(downcount * m_globals.last_OC_factor_inverted);
}

int CoreTimingManager::CyclesToDowncount(int cycles) const
{
  return static_cast<int>(cycles * m_last_oc_factor);
}

EventType* CoreTimingManager::RegisterEvent(const char* name, TimedCallback callback)
{
  EventType* event_type;
  // we want event type names to remain unique so that we can use them for serialization.
  ASSERT_MSG(POWERPC, !FindEventType(name),
             "CoreTiming Event \"{}\" is already registered. Events should only be registered "
             "during Init to avoid breaking save states.",
             name);
  ASSERT_MSG(POWERPC, strlen(name) < CORE_TIMING_NAME_SIZE, "CoreTiming Event \"{}\": name too long",
             name);
  ASSERT_MSG(POWERPC, m_num_event_types < MAX_EVENT_TYPES, "Too many CoreTiming Events");
  if (m_num_event_types == MAX_EVENT_TYPES)
    return m_ev_lost;
  event_type = &m_event_types[m_num_event_types++];
  event_type->callback = callback;
  strncpy(event_type->name, name, CORE_TIMING_NAME_SIZE - 1);
  event_type->name[CORE_TIMING_NAME_SIZE - 1] = '\0';
  return event_type;
}

void CoreTimingManager::UnregisterAllEvents()
{
  ASSERT_MSG(POWERPC, m_num_events == 0, "Cannot unregister events with events pending");
  m_num_event_types = 0;
}

void CoreTimingManager::Init()
{
  m_system.GetPPCState().downcount = CyclesToDowncount(MAX_SLICE_LENGTH);
  m_globals.slice_length = MAX_SLICE_LENGTH;
  m_globals.global_timer = 0;
  m_idled_cycles = 0;

  // The time between CoreTiming being initialized and the first call to Advance() is considered
  // the slice boundary between slice -1 and slice 0. Dispatcher loops must call Advance() before
  // executing the first PPC cycle of each slice to prepare the slice length and downcount for
  // that slice.
  m_is_global_timer_sane = true;

  m_event_fifo_id = 0;
  m_num_events = 0;
  ReserveEvents(CORE_TIMING_FIRST_EVENTS);
  if (!m_ts_pool)
    m_ts_pool = object_pool_create(sizeof(ThreadSafeEvent), 64);
  m_ev_lost = RegisterEvent("_lost_event", &EmptyTimedCallback);

  m_registered_config_callback_id =
      CPUThreadConfigCallback::AddConfigChangedCallback([this]() { RefreshConfig(); });
  RefreshConfig();

  m_last_oc_factor = m_config_oc_factor;
  m_globals.last_OC_factor_inverted = m_config_oc_inv_factor;
}

CoreTimingManager::~CoreTimingManager()
{
  mpsc_stack_node_t* node = mpsc_stack_drain(&m_ts_queue);
  while (node)
  {
    mpsc_stack_node_t* const next = node->next;
    object_pool_free(node);
    node = next;
  }
  object_pool_destroy(m_ts_pool);
  free(m_event_queue);
}

void CoreTimingManager::Shutdown()
{
  MoveEvents();
  ClearPendingEvents();
  UnregisterAllEvents();
  CPUThreadConfigCallback::RemoveConfigChangedCallback(m_registered_config_callback_id);
}

void CoreTimingManager::RefreshConfig()
{
  m_config_oc_factor =
      (Config::Get(Config::MAIN_OVERCLOCK_ENABLE) ? Config::Get(Config::MAIN_OVERCLOCK) : 1.0f) *
      (Config::Get(Config::MAIN_VI_OVERCLOCK_ENABLE) ? Config::Get(Config::MAIN_VI_OVERCLOCK) :
                                                       1.0f);
  m_config_oc_inv_factor = 1.0f / m_config_oc_factor;
  m_config_sync_on_skip_idle = Config::Get(Config::MAIN_SYNC_ON_SKIP_IDLE);
}

void CoreTimingManager::DoState(PointerWrap& p)
{
  p.Do(m_globals.slice_length);
  p.Do(m_globals.global_timer);
  p.Do(m_idled_cycles);
  p.Do(m_fake_dec_start_value);
  p.Do(m_fake_dec_start_ticks);
  p.Do(m_globals.fake_TB_start_value);
  p.Do(m_globals.fake_TB_start_ticks);
  p.Do(m_last_oc_factor);
  m_globals.last_OC_factor_inverted = 1.0f / m_last_oc_factor;
  p.Do(m_event_fifo_id);

  p.DoMarker("CoreTimingData");

  MoveEvents();
  // a count, then each event: its time, order, userdata and its type's name (a count and the
  // characters), as the events are not registered the same way every time
  u32 count = m_num_events;
  p.Do(count);
  if (p.IsReadMode() && !ReserveEvents(count))
  {
    p.SetMeasureMode();
    return;
  }
  m_num_events = count;
  for (u32 i = 0; i < count; i++)
  {
    Event& ev = m_event_queue[i];
    char name[CORE_TIMING_NAME_SIZE];
    u32 name_len = p.IsReadMode() ? 0 : static_cast<u32>(strlen(ev.type->name));
    p.Do(ev.time);
    p.Do(ev.fifo_order);
    // this is why we can't have (nice things) pointers as userdata
    p.Do(ev.userdata);
    p.Do(name_len);
    if (!p.IsReadMode())
    {
      p.DoArray(ev.type->name, name_len);
      continue;
    }
    if (name_len >= CORE_TIMING_NAME_SIZE)
    {
      // no registered name is as long: skip it
      static_cast<void>(p.DoExternal(name_len));
      ev.type = m_ev_lost;
      WARN_LOG_FMT(POWERPC, "Lost event from savestate: its type's name is too long");
      continue;
    }
    p.DoArray(name, name_len);
    name[name_len] = '\0';
    ev.type = FindEventType(name);
    if (!ev.type)
    {
      WARN_LOG_FMT(POWERPC,
                   "Lost event from savestate because its type, \"{}\", has not been registered.",
                   name);
      ev.type = m_ev_lost;
    }
  }
  p.DoMarker("CoreTimingEvents");

  if (p.IsReadMode())
  {
    // When loading from a save state, we must assume the Event order is random and meaningless:
    // the layout of the heap is the saver's.
    MakeHeap();
  }
}

// This should only be called from the CPU thread. If you are calling
// it from any other thread, you are doing something evil
u64 CoreTimingManager::GetTicks() const
{
  u64 ticks = static_cast<u64>(m_globals.global_timer);
  if (!m_is_global_timer_sane)
  {
    int downcount = DowncountToCycles(m_system.GetPPCState().downcount);
    ticks += m_globals.slice_length - downcount;
  }
  return ticks;
}

u64 CoreTimingManager::GetProcessedTicks() const
{
  return m_is_global_timer_sane ? m_dispatch_time : static_cast<u64>(m_globals.global_timer);
}

void CoreTimingManager::ClearPendingEvents()
{
  m_num_events = 0;
}

void CoreTimingManager::ScheduleEvent(s64 cycles_into_future, EventType* event_type, u64 userdata,
                                      FromThread from)
{
  ASSERT_MSG(POWERPC, event_type, "Event type is nullptr, will crash now.");

  bool from_cpu_thread;
  if (from == FromThread::ANY)
  {
    from_cpu_thread = Core::IsCPUThread();
  }
  else
  {
    from_cpu_thread = from == FromThread::CPU;
    ASSERT_MSG(POWERPC, from_cpu_thread == Core::IsCPUThread(),
               "A \"{}\" event was scheduled from the wrong thread ({})", event_type->name,
               from_cpu_thread ? "CPU" : "non-CPU");
  }

  if (from_cpu_thread)
  {
    s64 timeout = GetTicks() + cycles_into_future;

    // If this event needs to be scheduled before the next advance(), force one early
    if (!m_is_global_timer_sane)
      ForceExceptionCheck(cycles_into_future);

    const Event ev = {timeout, m_event_fifo_id++, userdata, event_type};
    PushEvent(ev);
  }
  else
  {
    if (Core::WantsDeterminism())
    {
      ERROR_LOG_FMT(POWERPC,
                    "Someone scheduled an off-thread \"{}\" event while netplay or "
                    "movie play/record was active.  This is likely to cause a desync.",
                    event_type->name);
    }

    ThreadSafeEvent* const node =
        static_cast<ThreadSafeEvent*>(object_pool_alloc(m_ts_pool, sizeof(ThreadSafeEvent)));
    if (!node)
    {
      ERROR_LOG_FMT(POWERPC, "Lost a \"{}\" event: out of memory", event_type->name);
      return;
    }
    node->link.next = nullptr;
    node->event.time = cycles_into_future;
    node->event.fifo_order = 0;
    node->event.userdata = userdata;
    node->event.type = event_type;
    mpsc_stack_push(&m_ts_queue, &node->link);
  }
}

void CoreTimingManager::RemoveEvent(EventType* event_type)
{
  u32 i, kept = 0;
  for (i = 0; i < m_num_events; i++)
  {
    if (m_event_queue[i].type != event_type)
      m_event_queue[kept++] = m_event_queue[i];
  }

  // Removing random items breaks the invariant so we have to re-establish it.
  if (kept != m_num_events)
  {
    m_num_events = kept;
    MakeHeap();
  }
}

void CoreTimingManager::RemoveAllEvents(EventType* event_type)
{
  MoveEvents();
  RemoveEvent(event_type);
}

void CoreTimingManager::ForceExceptionCheck(s64 cycles)
{
  if (cycles < 0)
    cycles = 0;
  auto& ppc_state = m_system.GetPPCState();
  if (DowncountToCycles(ppc_state.downcount) > cycles)
  {
    // downcount is always (much) smaller than MAX_INT so we can safely cast cycles to an int here.
    // Account for cycles already executed by adjusting the m_globals.slice_length
    m_globals.slice_length -= DowncountToCycles(ppc_state.downcount) - static_cast<int>(cycles);
    ppc_state.downcount = CyclesToDowncount(static_cast<int>(cycles));
  }
}

void CoreTimingManager::MoveEvents()
{
  if (mpsc_stack_empty(&m_ts_queue))
    return;

  // Oldest first, so events keep the order they were scheduled in.
  mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&m_ts_queue));
  while (node)
  {
    ThreadSafeEvent* const ts_event = reinterpret_cast<ThreadSafeEvent*>(node);
    Event ev = ts_event->event;
    node = node->next;
    object_pool_free(ts_event);

    ev.fifo_order = m_event_fifo_id++;
    ev.time += m_globals.global_timer;
    PushEvent(ev);
  }
}

void CoreTimingManager::Advance()
{
  CPUThreadConfigCallback::CheckForConfigChanges();

  MoveEvents();

  auto& power_pc = m_system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();

  int cyclesExecuted = m_globals.slice_length - DowncountToCycles(ppc_state.downcount);
  m_globals.global_timer += cyclesExecuted;
  m_last_oc_factor = m_config_oc_factor;
  m_globals.last_OC_factor_inverted = m_config_oc_inv_factor;
  m_globals.slice_length = MAX_SLICE_LENGTH;

  m_is_global_timer_sane = true;

  while (m_num_events && m_event_queue[0].time <= m_globals.global_timer)
  {
    const Event evt = m_event_queue[0];
    PopEvent();
    m_dispatch_time = static_cast<u64>(evt.time);
    evt.type->callback(m_system, evt.userdata, m_globals.global_timer - evt.time);
  }

  m_is_global_timer_sane = false;

  // Still events left (scheduled in the future)
  if (m_num_events)
  {
    const s64 until = m_event_queue[0].time - m_globals.global_timer;
    m_globals.slice_length = static_cast<int>(until < MAX_SLICE_LENGTH ? until : MAX_SLICE_LENGTH);
  }

  ppc_state.downcount = CyclesToDowncount(m_globals.slice_length);

  // Check for any external exceptions.
  // It's important to do this after processing events otherwise any exceptions will be delayed
  // until the next slice:
  //        Pokemon Box refuses to boot if the first exception from the audio DMA is received late
  power_pc.CheckExternalExceptions();
}

float CoreTimingManager::GetOverclock() const
{
  return m_config_oc_factor;
}

bool CoreTimingManager::UseSyncOnSkipIdle() const
{
  return m_config_sync_on_skip_idle;
}

// Should only be called from the CPU thread after the PPC clock has changed
void CoreTimingManager::AdjustEventQueueTimes(u32 new_ppc_clock, u32 old_ppc_clock)
{
  const s64 ticks = m_globals.global_timer;

  u32 i;
  for (i = 0; i < m_num_events; i++)
  {
    Event& ev = m_event_queue[i];
    const s64 ev_ticks = (ev.time - ticks) * new_ppc_clock / old_ppc_clock;
    ev.time = ticks + ev_ticks;
  }
}

void CoreTimingManager::Idle()
{
  if (m_config_sync_on_skip_idle)
  {
    // When the FIFO is processing data we must not advance because in this way
    // the VI will be desynchronized. So, We are waiting until the FIFO finish and
    // while we process only the events required by the FIFO.
    m_system.GetFifo().FlushGpu();
  }

  auto& ppc_state = m_system.GetPPCState();
  PowerPC::UpdatePerformanceMonitor(ppc_state.downcount, 0, 0, ppc_state);
  m_idled_cycles += DowncountToCycles(ppc_state.downcount);
  ppc_state.downcount = 0;
}

u32 CoreTimingManager::GetFakeDecStartValue() const
{
  return m_fake_dec_start_value;
}

void CoreTimingManager::SetFakeDecStartValue(u32 val)
{
  m_fake_dec_start_value = val;
}

u64 CoreTimingManager::GetFakeDecStartTicks() const
{
  return m_fake_dec_start_ticks;
}

void CoreTimingManager::SetFakeDecStartTicks(u64 val)
{
  m_fake_dec_start_ticks = val;
}

u64 CoreTimingManager::GetFakeTBStartValue() const
{
  return m_globals.fake_TB_start_value;
}

void CoreTimingManager::SetFakeTBStartValue(u64 val)
{
  m_globals.fake_TB_start_value = val;
}

u64 CoreTimingManager::GetFakeTBStartTicks() const
{
  return m_globals.fake_TB_start_ticks;
}

void CoreTimingManager::SetFakeTBStartTicks(u64 val)
{
  m_globals.fake_TB_start_ticks = val;
}

void GlobalAdvance()
{
  Core::System::GetInstance().GetCoreTiming().Advance();
}

void GlobalIdle()
{
  Core::System::GetInstance().GetCoreTiming().Idle();
}

}  // namespace CoreTiming
