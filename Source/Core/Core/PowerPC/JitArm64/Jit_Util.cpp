// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitArm64/Jit_Util.h"

#include "Common/Arm64Emitter.h"
#include "Common/Common.h"

#include "Core/HW/MMIO.h"
#include "Core/PowerPC/JitArm64/Jit.h"
#include "Core/System.h"

using namespace Arm64Gen;

// The store of src_reg to a MMIO register: nothing, a store to the register's backing memory,
// or a call to its write function.
template <typename T>
static void MMIOWrite(Core::System* system, ARM64XEmitter* emit, BitSet32 gprs_in_use,
                      BitSet32 fprs_in_use, ARM64Reg src_reg, u32 address,
                      const MMIO::WriteHandlingMethod<T>& m)
{
  const int sbits = 8 * sizeof(T);

  switch (m.kind)
  {
  case MMIO::MMIO_HANDLER_NOP:
    return;
  case MMIO::MMIO_HANDLER_DIRECT:
  {
    const s32 offset = emit->MOVPage2R(ARM64Reg::X0, m.ptr);
    const u32 all_ones = (1ULL << sbits) - 1;
    ARM64Reg reg = src_reg;

    if ((all_ones & m.mask) != all_ones)
    {
      emit->ANDI2R(ARM64Reg::W1, src_reg, m.mask, ARM64Reg::W1);
      reg = ARM64Reg::W1;
    }
    switch (sbits)
    {
    case 8:
      emit->STRB(IndexType::Unsigned, reg, ARM64Reg::X0, offset);
      break;
    case 16:
      emit->STRH(IndexType::Unsigned, reg, ARM64Reg::X0, offset);
      break;
    default:
      emit->STR(IndexType::Unsigned, reg, ARM64Reg::X0, offset);
      break;
    }
    return;
  }
  default:
  {
    ARM64FloatEmitter float_emit(emit);

    emit->ABI_PushRegisters(gprs_in_use);
    float_emit.ABI_PushRegisters(fprs_in_use, ARM64Reg::X1);

    emit->ABI_CallFunction(m.fn, system, m.ctx, address, src_reg);

    float_emit.ABI_PopRegisters(fprs_in_use, ARM64Reg::X1);
    emit->ABI_PopRegisters(gprs_in_use);
    return;
  }
  }
}

// The load from a MMIO register into dst_reg, extended to 32 bits: a constant, a load from the
// register's backing memory, or a call to its read function.
template <typename T>
static void MMIORead(Core::System* system, ARM64XEmitter* emit, BitSet32 gprs_in_use,
                     BitSet32 fprs_in_use, ARM64Reg dst_reg, u32 address, bool sign_extend,
                     const MMIO::ReadHandlingMethod<T>& m)
{
  const int sbits = 8 * sizeof(T);

  switch (m.kind)
  {
  case MMIO::MMIO_HANDLER_CONSTANT:
    emit->MOVI2R(dst_reg, m.value);
    if (sign_extend)
      emit->SBFM(dst_reg, dst_reg, 0, sbits - 1);
    return;
  case MMIO::MMIO_HANDLER_DIRECT:
  {
    const s32 offset = emit->MOVPage2R(ARM64Reg::X0, m.ptr);
    // If we do not need to mask, we can do the sign extend while loading from memory. If masking
    // is required, we have to first zero extend, then mask, then sign extend if needed (1 instr
    // vs. ~4).
    const u32 all_ones = (1ULL << sbits) - 1;
    const bool masked = (all_ones & m.mask) != all_ones;
    const bool extend_in_load = sign_extend && !masked;

    switch (sbits)
    {
    case 8:
      if (extend_in_load)
        emit->LDRSB(IndexType::Unsigned, dst_reg, ARM64Reg::X0, offset);
      else
        emit->LDRB(IndexType::Unsigned, dst_reg, ARM64Reg::X0, offset);
      break;
    case 16:
      if (extend_in_load)
        emit->LDRSH(IndexType::Unsigned, dst_reg, ARM64Reg::X0, offset);
      else
        emit->LDRH(IndexType::Unsigned, dst_reg, ARM64Reg::X0, offset);
      break;
    default:
      emit->LDR(IndexType::Unsigned, dst_reg, ARM64Reg::X0, offset);
      break;
    }
    if (masked)
    {
      emit->ANDI2R(dst_reg, dst_reg, m.mask, ARM64Reg::W0);
      if (sign_extend)
        emit->SBFM(dst_reg, dst_reg, 0, sbits - 1);
    }
    return;
  }
  default:
  {
    ARM64FloatEmitter float_emit(emit);

    emit->ABI_PushRegisters(gprs_in_use);
    float_emit.ABI_PushRegisters(fprs_in_use, ARM64Reg::X1);

    emit->ABI_CallFunction(m.fn, system, m.ctx, address);

    if (sign_extend)
      emit->SBFM(dst_reg, ARM64Reg::W0, 0, sbits - 1);
    else
      emit->UBFM(dst_reg, ARM64Reg::W0, 0, sbits - 1);

    float_emit.ABI_PopRegisters(fprs_in_use, ARM64Reg::X1);
    emit->ABI_PopRegisters(gprs_in_use);
    return;
  }
  }
}

