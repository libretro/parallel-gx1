// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <retro_miscellaneous.h>

#include "Common/CommonTypes.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HLE/HLE_Misc.h"
#include "Core/HLE/HLE_OS.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace HLE
{
// The hooked addresses and their HLE hook indexes, sorted by address
struct HookedAddress
{
  u32 address;
  u32 index;
};
static HookedAddress* s_hooked;
static u32 s_hooked_count;
static u32 s_hooked_capacity;

// The first slot whose address is not below address
static u32 FindHookSlot(u32 address)
{
  u32 low = 0;
  u32 high = s_hooked_count;
  while (low < high)
  {
    const u32 mid = low + (high - low) / 2;
    if (s_hooked[mid].address < address)
      low = mid + 1;
    else
      high = mid;
  }
  return low;
}

static void SetHook(u32 address, u32 index)
{
  const u32 slot = FindHookSlot(address);
  if (slot < s_hooked_count && s_hooked[slot].address == address)
  {
    s_hooked[slot].index = index;
    return;
  }
  if (s_hooked_count == s_hooked_capacity)
  {
    const u32 capacity = s_hooked_capacity ? s_hooked_capacity * 2 : 256;
    HookedAddress* const grown =
        static_cast<HookedAddress*>(std::realloc(s_hooked, capacity * sizeof(HookedAddress)));
    if (!grown)
      return;
    s_hooked = grown;
    s_hooked_capacity = capacity;
  }
  std::memmove(s_hooked + slot + 1, s_hooked + slot,
               (s_hooked_count - slot) * sizeof(HookedAddress));
  s_hooked[slot].address = address;
  s_hooked[slot].index = index;
  s_hooked_count++;
}

static void RemoveHookSlot(u32 slot)
{
  std::memmove(s_hooked + slot, s_hooked + slot + 1,
               (s_hooked_count - slot - 1) * sizeof(HookedAddress));
  s_hooked_count--;
}

// clang-format off
constexpr Hook os_patches[23]{
    // Placeholder, os_patches[0] is the "non-existent function" index
    {"FAKE_TO_SKIP_0",               HLE_Misc::UnimplementedFunction,       HookType::Replace, HookFlag::Generic},

    // Name doesn't matter, installed in CBoot::BootUp()
    {"HBReload",                     HLE_Misc::HBReload,                    HookType::Replace, HookFlag::Fixed},

    // Debug/OS Support
    {"OSPanic",                      HLE_OS::HLE_OSPanic,                   HookType::Replace, HookFlag::Debug},

    // This needs to be put before vprintf (because vprintf is called indirectly by this)
    {"JUTWarningConsole_f",          HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},

    {"OSReport",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"DEBUGPrint",                   HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"WUD_DEBUGPrint",               HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"__DSP_debug_printf",           HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"vprintf",                      HLE_OS::HLE_GeneralDebugVPrint,        HookType::Start,   HookFlag::Debug},
    {"printf",                       HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"vdprintf",                     HLE_OS::HLE_LogVDPrint,                HookType::Start,   HookFlag::Debug},
    {"dprintf",                      HLE_OS::HLE_LogDPrint,                 HookType::Start,   HookFlag::Debug},
    {"vfprintf",                     HLE_OS::HLE_LogVFPrint,                HookType::Start,   HookFlag::Debug},
    {"fprintf",                      HLE_OS::HLE_LogFPrint,                 HookType::Start,   HookFlag::Debug},
    {"nlPrintf",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"DWC_Printf",                   HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"RANK_Printf",                  HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"puts",                         HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug}, // gcc-optimized printf?
    {"___blank",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug}, // used for early init things (normally)
    {"__write_console",              HLE_OS::HLE_write_console,             HookType::Start,   HookFlag::Debug}, // used by sysmenu (+more?)

    {"GeckoCodehandler",             HLE_Misc::GeckoCodeHandlerICacheFlush, HookType::Start,   HookFlag::Fixed},
    {"GeckoHandlerReturnTrampoline", HLE_Misc::GeckoReturnTrampoline,       HookType::Replace, HookFlag::Fixed},
    {"AppLoaderReport",              HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Fixed} // apploader needs OSReport-like function
};
// clang-format on

void Patch(Core::System& system, u32 addr, std::string_view func_name)
{
  auto& ppc_state = system.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();
  for (u32 i = 1; i < ARRAY_SIZE(os_patches); ++i)
  {
    if (os_patches[i].name == func_name)
    {
      SetHook(addr, i);
      ppc_state.iCache.Invalidate(memory, jit_interface, addr);
      return;
    }
  }
}

void PatchFixedFunctions(Core::System& system)
{
  // MIOS puts patch data in low MEM1 (0x1800-0x3000) for its own use.
  // Overwriting data in this range can cause the IPL to crash when launching games
  // that get patched by MIOS. See https://bugs.dolphin-emu.org/issues/11952 for more info.
  // Not applying the Gecko HLE patches means that Gecko codes will not work under MIOS,
  // but this is better than the alternative of having specific games crash.
  if (system.IsMIOS())
    return;

  // HLE jump to loader (homebrew).  Disabled when Gecko is active as it interferes with the code
  // handler
  if (!Config::AreCheatsEnabled())
  {
    Patch(system, 0x80001800, "HBReload");
    auto& memory = system.GetMemory();
    memory.CopyToEmu(0x00001804, "STUBHAXX", 8);
  }

  // Not part of the binary itself, but either we or Gecko OS might insert
  // this, and it doesn't clear the icache properly.
  Patch(system, Gecko::ENTRY_POINT, "GeckoCodehandler");
  // This has to always be installed even if cheats are not enabled because of the possibility of
  // loading a savestate where PC is inside the code handler while cheats are disabled.
  Patch(system, Gecko::HLE_TRAMPOLINE_ADDRESS, "GeckoHandlerReturnTrampoline");
}

void PatchFunctions(Core::System& system)
{
  auto& power_pc = system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();
  auto& ppc_symbol_db = power_pc.GetSymbolDB();

  // Remove all hooks that aren't fixed address hooks
  u32 kept = 0;
  for (u32 i = 0; i < s_hooked_count; i++)
  {
    if (os_patches[s_hooked[i].index].flags != HookFlag::Fixed)
      ppc_state.iCache.Invalidate(memory, jit_interface, s_hooked[i].address);
    else
      s_hooked[kept++] = s_hooked[i];
  }
  s_hooked_count = kept;

  for (u32 i = 1; i < ARRAY_SIZE(os_patches); ++i)
  {
    // Fixed hooks don't map to symbols
    if (os_patches[i].flags == HookFlag::Fixed)
      continue;

    for (const auto& symbol : ppc_symbol_db.GetSymbolsFromName(os_patches[i].name))
    {
      for (u32 addr = symbol->address; addr < symbol->address + symbol->size; addr += 4)
      {
        SetHook(addr, i);
        ppc_state.iCache.Invalidate(memory, jit_interface, addr);
      }
      INFO_LOG(OSHLE, "Patching %s %08x", os_patches[i].name, symbol->address);
    }
  }

}

void Clear()
{
  std::free(s_hooked);
  s_hooked = nullptr;
  s_hooked_count = 0;
  s_hooked_capacity = 0;
}

void Reload(Core::System& system)
{
  Clear();
  PatchFixedFunctions(system);
  PatchFunctions(system);
}

void Execute(u32 current_pc, u32 hook_index)
{
  hook_index &= 0xFFFFF;
  if (hook_index > 0 && hook_index < ARRAY_SIZE(os_patches))
  {
    os_patches[hook_index].function();
  }
  else
  {
    PanicAlert("HLE system tried to call an undefined HLE function %u.", hook_index);
  }
}

void ExecuteFromJIT(u32 current_pc, u32 hook_index, Core::System& system)
{
  ASSERT(Core::IsCPUThread());
  Execute(current_pc, hook_index);
}

u32 GetHookByAddress(u32 address)
{
  const u32 slot = FindHookSlot(address);
  return (slot < s_hooked_count && s_hooked[slot].address == address) ? s_hooked[slot].index : 0;
}

u32 GetHookByFunctionAddress(PPCSymbolDB& ppc_symbol_db, u32 address)
{
  const u32 index = GetHookByAddress(address);
  // Fixed hooks use a fixed address and don't patch the whole function
  if (index == 0 || os_patches[index].flags == HookFlag::Fixed)
    return index;

  const Common::Symbol* const symbol = ppc_symbol_db.GetSymbolFromAddr(address);
  return (symbol && symbol->address == address) ? index : 0;
}

HookType GetHookTypeByIndex(u32 index)
{
  return os_patches[index].type;
}

HookFlag GetHookFlagsByIndex(u32 index)
{
  return os_patches[index].flags;
}

TryReplaceFunctionResult TryReplaceFunction(PPCSymbolDB& ppc_symbol_db, u32 address,
                                            PowerPC::CoreMode mode)
{
  const u32 hook_index = GetHookByFunctionAddress(ppc_symbol_db, address);
  if (hook_index == 0)
    return {};

  const HookType type = GetHookTypeByIndex(hook_index);
  if (type != HookType::Start && type != HookType::Replace)
    return {};

  const HookFlag flags = GetHookFlagsByIndex(hook_index);
  if (!IsEnabled(flags, mode))
    return {};

  return {type, hook_index};
}

bool IsEnabled(HookFlag flag, PowerPC::CoreMode mode)
{
  return flag != HLE::HookFlag::Debug || mode == PowerPC::CoreMode::Interpreter;
}

u32 UnPatch(Core::System& system, std::string_view patch_name)
{
  const auto patch = std::ranges::find(os_patches, patch_name, &Hook::name);
  if (patch == std::end(os_patches))
    return 0;

  auto& power_pc = system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();

  if (patch->flags == HookFlag::Fixed)
  {
    const u32 patch_idx = static_cast<u32>(patch - os_patches);
    u32 addr = 0;
    // Reverse search by OSPatch key instead of address
    u32 kept = 0;
    for (u32 i = 0; i < s_hooked_count; i++)
    {
      if (s_hooked[i].index == patch_idx)
      {
        addr = s_hooked[i].address;
        ppc_state.iCache.Invalidate(memory, jit_interface, addr);
      }
      else
      {
        s_hooked[kept++] = s_hooked[i];
      }
    }
    s_hooked_count = kept;
    return addr;
  }

  const auto symbols = power_pc.GetSymbolDB().GetSymbolsFromName(patch_name);
  if (!symbols.empty())
  {
    const Common::Symbol* const symbol = symbols.front();
    for (u32 addr = symbol->address; addr < symbol->address + symbol->size; addr += 4)
    {
      const u32 slot = FindHookSlot(addr);
      if (slot < s_hooked_count && s_hooked[slot].address == addr)
        RemoveHookSlot(slot);
      ppc_state.iCache.Invalidate(memory, jit_interface, addr);
    }
    return symbol->address;
  }

  return 0;
}

}  // namespace HLE
