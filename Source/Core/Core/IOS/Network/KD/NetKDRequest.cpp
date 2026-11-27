// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Network/KD/NetKDRequest.h"
#include "Common/Thread.h"

#include <algorithm>
#include <cinttypes>
#include <retro_miscellaneous.h>
#include <string>
#include <string_view>
#include <utility>

#include <crypto/crypto.h>
#include <crypto/kdf.h>

#include "Common/BitUtils.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FrameClock.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/SettingsHandler.h"
#include "Common/StringUtil.h"

#include "Common/Random.h"
#include "Common/ScopeGuard.h"
#include "Core/CommonTitles.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Network/KD/NetKDTime.h"
#include "Core/IOS/Network/KD/VFF/VFFUtil.h"
#include "Core/IOS/Network/Socket.h"
#include "Core/IOS/Uids.h"
#include "Core/System.h"
#include "Core/WC24PatchEngine.h"

namespace IOS::HLE
{
namespace
{
enum class HardwareModel : u8
{
  RVT = 0,
  RVV = 0,
  RVL = 1,
  RVD = 2,
  Unknown = 7
};

u8 GetAreaCode(const char* area)
{
  static const struct
  {
    const char* name;
    u8 code;
  } regions[] = {
      {"JPN", 0}, {"USA", 1}, {"EUR", 2}, {"AUS", 2}, {"BRA", 1}, {"TWN", 3}, {"ROC", 3},
      {"KOR", 4}, {"HKG", 5}, {"ASI", 5}, {"LTN", 1}, {"SAF", 2}, {"CHN", 6},
  };

  for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
  {
    if (!std::strcmp(area, regions[i].name))
      return regions[i].code;
  }

  return 7;  // Unknown
}

HardwareModel GetHardwareModel(const char* model)
{
  static const struct
  {
    const char* name;
    HardwareModel model;
  } models[] = {
      {"RVL", HardwareModel::RVL},
      {"RVT", HardwareModel::RVT},
      {"RVV", HardwareModel::RVV},
      {"RVD", HardwareModel::RVD},
  };

  for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++)
  {
    if (!std::strcmp(model, models[i].name))
      return models[i].model;
  }

  return HardwareModel::Unknown;
}

s32 NWC24MakeUserID(u64* nwc24_id, u32 hollywood_id, u16 id_ctr, HardwareModel hardware_model,
                    u8 area_code)
{
  static constexpr u8 table2[8]{
      0x1, 0x5, 0x0, 0x4, 0x2, 0x3, 0x6, 0x7,
  };
  static constexpr u8 table1[16]{
      0x4, 0xB, 0x7, 0x9, 0xF, 0x1, 0xD, 0x3, 0xC, 0x2, 0x6, 0xE, 0x8, 0x0, 0xA, 0x5,
  };

  constexpr auto u64_get_byte = [](u64 value, u32 shift) -> u8 { return u8(value >> (shift * 8)); };

  constexpr auto u64_insert_byte = [](u64 value, u32 shift, u8 byte) -> u64 {
    const u64 mask = 0x00000000000000FFULL << (shift * 8);
    const u64 inst = u64{byte} << (shift * 8);
    return (value & ~mask) | inst;
  };

  u64 mix_id = (u64{area_code} << 50) | (u64(hardware_model) << 47) | (u64{hollywood_id} << 15) |
               (u64{id_ctr} << 10);
  const u64 mix_id_copy1 = mix_id;

  u32 ctr = 0;
  for (ctr = 0; ctr <= 42; ctr++)
  {
    u64 value = mix_id >> (52 - ctr);
    if ((value & 1) != 0)
    {
      value = 0x0000000000000635ULL << (42 - ctr);
      mix_id ^= value;
    }
  }

  mix_id = (mix_id_copy1 | (mix_id & 0xFFFFFFFFUL)) ^ 0x0000B3B3B3B3B3B3ULL;
  mix_id = (mix_id >> 10) | ((mix_id & 0x3FF) << (11 + 32));

  for (ctr = 0; ctr <= 5; ctr++)
  {
    const u8 ret = u64_get_byte(mix_id, ctr);
    const u8 foobar = u8((u32{table1[(ret >> 4) & 0xF]} << 4) | table1[ret & 0xF]);
    mix_id = u64_insert_byte(mix_id, ctr, foobar & 0xff);
  }

  const u64 mix_id_copy2 = mix_id;

  for (ctr = 0; ctr <= 5; ctr++)
  {
    const u8 ret = u64_get_byte(mix_id_copy2, ctr);
    mix_id = u64_insert_byte(mix_id, table2[ctr], ret);
  }

  mix_id &= 0x001FFFFFFFFFFFFFULL;
  mix_id = (mix_id << 1) | ((mix_id >> 52) & 1);

  mix_id ^= 0x00005E5E5E5E5E5EULL;
  mix_id &= 0x001FFFFFFFFFFFFFULL;

  *nwc24_id = mix_id;

  if (mix_id > 9999999999999999ULL)
    return NWC24::WC24_ERR_FATAL;

  return NWC24::WC24_OK;
}

// AES-128-OFB, which libretro-common has no mode for: the key stream is the iv encrypted again and
// again, and iv is left at its last block. Encrypting and decrypting are the same.
void CryptOFB(const u8* key, u8* iv, const u8* in, u8* out, size_t size)
{
  struct aes_ctx ctx;
  size_t i;
  aes_init(&ctx, key, 16);
  for (i = 0; i < size; i++)
  {
    if (!(i & 15))
      aes_encrypt_block(&ctx, iv, iv);
    out[i] = in[i] ^ iv[i & 15];
  }
}
}  // Anonymous namespace

NetKDRequestDevice::NetKDRequestDevice(EmulationKernel& ios, const char* device_name,
                                       NetKDTimeDevice* time_device)
    : EmulationDevice(ios, device_name), m_config{ios.GetFS()}, m_dl_list{ios.GetFS()},
      m_send_list{ios.GetFS()}, m_friend_list{ios.GetFS()}, m_time_device{time_device}
{
#if defined(SKIP_WICONNECT24_WORKER_THREAD)
  return;
#endif
  // Enable all NWC24 permissions
  StoreSchedulerWord(1, Common::swap32(-1));

  m_work_queue.Reset("WiiConnect24 Worker", [this](const AsyncTask& task) {
    const IPCReply reply = (this->*task.handler)(task.request);
    auto* async_reply = new AsyncReply{{}, task.request, reply.return_value};
    mpsc_stack_push(&m_async_replies, &async_reply->link);
  });

  m_handle_mail = !ios.GetIOSC().IsUsingDefaultId() && !m_send_list.IsDisabled();
  m_scheduler_work_queue.Reset("WiiConnect24 Scheduler Worker");

  m_scheduler_timer_thread =
      sthread_create(Common::ThreadEntry<&NetKDRequestDevice::SchedulerTimer>, this);
}

