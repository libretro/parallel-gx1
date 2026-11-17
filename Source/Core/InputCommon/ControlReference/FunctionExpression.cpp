// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControlReference/FunctionExpression.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <type_traits>

namespace ciface::ExpressionParser
{

// usage: toggle(toggle_state_input, [clear_state_input])
class ToggleExpression final : public FunctionExpression
{
public:
  ToggleExpression() : FunctionExpression(FunctionKind::Toggle) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    // Optional 2nd argument for clearing state:
    if (GetArgCount() == 1 || GetArgCount() == 2)
      return true;
    else
      return false;
  }

  ControlState GetValue()
  {
    const ControlState inner_value = GetArg(0).GetValue();

    if (inner_value < CONDITION_THRESHOLD)
    {
      m_released = true;
    }
    else if (m_released && inner_value > CONDITION_THRESHOLD)
    {
      m_released = false;
      m_state ^= true;
    }

    if (2 == GetArgCount() && GetArg(1).GetValue() > CONDITION_THRESHOLD)
    {
      m_state = false;
    }

    return m_state;
  }

  bool m_released{};
  bool m_state{};
};

// usage: not(expression)
class NotExpression final : public FunctionExpression
{
public:
  NotExpression() : FunctionExpression(FunctionKind::Not) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return 1.0 - GetArg(0).GetValue(); }
  void SetValue(ControlState value) { GetArg(0).SetValue(1.0 - value); }
};

// usage: abs(expression)
class AbsExpression final : public FunctionExpression
{
public:
  AbsExpression() : FunctionExpression(FunctionKind::Abs) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::abs(GetArg(0).GetValue()); }
};

// usage: sin(expression)
class SinExpression final : public FunctionExpression
{
public:
  SinExpression() : FunctionExpression(FunctionKind::Sin) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::sin(GetArg(0).GetValue()); }
};

// usage: cos(expression)
class CosExpression final : public FunctionExpression
{
public:
  CosExpression() : FunctionExpression(FunctionKind::Cos) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::cos(GetArg(0).GetValue()); }
};

// usage: tan(expression)
class TanExpression final : public FunctionExpression
{
public:
  TanExpression() : FunctionExpression(FunctionKind::Tan) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::tan(GetArg(0).GetValue()); }
};

// usage: asin(expression)
class ASinExpression final : public FunctionExpression
{
public:
  ASinExpression() : FunctionExpression(FunctionKind::ASin) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::asin(GetArg(0).GetValue()); }
};

// usage: acos(expression)
class ACosExpression final : public FunctionExpression
{
public:
  ACosExpression() : FunctionExpression(FunctionKind::ACos) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::acos(GetArg(0).GetValue()); }
};

// usage: atan(expression)
class ATanExpression final : public FunctionExpression
{
public:
  ATanExpression() : FunctionExpression(FunctionKind::ATan) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::atan(GetArg(0).GetValue()); }
};

// usage: atan2(y, x)
class ATan2Expression final : public FunctionExpression
{
public:
  ATan2Expression() : FunctionExpression(FunctionKind::ATan2) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 2)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::atan2(GetArg(0).GetValue(), GetArg(1).GetValue()); }
};

// usage: sqrt(expression)
class SqrtExpression final : public FunctionExpression
{
public:
  SqrtExpression() : FunctionExpression(FunctionKind::Sqrt) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::sqrt(GetArg(0).GetValue()); }
};

// usage: pow(base, exponent)
class PowExpression final : public FunctionExpression
{
public:
  PowExpression() : FunctionExpression(FunctionKind::Pow) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 2)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::pow(GetArg(0).GetValue(), GetArg(1).GetValue()); }
};

// usage: min(a, b)
class MinExpression final : public FunctionExpression
{
public:
  MinExpression() : FunctionExpression(FunctionKind::Min) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 2)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::min(GetArg(0).GetValue(), GetArg(1).GetValue()); }
};

