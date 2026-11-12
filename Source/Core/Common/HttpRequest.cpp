// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/HttpRequest.h"

#include <cstdlib>
#include <cstring>

#include <net/net_http.h>

#include "Common/HttpService.h"
#include "Common/Logging/Log.h"

namespace Common
{
HttpRequest::HttpRequest(int timeout_ms) : m_timeout_ms(timeout_ms)
{
}

HttpRequest::~HttpRequest()
{
  std::free(m_headers);
}

void HttpRequest::FollowRedirects()
{
  m_follow_redirects = true;
}

s32 HttpRequest::GetLastResponseCode() const
{
  return m_last_response_code;
}

std::string HttpRequest::GetHeaderValue(std::string_view name) const
{
  char key[128];
  if (!m_headers || name.size() >= sizeof(key))
    return {};
  std::memcpy(key, name.data(), name.size());
  key[name.size()] = '\0';
  const char* value = net_http_header_value(m_headers, key);
  return value ? std::string(value) : std::string();
}

HttpRequest::Response HttpRequest::Get(const std::string& url)
{
  return Fetch(url, "GET", nullptr, nullptr, 0);
}

HttpRequest::Response HttpRequest::Post(const std::string& url, std::string_view payload)
{
  return Fetch(url, "POST", "application/x-www-form-urlencoded", payload.data(), payload.size());
}

HttpRequest::Response HttpRequest::PostMultiform(const std::string& url,
                                                 const Multiform* multiform,
                                                 size_t multiform_count)
{
  static const char boundary[] = "------------------------dolphinemuwc24";
  std::string body;
  for (size_t i = 0; i < multiform_count; i++)
  {
    body += "--";
    body += boundary;
    body += "\r\nContent-Disposition: form-data; name=\"";
    body += multiform[i].name;
    body += "\"\r\n\r\n";
    body += multiform[i].data;
    body += "\r\n";
  }
  body += "--";
  body += boundary;
  body += "--\r\n";
  std::string content_type = "multipart/form-data; boundary=";
  content_type += boundary;
  return Fetch(url, "POST", content_type.c_str(), body.data(), body.size());
}

static bool AppendBody(void* userdata, const void* data, size_t len)
{
  auto* buffer = static_cast<std::vector<u8>*>(userdata);
  const u8* bytes = static_cast<const u8*>(data);
  buffer->insert(buffer->end(), bytes, bytes + len);
  return true;
}

HttpRequest::Response HttpRequest::Fetch(const std::string& url, const char* method,
                                         const char* content_type, const void* body,
                                         size_t body_len)
{
  std::vector<u8> buffer;
  http_job_t job{};
  job.url = url.c_str();
  job.method = method;
  job.content_type = content_type;
  job.body = body;
  job.body_len = body_len;
  job.sink = AppendBody;
  job.sink_userdata = &buffer;
  job.timeout_ms = m_timeout_ms;
  job.follow_redirects = m_follow_redirects;
  http_service_run(&job);

  std::free(m_headers);
  m_headers = job.headers;
  m_last_response_code = job.status < 0 ? 0 : job.status;
  if (job.status != 200)
  {
    if (job.status < 0)
      ERROR_LOG(COMMON, "Failed to %s %s", method, url.c_str());
    else
      ERROR_LOG(COMMON, "Failed to %s %s: server replied with code %d", method, url.c_str(),
                job.status);
    return {};
  }
  return buffer;
}
}  // namespace Common