NetKDRequestDevice::~NetKDRequestDevice()
{
  auto socket_manager = GetEmulationKernel().GetSocketManager();
  if (socket_manager)
    socket_manager->Clean();

  if (!m_scheduler_timer_thread)
    return;

  m_scheduler_running.Clear();
  frame_clock_wake_all();

  Common::JoinThread(m_scheduler_timer_thread);
  m_scheduler_work_queue.Shutdown();
  m_work_queue.Shutdown();

  for (mpsc_stack_node_t* node = mpsc_stack_drain(&m_async_replies); node;)
  {
    auto* reply = reinterpret_cast<AsyncReply*>(node);
    node = node->next;
    delete reply;
  }
}

void NetKDRequestDevice::Update()
{
  // Oldest first, as the workers finished them.
  for (mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&m_async_replies)); node;)
  {
    auto* reply = reinterpret_cast<AsyncReply*>(node);
    node = node->next;
    GetEmulationKernel().EnqueueIPCReply(reply->request, reply->return_value);
    delete reply;
  }
}

u32 NetKDRequestDevice::LoadSchedulerWord(size_t index) const
{
  return static_cast<u32>(
      retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t*>(&m_scheduler_buffer[index])));
}

void NetKDRequestDevice::StoreSchedulerWord(size_t index, u32 swapped)
{
  retro_atomic_store_release_int(&m_scheduler_buffer[index], static_cast<int>(swapped));
}

void NetKDRequestDevice::IncrementSchedulerWord(size_t index)
{
  int cur = retro_atomic_load_acquire_int(&m_scheduler_buffer[index]);
  while (!retro_atomic_cas_int(
      &m_scheduler_buffer[index], cur,
      static_cast<int>(Common::swap32(Common::swap32(static_cast<u32>(cur)) + 1))))
  {
    cur = retro_atomic_load_acquire_int(&m_scheduler_buffer[index]);
  }
}

void NetKDRequestDevice::SchedulerTimer()
{
#if defined(SKIP_KDSCHEDULER_TIMER_THREAD)
  return;
#endif
  u32 mail_time_state = 0;
  u32 download_time_state = 0;
  Common::SetCurrentThreadName("KD Scheduler Timer");
  while (true)
  {
    {
      if (static_cast<u32>(retro_atomic_load_acquire_int(&m_mail_span)) <= mail_time_state &&
          m_handle_mail)
      {
        m_scheduler_work_queue.Push([this] { SchedulerWorker(SchedulerEvent::Mail); });
        INFO_LOG(IOS_WC24, "NET_KD_REQ: Dispatching Mail Task from Scheduler");
        mail_time_state = 0;
      }

      // The worker checks whether downloads are disabled; the list is its.
      if (static_cast<u32>(retro_atomic_load_acquire_int(&m_download_span)) <=
          download_time_state)
      {
        INFO_LOG(IOS_WC24, "NET_KD_REQ: Dispatching Download Task from Scheduler");
        m_scheduler_work_queue.Push([this] { SchedulerWorker(SchedulerEvent::Download); });
        download_time_state = 0;
      }
    }

    // A minute of frontend frames; the scheduler's spans are in minutes.
    if (!frame_clock_wait_until(frame_clock_now() + FRAMES_PER_MINUTE,
                                m_scheduler_running.Raw()))
      return;

    mail_time_state++;
    download_time_state++;
  }
}

void NetKDRequestDevice::SchedulerWorker(const SchedulerEvent event)
{
  if (event == SchedulerEvent::Download)
  {
    u16 entry_index{};
    s32 subtask_id = NWC24::NO_SUBTASK;
    NWC24::ErrorCode code = DetermineDownloadTask(&entry_index, &subtask_id);
    if (code != NWC24::WC24_OK)
    {
      if (code == NWC24::WC24_ERR_NOT_FOUND)
        retro_atomic_store_release_int(&m_download_span, DEFAULT_SCHEDULER_SPAN_MINUTES);

      LogError(ErrorType::KD_Download, code);
      return;
    }

    code = KDDownload(entry_index, subtask_id);
    if (code != NWC24::WC24_OK)
    {
      LogError(ErrorType::KD_Download, code);
    }
  }
  else
  {
    if (!m_config.IsRegistered())
      return;

    u32 mail_flag{};
    u32 interval{};

    NWC24::ErrorCode code = KDCheckMail(&mail_flag, &interval);
    if (code != NWC24::WC24_OK)
    {
      LogError(ErrorType::CheckMail, code);
    }

    code = KDSendMail();
    if (code != NWC24::WC24_OK)
    {
      LogError(ErrorType::SendMail, code);
    }
  }
}

// Large enough for every value KD reads from a CGI reply, terminator included.
constexpr size_t CGI_VALUE_SIZE = 256;

