/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "Common/HttpService.h"

#include <stdlib.h>
#include <string.h>

#include <net/net_http.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

static mpsc_stack_t        s_jobs;
static retro_eventcount_t  s_work;  /* a job was pushed, or quit */
static retro_eventcount_t  s_done;  /* a job finished */
static retro_atomic_int_t  s_quit;
static retro_atomic_int_t  s_running;
static sthread_t          *s_thread;

static int64_t http_job_source(void *userdata, void *buf, size_t len)
{
   http_job_t *job = (http_job_t*)userdata;
   size_t      n   = job->body_len - job->body_off;
   if (n > len)
      n = len;
   memcpy(buf, (const uint8_t*)job->body + job->body_off, n);
   job->body_off += n;
   return (int64_t)n;
}

static bool http_job_rewind(void *userdata)
{
   ((http_job_t*)userdata)->body_off = 0;
   return true;
}

static void http_job_execute(http_job_t *job)
{
   struct http_connection_t *conn;
   struct http_t            *http;
   size_t progress = 0;
   size_t total    = 0;
   bool   timed_out = false;

   job->status  = -1;
   job->headers = NULL;
   if (!(conn = net_http_connection_new(job->url, job->method, NULL)))
      return;
   net_http_connection_set_redirects(conn, job->follow_redirects);
   if (job->body)
   {
      job->body_off = 0;
      net_http_connection_set_content_source(conn, job->content_type, job->body_len,
            http_job_source, http_job_rewind, job);
   }
   if (job->sink)
      net_http_connection_set_sink(conn, job->sink, job->sink_userdata);
   while (!net_http_connection_iterate(conn)) { }
   if (!net_http_connection_done(conn) || !(http = net_http_new(conn)))
   {
      net_http_connection_free(conn);
      return;
   }
   while (!net_http_update(http, &progress, &total))
   {
      if (!net_http_wait(http, job->timeout_ms))
      {
         timed_out = true;
         break;
      }
   }
   if (!timed_out)
   {
      job->status  = net_http_status(http);
      /* a 2xx whose body broke off is a failed transfer */
      if (job->status >= 200 && job->status <= 299 && net_http_error(http))
         job->status = -1;
      job->headers = net_http_headers_take(http, true);
   }
   net_http_delete(http);
   net_http_connection_free(conn);
}

static void http_service_thread(void *userdata)
{
   (void)userdata;
   for (;;)
   {
      mpsc_stack_node_t *chain;
      mpsc_stack_node_t *ordered = NULL;
      int                key     = retro_eventcount_prepare_wait(&s_work);

      chain = mpsc_stack_drain(&s_jobs);
      if (!chain)
      {
         if (retro_atomic_load_acquire_int(&s_quit))
         {
            retro_eventcount_cancel_wait(&s_work);
            break;
         }
         retro_eventcount_commit_wait(&s_work, key);
         continue;
      }
      retro_eventcount_cancel_wait(&s_work);

      /* pushed newest first: run them in the order they came */
      while (chain)
      {
         mpsc_stack_node_t *next = chain->next;
         chain->next = ordered;
         ordered     = chain;
         chain       = next;
      }
      while (ordered)
      {
         http_job_t *job = (http_job_t*)ordered;
         ordered         = ordered->next;
         http_job_execute(job);
         retro_atomic_store_release_int(&job->done, 1);
         retro_eventcount_notify(&s_done);
      }
   }
   net_http_deinit();
}

void http_service_init(void)
{
   if (retro_atomic_load_acquire_int(&s_running))
      return;
   mpsc_stack_init(&s_jobs);
   retro_atomic_int_init(&s_quit, 0);
   if (!retro_eventcount_init(&s_work))
      return;
   if (!retro_eventcount_init(&s_done))
   {
      retro_eventcount_free(&s_work);
      return;
   }
   net_http_init();
   if (!(s_thread = sthread_create(http_service_thread, NULL)))
   {
      net_http_deinit();
      retro_eventcount_free(&s_done);
      retro_eventcount_free(&s_work);
      return;
   }
   retro_atomic_store_release_int(&s_running, 1);
}

void http_service_deinit(void)
{
   if (!retro_atomic_load_acquire_int(&s_running))
      return;
   retro_atomic_store_release_int(&s_quit, 1);
   retro_eventcount_notify(&s_work);
   sthread_join(s_thread);
   s_thread = NULL;
   retro_atomic_store_release_int(&s_running, 0);
   retro_eventcount_free(&s_done);
   retro_eventcount_free(&s_work);
}

void http_service_run(http_job_t *job)
{
   job->status  = -1;
   job->headers = NULL;
   if (!retro_atomic_load_acquire_int(&s_running))
      return;
   retro_atomic_int_init(&job->done, 0);
   mpsc_stack_push(&s_jobs, &job->node);
   retro_eventcount_notify(&s_work);
   for (;;)
   {
      int key = retro_eventcount_prepare_wait(&s_done);
      if (retro_atomic_load_acquire_int(&job->done))
      {
         retro_eventcount_cancel_wait(&s_done);
         return;
      }
      retro_eventcount_commit_wait(&s_done, key);
   }
}

const char *http_job_header(const http_job_t *job, const char *name)
{
   return job->headers ? net_http_header_value(job->headers, name) : NULL;
}

void http_job_free(http_job_t *job)
{
   free(job->headers);
   job->headers = NULL;
}
