// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/GeckoCode.h"

#include <cstdlib>
#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace Gecko
{
static constexpr u32 CODE_SIZE = 8;

enum class Installation
{
  Uninstalled,
  Installed,
  Failed
};

static Installation s_code_handler_installed = Installation::Uninstalled;
/* The currently active codes: each one's name and where its words start among
 * s_active_words, which holds two per line, address then data. */
struct ActiveCode
{
  char* name;
  u32 first_word;
  u32 num_words;
};
static ActiveCode* s_active_codes;
static u32 s_num_active_codes;
static u32* s_active_words;

static void FreeActiveCodes()
{
  u32 i;
  for (i = 0; i < s_num_active_codes; i++)
    std::free(s_active_codes[i].name);
  std::free(s_active_codes);
  std::free(s_active_words);
  s_active_codes = nullptr;
  s_num_active_codes = 0;
  s_active_words = nullptr;
}

size_t CountEnabledCodes()
{
  if (!Config::AreCheatsEnabled())
    return 0;
  return s_num_active_codes;
}

void SetActiveCodes(const GeckoCode* gcodes, size_t count)
{
  FreeActiveCodes();
  if (Config::AreCheatsEnabled())
  {
    size_t num_codes = 0;
    size_t num_words = 0;
    size_t i;
    for (i = 0; i < count; i++)
    {
      if (!gcodes[i].enabled)
        continue;
      num_codes++;
      num_words += size_t(gcodes[i].num_lines) * 2;
    }
    if (num_codes)
    {
      s_active_codes = static_cast<ActiveCode*>(std::calloc(num_codes, sizeof(ActiveCode)));
      s_active_words = static_cast<u32*>(std::malloc((num_words ? num_words : 1) * sizeof(u32)));
      if (!s_active_codes || !s_active_words)
      {
        FreeActiveCodes();
      }
      else
      {
        u32 word = 0;
        for (i = 0; i < count; i++)
        {
          const GeckoCode* const code = &gcodes[i];
          ActiveCode* const active = &s_active_codes[s_num_active_codes++];
          u32 l;
          if (!code->enabled)
          {
            s_num_active_codes--;
            continue;
          }
          if (code->name)
          {
            const size_t len = std::strlen(code->name);
            active->name = static_cast<char*>(std::malloc(len + 1));
            if (active->name)
              std::memcpy(active->name, code->name, len + 1);
          }
          active->first_word = word;
          active->num_words = code->num_lines * 2;
          for (l = 0; l < code->num_lines; l++)
          {
            s_active_words[word++] = code->lines[l].address;
            s_active_words[word++] = code->lines[l].data;
          }
        }
      }
    }
  }

  s_code_handler_installed = Installation::Uninstalled;
}

// Emulation thread only, like everything touching the active codes.
// NOTE: Refer to "codehandleronly.s" from Gecko OS.
static Installation InstallCodeHandlerLocked()
{
  File::IOFile handler(File::GetSysDirectory() + GECKO_CODE_HANDLER, "rb");
  if (!handler)
  {
    ERROR_LOG(ACTIONREPLAY, "Could not enable cheats because " GECKO_CODE_HANDLER " was missing.");
    return Installation::Failed;
  }

  const u64 handler_size = handler.GetSize();
  if (handler_size > INSTALLER_END_ADDRESS - INSTALLER_BASE_ADDRESS - CODE_SIZE)
  {
    ERROR_LOG(ACTIONREPLAY, GECKO_CODE_HANDLER " is too big. The file may be corrupt.");
    return Installation::Failed;
  }

  u8 mmio_addr = 0xCC;
  if (Core::System::GetInstance().IsWii())
  {
    mmio_addr = 0xCD;
  }

  // Install code handler, from the file straight into emulated memory
  {
    u8* const dest = Core::System::GetInstance().GetMemory().GetPointerForRange(
        INSTALLER_BASE_ADDRESS, handler_size);
    if (!dest || !handler.ReadBytes(dest, handler_size))
      return Installation::Failed;
  }
  const u32 data_size = static_cast<u32>(handler_size);

  // Patch the code handler to the current system type (Gamecube/Wii)
  for (u32 h = 0; h < data_size; h += 4)
  {
    // Patch MMIO address
    if (PowerPC::MMU::HostRead<u32>(INSTALLER_BASE_ADDRESS + h) ==
        (0x3f000000u | ((mmio_addr ^ 1) << 8)))
    {
      NOTICE_LOG(ACTIONREPLAY, "Patching MMIO access at %08x", INSTALLER_BASE_ADDRESS + h);
      PowerPC::MMU::HostWrite<u32>(0x3f000000u | mmio_addr << 8, INSTALLER_BASE_ADDRESS + h);
    }
  }

  const u32 codelist_base_address =
      INSTALLER_BASE_ADDRESS + data_size - CODE_SIZE;
  const u32 codelist_end_address = INSTALLER_END_ADDRESS;

  // Write a magic value to 'gameid' (codehandleronly does not actually read this).
  // This value will be read back and modified over time by HLE_Misc::GeckoCodeHandlerICacheFlush.
  PowerPC::MMU::HostWrite<u32>(MAGIC_GAMEID, INSTALLER_BASE_ADDRESS);

  // Create GCT in memory
  PowerPC::MMU::HostWrite<u32>(0x00d0c0de, codelist_base_address);
  PowerPC::MMU::HostWrite<u32>(0x00d0c0de, codelist_base_address + 4);

  // Each code is 8 bytes (2 words) wide. There is a starter code and an end code.
  const u32 start_address = codelist_base_address + CODE_SIZE;
  const u32 end_address = codelist_end_address - CODE_SIZE;
  u32 next_address = start_address;

  // NOTE: Only active codes are in the list
  for (u32 c = 0; c < s_num_active_codes; c++)
  {
    const ActiveCode* const active_code = &s_active_codes[c];
    const u32 size = active_code->num_words / 2 * CODE_SIZE;
    // If the code is not going to fit in the space we have left then we have to skip it
    if (u64(next_address) + size > end_address)
    {
      NOTICE_LOG(ACTIONREPLAY,
                 "Too many GeckoCodes! Ran out of storage space in Game RAM. Could "
                 "not write: \"%s\". Need %u bytes, only %u remain.",
                 active_code->name ? active_code->name : "", size, end_address - next_address);
      continue;
    }

    for (u32 w = 0; w < active_code->num_words; w += 2)
    {
      PowerPC::MMU::HostWrite<u32>(s_active_words[active_code->first_word + w], next_address);
      PowerPC::MMU::HostWrite<u32>(s_active_words[active_code->first_word + w + 1],
                                   next_address + 4);
      next_address += CODE_SIZE;
    }
  }

  WARN_LOG(ACTIONREPLAY, "GeckoCodes: Using %u of %u bytes", next_address - start_address,
           end_address - start_address);

  // Stop code. Tells the handler that this is the end of the list.
  PowerPC::MMU::HostWrite<u32>(0xF0000000, next_address);
  PowerPC::MMU::HostWrite<u32>(0x00000000, next_address + 4);
  PowerPC::MMU::HostWrite<u32>(0, HLE_TRAMPOLINE_ADDRESS);

  // Turn on codes
  PowerPC::MMU::HostWrite<u8>(1, INSTALLER_BASE_ADDRESS + 7);

  // Invalidate the icache and any asm codes
  auto& ppc_state = Core::System::GetInstance().GetPPCState();
  auto& memory = Core::System::GetInstance().GetMemory();
  auto& jit_interface = Core::System::GetInstance().GetJitInterface();
  for (u32 j = 0; j < (INSTALLER_END_ADDRESS - INSTALLER_BASE_ADDRESS); j += 32)
  {
    ppc_state.iCache.Invalidate(memory, jit_interface, INSTALLER_BASE_ADDRESS + j);
  }
  return Installation::Installed;
}

// Gecko needs to participate in the savestate system because the handler is embedded within the
// game directly. The PC may be inside the code handler in the save state and the codehandler.bin
// on the disk may be different resulting in the PC pointing at a different instruction and then
// the game malfunctions or crashes. [Also, self-modifying codes will break since the
// modifications will be reset]
void DoState(PointerWrap& p)
{
  p.Do(s_code_handler_installed);
  // FIXME: The active codes list will disagree with the embedded GCT
}

void Shutdown()
{
  FreeActiveCodes();
  s_code_handler_installed = Installation::Uninstalled;
}

void RunCodeHandler()
{
  if (!Config::AreCheatsEnabled())
    return;

  // Emulation thread only; the codes are never touched elsewhere.
  {
    if (s_code_handler_installed != Installation::Installed)
    {
      // Don't spam retry if the install failed. The corrupt / missing disk file is not likely to be
      // fixed within 1 frame of the last error.
      if (!s_num_active_codes || s_code_handler_installed == Installation::Failed)
        return;
      s_code_handler_installed = InstallCodeHandlerLocked();

      // A warning was already issued for the install failing
      if (s_code_handler_installed != Installation::Installed)
        return;
    }
  }

  auto& ppc_state = Core::System::GetInstance().GetPPCState();

  // We always do this to avoid problems with the stack since we're branching in random locations.
  // Even with function call return hooks (PC == LR), hand coded assembler won't necessarily
  // follow the ABI. [Volatile FPR, GPR, CR may not be volatile]
  // The codehandler will STMW all of the GPR registers, but we need to fix the Stack's Red
  // Zone, the LR, PC (return address) and the volatile floating point registers.
  // Build a function call stack frame.
  u32 SFP = ppc_state.gpr[1];                     // Stack Frame Pointer
  ppc_state.gpr[1] -= 256;                        // Stack's Red Zone
  ppc_state.gpr[1] -= 16 + 2 * 14 * sizeof(u64);  // Our stack frame
                                                  // (HLE_Misc::GeckoReturnTrampoline)
  ppc_state.gpr[1] -= 8;                          // Fake stack frame for codehandler
  ppc_state.gpr[1] &= 0xFFFFFFF0;                 // Align stack to 16bytes
  u32 SP = ppc_state.gpr[1];                      // Stack Pointer
  PowerPC::MMU::HostWrite<u32>(SP + 8, SP);
  // SP + 4 is reserved for the codehandler to save LR to the stack.
  PowerPC::MMU::HostWrite<u32>(SFP, SP + 8);  // Real stack frame
  PowerPC::MMU::HostWrite<u32>(ppc_state.pc, SP + 12);
  PowerPC::MMU::HostWrite<u32>(LR(ppc_state), SP + 16);
  PowerPC::MMU::HostWrite<u32>(ppc_state.cr.Get(), SP + 20);
  // Registers FPR0->13 are volatile
  for (u32 i = 0; i < 14; ++i)
  {
    PowerPC::MMU::HostWrite<u64>(ppc_state.ps[i].PS0AsU64(), SP + 24 + 2 * i * sizeof(u64));
    PowerPC::MMU::HostWrite<u64>(ppc_state.ps[i].PS1AsU64(), SP + 24 + (2 * i + 1) * sizeof(u64));
  }
  DEBUG_LOG(ACTIONREPLAY,
            "GeckoCodes: Initiating phantom branch-and-link. "
            "PC = %#010x, SP = %#010x, SFP = %#010x",
            ppc_state.pc, SP, SFP);
  LR(ppc_state) = HLE_TRAMPOLINE_ADDRESS;
  ppc_state.pc = ppc_state.npc = ENTRY_POINT;
}

}  // namespace Gecko
