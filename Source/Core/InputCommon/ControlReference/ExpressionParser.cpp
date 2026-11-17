// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControlReference/ExpressionParser.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

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

// The inputs hotkeys hold back: while a hotkey's modifiers are pressed, its final input reads as
// released to everything but hotkeys whose modifiers include each one holding it. One entry per
// final input and modifier, counted, as hotkeys can share them.
struct HotkeySuppression
{
  Device::Input* final_input;
  Device::Input* modifier;
  u32 level;
};

enum
{
  // Entries no allocation is needed for: far more than the hotkeys pressed at once
  HOTKEY_SUPPRESSIONS_INLINE = 64,
};

static HotkeySuppression s_suppressions_inline[HOTKEY_SUPPRESSIONS_INLINE];
static HotkeySuppression* s_suppressions = s_suppressions_inline;
static std::size_t s_num_suppressions;
static std::size_t s_cap_suppressions = HOTKEY_SUPPRESSIONS_INLINE;

static bool IsSuppressed(const Device::Input* input)
{
  // Input is suppressed if it is held back by any modifier.
  std::size_t i;
  for (i = 0; i < s_num_suppressions; i++)
  {
    if (s_suppressions[i].final_input == input)
      return true;
  }
  return false;
}

static void AddSuppression(Device::Input* final_input, Device::Input* modifier)
{
  std::size_t i;
  for (i = 0; i < s_num_suppressions; i++)
  {
    if (s_suppressions[i].final_input == final_input && s_suppressions[i].modifier == modifier)
    {
      s_suppressions[i].level++;
      return;
    }
  }
  if (s_num_suppressions == s_cap_suppressions)
  {
    const std::size_t cap = s_cap_suppressions * 2;
    HotkeySuppression* const grown =
        static_cast<HotkeySuppression*>(std::malloc(cap * sizeof(HotkeySuppression)));
    if (!grown)
      return;
    std::memcpy(grown, s_suppressions, s_num_suppressions * sizeof(HotkeySuppression));
    if (s_suppressions != s_suppressions_inline)
      std::free(s_suppressions);
    s_suppressions = grown;
    s_cap_suppressions = cap;
  }
  s_suppressions[s_num_suppressions].final_input = final_input;
  s_suppressions[s_num_suppressions].modifier = modifier;
  s_suppressions[s_num_suppressions].level = 1;
  s_num_suppressions++;
}

static void RemoveSuppression(Device::Input* modifier, Device::Input* final_input)
{
  std::size_t i;
  for (i = 0; i < s_num_suppressions; i++)
  {
    if (s_suppressions[i].final_input == final_input && s_suppressions[i].modifier == modifier)
    {
      if (--s_suppressions[i].level == 0)
        s_suppressions[i] = s_suppressions[--s_num_suppressions];
      return;
    }
  }
}

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
    if (IsSuppressed(m_input))
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

  ~VariableExpression()
  {
    if (m_variable)
      ReleaseExpressionVariable(m_variable);
  }

  ControlState GetValue() { return m_variable ? m_variable->value : 0; }

  void SetValue(ControlState value)
  {
    if (m_variable)
      m_variable->value = value;
  }

  int CountNumControls() const { return 1; }

  void UpdateReferences(ControlEnvironment& env)
  {
    ExpressionVariable* const variable = env.AcquireVariable(m_name);
    if (m_variable)
      ReleaseExpressionVariable(m_variable);
    m_variable = variable;
  }

protected:
  const std::string m_name;
  ExpressionVariable* m_variable = nullptr;
};

class HotkeyExpression : public Expression
{
public:
  // Takes the inputs, count of them in an allocation it frees: the modifiers, then the final input.
  HotkeyExpression(ControlExpression** inputs, u32 count)
      : Expression(ExpressionKind::Hotkey), m_modifiers(inputs), m_num_modifiers(count - 1),
        m_final_input(inputs[count - 1]),
        m_suppressed(static_cast<Device::Input**>(std::calloc(count, sizeof(Device::Input*))))
  {
  }

  ~HotkeyExpression()
  {
    ReleaseSuppression();
    for (u32 i = 0; i < m_num_modifiers; i++)
      delete m_modifiers[i];
    delete m_final_input;
    std::free(m_modifiers);
    std::free(m_suppressed);
  }