void SwapPairs(ARM64XEmitter* emit, ARM64Reg dst_reg, ARM64Reg src_reg, u32 flags)
{
  if (flags & BackPatchInfo::FLAG_SIZE_32)
    emit->ROR(dst_reg, src_reg, 32);
  else if (flags & BackPatchInfo::FLAG_SIZE_16)
    emit->ROR(dst_reg, src_reg, 16);
  else
    emit->REV16(dst_reg, src_reg);
}

void ByteswapAfterLoad(ARM64XEmitter* emit, ARM64FloatEmitter* float_emit, ARM64Reg dst_reg,
                       ARM64Reg src_reg, u32 flags, bool is_reversed, bool is_extended)
{
  if (is_reversed == !(flags & BackPatchInfo::FLAG_REVERSE))
  {
    if (flags & BackPatchInfo::FLAG_SIZE_64)
    {
      if (flags & BackPatchInfo::FLAG_FLOAT)
        float_emit->REV64(8, dst_reg, src_reg);
      else
        emit->REV64(dst_reg, src_reg);

      src_reg = dst_reg;
    }
    else if (flags & BackPatchInfo::FLAG_SIZE_32)
    {
      if (flags & BackPatchInfo::FLAG_FLOAT)
        float_emit->REV32(8, dst_reg, src_reg);
      else
        emit->REV32(dst_reg, src_reg);

      src_reg = dst_reg;
    }
    else if (flags & BackPatchInfo::FLAG_SIZE_16)
    {
      if (flags & BackPatchInfo::FLAG_FLOAT)
        float_emit->REV16(8, dst_reg, src_reg);
      else
        emit->REV16(dst_reg, src_reg);

      src_reg = dst_reg;
    }
  }

  if (!is_extended && (flags & BackPatchInfo::FLAG_EXTEND))
  {
    emit->SXTH(dst_reg, src_reg);
    src_reg = dst_reg;
  }

  if (dst_reg != src_reg)
  {
    if (flags & BackPatchInfo::FLAG_FLOAT)
      float_emit->ORR(dst_reg, src_reg, src_reg);
    else
      emit->MOV(dst_reg, src_reg);
  }
}

