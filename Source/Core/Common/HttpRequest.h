// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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
  explicit HttpRequest(int timeout_ms = 3000);
  ~HttpRequest();
  HttpRequest(const HttpRequest&) = delete;
  HttpRequest& operator=(const HttpRequest&) = delete;

  struct Multiform
  {
    std::string name;
    std::string data;
  };

  void FollowRedirects();
  s32 GetLastResponseCode() const;
  std::string GetHeaderValue(std::string_view name) const;
  // Fill response with the body; false on anything other than a 200 reply.
  bool Get(const std::string& url, std::vector<u8>* response);
  bool Post(const std::string& url, std::string_view payload, std::vector<u8>* response);
  bool PostMultiform(const std::string& url, const Multiform* multiform, size_t multiform_count,
                     std::vector<u8>* response);

private:
  bool Fetch(const std::string& url, const char* method, const char* content_type, const void* body,
             size_t body_len, std::vector<u8>* response);

  int m_timeout_ms;
  bool m_follow_redirects = false;
  s32 m_last_response_code = 0;
  char* m_headers = nullptr;
};
}  // namespace Common
