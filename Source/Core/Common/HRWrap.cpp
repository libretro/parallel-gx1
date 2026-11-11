// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "HRWrap.h"

#include <cstdio>

#include <windows.h>

namespace Common
{
HResultText HRText(HRESULT hr)
{
  HResultText out;
  char message[256];
  DWORD len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                             static_cast<DWORD>(hr), 0, message, sizeof(message), nullptr);
  while (len && (message[len - 1] == '\r' || message[len - 1] == '\n' || message[len - 1] == ' '))
    message[--len] = '\0';
  snprintf(out.text, sizeof(out.text), "%s (%#010x)", len ? message : "Unknown error",
           static_cast<unsigned>(hr));
  return out;
}

std::string GetHResultMessage(HRESULT hr)
{
  auto err = winrt::hresult_error(hr);
  return winrt::to_string(err.message());
}
std::string GetHResultMessage(const winrt::hresult& hr)
{
  return GetHResultMessage(hr.value);
}
}  // namespace Common