ARM64Reg ByteswapBeforeStore(ARM64XEmitter* emit, ARM64FloatEmitter* float_emit, ARM64Reg tmp_reg,
                             ARM64Reg src_reg, u32 flags, bool want_reversed)
{
  // Byteswapping zero is still zero.
  // We'd typically expect a writable register to be passed in, but recognize
  // WZR for optimization purposes.
  if ((flags & BackPatchInfo::FLAG_FLOAT) == 0 && src_reg == ARM64Reg::WZR)
    return ARM64Reg::WZR;

  ARM64Reg dst_reg = src_reg;

  if (want_reversed == !(flags & BackPatchInfo::FLAG_REVERSE))
  {
    if (flags & BackPatchInfo::FLAG_SIZE_64)
    {
      dst_reg = tmp_reg;

      if (flags & BackPatchInfo::FLAG_FLOAT)
        float_emit->REV64(8, dst_reg, src_reg);
      else
        emit->REV64(dst_reg, src_reg);
    }
    else if (flags & BackPatchInfo::FLAG_SIZE_32)
    {
      dst_reg = tmp_reg;

      if (flags & BackPatchInfo::FLAG_FLOAT)
        float_emit->REV32(8, dst_reg, src_reg);
      else
        emit->REV32(dst_reg, src_reg);
    }
    else if (flags & BackPatchInfo::FLAG_SIZE_16)
    {
      dst_reg = tmp_reg;

      if (flags & BackPatchInfo::FLAG_FLOAT)
        float_emit->REV16(8, dst_reg, src_reg);
      else
        emit->REV16(dst_reg, src_reg);
    }
  }

  return dst_reg;
}

void MMIOLoadToReg(Core::System& system, MMIO::Mapping* mmio, ARM64XEmitter* emit,
                   ARM64FloatEmitter* float_emit, BitSet32 gprs_in_use, BitSet32 fprs_in_use,
                   ARM64Reg dst_reg, u32 address, u32 flags)
{
  ASSERT(!(flags & BackPatchInfo::FLAG_FLOAT));

  const bool sign_extend = (flags & BackPatchInfo::FLAG_EXTEND) != 0;
  if (flags & BackPatchInfo::FLAG_SIZE_8)
    MMIORead<u8>(&system, emit, gprs_in_use, fprs_in_use, dst_reg, address, sign_extend,
                 mmio->GetHandlerForRead<u8>(address).Method());
  else if (flags & BackPatchInfo::FLAG_SIZE_16)
    MMIORead<u16>(&system, emit, gprs_in_use, fprs_in_use, dst_reg, address, sign_extend,
                  mmio->GetHandlerForRead<u16>(address).Method());
  else if (flags & BackPatchInfo::FLAG_SIZE_32)
    MMIORead<u32>(&system, emit, gprs_in_use, fprs_in_use, dst_reg, address, sign_extend,
                  mmio->GetHandlerForRead<u32>(address).Method());

  ByteswapAfterLoad(emit, float_emit, dst_reg, dst_reg, flags, false, true);
}

void MMIOWriteRegToAddr(Core::System& system, MMIO::Mapping* mmio, ARM64XEmitter* emit,
                        ARM64FloatEmitter* float_emit, BitSet32 gprs_in_use, BitSet32 fprs_in_use,
                        ARM64Reg src_reg, u32 address, u32 flags)
{
  ASSERT(!(flags & BackPatchInfo::FLAG_FLOAT));

  src_reg = ByteswapBeforeStore(emit, float_emit, ARM64Reg::W1, src_reg, flags, false);

  if (flags & BackPatchInfo::FLAG_SIZE_8)
    MMIOWrite<u8>(&system, emit, gprs_in_use, fprs_in_use, src_reg, address,
                  mmio->GetHandlerForWrite<u8>(address).Method());
  else if (flags & BackPatchInfo::FLAG_SIZE_16)
    MMIOWrite<u16>(&system, emit, gprs_in_use, fprs_in_use, src_reg, address,
                   mmio->GetHandlerForWrite<u16>(address).Method());
  else if (flags & BackPatchInfo::FLAG_SIZE_32)
    MMIOWrite<u32>(&system, emit, gprs_in_use, fprs_in_use, src_reg, address,
                   mmio->GetHandlerForWrite<u32>(address).Method());
}
