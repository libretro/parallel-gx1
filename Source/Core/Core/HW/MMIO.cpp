// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/MMIO.h"

#include <cstdlib>
#include <functional>
#include <utility>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Core/HW/MMIOHandlers.h"

namespace MMIO
{
// Base classes for the two handling method hierarchies. Note that a single
// class can inherit from both.
//
// At the moment the only common element between all the handling method is
// that they should be able to accept a visitor of the appropriate type.
template <typename T>
class ReadHandlingMethod
{
public:
  virtual ~ReadHandlingMethod() = default;
  virtual void AcceptReadVisitor(ReadHandlingMethodVisitor<T>& v) const = 0;
};
template <typename T>
class WriteHandlingMethod
{
public:
  virtual ~WriteHandlingMethod() = default;
  virtual void AcceptWriteVisitor(WriteHandlingMethodVisitor<T>& v) const = 0;
};

// Constant: handling method holds a single integer and passes it to the
// visitor. This is a read only handling method: storing to a constant does not
// mean anything.
template <typename T>
class ConstantHandlingMethod : public ReadHandlingMethod<T>
{
public:
  explicit ConstantHandlingMethod(T value) : value_(value) {}
  ~ConstantHandlingMethod() override = default;
  void AcceptReadVisitor(ReadHandlingMethodVisitor<T>& v) const override
  {
    v.VisitConstant(value_);
  }

private:
  T value_;
};
template <typename T>
ReadHandlingMethod<T>* Constant(T value)
{
  return new ConstantHandlingMethod<T>(value);
}

// Nop: extremely simple write handling method that does nothing at all, only
// respond to visitors and dispatch to the correct method. This is write only
// since reads should always at least return a value.
template <typename T>
class NopHandlingMethod : public WriteHandlingMethod<T>
{
public:
  NopHandlingMethod() {}
  ~NopHandlingMethod() override = default;
  void AcceptWriteVisitor(WriteHandlingMethodVisitor<T>& v) const override { v.VisitNop(); }
};
template <typename T>
WriteHandlingMethod<T>* Nop()
{
  return new NopHandlingMethod<T>();
}

// Direct: handling method holds a pointer to the value where to read/write the
// data from, as well as a mask that is used to restrict reading/writing only
// to a given set of bits.
template <typename T>
class DirectHandlingMethod : public ReadHandlingMethod<T>, public WriteHandlingMethod<T>
{
public:
  DirectHandlingMethod(T* addr, u32 mask) : addr_(addr), mask_(mask) {}
  ~DirectHandlingMethod() override = default;
  void AcceptReadVisitor(ReadHandlingMethodVisitor<T>& v) const override
  {
    v.VisitDirect(addr_, mask_);
  }

  void AcceptWriteVisitor(WriteHandlingMethodVisitor<T>& v) const override
  {
    v.VisitDirect(addr_, mask_);
  }

private:
  T* addr_;
  u32 mask_;
};
template <typename T>
ReadHandlingMethod<T>* DirectRead(const T* addr, u32 mask)
{
  return new DirectHandlingMethod<T>(const_cast<T*>(addr), mask);
}
template <typename T>
WriteHandlingMethod<T>* DirectWrite(T* addr, u32 mask)
{
  return new DirectHandlingMethod<T>(addr, mask);
}

// Complex: holds a lambda that is called when a read or a write is executed.
// This gives complete control to the user as to what is going to happen during
// that read or write, but reduces the optimization potential.
template <typename T>
class ComplexHandlingMethod : public ReadHandlingMethod<T>, public WriteHandlingMethod<T>
{
public:
  explicit ComplexHandlingMethod(std::function<T(Core::System&, u32)> read_lambda)
      : read_lambda_(std::move(read_lambda)), write_lambda_(InvalidWriteLambda())
  {
  }

  explicit ComplexHandlingMethod(std::function<void(Core::System&, u32, T)> write_lambda)
      : read_lambda_(InvalidReadLambda()), write_lambda_(std::move(write_lambda))
  {
  }

  ~ComplexHandlingMethod() override = default;
  void AcceptReadVisitor(ReadHandlingMethodVisitor<T>& v) const override
  {
    v.VisitComplex(&read_lambda_);
  }

