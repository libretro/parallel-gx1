// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

namespace WC24PatchEngine
{
// A network patch of a WiiConnect24 channel's INI file: the domain it replaces, and with what;
// is_kd for the KD's downloads, else for the channel's own requests.
struct NetworkPatch
{
  char* name;
  char* source;
  char* replacement;
  bool enabled;
  bool is_kd;
};

void Reload();

// The replacement of the domain, or null when no enabled patch replaces it.
const char* GetNetworkPatch(const char* source, size_t source_len, bool is_kd);

// When we patch for the Socket, we aren't given the URL. Instead, it is in a Host header in the
// payload that the socket is going to send: the payload with that header's domain replaced, in
// an allocation the caller frees, or null when no enabled patch replaces it.
char* GetNetworkPatchByPayload(const char* payload, size_t size, size_t* patched_size);
}  // namespace WC24PatchEngine