static bool IsCGIWhitespace(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool NetKDRequestDevice::GetValueFromCGIResponse(const u8* response, size_t response_size,
                                                 const char* key, char* out, size_t out_size)
{
  const char* const text = reinterpret_cast<const char*>(response);
  const size_t key_size = std::strlen(key);
  size_t start = 0;
  out[0] = '\0';
  while (start < response_size)
  {
    const char* const field = text + start;
    const char* const line_end =
        static_cast<const char*>(std::memchr(field, '\n', response_size - start));
    const size_t field_size =
        line_end ? static_cast<size_t>(line_end - field) : response_size - start;
    const char* const separator = static_cast<const char*>(std::memchr(field, '=', field_size));
    start += field_size + 1;
    if (!separator)
      continue;

    // A field holds a key and a value: "key=value", or "key=value=" with the last '=' dropped.
    const size_t key_end = static_cast<size_t>(separator - field);
    const char* const second =
        static_cast<const char*>(std::memchr(separator + 1, '=', field_size - key_end - 1));
    size_t value_end = field_size;
    if (second)
    {
      const size_t second_end = static_cast<size_t>(second - field);
      if (second_end != field_size - 1)
        continue;
      value_end = second_end;
    }
    else if (key_end + 1 == field_size)
    {
      continue;
    }

    if (key_end != key_size || std::memcmp(field, key, key_size) != 0)
      continue;

    size_t value_start = key_end + 1;
    while (value_start < value_end && IsCGIWhitespace(field[value_start]))
      value_start++;
    while (value_end > value_start && IsCGIWhitespace(field[value_end - 1]))
      value_end--;
    if (value_end - value_start >= out_size)
      return false;
    std::memcpy(out, field + value_start, value_end - value_start);
    out[value_end - value_start] = '\0';
    return true;
  }

  return false;
}

void NetKDRequestDevice::LogError(ErrorType error_type, s32 error_code)
{
  s32 new_code{};
  switch (error_type)
  {
  case ErrorType::Account:
    new_code = -(101200 - error_code);
    break;
  case ErrorType::Client:
    new_code = -(107300 - error_code);
    break;
  case ErrorType::KD_Download:
    new_code = -(107200 - error_code);
    break;
  case ErrorType::Server:
    new_code = -(117000 + error_code);
    break;
  case ErrorType::CheckMail:
    new_code = -(102200 - error_code);
    break;
  case ErrorType::SendMail:
    new_code = -(105000 - error_code);
    break;
  case ErrorType::ReceiveMail:
    new_code = -(100300 - error_code);
    break;
  case ErrorType::CGI:
    new_code = -(error_code + 110000);
    break;
  }

  const u32 error_index = static_cast<u32>(retro_atomic_fetch_add_int(&m_error_count, 1));
  StoreSchedulerWord(32 + (error_index % 32), Common::swap32(new_code));
  StoreSchedulerWord(5, Common::swap32(error_index + 1));
  StoreSchedulerWord(2, Common::swap32(new_code));
}

NWC24::ErrorCode NetKDRequestDevice::KDCheckMail(u32* mail_flag, u32* interval)
{
  bool success = false;
  Common::ScopeGuard state_guard([&] {
    if (success)
    {
      // m_scheduler_buffer[11] contains the amount of times we have checked for mail this IOS
      // session.
      IncrementSchedulerWord(11);
    }
    StoreSchedulerWord(4, static_cast<u32>(CurrentFunction::None));
  });

  StoreSchedulerWord(4, Common::swap32(static_cast<u32>(CurrentFunction::Check)));

  u64 random_number{};
  Common::Random::Generate(&random_number, sizeof(u64));
  char mlchkid[NWC24::NWC24Config::MLCHKID_SIZE];
  char check_url[NWC24::NWC24Config::URL_SIZE];
  char form_data[128];
  m_config.GetMlchkid(mlchkid, sizeof(mlchkid));
  m_config.GetCheckURL(check_url, sizeof(check_url));
  const int form_size = std::snprintf(form_data, sizeof(form_data), "mlchkid=%s&chlng=%" PRIu64,
                                      mlchkid, (u64)random_number);
  if (!m_http.Post(check_url, form_data, static_cast<size_t>(form_size)))
  {
    ERROR_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_CHECK_MAIL_NOW: Failed to request data at %s.",
              check_url);
    return NWC24::WC24_ERR_SERVER;
  }

  const u8* const response = m_http.GetBody();
  const size_t response_size = m_http.GetBodySize();
  char code[CGI_VALUE_SIZE];
  GetValueFromCGIResponse(response, response_size, "cd", code, sizeof(code));
  if (std::strcmp(code, "100"))
  {
    ERROR_LOG(IOS_WC24,
              "NET_KD_REQ: IOCTL_NWC24_CHECK_MAIL_NOW: Mail server returned non-success code: %s",
              code);
    return NWC24::WC24_ERR_SERVER;
  }

  char server_hmac[CGI_VALUE_SIZE];
  char str_mail_flag[CGI_VALUE_SIZE];
  char str_interval[CGI_VALUE_SIZE];
  GetValueFromCGIResponse(response, response_size, "res", server_hmac, sizeof(server_hmac));
  GetValueFromCGIResponse(response, response_size, "mail.flag", str_mail_flag,
                          sizeof(str_mail_flag));
  GetValueFromCGIResponse(response, response_size, "interval", str_interval, sizeof(str_interval));
  DEBUG_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_CHECK_MAIL_NOW: Server HMAC: %s", server_hmac);

  // On a real Wii, a response to a challenge is expected and would be verified by KD.
  const std::string hmac_message =
      StringFromFormat("%" PRIu64 "\nw%016" PRIu64 "\n%s\n%s", (u64)random_number,
                       (u64)m_config.Id(), str_mail_flag, str_interval);
  u8 hashed[20]{};
  hmac_sha1(MAIL_CHECK_KEY, ARRAY_SIZE(MAIL_CHECK_KEY),
            reinterpret_cast<const u8*>(hmac_message.data()), hmac_message.size(), hashed);

  // On a real Wii, strncmp is used to compare both hashes. This means that it is a case-sensitive
  // comparison. KD will generate a lowercase hash as well as expect a lowercase hash from the
  // server.
  char hashed_hex[sizeof(hashed) * 2 + 1];
  for (size_t i = 0; i < sizeof(hashed); i++)
    std::snprintf(hashed_hex + i * 2, 3, "%02x", hashed[i]);
  if (std::strcmp(hashed_hex, server_hmac))
  {
    ERROR_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_CHECK_MAIL_NOW: Server HMAC is invalid.");
    return NWC24::WC24_ERR_SERVER;
  }

  *mail_flag = std::strncmp(str_mail_flag, m_send_list.GetMailFlag(), 22) != 0;

  {
    const char* const mail_span = m_http.GetHeaderValue("X-Wii-Mail-Check-Span");
    bool did_parse = mail_span && TryParse(mail_span, interval);
    if (did_parse)
    {
      if (*interval == 0)
      {
        *interval = 1;
      }

      retro_atomic_store_release_int(&m_mail_span, static_cast<int>(*interval));
    }

    u32 download_span = 0;
    const char* const download_span_value = m_http.GetHeaderValue("X-Wii-Download-Span");
    did_parse = download_span_value && TryParse(download_span_value, &download_span);
    if (did_parse)
      retro_atomic_store_release_int(&m_download_span, static_cast<int>(std::max(download_span, 1u)));
  }

  success = true;
  return NWC24::WC24_OK;
}

NWC24::ErrorCode NetKDRequestDevice::DetermineDownloadTask(u16* entry_index, s32* subtask_id) const
{
  // As the scheduler does not tell us which entry to download, we must determine that.
  // A correct entry is one that hasn't been downloaded the longest compared to other entries.
  // We first need current UTC.
  const u64 current_utc = m_time_device->GetAdjustedUTC();
  u64 lowest_timestamp = std::numeric_limits<u64>::max();

  for (u16 i = 0; i < static_cast<u16>(NWC24::NWC24Dl::MAX_ENTRIES); i++)
  {
    if (!m_dl_list.DoesEntryExist(i))
      continue;

    if (m_dl_list.SkipSchedulerDownload(i))
      continue;

    const u64 next_dl_time = m_dl_list.GetNextDownloadTime(i);

    // First determine if UTC is greater than the next download time.
    if (current_utc < next_dl_time)
      continue;

    // If this task's next download time is less than the lowest_timestamp, this is the task we
    // want. However, we must determine if this has a subtask and wants to be downloaded.
    if (next_dl_time < lowest_timestamp)
    {
      if (m_dl_list.HasSubtask(i))
      {
        NWC24::ErrorCode code = DetermineSubtask(i, subtask_id);
        if (code != NWC24::WC24_OK)
        {
          // No valid subtask found or downloading is disabled.
          continue;
        }
      }

      lowest_timestamp = next_dl_time;
      *entry_index = i;
    }
  }

  // Determine if we actually found an entry to download.
  if (lowest_timestamp == std::numeric_limits<u64>::max())
    return NWC24::WC24_ERR_NOT_FOUND;

  return NWC24::WC24_OK;
}

