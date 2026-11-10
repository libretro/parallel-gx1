// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// PatchEngine
// Supports simple memory patches, and has a partial Action Replay implementation
// in ActionReplay.cpp/h.

#include "Core/PatchEngine.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/Align.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"

#include "Core/ActionReplay.h"
#include "Core/CheatCodes.h"
#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

void ApplyMemoryPatch(const Core::CPUThreadGuard& guard, std::span<u8> value, const u32 address,
                      bool store_existing_value)
{
  if (value.empty())
    return;

  const std::size_t size = value.size();
  if (!PowerPC::MMU::HostIsRAMAddress(guard, address))
    return;

  auto& power_pc = guard.GetSystem().GetPowerPC();

  bool should_invalidate_cache = false;
  for (u32 offset = 0; offset < size; ++offset)
  {
    u8 old_value = PowerPC::MMU::HostRead<u8>(guard, address + offset);
    if (old_value != value[offset])
    {
      PowerPC::MMU::HostWrite<u8>(guard, value[offset], address + offset);
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
constexpr std::array<const char*, 3> s_patch_type_strings{{
    "byte",
    "word",
    "dword",
}};

static std::vector<Patch> s_on_frame;

const char* PatchTypeAsString(PatchType type)
{
  return s_patch_type_strings.at(static_cast<int>(type));
}

std::optional<PatchEntry> DeserializeLine(std::string line)
{
  std::string::size_type loc = line.find('=');
  if (loc != std::string::npos)
    line[loc] = ':';

  const std::vector<std::string> items = SplitString(line, ':');
  PatchEntry entry;

  if (items.size() < 3)
    return std::nullopt;

  if (!TryParse(items[0], &entry.address))
    return std::nullopt;
  if (!TryParse(items[2], &entry.value))
    return std::nullopt;

  if (items.size() >= 4)
  {
    if (!TryParse(items[3], &entry.comparand))
      return std::nullopt;
    entry.conditional = true;
  }

  const auto iter = std::ranges::find(s_patch_type_strings, items[1]);
  if (iter == s_patch_type_strings.end())
    return std::nullopt;
  entry.type = static_cast<PatchType>(std::distance(s_patch_type_strings.begin(), iter));

  return entry;
}

std::string SerializeLine(const PatchEntry& entry)
{
  if (entry.conditional)
  {
    return fmt::format("0x{:08X}:{}:0x{:08X}:0x{:08X}", entry.address,
                       PatchEngine::PatchTypeAsString(entry.type), entry.value, entry.comparand);
  }
  else
  {
    return fmt::format("0x{:08X}:{}:0x{:08X}", entry.address,
                       PatchEngine::PatchTypeAsString(entry.type), entry.value);
  }
}

void LoadPatchSection(const std::string& section, std::vector<Patch>* patches,
                      const Common::IniFile& globalIni, const Common::IniFile& localIni)
{
  for (const auto* ini : {&globalIni, &localIni})
  {
    std::vector<std::string> lines;
    Patch currentPatch;
    ini->GetLines(section, &lines);

    for (std::string& line : lines)
    {
      if (line.empty())
        continue;

      if (line[0] == '$')
      {
        // Take care of the previous code
        if (!currentPatch.name.empty())
        {
          patches->push_back(currentPatch);
        }
        currentPatch.entries.clear();

        // Set name and whether the patch is user defined
        currentPatch.name = line.substr(1, line.size() - 1);
        currentPatch.user_defined = (ini == &localIni);
      }
      else
      {
        if (std::optional<PatchEntry> entry = DeserializeLine(line))
          currentPatch.entries.push_back(*entry);
      }
    }

    if (!currentPatch.name.empty() && !currentPatch.entries.empty())
    {
      patches->push_back(currentPatch);
    }

    ReadEnabledAndDisabled(*ini, section, patches);

    if (ini == &globalIni)
    {
      for (Patch& patch : *patches)
        patch.default_enabled = patch.enabled;
    }
  }
}

void SavePatchSection(Common::IniFile* local_ini, std::span<const Patch> patches)
{
  std::vector<std::string> lines;
  std::vector<std::string> lines_enabled;
  std::vector<std::string> lines_disabled;

  for (const auto& patch : patches)
  {
    if (patch.enabled != patch.default_enabled)
      (patch.enabled ? lines_enabled : lines_disabled).emplace_back('$' + patch.name);

    if (!patch.user_defined)
      continue;

    lines.emplace_back('$' + patch.name);

    for (const PatchEntry& entry : patch.entries)
      lines.emplace_back(SerializeLine(entry));
  }

  local_ini->SetLines("OnFrame_Enabled", lines_enabled);
  local_ini->SetLines("OnFrame_Disabled", lines_disabled);
  local_ini->SetLines("OnFrame", lines);
}

void LoadPatches()
{
  const auto& sconfig = SConfig::GetInstance();
  Common::IniFile merged = sconfig.LoadGameIni();
  Common::IniFile globalIni = sconfig.LoadDefaultGameIni();
  Common::IniFile localIni = sconfig.LoadLocalGameIni();

  LoadPatchSection("OnFrame", &s_on_frame, globalIni, localIni);


  // Check if I'm syncing Codes
  if (Config::Get(Config::SESSION_CODE_SYNC_OVERRIDE))
  {
    Gecko::SetSyncedCodesAsActive();
    ActionReplay::SetSyncedCodesAsActive();
  }
  else
  {
    Gecko::SetActiveCodes(Gecko::LoadCodes(globalIni, localIni), sconfig.GetGameID(),
                          sconfig.GetRevision());
    ActionReplay::LoadAndApplyCodes(globalIni, localIni, sconfig.GetGameID(),
                                    sconfig.GetRevision());
  }

  const size_t enabled_patch_count =
      std::ranges::count_if(s_on_frame, [](const Patch& patch) { return patch.enabled; });
  if (enabled_patch_count > 0)
  {
    Core::DisplayMessage(fmt::format("{} game patch(es) enabled", enabled_patch_count));
  }

  const size_t enabled_cheat_count = ActionReplay::CountEnabledCodes() + Gecko::CountEnabledCodes();
  if (enabled_cheat_count > 0)
    Core::DisplayMessage(fmt::format("{} cheat(s) enabled", enabled_cheat_count));
}

static void ApplyPatches(const Core::CPUThreadGuard& guard, const std::vector<Patch>& patches)
{
  for (const Patch& patch : patches)
  {
    if (patch.enabled)
    {
      for (const PatchEntry& entry : patch.entries)
      {
        u32 addr = entry.address;
        u32 value = entry.value;
        u32 comparand = entry.comparand;
        switch (entry.type)
        {
        case PatchType::Patch8Bit:
          if (!entry.conditional ||
              PowerPC::MMU::HostRead<u8>(guard, addr) == static_cast<u8>(comparand))
          {
            ApplyMemoryPatch<u8>(guard, static_cast<u8>(value), addr);
          }
          break;
        case PatchType::Patch16Bit:
          if (!entry.conditional ||
              PowerPC::MMU::HostRead<u16>(guard, addr) == static_cast<u16>(comparand))
          {
            ApplyMemoryPatch<u16>(guard, static_cast<u16>(value), addr);
          }
          break;
        case PatchType::Patch32Bit:
          if (!entry.conditional || PowerPC::MMU::HostRead<u32>(guard, addr) == comparand)
            ApplyMemoryPatch<u32>(guard, value, addr);
          break;
        default:
          // unknown patchtype
          break;
        }
      }
    }
  }
}

// Requires MSR.DR, MSR.IR
// There's no perfect way to do this, it's just a heuristic.
// We require at least 2 stack frames, if the stack is shallower than that then it won't work.
static bool IsStackValid(const Core::CPUThreadGuard& guard)
{
  const auto& ppc_state = guard.GetSystem().GetPPCState();

  DEBUG_ASSERT(ppc_state.msr.DR && ppc_state.msr.IR);

  // Check the stack pointer
  const u32 SP = ppc_state.gpr[1];
  if (!PowerPC::MMU::HostIsRAMAddress(guard, SP))
    return false;

  // Read the frame pointer from the stack (find 2nd frame from top), assert that it makes sense
  const u32 next_SP = PowerPC::MMU::HostRead<u32>(guard, SP);
  if (next_SP <= SP || !PowerPC::MMU::HostIsRAMAddress(guard, next_SP) ||
      !PowerPC::MMU::HostIsRAMAddress(guard, next_SP + 4))
  {
    return false;
  }

  // Check the link register makes sense (that it points to a valid IBAT address)
  const u32 address = PowerPC::MMU::HostRead<u32>(guard, next_SP + 4);
  return PowerPC::MMU::HostIsInstructionRAMAddress(guard, address) &&
         0 != PowerPC::MMU::HostRead_Instruction(guard, address);
}

static void ApplyStartupPatches(Core::System& system)
{
  ASSERT(Core::IsCPUThread());
  Core::CPUThreadGuard guard(system);

  const auto& ppc_state = system.GetPPCState();
  if (!ppc_state.msr.DR || !ppc_state.msr.IR)
  {
    DEBUG_LOG_FMT(ACTIONREPLAY,
                  "Need to retry later. CPU configuration is currently incorrect. PC = {:#010x}, "
                  "MSR = {:#010x}",
                  ppc_state.pc, ppc_state.msr.Hex);
    return;
  }

  ApplyPatches(guard, s_on_frame);
}

/* Whether a field has anything to apply: an enabled OnFrame patch, a Gecko
 * code or an Action Replay code. Usually none. */
static bool HasFramePatches()
{
  for (const Patch& patch : s_on_frame)
  {
    if (patch.enabled)
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
  Core::CPUThreadGuard guard(system);

  // Because we're using the VI Interrupt to time this instead of patching the game with a
  // callback hook we can end up catching the game in an exception vector.
  // We deal with this by returning false so that SystemTimers will reschedule us in a few cycles
  // where we can try again after the CPU hopefully returns back to the normal instruction flow.
  if (!ppc_state.msr.DR || !ppc_state.msr.IR || !IsStackValid(guard))
  {
    DEBUG_LOG_FMT(ACTIONREPLAY,
                  "Need to retry later. CPU configuration is currently incorrect. PC = {:#010x}, "
                  "MSR = {:#010x}",
                  ppc_state.pc, ppc_state.msr.Hex);
    return false;
  }

  ApplyPatches(guard, s_on_frame);

  // Run the Gecko code handler
  Gecko::RunCodeHandler(guard);
  ActionReplay::RunAllActive(guard);

  return true;
}

void Shutdown()
{
  s_on_frame.clear();
  ActionReplay::ApplyCodes({}, "", 0);
  Gecko::Shutdown();
}

void Reload(Core::System& system)
{
  Shutdown();
  LoadPatches();
  ApplyStartupPatches(system);
}

}  // namespace PatchEngine