// usage: max(a, b)
class MaxExpression final : public FunctionExpression
{
public:
  MaxExpression() : FunctionExpression(FunctionKind::Max) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 2)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return std::max(GetArg(0).GetValue(), GetArg(1).GetValue()); }
};

// usage: clamp(value, min, max)
class ClampExpression final : public FunctionExpression
{
public:
  ClampExpression() : FunctionExpression(FunctionKind::Clamp) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 3)
      return true;
    else
      return false;
  }

  ControlState GetValue()
  {
    return std::clamp(GetArg(0).GetValue(), GetArg(1).GetValue(), GetArg(2).GetValue());
  }
};

// usage: timer(seconds)
class TimerExpression final : public FunctionExpression
{
public:
  TimerExpression() : FunctionExpression(FunctionKind::Timer) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue()
  {
    const auto now = Clock::now();
    const auto elapsed = now - m_start_time;

    const ControlState val = GetArg(0).GetValue();

    ControlState progress = DTToSeconds(elapsed) / val;

    if (std::isinf(progress) || progress < 0.0)
    {
      // User configured a non-positive timer. Reset the timer and return 0.0.
      progress = 0.0;
      m_start_time = now;
    }
    else if (progress >= 1.0)
    {
      const ControlState reset_count = std::floor(progress);

      m_start_time += DTFromSecondsF(val * reset_count);
      progress -= reset_count;
    }

    return progress;
  }

private:
  TimePoint m_start_time = Clock::now();
};

// usage: if(condition, true_expression, false_expression)
class IfExpression final : public FunctionExpression
{
public:
  IfExpression() : FunctionExpression(FunctionKind::If) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 3)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return GetLValue()->GetValue(); }

  Expression* GetLValue()
  {
    return (GetArg(0).GetValue() > CONDITION_THRESHOLD) ? &GetArg(1) : &GetArg(2);
  }
};

// usage: minus(expression)
class UnaryMinusExpression final : public FunctionExpression
{
public:
  UnaryMinusExpression() : FunctionExpression(FunctionKind::UnaryMinus) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue()
  {
    // Subtraction for clarity:
    return 0.0 - GetArg(0).GetValue();
  }
};

// usage: plus(expression)
class UnaryPlusExpression final : public FunctionExpression
{
public:
  UnaryPlusExpression() : FunctionExpression(FunctionKind::UnaryPlus) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

private:
  bool ValidateArguments()
  {
    if (GetArgCount() == 1)
      return true;
    else
      return false;
  }

  ControlState GetValue() { return GetArg(0).GetValue(); }
};

// usage: deadzone(input, amount)
class DeadzoneExpression final : public FunctionExpression
{
public:
  DeadzoneExpression() : FunctionExpression(FunctionKind::Deadzone) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

  bool ValidateArguments()
  {
    if (GetArgCount() == 2)
      return true;
    else
      return false;
  }

  ControlState GetValue()
  {
    const ControlState val = GetArg(0).GetValue();
    const ControlState deadzone = GetArg(1).GetValue();
    return std::copysign(std::max(0.0, std::abs(val) - deadzone) / (1.0 - deadzone), val);
  }
};

// usage: smooth(input, seconds_up, seconds_down = seconds_up)
// seconds is seconds to change from 0.0 to 1.0
class SmoothExpression final : public FunctionExpression
{
public:
  SmoothExpression() : FunctionExpression(FunctionKind::Smooth) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

  bool ValidateArguments()
  {
    if (GetArgCount() == 2 || GetArgCount() == 3)
      return true;
    else
      return false;
  }