NWC24::ErrorCode NetKDRequestDevice::DetermineSubtask(u16 entry_index, s32* subtask_id) const
{
  // Before we do anything, determine if this task wants to be downloaded
  if (m_dl_list.IsSubtaskDownloadDisabled(entry_index))
    return NWC24::WC24_ERR_DISABLED;

  const u64 current_utc = m_time_device->GetAdjustedUTC();
  for (u8 i = 0; i < 32; i++)
  {
    if (!m_dl_list.IsValidSubtask(entry_index, i))
      continue;

    // Unlike DetermineDownloadTask, DetermineSubtask gets the first download time lower than UTC.
    const u64 last_download_time = m_dl_list.GetLastSubtaskDownloadTime(entry_index, i);
    if (last_download_time < current_utc)
    {
      *subtask_id = i;
      return NWC24::WC24_OK;
    }
  }

  return NWC24::WC24_ERR_INVALID_VALUE;
}

// A mail sent as one part of the multipart form
struct MailPart
{
  char name[16];
  u8* data;
};

NWC24::ErrorCode NetKDRequestDevice::KDSendMail()
{
  bool success = false;
  Common::ScopeGuard exit_guard([&] {
    if (success)
    {
      // m_scheduler_buffer[11] contains the amount of times we have sent for mail this IOS
      // session.
      IncrementSchedulerWord(14);
    }
    StoreSchedulerWord(4, static_cast<u32>(CurrentFunction::None));

    m_send_list.WriteSendList();
  });

  StoreSchedulerWord(4, Common::swap32(static_cast<u32>(CurrentFunction::Send)));

  m_send_list.ReadSendList();
  char password[NWC24::NWC24Config::PASSWORD_SIZE];
  char auth[96];
  m_config.GetPassword(password, sizeof(password));
  const int auth_size = std::snprintf(auth, sizeof(auth), "mlid=w%016" PRIu64 "\r\npasswd=%s",
                                      (u64)m_config.Id(), password);
  constexpr u32 MAX_MAILS = NWC24::Mail::WC24SendList::MAX_MAILS_PER_SEND;
  u32 mails[MAX_MAILS];
  const u32 mail_candidates = m_send_list.GetMailToSend(mails);
  u32 mail_count = 0;
  // One part for the login, then one per mail; the mails' names and contents are kept until sent.
  Common::HttpRequest::Multiform multiform[1 + MAX_MAILS];
  MailPart parts[MAX_MAILS];
  multiform[0] = {"mlid", auth, static_cast<size_t>(auth_size)};
  Common::ScopeGuard parts_guard([&] {
    for (u32 part = 0; part < mail_count; part++)
      std::free(parts[part].data);
  });

  // Mails that cannot be sent are deleted and left out, keeping the others in order.
  for (u32 candidate = 0; candidate < mail_candidates; candidate++)
  {
    const u32 file_index = mails[candidate];
    const u32 entry_id = m_send_list.GetEntryId(file_index);
    const u32 mail_size = m_send_list.GetMailSize(file_index);
    if (mail_size > MAX_MAIL_SIZE)
    {
      WARN_LOG(IOS_WC24,
               "NET_KD_REQ: IOCTL_NWC24_SEND_MAIL_NOW: Mail at index %u was too large to send.",
               entry_id);
      LogError(ErrorType::SendMail, NWC24::WC24_MSG_TOO_BIG);

      NWC24::ErrorCode res = m_send_list.DeleteMessage(file_index);
      if (res != NWC24::WC24_OK)
      {
        LogError(ErrorType::SendMail, res);
      }
      continue;
    }

    MailPart& part = parts[mail_count];
    char mail_path[NWC24::Mail::WC24SendList::MAIL_PATH_SIZE];
    m_send_list.GetMailPath(file_index, mail_path, sizeof(mail_path));
    part.data = static_cast<u8*>(std::malloc(mail_size ? mail_size : 1));
    NWC24::ErrorCode res = part.data ? NWC24::ReadFromVFF(NWC24::Mail::SEND_BOX_PATH, mail_path,
                                                          m_ios.GetFS(), part.data, mail_size) :
                                       NWC24::WC24_ERR_FILE_READ;
    if (res != NWC24::WC24_OK)
    {
      ERROR_LOG(IOS_WC24, "Reading mail at index %u failed with error code %d.", entry_id,
                static_cast<s32>(res));
      LogError(ErrorType::SendMail, NWC24::WC24_MSG_DAMAGED);
      std::free(part.data);
      part.data = nullptr;
      res = m_send_list.DeleteMessage(file_index);
      if (res != NWC24::WC24_OK)
      {
        LogError(ErrorType::SendMail, res);
      }
      continue;
    }

    std::snprintf(part.name, sizeof(part.name), "m%u", entry_id);
    multiform[1 + mail_count] = {part.name, part.data, mail_size};
    mails[mail_count++] = file_index;
  }

  char send_url[NWC24::NWC24Config::URL_SIZE];
  m_config.GetSendURL(send_url, sizeof(send_url));
  if (!m_http.PostMultiform(send_url, multiform, 1 + mail_count))
  {
    ERROR_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_SEND_MAIL_NOW: Failed to request data at %s.",
              send_url);
    return NWC24::WC24_ERR_SERVER;
  }

  // Now check if any mail failed to save to the server.
  const u8* const response = m_http.GetBody();
  const size_t response_size = m_http.GetBodySize();
  char code[CGI_VALUE_SIZE];
  GetValueFromCGIResponse(response, response_size, "cd", code, sizeof(code));
  if (std::strcmp(code, "100"))
  {
    ERROR_LOG(IOS_WC24,
              "NET_KD_REQ: IOCTL_NWC24_CHECK_MAIL_NOW: Mail server returned non-success code: %s",
              code);
    return NWC24::WC24_ERR_SERVER;
  }

  // Reverse in order to delete from bottom to top of the send list.
  // We do this to ensure that new entries can be written as close to the beginning of the file as
  // possible.
  for (u32 mail = mail_count; mail-- > 0;)
  {
    const u32 file_index = mails[mail];
    const u32 entry_id = m_send_list.GetEntryId(file_index);
    Common::ScopeGuard delete_guard([&] {
      NWC24::ErrorCode res = m_send_list.DeleteMessage(file_index);
      if (res != NWC24::WC24_OK)
      {
        LogError(ErrorType::SendMail, res);
      }
    });

    char key[16];
    char value[CGI_VALUE_SIZE];
    std::snprintf(key, sizeof(key), "cd%u", entry_id);
    GetValueFromCGIResponse(response, response_size, key, value, sizeof(value));

    s32 cgi_code{};
    const bool did_parse = TryParse(value, &cgi_code);
    if (!did_parse)
    {
      ERROR_LOG(IOS_WC24, "Mail server returned invalid CGI response code.");
      LogError(ErrorType::CGI, NWC24::WC24_ERR_SERVER);
      break;
    }

    if (cgi_code != 100)
    {
      ERROR_LOG(IOS_WC24, "Mail server failed to save mail at index %u", entry_id);
      LogError(ErrorType::CGI, cgi_code);
    }
  }

  success = true;
  return NWC24::WC24_OK;
}

