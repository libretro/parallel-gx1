// Copyright 2008 Dolphin Emulator Project
// Copyright 2005 Duddie
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/DSP/DSPDisassembler.h"

#include <cstdarg>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "Core/DSP/DSPTables.h"

namespace DSP
{
// Appends @format's text to @dest; a disassembly piece is short, so it is made on the stack.
static void Append(std::string& dest, const char* format, ...) DOLPHIN_PRINTF_ATTR(2, 3);
static void Append(std::string& dest, const char* format, ...)
{
  char text[64];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  dest += text;
}

DSPDisassembler::DSPDisassembler(const AssemblerSettings& settings) : settings_(settings)
{
}

bool DSPDisassembler::Disassemble(const u16* code, size_t size, std::string& text)
{
  if (size > std::numeric_limits<u16>::max())
  {
    text.append("; code too large for 16-bit addressing\n");
    return false;
  }

  for (u16 pc = 0; pc < size;)
  {
    bool failed = !DisassembleOpcode(code, size, &pc, text);
    text.append("\n");
    if (failed)
      return false;
  }
  return true;
}

std::string DSPDisassembler::DisassembleParameters(const DSPOPCTemplate& opc, u16 op1, u16 op2)
{
  std::string buf;

  for (int j = 0; j < opc.param_count; j++)
  {
    if (j > 0)
      buf += ", ";

    u32 val = (opc.params[j].loc >= 1) ? op2 : op1;
    val &= opc.params[j].mask;
    if (opc.params[j].lshift < 0)
      val = val << (-opc.params[j].lshift);
    else
      val = val >> opc.params[j].lshift;

    u32 type = opc.params[j].type;
    if ((type & 0xff) == 0x10)
      type &= 0xff00;

    if (type & P_REG)
    {
      // Check for _D parameter - if so flip.
      if ((type == P_ACC_D) || (type == P_ACCM_D))  // Used to be P_ACCM_D TODO verify
        val = (~val & 0x1) | ((type & P_REGS_MASK) >> 8);
      else
        val |= (type & P_REGS_MASK) >> 8;
      type &= ~P_REGS_MASK;
    }

    switch (type)
    {
    case P_REG:
      if (settings_.decode_registers)
        Append(buf, "$%s", pdregname(val).c_str());
      else
        Append(buf, "$%u", static_cast<unsigned>(val));
      break;

    case P_PRG:
      if (settings_.decode_registers)
        Append(buf, "@$%s", pdregname(val).c_str());
      else
        Append(buf, "@$%u", static_cast<unsigned>(val));
      break;

    case P_VAL:
    case P_ADDR_I:
    case P_ADDR_D:
      if (settings_.decode_names)
      {
        buf += pdname(val);
      }
      else
      {
        Append(buf, "0x%04x", static_cast<unsigned>(val));
      }
      break;

    case P_IMM:
      if (opc.params[j].size != 2)
      {
        // LSL, LSR, ASL, ASR
        if (opc.params[j].mask == 0x003f)
        {
          // Left and right shifts function essentially as a single shift by a 7-bit signed value,
          // but are split into two instructions for clarity.
          Append(buf, "#%d", (val & 0x20) != 0 ? (int(val) - 64) : int(val));
        }
        else
        {
          Append(buf, "#0x%02x", static_cast<unsigned>(val));
        }
      }
      else
      {
        Append(buf, "#0x%04x", static_cast<unsigned>(val));
      }
      break;

    case P_MEM:
      if (opc.params[j].size != 2)
        val = (u16)(s16)(s8)val;

      if (settings_.decode_names)
        Append(buf, "@%s", pdname(val).c_str());
      else
        Append(buf, "@0x%04x", static_cast<unsigned>(val));
      break;

    default:
      ERROR_LOG(DSPLLE, "Unknown parameter type: %x", std::to_underlying(opc.params[j].type));
      break;
    }
  }

  return buf;
}

bool DSPDisassembler::DisassembleOpcode(const u16* binbuf, size_t binbuf_size, u16* pc,
                                        std::string& dest)
{
  const u16 wrapped_pc = (*pc & 0x7fff);
  if (wrapped_pc >= binbuf_size)
  {
    ++pc;
    dest.append("; outside memory");
    return false;
  }

  const u16 op1 = binbuf[wrapped_pc];

  // Find main opcode
  const DSPOPCTemplate* opc = FindOpInfoByOpcode(op1);
  if (!opc)
    opc = &cw;

  bool is_extended = false;
  bool is_only_7_bit_ext = false;

  if (((opc->opcode >> 12) == 0x3) && (op1 & 0x007f))
  {
    is_extended = true;
    is_only_7_bit_ext = true;
  }
  else if (((opc->opcode >> 12) > 0x3) && (op1 & 0x00ff))
  {
    is_extended = true;
  }

  const DSPOPCTemplate* opc_ext = nullptr;
  if (is_extended)
  {
    // opcode has an extension
    const u16 extended_opcode = is_only_7_bit_ext ? op1 & 0x7F : op1;
    opc_ext = FindExtOpInfoByOpcode(extended_opcode);
  }

  // printing

  if (settings_.show_pc)
    Append(dest, "%04x ", static_cast<unsigned>(wrapped_pc));

  u16 op2;

  // Size 2 - the op has a large immediate.
  if (opc->size == 2)
  {
    if (wrapped_pc + 1u >= binbuf_size)
    {
      if (settings_.show_hex)
        Append(dest, "%04x ???? ", static_cast<unsigned>(op1));
      dest += "; Insufficient data for large immediate";
      *pc += opc->size;
      return false;
    }

    op2 = binbuf[wrapped_pc + 1];
    if (settings_.show_hex)
      Append(dest, "%04x %04x ", static_cast<unsigned>(op1), static_cast<unsigned>(op2));
  }
  else
  {
    op2 = 0;
    if (settings_.show_hex)
      Append(dest, "%04x      ", static_cast<unsigned>(op1));
  }

  std::string opname = opc->name;
  if (is_extended)
  {
    opname += settings_.ext_separator;
    opname += opc_ext->name;
  }
  if (settings_.lower_case_ops)
    Common::ToLower(&opname);

  if (settings_.print_tabs)
  {
    dest += opname;
    dest += '\t';
  }
  else
  {
    dest += opname;
    if (opname.size() < 12)
      dest.append(12 - opname.size(), ' ');
  }

  if (opc->param_count > 0)
    dest += DisassembleParameters(*opc, op1, op2);

  // Handle opcode extension.
  if (is_extended)
  {
    if (opc->param_count > 0)
      dest += " ";

    dest += ": ";

    if (opc_ext->param_count > 0)
      dest += DisassembleParameters(*opc_ext, op1, op2);
  }

  if (opc->opcode_mask == 0)
  {
    // unknown opcode
    dest += "\t\t; *** UNKNOWN OPCODE ***";
  }

  *pc += is_extended ? opc_ext->size : opc->size;

  return true;
}
}  // namespace DSP