  ControlState GetValue()
  {
    const auto now = Clock::now();
    const auto elapsed = now - m_last_update;
    m_last_update = now;

    const ControlState desired_value = GetArg(0).GetValue();

    const ControlState smooth_up = GetArg(1).GetValue();
    const ControlState smooth_down = GetArgCount() == 3 ? GetArg(2).GetValue() : smooth_up;

    const ControlState smooth = (desired_value < m_value) ? smooth_down : smooth_up;
    const ControlState max_move = DTToSeconds(elapsed) / smooth;

    if (std::isinf(max_move))
    {
      m_value = desired_value;
    }
    else
    {
      const ControlState diff = desired_value - m_value;
      m_value += std::copysign(std::min(max_move, std::abs(diff)), diff);
    }

    return m_value;
  }

private:
  ControlState m_value = 0.0;
  TimePoint m_last_update = Clock::now();
};

// usage: hold(input, seconds)
class HoldExpression final : public FunctionExpression
{
public:
  HoldExpression() : FunctionExpression(FunctionKind::Hold) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

  bool ValidateArguments()
  {
    if (GetArgCount() == 2)
      return true;
    else
      return false;
  }

  ControlState GetValue()
  {
    const auto now = Clock::now();

    const ControlState input = GetArg(0).GetValue();

    if (input < CONDITION_THRESHOLD)
    {
      m_state = false;
      m_start_time = Clock::now();
    }
    else if (!m_state)
    {
      const auto hold_time = now - m_start_time;

      if (DTToSeconds(hold_time) >= GetArg(1).GetValue())
        m_state = true;
    }

    return m_state;
  }

private:
  bool m_state = false;
  TimePoint m_start_time = Clock::now();
};

// usage: tap(input, seconds, taps=2)
class TapExpression final : public FunctionExpression
{
public:
  TapExpression() : FunctionExpression(FunctionKind::Tap) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

  bool ValidateArguments()
  {
    if (GetArgCount() == 2 || GetArgCount() == 3)
      return true;
    else
      return false;
  }

  ControlState GetValue()
  {
    const auto now = Clock::now();

    const auto elapsed = DTToSeconds(now - m_start_time);

    const ControlState input = GetArg(0).GetValue();
    const ControlState seconds = GetArg(1).GetValue();

    const bool is_time_up = elapsed > seconds;

    const u32 desired_taps = GetArgCount() == 3 ? u32(GetArg(2).GetValue() + 0.5) : 2;

    if (input < CONDITION_THRESHOLD)
    {
      m_released = true;

      if (m_taps > 0 && is_time_up)
      {
        m_taps = 0;
      }
    }
    else
    {
      if (m_released)
      {
        if (!m_taps)
        {
          m_start_time = now;
        }

        ++m_taps;
        m_released = false;
      }

      return desired_taps == m_taps;
    }

    return 0.0;
  }

private:
  bool m_released = true;
  u32 m_taps = 0;
  TimePoint m_start_time = Clock::now();
};

// usage: relative(input, speed, [max_abs_value, [shared_state]])
// speed is max movement per second
class RelativeExpression final : public FunctionExpression
{
public:
  RelativeExpression() : FunctionExpression(FunctionKind::Relative) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

  bool ValidateArguments()
  {
    if (GetArgCount() >= 2 && GetArgCount() <= 4)
      return true;
    else
      return false;
  }

