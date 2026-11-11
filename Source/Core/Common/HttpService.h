/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DOLPHIN_COMMON_HTTPSERVICE_H
#define DOLPHIN_COMMON_HTTPSERVICE_H

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <queues/mpsc_stack.h>
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP transfers on one thread of their own.
 *
 * libretro-common's net_http keeps its connection pool and DNS cache for
 * the one thread that drives transfers, and the core downloads from more
 * than one (WiiConnect24's worker, the Bluetooth firmware loader). Each of
 * them hands its request to this thread and waits for it; neither takes a
 * lock. */

/* Called with each run of the response body. Return false to abort. */
typedef bool (*http_job_sink_t)(void *userdata, const void *data, size_t len);

typedef struct http_job
{
   mpsc_stack_node_t node;
   /* the request, borrowed until http_service_run returns */
   const char *url;
   const char *method;
   const char *content_type; /* with a body, NULL for none */
   const void *body;
   size_t body_len;
   size_t body_off;
   http_job_sink_t sink;
   void *sink_userdata;
   int timeout_ms;           /* without progress */
   bool follow_redirects;
   /* the result: the status, -1 when the transfer failed, and the
    * response's header block (net_http_headers_take), freed with
    * http_job_free */
   int status;
   char *headers;
   retro_atomic_int_t done;
} http_job_t;

/* Starts the thread. Once, before the first transfer. */
void http_service_init(void);
/* Stops it, after the last transfer. */
void http_service_deinit(void);

/* Runs @job on the HTTP thread and returns when it has finished. */
void http_service_run(http_job_t *job);

/* The value of response header @name in @job, NULL when there is none. */
const char *http_job_header(const http_job_t *job, const char *name);

void http_job_free(http_job_t *job);

#ifdef __cplusplus
}
#endif

#endif
