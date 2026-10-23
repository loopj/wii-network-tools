#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Longest test, in seconds, that keeps a count for every second
#define IPERF_SECONDS_MAX 60

// Outcome of one iperf3 test
struct iperf_result {
  // Bytes this side sent or received during the test
  uint64_t bytes;

  // Length of the test, in milliseconds
  uint32_t ms;

  // Bytes moved in each second of the test
  uint32_t second_bytes[IPERF_SECONDS_MAX];

  // Whole seconds counted in second_bytes
  int seconds;
};

// Send or, in reverse, receive one iperf3 TCP test, returning 0, -EBUSY for a busy server or an error, with a note
int iperf_run(const char *host, int port, int seconds, bool reverse, struct iperf_result *result, char *note,
              size_t note_len);
