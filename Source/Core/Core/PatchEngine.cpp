// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// PatchEngine
// Supports simple memory patches, and has a partial Action Replay implementation
// in ActionReplay.cpp/h.

#include "Core/PatchEngine.h"

#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/IniData.h"
#include "Common/IniFile.h"

#include "Core/ActionReplay.h"
#include "Core/CheatCodes.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

void ApplyMemoryPatch(u8* value, size_t size, const u32 address, bool store_existing_value)
{
  if (!size)
    return;

  if (!PowerPC::MMU::HostIsRAMAddress(address))
    return;

  auto& power_pc = Core::System::GetInstance().GetPowerPC();

  bool should_invalidate_cache = false;
  for (u32 offset = 0; offset < size; ++offset)
  {
    u8 old_value = PowerPC::MMU::HostRead<u8>(address + offset);
    if (old_value != value[offset])
    {
      PowerPC::MMU::HostWrite<u8>(value[offset], address + offset);
      should_invalidate_cache = true;
      if (store_existing_value)
        value[offset] = old_value;
    }

    if (((address + offset) % 4) == 3)
    {
      if (should_invalidate_cache)
        power_pc.ScheduleInvalidateCacheThreadSafe(Common::AlignDown(address + offset, 4));
      should_invalidate_cache = false;
    }
  }
  if (should_invalidate_cache)
  {
    power_pc.ScheduleInvalidateCacheThreadSafe(
        Common::AlignDown(address + static_cast<u32>(size) - 1, 4));
  }
}

