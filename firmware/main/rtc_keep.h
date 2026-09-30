#pragma once

/*
 * Today's counters kept in RTC memory (RTC_NOINIT_ATTR): they survive a software restart, a panic, a watchdog reset
 * and the restart after an OTA, and are lost on power loss. A block is trusted only with its magic and a matching sum;
 * change the magic whenever the layout of a block changes (another firmware may have left a different one behind).
 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* FNV-1a over the block, without its trailing `sum` field */
static inline uint32_t rtc_keep_sum(const void *block, size_t len_without_sum)
{
    const uint8_t *p = (const uint8_t *) block;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len_without_sum; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* A local calendar day, unique across years (tm_yday alone repeats every year) */
static inline int32_t rtc_keep_day(const struct tm *t)
{
    return (int32_t) t->tm_year * 366 + t->tm_yday;
}
