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
  std::free(m_body);
}

void HttpRequest::FollowRedirects()
{
  m_follow_redirects = true;
}

s32 HttpRequest::GetLastResponseCode() const
{
  return m_last_response_code;
}

const char* HttpRequest::GetHeaderValue(const char* name) const
{
  return m_headers ? net_http_header_value(m_headers, name) : nullptr;
}

bool HttpRequest::Get(const char* url)
{
  return Fetch(url, "GET", nullptr, nullptr, 0);
}

bool HttpRequest::Post(const char* url, const char* payload, size_t payload_size)
{
  return Fetch(url, "POST", "application/x-www-form-urlencoded", payload, payload_size);
}

#define MULTIFORM_BOUNDARY "------------------------dolphinemuwc24"

bool HttpRequest::PostMultiform(const char* url, const Multiform* multiform, size_t multiform_count)
{
  static const char part_head[] =
      "--" MULTIFORM_BOUNDARY "\r\nContent-Disposition: form-data; name=\"";
  static const char part_name_end[] = "\"\r\n\r\n";
  static const char tail[] = "--" MULTIFORM_BOUNDARY "--\r\n";
  size_t size = sizeof(tail) - 1;
  size_t i;
  for (i = 0; i < multiform_count; i++)
  {
    size += sizeof(part_head) - 1 + std::strlen(multiform[i].name) + sizeof(part_name_end) - 1 +
            multiform[i].data_size + 2;
  }

  char* const body = static_cast<char*>(std::malloc(size));
  if (!body)
    return false;
  char* out = body;
  for (i = 0; i < multiform_count; i++)
  {
    const size_t name_size = std::strlen(multiform[i].name);
    std::memcpy(out, part_head, sizeof(part_head) - 1);
    out += sizeof(part_head) - 1;
    std::memcpy(out, multiform[i].name, name_size);
    out += name_size;
    std::memcpy(out, part_name_end, sizeof(part_name_end) - 1);
    out += sizeof(part_name_end) - 1;
    if (multiform[i].data_size)
      std::memcpy(out, multiform[i].data, multiform[i].data_size);
    out += multiform[i].data_size;
    *out++ = '\r';
    *out++ = '\n';
  }
  std::memcpy(out, tail, sizeof(tail) - 1);

  const bool result =
      Fetch(url, "POST", "multipart/form-data; boundary=" MULTIFORM_BOUNDARY, body, size);
  std::free(body);
  return result;
}

bool HttpRequest::AppendBody(void* userdata, const void* data, size_t len)
{
  HttpRequest* const request = static_cast<HttpRequest*>(userdata);
  if (request->m_body_size + len > request->m_body_capacity)
  {
    size_t capacity = request->m_body_capacity ? request->m_body_capacity : 4096;
    while (capacity < request->m_body_size + len)
      capacity *= 2;
    u8* const grown = static_cast<u8*>(std::realloc(request->m_body, capacity));
    if (!grown)
      return false;
    request->m_body = grown;
    request->m_body_capacity = capacity;
  }
  std::memcpy(request->m_body + request->m_body_size, data, len);
  request->m_body_size += len;
  return true;
}

bool HttpRequest::Fetch(const char* url, const char* method, const char* content_type,
                        const void* body, size_t body_len)
{
  m_body_size = 0;
  http_job_t job{};
  job.url = url;
  job.method = method;
  job.content_type = content_type;
  job.body = body;
  job.body_len = body_len;
  job.sink = AppendBody;
  job.sink_userdata = this;
  job.timeout_ms = m_timeout_ms;
  job.follow_redirects = m_follow_redirects;
  http_service_run(&job);

  std::free(m_headers);
  m_headers = job.headers;
  m_last_response_code = job.status < 0 ? 0 : job.status;
  if (job.status != 200)
  {
    if (job.status < 0)
      ERROR_LOG(COMMON, "Failed to %s %s", method, url);
    else
      ERROR_LOG(COMMON, "Failed to %s %s: server replied with code %d", method, url, job.status);
    return false;
  }
  return true;
}
}  // namespace Common
