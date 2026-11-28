// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControlReference/ControlReference.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "Common/CommonTypes.h"

using namespace ciface::ExpressionParser;

static thread_local bool tls_input_gate = true;

void ControlReference::SetInputGate(bool enable)
{
  tls_input_gate = enable;
}

bool ControlReference::GetInputGate()
{
  return tls_input_gate;
}

//
// UpdateReference
//
// Updates a controlreference's binded devices/controls
// need to call this to re-bind a control reference after changing its expression
//
ControlReference::Binding* ControlReference::NewBinding(const char* expr, size_t length)
{
  Binding* const b =
      static_cast<Binding*>(std::malloc(offsetof(Binding, expression) + length + 1));
  if (!b)
    return nullptr;
  std::memset(&b->node, 0, sizeof(b->node));
  std::memcpy(b->expression, expr, length);
  b->expression[length] = '\0';
  b->expression_length = length;
  const ParseResult parse_result = ParseExpression(b->expression, length);
  b->status = parse_result.status;
  b->parsed = parse_result.expr;
  return b;
}

void ControlReference::DeleteBinding(Binding* binding)
{
  if (!binding)
    return;
  FreeExpression(binding->parsed);
  std::free(binding);
}

void ControlReference::FreeBinding(quiesce_node_t* node)
{
  DeleteBinding(reinterpret_cast<Binding*>(node));
}

void ControlReference::Publish(Binding* binding)
{
  retro_atomic_store_release_int(&m_has_expression, binding->expression_length != 0);
  if (void* old = retro_atomic_exchange_ptr(&m_binding, binding))
    quiesce_retire(ciface::InputQuiesce(), &static_cast<Binding*>(old)->node, FreeBinding);
}

void ControlReference::UpdateReference(ciface::ExpressionParser::ControlEnvironment& env)
{
  ciface::InputReadScope scope;
  for (;;)
  {
    Binding* const cur = PeekBinding();
    if (!cur || !cur->parsed)
      return;

    // A fresh parse to bind, leaving the published one untouched for its readers.
    Binding* const next = NewBinding(cur->expression, cur->expression_length);
    if (!next)
      return;
    if (next->parsed)
      next->parsed->UpdateReferences(env);

    // Lost to a SetExpression or another rebind meanwhile: bind what is there now.
    if (retro_atomic_cas_ptr(&m_binding, cur, next))
    {
      quiesce_retire(ciface::InputQuiesce(), &cur->node, FreeBinding);
      return;
    }
    DeleteBinding(next);
  }
}

int ControlReference::BoundCount() const
{
  ciface::InputReadScope scope;
  const Binding* const b = PeekBinding();
  return b && b->parsed ? b->parsed->CountNumControls() : 0;
}

char* ControlReference::CopyExpression() const
{
  ciface::InputReadScope scope;
  const Binding* const b = PeekBinding();
  if (!b || !b->expression_length)
    return nullptr;
  char* const copy = static_cast<char*>(std::malloc(b->expression_length + 1));
  if (copy)
    std::memcpy(copy, b->expression, b->expression_length + 1);
  return copy;
}

bool ControlReference::HasExpression() const
{
  return retro_atomic_load_acquire_int(&m_has_expression) != 0;
}

void ControlReference::SetExpression(const char* expr)
{
  SetExpression(expr, std::strlen(expr));
}

void ControlReference::SetExpression(const char* expr, size_t length)
{
  if (Binding* const binding = NewBinding(expr, length))
    Publish(binding);
}

ControlState ControlReference::GetRange() const
{
  const u64 bits = static_cast<u64>(retro_atomic_load_acquire_64(&m_range_bits));
  ControlState range;
  std::memcpy(&range, &bits, sizeof(range));
  return range;
}

void ControlReference::SetRange(ControlState range)
{
  u64 bits;
  std::memcpy(&bits, &range, sizeof(bits));
  retro_atomic_store_release_64(&m_range_bits, static_cast<int64_t>(bits));
}

ControlReference::ControlReference(Kind kind) : m_kind(kind)
{
  SetRange(1.0);
}

ControlReference::~ControlReference()
{
  DeleteBinding(static_cast<Binding*>(retro_atomic_exchange_ptr(&m_binding, nullptr)));
}

ControlState ControlReference::State(const ControlState state)
{
  if (m_kind == Kind::Output)
  {
    ciface::InputReadScope scope;
    const Binding* const b = PeekBinding();
    if (b && b->parsed)
      b->parsed->SetValue(state * GetRange());
    return 0.0;
  }

  if (!GetInputGate())
    return 0.0;
  ciface::InputReadScope scope;
  const Binding* const b = PeekBinding();
  return b && b->parsed ? b->parsed->GetValue() * GetRange() : 0.0;
}