namespace PatchEngine
{
enum
{
  PATCH_8BIT,
  PATCH_16BIT,
  PATCH_32BIT,
};

/* One write of a patch: with conditional, made only where the address holds comparand. */
struct PatchEntry
{
  u32 address;
  u32 value;
  u32 comparand;
  u8 type;
  u8 conditional;
};

/* An OnFrame patch of the game's INI files: its name, without the '$', and its writes. */
struct Patch
{
  char* name;
  PatchEntry* entries;
  u32 num_entries;
  u32 cap_entries;
  bool enabled;
};

static Patch* s_on_frame;
static u32 s_num_on_frame;
static u32 s_cap_on_frame;

static void PatchFree(Patch* patch)
{
  std::free(patch->name);
  std::free(patch->entries);
  std::memset(patch, 0, sizeof(*patch));
}

static void FreePatches()
{
  u32 i;
  for (i = 0; i < s_num_on_frame; i++)
    PatchFree(&s_on_frame[i]);
  std::free(s_on_frame);
  s_on_frame = nullptr;
  s_num_on_frame = 0;
  s_cap_on_frame = 0;
}

/* The patch goes behind the others, which own it from here; dropped when out of memory. */
static void PatchKeep(Patch* patch)
{
  if (s_num_on_frame == s_cap_on_frame)
  {
    const u32 cap = s_cap_on_frame ? s_cap_on_frame * 2 : 16;
    Patch* const grown = static_cast<Patch*>(std::realloc(s_on_frame, cap * sizeof(Patch)));
    if (!grown)
    {
      PatchFree(patch);
      return;
    }
    s_on_frame = grown;
    s_cap_on_frame = cap;
  }
  s_on_frame[s_num_on_frame++] = *patch;
  std::memset(patch, 0, sizeof(*patch));
}

/* A field of a line as an unsigned number in C's notation, the whole field being the number. */
static bool ParseField(const char* field, u32* out)
{
  char* end = nullptr;
  unsigned long long value;
  errno = 0;
  value = std::strtoull(field, &end, 0);
  if (!end || *end != '\0' || errno == ERANGE || value > 0xFFFFFFFFull)
    return false;
  *out = static_cast<u32>(value);
  return true;
}

/* A line "address:type:value[:comparand]", where the first ':' may be a '=', type being byte,
 * word or dword. The fields are split in a copy of the line. */
static bool DeserializeLine(const char* text, size_t len, PatchEntry* entry)
{
  char* const line = static_cast<char*>(std::malloc(len + 1));
  char* fields[4] = {};
  u32 num_fields = 0;
  size_t start = 0;
  bool parsed = false;

  if (!line)
    return false;
  std::memcpy(line, text, len);
  line[len] = '\0';
  {
    char* const equals = static_cast<char*>(std::memchr(line, '=', len));
    if (equals)
      *equals = ':';
  }
  while (start < len)
  {
    char* const colon = static_cast<char*>(std::memchr(line + start, ':', len - start));
    const size_t end = colon ? size_t(colon - line) : len;
    line[end] = '\0';
    if (num_fields < 4)
      fields[num_fields] = line + start;
    num_fields++;
    start = end + 1;
  }

  std::memset(entry, 0, sizeof(*entry));
  if (num_fields >= 3 && ParseField(fields[0], &entry->address) &&
      ParseField(fields[2], &entry->value) &&
      (num_fields < 4 || ParseField(fields[3], &entry->comparand)))
  {
    entry->conditional = num_fields >= 4;
    parsed = true;
    if (!std::strcmp(fields[1], "byte"))
      entry->type = PATCH_8BIT;
    else if (!std::strcmp(fields[1], "word"))
      entry->type = PATCH_16BIT;
    else if (!std::strcmp(fields[1], "dword"))
      entry->type = PATCH_32BIT;
    else
      parsed = false;
  }
  std::free(line);
  return parsed;
}

static void PatchAddEntry(Patch* patch, const PatchEntry* entry)
{
  if (patch->num_entries == patch->cap_entries)
  {
    const u32 cap = patch->cap_entries ? patch->cap_entries * 2 : 8;
    PatchEntry* const grown =
        static_cast<PatchEntry*>(std::realloc(patch->entries, cap * sizeof(PatchEntry)));
    if (!grown)
      return;
    patch->entries = grown;
    patch->cap_entries = cap;
  }
  patch->entries[patch->num_entries++] = *entry;
}

/* The OnFrame patches of one INI file, behind those already read. */
static void LoadPatchSection(const Common::IniFile& ini)
{
  const Common::IniFile::Section* const lines = ini.GetSection("OnFrame");
  Patch current = {};
  size_t i;

  for (i = 0; lines && i < lines->Data()->num_lines; i++)
  {
    const char* text;
    size_t len;
    if (!ini_section_line(lines->Data(), i, true, &text, &len) || !len)
      continue;

    if (text[0] == '$')
    {
      // Take care of the previous code
      if (current.name && current.name[0])
        PatchKeep(&current);
      current.num_entries = 0;

      std::free(current.name);
      current.name = static_cast<char*>(std::malloc(len));
      if (current.name)
      {
        std::memcpy(current.name, text + 1, len - 1);
        current.name[len - 1] = '\0';
      }
    }
    else
    {
      PatchEntry entry;
      if (DeserializeLine(text, len, &entry))
        PatchAddEntry(&current, &entry);
    }
  }

  if (current.name && current.name[0] && current.num_entries)
    PatchKeep(&current);
  PatchFree(&current);

  ReadEnabledAndDisabled(ini, "OnFrame", s_on_frame, s_num_on_frame, sizeof(Patch),
                         offsetof(Patch, name), offsetof(Patch, enabled));
}

void LoadPatches()
{
  const auto& sconfig = SConfig::GetInstance();
  Common::IniFile globalIni = sconfig.LoadDefaultGameIni();
  Common::IniFile localIni = sconfig.LoadLocalGameIni();
  u32 enabled_patch_count = 0;
  u32 i;

  LoadPatchSection(globalIni);
  LoadPatchSection(localIni);

  {
    Gecko::GeckoCodeList gecko_codes = {};
    Gecko::LoadCodes(globalIni, localIni, &gecko_codes);
    Gecko::SetActiveCodes(gecko_codes.codes, gecko_codes.count);
    Gecko::FreeCodeList(&gecko_codes);
  }
  ActionReplay::LoadAndApplyCodes(globalIni, localIni);

  for (i = 0; i < s_num_on_frame; i++)
    enabled_patch_count += s_on_frame[i].enabled ? 1 : 0;
  if (enabled_patch_count > 0)
    Core::DisplayMessage("%u game patch(es) enabled", enabled_patch_count);

  const size_t enabled_cheat_count = ActionReplay::CountEnabledCodes() + Gecko::CountEnabledCodes();
  if (enabled_cheat_count > 0)
    Core::DisplayMessage("%" PRIu64 " cheat(s) enabled", (u64)enabled_cheat_count);
}

static void ApplyPatches()
{
  u32 p;
  for (p = 0; p < s_num_on_frame; p++)
  {
    const Patch* const patch = &s_on_frame[p];
    u32 e;
    if (!patch->enabled)
      continue;
    for (e = 0; e < patch->num_entries; e++)
    {
      const PatchEntry* const entry = &patch->entries[e];
      const u32 addr = entry->address;
      const u32 value = entry->value;
      const u32 comparand = entry->comparand;
      switch (entry->type)
      {
      case PATCH_8BIT:
        if (!entry->conditional || PowerPC::MMU::HostRead<u8>(addr) == static_cast<u8>(comparand))
        {
          ApplyMemoryPatch<u8>(static_cast<u8>(value), addr);
        }
        break;
      case PATCH_16BIT:
        if (!entry->conditional || PowerPC::MMU::HostRead<u16>(addr) == static_cast<u16>(comparand))
        {
          ApplyMemoryPatch<u16>(static_cast<u16>(value), addr);
        }
        break;
      case PATCH_32BIT:
        if (!entry->conditional || PowerPC::MMU::HostRead<u32>(addr) == comparand)
          ApplyMemoryPatch<u32>(value, addr);
        break;
      default:
        break;
      }
    }
  }
}

// Requires MSR.DR, MSR.IR
// There's no perfect way to do this, it's just a heuristic.
// We require at least 2 stack frames, if the stack is shallower than that then it won't work.
static bool IsStackValid()
{
  const auto& ppc_state = Core::System::GetInstance().GetPPCState();

  DEBUG_ASSERT(ppc_state.msr.DR && ppc_state.msr.IR);

  // Check the stack pointer
  const u32 SP = ppc_state.gpr[1];
  if (!PowerPC::MMU::HostIsRAMAddress(SP))
    return false;

  // Read the frame pointer from the stack (find 2nd frame from top), assert that it makes sense
  const u32 next_SP = PowerPC::MMU::HostRead<u32>(SP);
  if (next_SP <= SP || !PowerPC::MMU::HostIsRAMAddress(next_SP) ||
      !PowerPC::MMU::HostIsRAMAddress(next_SP + 4))
  {
    return false;
  }

  // Check the link register makes sense (that it points to a valid IBAT address)
  const u32 address = PowerPC::MMU::HostRead<u32>(next_SP + 4);
  return PowerPC::MMU::HostIsInstructionRAMAddress(address) &&
         0 != PowerPC::MMU::HostRead_Instruction(address);
}

static void ApplyStartupPatches(Core::System& system)
{
  ASSERT(Core::IsCPUThread());

  const auto& ppc_state = system.GetPPCState();
  if (!ppc_state.msr.DR || !ppc_state.msr.IR)
  {
    DEBUG_LOG(ACTIONREPLAY,
              "Need to retry later. CPU configuration is currently incorrect. PC = %#010x, "
              "MSR = %#010x",
              ppc_state.pc, ppc_state.msr.Hex);
    return;
  }

  ApplyPatches();
}

/* Whether a field has anything to apply: an enabled OnFrame patch, a Gecko
 * code or an Action Replay code. Usually none. */
static bool HasFramePatches()
{
  u32 i;
  for (i = 0; i < s_num_on_frame; i++)
  {
    if (s_on_frame[i].enabled)
      return true;
  }
  return Gecko::CountEnabledCodes() != 0 || ActionReplay::CountEnabledCodes() != 0;
}

bool ApplyFramePatches(Core::System& system)
{
  // Nothing to apply: done, with no walk of the guest's stack, and no retry every 1000 cycles
  // when it is not in a state to walk (an exception vector, the FIFO player).
  if (!HasFramePatches())
    return true;

  const auto& ppc_state = system.GetPPCState();

  ASSERT(Core::IsCPUThread());

  // Because we're using the VI Interrupt to time this instead of patching the game with a
  // callback hook we can end up catching the game in an exception vector.
  // We deal with this by returning false so that SystemTimers will reschedule us in a few cycles
  // where we can try again after the CPU hopefully returns back to the normal instruction flow.
  if (!ppc_state.msr.DR || !ppc_state.msr.IR || !IsStackValid())
  {
    DEBUG_LOG(ACTIONREPLAY,
              "Need to retry later. CPU configuration is currently incorrect. PC = %#010x, "
              "MSR = %#010x",
              ppc_state.pc, ppc_state.msr.Hex);
    return false;
  }

  ApplyPatches();

  // Run the Gecko code handler
  Gecko::RunCodeHandler();
  ActionReplay::RunAllActive();

  return true;
}

void Shutdown()
{
  FreePatches();
  ActionReplay::ApplyCodes(nullptr, 0);
  Gecko::Shutdown();
}

void Reload(Core::System& system)
{
  Shutdown();
  LoadPatches();
  ApplyStartupPatches(system);
}

}  // namespace PatchEngine
