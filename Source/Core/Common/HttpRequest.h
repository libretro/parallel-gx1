// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

namespace Common
{
class HttpRequest final
{
public:
  explicit HttpRequest(int timeout_ms = 3000);
  ~HttpRequest();
  HttpRequest(const HttpRequest&) = delete;
  HttpRequest& operator=(const HttpRequest&) = delete;

  struct Multiform
  {
    const char* name;
    const void* data;
    size_t data_size;
  };

  void FollowRedirects();
  s32 GetLastResponseCode() const;
  // The value of a header of the last reply, or nullptr when it has none.
  const char* GetHeaderValue(const char* name) const;
  // The body of the last reply, valid until the next request.
  const u8* GetBody() const { return m_body; }
  size_t GetBodySize() const { return m_body_size; }
  // False on anything other than a 200 reply.
  bool Get(const char* url);
  bool Post(const char* url, const char* payload, size_t payload_size);
  bool PostMultiform(const char* url, const Multiform* multiform, size_t multiform_count);

private:
  bool Fetch(const char* url, const char* method, const char* content_type, const void* body,
             size_t body_len);
  static bool AppendBody(void* userdata, const void* data, size_t len);

  int m_timeout_ms;
  bool m_follow_redirects = false;
  s32 m_last_response_code = 0;
  char* m_headers = nullptr;
  // Grown by doubling and kept across requests
  u8* m_body = nullptr;
  size_t m_body_size = 0;
  size_t m_body_capacity = 0;
};
}  // namespace Common
