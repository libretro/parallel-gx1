// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControlReference/ExpressionParser.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "InputCommon/ControlReference/FunctionExpression.h"

namespace ciface::ExpressionParser
{
using namespace ciface::Core;

class ControlExpression;

// Check if operator is usable with assignment, e.g. += -= *=
static bool IsCompoundAssignmentUsableBinaryOperator(TokenType type)
{
  return type >= TOK_COMPOUND_ASSIGN_OPS_BEGIN && type < TOK_COMPOUND_ASSIGN_OPS_END;
}

static TokenType GetBinaryOperatorTokenTypeFromChar(char c)
{
  switch (c)
  {
  case '+':
    return TOK_ADD;
  case '-':
    return TOK_SUB;
  case '*':
    return TOK_MUL;
  case '/':
    return TOK_DIV;
  case '%':
    return TOK_MOD;
  case '=':
    return TOK_ASSIGN;
  case '<':
    return TOK_LTHAN;
  case '>':
    return TOK_GTHAN;
  case ',':
    return TOK_COMMA;
  case '^':
    return TOK_XOR;
  case '&':
    return TOK_AND;
  case '|':
    return TOK_OR;
  default:
    return TOK_INVALID;
  }
}

class HotkeySuppressions
{
public:
  using Modifiers = std::vector<std::unique_ptr<ControlExpression>>;

  struct InvokingDeleter
  {
    template <typename T>
    void operator()(T* func)
    {
      (*func)();
      delete func;
    }
  };

  using Suppressor = std::unique_ptr<std::function<void()>, InvokingDeleter>;

  bool IsSuppressed(Device::Input* input) const
  {
    // Input is suppressed if it exists in the map at all.
    return m_suppressions.lower_bound({input, nullptr}) !=
           m_suppressions.lower_bound({input + 1, nullptr});
  }

  bool IsSuppressedIgnoringModifiers(Device::Input* input, const Modifiers& ignore_modifiers) const;

  // Suppresses each input + modifier pair.
  // The returned object removes the suppression on destruction.
  Suppressor MakeSuppressor(const Modifiers* modifiers,
                            const std::unique_ptr<ControlExpression>* final_input);

private:
  using Suppression = std::pair<Device::Input*, Device::Input*>;
  using SuppressionLevel = u16;

  void RemoveSuppression(Device::Input* modifier, Device::Input* final_input)
  {
    auto it = m_suppressions.find({final_input, modifier});
    if (it != m_suppressions.end() && (--it->second) == 0)
      m_suppressions.erase(it);
  }