NWC24::ErrorCode NetKDRequestDevice::KDDownload(const u16 entry_index, const s32 subtask_id)
{
  bool success = false;
  Common::ScopeGuard state_guard([&] {
    const u64 current_utc = m_time_device->GetAdjustedUTC();
    if (success)
    {
      // Set the next download time to the dl_margin
      m_dl_list.SetNextDownloadTime(
          entry_index, current_utc + m_dl_list.GetDownloadMargin(entry_index), subtask_id);
    }
    else
    {
      // Else set it to the retry margin
      m_dl_list.SetNextDownloadTime(entry_index, current_utc + m_dl_list.GetRetryTime(entry_index),
                                    subtask_id);
    }

    // Finally flush
    m_dl_list.WriteDlList();
  });

  // Content metadata
  char content_name[NWC24::NWC24Dl::VFF_CONTENT_NAME_SIZE];
  char download_url[NWC24::NWC24Dl::DOWNLOAD_URL_SIZE];
  m_dl_list.GetVFFContentName(entry_index, subtask_id, content_name, sizeof(content_name));
  m_dl_list.GetDownloadURL(entry_index, subtask_id, download_url, sizeof(download_url));

  if (!content_name[0])
  {
    // If a content has an empty name it is meant to be saved to the mailbox. We do not support
    // saving mail to the mailbox yet and as a result must skip these entries.
    success = true;
    return NWC24::WC24_OK;
  }

  // The host is what follows the second '/' ("scheme://host/..."), up to the next one.
  const char* const first_separator = std::strchr(download_url, '/');
  const char* const second_separator =
      first_separator ? std::strchr(first_separator + 1, '/') : nullptr;
  if (!second_separator || !second_separator[1])
  {
    // Invalid URL
    LogError(ErrorType::KD_Download, NWC24::WC24_ERR_SERVER);
    return NWC24::WC24_ERR_SERVER;
  }
  const char* const host = second_separator + 1;
  const char* const host_end = std::strchr(host, '/');
  const size_t host_size = host_end ? static_cast<size_t>(host_end - host) : std::strlen(host);

  // Reroute to custom server if enabled.
  char url[512];
  if (const char* const patch = WC24PatchEngine::GetNetworkPatch(host, host_size, true))
  {
    std::snprintf(url, sizeof(url), "%.*s%s%s", static_cast<int>(host - download_url), download_url,
                  patch, host + host_size);
  }
  else
  {
    std::snprintf(url, sizeof(url), "%s", download_url);
  }

  INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_DOWNLOAD_NOW_EX - NI - URL: %s", url);

  INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_DOWNLOAD_NOW_EX - NI - Name: %s", content_name);

  if (!m_http.Get(url))
  {
    const s32 last_response_code = m_http.GetLastResponseCode();
    ERROR_LOG(IOS_WC24, "Failed to request data at %s. HTTP Status Code: %d", url,
              last_response_code);

    // On a real Wii, KD throws 107305 if it cannot connect to the host. While other issues other
    // than invalid host may arise, this code is essentially a catch-all for HTTP client failure.
    LogError(last_response_code ? ErrorType::Server : ErrorType::Client,
             last_response_code ? last_response_code : NWC24::WC24_ERR_NULL);
    return NWC24::WC24_ERR_SERVER;
  }

  const u8* const response = m_http.GetBody();
  const size_t response_size = m_http.GetBodySize();
  const u8* file_data;
  size_t file_size;
  u8* decrypted = nullptr;
  if (!m_dl_list.IsRSASigned(entry_index))
  {
    // Data that is not signed with an RSA key will not have the WC24 header or 320 bytes before the
    // actual data. We just have to make sure that the response is not empty.
    if (!response_size)
    {
      ERROR_LOG(IOS_WC24, "File at %s is empty.", url);
      LogError(ErrorType::KD_Download, NWC24::WC24_ERR_BROKEN);
      return NWC24::WC24_ERR_BROKEN;
    }

    file_data = response;
    file_size = response_size;
  }
  else
  {
    // Check if the filesize is smaller than the header size.
    if (response_size < sizeof(NWC24::WC24File))
    {
      ERROR_LOG(IOS_WC24, "File at %s is too small to be a valid file.", url);
      LogError(ErrorType::KD_Download, NWC24::WC24_ERR_BROKEN);
      return NWC24::WC24_ERR_BROKEN;
    }

    // Now we read the file
    NWC24::WC24File wc24_file;
    std::memcpy(&wc24_file, response, sizeof(NWC24::WC24File));
    file_data = response + sizeof(NWC24::WC24File);
    file_size = response_size - sizeof(NWC24::WC24File);

    if (m_dl_list.IsEncrypted(entry_index))
    {
      NWC24::WC24PubkMod pubk_mod;
      if (!m_dl_list.GetWC24PubkMod(entry_index, &pubk_mod))
      {
        ERROR_LOG(IOS_WC24, "Failed to get wc24pubk.mod for the current task.");
        LogError(ErrorType::KD_Download, NWC24::WC24_ERR_FILE_OPEN);
        return NWC24::WC24_ERR_FILE_OPEN;
      }

      decrypted = static_cast<u8*>(std::malloc(file_size ? file_size : 1));
      if (!decrypted)
      {
        LogError(ErrorType::KD_Download, NWC24::WC24_ERR_BROKEN);
        return NWC24::WC24_ERR_BROKEN;
      }
      CryptOFB(pubk_mod.aes_key, wc24_file.iv, file_data, decrypted, file_size);
      file_data = decrypted;
    }
  }

  char vff_path[64];
  m_dl_list.GetVFFPath(entry_index, vff_path, sizeof(vff_path));
  NWC24::ErrorCode reply =
      IOS::HLE::NWC24::WriteToVFF(vff_path, content_name, m_ios.GetFS(), file_data, file_size);
  std::free(decrypted);

  if (reply != NWC24::WC24_OK)
  {
    LogError(ErrorType::KD_Download, reply);
    return reply;
  }

  success = true;
  return reply;
}