  ControlState GetValue()
  {
    // There is a lot of funky math in this function but it allows for a variety of uses:
    //
    // e.g. A single mapping with a relatively adjusted value between 0.0 and 1.0
    // Potentially useful for a trigger input
    //  relative(`Up` - `Down`, 2.0)
    //
    // e.g. A value with two mappings (such as analog stick Up/Down)
    // The shared state allows the two mappings to work together.
    // This mapping (for up) returns a value clamped between 0.0 and 1.0
    //  relative(`Up`, 2.0, 1.0, $y)
    // This mapping (for down) returns the negative value clamped between 0.0 and 1.0
    // (Adjustments created by `Down` are applied negatively to the shared state)
    //  relative(`Down`, 2.0, -1.0, $y)

    const auto now = Clock::now();

    if (GetArgCount() >= 4)
      m_state = GetArg(3).GetValue();

    const auto elapsed = DTToSeconds(now - m_last_update);
    m_last_update = now;

    const ControlState input = GetArg(0).GetValue();
    const ControlState speed = GetArg(1).GetValue();

    const ControlState max_abs_value = (GetArgCount() >= 3) ? GetArg(2).GetValue() : 1.0;

    const ControlState max_move = input * elapsed * speed;
    const ControlState diff_from_zero = std::abs(0.0 - m_state);
    const ControlState diff_from_max = std::abs(max_abs_value - m_state);

    m_state += std::min(std::max(max_move, -diff_from_zero), diff_from_max) *
               std::copysign(1.0, max_abs_value);

    if (GetArgCount() >= 4)
      const_cast<Expression&>(GetArg(3)).SetValue(m_state);

    return std::max(0.0, m_state * std::copysign(1.0, max_abs_value));
  }

private:
  ControlState m_state = 0.0;
  TimePoint m_last_update = Clock::now();
};

// usage: pulse(input, seconds)
class PulseExpression final : public FunctionExpression
{
public:
  PulseExpression() : FunctionExpression(FunctionKind::Pulse) {}

private:
  // the base branches to its calls
  friend class FunctionExpression;

  bool ValidateArguments()
  {
    if (GetArgCount() == 2)
      return true;
    else
      return false;
  }

  ControlState GetValue()
  {
    const auto now = Clock::now();

    const ControlState input = GetArg(0).GetValue();

    if (input < CONDITION_THRESHOLD)
    {
      m_released = true;
    }
    else if (m_released)
    {
      m_released = false;

      const auto seconds = DTFromSecondsF(GetArg(1).GetValue());

      if (m_state)
      {
        m_release_time += seconds;
      }
      else
      {
        m_state = true;
        m_release_time = now + seconds;
      }
    }

    if (m_state && now >= m_release_time)
    {
      m_state = false;
    }

    return m_state;
  }

private:
  bool m_released = false;
  bool m_state = false;
  TimePoint m_release_time = Clock::now();
};

FunctionExpression* MakeFunctionExpression(std::string_view name)
{
  if (name == "not")
    return new NotExpression();
  if (name == "if")
    return new IfExpression();
  if (name == "abs")
    return new AbsExpression();
  if (name == "sin")
    return new SinExpression();
  if (name == "cos")
    return new CosExpression();
  if (name == "tan")
    return new TanExpression();
  if (name == "asin")
    return new ASinExpression();
  if (name == "acos")
    return new ACosExpression();
  if (name == "atan")
    return new ATanExpression();
  if (name == "atan2")
    return new ATan2Expression();
  if (name == "sqrt")
    return new SqrtExpression();
  if (name == "pow")
    return new PowExpression();
  if (name == "min")
    return new MinExpression();
  if (name == "max")
    return new MaxExpression();
  if (name == "clamp")
    return new ClampExpression();
  if (name == "timer")
    return new TimerExpression();
  if (name == "toggle")
    return new ToggleExpression();
  if (name == "minus")
    return new UnaryMinusExpression();
  if (name == "plus")
    return new UnaryPlusExpression();
  if (name == "deadzone")
    return new DeadzoneExpression();
  if (name == "smooth")
    return new SmoothExpression();
  if (name == "hold")
    return new HoldExpression();
  if (name == "tap")
    return new TapExpression();
  if (name == "relative")
    return new RelativeExpression();
  if (name == "pulse")
    return new PulseExpression();

  return nullptr;
}

int FunctionExpression::CountNumControls() const
{
  int result = 0;

  for (u32 i = 0; i < m_arg_count; i++)
    result += m_args[i]->CountNumControls();

  return result;
}

void FunctionExpression::UpdateReferences(ControlEnvironment& env)
{
  for (u32 i = 0; i < m_arg_count; i++)
    m_args[i]->UpdateReferences(env);
}

