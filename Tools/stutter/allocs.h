/* allocs: heap allocations the core makes while counting (allocs.c). */
#ifndef STUTTER_ALLOCS_H
#define STUTTER_ALLOCS_H

/* Finds the core from a function of it: 0 where allocations cannot be
 * counted (not glibc on Linux, or the core was not found). */
int allocs_init(void (*function_in_core)(void));
/* Counts the core's allocations from now (1), or stops (0). */
void allocs_count(int on);
/* "allocs play=N bytes=B sites_lost=L", then the top sites by count:
 * "alloc_site count=N at=0xOFFSET from=0xOFFSET", offsets in the core. */
void allocs_report(unsigned top);

#endif