IPCReply NetKDRequestDevice::HandleNWC24CheckMailNow(const IOCtlRequest& request)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  if (!m_handle_mail)
  {
    LogError(ErrorType::CheckMail, NWC24::WC24_ERR_BROKEN);
    WriteReturnValue(memory, NWC24::WC24_ERR_BROKEN, request.buffer_out);
    return IPCReply(IPC_SUCCESS);
  }

  u32 mail_flag{};
  u32 interval{};
  const NWC24::ErrorCode reply = KDCheckMail(&mail_flag, &interval);

  WriteReturnValue(memory, reply, request.buffer_out);
  memory.Write_U32(mail_flag, request.buffer_out + 4);
  memory.Write_U32(interval, request.buffer_out + 8);
  return IPCReply(IPC_SUCCESS);
}

IPCReply NetKDRequestDevice::HandleNWC24SendMailNow(const IOCtlRequest& request)
{
  auto& memory = GetSystem().GetMemory();

  const NWC24::ErrorCode reply = KDSendMail();
  WriteReturnValue(memory, reply, request.buffer_out);
  return IPCReply(IPC_SUCCESS);
}

IPCReply NetKDRequestDevice::HandleNWC24DownloadNowEx(const IOCtlRequest& request)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  if (m_dl_list.IsDisabled() || !m_dl_list.ReadDlList())
  {
    // Signal that the DL List is broken.
    LogError(ErrorType::KD_Download, NWC24::WC24_ERR_BROKEN);
    WriteReturnValue(memory, NWC24::WC24_ERR_BROKEN, request.buffer_out);
    return IPCReply(IPC_SUCCESS);
  }

  const u32 flags = memory.Read_U32(request.buffer_in);
  // Nintendo converts the entry ID between a u32 and u16
  // several times, presumably for alignment purposes.
  // We'll skip past buffer_in+4 and keep the entry index as a u16.
  const u16 entry_index = memory.Read_U16(request.buffer_in + 6);
  const u32 subtask_bitmask = memory.Read_U32(request.buffer_in + 8);

  INFO_LOG(IOS_WC24,
           "NET_KD_REQ: IOCTL_NWC24_DOWNLOAD_NOW_EX - NI - flags: %u, index: %u, bitmask: %u",
           flags, (unsigned)entry_index, subtask_bitmask);

  if (entry_index >= NWC24::NWC24Dl::MAX_ENTRIES)
  {
    ERROR_LOG(IOS_WC24, "NET_KD_REQ: Entry index out of range.");
    LogError(ErrorType::KD_Download, NWC24::WC24_ERR_INVALID_VALUE);
    WriteReturnValue(memory, NWC24::WC24_ERR_INVALID_VALUE, request.buffer_out);
    return IPCReply(IPC_SUCCESS);
  }

  if (!m_dl_list.DoesEntryExist(entry_index))
  {
    ERROR_LOG(IOS_WC24, "NET_KD_REQ: Requested entry does not exist in download list!");
    LogError(ErrorType::KD_Download, NWC24::WC24_ERR_NOT_FOUND);
    WriteReturnValue(memory, NWC24::WC24_ERR_NOT_FOUND, request.buffer_out);
    return IPCReply(IPC_SUCCESS);
  }

  // While in theory reply will always get initialized by KDDownload, things happen.
  // Returning NWC24::WC24_ERR_BROKEN or anything that isn't OK will prompt the channel to fix the
  // entry's data.
  NWC24::ErrorCode reply = NWC24::WC24_ERR_BROKEN;

  // Determine if we have subtasks to handle
  if (Common::ExtractBit(flags, 2))
  {
    for (u8 subtask_id = 0; subtask_id < 32; subtask_id++)
    {
      if (Common::ExtractBit(subtask_bitmask, subtask_id))
      {
        reply = KDDownload(entry_index, subtask_id);
        if (reply != NWC24::WC24_OK)
        {
          // An error has occurred, break out and return error.
          break;
        }
      }
    }
  }
  else
  {
    reply = KDDownload(entry_index, NWC24::NO_SUBTASK);
  }

  WriteReturnValue(memory, reply, request.buffer_out);
  return IPCReply(IPC_SUCCESS);
}