FunctionExpression::~FunctionExpression()
{
  for (u32 i = 0; i < m_arg_count; i++)
    delete m_args[i];
  std::free(m_args);
}

bool FunctionExpression::AddArgument(Expression* arg)
{
  if (m_arg_count == m_arg_cap)
  {
    const u32 cap = m_arg_cap ? m_arg_cap * 2 : 4;
    Expression** const grown =
        static_cast<Expression**>(std::realloc(m_args, cap * sizeof(Expression*)));
    if (!grown)
    {
      FreeExpression(arg);
      return false;
    }
    m_args = grown;
    m_arg_cap = cap;
  }
  m_args[m_arg_count++] = arg;
  return true;
}

Expression& FunctionExpression::GetArg(u32 number)
{
  return *m_args[number];
}

u32 FunctionExpression::GetArgCount() const
{
  return m_arg_count;
}

/* The call on the function class this is: its own or, when it has none, the default. */
#define FUNC_HAS_OWN(D, Fn) (!std::is_same_v<decltype(&D::Fn), decltype(&FunctionExpression::Fn)>)

#define FUNC_DISPATCH                                                                              \
  switch (function_kind)                                                                           \
  {                                                                                                \
  case FunctionKind::Toggle:                                                                       \
    return call(static_cast<ToggleExpression*>(this));                                             \
  case FunctionKind::Not:                                                                          \
    return call(static_cast<NotExpression*>(this));                                                \
  case FunctionKind::Abs:                                                                          \
    return call(static_cast<AbsExpression*>(this));                                                \
  case FunctionKind::Sin:                                                                          \
    return call(static_cast<SinExpression*>(this));                                                \
  case FunctionKind::Cos:                                                                          \
    return call(static_cast<CosExpression*>(this));                                                \
  case FunctionKind::Tan:                                                                          \
    return call(static_cast<TanExpression*>(this));                                                \
  case FunctionKind::ASin:                                                                         \
    return call(static_cast<ASinExpression*>(this));                                               \
  case FunctionKind::ACos:                                                                         \
    return call(static_cast<ACosExpression*>(this));                                               \
  case FunctionKind::ATan:                                                                         \
    return call(static_cast<ATanExpression*>(this));                                               \
  case FunctionKind::ATan2:                                                                        \
    return call(static_cast<ATan2Expression*>(this));                                              \
  case FunctionKind::Sqrt:                                                                         \
    return call(static_cast<SqrtExpression*>(this));                                               \
  case FunctionKind::Pow:                                                                          \
    return call(static_cast<PowExpression*>(this));                                                \
  case FunctionKind::Min:                                                                          \
    return call(static_cast<MinExpression*>(this));                                                \
  case FunctionKind::Max:                                                                          \
    return call(static_cast<MaxExpression*>(this));                                                \
  case FunctionKind::Clamp:                                                                        \
    return call(static_cast<ClampExpression*>(this));                                              \
  case FunctionKind::Timer:                                                                        \
    return call(static_cast<TimerExpression*>(this));                                              \
  case FunctionKind::If:                                                                           \
    return call(static_cast<IfExpression*>(this));                                                 \
  case FunctionKind::UnaryMinus:                                                                   \
    return call(static_cast<UnaryMinusExpression*>(this));                                         \
  case FunctionKind::UnaryPlus:                                                                    \
    return call(static_cast<UnaryPlusExpression*>(this));                                          \
  case FunctionKind::Deadzone:                                                                     \
    return call(static_cast<DeadzoneExpression*>(this));                                           \
  case FunctionKind::Smooth:                                                                       \
    return call(static_cast<SmoothExpression*>(this));                                             \
  case FunctionKind::Hold:                                                                         \
    return call(static_cast<HoldExpression*>(this));                                               \
  case FunctionKind::Tap:                                                                          \
    return call(static_cast<TapExpression*>(this));                                                \
  case FunctionKind::Relative:                                                                     \
    return call(static_cast<RelativeExpression*>(this));                                           \
  case FunctionKind::Pulse:                                                                        \
    return call(static_cast<PulseExpression*>(this));                                              \
  }                                                                                                \
  return call(static_cast<PulseExpression*>(this));

