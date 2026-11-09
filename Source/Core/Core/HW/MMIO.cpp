// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/MMIO.h"


#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"
#include "Core/HW/MMIOHandlers.h"

namespace MMIO
{
// Constant: the value read. A read only handling method: storing to a constant does not
// mean anything.
template <typename T>
ReadHandlingMethod<T> Constant(T value)
{
  ReadHandlingMethod<T> m = {};
  m.kind = MMIO_HANDLER_CONSTANT;
  m.value = value;
  return m;
}

// Nop: a write that does nothing at all. Write only, since reads should always at least
// return a value.
template <typename T>
WriteHandlingMethod<T> Nop()
{
  WriteHandlingMethod<T> m = {};
  m.kind = MMIO_HANDLER_NOP;
  return m;
}

// Direct: a pointer to the value where to read/write the data from, as well as a mask that is
// used to restrict reading/writing only to a given set of bits.
template <typename T>
ReadHandlingMethod<T> DirectRead(const T* addr, u32 mask)
{
  ReadHandlingMethod<T> m = {};
  m.kind = MMIO_HANDLER_DIRECT;
  m.ptr = addr;
  m.mask = mask;
  return m;
}
template <typename T>
WriteHandlingMethod<T> DirectWrite(T* addr, u32 mask)
{
  WriteHandlingMethod<T> m = {};
  m.kind = MMIO_HANDLER_DIRECT;
  m.ptr = addr;
  m.mask = mask;
  return m;
}

// Complex: a function called when a read or a write is executed, with its context. This gives
// complete control to the user as to what is going to happen during that read or write, but
// reduces the optimization potential.
template <typename T>
ReadHandlingMethod<T> ComplexRead(ReadFn<T> fn, uintptr_t ctx)
{
  ReadHandlingMethod<T> m = {};
  m.kind = MMIO_HANDLER_COMPLEX;
  m.fn = fn;
  m.ctx = ctx;
  return m;
}
template <typename T>
WriteHandlingMethod<T> ComplexWrite(WriteFn<T> fn, uintptr_t ctx)
{
  WriteHandlingMethod<T> m = {};
  m.kind = MMIO_HANDLER_COMPLEX;
  m.fn = fn;
  m.ctx = ctx;
  return m;
}

// Invalid: complex methods that display error messages.
template <typename T>
static T ReadInvalid(Core::System&, uintptr_t, u32 addr)
{
  ERROR_LOG_FMT(MEMMAP, "Trying to read {} bits from an invalid MMIO (addr={:08x})",
                8 * sizeof(T), addr);
  return 0;
}
template <typename T>
static void WriteInvalid(Core::System&, uintptr_t, u32 addr, T val)
{
  ERROR_LOG_FMT(MEMMAP, "Trying to write {} bits to an invalid MMIO (addr={:08x}, val={:08x})",
                8 * sizeof(T), addr, val);
}
template <typename T>
ReadHandlingMethod<T> InvalidRead()
{
  return ComplexRead<T>(ReadInvalid<T>);
}
template <typename T>
WriteHandlingMethod<T> InvalidWrite()
{
  return ComplexWrite<T>(WriteInvalid<T>);
}

// Converters to larger and smaller size. Probably the most complex of these
// handlers to implement. They do not define new handling method types but
// instead will internally use the types defined above.
template <typename T>
struct SmallerAccessSize
{
};
template <>
struct SmallerAccessSize<u16>
{
  typedef u8 value;
};
template <>
struct SmallerAccessSize<u32>
{
  typedef u16 value;
};

template <typename T>
struct LargerAccessSize
{
};
template <>
struct LargerAccessSize<u8>
{
  typedef u16 value;
};
template <>
struct LargerAccessSize<u16>
{
  typedef u32 value;
};

// To smaller: the context is the high part's handler, the low part's the next one; the parts are
// at the access's address, aligned as the access is (high at the register, low after it).
template <typename T>
static T ReadSmaller(Core::System& system, uintptr_t ctx, u32 addr)
{
  typedef typename SmallerAccessSize<T>::value ST;
  ReadHandler<ST>* const high_part = reinterpret_cast<ReadHandler<ST>*>(ctx);
  const u32 high_part_addr = addr & ~u32(sizeof(T) - 1);
  return (T(high_part[0].Read(system, high_part_addr)) << (8 * sizeof(ST))) |
         high_part[1].Read(system, high_part_addr + u32(sizeof(ST)));
}

template <typename T>
static void WriteSmaller(Core::System& system, uintptr_t ctx, u32 addr, T val)
{
  typedef typename SmallerAccessSize<T>::value ST;
  WriteHandler<ST>* const high_part = reinterpret_cast<WriteHandler<ST>*>(ctx);
  const u32 high_part_addr = addr & ~u32(sizeof(T) - 1);
  high_part[0].Write(system, high_part_addr, ST(val >> (8 * sizeof(ST))));
  high_part[1].Write(system, high_part_addr + u32(sizeof(ST)), ST(val));
}

template <typename T>
ReadHandlingMethod<T> ReadToSmaller(Mapping* mmio, u32 high_part_addr, u32 low_part_addr)
{
  typedef typename SmallerAccessSize<T>::value ST;
  ReadHandler<ST>* const high_part = &mmio->GetHandlerForRead<ST>(high_part_addr);
  ASSERT(&mmio->GetHandlerForRead<ST>(low_part_addr) == high_part + 1);
  return ComplexRead<T>(ReadSmaller<T>, reinterpret_cast<uintptr_t>(high_part));
}

template <typename T>
WriteHandlingMethod<T> WriteToSmaller(Mapping* mmio, u32 high_part_addr, u32 low_part_addr)
{
  typedef typename SmallerAccessSize<T>::value ST;
  WriteHandler<ST>* const high_part = &mmio->GetHandlerForWrite<ST>(high_part_addr);
  ASSERT(&mmio->GetHandlerForWrite<ST>(low_part_addr) == high_part + 1);
  return ComplexWrite<T>(WriteSmaller<T>, reinterpret_cast<uintptr_t>(high_part));
}

// To larger: the context is the larger handler with the shift / 8 in its low bits (a handler
// is aligned to more than 4).
template <typename T>
static T ReadLarger(Core::System& system, uintptr_t ctx, u32 addr)
{
  typedef typename LargerAccessSize<T>::value LT;
  ReadHandler<LT>* const large = reinterpret_cast<ReadHandler<LT>*>(ctx & ~uintptr_t(3));
  const u32 shift = u32(ctx & 3) * 8;
  return T(large->Read(system, addr & ~u32(sizeof(LT) - 1)) >> shift);
}

template <typename T>
ReadHandlingMethod<T> ReadToLarger(Mapping* mmio, u32 larger_addr, u32 shift)
{
  typedef typename LargerAccessSize<T>::value LT;
  ReadHandler<LT>* const large = &mmio->GetHandlerForRead<LT>(larger_addr);
  static_assert(alignof(ReadHandler<LT>) >= 4);
  ASSERT(shift % 8 == 0 && shift < 32);
  return ComplexRead<T>(ReadLarger<T>, reinterpret_cast<uintptr_t>(large) | (shift / 8));
}

// Implementation of the ReadHandler and WriteHandler class.
template <typename T>
void ReadHandler<T>::Visit(ReadHandlingMethodVisitor<T>& visitor)
{
  switch (m.kind)
  {
  case MMIO_HANDLER_CONSTANT:
    visitor.VisitConstant(m.value);
    return;
  case MMIO_HANDLER_DIRECT:
    visitor.VisitDirect(m.ptr, m.mask);
    return;
  case MMIO_HANDLER_COMPLEX:
    break;
  default:
    m = InvalidRead<T>();
    break;
  }
  visitor.VisitComplex(m.fn, m.ctx);
}

template <typename T>
void WriteHandler<T>::Visit(WriteHandlingMethodVisitor<T>& visitor)
{
  switch (m.kind)
  {
  case MMIO_HANDLER_NOP:
    visitor.VisitNop();
    return;
  case MMIO_HANDLER_DIRECT:
    visitor.VisitDirect(m.ptr, m.mask);
    return;
  case MMIO_HANDLER_COMPLEX:
    break;
  default:
    m = InvalidWrite<T>();
    break;
  }
  visitor.VisitComplex(m.fn, m.ctx);
}

/* The handler arrays, the system's zeroed pages: every handler invalid until a method is
 * registered, and only the pages of registered ones touched. */
template <typename H>
static H* NewHandlers(u32 count)
{
  return static_cast<H*>(Common::AllocateMemoryPages(count * sizeof(H)));
}

Mapping::Mapping()
    : m_read_handlers8(NewHandlers<ReadHandler<u8>>(NUM_MMIOS)),
      m_read_handlers16(NewHandlers<ReadHandler<u16>>(NUM_MMIOS / 2)),
      m_read_handlers32(NewHandlers<ReadHandler<u32>>(NUM_MMIOS / 4)),
      m_write_handlers8(NewHandlers<WriteHandler<u8>>(NUM_MMIOS)),
      m_write_handlers16(NewHandlers<WriteHandler<u16>>(NUM_MMIOS / 2)),
      m_write_handlers32(NewHandlers<WriteHandler<u32>>(NUM_MMIOS / 4))
{
}

template <typename H>
static void FreeHandlers(H* handlers, u32 count)
{
  if (handlers)
    Common::FreeMemoryPages(handlers, count * sizeof(H));
}

Mapping::~Mapping()
{
  FreeHandlers(m_read_handlers8, NUM_MMIOS);
  FreeHandlers(m_read_handlers16, NUM_MMIOS / 2);
  FreeHandlers(m_read_handlers32, NUM_MMIOS / 4);
  FreeHandlers(m_write_handlers8, NUM_MMIOS);
  FreeHandlers(m_write_handlers16, NUM_MMIOS / 2);
  FreeHandlers(m_write_handlers32, NUM_MMIOS / 4);
}

// Define all the public specializations that are exported in MMIOHandlers.h.
#define MaybeExtern
MMIO_PUBLIC_SPECIALIZATIONS()
#undef MaybeExtern
}  // namespace MMIO