  HotkeyExpression(const HotkeyExpression&) = delete;
  HotkeyExpression& operator=(const HotkeyExpression&) = delete;

  ControlState GetValue()
  {
    // True if we have no modifiers
    bool modifiers_pressed = true;
    for (u32 i = 0; i < m_num_modifiers; i++)
    {
      if (m_modifiers[i]->GetValue() <= CONDITION_THRESHOLD)
      {
        modifiers_pressed = false;
        break;
      }
    }

    const auto final_input_state = m_final_input->GetValueIgnoringSuppression();

    if (modifiers_pressed)
    {
      // Ignore suppression of our own modifiers. This also allows superset modifiers to function.
      const bool is_suppressed = IsSuppressedIgnoringModifiers(m_final_input->GetInput());

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
      ReleaseSuppression();
      m_is_blocked = final_input_state > CONDITION_THRESHOLD;
    }

    return 0;
  }

  void SetValue(ControlState) {}

  int CountNumControls() const
  {
    int result = 0;
    for (u32 i = 0; i < m_num_modifiers; i++)
      result += m_modifiers[i]->CountNumControls();
    return result + m_final_input->CountNumControls();
  }

  void UpdateReferences(ControlEnvironment& env)
  {
    for (u32 i = 0; i < m_num_modifiers; i++)
      m_modifiers[i]->UpdateReferences(env);

    m_final_input->UpdateReferences(env);

    // We must update our suppression with valid pointers.
    if (m_suppressing)
      EnableSuppression(true);
  }

private:
  bool IsSuppressedIgnoringModifiers(Device::Input* input) const
  {
    // Input is suppressed if it is held back with a modifier that we aren't ignoring.
    for (std::size_t s = 0; s < s_num_suppressions; s++)
    {
      if (s_suppressions[s].final_input != input)
        continue;
      bool ignored = false;
      for (u32 i = 0; i < m_num_modifiers && !ignored; i++)
      {
        // We need to ignore L_Ctrl R_Ctrl when supplied Ctrl and vice-versa.
        Device::Input* const i1 = m_modifiers[i]->GetInput();
        Device::Input* const i2 = s_suppressions[s].modifier;
        ignored = i1 && i2 && (i1 == i2 || i1->IsChild(i2) || i2->IsChild(i1));
      }
      if (!ignored)
        return true;
    }
    return false;
  }

  // Suppresses each input + modifier pair, as they are now. The pairs are kept to be released.
  void EnableSuppression(bool force = false)
  {
    if (m_suppressing && !force)
      return;
    ReleaseSuppression();
    if (!m_suppressed)
      return;
    m_suppressed_final = m_final_input->GetInput();
    for (u32 i = 0; i < m_num_modifiers; i++)
    {
      m_suppressed[i] = m_modifiers[i]->GetInput();
      // Inputs might be null, don't suppress with nullptr
      if (m_suppressed_final && m_suppressed[i])
        AddSuppression(m_suppressed_final, m_suppressed[i]);
    }
    m_suppressing = true;
  }

  void ReleaseSuppression()
  {
    if (!m_suppressing)
      return;
    for (u32 i = 0; i < m_num_modifiers; i++)
      RemoveSuppression(m_suppressed[i], m_suppressed_final);
    m_suppressing = false;
  }

