// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include <memory>

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

  virtual ~ControlReference();
  virtual ControlState State(const ControlState state = 0) = 0;
  virtual bool IsInput() const = 0;

  template <typename T>
  T GetState()
  {
    return ControllerEmu::ControlStateCast<T>(State());
  }

  int BoundCount() const;
  // Binds the expression to the devices in env. Builds a new binding and swaps it in, so a
  // thread reading the old one is never disturbed.
  void UpdateReference(ciface::ExpressionParser::ControlEnvironment& env);
  std::string GetExpression() const;
  // Whether there is an expression at all, without copying it.
  bool HasExpression() const;

  // Returns a human-readable error description when the given expression is invalid.
  std::optional<std::string> SetExpression(std::string expr);

  ControlState GetRange() const;
  void SetRange(ControlState range);

protected:
  ControlReference();
  ControlReference(const ControlReference&) = delete;
  ControlReference& operator=(const ControlReference&) = delete;

  // An expression with what it was parsed into. Never changed once published.
  struct Binding
  {
    quiesce_node_t node;  // first: the free callback casts it back
    std::string expression;
    ciface::ExpressionParser::ParseStatus status;
    std::unique_ptr<ciface::ExpressionParser::Expression> parsed;
  };
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

//
// InputReference
//
// Control reference for inputs
//
class InputReference : public ControlReference
{
public:
  InputReference();
  bool IsInput() const override;
  ControlState State(const ControlState state) override;
};

//
// OutputReference
//
// Control reference for outputs
//
class OutputReference : public ControlReference
{
public:
  OutputReference();
  bool IsInput() const override;
  ControlState State(const ControlState state) override;
};
