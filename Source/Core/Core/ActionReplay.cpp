// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// -----------------------------------------------------------------------------------------
// Partial Action Replay code system implementation.
// Will never be able to support some AR codes - specifically those that patch the running
// Action Replay engine itself - yes they do exist!!!
// Action Replay actually is a small virtual machine with a limited number of commands.
// It probably is Turing complete - but what does that matter when AR codes can write
// actual PowerPC code...
// -----------------------------------------------------------------------------------------

// -------------------------------------------------------------------------------------------------------------
// Code Types:
// (Unconditional) Normal Codes (0): this one has subtypes inside
// (Conditional) Normal Codes (1 - 7): these just compare values and set the line skip info
// Zero Codes: any code with no address.  These codes are used to do special operations like memory
// copy, etc
// -------------------------------------------------------------------------------------------------------------

#include "Core/ActionReplay.h"

#include <bit>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/IniData.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/MsgHandler.h"

#include "Core/ARDecrypt.h"
#include "Core/CheatCodes.h"
#include "Core/Config/MainSettings.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/MMU.h"

#include <retro_atomic.h>

namespace ActionReplay
{
enum
{
  // Zero Code Types
  ZCODE_END = 0x00,
  ZCODE_NORM = 0x02,
  ZCODE_ROW = 0x03,
  ZCODE_04 = 0x04,

  // Conditional Codes
  CONDTIONAL_EQUAL = 0x01,
  CONDTIONAL_NOT_EQUAL = 0x02,
  CONDTIONAL_LESS_THAN_SIGNED = 0x03,
  CONDTIONAL_GREATER_THAN_SIGNED = 0x04,
  CONDTIONAL_LESS_THAN_UNSIGNED = 0x05,
  CONDTIONAL_GREATER_THAN_UNSIGNED = 0x06,
  CONDTIONAL_AND = 0x07,  // bitwise AND

  // Conditional Line Counts
  CONDTIONAL_ONE_LINE = 0x00,
  CONDTIONAL_TWO_LINES = 0x01,
  CONDTIONAL_ALL_LINES_UNTIL = 0x02,
  CONDTIONAL_ALL_LINES = 0x03,

  // Data Types
  DATATYPE_8BIT = 0x00,
  DATATYPE_16BIT = 0x01,
  DATATYPE_32BIT = 0x02,
  DATATYPE_32BIT_FLOAT = 0x03,

  // Normal Code 0 Subtypes
  SUB_RAM_WRITE = 0x00,
  SUB_WRITE_POINTER = 0x01,
  SUB_ADD_CODE = 0x02,
  SUB_MASTER_CODE = 0x03,
};

// Codes list and internal log: emulation thread only. Cheats are changed through
// RunOnCPUThread, the patch engine runs on it, and nothing else touches them.
// The active codes: each one's name and where its lines start among s_active_ops.
struct ActiveCode
{
  char* name;
  u32 first_op;
  u32 num_ops;
};
static ActiveCode* s_active_codes;
static u32 s_num_active_codes;
static AREntry* s_active_ops;
// the name of the code currently being run, (used by log messages that include the code name)
static const char* s_current_name = "";
static bool s_disable_logging = false;

struct ARAddr
{
  union
  {
    u32 address;
    struct
    {
      u32 gcaddr : 25;
      u32 size : 2;
      u32 type : 3;
      u32 subtype : 2;
    };
  };

