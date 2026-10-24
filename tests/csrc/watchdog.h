// SPDX-License-Identifier: BSD-3-Clause
// A test's watchdog, the same on every system: a host thread, which no stop of the world waits
// for, ending the test with status 99 when it runs too long, as a hang would.
#ifndef JELLO_TESTS_WATCHDOG_H
#define JELLO_TESTS_WATCHDOG_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char* watchdog_name;

static void* watchdog_run(void* seconds) {
  struct timespec wait = {(time_t)(uintptr_t)seconds, 0};
  while(nanosleep(&wait, &wait) != 0) {}
  fprintf(stderr, "WATCHDOG: %s took too long, aborting\n", watchdog_name);
  fflush(stderr);
  _Exit(99);
}

/* Ends this program, test `name`, with status 99 if it still runs after `seconds`. */
static void start_watchdog(const char* name, unsigned seconds) {
  pthread_t thread;
  watchdog_name = name;
  if(pthread_create(&thread, NULL, watchdog_run, (void*)(uintptr_t)seconds) != 0) abort();
  pthread_detach(thread);
}

#endif
