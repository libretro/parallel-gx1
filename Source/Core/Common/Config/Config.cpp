// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Config/Config.h"

#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>

#include <retro_atomic.h>
#include "Common/CallGate.h"
#include "Common/Quiesce.h"

namespace Config
{
// Every structure here is read far more often than written, and read from any thread: a
// published pointer per entry, swapped whole by writers and freed once no reader is inside.

namespace
{
struct LayerRef
{
  quiesce_node_t node;  // first: the free callback casts it back to the whole
  Layer* layer;         // owned
};

struct CallbackRef
{
  quiesce_node_t node;  // first, as above
  size_t id;
  ConfigChangedCallback func;
  void* user;
  call_gate_t gate;
};

// Callbacks registered at once; Dolphin uses a few dozen.
constexpr size_t MAX_CALLBACKS = 128;
constexpr size_t NUM_LAYERS = static_cast<size_t>(LayerType::Meta);

}  // namespace

static quiesce_t s_quiesce;
static quiesce_t s_info_quiesce;
static retro_atomic_ptr_t s_layers[NUM_LAYERS];
static retro_atomic_ptr_t s_callbacks[MAX_CALLBACKS];
static retro_atomic_size_t s_next_callback_id;
static retro_atomic_int_t s_callback_guards;
static retro_atomic_64_t s_config_version;

quiesce_t* detail::InfoQuiesce()
{
  return &s_info_quiesce;
}

static void FreeLayerRef(quiesce_node_t* node)
{
  LayerRef* const ref = reinterpret_cast<LayerRef*>(node);
  delete ref->layer;
  delete ref;
}

static void FreeCallbackRef(quiesce_node_t* node)
{
  delete reinterpret_cast<CallbackRef*>(node);
}

// Callers are inside s_quiesce; the Layer stays alive until they leave.
static Layer* PeekLayer(LayerType type)
{
  const auto* ref =
      static_cast<const LayerRef*>(retro_atomic_load_acquire_ptr(&s_layers[size_t(type)]));
  return ref ? ref->layer : nullptr;
}

// takes layer
static void PublishLayer(LayerType type, Layer* layer)
{
  LayerRef* ref = layer ? new LayerRef{{}, layer} : nullptr;
  void* old = retro_atomic_exchange_ptr(&s_layers[size_t(type)], ref);
  if (old)
    quiesce_retire(&s_quiesce, &static_cast<LayerRef*>(old)->node, FreeLayerRef);
}

// Entered here, left in the destructor
static Layer* EnterLayer(LayerType type)
{
  quiesce_enter(&s_quiesce);
  return PeekLayer(type);
}

LayerAccess::LayerAccess(LayerType type) : layer(EnterLayer(type))
{
}

LayerAccess::~LayerAccess()
{
  quiesce_exit(&s_quiesce);
}

void AddLayer(ConfigLayerLoader* loader)
{
  Layer* const layer = new Layer(loader);
  PublishLayer(layer->GetLayer(), layer);
  OnConfigChanged();
}

void RemoveLayer(LayerType layer)
{
  PublishLayer(layer, nullptr);
  OnConfigChanged();
}

ConfigChangedCallbackID AddConfigChangedCallback(ConfigChangedCallback func, void* user)
{
  auto* ref = new CallbackRef{{}, 0, func, user, {}};
  ref->id = retro_atomic_fetch_add_size(&s_next_callback_id, 1);
  for (auto& slot : s_callbacks)
  {
    if (!retro_atomic_load_acquire_ptr(&slot) && retro_atomic_cas_ptr(&slot, nullptr, ref))
      return ConfigChangedCallbackID{ref->id};
  }
  // More live callbacks than MAX_CALLBACKS: a leak in the caller, not a load to grow for.
  std::abort();
}

void RemoveConfigChangedCallback(ConfigChangedCallbackID callback_id)
{
  quiesce_enter(&s_quiesce);
  for (auto& slot : s_callbacks)
  {
    auto* ref = static_cast<CallbackRef*>(retro_atomic_load_acquire_ptr(&slot));
    if (!ref || ref->id != callback_id.id || !retro_atomic_cas_ptr(&slot, ref, nullptr))
      continue;

    // A trigger on another thread may still be inside it, and the caller is about to free
    // whatever it captured.
    call_gate_close(&ref->gate);
    quiesce_retire(&s_quiesce, &ref->node, FreeCallbackRef);
    break;
  }
  quiesce_exit(&s_quiesce);
}

void OnConfigChanged()
{
  // Increment the config version to invalidate caches.
  // To ensure that getters do not return stale data, this should always be done
  // even when callbacks are suppressed.
  retro_atomic_fetch_add_64(&s_config_version, 1);

  if (retro_atomic_load_acquire_int(&s_callback_guards))
    return;

  quiesce_enter(&s_quiesce);
  for (auto& slot : s_callbacks)
  {
    auto* ref = static_cast<CallbackRef*>(retro_atomic_load_acquire_ptr(&slot));
    if (!ref)
      continue;
    call_gate_frame_t frame;
    if (call_gate_enter(&ref->gate, &frame))
    {
      ref->func(ref->user);
      call_gate_exit(&ref->gate, &frame);
    }
  }
  quiesce_exit(&s_quiesce);
}

u64 GetConfigVersion()
{
  return static_cast<u64>(retro_atomic_load_acquire_64(&s_config_version));
}

// Explicit load and save of layers
void Load()
{
  quiesce_enter(&s_quiesce);
  for (size_t i = 0; i < NUM_LAYERS; ++i)
  {
    if (Layer* layer = PeekLayer(LayerType(i)))
      layer->Load();
  }
  quiesce_exit(&s_quiesce);
  OnConfigChanged();
}

void Save()
{
  quiesce_enter(&s_quiesce);
  for (size_t i = 0; i < NUM_LAYERS; ++i)
  {
    if (Layer* layer = PeekLayer(LayerType(i)))
      layer->Save();
  }
  quiesce_exit(&s_quiesce);
  OnConfigChanged();
}

void Init()
{
  // These layers contain temporary values
  ClearCurrentRunLayer();
}

void Shutdown()
{
  for (size_t i = 0; i < NUM_LAYERS; ++i)
    PublishLayer(LayerType(i), nullptr);
}

void ClearCurrentRunLayer()
{
  PublishLayer(LayerType::CurrentRun, new Layer(LayerType::CurrentRun));
}

// Each system's name, in the order of System
static const char* const s_system_names[] = {
    "Dolphin",  "SYSCONF", "GCPad",   "Wiimote",          "GCKeyboard",
    "Graphics", "Logger",  "Session", "GameSettingsOnly", "Achievements",
};
static_assert(sizeof(s_system_names) / sizeof(s_system_names[0]) ==
              static_cast<size_t>(System::Achievements) + 1);

const char* GetSystemName(System system)
{
  return s_system_names[static_cast<size_t>(system)];
}

bool GetSystemFromName(const char* name, System* system)
{
  for (size_t i = 0; i < sizeof(s_system_names) / sizeof(s_system_names[0]); i++)
  {
    if (!std::strcmp(s_system_names[i], name))
    {
      *system = static_cast<System>(i);
      return true;
    }
  }
  return false;
}

LayerType GetActiveLayerForConfig(const Location& config)
{
  // If config is not present in any layer, base layer is considered active.
  LayerType result = LayerType::Base;
  quiesce_enter(&s_quiesce);
  for (auto type : SEARCH_ORDER)
  {
    const Layer* layer = type == LayerType::Meta ? nullptr : PeekLayer(type);
    if (layer && layer->Exists(config))
    {
      result = type;
      break;
    }
  }
  quiesce_exit(&s_quiesce);
  return result;
}

std::optional<std::string> GetAsString(const Location& config)
{
  std::optional<std::string> result;
  quiesce_enter(&s_quiesce);
  for (auto type : SEARCH_ORDER)
  {
    const Layer* layer = type == LayerType::Meta ? nullptr : PeekLayer(type);
    if (layer)
    {
      result = layer->Get<std::string>(config);
      if (result.has_value())
        break;
    }
  }
  quiesce_exit(&s_quiesce);
  return result;
}

ConfigChangeCallbackGuard::ConfigChangeCallbackGuard()
{
  retro_atomic_inc_int(&s_callback_guards);
}

ConfigChangeCallbackGuard::~ConfigChangeCallbackGuard()
{
  if (retro_atomic_fetch_sub_int(&s_callback_guards, 1) != 1)
    return;

  OnConfigChanged();
}

}  // namespace Config
