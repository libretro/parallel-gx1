// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControlReference/ControlReference.h"

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
void ControlReference::FreeBinding(quiesce_node_t* node)
{
  delete reinterpret_cast<Binding*>(node);
}

void ControlReference::Publish(Binding* binding)
{
  retro_atomic_store_release_int(&m_has_expression, !binding->expression.empty());
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
    auto parse_result = ParseExpression(cur->expression);
    auto* next = new Binding{{}, cur->expression, parse_result.status, std::move(parse_result.expr)};
    if (next->parsed)
      next->parsed->UpdateReferences(env);

    // Lost to a SetExpression or another rebind meanwhile: bind what is there now.
    if (retro_atomic_cas_ptr(&m_binding, cur, next))
    {
      quiesce_retire(ciface::InputQuiesce(), &cur->node, FreeBinding);
      return;
    }
    delete next;
  }
}

int ControlReference::BoundCount() const
{
  ciface::InputReadScope scope;
  const Binding* const b = PeekBinding();
  return b && b->parsed ? b->parsed->CountNumControls() : 0;
}

std::string ControlReference::GetExpression() const
{
  ciface::InputReadScope scope;
  const Binding* const b = PeekBinding();
  return b ? b->expression : std::string{};
}

bool ControlReference::HasExpression() const
{
  return retro_atomic_load_acquire_int(&m_has_expression) != 0;
}

std::optional<std::string> ControlReference::SetExpression(std::string expr)
{
  auto parse_result = ParseExpression(expr);
  Publish(new Binding{{}, std::move(expr), parse_result.status, std::move(parse_result.expr)});
  return parse_result.description;
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

ControlReference::ControlReference()
{
  SetRange(1.0);
}

ControlReference::~ControlReference()
{
  delete static_cast<Binding*>(retro_atomic_exchange_ptr(&m_binding, nullptr));
}


InputReference::InputReference() : ControlReference()
{
}

OutputReference::OutputReference() : ControlReference()
{
}

bool InputReference::IsInput() const
{
  return true;
}
bool OutputReference::IsInput() const
{
  return false;
}

//
// InputReference :: State
//
// Gets the state of an input reference
// override function for ControlReference::State ...
//
ControlState InputReference::State(const ControlState ignore)
{
  if (!GetInputGate())
    return 0.0;
  ciface::InputReadScope scope;
  const Binding* const b = PeekBinding();
  return b && b->parsed ? b->parsed->GetValue() * GetRange() : 0.0;
}

//
// OutputReference :: State
//
// Set the state of all binded outputs
// overrides ControlReference::State .. combined them so I could make the GUI simple / inputs ==
// same as outputs one list
// I was lazy and it works so watever
//
ControlState OutputReference::State(const ControlState state)
{
  ciface::InputReadScope scope;
  const Binding* const b = PeekBinding();
  if (b && b->parsed)
    b->parsed->SetValue(state * GetRange());
  return 0.0;
}
