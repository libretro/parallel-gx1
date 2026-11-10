// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/DSP/DSPCodeUtil.h"

#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "Core/DSP/DSPDisassembler.h"

namespace DSP
{
static bool Disassemble(const u16* code, size_t size, bool line_numbers, std::string& text)
{
  if (!size)
    return false;

  AssemblerSettings settings;

  // These two prevent roundtripping.
  settings.show_hex = true;
  settings.show_pc = line_numbers;
  settings.ext_separator = '\'';
  settings.decode_names = true;
  settings.decode_registers = true;

  DSPDisassembler disasm(settings);
  bool success = disasm.Disassemble(code, size, text);
  return success;
}

bool DumpDSPCode(const u8* code_be, size_t size_in_bytes, u32 crc)
{
  const std::string root_name =
      File::GetUserPath(D_DUMPDSP_IDX) + fmt::format("DSP_UC_{:08X}", crc);
  const std::string binary_file = root_name + ".bin";
  const std::string text_file = root_name + ".txt";

  if (!File::IOFile(binary_file, "wb").WriteBytes(code_be, size_in_bytes))
  {
    PanicAlertFmt("Can't dump UCode to file '{}'!!", binary_file);
    return false;
  }

  // The disassembler works in native endian
  std::vector<u16> code(size_in_bytes / 2);
  for (size_t i = 0; i < code.size(); i++)
    code[i] = Common::swap16(&code_be[i * 2]);

  std::string text;
  if (!Disassemble(code.data(), code.size(), true, text))
    return false;

  return File::WriteStringToFile(text_file, text);
}

}  // namespace DSP