IPCReply NetKDRequestDevice::HandleRequestRegisterUserId(const IOS::HLE::IOCtlRequest& request)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_REQUEST_REGISTER_USER_ID");
  // Always 0 for some reason, never modified anywhere else
  memory.Write_U32(0, request.buffer_out + 4);

  if (m_ios.GetIOSC().IsUsingDefaultId())
  {
    // If the user is using the default console ID, the below will always throw an error if it needs
    // to be registered. Due to the high likelihood of multiple users having the same Wii Number,
    // Nintendo's register endpoint will most likely return a duplicate registration error.
    m_config.SetCreationStage(NWC24::NWC24CreationStage::Registered);
    m_config.SetChecksum(m_config.CalculateNwc24ConfigChecksum());
    m_config.WriteConfig();
    m_config.WriteCBK();

    WriteReturnValue(memory, NWC24::WC24_OK, request.buffer_out);
    return IPCReply{IPC_SUCCESS};
  }

  // First check if the message config file is in the correct state to handle this.
  if (m_config.IsRegistered())
  {
    WriteReturnValue(memory, NWC24::WC24_ERR_ID_REGISTERED, request.buffer_out);
    LogError(ErrorType::Account, NWC24::WC24_ERR_ID_REGISTERED);
    return IPCReply{IPC_SUCCESS};
  }

  if (!m_config.IsGenerated())
  {
    WriteReturnValue(memory, NWC24::WC24_ERR_ID_NOT_GENERATED, request.buffer_out);
    LogError(ErrorType::Account, NWC24::WC24_ERR_ID_NOT_GENERATED);
    return IPCReply{IPC_SUCCESS};
  }

  // We require the user's serial number.
  char settings_file_path[Common::NAND_PATH_SIZE];
  Common::GetTitleDataFilePath(Titles::SYSTEM_MENU, WII_SETTING, settings_file_path,
                               sizeof(settings_file_path));
  const auto file = m_ios.GetFS()->OpenFile(PID_KD, PID_KD, settings_file_path, FS::Mode::Read);
  if (!file)
  {
    WriteReturnValue(memory, NWC24::WC24_ERR_FILE_OPEN, request.buffer_out);
    LogError(ErrorType::Account, NWC24::WC24_ERR_FILE_OPEN);
    return IPCReply{IPC_SUCCESS};
  }

  settings_buffer_t data;
  if (!file.Read(data, sizeof(data)))
  {
    WriteReturnValue(memory, NWC24::WC24_ERR_FILE_READ, request.buffer_out);
    LogError(ErrorType::Account, NWC24::WC24_ERR_FILE_READ);
    return IPCReply{IPC_SUCCESS};
  }

  settings_reader_t settings_reader;
  char serno[SETTINGS_VALUE_SIZE];
  settings_reader_init(&settings_reader, data);
  settings_reader_get(&settings_reader, "SERNO", serno, sizeof(serno));
  char account_url[NWC24::NWC24Config::URL_SIZE];
  char form_data[256];
  m_config.GetAccountURL(account_url, sizeof(account_url));
  const int form_size =
      std::snprintf(form_data, sizeof(form_data), "mlid=w%016" PRIu64 "&hdid=%u&rgncd=%s",
                    (u64)m_config.Id(), m_ios.GetIOSC().GetDeviceId(), serno);
  if (form_size < 0 || static_cast<size_t>(form_size) >= sizeof(form_data) ||
      !m_http.Post(account_url, form_data, static_cast<size_t>(form_size)))
  {
    ERROR_LOG(IOS_WC24,
              "NET_KD_REQ: IOCTL_NWC24_REQUEST_REGISTER_USER_ID: Failed to request data at %s.",
              account_url);
    WriteReturnValue(memory, NWC24::WC24_ERR_SERVER, request.buffer_out);
    LogError(ErrorType::Account, NWC24::WC24_ERR_SERVER);
    return IPCReply{IPC_SUCCESS};
  }

  const u8* const response = m_http.GetBody();
  const size_t response_size = m_http.GetBodySize();
  char code[CGI_VALUE_SIZE];
  GetValueFromCGIResponse(response, response_size, "cd", code, sizeof(code));
  s32 cgi_code{};
  const bool did_parse = TryParse(code, &cgi_code);
  if (!did_parse)
  {
    ERROR_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_REQUEST_REGISTER_USER_ID: Mail server "
                        "returned invalid CGI response code.");
    LogError(ErrorType::Account, NWC24::WC24_ERR_SERVER);
  }

  if (cgi_code != 100)
  {
    ERROR_LOG(IOS_WC24,
              "NET_KD_REQ: IOCTL_NWC24_REQUEST_REGISTER_USER_ID: Mail server returned "
              "non-success code: %d",
              cgi_code);
    WriteReturnValue(memory, NWC24::WC24_ERR_SERVER, request.buffer_out);
    LogError(ErrorType::CGI, cgi_code);
    return IPCReply{IPC_SUCCESS};
  }

  char password[CGI_VALUE_SIZE];
  char mail_check_id[CGI_VALUE_SIZE];
  GetValueFromCGIResponse(response, response_size, "passwd", password, sizeof(password));
  GetValueFromCGIResponse(response, response_size, "mlchkid", mail_check_id, sizeof(mail_check_id));
  if (std::strlen(mail_check_id) != 32)
  {
    ERROR_LOG(IOS_WC24,
              "NET_KD_REQ: IOCTL_NWC24_REQUEST_REGISTER_USER_ID: Mail server returned invalid "
              "mlchkid: %s",
              mail_check_id);
    WriteReturnValue(memory, NWC24::WC24_ERR_SERVER, request.buffer_out);
    LogError(ErrorType::Account, NWC24::WC24_ERR_SERVER);
    return IPCReply{IPC_SUCCESS};
  }

  // Now write to the config.
  m_config.SetCreationStage(NWC24::NWC24CreationStage::Registered);
  m_config.SetPassword(password);
  m_config.SetMailCheckID(mail_check_id);
  m_config.SetChecksum(m_config.CalculateNwc24ConfigChecksum());
  m_config.WriteConfig();
  m_config.WriteCBK();

  WriteReturnValue(memory, NWC24::WC24_OK, request.buffer_out);

  return IPCReply{IPC_SUCCESS};
}