bool FunctionExpression::ValidateArguments()
{
  const auto call = [](auto* f) { return f->ValidateArguments(); };
  FUNC_DISPATCH
}

ControlState FunctionExpression::GetValue()
{
  const auto call = [](auto* f) { return f->GetValue(); };
  FUNC_DISPATCH
}

void FunctionExpression::SetValue(ControlState value)
{
  const auto call = [&](auto* f) {
    using D = std::remove_pointer_t<decltype(f)>;
    if constexpr (FUNC_HAS_OWN(D, SetValue))
      f->SetValue(value);
    else
      SetValueDefault(value);
  };
  FUNC_DISPATCH
}

Expression* FunctionExpression::GetLValue()
{
  const auto call = [this](auto* f) -> Expression* {
    using D = std::remove_pointer_t<decltype(f)>;
    if constexpr (FUNC_HAS_OWN(D, GetLValue))
      return f->GetLValue();
    else
      return GetLValueDefault();
  };
  FUNC_DISPATCH
}

void FunctionExpression::Destroy(FunctionExpression* function)
{
  const auto call = [](auto* f) {
    using D = std::remove_pointer_t<decltype(f)>;
    f->~D();
    ::operator delete(static_cast<void*>(f));
  };
  switch (function->function_kind)
  {
  case FunctionKind::Toggle:
    return call(static_cast<ToggleExpression*>(function));
  case FunctionKind::Not:
    return call(static_cast<NotExpression*>(function));
  case FunctionKind::Abs:
    return call(static_cast<AbsExpression*>(function));
  case FunctionKind::Sin:
    return call(static_cast<SinExpression*>(function));
  case FunctionKind::Cos:
    return call(static_cast<CosExpression*>(function));
  case FunctionKind::Tan:
    return call(static_cast<TanExpression*>(function));
  case FunctionKind::ASin:
    return call(static_cast<ASinExpression*>(function));
  case FunctionKind::ACos:
    return call(static_cast<ACosExpression*>(function));
  case FunctionKind::ATan:
    return call(static_cast<ATanExpression*>(function));
  case FunctionKind::ATan2:
    return call(static_cast<ATan2Expression*>(function));
  case FunctionKind::Sqrt:
    return call(static_cast<SqrtExpression*>(function));
  case FunctionKind::Pow:
    return call(static_cast<PowExpression*>(function));
  case FunctionKind::Min:
    return call(static_cast<MinExpression*>(function));
  case FunctionKind::Max:
    return call(static_cast<MaxExpression*>(function));
  case FunctionKind::Clamp:
    return call(static_cast<ClampExpression*>(function));
  case FunctionKind::Timer:
    return call(static_cast<TimerExpression*>(function));
  case FunctionKind::If:
    return call(static_cast<IfExpression*>(function));
  case FunctionKind::UnaryMinus:
    return call(static_cast<UnaryMinusExpression*>(function));
  case FunctionKind::UnaryPlus:
    return call(static_cast<UnaryPlusExpression*>(function));
  case FunctionKind::Deadzone:
    return call(static_cast<DeadzoneExpression*>(function));
  case FunctionKind::Smooth:
    return call(static_cast<SmoothExpression*>(function));
  case FunctionKind::Hold:
    return call(static_cast<HoldExpression*>(function));
  case FunctionKind::Tap:
    return call(static_cast<TapExpression*>(function));
  case FunctionKind::Relative:
    return call(static_cast<RelativeExpression*>(function));
  case FunctionKind::Pulse:
    return call(static_cast<PulseExpression*>(function));
  }
  call(static_cast<PulseExpression*>(function));
}

}  // namespace ciface::ExpressionParser
