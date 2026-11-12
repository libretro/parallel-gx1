// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <queue>
#include <string>

#include <rthreads/rthreads.h>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/Flag.h"
#include "Common/HttpRequest.h"
#include "Common/WorkQueueThread.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/Network/KD/Mail/WC24FriendList.h"
#include "Core/IOS/Network/KD/Mail/WC24Send.h"
#include "Core/IOS/Network/KD/NWC24Config.h"
#include "Core/IOS/Network/KD/NWC24DL.h"
#include "Core/IOS/Network/KD/NetKDTime.h"
#include <queues/mpsc_stack.h>
#include <retro_atomic.h>

namespace IOS::HLE
{
// KD is the IOS module responsible for implementing WiiConnect24 functionality.
// It can perform HTTPS downloads, send and receive mail via SMTP, and execute a
// JavaScript-like language while the Wii is in standby mode.
class NetKDRequestDevice : public EmulationDevice
{
public:
  NetKDRequestDevice(EmulationKernel& ios, const std::string& device_name,
                     const std::shared_ptr<NetKDTimeDevice>& time_device);
  IPCReply HandleNWC24DownloadNowEx(const IOCtlRequest& request);
  NWC24::ErrorCode KDDownload(const u16 entry_index, const std::optional<u8> subtask_id);
  IPCReply HandleNWC24CheckMailNow(const IOCtlRequest& request);
  ~NetKDRequestDevice() override;

  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  void Update() override;

private:
  struct AsyncTask
  {
    IOS::HLE::Request request;
    std::function<IPCReply()> handler;
  };

  struct AsyncReply
  {
    mpsc_stack_node_t link;  // first: the drained chain is cast back
    IOS::HLE::Request request;
    s32 return_value;
  };

  // The scheduler buffer is read by the emulation thread and written by both worker threads,
  // a word at a time; the words are kept byte-swapped as the guest reads them.
  u32 LoadSchedulerWord(size_t index) const;
  void StoreSchedulerWord(size_t index, u32 swapped);
  void IncrementSchedulerWord(size_t index);

  template <typename Method, typename Request>
  std::optional<IPCReply> LaunchAsyncTask(Method method, const Request& request)
  {
    m_work_queue.EmplaceItem(AsyncTask{request, std::bind(method, this, request)});
    return std::nullopt;
  }

  enum class CurrentFunction : u32
  {
    None = 0,
    Account = 1,
    Check = 2,
    Receive = 3,
    Send = 5,
    Save = 6,
    Download = 7,
  };

  enum class ErrorType
  {
    Account,
    KD_Download,
    Client,
    Server,
    CheckMail,
    SendMail,
    ReceiveMail,
    CGI,
  };

  enum class SchedulerEvent
  {
    Mail,
    Download,
  };

  IPCReply HandleNWC24SendMailNow(const IOCtlRequest& request);
  NWC24::ErrorCode KDCheckMail(u32* mail_flag, u32* interval);
  IPCReply HandleRequestRegisterUserId(const IOCtlRequest& request);
  NWC24::ErrorCode KDSendMail();

  void LogError(ErrorType error_type, s32 error_code);
  void SchedulerTimer();
  void SchedulerWorker(SchedulerEvent event);
  NWC24::ErrorCode DetermineDownloadTask(u16* entry_index, std::optional<u8>* subtask_id) const;
  NWC24::ErrorCode DetermineSubtask(u16 entry_index, std::optional<u8>* subtask_id) const;

  static constexpr u32 MAX_MAIL_SIZE = 208952;
  static std::string GetValueFromCGIResponse(const std::string& response, const std::string& key);
  static constexpr std::array<u8, 20> MAIL_CHECK_KEY = {0xce, 0x4c, 0xf2, 0x9a, 0x3d, 0x6b, 0xe1,
                                                        0xc2, 0x61, 0x91, 0x72, 0xb5, 0xcb, 0x29,
                                                        0x8c, 0x89, 0x72, 0xd4, 0x50, 0xad};

  static constexpr u32 DEFAULT_SCHEDULER_SPAN_MINUTES = 11;

  NWC24::NWC24Config m_config;
  NWC24::NWC24Dl m_dl_list;
  NWC24::Mail::WC24SendList m_send_list;
  NWC24::Mail::WC24FriendList m_friend_list;
  Common::WorkQueueThreadSP<AsyncTask> m_work_queue;
  Common::AsyncWorkThreadSP m_scheduler_work_queue;
  // Replies from the worker threads, handed to the emulation thread by Update.
  mpsc_stack_t m_async_replies{};
  retro_atomic_int_t m_error_count{};
  std::array<retro_atomic_int_t, 256> m_scheduler_buffer{};
  std::shared_ptr<NetKDTimeDevice> m_time_device;
  // TODO: Maybe move away from Common::HttpRequest?
  Common::HttpRequest m_http{60 * 1000};
  // Minutes between scheduled tasks, set by the workers from server replies.
  retro_atomic_int_t m_download_span{2};
  retro_atomic_int_t m_mail_span{1};
  bool m_handle_mail;  // set before the threads start
  Common::Flag m_scheduler_running{true};
  // A minute at 60 frames a second; 50 Hz games schedule a fifth slower, as WC24 allows.
  static constexpr u64 FRAMES_PER_MINUTE = 60 * 60;
  sthread_t* m_scheduler_timer_thread = nullptr;
};
}  // namespace IOS::HLE
