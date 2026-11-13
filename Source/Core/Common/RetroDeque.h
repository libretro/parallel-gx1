// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/* libretro-common's retro_deque for the core: the u32 queue several devices
 * keep, and a queue's state written as PointerWrap writes a std::deque. */

#include <queues/retro_deque.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"

RETRO_DEQUE_DEFINE(u32_deque, u32);
RETRO_DEQUE_DEFINE(u8_deque, u8);

/* The state of the retro_deque q made by RETRO_DEQUE_DEFINE(name, ...): the
 * count as a u32, then each element as p.Do writes it - what a std::deque
 * of the same elements was, so a state either saved loads in the other.
 * Read back, the queue grows to hold what the state does. */
#define DO_RETRO_DEQUE(p, name, q)                                                                 \
  do                                                                                               \
  {                                                                                                \
    u32 do_count_ = static_cast<u32>(name##_count(q));                                            \
    u32 do_i_;                                                                                     \
    (p).Do(do_count_);                                                                             \
    if ((p).IsReadMode())                                                                          \
    {                                                                                              \
      name##_clear(q);                                                                             \
      for (do_i_ = 0; do_i_ < do_count_; do_i_++)                                                  \
      {                                                                                            \
        name##_value_t do_value_{};                                                                \
        (p).Do(do_value_);                                                                         \
        name##_push_back_growing(q, &do_value_);                                                   \
      }                                                                                            \
    }                                                                                              \
    else                                                                                           \
    {                                                                                              \
      for (do_i_ = 0; do_i_ < do_count_; do_i_++)                                                  \
        (p).Do(*name##_at(q, do_i_));                                                              \
    }                                                                                              \
  } while (0)