  ARAddr(const u32 addr) : address(addr) {}
  u32 GCAddress() const { return gcaddr | 0x80000000; }
  operator u32() const { return address; }
};

// ----------------------
// AR Remote Functions
static void FreeActiveCodes()
{
  u32 i;
  for (i = 0; i < s_num_active_codes; i++)
    std::free(s_active_codes[i].name);
  std::free(s_active_codes);
  std::free(s_active_ops);
  s_active_codes = nullptr;
  s_num_active_codes = 0;
  s_active_ops = nullptr;
  s_current_name = "";
}

static char* CopyText(const char* text, size_t len)
{
  char* const copy = static_cast<char*>(std::malloc(len + 1));
  if (copy)
  {
    std::memcpy(copy, text, len);
    copy[len] = '\0';
  }
  return copy;
}

void ApplyCodes(const ARCode* codes, size_t count)
{
  size_t num_codes = 0;
  size_t num_ops = 0;
  size_t i;

  if (!Config::AreCheatsEnabled())
    return;
  s_disable_logging = false;
  FreeActiveCodes();
  for (i = 0; i < count; i++)
  {
    if (!codes[i].enabled)
      continue;
    num_codes++;
    num_ops += codes[i].num_ops;
  }
  if (!num_codes)
    return;

  s_active_codes = static_cast<ActiveCode*>(std::calloc(num_codes, sizeof(ActiveCode)));
  s_active_ops = static_cast<AREntry*>(std::malloc((num_ops ? num_ops : 1) * sizeof(AREntry)));
  if (!s_active_codes || !s_active_ops)
  {
    FreeActiveCodes();
    return;
  }
  {
    u32 op = 0;
    for (i = 0; i < count; i++)
    {
      const ARCode* const code = &codes[i];
      ActiveCode* active;
      if (!code->enabled)
        continue;
      active = &s_active_codes[s_num_active_codes++];
      active->name =
          CopyText(code->name ? code->name : "", code->name ? std::strlen(code->name) : 0);
      active->first_op = op;
      active->num_ops = code->num_ops;
      if (code->num_ops)
        std::memcpy(&s_active_ops[op], code->ops, code->num_ops * sizeof(AREntry));
      op += code->num_ops;
    }
  }
}

size_t CountEnabledCodes()
{
  if (!Config::AreCheatsEnabled())
    return 0;
  return s_num_active_codes;
}

void LoadAndApplyCodes(const Common::IniFile& global_ini, const Common::IniFile& local_ini)
{
  ARCodeList codes = {};
  LoadCodes(global_ini, local_ini, &codes);
  ApplyCodes(codes.codes, codes.count);
  FreeCodeList(&codes);
}

/* Frees what the code holds, and leaves it empty. */
static void CodeFree(ARCode* code)
{
  std::free(code->name);
  std::free(code->ops);
  std::memset(code, 0, sizeof(*code));
}

void FreeCodeList(ARCodeList* list)
{
  u32 c;
  for (c = 0; c < list->count; c++)
    CodeFree(&list->codes[c]);
  std::free(list->codes);
  std::memset(list, 0, sizeof(*list));
}

void CodeAddOp(ARCode* code, u32 cmd_addr, u32 value)
{
  if (code->num_ops == code->cap_ops)
  {
    const u32 cap = code->cap_ops ? code->cap_ops * 2 : 8;
    AREntry* const grown = static_cast<AREntry*>(std::realloc(code->ops, cap * sizeof(AREntry)));
    if (!grown)
      return;
    code->ops = grown;
    code->cap_ops = cap;
  }
  code->ops[code->num_ops].cmd_addr = cmd_addr;
  code->ops[code->num_ops].value = value;
  code->num_ops++;
}

/* A copy of the code, with its name and lines, goes behind the others in the list; dropped when
 * out of memory. */
static void CodeKeepCopy(ARCodeList* list, const ARCode* code)
{
  ARCode copy = {};
  if (list->count == list->cap)
  {
    const u32 cap = list->cap ? list->cap * 2 : 16;
    ARCode* const grown = static_cast<ARCode*>(std::realloc(list->codes, cap * sizeof(ARCode)));
    if (!grown)
      return;
    list->codes = grown;
    list->cap = cap;
  }
  copy.name = CopyText(code->name ? code->name : "", code->name ? std::strlen(code->name) : 0);
  if (!copy.name)
    return;
  if (code->num_ops)
  {
    copy.ops = static_cast<AREntry*>(std::malloc(code->num_ops * sizeof(AREntry)));
    if (!copy.ops)
    {
      std::free(copy.name);
      return;
    }
    std::memcpy(copy.ops, code->ops, code->num_ops * sizeof(AREntry));
  }
  copy.num_ops = code->num_ops;
  copy.cap_ops = code->num_ops;
  copy.enabled = code->enabled;
  list->codes[list->count++] = copy;
}

/* The encrypted lines of a code, kept until the code ends: decryption is done in blocks. */
struct EncryptedLines
{
  char* text;
  size_t count;
  size_t cap;
};

/* A field of a line as a hexadecimal number, the whole field being the number. */
static bool ParseHex(const char* field, size_t len, u32* out)
{
  char text[16];
  char* end = nullptr;
  unsigned long long value;
  if (len >= sizeof(text))
    return false;
  std::memcpy(text, field, len);
  text[len] = '\0';
  errno = 0;
  value = std::strtoull(text, &end, 16);
  if (!end || *end != '\0' || errno == ERANGE || value > 0xFFFFFFFFull)
    return false;
  *out = static_cast<u32>(value);
  return true;
}

/* The fields of a line split at sep as SplitString splits them: their count, and the first
 * max_fields of them. */
static size_t SplitLine(const char* text, size_t len, char sep, const char** fields,
                        size_t* lengths, size_t max_fields)
{
  size_t count = 0;
  size_t start = 0;
  while (start < len)
  {
    const char* const found = static_cast<const char*>(std::memchr(text + start, sep, len - start));
    const size_t end = found ? size_t(found - text) : len;
    if (count < max_fields)
    {
      fields[count] = text + start;
      lengths[count] = end - start;
    }
    count++;
    start = end + 1;
  }
  return count;
}

enum LineKind
{
  LINE_INVALID,
  LINE_DECRYPTED,
  LINE_ENCRYPTED,
};

/* A line "XXXXXXXX XXXXXXXX", decrypted, or "XXXX-XXXX-XXXXX", encrypted, whose 13 characters go
 * into encrypted. */
static LineKind DeserializeLine(const char* text, size_t len, AREntry* op, char* encrypted)
{
  const char* fields[3];
  size_t lengths[3];
  size_t count = SplitLine(text, len, ' ', fields, lengths, 3);

  // Decrypted AR code
  if (count == 2 && lengths[0] == 8 && lengths[1] == 8 &&
      ParseHex(fields[0], lengths[0], &op->cmd_addr) && ParseHex(fields[1], lengths[1], &op->value))
  {
    return LINE_DECRYPTED;
  }

  // Encrypted AR code
  count = SplitLine(text, len, '-', fields, lengths, 3);
  if (count == 3 && lengths[0] == 4 && lengths[1] == 4 && lengths[2] == 5)
  {
    std::memcpy(encrypted, fields[0], 4);
    std::memcpy(encrypted + 4, fields[1], 4);
    std::memcpy(encrypted + 8, fields[2], 5);
    return LINE_ENCRYPTED;
  }

  // Parsing failed
  return LINE_INVALID;
}

static void EncryptedAdd(EncryptedLines* lines, const char* line)
{
  if (lines->count == lines->cap)
  {
    const size_t cap = lines->cap ? lines->cap * 2 : 8;
    char* const grown = static_cast<char*>(std::realloc(lines->text, cap * ENCRYPTED_LINE_SIZE));
    if (!grown)
      return;
    lines->text = grown;
    lines->cap = cap;
  }
  std::memcpy(lines->text + lines->count * ENCRYPTED_LINE_SIZE, line, ENCRYPTED_LINE_SIZE);
  lines->count++;
}

// Parses the Action Replay section of one game ini file, behind the codes already read.
static void LoadSection(const Common::IniFile& ini, ARCodeList* list)
{
  const Common::IniFile::Section* const lines = ini.GetSection("ActionReplay");
  EncryptedLines encrypted_lines = {};
  ARCode current_code = {};
  size_t i;

  for (i = 0; lines && i < lines->Data()->num_lines; i++)
  {
    const char* text;
    size_t len;
    if (!ini_section_line(lines->Data(), i, true, &text, &len) || !len)
      continue;

    // Check if the line is a name of the code
    if (text[0] == '$')
    {
      if (current_code.num_ops)
      {
        CodeKeepCopy(list, &current_code);
        current_code.num_ops = 0;
      }
      if (encrypted_lines.count)
      {
        DecryptARCode(encrypted_lines.text, encrypted_lines.count, &current_code);
        CodeKeepCopy(list, &current_code);
        current_code.num_ops = 0;
        encrypted_lines.count = 0;
      }

      std::free(current_code.name);
      current_code.name = CopyText(text + 1, len - 1);
    }
    else
    {
      AREntry op;
      char encrypted[ENCRYPTED_LINE_SIZE];
      switch (DeserializeLine(text, len, &op, encrypted))
      {
      case LINE_DECRYPTED:
        CodeAddOp(&current_code, op.cmd_addr, op.value);
        break;
      case LINE_ENCRYPTED:
        EncryptedAdd(&encrypted_lines, encrypted);
        break;
      default:
        PanicAlert("Action Replay Error: invalid AR code line: %.*s", static_cast<int>(len), text);
        break;
      }
    }
  }

  // Handle the last code correctly.
  if (current_code.num_ops)
    CodeKeepCopy(list, &current_code);
  if (encrypted_lines.count)
  {
    DecryptARCode(encrypted_lines.text, encrypted_lines.count, &current_code);
    CodeKeepCopy(list, &current_code);
  }
  CodeFree(&current_code);
  std::free(encrypted_lines.text);

  ReadEnabledAndDisabled(ini, "ActionReplay", list->codes, list->count, sizeof(ARCode),
                         offsetof(ARCode, name), offsetof(ARCode, enabled));
}

void LoadCodes(const Common::IniFile& global_ini, const Common::IniFile& local_ini,
               ARCodeList* list)
{
  FreeCodeList(list);
  LoadSection(global_ini, list);
  LoadSection(local_ini, list);
}

// An ACTIONREPLAY info message, formatted on the stack only when it is logged.
static void LogInfo(const char* format, ...) DOLPHIN_PRINTF_ATTR(1, 2);
static void LogInfo(const char* format, ...)
{
  if (s_disable_logging)
    return;

  if constexpr (Common::Log::MAX_EFFECTIVE_LOGLEVEL < Common::Log::LogLevel::LINFO)
    return;

  const auto* const log = Common::Log::LogManager::GetInstance();
  if (!log || !log->IsEnabled(Common::Log::LogType::ACTIONREPLAY, Common::Log::LogLevel::LINFO))
    return;

  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  INFO_LOG(ACTIONREPLAY, "%s", message);
}

// ----------------------
// Code Functions
static bool Subtype_RamWriteAndFill(const Core::CPUThreadGuard& guard, const ARAddr& addr,
                                    const u32 data)
{
  const u32 new_addr = addr.GCAddress();

  LogInfo("Hardware Address: %08x", new_addr);
  LogInfo("Size: %08x", addr.size);

  switch (addr.size)
  {
  case DATATYPE_8BIT:
  {
    LogInfo("8-bit Write");
    LogInfo("--------");
    const u32 repeat = data >> 8;
    for (u32 i = 0; i <= repeat; ++i)
    {
      ApplyMemoryPatch<u8>(guard, static_cast<u8>(data), new_addr + i);
      LogInfo("Wrote %08x to address %08x", data & 0xFF, new_addr + i);
    }
    LogInfo("--------");
    break;
  }

  case DATATYPE_16BIT:
  {
    LogInfo("16-bit Write");
    LogInfo("--------");
    const u32 repeat = data >> 16;
    for (u32 i = 0; i <= repeat; ++i)
    {
      ApplyMemoryPatch<u16>(guard, static_cast<u16>(data), new_addr + i * 2);
      LogInfo("Wrote %08x to address %08x", data & 0xFFFF, new_addr + i * 2);
    }
    LogInfo("--------");
    break;
  }

  case DATATYPE_32BIT_FLOAT:
  case DATATYPE_32BIT:  // Dword write
    LogInfo("32-bit Write");
    LogInfo("--------");
    ApplyMemoryPatch<u32>(guard, data, new_addr);
    LogInfo("Wrote %08x to address %08x", data, new_addr);
    LogInfo("--------");
    break;

  default:
    LogInfo("Bad Size");
    PanicAlert("Action Replay Error: Invalid size "
               "(%08x : address = %08x) in Ram Write And Fill (%s)",
               addr.size, addr.gcaddr, s_current_name);
    return false;
  }

  return true;
}

static bool Subtype_WriteToPointer(const Core::CPUThreadGuard& guard, const ARAddr& addr,
                                   const u32 data)
{
  const u32 new_addr = addr.GCAddress();
  const u32 ptr = PowerPC::MMU::HostRead<u32>(guard, new_addr);

  LogInfo("Hardware Address: %08x", new_addr);
  LogInfo("Size: %08x", addr.size);

  switch (addr.size)
  {
  case DATATYPE_8BIT:
  {
    LogInfo("Write 8-bit to pointer");
    LogInfo("--------");
    const u8 thebyte = static_cast<u8>(data);
    const u32 offset = data >> 8;
    LogInfo("Pointer: %08x", ptr);
    LogInfo("Byte: %08x", (unsigned)thebyte);
    LogInfo("Offset: %08x", offset);
    ApplyMemoryPatch<u8>(guard, thebyte, ptr + offset);
    LogInfo("Wrote %08x to address %08x", (unsigned)thebyte, ptr + offset);
    LogInfo("--------");
    break;
  }

  case DATATYPE_16BIT:
  {
    LogInfo("Write 16-bit to pointer");
    LogInfo("--------");
    const u16 theshort = static_cast<u16>(data);
    const u32 offset = (data >> 16) << 1;
    LogInfo("Pointer: %08x", ptr);
    LogInfo("Byte: %08x", (unsigned)theshort);
    LogInfo("Offset: %08x", offset);
    ApplyMemoryPatch<u16>(guard, theshort, ptr + offset);
    LogInfo("Wrote %08x to address %08x", (unsigned)theshort, ptr + offset);
    LogInfo("--------");
    break;
  }

  case DATATYPE_32BIT_FLOAT:
  case DATATYPE_32BIT:
    LogInfo("Write 32-bit to pointer");
    LogInfo("--------");
    ApplyMemoryPatch<u32>(guard, data, ptr);
    LogInfo("Wrote %08x to address %08x", data, ptr);
    LogInfo("--------");
    break;

  default:
    LogInfo("Bad Size");
    PanicAlert("Action Replay Error: Invalid size "
               "(%08x : address = %08x) in Write To Pointer (%s)",
               addr.size, addr.gcaddr, s_current_name);
    return false;
  }
  return true;
}

static bool Subtype_AddCode(const Core::CPUThreadGuard& guard, const ARAddr& addr, const u32 data)
{
  // Used to increment/decrement a value in memory
  const u32 new_addr = addr.GCAddress();

  LogInfo("Hardware Address: %08x", new_addr);
  LogInfo("Size: %08x", addr.size);

  switch (addr.size)
  {
  case DATATYPE_8BIT:
    LogInfo("8-bit Add");
    LogInfo("--------");
    ApplyMemoryPatch<u8>(guard, PowerPC::MMU::HostRead<u8>(guard, new_addr) + data, new_addr);
    LogInfo("Wrote %02x to address %08x", (unsigned)(PowerPC::MMU::HostRead<u8>(guard, new_addr)),
            new_addr);
    LogInfo("--------");
    break;

  case DATATYPE_16BIT:
    LogInfo("16-bit Add");
    LogInfo("--------");
    ApplyMemoryPatch<u16>(guard, PowerPC::MMU::HostRead<u16>(guard, new_addr) + data, new_addr);
    LogInfo("Wrote %04x to address %08x", (unsigned)(PowerPC::MMU::HostRead<u16>(guard, new_addr)),
            new_addr);
    LogInfo("--------");
    break;

  case DATATYPE_32BIT:
    LogInfo("32-bit Add");
    LogInfo("--------");
    ApplyMemoryPatch<u32>(guard, PowerPC::MMU::HostRead<u32>(guard, new_addr) + data, new_addr);
    LogInfo("Wrote %08x to address %08x", PowerPC::MMU::HostRead<u32>(guard, new_addr), new_addr);
    LogInfo("--------");
    break;

  case DATATYPE_32BIT_FLOAT:
  {
    LogInfo("32-bit floating Add");
    LogInfo("--------");

    const u32 read = PowerPC::MMU::HostRead<u32>(guard, new_addr);
    const float read_float = std::bit_cast<float>(read);
    // data contains an (unsigned?) integer value
    const float fread = read_float + static_cast<float>(data);
    const u32 newval = std::bit_cast<u32>(fread);
    ApplyMemoryPatch<u32>(guard, newval, new_addr);
    LogInfo("Old Value %08x", read);
    LogInfo("Increment %08x", data);
    LogInfo("New value %08x", newval);
    LogInfo("--------");
    break;
  }

  default:
    LogInfo("Bad Size");
    PanicAlert("Action Replay Error: Invalid size "
               "(%08x : address = %08x) in Add Code (%s)",
               addr.size, addr.gcaddr, s_current_name);
    return false;
  }
  return true;
}

static bool Subtype_MasterCodeAndWriteToCCXXXXXX(const ARAddr& addr, const u32 data)
{
  // code not yet implemented - TODO
  // u32 new_addr = (addr & 0x01FFFFFF) | 0x80000000;
  // u8  mcode_type = (data & 0xFF0000) >> 16;
  // u8  mcode_count = (data & 0xFF00) >> 8;
  // u8  mcode_number = data & 0xFF;
  PanicAlert("Action Replay Error: Master Code and Write To CCXXXXXX not implemented (%s)\n"
             "Master codes are not needed. Do not use master codes.",
             s_current_name);
  return false;
}

// This needs more testing
static bool ZeroCode_FillAndSlide(const Core::CPUThreadGuard& guard, const u32 val_last,
                                  const ARAddr& addr, const u32 data)
{
  const u32 new_addr = ARAddr(val_last).GCAddress();
  const u8 size = ARAddr(val_last).size;

  const s16 addr_incr = static_cast<s16>(data & 0xFFFF);
  const s8 val_incr = static_cast<s8>(data >> 24);
  const u8 write_num = static_cast<u8>((data & 0xFF0000) >> 16);

  u32 val = addr;
  u32 curr_addr = new_addr;

  LogInfo("Current Hardware Address: %08x", new_addr);
  LogInfo("Size: %08x", addr.size);
  LogInfo("Write Num: %08x", (unsigned)write_num);
  LogInfo("Address Increment: %d", (int)addr_incr);
  LogInfo("Value Increment: %d", s32{val_incr});

  switch (size)
  {
  case DATATYPE_8BIT:
    LogInfo("8-bit Write");
    LogInfo("--------");
    for (int i = 0; i < write_num; ++i)
    {
      ApplyMemoryPatch<u8>(guard, static_cast<u8>(val), curr_addr);
      curr_addr += addr_incr;
      val += val_incr;
      LogInfo("Write %08x to address %08x", val & 0xFF, curr_addr);

      LogInfo("Value Update: %08x", val);
      LogInfo("Current Hardware Address Update: %08x", curr_addr);
    }
    LogInfo("--------");
    break;

  case DATATYPE_16BIT:
    LogInfo("16-bit Write");
    LogInfo("--------");
    for (int i = 0; i < write_num; ++i)
    {
      ApplyMemoryPatch<u16>(guard, static_cast<u16>(val), curr_addr);
      LogInfo("Write %08x to address %08x", val & 0xFFFF, curr_addr);
      curr_addr += addr_incr * 2;
      val += val_incr;
      LogInfo("Value Update: %08x", val);
      LogInfo("Current Hardware Address Update: %08x", curr_addr);
    }
    LogInfo("--------");
    break;

  case DATATYPE_32BIT:
    LogInfo("32-bit Write");
    LogInfo("--------");
    for (int i = 0; i < write_num; ++i)
    {
      ApplyMemoryPatch<u32>(guard, val, curr_addr);
      LogInfo("Write %08x to address %08x", val, curr_addr);
      curr_addr += addr_incr * 4;
      val += val_incr;
      LogInfo("Value Update: %08x", val);
      LogInfo("Current Hardware Address Update: %08x", curr_addr);
    }
    LogInfo("--------");
    break;

  default:
    LogInfo("Bad Size");
    PanicAlert("Action Replay Error: Invalid size (%08x : address = %08x) in Fill and Slide (%s)",
               (unsigned)size, new_addr, s_current_name);
    return false;
  }
  return true;
}

// kenobi's "memory copy" Z-code. Requires an additional master code
// on a real AR device. Documented here:
// https://github.com/dolphin-emu/dolphin/wiki/GameCube-Action-Replay-Code-Types#type-z4-size-3--memory-copy
static bool ZeroCode_MemoryCopy(const Core::CPUThreadGuard& guard, const u32 val_last,
                                const ARAddr& addr, const u32 data)
{
  const u32 addr_dest = val_last & ~0x06000000;
  const u32 addr_src = addr.GCAddress();

  const u8 num_bytes = data & 0x7FFF;

  LogInfo("Dest Address: %08x", addr_dest);
  LogInfo("Src Address: %08x", addr_src);
  LogInfo("Size: %08x", (unsigned)num_bytes);

  if ((data & 0xFF0000) == 0)
  {
    if ((data >> 24) != 0x0)
    {  // Memory Copy With Pointers Support
      LogInfo("Memory Copy With Pointers Support");
      LogInfo("--------");
      const u32 ptr_dest = PowerPC::MMU::HostRead<u32>(guard, addr_dest);
      LogInfo("Resolved Dest Address to: %08x", ptr_dest);
      const u32 ptr_src = PowerPC::MMU::HostRead<u32>(guard, addr_src);
      LogInfo("Resolved Src Address to: %08x", ptr_src);
      for (int i = 0; i < num_bytes; ++i)
      {
        ApplyMemoryPatch<u8>(guard, PowerPC::MMU::HostRead<u8>(guard, ptr_src + i), ptr_dest + i);
        LogInfo("Wrote %08x to address %08x",
                (unsigned)(PowerPC::MMU::HostRead<u8>(guard, ptr_src + i)), ptr_dest + i);
      }
      LogInfo("--------");
    }
    else
    {  // Memory Copy Without Pointer Support
      LogInfo("Memory Copy Without Pointers Support");
      LogInfo("--------");
      for (int i = 0; i < num_bytes; ++i)
      {
        ApplyMemoryPatch<u8>(guard, PowerPC::MMU::HostRead<u8>(guard, addr_src + i), addr_dest + i);
        LogInfo("Wrote %08x to address %08x",
                (unsigned)(PowerPC::MMU::HostRead<u8>(guard, addr_src + i)), addr_dest + i);
      }
      LogInfo("--------");
      return true;
    }
  }
  else
  {
    LogInfo("Bad Value");
    PanicAlert("Action Replay Error: Invalid value (%08x) in Memory Copy (%s)", (data & ~0x7FFF),
               s_current_name);
    return false;
  }
  return true;
}

static bool NormalCode(const Core::CPUThreadGuard& guard, const ARAddr& addr, const u32 data)
{
  switch (addr.subtype)
  {
  case SUB_RAM_WRITE:  // Ram write (and fill)
    LogInfo("Doing Ram Write And Fill");
    if (!Subtype_RamWriteAndFill(guard, addr, data))
      return false;
    break;

  case SUB_WRITE_POINTER:  // Write to pointer
    LogInfo("Doing Write To Pointer");
    if (!Subtype_WriteToPointer(guard, addr, data))
      return false;
    break;

  case SUB_ADD_CODE:  // Increment Value
    LogInfo("Doing Add Code");
    if (!Subtype_AddCode(guard, addr, data))
      return false;
    break;

  case SUB_MASTER_CODE:  // Master Code & Write to CCXXXXXX
    LogInfo("Doing Master Code And Write to CCXXXXXX (ncode not supported)");
    if (!Subtype_MasterCodeAndWriteToCCXXXXXX(addr, data))
      return false;
    break;

  default:
    LogInfo("Bad Subtype");
    PanicAlert("Action Replay: Normal Code 0: Invalid Subtype %08x (%s)", addr.subtype,
               s_current_name);
    return false;
  }

  return true;
}

static bool CompareValues(const u32 val1, const u32 val2, const int type)
{
  switch (type)
  {
  case CONDTIONAL_EQUAL:
    LogInfo("Type 1: If Equal");
    return val1 == val2;

  case CONDTIONAL_NOT_EQUAL:
    LogInfo("Type 2: If Not Equal");
    return val1 != val2;

  case CONDTIONAL_LESS_THAN_SIGNED:
    LogInfo("Type 3: If Less Than (Signed)");
    return static_cast<s32>(val1) < static_cast<s32>(val2);

  case CONDTIONAL_GREATER_THAN_SIGNED:
    LogInfo("Type 4: If Greater Than (Signed)");
    return static_cast<s32>(val1) > static_cast<s32>(val2);

  case CONDTIONAL_LESS_THAN_UNSIGNED:
    LogInfo("Type 5: If Less Than (Unsigned)");
    return val1 < val2;

  case CONDTIONAL_GREATER_THAN_UNSIGNED:
    LogInfo("Type 6: If Greater Than (Unsigned)");
    return val1 > val2;

  case CONDTIONAL_AND:
    LogInfo("Type 7: If And");
    return !!(val1 & val2);  // bitwise AND

  default:
    LogInfo("Unknown Compare type");
    PanicAlert("Action Replay: Invalid Normal Code Type %08x (%s)", (unsigned)type, s_current_name);
    return false;
  }
}

static bool ConditionalCode(const Core::CPUThreadGuard& guard, const ARAddr& addr, const u32 data,
                            int* const pSkipCount)
{
  const u32 new_addr = addr.GCAddress();

  LogInfo("Size: %08x", addr.size);
  LogInfo("Hardware Address: %08x", new_addr);

  bool result = true;

  switch (addr.size)
  {
  case DATATYPE_8BIT:
    result = CompareValues(PowerPC::MMU::HostRead<u8>(guard, new_addr), (data & 0xFF), addr.type);
    break;

  case DATATYPE_16BIT:
    result =
        CompareValues(PowerPC::MMU::HostRead<u16>(guard, new_addr), (data & 0xFFFF), addr.type);
    break;

  case DATATYPE_32BIT_FLOAT:
  case DATATYPE_32BIT:
    result = CompareValues(PowerPC::MMU::HostRead<u32>(guard, new_addr), data, addr.type);
    break;

  default:
    LogInfo("Bad Size");
    PanicAlert("Action Replay: Conditional Code: Invalid Size %08x (%s)", addr.size,
               s_current_name);
    return false;
  }

  // if the comparison failed we need to skip some lines
  if (false == result)
  {
    switch (addr.subtype)
    {
    case CONDTIONAL_ONE_LINE:
    case CONDTIONAL_TWO_LINES:
      *pSkipCount = addr.subtype + 1;  // Skip 1 or 2 lines
      break;

    // Skip all lines,
    // Skip lines until a "00000000 40000000" line is reached
    case CONDTIONAL_ALL_LINES:
    case CONDTIONAL_ALL_LINES_UNTIL:
      *pSkipCount = -static_cast<int>(addr.subtype);
      break;

    default:
      LogInfo("Bad Subtype");
      PanicAlert("Action Replay: Normal Code %d: Invalid subtype %08x (%s)", 1, addr.subtype,
                 s_current_name);
      return false;
    }
  }

  return true;
}

// Emulation thread only, like s_current_name and LogInfo
static bool RunCodeLocked(const Core::CPUThreadGuard& guard, const ActiveCode& arcode)
{
  // The mechanism is different than what the real AR uses, so there may be compatibility problems.

  bool do_fill_and_slide = false;
  bool do_memory_copy = false;

  // used for conditional codes
  int skip_count = 0;

  u32 val_last = 0;

  s_current_name = arcode.name ? arcode.name : "";

  LogInfo("Code Name: %s", s_current_name);
  LogInfo("Number of codes: %u", arcode.num_ops);

  for (u32 op = 0; op < arcode.num_ops; op++)
  {
    const AREntry& entry = s_active_ops[arcode.first_op + op];
    const ARAddr addr(entry.cmd_addr);
    const u32 data = entry.value;

    // after a conditional code, skip lines if needed
    if (skip_count)
    {
      if (skip_count > 0)  // skip x lines
      {
        LogInfo("Line skipped");
        --skip_count;
      }
      else if (-CONDTIONAL_ALL_LINES == skip_count)
      {
        // skip all lines
        LogInfo("All Lines skipped");
        return true;  // don't need to iterate through the rest of the ops
      }
      else if (-CONDTIONAL_ALL_LINES_UNTIL == skip_count)
      {
        // skip until a "00000000 40000000" line is reached
        LogInfo("Line skipped");
        if (addr == 0 && 0x40000000 == data)  // check for an endif line
          skip_count = 0;
      }

      continue;
    }

    LogInfo("--- Running Code: %08x %08x ---", addr.address, data);

    // Do Fill & Slide
    if (do_fill_and_slide)
    {
      do_fill_and_slide = false;
      LogInfo("Doing Fill And Slide");
      if (false == ZeroCode_FillAndSlide(guard, val_last, addr, data))
        return false;
      continue;
    }

    // Memory Copy
    if (do_memory_copy)
    {
      do_memory_copy = false;
      LogInfo("Doing Memory Copy");
      if (false == ZeroCode_MemoryCopy(guard, val_last, addr, data))
        return false;
      continue;
    }

    // ActionReplay program self modification codes
    if (addr >= 0x00002000 && addr < 0x00003000)
    {
      LogInfo(
          "This action replay simulator does not support codes that modify Action Replay itself.");
      PanicAlert(
          "This action replay simulator does not support codes that modify Action Replay itself.");
      return false;
    }

    // skip these weird init lines
    // TODO: Where are the "weird init lines"?
    // if (iter == code.ops.begin() && cmd == 1)
    // continue;

    // Zero codes
    if (0x0 == addr)  // Check if the code is a zero code
    {
      const u8 zcode = data >> 29;

      LogInfo("Doing Zero Code %08x", (unsigned)zcode);

      switch (zcode)
      {
      case ZCODE_END:  // END OF CODES
        LogInfo("ZCode: End Of Codes");
        return true;

      // TODO: the "00000000 40000000"(end if) codes fall into this case, I don't think that is
      // correct
      case ZCODE_NORM:  // Normal execution of codes
        // Todo: Set register 1BB4 to 0
        LogInfo("ZCode: Normal execution of codes, set register 1BB4 to 0 (zcode not supported)");
        break;

      case ZCODE_ROW:  // Executes all codes in the same row
        // Todo: Set register 1BB4 to 1
        LogInfo("ZCode: Executes all codes in the same row, Set register 1BB4 to 1 (zcode not "
                "supported)");
        PanicAlert("Zero 3 code not supported");
        return false;

      case ZCODE_04:  // Fill & Slide or Memory Copy
        if (0x3 == ((data >> 25) & 0x03))
        {
          LogInfo("ZCode: Memory Copy");
          do_memory_copy = true;
          val_last = data;
        }
        else
        {
          LogInfo("ZCode: Fill And Slide");
          do_fill_and_slide = true;
          val_last = data;
        }
        break;

      default:
        LogInfo("ZCode: Unknown");
        PanicAlert("Zero code unknown to Dolphin: %08x", (unsigned)zcode);
        return false;
      }

      // done handling zero codes
      continue;
    }

    // Normal codes
    LogInfo("Doing Normal Code %08x", addr.type);
    LogInfo("Subtype: %08x", addr.subtype);

    switch (addr.type)
    {
    case 0x00:
      if (false == NormalCode(guard, addr, data))
        return false;
      break;

    default:
      LogInfo("This Normal Code is a Conditional Code");
      if (false == ConditionalCode(guard, addr, data, &skip_count))
        return false;
      break;
    }
  }

  return true;
}

void RunAllActive(const Core::CPUThreadGuard& cpu_guard)
{
  if (!Config::AreCheatsEnabled())
    return;

  // If the mutex is idle then acquiring it should be cheap, fast mutexes
  // are only atomic ops unless contested. It should be rare for this to
  // be contested.
  // A code that fails is dropped; those after it keep their order.
  {
    u32 kept = 0;
    for (u32 i = 0; i < s_num_active_codes; i++)
    {
      const bool success = RunCodeLocked(cpu_guard, s_active_codes[i]);
      LogInfo("\n");
      if (success)
        s_active_codes[kept++] = s_active_codes[i];
      else
        std::free(s_active_codes[i].name);
    }
    s_num_active_codes = kept;
    s_current_name = "";
  }
  s_disable_logging = true;
}

}  // namespace ActionReplay
