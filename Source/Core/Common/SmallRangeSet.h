// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/* A set of [from, to) address ranges held in place: sorted, those that
 * overlap or touch merged, as Common::RangeSet keeps them, without a node
 * per range. It holds SMALL_RANGE_SET_MAX ranges; past that, the two with
 * the smallest gap between them become one, which only makes the set
 * cover more than was inserted. Copied by assignment. */

#include "Common/CommonTypes.h"

enum
{
  SMALL_RANGE_SET_MAX = 16
};

struct SmallRange
{
  u32 from;
  u32 to;
};

struct SmallRangeSet
{
  SmallRange ranges[SMALL_RANGE_SET_MAX];
  u32 count = 0;

  /* in order, for range-based for */
  const SmallRange* begin() const { return ranges; }
  const SmallRange* end() const { return ranges + count; }
  bool empty() const { return count == 0; }
  void clear() { count = 0; }

  bool overlaps(u32 from, u32 to) const
  {
    u32 i;
    if (from >= to)
      return false;
    for (i = 0; i < count && ranges[i].from < to; i++)
    {
      if (from < ranges[i].to)
        return true;
    }
    return false;
  }

  void insert(u32 from, u32 to)
  {
    u32 i, j;
    if (from >= to)
      return;
    /* the first range that ends at or past from: it and those after it that start at or before
     * to are merged into the new one */
    for (i = 0; i < count && ranges[i].to < from; i++)
    {
    }
    for (j = i; j < count && ranges[j].from <= to; j++)
    {
      if (ranges[j].from < from)
        from = ranges[j].from;
      if (ranges[j].to > to)
        to = ranges[j].to;
    }
    if (j > i)
    {
      /* [i, j) become one at i */
      ranges[i].from = from;
      ranges[i].to = to;
      for (; j < count; j++)
        ranges[++i] = ranges[j];
      count = i + 1;
      return;
    }
    if (count == SMALL_RANGE_SET_MAX)
    {
      /* full: the two closest neighbours, the new range among them, become one */
      u32 k, best = 0, best_gap = 0xffffffffu;
      /* the new range goes into its closer neighbour, unless two stored ranges are closer still:
       * those become one, and the new range is inserted */
      for (k = 0; k + 1 < count; k++)
      {
        const u32 gap = ranges[k + 1].from - ranges[k].to;
        if (gap < best_gap)
        {
          best_gap = gap;
          best = k;
        }
      }
      {
        const u32 gap_left = i > 0 ? from - ranges[i - 1].to : 0xffffffffu;
        const u32 gap_right = i < count ? ranges[i].from - to : 0xffffffffu;
        if (gap_left <= best_gap && gap_left <= gap_right)
        {
          ranges[i - 1].to = to;
          return;
        }
        if (gap_right <= best_gap)
        {
          ranges[i].from = from;
          return;
        }
      }
      ranges[best].to = ranges[best + 1].to;
      for (k = best + 1; k + 1 < count; k++)
        ranges[k] = ranges[k + 1];
      count--;
      if (i > best + 1)
        i--;
    }
    for (j = count; j > i; j--)
      ranges[j] = ranges[j - 1];
    ranges[i].from = from;
    ranges[i].to = to;
    count++;
  }
};
