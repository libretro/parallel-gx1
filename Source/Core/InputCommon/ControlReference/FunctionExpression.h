// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "InputCommon/ControlReference/ExpressionParser.h"

namespace ciface::ExpressionParser
{
constexpr ControlState CONDITION_THRESHOLD = 0.5;

enum class FunctionKind : u8
{
  Toggle,
  Not,
  Abs,
  Sin,
  Cos,
  Tan,
  ASin,
  ACos,
  ATan,
  ATan2,
  Sqrt,
  Pow,
  Min,
  Max,
  Clamp,
  Timer,
  If,
  UnaryMinus,
  UnaryPlus,
  Deadzone,
  Smooth,
  Hold,
  Tap,
  Relative,
  Pulse,
};

class FunctionExpression : public Expression
{
public:
  int CountNumControls() const;
  void UpdateReferences(ControlEnvironment& env);

  // The function takes the argument, behind those it has; false when out of memory, the argument
  // freed.
  bool AddArgument(std::unique_ptr<Expression>&& arg);

  // These branch to the function this is.
  // Whether the function was given the arguments it takes
  bool ValidateArguments();
  ControlState GetValue();
  void SetValue(ControlState value);
  Expression* GetLValue();

  // Destroys and frees the function as the function it is
  static void Destroy(FunctionExpression* function);

  // Frees the arguments. Public, as a delete of a function names it even though the destroying
  // delete of Expression is what runs it.
  ~FunctionExpression();
  FunctionExpression(const FunctionExpression&) = delete;
  FunctionExpression& operator=(const FunctionExpression&) = delete;

  const FunctionKind function_kind;

protected:
  explicit FunctionExpression(FunctionKind kind)
      : Expression(ExpressionKind::Function), function_kind(kind)
  {
  }

  // What a function that has no call of its own does: setting it does nothing
  void SetValueDefault(ControlState) {}

  Expression& GetArg(u32 number);
  u32 GetArgCount() const;

private:
  // The arguments, which the function owns
  Expression** m_args = nullptr;
  u32 m_arg_count = 0;
  u32 m_arg_cap = 0;
};

std::unique_ptr<FunctionExpression> MakeFunctionExpression(std::string_view name);

}  // namespace ciface::ExpressionParser
