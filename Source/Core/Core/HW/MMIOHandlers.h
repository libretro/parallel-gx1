// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>

#include "Common/CommonTypes.h"

// All the templated and very repetitive MMIO-related code is isolated in this
// file for easier reading. It mostly contains code related to handling methods
// (including the declaration of the public functions for creating handling
// method objects), visitors for these handling methods, and interface of the
// handler classes.
//
// This code is very genericized (aka. lots of templates) in order to handle
// u8/u16/u32 with the same code while providing type safety: it is impossible
// to mix code from these types, and the type system enforces it.
//
// A method, and the handler it becomes, is plain C data: a kind, the value or pointer of the
// simple kinds, and for a complex one a function and a word of context it is called with (the
// index, object or register its lambda used to capture). Nothing is allocated or destroyed.

namespace Core
{
class System;
}
namespace MMIO
{
class Mapping;

// The kinds of handler, the first zero: a handler of an address nothing registered.
enum
{
  MMIO_HANDLER_INVALID = 0,
  MMIO_HANDLER_CONSTANT,
  MMIO_HANDLER_DIRECT,
  MMIO_HANDLER_NOP,
  MMIO_HANDLER_COMPLEX
};

// A complex read or write: called with the context it was registered with.
template <typename T>
using ReadFn = T (*)(Core::System& system, uintptr_t ctx, u32 addr);
template <typename T>
using WriteFn = void (*)(Core::System& system, uintptr_t ctx, u32 addr, T val);

// Read and write handling methods are separated for type safety. On top of
// that, some handling methods require different arguments for reads and writes
// (Complex, for example).
template <typename T>
struct ReadHandlingMethod
{
  ReadFn<T> fn;
  uintptr_t ctx;
  const T* ptr;
  u32 mask;
  T value;
  u8 kind;
};
template <typename T>
struct WriteHandlingMethod
{
  WriteFn<T> fn;
  uintptr_t ctx;
  T* ptr;
  u32 mask;
  u8 kind;
};

// Constant: use when the value read on this MMIO is always the same. This is
// only for reads.
template <typename T>
ReadHandlingMethod<T> Constant(T value);

// Nop: use for writes that shouldn't have any effect and shouldn't log an
// error either.
template <typename T>
WriteHandlingMethod<T> Nop();

// Direct: use when all the MMIO does is read/write the given value to/from a
// global variable, with an optional mask applied on the read/written value.
template <typename T>
ReadHandlingMethod<T> DirectRead(const T* addr, u32 mask = 0xFFFFFFFF);
template <typename T>
WriteHandlingMethod<T> DirectWrite(T* addr, u32 mask = 0xFFFFFFFF);

// Complex: use when no other handling method fits your needs: a function called for the
// read or write, with ctx (a captureless lambda converts to one).
template <typename T>
ReadHandlingMethod<T> ComplexRead(ReadFn<T> fn, uintptr_t ctx = 0);
template <typename T>
WriteHandlingMethod<T> ComplexWrite(WriteFn<T> fn, uintptr_t ctx = 0);

// Invalid: log an error and return -1 in case of a read. These are the default
// handlers set for all MMIO types.
template <typename T>
ReadHandlingMethod<T> InvalidRead();
template <typename T>
WriteHandlingMethod<T> InvalidWrite();

// {Read,Write}To{Smaller,Larger}: these functions are not themselves handling
// methods but will try to combine accesses to two handlers into one new
// handler object.
//
// This is used for example when 32 bit reads have the exact same handling as
// 16 bit. Handlers need to be registered for both 32 and 16, and it would be
// repetitive and unoptimal to require users to write the same handling code in
// both cases. Instead, an MMIO module can simply define all handlers in terms
// of 16 bit reads, then use ReadToSmaller<u32> to convert u32 reads to u16
// reads.
//
// The low part is the one after the high part (as every caller registers them).
//
// Warning: unlike the other handling methods, *ToSmaller are obviously not
// available for u8, and *ToLarger are not available for u32.
template <typename T>
ReadHandlingMethod<T> ReadToSmaller(Mapping* mmio, u32 high_part_addr, u32 low_part_addr);
template <typename T>
WriteHandlingMethod<T> WriteToSmaller(Mapping* mmio, u32 high_part_addr, u32 low_part_addr);
template <typename T>
ReadHandlingMethod<T> ReadToLarger(Mapping* mmio, u32 larger_addr, u32 shift);

// Use these visitors interfaces if you need to write code that performs
// different actions based on the handling method used by a handler. Write your
// visitor implementing that interface, then use handler->VisitHandlingMethod
// to run the proper function.
template <typename T>
class ReadHandlingMethodVisitor
{
public:
  virtual void VisitConstant(T value) = 0;
  virtual void VisitDirect(const T* addr, u32 mask) = 0;
  virtual void VisitComplex(ReadFn<T> fn, uintptr_t ctx) = 0;
};
template <typename T>
class WriteHandlingMethodVisitor
{
public:
  virtual void VisitNop() = 0;
  virtual void VisitDirect(T* addr, u32 mask) = 0;
  virtual void VisitComplex(WriteFn<T> fn, uintptr_t ctx) = 0;
};

// These classes are INTERNAL. Do not use outside of the MMIO implementation
// code. Unfortunately, because we want to make Read() and Write() fast and
// inlinable, we need to provide some of the implementation of these two
// classes here and can't just use a forward declaration.
//
// A handler is its method, zero when none was registered for its address (an invalid one,
// made on first use): the mapping's arrays are the system's zeroed pages.
template <typename T>
struct ReadHandler
{
  ReadHandlingMethod<T> m;