  ControlExpression** m_modifiers;
  u32 m_num_modifiers;
  ControlExpression* m_final_input;
  // The modifiers and the final input as they were suppressed
  Device::Input** m_suppressed;
  Device::Input* m_suppressed_final = nullptr;
  bool m_suppressing = false;
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

void ReleaseExpressionVariable(ExpressionVariable* variable)
{
  if (retro_atomic_fetch_sub_int(&variable->refs, 1) == 1)
  {
    std::free(variable->name);
    std::free(variable);
  }
}

void FreeExpressionVariables(ExpressionVariables* variables)
{
  std::size_t i;
  for (i = 0; i < variables->count; i++)
    ReleaseExpressionVariable(variables->items[i]);
  std::free(variables->items);
  variables->items = nullptr;
  variables->count = 0;
  variables->cap = 0;
}

ExpressionVariable* ControlEnvironment::AcquireVariable(const std::string& name)
{
  ExpressionVariable* variable = nullptr;
  std::size_t i;

  // Do not accept an empty string as key, even if the expression parser already prevents this case.
  if (name.empty())
    return nullptr;
  for (i = 0; i < m_variables.count; i++)
  {
    if (!std::strcmp(m_variables.items[i]->name, name.c_str()))
    {
      variable = m_variables.items[i];
      break;
    }
  }

  // If new, the table holds it
  if (!variable)
  {
    if (m_variables.count == m_variables.cap)
    {
      const std::size_t cap = m_variables.cap ? m_variables.cap * 2 : 8;
      ExpressionVariable** const grown = static_cast<ExpressionVariable**>(
          std::realloc(m_variables.items, cap * sizeof(ExpressionVariable*)));
      if (!grown)
        return nullptr;
      m_variables.items = grown;
      m_variables.cap = cap;
    }
    variable = static_cast<ExpressionVariable*>(std::calloc(1, sizeof(ExpressionVariable)));
    if (!variable)
      return nullptr;
    variable->name = static_cast<char*>(std::malloc(name.size() + 1));
    if (!variable->name)
    {
      std::free(variable);
      return nullptr;
    }
    std::memcpy(variable->name, name.c_str(), name.size() + 1);
    retro_atomic_store_relaxed_int(&variable->refs, 1);
    m_variables.items[m_variables.count++] = variable;
  }

  retro_atomic_fetch_add_int(&variable->refs, 1);
  return variable;
}

void ControlEnvironment::CleanUnusedVariables()
{
  std::size_t kept = 0;
  std::size_t i;
  for (i = 0; i < m_variables.count; i++)
  {
    ExpressionVariable* const variable = m_variables.items[i];
    // Don't count ourselves as reference. Only this thread acquires a variable, so one held by the
    // table alone stays so.
    if (retro_atomic_load_acquire_int(&variable->refs) <= 1)
      ReleaseExpressionVariable(variable);
    else
      m_variables.items[kept++] = variable;
  }
  m_variables.count = kept;
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
    if (TOK_LPAREN != Peek().type)
    {
      // Single argument with no parens (useful for unary ! function)
      const auto tok = Chew();
      auto arg = ParseAtom(tok);
      if (ParseStatus::Successful != arg.status)
        return arg;

      if (!func->AddArgument(std::move(arg.expr)))
        return ParseResult::MakeErrorResult();
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

          if (!func->AddArgument(std::move(arg.expr)))
            return ParseResult::MakeErrorResult();

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

    if (!func->ValidateArguments())
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
        if (!conditional->AddArgument(std::move(expr)) ||
            !conditional->AddArgument(std::move(true_result.expr)) ||
            !conditional->AddArgument(std::move(false_result.expr)))
        {
          return ParseResult::MakeErrorResult();
        }
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

    ControlExpression** inputs = nullptr;
    u32 count = 0;
    u32 cap = 0;
    // The inputs are freed when the expression cannot be made
    const auto free_inputs = [&] {
      for (u32 i = 0; i < count; i++)
        delete inputs[i];
      std::free(inputs);
    };

    while (true)
    {
      tok = Chew();

      if (tok.type != TOK_CONTROL && tok.type != TOK_BAREWORD)
      {
        free_inputs();
        return ParseResult::MakeErrorResult();
      }

      if (count == cap)
      {
        cap = cap ? cap * 2 : 4;
        ControlExpression** const grown =
            static_cast<ControlExpression**>(std::realloc(inputs, cap * sizeof(*inputs)));
        if (!grown)
        {
          free_inputs();
          return ParseResult::MakeErrorResult();
        }
        inputs = grown;
      }
      ControlQualifier cq;
      cq.FromString(std::string(tok.Data()));
      inputs[count++] = new ControlExpression(std::move(cq));

      tok = Chew();

      if (tok.type == TOK_ADD)
        continue;

      if (tok.type == TOK_RPAREN)
        break;

      free_inputs();
      return ParseResult::MakeErrorResult();
    }

    return ParseResult::MakeSuccessfulResult(std::make_unique<HotkeyExpression>(inputs, count));
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
