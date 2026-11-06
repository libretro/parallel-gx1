// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/AsyncRequests.h"

#include "Core/System.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/VertexManagerBase.h"

AsyncRequests AsyncRequests::s_singleton;

AsyncRequests::AsyncRequests()
{
  retro_waitable_spsc_init(&m_queue, QUEUE_REQUESTS * sizeof(Request*));
  retro_eventcount_init(&m_completed);
}

AsyncRequests::~AsyncRequests()
{
  retro_eventcount_free(&m_completed);
  retro_waitable_spsc_free(&m_queue);
}

void AsyncRequests::PullEvents()
{
  if (retro_spsc_read_avail(&m_queue.queue) < sizeof(Request*))
    return;

  // This is only called if the queue isn't empty.
  // So just flush the pipeline to get accurate results.
  g_vertex_manager->Flush();

  Request* request;
  while (retro_waitable_spsc_read(&m_queue, &request, sizeof(request)) == sizeof(request))
    request->run(request);
}

void AsyncRequests::Submit(Request* request)
{
  // Only waits when the video thread is QUEUE_REQUESTS requests behind.
  while (!retro_waitable_spsc_wait_writable(&m_queue, sizeof(request), 1000000))
  {
  }
  retro_waitable_spsc_write(&m_queue, &request, sizeof(request));

  auto& system = Core::System::GetInstance();
  system.GetFifo().RunGpu();
}

void AsyncRequests::WaitDone(retro_atomic_int_t* done)
{
  while (!retro_atomic_load_acquire_int(done))
  {
    const int key = retro_eventcount_prepare_wait(&m_completed);
    if (retro_atomic_load_acquire_int(done))
    {
      retro_eventcount_cancel_wait(&m_completed);
      break;
    }
    retro_eventcount_commit_wait(&m_completed, key);
  }
}

void AsyncRequests::SetPassthrough(bool enable)
{
  m_passthrough = enable;
}
