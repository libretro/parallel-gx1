// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Common
{
// A blocking HTTP request, run on the HTTP thread (Common/HttpService.h).
class HttpRequest final
{
public:
  explicit HttpRequest(std::chrono::milliseconds timeout_ms = std::chrono::milliseconds{3000});
  ~HttpRequest();
  HttpRequest(const HttpRequest&) = delete;
  HttpRequest& operator=(const HttpRequest&) = delete;

  using Response = std::optional<std::vector<u8>>;

  struct Multiform
  {
    std::string name;
    std::string data;
  };

  void FollowRedirects();
  s32 GetLastResponseCode() const;
  std::string GetHeaderValue(std::string_view name) const;
  // A response other than 200 is no response.
  Response Get(const std::string& url);
  Response Post(const std::string& url, std::string_view payload);
  Response PostMultiform(const std::string& url, const Multiform* multiform,
                         size_t multiform_count);

private:
  Response Fetch(const std::string& url, const char* method, const char* content_type,
                 const void* body, size_t body_len);

  int m_timeout_ms;
  bool m_follow_redirects = false;
  s32 m_last_response_code = 0;
  char* m_headers = nullptr;
};
}  // namespace Common
