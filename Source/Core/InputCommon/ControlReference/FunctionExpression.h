// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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
  struct ArgumentsAreValid
  {
  };

  struct ExpectedArguments
  {
    std::string text;
  };

  using ArgumentValidation = std::variant<ArgumentsAreValid, ExpectedArguments>;

  int CountNumControls() const;
  void UpdateReferences(ControlEnvironment& env);

  void SetArguments(std::vector<std::unique_ptr<Expression>>&& args);

  // These branch to the function this is.
  ArgumentValidation ValidateArguments();
  ControlState GetValue();
  void SetValue(ControlState value);
  Expression* GetLValue();

  // Destroys and frees the function as the function it is
  static void Destroy(FunctionExpression* function);

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
  std::vector<std::unique_ptr<Expression>> m_args;
};

std::unique_ptr<FunctionExpression> MakeFunctionExpression(std::string_view name);

}  // namespace ciface::ExpressionParser