  void AcceptWriteVisitor(WriteHandlingMethodVisitor<T>& v) const override
  {
    v.VisitComplex(&write_lambda_);
  }

private:
  std::function<T(Core::System&, u32)> InvalidReadLambda() const
  {
    return [](Core::System&, u32) {
      DEBUG_ASSERT_MSG(MEMMAP, 0,
                       "Called the read lambda on a write "
                       "complex handler.");
      return 0;
    };
  }

  std::function<void(Core::System&, u32, T)> InvalidWriteLambda() const
  {
    return [](Core::System&, u32, T) {
      DEBUG_ASSERT_MSG(MEMMAP, 0,
                       "Called the write lambda on a read "
                       "complex handler.");
    };
  }

  std::function<T(Core::System&, u32)> read_lambda_;
  std::function<void(Core::System&, u32, T)> write_lambda_;
};
template <typename T>
ReadHandlingMethod<T>* ComplexRead(std::function<T(Core::System&, u32)> lambda)
{
  return new ComplexHandlingMethod<T>(std::move(lambda));
}
template <typename T>
WriteHandlingMethod<T>* ComplexWrite(std::function<void(Core::System&, u32, T)> lambda)
{
  return new ComplexHandlingMethod<T>(std::move(lambda));
}

// Invalid: specialization of the complex handling type with lambdas that
// display error messages.
template <typename T>
ReadHandlingMethod<T>* InvalidRead()
{
  return ComplexRead<T>([](Core::System&, u32 addr) {
    ERROR_LOG_FMT(MEMMAP, "Trying to read {} bits from an invalid MMIO (addr={:08x})",
                  8 * sizeof(T), addr);
    return 0;
  });
}
template <typename T>
WriteHandlingMethod<T>* InvalidWrite()
{
  return ComplexWrite<T>([](Core::System&, u32 addr, T val) {
    ERROR_LOG_FMT(MEMMAP, "Trying to write {} bits to an invalid MMIO (addr={:08x}, val={:08x})",
                  8 * sizeof(T), addr, val);
  });
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

template <typename T>
ReadHandlingMethod<T>* ReadToSmaller(Mapping* mmio, u32 high_part_addr, u32 low_part_addr)
{
  typedef typename SmallerAccessSize<T>::value ST;

  ReadHandler<ST>* high_part = &mmio->GetHandlerForRead<ST>(high_part_addr);
  ReadHandler<ST>* low_part = &mmio->GetHandlerForRead<ST>(low_part_addr);

  // TODO(delroth): optimize
  return ComplexRead<T>([=](Core::System& system, u32 addr) {
    return ((T)high_part->Read(system, high_part_addr) << (8 * sizeof(ST))) |
           low_part->Read(system, low_part_addr);
  });
}

template <typename T>
WriteHandlingMethod<T>* WriteToSmaller(Mapping* mmio, u32 high_part_addr, u32 low_part_addr)
{
  typedef typename SmallerAccessSize<T>::value ST;

  WriteHandler<ST>* high_part = &mmio->GetHandlerForWrite<ST>(high_part_addr);
  WriteHandler<ST>* low_part = &mmio->GetHandlerForWrite<ST>(low_part_addr);

  // TODO(delroth): optimize
  return ComplexWrite<T>([=](Core::System& system, u32 addr, T val) {
    high_part->Write(system, high_part_addr, val >> (8 * sizeof(ST)));
    low_part->Write(system, low_part_addr, (ST)val);
  });
}

template <typename T>
ReadHandlingMethod<T>* ReadToLarger(Mapping* mmio, u32 larger_addr, u32 shift)
{
  typedef typename LargerAccessSize<T>::value LT;

  ReadHandler<LT>* large = &mmio->GetHandlerForRead<LT>(larger_addr);

  // TODO(delroth): optimize
  return ComplexRead<T>([large, shift](Core::System& system, u32 addr) {
    return large->Read(system, addr & ~(sizeof(LT) - 1)) >> shift;
  });
}

// Implementation of the ReadHandler and WriteHandler class. There is a lot of
// redundant code between these two classes but trying to abstract it away
// brings more trouble than it fixes.
template <typename T>
void ReadHandler<T>::Visit(ReadHandlingMethodVisitor<T>& visitor)
{
  if (!m_Method)
    InitializeInvalid();

  m_Method->AcceptReadVisitor(visitor);
}

template <typename T>
void ReadHandler<T>::ResetMethod(ReadHandlingMethod<T>* method)
{
  struct KindVisitor : public ReadHandlingMethodVisitor<T>
  {
    ReadHandler<T>* h;
    void VisitConstant(T value) override
    {
      h->m_kind = MMIO_HANDLER_CONSTANT;
      h->m_value = value;
    }
    void VisitDirect(const T* addr, u32 mask) override
    {
      h->m_kind = MMIO_HANDLER_DIRECT;
      h->m_ptr = addr;
      h->m_mask = mask;
    }
    void VisitComplex(const std::function<T(Core::System&, u32)>* lambda) override
    {
      h->m_kind = MMIO_HANDLER_COMPLEX;
      h->m_func = lambda;
    }
  };
  KindVisitor v;

  delete m_Method;
  m_Method = method;
  m_kind = MMIO_HANDLER_INVALID;
  if (!method)
    return;
  v.h = this;
  method->AcceptReadVisitor(v);
}

template <typename T>
void ReadHandler<T>::Free()
{
  ResetMethod(nullptr);
}

template <typename T>
void ReadHandler<T>::InitializeInvalid()
{
  ResetMethod(InvalidRead<T>());
}

template <typename T>
void WriteHandler<T>::Visit(WriteHandlingMethodVisitor<T>& visitor)
{
  if (!m_Method)
    InitializeInvalid();

  m_Method->AcceptWriteVisitor(visitor);
}

template <typename T>
void WriteHandler<T>::ResetMethod(WriteHandlingMethod<T>* method)
{
  struct KindVisitor : public WriteHandlingMethodVisitor<T>
  {
    WriteHandler<T>* h;
    void VisitNop() override { h->m_kind = MMIO_HANDLER_NOP; }
    void VisitDirect(T* ptr, u32 mask) override
    {
      h->m_kind = MMIO_HANDLER_DIRECT;
      h->m_ptr = ptr;
      h->m_mask = mask;
    }
    void VisitComplex(const std::function<void(Core::System&, u32, T)>* lambda) override
    {
      h->m_kind = MMIO_HANDLER_COMPLEX;
      h->m_func = lambda;
    }
  };
  KindVisitor v;

  delete m_Method;
  m_Method = method;
  m_kind = MMIO_HANDLER_INVALID;
  if (!method)
    return;
  v.h = this;
  method->AcceptWriteVisitor(v);
}

template <typename T>
void WriteHandler<T>::Free()
{
  ResetMethod(nullptr);
}

template <typename T>
void WriteHandler<T>::InitializeInvalid()
{
  ResetMethod(InvalidWrite<T>());
}

/* The handler arrays, zeroed: every handler invalid until a method is registered. */
Mapping::Mapping()
    : m_read_handlers8(static_cast<ReadHandler<u8>*>(calloc(NUM_MMIOS, sizeof(ReadHandler<u8>)))),
      m_read_handlers16(
          static_cast<ReadHandler<u16>*>(calloc(NUM_MMIOS / 2, sizeof(ReadHandler<u16>)))),
      m_read_handlers32(
          static_cast<ReadHandler<u32>*>(calloc(NUM_MMIOS / 4, sizeof(ReadHandler<u32>)))),
      m_write_handlers8(
          static_cast<WriteHandler<u8>*>(calloc(NUM_MMIOS, sizeof(WriteHandler<u8>)))),
      m_write_handlers16(
          static_cast<WriteHandler<u16>*>(calloc(NUM_MMIOS / 2, sizeof(WriteHandler<u16>)))),
      m_write_handlers32(
          static_cast<WriteHandler<u32>*>(calloc(NUM_MMIOS / 4, sizeof(WriteHandler<u32>))))
{
}

/* Only the handlers given a method have one to delete. */
template <typename H>
static void FreeHandlers(H* handlers, u32 count)
{
  u32 i;
  if (!handlers)
    return;
  for (i = 0; i < count; i++)
    if (handlers[i].m_Method)
      handlers[i].Free();
  free(handlers);
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