  // Holds counts of suppressions for each input/modifier pair.
  std::map<Suppression, SuppressionLevel> m_suppressions;
};

static HotkeySuppressions s_hotkey_suppressions;

static bool IsAsciiAlpha(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool IsAsciiDigit(char c)
{
  return c >= '0' && c <= '9';
}

bool Token::IsBinaryOperator() const
{
  return type >= TOK_BINARY_OPS_BEGIN && type < TOK_BINARY_OPS_END;
}

Lexer::Lexer(const char* expr, std::size_t length) : m_expr(expr), m_length(length), m_pos(0)
{
}

Token Lexer::MakeToken(TokenType type, std::size_t data_start, std::size_t data_end) const
{
  Token token;
  token.type = type;
  token.data = m_expr + data_start;
  token.data_length = data_end - data_start;
  token.string_position = 0;
  token.string_length = 0;
  return token;
}

Token Lexer::GetDelimitedToken(TokenType type, char delimiter)
{
  const std::size_t start = m_pos;
  while (m_pos < m_length && m_expr[m_pos] != delimiter && m_expr[m_pos] != '\n')
    m_pos++;

  if (m_pos == m_length || m_expr[m_pos] != delimiter)
    return MakeToken(TOK_INVALID, m_pos, m_pos);

  m_pos++;
  return MakeToken(type, start, m_pos - 1);
}

void Lexer::FetchWordChars()
{
  while (m_pos < m_length &&
         (IsAsciiAlpha(m_expr[m_pos]) || IsAsciiDigit(m_expr[m_pos]) || m_expr[m_pos] == '_'))
  {
    m_pos++;
  }
}

Token Lexer::GetVariable()
{
  const std::size_t start = m_pos;
  FetchWordChars();
  return MakeToken(TOK_VARIABLE, start, m_pos);
}

// Its first character already read
Token Lexer::GetBareword()
{
  const std::size_t start = m_pos - 1;
  FetchWordChars();
  return MakeToken(TOK_BAREWORD, start, m_pos);
}

// Its first character, a digit, already read
Token Lexer::GetRealLiteral()
{
  const std::size_t start = m_pos - 1;
  std::size_t i = start;
  bool valid;

  while (m_pos < m_length && (IsAsciiDigit(m_expr[m_pos]) || m_expr[m_pos] == '.'))
    m_pos++;

  // digits, optionally followed by '.' and more digits
  while (i < m_pos && IsAsciiDigit(m_expr[i]))
    i++;
  valid = i > start;
  if (valid && i < m_pos)
  {
    const std::size_t fraction = ++i;
    while (i < m_pos && IsAsciiDigit(m_expr[i]))
      i++;
    valid = i > fraction && i == m_pos;
  }
  if (valid)
    return MakeToken(TOK_LITERAL, start, m_pos);

  return MakeToken(TOK_INVALID, m_pos, m_pos);
}

Token Lexer::NextToken()
{
  if (m_pos == m_length)
    return MakeToken(TOK_EOF, m_pos, m_pos);

  const char c = m_expr[m_pos++];

  // Handle /* */ style comments.
  if (c == '/' && m_pos < m_length && m_expr[m_pos] == '*')
  {
    std::size_t end;
    m_pos++;
    for (end = m_pos; end + 1 < m_length; end++)
    {
      if (m_expr[end] == '*' && m_expr[end + 1] == '/')
        break;
    }
    if (end + 1 >= m_length)
      return MakeToken(TOK_INVALID, m_pos, m_pos);
    m_pos = end + 2;
    return MakeToken(TOK_COMMENT, m_pos, m_pos);
  }

  const auto tok_type = GetBinaryOperatorTokenTypeFromChar(c);
  if (tok_type != TOK_INVALID)
  {
    // Check for compound assignment op, e.g. + immediately followed by =.
    if (IsCompoundAssignmentUsableBinaryOperator(tok_type) && m_pos < m_length &&
        m_expr[m_pos] == '=')
    {
      m_pos++;
      // the operator is the token's text
      return MakeToken(TOK_ASSIGN, m_pos - 2, m_pos - 1);
    }

    return MakeToken(tok_type, m_pos, m_pos);
  }

  switch (c)
  {
  case ' ':
  case '\t':
  case '\n':
  case '\r':
    return MakeToken(TOK_WHITESPACE, m_pos, m_pos);
  case '(':
    return MakeToken(TOK_LPAREN, m_pos, m_pos);
  case ')':
    return MakeToken(TOK_RPAREN, m_pos, m_pos);
  case '@':
    return MakeToken(TOK_HOTKEY, m_pos, m_pos);
  case '?':
    return MakeToken(TOK_QUESTION, m_pos, m_pos);
  case ':':
    return MakeToken(TOK_COLON, m_pos, m_pos);
  case '!':
    return MakeToken(TOK_NOT, m_pos, m_pos);
  case '\'':
    return GetDelimitedToken(TOK_LITERAL, '\'');
  case '$':
    return GetVariable();
  case '`':
    return GetDelimitedToken(TOK_CONTROL, '`');
  default:
    if (IsAsciiAlpha(c))
      return GetBareword();
    else if (IsAsciiDigit(c))
      return GetRealLiteral();
    else
      return MakeToken(TOK_INVALID, m_pos, m_pos);
  }
}

ParseStatus Lexer::Tokenize(Token* tokens, std::size_t* count)
{
  *count = 0;
  while (true)
  {
    const std::size_t prev_pos = m_pos;
    Token tok = NextToken();
    tok.string_position = prev_pos;
    tok.string_length = m_pos - prev_pos;
    tokens[(*count)++] = tok;

    if (tok.type == TOK_INVALID)
      return ParseStatus::SyntaxError;

    if (tok.type == TOK_EOF)
      break;
  }
  return ParseStatus::Successful;
}

class ControlExpression : public Expression
{
public:
  explicit ControlExpression(ControlQualifier qualifier)
      : Expression(ExpressionKind::Control), m_qualifier(std::move(qualifier))
  {
  }

  ControlState GetValue()
  {
    if (s_hotkey_suppressions.IsSuppressed(m_input))
      return 0;
    return GetValueIgnoringSuppression();
  }

  ControlState GetValueIgnoringSuppression() const
  {
    if (!m_input)
      return 0.0;

    // Note: Inputs may return negative values in situations where opposing directions are
    // activated. We clamp off the negative values here.

    // FYI: Clamping values greater than 1.0 is purposely not done to support unbounded values in
    // the future. (e.g. raw accelerometer/gyro data)

    return std::max(0.0, m_input->GetState());
  }
  void SetValue(ControlState value)
  {
    if (m_output)
      m_output->SetState(value);
  }
  int CountNumControls() const { return (m_input || m_output) ? 1 : 0; }
  void UpdateReferences(ControlEnvironment& env)
  {
    m_device = env.FindDevice(m_qualifier);
    m_input = env.FindInput(m_qualifier);
    m_output = env.FindOutput(m_qualifier);
  }

  Device::Input* GetInput() const { return m_input; }

private:
  // Keep a shared_ptr to the device so the control pointer doesn't become invalid.
  std::shared_ptr<Device> m_device;
  ControlQualifier m_qualifier;
  Device::Input* m_input = nullptr;
  Device::Output* m_output = nullptr;
};

bool HotkeySuppressions::IsSuppressedIgnoringModifiers(Device::Input* input,
                                                       const Modifiers& ignore_modifiers) const
{
  // Input is suppressed if it exists in the map with a modifier that we aren't ignoring.
  auto it = m_suppressions.lower_bound({input, nullptr});
  auto it_end = m_suppressions.lower_bound({input + 1, nullptr});

  // We need to ignore L_Ctrl R_Ctrl when supplied Ctrl and vice-versa.
  const auto is_same_modifier = [](Device::Input* i1, Device::Input* i2) {
    return i1 && i2 && (i1 == i2 || i1->IsChild(i2) || i2->IsChild(i1));
  };

  return std::any_of(it, it_end, [&](const auto& s) {
    return std::ranges::none_of(ignore_modifiers, [&](const auto& m) {
      return is_same_modifier(m->GetInput(), s.first.second);
    });
  });
}

HotkeySuppressions::Suppressor
HotkeySuppressions::MakeSuppressor(const Modifiers* modifiers,
                                   const std::unique_ptr<ControlExpression>* final_input)
{
  for (auto& modifier : *modifiers)
  {
    // Inputs might be null, don't add nullptr to the map
    if ((*final_input)->GetInput() && modifier->GetInput())
    {
      ++m_suppressions[{(*final_input)->GetInput(), modifier->GetInput()}];
    }
  }

  return Suppressor(std::make_unique<std::function<void()>>([this, modifiers, final_input] {
                      for (auto& modifier : *modifiers)
                        RemoveSuppression(modifier->GetInput(), (*final_input)->GetInput());
                    }).release(),
                    InvokingDeleter{});
}

class BinaryExpression : public Expression
{
public:
  TokenType op;
  std::unique_ptr<Expression> lhs;
  std::unique_ptr<Expression> rhs;

  BinaryExpression(TokenType op_, std::unique_ptr<Expression>&& lhs_,
                   std::unique_ptr<Expression>&& rhs_, ExpressionKind kind = ExpressionKind::Binary)
      : Expression(kind), op(op_), lhs(std::move(lhs_)), rhs(std::move(rhs_))
  {
  }

  ControlState GetValue()
  {
    if (op == TOK_ASSIGN || op == TOK_COMMA)
    {
      return GetLValue()->GetValue();
    }

    // Strict evaluation order of lhs,rhs in case of side effects.
    const ControlState lhs_value = lhs->GetValue();
    const ControlState rhs_value = rhs->GetValue();

    return CalculateValue(op, lhs_value, rhs_value);
  }

  static ControlState CalculateValue(TokenType op, ControlState lhs_value, ControlState rhs_value)
  {
    switch (op)
    {
    case TOK_AND:
      return std::min(lhs_value, rhs_value);
    case TOK_OR:
      return std::max(lhs_value, rhs_value);
    case TOK_ADD:
      return lhs_value + rhs_value;
    case TOK_SUB:
      return lhs_value - rhs_value;
    case TOK_MUL:
      return lhs_value * rhs_value;
    case TOK_DIV:
    {
      const ControlState result = lhs_value / rhs_value;
      return std::isinf(result) ? 0.0 : result;
    }
    case TOK_MOD:
    {
      const ControlState result = std::fmod(lhs_value, rhs_value);
      return std::isnan(result) ? 0.0 : result;
    }
    case TOK_LTHAN:
      return lhs_value < rhs_value;
    case TOK_GTHAN:
      return lhs_value > rhs_value;
    case TOK_XOR:
      return std::max(std::min(1 - lhs_value, rhs_value), std::min(lhs_value, 1 - rhs_value));
    default:
      assert(false);
      return 0;
    }
  }

  Expression* GetLValue()
  {
    switch (op)
    {
    case TOK_ASSIGN:
    {
      Expression* const lvalue = lhs->GetLValue();
      const ControlState rvalue = rhs->GetValue();
      lvalue->SetValue(rvalue);
      return lvalue;
    }
    case TOK_COMMA:
      lhs->GetValue();
      return rhs->GetLValue();
    default:
      return this;
    }
  }

  void SetValue(ControlState value)
  {
    // Don't do anything special with the op we have.
    // Treat "A & B" the same as "A | B".
    lhs->SetValue(value);
    rhs->SetValue(value);
  }

  int CountNumControls() const { return lhs->CountNumControls() + rhs->CountNumControls(); }

  void UpdateReferences(ControlEnvironment& env)
  {
    lhs->UpdateReferences(env);
    rhs->UpdateReferences(env);
  }
};

class CompoundAssignmentExpression : public BinaryExpression
{
public:
  CompoundAssignmentExpression(TokenType op_, std::unique_ptr<Expression>&& lhs_,
                               std::unique_ptr<Expression>&& rhs_)
      : BinaryExpression(op_, std::move(lhs_), std::move(rhs_), ExpressionKind::CompoundAssignment)
  {
  }

  ControlState GetValue() { return GetLValue()->GetValue(); }

  Expression* GetLValue()
  {
    Expression* const lvalue = lhs->GetLValue();
    const ControlState lhs_value = lvalue->GetValue();
    const ControlState rhs_value = rhs->GetValue();
    lvalue->SetValue(CalculateValue(op, lhs_value, rhs_value));
    return lvalue;
  }
};

class LiteralExpression : public Expression
{
public:
  LiteralExpression() : Expression(ExpressionKind::Literal) {}

  void SetValue(ControlState)
  {
    // Do nothing.
  }

  int CountNumControls() const { return 1; }

  void UpdateReferences(ControlEnvironment&)
  {
    // Nothing needed.
  }
};

class LiteralReal : public LiteralExpression
{
public:
  explicit LiteralReal(ControlState value) : m_value(value) {}

  ControlState GetValue() { return m_value; }

private:
  const ControlState m_value{};
};

static ParseResult MakeLiteralExpression(const Token& token)
{
  ControlState val{};
  if (TryParse(std::string(token.Data()), &val))
    return ParseResult::MakeSuccessfulResult(std::make_unique<LiteralReal>(val));
  else
    return ParseResult::MakeErrorResult();
}

class VariableExpression : public Expression
{
public:
  explicit VariableExpression(std::string name)
      : Expression(ExpressionKind::Variable), m_name(std::move(name))
  {
  }

  ControlState GetValue() { return m_variable_ptr ? *m_variable_ptr : 0; }

  void SetValue(ControlState value)
  {
    if (m_variable_ptr)
      *m_variable_ptr = value;
  }

  int CountNumControls() const { return 1; }

  void UpdateReferences(ControlEnvironment& env) { m_variable_ptr = env.GetVariablePtr(m_name); }

protected:
  const std::string m_name;
  std::shared_ptr<ControlState> m_variable_ptr;
};

class HotkeyExpression : public Expression
{
public:
  explicit HotkeyExpression(std::vector<std::unique_ptr<ControlExpression>> inputs)
      : Expression(ExpressionKind::Hotkey), m_modifiers(std::move(inputs))
  {
    m_final_input = std::move(m_modifiers.back());
    m_modifiers.pop_back();
  }

  ControlState GetValue()
  {
    // True if we have no modifiers
    const bool modifiers_pressed = std::ranges::all_of(
        m_modifiers, [](const auto& input) { return input->GetValue() > CONDITION_THRESHOLD; });

    const auto final_input_state = m_final_input->GetValueIgnoringSuppression();

    if (modifiers_pressed)
    {
      // Ignore suppression of our own modifiers. This also allows superset modifiers to function.
      const bool is_suppressed = s_hotkey_suppressions.IsSuppressedIgnoringModifiers(
          m_final_input->GetInput(), m_modifiers);

      if (final_input_state <= CONDITION_THRESHOLD)
        m_is_blocked = false;

      // If some other hotkey suppressed us, require a release of final input to be ready again.
      if (is_suppressed)
        m_is_blocked = true;

      if (m_is_blocked)
        return 0;

      EnableSuppression();

      // Our modifiers are active. Pass through the final input.
      return final_input_state;
    }
    else
    {
      m_suppressor = {};
      m_is_blocked = final_input_state > CONDITION_THRESHOLD;
    }

    return 0;
  }

  void SetValue(ControlState) {}

  int CountNumControls() const
  {
    int result = 0;
    for (auto& input : m_modifiers)
      result += input->CountNumControls();
    return result + m_final_input->CountNumControls();
  }

  void UpdateReferences(ControlEnvironment& env)
  {
    for (auto& input : m_modifiers)
      input->UpdateReferences(env);

    m_final_input->UpdateReferences(env);

    // We must update our suppression with valid pointers.
    if (m_suppressor)
      EnableSuppression(true);
  }

private:
  void EnableSuppression(bool force = false)
  {
    if (!m_suppressor || force)
      m_suppressor = s_hotkey_suppressions.MakeSuppressor(&m_modifiers, &m_final_input);
  }

  HotkeySuppressions::Modifiers m_modifiers;
  std::unique_ptr<ControlExpression> m_final_input;
  HotkeySuppressions::Suppressor m_suppressor;
  bool m_is_blocked = false;
};

// This class proxies all methods to its either left-hand child if it has bound controls, or its
// right-hand child. Its intended use is for supporting old-style barewords expressions.
// Note that if you have a keyboard device as default device and the expression is a single digit
// number, this will usually resolve in a numerical key instead of a numerical value.
// Though if this expression belongs to NumericSetting, it will likely be simplified back to a
// value.
class CoalesceExpression : public Expression
{
public:
  CoalesceExpression(std::unique_ptr<Expression>&& lhs, std::unique_ptr<Expression>&& rhs)
      : Expression(ExpressionKind::Coalesce), m_lhs(std::move(lhs)), m_rhs(std::move(rhs))
  {
  }

  ControlState GetValue() { return GetActiveChild()->GetValue(); }
  void SetValue(ControlState value) { GetActiveChild()->SetValue(value); }

  int CountNumControls() const { return GetActiveChild()->CountNumControls(); }
  void UpdateReferences(ControlEnvironment& env)
  {
    m_lhs->UpdateReferences(env);
    if (m_rhs)
      m_rhs->UpdateReferences(env);
  }

private:
  const std::unique_ptr<Expression>& GetActiveChild() const
  {
    return m_lhs->CountNumControls() > 0 ? m_lhs : m_rhs;
  }

  std::unique_ptr<Expression> m_lhs;
  std::unique_ptr<Expression> m_rhs;
};

/* The call on the node class this is: its own, its parent's for a compound assignment, or,
 * for the l-value of a node that has none, the base's default. */
#define EXPR_HAS_OWN(D, Fn) (!std::is_same_v<decltype(&D::Fn), decltype(&Expression::Fn)>)

#define EXPR_DISPATCH(Q)                                                                           \
  switch (expression_kind)                                                                         \
  {                                                                                                \
  case ExpressionKind::Control:                                                                    \
    return call(static_cast<Q ControlExpression*>(this));                                          \
  case ExpressionKind::Binary:                                                                     \
    return call(static_cast<Q BinaryExpression*>(this));                                           \
  case ExpressionKind::CompoundAssignment:                                                         \
    return call(static_cast<Q CompoundAssignmentExpression*>(this));                               \
  case ExpressionKind::Literal:                                                                    \
    return call(static_cast<Q LiteralReal*>(this));                                                \
  case ExpressionKind::Variable:                                                                   \
    return call(static_cast<Q VariableExpression*>(this));                                         \
  case ExpressionKind::Hotkey:                                                                     \
    return call(static_cast<Q HotkeyExpression*>(this));                                           \
  case ExpressionKind::Coalesce:                                                                   \
    return call(static_cast<Q CoalesceExpression*>(this));                                         \
  case ExpressionKind::Function:                                                                   \
  default:                                                                                         \
    return call(static_cast<Q FunctionExpression*>(this));                                         \
  }

ControlState Expression::GetValue()
{
  const auto call = [](auto* e) { return e->GetValue(); };
  EXPR_DISPATCH()
}

void Expression::SetValue(ControlState state)
{
  const auto call = [&](auto* e) { e->SetValue(state); };
  EXPR_DISPATCH()
}

int Expression::CountNumControls() const
{
  const auto call = [](const auto* e) { return e->CountNumControls(); };
  EXPR_DISPATCH(const)
}

void Expression::UpdateReferences(ControlEnvironment& finder)
{
  const auto call = [&](auto* e) { e->UpdateReferences(finder); };
  EXPR_DISPATCH()
}

Expression* Expression::GetLValue()
{
  const auto call = [this](auto* e) -> Expression* {
    using D = std::remove_pointer_t<decltype(e)>;
    if constexpr (EXPR_HAS_OWN(D, GetLValue))
      return e->GetLValue();
    else
      return GetLValueDefault();
  };
  EXPR_DISPATCH()
}

void Expression::operator delete(Expression* expression, std::destroying_delete_t)
{
  if (expression->expression_kind == ExpressionKind::Function)
  {
    FunctionExpression::Destroy(static_cast<FunctionExpression*>(expression));
    return;
  }
  const auto call = [](auto* e) {
    using D = std::remove_pointer_t<decltype(e)>;
    e->~D();
    ::operator delete(static_cast<void*>(e));
  };
  switch (expression->expression_kind)
  {
  case ExpressionKind::Control:
    return call(static_cast<ControlExpression*>(expression));
  case ExpressionKind::Binary:
    return call(static_cast<BinaryExpression*>(expression));
  case ExpressionKind::CompoundAssignment:
    return call(static_cast<CompoundAssignmentExpression*>(expression));
  case ExpressionKind::Literal:
    return call(static_cast<LiteralReal*>(expression));
  case ExpressionKind::Variable:
    return call(static_cast<VariableExpression*>(expression));
  case ExpressionKind::Hotkey:
    return call(static_cast<HotkeyExpression*>(expression));
  case ExpressionKind::Coalesce:
  default:
    return call(static_cast<CoalesceExpression*>(expression));
  }
}

std::shared_ptr<Device> ControlEnvironment::FindDevice(const ControlQualifier& qualifier) const
{
  if (qualifier.has_device)
    return container.FindDevice(qualifier.device_qualifier);
  else
    return container.FindDevice(default_device);
}

Device::Input* ControlEnvironment::FindInput(const ControlQualifier& qualifier) const
{
  const std::shared_ptr<Device> device = FindDevice(qualifier);
  if (!device)
    return nullptr;

  return device->FindInput(qualifier.control_name);
}

Device::Output* ControlEnvironment::FindOutput(const ControlQualifier& qualifier) const
{
  const std::shared_ptr<Device> device = FindDevice(qualifier);
  if (!device)
    return nullptr;

  return device->FindOutput(qualifier.control_name);
}

std::shared_ptr<ControlState> ControlEnvironment::GetVariablePtr(const std::string& name)
{
  // Do not accept an empty string as key, even if the expression parser already prevents this case.
  if (name.empty())
    return nullptr;
  std::shared_ptr<ControlState>& variable = m_variables[name];
  // If new, make a shared ptr
  if (!variable)
  {
    variable = std::make_shared<ControlState>();
  }
  return variable;
}

void ControlEnvironment::CleanUnusedVariables()
{
  for (auto it = m_variables.begin(); it != m_variables.end();)
  {
    // Don't count ourselves as reference
    if (it->second.use_count() <= 1)
      m_variables.erase(it++);
    else
      ++it;
  }
}

ParseResult ParseResult::MakeEmptyResult()
{
  ParseResult result;
  result.status = ParseStatus::EmptyExpression;
  return result;
}

ParseResult ParseResult::MakeSuccessfulResult(std::unique_ptr<Expression>&& expr)
{
  ParseResult result;
  result.status = ParseStatus::Successful;
  result.expr = std::move(expr);
  return result;
}

ParseResult ParseResult::MakeErrorResult()
{
  ParseResult result;
  result.status = ParseStatus::SyntaxError;
  return result;
}

static bool IsInertToken(const Token& tok)
{
  return tok.type == TOK_COMMENT || tok.type == TOK_WHITESPACE;
}

class Parser
{
public:
  explicit Parser(const Token* tokens) : m_it(tokens) {}
  ParseResult Parse()
  {
    ParseResult result = ParseToplevel();

    if (ParseStatus::Successful != result.status)
      return result;

    if (Peek().type == TOK_EOF)
      return result;

    return ParseResult::MakeErrorResult();
  }

private:
  // The next token: the last of them is EOF
  const Token* m_it;

  Token Chew()
  {
    const Token tok = Peek();
    if (TOK_EOF != tok.type)
      ++m_it;
    return tok;
  }

  Token Peek()
  {
    while (IsInertToken(*m_it))
      ++m_it;
    return *m_it;
  }

  bool Expects(TokenType type)
  {
    Token tok = Chew();
    return tok.type == type;
  }

  ParseResult ParseFunctionArguments(std::unique_ptr<FunctionExpression>&& func)
  {
    std::vector<std::unique_ptr<Expression>> args;

    if (TOK_LPAREN != Peek().type)
    {
      // Single argument with no parens (useful for unary ! function)
      const auto tok = Chew();
      auto arg = ParseAtom(tok);
      if (ParseStatus::Successful != arg.status)
        return arg;

      args.emplace_back(std::move(arg.expr));
    }
    else
    {
      // Chew the L-Paren
      Chew();

      // Check for empty argument list:
      if (TOK_RPAREN == Peek().type)
      {
        Chew();
      }
      else
      {
        while (true)
        {
          // Read one argument.
          // Grab an expression, but stop at comma.
          auto arg = ParseInfixOperations(OperatorPrecedence(TOK_COMMA));
          if (ParseStatus::Successful != arg.status)
            return arg;

          args.emplace_back(std::move(arg.expr));

          // Right paren is the end of our arguments.
          const Token tok = Chew();
          if (TOK_RPAREN == tok.type)
            break;

          // Comma before the next argument.
          if (TOK_COMMA != tok.type)
            return ParseResult::MakeErrorResult();
        };
      }
    }

    func->SetArguments(std::move(args));
    const auto argument_validation = func->ValidateArguments();

    if (std::holds_alternative<FunctionExpression::ExpectedArguments>(argument_validation))
      return ParseResult::MakeErrorResult();

    return ParseResult::MakeSuccessfulResult(std::move(func));
  }

  ParseResult ParseAtom(const Token& tok)
  {
    switch (tok.type)
    {
    case TOK_BAREWORD:
    {
      auto func = MakeFunctionExpression(tok.Data());

      if (!func)
      {
        // Invalid function, interpret this as a bareword control.
        Token control_tok(tok);
        control_tok.type = TOK_CONTROL;
        return ParseAtom(control_tok);
      }

      return ParseFunctionArguments(std::move(func));
    }
    case TOK_CONTROL:
    {
      ControlQualifier cq;
      cq.FromString(std::string(tok.Data()));
      return ParseResult::MakeSuccessfulResult(std::make_unique<ControlExpression>(cq));
    }
    case TOK_NOT:
    {
      return ParseFunctionArguments(MakeFunctionExpression("not"));
    }
    case TOK_LITERAL:
    {
      return MakeLiteralExpression(tok);
    }
    case TOK_VARIABLE:
    {
      if (!tok.data_length)
        return ParseResult::MakeErrorResult();
      else
        return ParseResult::MakeSuccessfulResult(
            std::make_unique<VariableExpression>(std::string(tok.Data())));
    }
    case TOK_LPAREN:
    {
      return ParseParens();
    }
    case TOK_HOTKEY:
    {
      return ParseHotkeys();
    }
    case TOK_SUB:
    {
      // An atom was expected but we got a subtraction symbol.
      // Interpret it as a unary minus function.
      return ParseFunctionArguments(MakeFunctionExpression("minus"));
    }
    case TOK_ADD:
    {
      // An atom was expected but we got an addition symbol.
      // Interpret it as a unary plus.
      return ParseFunctionArguments(MakeFunctionExpression("plus"));
    }
    default:
    {
      return ParseResult::MakeErrorResult();
    }
    }
  }

  static constexpr int OperatorPrecedence(TokenType type = TOK_EOF)
  {
    switch (type)
    {
    case TOK_MUL:
    case TOK_DIV:
    case TOK_MOD:
      return 1;
    case TOK_ADD:
    case TOK_SUB:
      return 2;
    case TOK_GTHAN:
    case TOK_LTHAN:
      return 3;
    case TOK_AND:
      return 4;
    case TOK_XOR:
      return 5;
    case TOK_OR:
      return 6;
    case TOK_ASSIGN:
    case TOK_QUESTION:
      return 7;
    case TOK_COMMA:
      return 8;
    default:
      return 999;
    }
  }

  static bool IsRTLBinaryOp(TokenType type) { return type == TOK_ASSIGN; }

  static bool IsBinaryOpWithPrecedence(const Token& tok, int precedence)
  {
    if (!tok.IsBinaryOperator())
      return false;

    const int tok_precedence = OperatorPrecedence(tok.type);
    return (tok_precedence < precedence) ||
           (IsRTLBinaryOp(tok.type) && tok_precedence <= precedence);
  }

  ParseResult ParseInfixOperations(int precedence = OperatorPrecedence())
  {
    ParseResult lhs = ParseAtom(Chew());

    if (lhs.status == ParseStatus::SyntaxError)
      return lhs;

    std::unique_ptr<Expression> expr = std::move(lhs.expr);

    while (true)
    {
      const Token op = Peek();
      if (IsBinaryOpWithPrecedence(op, precedence))
      {
        Chew();
        ParseResult rhs = ParseInfixOperations(OperatorPrecedence(op.type));
        if (rhs.status == ParseStatus::SyntaxError)
          return rhs;

        // Compound assignment token has operator in the data string.
        if (op.type == TOK_ASSIGN && op.data_length)
        {
          const TokenType op_type = GetBinaryOperatorTokenTypeFromChar(op.data[0]);
          expr = std::make_unique<CompoundAssignmentExpression>(op_type, std::move(expr),
                                                                std::move(rhs.expr));
        }
        else
        {
          expr = std::make_unique<BinaryExpression>(op.type, std::move(expr), std::move(rhs.expr));
        }
      }
      else if (op.type == TOK_QUESTION && OperatorPrecedence(TOK_QUESTION) <= precedence)
      {
        // Handle conditional operator: (a ? b : c)
        Chew();
        auto true_result = ParseInfixOperations(OperatorPrecedence(op.type));
        if (true_result.status != ParseStatus::Successful)
          return true_result;

        const Token should_be_colon = Chew();
        if (should_be_colon.type != TOK_COLON)
          return ParseResult::MakeErrorResult();

        auto false_result = ParseInfixOperations(OperatorPrecedence(op.type));
        if (false_result.status != ParseStatus::Successful)
          return false_result;

        auto conditional = MakeFunctionExpression("if");
        std::vector<std::unique_ptr<Expression>> args;
        args.emplace_back(std::move(expr));
        args.emplace_back(std::move(true_result.expr));
        args.emplace_back(std::move(false_result.expr));
        conditional->SetArguments(std::move(args));
        expr = std::move(conditional);
      }
      else
      {
        break;
      }
    }
    return ParseResult::MakeSuccessfulResult(std::move(expr));
  }

  ParseResult ParseParens()
  {
    // lparen already chewed
    ParseResult result = ParseToplevel();
    if (result.status != ParseStatus::Successful)
      return result;

    const auto rparen = Chew();
    if (rparen.type != TOK_RPAREN)
    {
      return ParseResult::MakeErrorResult();
    }

    return result;
  }

  ParseResult ParseHotkeys()
  {
    Token tok = Chew();
    if (tok.type != TOK_LPAREN)
      return ParseResult::MakeErrorResult();

    std::vector<std::unique_ptr<ControlExpression>> inputs;

    while (true)
    {
      tok = Chew();

      if (tok.type != TOK_CONTROL && tok.type != TOK_BAREWORD)
        return ParseResult::MakeErrorResult();

      ControlQualifier cq;
      cq.FromString(std::string(tok.Data()));
      inputs.emplace_back(std::make_unique<ControlExpression>(std::move(cq)));

      tok = Chew();

      if (tok.type == TOK_ADD)
        continue;

      if (tok.type == TOK_RPAREN)
        break;

      return ParseResult::MakeErrorResult();
    }

    return ParseResult::MakeSuccessfulResult(std::make_unique<HotkeyExpression>(std::move(inputs)));
  }

  ParseResult ParseToplevel() { return ParseInfixOperations(); }
};  // namespace ExpressionParser

ParseResult ParseTokens(const Token* tokens)
{
  return Parser(tokens).Parse();
}

static ParseResult ParseComplexExpression(const std::string& str)
{
  // one token at most for each character, and EOF
  Token* const tokens = static_cast<Token*>(std::malloc((str.size() + 1) * sizeof(Token)));
  std::size_t count;
  Lexer l(str.data(), str.size());
  if (!tokens)
    return ParseResult::MakeErrorResult();
  const ParseStatus tokenize_status = l.Tokenize(tokens, &count);
  if (tokenize_status != ParseStatus::Successful)
  {
    std::free(tokens);
    return ParseResult::MakeErrorResult();
  }
  ParseResult result = ParseTokens(tokens);
  std::free(tokens);
  return result;
}

static std::unique_ptr<Expression> ParseBarewordExpression(const std::string& str)
{
  ControlQualifier qualifier;
  qualifier.control_name = str;
  qualifier.has_device = false;

  // This control expression will only work (find the specified control) with the default device.
  return std::make_unique<ControlExpression>(qualifier);
}

ParseResult ParseExpression(const std::string& str)
{
  if (StripWhitespace(str).empty())
    return ParseResult::MakeEmptyResult();

  auto bareword_expr = ParseBarewordExpression(str);
  ParseResult complex_result = ParseComplexExpression(str);

  if (complex_result.status != ParseStatus::Successful)
  {
    // This is a bit odd.
    // Return the error status of the complex expression with the fallback barewords expression.
    complex_result.expr = std::move(bareword_expr);
    return complex_result;
  }

  complex_result.expr = std::make_unique<CoalesceExpression>(std::move(bareword_expr),
                                                             std::move(complex_result.expr));
  return complex_result;
}
}  // namespace ciface::ExpressionParser
