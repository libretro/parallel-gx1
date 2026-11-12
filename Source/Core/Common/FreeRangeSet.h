// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/* The free [from, to) ranges of a code region, in order, in one array: what
 * a std::map and a std::multimap kept, a node each, made anew whenever a
 * range's start moved - every block compiled at the
 * start of the largest. Ranges that overlap or touch are merged as they were.
 * The array is kept when the set is cleared: once it has grown to the most
 * ranges the region is split into, nothing is allocated. The largest range is
 * found by looking at each: the first of the largest, in address order. */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "Common/CommonTypes.h"

class FreeRangeSet
{
public:
  struct Range
  {
    u8* from;
    u8* to;
  };

  /* the largest range, as an iterator by size would give it: null when there is none */
  struct Largest
  {
    const Range* range;
    u8* from() const { return range ? range->from : nullptr; }
    u8* to() const { return range ? range->to : nullptr; }
    bool operator==(const Largest& other) const { return range == other.range; }
    bool operator!=(const Largest& other) const { return range != other.range; }
  };

  FreeRangeSet() = default;
  ~FreeRangeSet() { free(m_ranges); }
  FreeRangeSet(const FreeRangeSet&) = delete;
  FreeRangeSet& operator=(const FreeRangeSet&) = delete;

  void clear() { m_count = 0; }
  bool empty() const { return m_count == 0; }
  size_t size() const { return m_count; }
  const Range* begin() const { return m_ranges; }
  const Range* end() const { return m_ranges + m_count; }

  Largest by_size_begin() const
  {
    const Range* largest = nullptr;
    size_t i;
    for (i = 0; i < m_count; i++)
    {
      if (!largest || m_ranges[i].to - m_ranges[i].from > largest->to - largest->from)
        largest = &m_ranges[i];
    }
    return Largest{largest};
  }
  Largest by_size_end() const { return Largest{nullptr}; }

  void insert(u8* from, u8* to)
  {
    size_t i, j;
    if (from >= to)
      return;
    i = FirstEndingAtOrAfter(from);
    /* [i, j): the ranges the new one overlaps or touches, merged into it */
    for (j = i; j < m_count && m_ranges[j].from <= to; j++)
    {
      if (m_ranges[j].from < from)
        from = m_ranges[j].from;
      if (m_ranges[j].to > to)
        to = m_ranges[j].to;
    }
    if (j > i)
    {
      m_ranges[i].from = from;
      m_ranges[i].to = to;
      Remove(i + 1, j - (i + 1));
      return;
    }
    if (!MakeRoom(i, 1))
      abort();
    m_ranges[i].from = from;
    m_ranges[i].to = to;
  }

  void erase(u8* from, u8* to)
  {
    size_t i, j;
    if (from >= to)
      return;
    /* the first range that ends after from: the ranges up to the first that starts at or after
     * to lose what [from, to) covers */
    i = FirstEndingAtOrAfter(from);
    if (i < m_count && m_ranges[i].to == from)
      i++;
    if (i == m_count || m_ranges[i].from >= to)
      return;
    if (m_ranges[i].from < from && m_ranges[i].to > to)
    {
      /* inside one range: it becomes two */
      if (!MakeRoom(i + 1, 1))
        abort();
      m_ranges[i + 1].from = to;
      m_ranges[i + 1].to = m_ranges[i].to;
      m_ranges[i].to = from;
      return;
    }
    if (m_ranges[i].from < from)
    {
      m_ranges[i].to = from;
      i++;
    }
    for (j = i; j < m_count && m_ranges[j].to <= to; j++)
    {
    }
    if (j < m_count && m_ranges[j].from < to)
      m_ranges[j].from = to;
    Remove(i, j - i);
  }

private:
  /* the first range whose end is at or after @address (it may touch it) */
  size_t FirstEndingAtOrAfter(u8* address) const
  {
    size_t lo = 0, hi = m_count;
    while (lo < hi)
    {
      const size_t mid = lo + (hi - lo) / 2;
      if (m_ranges[mid].to < address)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }

  bool MakeRoom(size_t at, size_t n)
  {
    if (m_count + n > m_cap)
    {
      size_t cap = m_cap ? m_cap * 2 : 64;
      Range* ranges;
      while (cap < m_count + n)
        cap *= 2;
      ranges = static_cast<Range*>(realloc(m_ranges, cap * sizeof(*ranges)));
      if (!ranges)
        return false;
      m_ranges = ranges;
      m_cap = cap;
    }
    memmove(m_ranges + at + n, m_ranges + at, (m_count - at) * sizeof(*m_ranges));
    m_count += n;
    return true;
  }

  void Remove(size_t at, size_t n)
  {
    if (!n)
      return;
    memmove(m_ranges + at, m_ranges + at + n, (m_count - at - n) * sizeof(*m_ranges));
    m_count -= n;
  }

  Range* m_ranges = nullptr;
  size_t m_count = 0;
  size_t m_cap = 0;
};
