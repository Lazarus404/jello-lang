// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* The wall clock and the local time zone, for std.sys.Date, which does its calendar
 * arithmetic itself. Times are milliseconds since 1970-01-01T00:00:00Z. */

#include "std.h"

#include <time.h>

/* The time now, by the system's wall clock. */
void std_date_now(jdlo_ctx* c) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  jdl_return_i64(c, (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* The local time zone's offset from UTC, in minutes east, at the time in argument 0; 0 for a
 * time the C library cannot place. */
void std_date_offset(jdlo_ctx* c) {
  int64_t ms = jdl_arg_i64(c, 0);
  // Seconds rounded down, so a time just before 1970 falls in the second it is part of.
  time_t seconds = (time_t)(ms / 1000 - (ms % 1000 < 0));
  struct tm local;
#ifdef _WIN32
  /* The local time read back as UTC is ahead of the time itself by the offset. */
  int ok = localtime_s(&local, &seconds) == 0;
  jdl_return_i32(c, ok ? (int32_t)((_mkgmtime(&local) - seconds) / 60) : 0);
#else
  jdl_return_i32(c, localtime_r(&seconds, &local) ? (int32_t)(local.tm_gmtoff / 60) : 0);
#endif
}
