// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>

#include "InputCommon/ControllerInterface/CoreDevice.h"

#include <retro_atomic.h>

namespace ciface::ExpressionParser
{
enum TokenType
{
  TOK_WHITESPACE,
  TOK_INVALID,
  TOK_EOF,
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_NOT,
  TOK_CONTROL,
  TOK_LITERAL,
  TOK_VARIABLE,
  TOK_BAREWORD,
  TOK_COMMENT,
  TOK_HOTKEY,
  TOK_QUESTION,
  TOK_COLON,
  // Binary Ops:
  TOK_BINARY_OPS_BEGIN,
  TOK_COMPOUND_ASSIGN_OPS_BEGIN = TOK_BINARY_OPS_BEGIN,
  TOK_AND = TOK_BINARY_OPS_BEGIN,
  TOK_OR,
  TOK_XOR,
  TOK_ADD,
  TOK_SUB,
  TOK_MUL,
  TOK_DIV,
  TOK_MOD,
  TOK_COMPOUND_ASSIGN_OPS_END,
  TOK_ASSIGN = TOK_COMPOUND_ASSIGN_OPS_END,
  TOK_LTHAN,
  TOK_GTHAN,
  TOK_COMMA,
  TOK_BINARY_OPS_END,
};

// A token of an expression: its type, and its text, lent from the expression being parsed, which
// outlives the tokens.
struct Token
{
  TokenType type;
  // The token's own text: a control's or variable's name, a literal, the operator of a compound
  // assignment. Not terminated.
  const char* data;
  std::size_t data_length;

  // Position in the input string:
  std::size_t string_position;
  std::size_t string_length;

  std::string_view Data() const { return std::string_view(data, data_length); }
  bool IsBinaryOperator() const;
};

enum class ParseStatus
{
  Successful,
  // Note that the expression could still work in this case (be valid and return a value)
  SyntaxError,
  // Will return the default value
  EmptyExpression,
};

// Splits an expression's text into tokens. The text is lent and outlives the lexer and its tokens.
class Lexer
{
public:
  Lexer(const char* expr, std::size_t length);

  // At most length + 1 tokens: one for each character and EOF. tokens has room for them; count
  // is how many were made.
  ParseStatus Tokenize(Token* tokens, std::size_t* count);

private:
  const char* m_expr;
  std::size_t m_length;
  std::size_t m_pos;

  void FetchWordChars();
  Token MakeToken(TokenType type, std::size_t data_start, std::size_t data_end) const;
  Token GetDelimitedToken(TokenType type, char delimiter);
  Token GetVariable();
  Token GetBareword();
  Token GetRealLiteral();

  Token NextToken();
};

class ControlQualifier
{
public:
  bool has_device;
  Core::DeviceQualifier device_qualifier;
  // Makes no distinction between input and output; a longer name is cut to fit
  static constexpr std::size_t CONTROL_NAME_SIZE = 256;
  char control_name[CONTROL_NAME_SIZE];

  ControlQualifier() : has_device(false), control_name{} {}

  // The name from len characters of str, cut to fit
  void SetControlName(const char* str, std::size_t len)
  {
    if (len >= sizeof(control_name))
      len = sizeof(control_name) - 1;
    std::memcpy(control_name, str, len);
    control_name[len] = '\0';
  }

  // From "device:control", or a control of the default device: len characters, not terminated.
  void FromString(const char* str, std::size_t len)
  {
    std::size_t col_pos = len;
    while (col_pos && str[col_pos - 1] != ':')
      col_pos--;

    has_device = col_pos != 0;
    if (has_device)
    {
      device_qualifier.FromString(str, col_pos - 1);
      SetControlName(str + col_pos, len - col_pos);
    }
    else
    {
      device_qualifier.FromString(str, 0);
      SetControlName(str, len);
    }
  }
};

// A variable of a controller's expressions: its name and value. Freed when neither the
// controller's table nor a bound expression holds it any more; an expression being read by another
// thread holds the variable it reads.
struct ExpressionVariable
{
  retro_atomic_int_t refs;
  ControlState value;
  char* name;
};

// One holder less: the last one frees the variable.
void ReleaseExpressionVariable(ExpressionVariable* variable);

// The variables of a controller's expressions, found by name; the table holds each of them.
struct ExpressionVariables
{
  ExpressionVariable** items;
  std::size_t count;
  std::size_t cap;
};

// The table lets go of its variables, and is left empty.
void FreeExpressionVariables(ExpressionVariables* variables);

class ControlEnvironment
{
public:
  using VariableContainer = ExpressionVariables;

  ControlEnvironment(const Core::DeviceContainer& container_, const Core::DeviceQualifier& default_,
                     VariableContainer& vars)
      : m_variables(vars), container(container_), default_device(default_)
  {
  }

  // with a reference the caller releases
  Core::Device* FindDevice(const ControlQualifier& qualifier) const;
  // The variable of that name, len characters terminated, which the caller now also holds; null
  // for an empty name or when out of memory.
  ExpressionVariable* AcquireVariable(const char* name, std::size_t len);

  // Lets go of the variables no expression holds any more.
  void CleanUnusedVariables();

private:
  VariableContainer& m_variables;
  const Core::DeviceContainer& container;
  const Core::DeviceQualifier& default_device;
};

// The node an Expression is, which its calls branch on
enum class ExpressionKind : u8
{
  Control,
  Binary,
  CompoundAssignment,
  Literal,
  Variable,
  Hotkey,
  Coalesce,
  Function,
};

class Expression
{
public:
  // These branch to the node this is.
  ControlState GetValue();
  void SetValue(ControlState state);
  int CountNumControls() const;
  void UpdateReferences(ControlEnvironment& finder);

  // Perform any side effects and return Expression to be SetValue'd.
  Expression* GetLValue();

  // Destroys and frees the node as the node it is; delete never runs this destructor alone
  void operator delete(Expression* expression, std::destroying_delete_t);
  ~Expression() = default;

  const ExpressionKind expression_kind;

protected:
  explicit Expression(ExpressionKind kind) : expression_kind(kind) {}
  Expression(const Expression&) = delete;
  Expression& operator=(const Expression&) = delete;

  // A node that has no l-value of its own is set itself
  Expression* GetLValueDefault() { return this; }
};

// Frees the expression, when there is one.
inline void FreeExpression(Expression* expression)
{
  // a destroying delete is not to be handed null
  if (expression)
    delete expression;
}

// What a parse made: the expression it parsed into, which the receiver owns, null when there is
// none. Only ParseExpression gives one with an error: the bareword fallback.
struct ParseResult
{
  static ParseResult MakeEmptyResult();
  static ParseResult MakeSuccessfulResult(Expression* expr);
  static ParseResult MakeErrorResult();

  ParseStatus status;
  Expression* expr;
};

ParseResult ParseExpression(const std::string& expr);
ParseResult ParseTokens(const Token* tokens);

}  // namespace ciface::ExpressionParser