  // Entry point for read handling method visitors.
  void Visit(ReadHandlingMethodVisitor<T>& visitor);

  T Read(Core::System& system, u32 addr)
  {
    switch (m.kind)
    {
    case MMIO_HANDLER_CONSTANT:
      return m.value;
    case MMIO_HANDLER_DIRECT:
      return static_cast<T>(*m.ptr & m.mask);
    case MMIO_HANDLER_COMPLEX:
      return m.fn(system, m.ctx, addr);
    default:
      m = InvalidRead<T>();
      return m.fn(system, m.ctx, addr);
    }
  }
};
template <typename T>
struct WriteHandler
{
  WriteHandlingMethod<T> m;

  // Entry point for write handling method visitors.
  void Visit(WriteHandlingMethodVisitor<T>& visitor);

  void Write(Core::System& system, u32 addr, T val)
  {
    switch (m.kind)
    {
    case MMIO_HANDLER_NOP:
      return;
    case MMIO_HANDLER_DIRECT:
      *m.ptr = static_cast<T>(val & m.mask);
      return;
    case MMIO_HANDLER_COMPLEX:
      m.fn(system, m.ctx, addr, val);
      return;
    default:
      m = InvalidWrite<T>();
      m.fn(system, m.ctx, addr, val);
      return;
    }
  }
};

// Boilerplate boilerplate boilerplate.
//
// This is used to be able to avoid putting the templates implementation in the
// header files and slow down compilation times. Instead, we declare 3
// specializations in the header file as already implemented in another
// compilation unit: u8, u16, u32.
//
// The "MaybeExtern" is there because that same macro is used for declaration
// (where MaybeExtern = "extern") and definition (MaybeExtern = "").
#define MMIO_GENERIC_PUBLIC_SPECIALIZATIONS(MaybeExtern, T)                                        \
  MaybeExtern template ReadHandlingMethod<T> Constant<T>(T value);                                \
  MaybeExtern template WriteHandlingMethod<T> Nop<T>();                                           \
  MaybeExtern template ReadHandlingMethod<T> DirectRead(const T* addr, u32 mask);                 \
  MaybeExtern template WriteHandlingMethod<T> DirectWrite(T* addr, u32 mask);                     \
  MaybeExtern template ReadHandlingMethod<T> ComplexRead<T>(ReadFn<T> fn, uintptr_t ctx);         \
  MaybeExtern template WriteHandlingMethod<T> ComplexWrite<T>(WriteFn<T> fn, uintptr_t ctx);      \
  MaybeExtern template ReadHandlingMethod<T> InvalidRead<T>();                                    \
  MaybeExtern template WriteHandlingMethod<T> InvalidWrite<T>();                                  \
  MaybeExtern template struct ReadHandler<T>;                                                      \
  MaybeExtern template struct WriteHandler<T>

#define MMIO_SPECIAL_PUBLIC_SPECIALIZATIONS(MaybeExtern)                                           \
  MaybeExtern template ReadHandlingMethod<u16> ReadToSmaller(Mapping* mmio, u32 high_part_addr,   \
                                                              u32 low_part_addr);                  \
  MaybeExtern template ReadHandlingMethod<u32> ReadToSmaller(Mapping* mmio, u32 high_part_addr,   \
                                                              u32 low_part_addr);                  \
  MaybeExtern template WriteHandlingMethod<u16> WriteToSmaller(Mapping* mmio, u32 high_part_addr, \
                                                                u32 low_part_addr);                \
  MaybeExtern template WriteHandlingMethod<u32> WriteToSmaller(Mapping* mmio, u32 high_part_addr, \
                                                                u32 low_part_addr);                \
  MaybeExtern template ReadHandlingMethod<u8> ReadToLarger(Mapping* mmio, u32 larger_addr,        \
                                                            u32 shift);                            \
  MaybeExtern template ReadHandlingMethod<u16> ReadToLarger(Mapping* mmio, u32 larger_addr,       \
                                                             u32 shift)

#define MMIO_PUBLIC_SPECIALIZATIONS()                                                              \
  MMIO_GENERIC_PUBLIC_SPECIALIZATIONS(MaybeExtern, u8);                                            \
  MMIO_GENERIC_PUBLIC_SPECIALIZATIONS(MaybeExtern, u16);                                           \
  MMIO_GENERIC_PUBLIC_SPECIALIZATIONS(MaybeExtern, u32);                                           \
  MMIO_SPECIAL_PUBLIC_SPECIALIZATIONS(MaybeExtern);

#define MaybeExtern extern
MMIO_PUBLIC_SPECIALIZATIONS()
#undef MaybeExtern
}  // namespace MMIO