IPCReply NetKDRequestDevice::IOCtl(const IOCtlRequest& request)
{
  enum : u32
  {
    IOCTL_NWC24_SUSPEND_SCHEDULER = 0x01,
    IOCTL_NWC24_EXEC_TRY_SUSPEND_SCHEDULER = 0x02,
    IOCTL_NWC24_EXEC_RESUME_SCHEDULER = 0x03,
    IOCTL_NWC24_KD_GET_TIME_TRIGGERS = 0x04,
    IOCTL_NWC24_SET_SCHEDULE_SPAN = 0x05,
    IOCTL_NWC24_STARTUP_SOCKET = 0x06,
    IOCTL_NWC24_CLEANUP_SOCKET = 0x07,
    IOCTL_NWC24_LOCK_SOCKET = 0x08,
    IOCTL_NWC24_UNLOCK_SOCKET = 0x09,
    IOCTL_NWC24_CHECK_MAIL_NOW = 0x0A,
    IOCTL_NWC24_SEND_MAIL_NOW = 0x0B,
    IOCTL_NWC24_RECEIVE_MAIL_NOW = 0x0C,
    IOCTL_NWC24_SAVE_MAIL_NOW = 0x0D,
    IOCTL_NWC24_DOWNLOAD_NOW_EX = 0x0E,
    IOCTL_NWC24_REQUEST_GENERATED_USER_ID = 0x0F,
    IOCTL_NWC24_REQUEST_REGISTER_USER_ID = 0x10,
    IOCTL_NWC24_GET_SCHEDULER_STAT = 0x1E,
    IOCTL_NWC24_SET_FILTER_MODE = 0x1F,
    IOCTL_NWC24_SET_DEBUG_MODE = 0x20,
    IOCTL_NWC24_KD_SET_NEXT_WAKEUP = 0x21,
    IOCTL_NWC24_SET_SCRIPT_MODE = 0x22,
    IOCTL_NWC24_REQUEST_SHUTDOWN = 0x28,
  };

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  s32 return_value = 0;
  switch (request.request)
  {
  case IOCTL_NWC24_SUSPEND_SCHEDULER:
    // NWC24iResumeForCloseLib  from NWC24SuspendScheduler (Input: none, Output: 32 bytes)
    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_SUSPEND_SCHEDULER - NI");
    WriteReturnValue(memory, 0, request.buffer_out);  // no error
    break;

  case IOCTL_NWC24_EXEC_TRY_SUSPEND_SCHEDULER:  // NWC24iResumeForCloseLib
    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_EXEC_TRY_SUSPEND_SCHEDULER - NI");
    break;

  case IOCTL_NWC24_EXEC_RESUME_SCHEDULER:  // NWC24iResumeForCloseLib
    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_EXEC_RESUME_SCHEDULER - NI");
    WriteReturnValue(memory, 0, request.buffer_out);  // no error
    break;

  case IOCTL_NWC24_STARTUP_SOCKET:  // NWC24iStartupSocket
    WriteReturnValue(memory, 0, request.buffer_out);
    memory.Write_U32(0, request.buffer_out + 4);
    return_value = 0;
    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_STARTUP_SOCKET - NI");
    break;

  case IOCTL_NWC24_CLEANUP_SOCKET:
    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_CLEANUP_SOCKET");
    GetEmulationKernel().GetSocketManager()->Clean();
    break;

  case IOCTL_NWC24_LOCK_SOCKET:  // WiiMenu
    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_LOCK_SOCKET - NI");
    break;

  case IOCTL_NWC24_UNLOCK_SOCKET:
    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_UNLOCK_SOCKET - NI");
    break;

  case IOCTL_NWC24_REQUEST_REGISTER_USER_ID:
    return LaunchAsyncTask(&NetKDRequestDevice::HandleRequestRegisterUserId, request);

  case IOCTL_NWC24_REQUEST_GENERATED_USER_ID:  // (Input: none, Output: 32 bytes)
    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_REQUEST_GENERATED_USER_ID");
    if (m_config.IsCreated())
    {
      char settings_file_path[Common::NAND_PATH_SIZE];
      Common::GetTitleDataFilePath(Titles::SYSTEM_MENU, WII_SETTING, settings_file_path,
                                   sizeof(settings_file_path));
      char area[SETTINGS_VALUE_SIZE] = "";
      char model[SETTINGS_VALUE_SIZE] = "";

      const auto fs = m_ios.GetFS();
      if (const auto file = fs->OpenFile(PID_KD, PID_KD, settings_file_path, FS::Mode::Read))
      {
        settings_buffer_t data;
        if (file.Read(data, sizeof(data)))
        {
          settings_reader_t settings_reader;
          settings_reader_init(&settings_reader, data);
          settings_reader_get(&settings_reader, "AREA", area, sizeof(area));
          settings_reader_get(&settings_reader, "MODEL", model, sizeof(model));
        }
      }

      if (area[0] && model[0])
      {
        const u8 area_code = GetAreaCode(area);
        const u8 id_ctr = u8(m_config.IdGen());
        const HardwareModel hardware_model = GetHardwareModel(model);

        const u32 hollywood_id = m_ios.GetIOSC().GetDeviceId();
        u64 user_id = 0;

        const s32 ret = NWC24MakeUserID(&user_id, hollywood_id, id_ctr, hardware_model, area_code);
        m_config.SetId(user_id);
        m_config.IncrementIdGen();
        m_config.SetCreationStage(NWC24::NWC24CreationStage::Generated);
        m_config.SetChecksum(m_config.CalculateNwc24ConfigChecksum());
        m_config.WriteConfig();
        m_config.WriteCBK();

        WriteReturnValue(memory, ret, request.buffer_out);
      }
      else
      {
        LogError(ErrorType::Account, NWC24::WC24_ERR_INVALID_VALUE);
        WriteReturnValue(memory, NWC24::WC24_ERR_FATAL, request.buffer_out);
      }
    }
    else if (m_config.IsGenerated())
    {
      WriteReturnValue(memory, NWC24::WC24_ERR_ID_GENERATED, request.buffer_out);
    }
    else if (m_config.IsRegistered())
    {
      WriteReturnValue(memory, NWC24::WC24_ERR_ID_REGISTERED, request.buffer_out);
    }
    memory.Write_U64(m_config.Id(), request.buffer_out + 4);
    memory.Write_U32(u32(m_config.CreationStage()), request.buffer_out + 0xC);
    break;

  case IOCTL_NWC24_GET_SCHEDULER_STAT:
  {
    if (request.buffer_out == 0 || request.buffer_out % 4 != 0 || request.buffer_out_size < 16)
    {
      return_value = IPC_EINVAL;
      ERROR_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_GET_SCHEDULER_STAT = IPC_EINVAL");
      break;
    }

    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_GET_SCHEDULER_STAT - buffer out size: %u",
             request.buffer_out_size);

    // On a real Wii, GetSchedulerStat copies memory containing a list of error codes recorded by
    // KD among other things.
    u32 buffer[256];
    for (size_t i = 0; i < 256; ++i)
      buffer[i] = LoadSchedulerWord(i);
    const u32 out_size = std::min(request.buffer_out_size, 256U);
    memory.CopyToEmu(request.buffer_out, buffer, out_size);
    break;
  }

  case IOCTL_NWC24_SAVE_MAIL_NOW:
    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_SAVE_MAIL_NOW - NI");
    break;

  case IOCTL_NWC24_CHECK_MAIL_NOW:
    return LaunchAsyncTask(&NetKDRequestDevice::HandleNWC24CheckMailNow, request);

  case IOCTL_NWC24_DOWNLOAD_NOW_EX:
    return LaunchAsyncTask(&NetKDRequestDevice::HandleNWC24DownloadNowEx, request);

  case IOCTL_NWC24_SEND_MAIL_NOW:
    return LaunchAsyncTask(&NetKDRequestDevice::HandleNWC24SendMailNow, request);

  case IOCTL_NWC24_REQUEST_SHUTDOWN:
  {
    if (request.buffer_in == 0 || request.buffer_in % 4 != 0 || request.buffer_in_size < 8 ||
        request.buffer_out == 0 || request.buffer_out % 4 != 0 || request.buffer_out_size < 4)
    {
      return_value = IPC_EINVAL;
      ERROR_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_REQUEST_SHUTDOWN = IPC_EINVAL");
      break;
    }

    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_REQUEST_SHUTDOWN");
    [[maybe_unused]] const u32 event = memory.Read_U32(request.buffer_in);
    // TODO: Advertise shutdown event
    // TODO: Shutdown USB keyboard LEDs if event == 3
    // TODO: IOCTLV_NCD_SETCONFIG
    // TODO: DHCP related features
    // SOGetInterfaceOpt(0xfffe,0x4003);  // IP settings
    // SOGetInterfaceOpt(0xfffe,0xc001);  // DHCP lease time remaining?
    // SOGetInterfaceOpt(0xfffe,0x1003);  // Error
    // Call /dev/net/ip/top 0x1b (SOCleanup), it closes all sockets
    GetEmulationKernel().GetSocketManager()->Clean();
    return_value = IPC_SUCCESS;
    break;
  }
  case IOCTL_NWC24_KD_GET_TIME_TRIGGERS:
  {
    if (request.buffer_out == 0 || request.buffer_out % 4 != 0 || request.buffer_out_size < 12)
    {
      return_value = IPC_EINVAL;
      ERROR_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_KD_GET_TIME_TRIGGERS = IPC_EINVAL");
      break;
    }

    INFO_LOG(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_KD_GET_TIME_TRIGGERS");

    memory.Write_U32(static_cast<u32>(retro_atomic_load_acquire_int(&m_mail_span)),
                     request.buffer_out + 4);
    memory.Write_U32(static_cast<u32>(retro_atomic_load_acquire_int(&m_download_span)),
                     request.buffer_out + 8);
    WriteReturnValue(memory, NWC24::WC24_OK, request.buffer_out);

    return_value = IPC_SUCCESS;
    break;
  }

  default:
    request.Log(GetDeviceName(), Common::Log::LogType::IOS_WC24);
  }

  return IPCReply(return_value);
}
}  // namespace IOS::HLE
