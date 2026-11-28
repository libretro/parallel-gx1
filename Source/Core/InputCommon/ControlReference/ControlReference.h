// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>

#include <retro_atomic.h>

#include "Common/Quiesce.h"
#include "InputCommon/ControlReference/ExpressionParser.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
template <typename T>
T ControlStateCast(ControlState value);
}

// ControlReference
//
// These are what you create to actually use the inputs, InputReference or OutputReference.
//
// After being bound to devices and controls with UpdateReference,
// each one can link to multiple devices and controls
// when you change a ControlReference's expression,
// you must use UpdateReference on it to rebind controls
//

class ControlReference
{
public:
  // Note: this is per thread.
  static void SetInputGate(bool enable);
  static bool GetInputGate();

  // An input reads its bound controls; an output sets them to state
  enum class Kind : u8
  {
    Input,
    Output,
  };

  explicit ControlReference(Kind kind = Kind::Input);
  ~ControlReference();

  // An input's value, or, for an output, 0 once its controls are set to state
  ControlState State(const ControlState state = 0);

  template <typename T>
  T GetState()
  {
    return ControllerEmu::ControlStateCast<T>(State());
  }

  int BoundCount() const;
  // Binds the expression to the devices in env. Builds a new binding and swaps it in, so a
  // thread reading the old one is never disturbed.
  void UpdateReference(ciface::ExpressionParser::ControlEnvironment& env);
  // A malloc'd copy of the expression for the caller to free, null when there is none.
  char* CopyExpression() const;
  // Whether there is an expression at all, without copying it.
  bool HasExpression() const;

  void SetExpression(const char* expr);
  // The expression is length characters, not terminated
  void SetExpression(const char* expr, size_t length);

  ControlState GetRange() const;
  void SetRange(ControlState range);

protected:
  ControlReference(const ControlReference&) = delete;
  ControlReference& operator=(const ControlReference&) = delete;

  // An expression with what it was parsed into, in one block. Never changed once published.
  struct Binding
  {
    quiesce_node_t node;  // first: the free callback casts it back
    ciface::ExpressionParser::ParseStatus status;
    // Owned, null when there is none
    ciface::ExpressionParser::Expression* parsed;
    size_t expression_length;
    char expression[1];  // terminated, allocated to fit
  };
  // Null when out of memory
  static Binding* NewBinding(const char* expr, size_t length);
  static void DeleteBinding(Binding* binding);
  static void FreeBinding(quiesce_node_t* node);
  // Only inside an InputReadScope.
  Binding* PeekBinding() const
  {
    return static_cast<Binding*>(retro_atomic_load_acquire_ptr(&m_binding));
  }
  void Publish(Binding* binding);

  mutable retro_atomic_ptr_t m_binding{};
  retro_atomic_int_t m_has_expression{};
  // The range as the bits of a double, 1.0 until set.
  retro_atomic_64_t m_range_bits;
  const Kind m_kind;
};

namespace ControllerEmu
{
template <>
inline bool ControlStateCast<bool>(ControlState value)
{
  // Round to nearest of 0 or 1.
  return std::lround(value) > 0;
}

template <>
inline int ControlStateCast<int>(ControlState value)
{
  return std::lround(value);
}

template <>
inline ControlState ControlStateCast<ControlState>(ControlState value)
{
  return value;
}
}  // namespace ControllerEmu

// A reference read for its value: an input, as it is made by default
using InputReference = ControlReference;
