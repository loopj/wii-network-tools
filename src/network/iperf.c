#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gccore.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>

#include "sock.h"
#include "suite.h"

#include "iperf.h"

// Length of the cookie that ties the data connection to the control connection, 36 characters and a zero
#define COOKIE_LEN 37

// Bytes per send
#define BLOCK_LEN 16384

// Time allowed for each control message
#define CONTROL_WAIT_MS 10000

// Longest wait for data before checking the clock and the control connection again
#define DATA_WAIT_MS 100

// Offset added to a socket error by read_state, so it stays apart from the states
#define SOCKET_ERROR_OFFSET 100

// Control states the server sends and the client answers with, one signed byte each, with iperf3's names
enum iperf_state {
  TEST_START       = 1,
  TEST_RUNNING     = 2,
  TEST_END         = 4,
  PARAM_EXCHANGE   = 9,
  CREATE_STREAMS   = 10,
  SERVER_TERMINATE = 11,
  EXCHANGE_RESULTS = 13,
  DISPLAY_RESULTS  = 14,
  IPERF_DONE       = 16,
  ACCESS_DENIED    = -1,
  SERVER_ERROR     = -2,
};

// Characters a cookie is made of, iperf3's set
static const char cookie_characters[] = "abcdefghijklmnopqrstuvwxyz234567";

// The block sent or received
static char block[BLOCK_LEN] ATTRIBUTE_ALIGN(32);

// Read one control state byte, returning it, or a value below SERVER_ERROR on a socket error
static int read_state(int control)
{
  signed char state;
  int error = sock_recv_all(control, &state, 1, CONTROL_WAIT_MS);
  return error < 0 ? error - SOCKET_ERROR_OFFSET : state;
}

// Send one control state byte
static int send_state(int control, signed char state)
{
  return sock_send_all(control, &state, 1);
}

// Send a JSON message, a big endian length followed by the text
static int send_json(int control, const char *text)
{
  uint32_t len      = (uint32_t)strlen(text);
  uint8_t header[4] = {len >> 24, len >> 16, len >> 8, len};
  int error         = sock_send_all(control, header, sizeof header);
  return error < 0 ? error : sock_send_all(control, text, len);
}

// Read a JSON message and drop it, since the app keeps its own counts
static int skip_json(int control)
{
  // Read the length
  uint8_t header[4];
  int error = sock_recv_all(control, header, sizeof header, CONTROL_WAIT_MS);
  if (error < 0)
    return error;

  // Read the text a block at a time
  uint32_t len = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) | header[3];
  while (len > 0) {
    uint32_t chunk = len < BLOCK_LEN ? len : BLOCK_LEN;
    if ((error = sock_recv_all(control, block, chunk, CONTROL_WAIT_MS)) < 0)
      return error;
    len -= chunk;
  }
  return 0;
}

// Make a random cookie of letters and digits, like iperf3
static void make_cookie(char *cookie)
{
  srand((unsigned)gettime());
  for (int i = 0; i < COOKIE_LEN - 1; i++)
    cookie[i] = cookie_characters[rand() % (sizeof cookie_characters - 1)];
  cookie[COOKIE_LEN - 1] = 0;
}

// Wait for the server's next state and check it is the one expected
static int expect_state(int control, int expected, char *note, size_t note_len)
{
  // Accept the state expected
  int state = read_state(control);
  if (state == expected)
    return 0;

  // Name a busy server, a socket error or an unexpected state
  if (state == ACCESS_DENIED) {
    snprintf(note, note_len, "server busy with another test");
    return -EBUSY;
  }
  if (state < SERVER_ERROR) {
    snprintf(note, note_len, "no answer from server, waiting for state %d", expected);
    return state + SOCKET_ERROR_OFFSET;
  }
  snprintf(note, note_len, "server sent state %d, expected %d", state, expected);
  return -EPROTO;
}

// Add bytes to the count for the second of the test they moved in
static void count_second(struct iperf_result *result, u64 start, uint32_t bytes)
{
  uint32_t second = diff_msec(start, gettime()) / 1000;
  if (second < IPERF_SECONDS_MAX)
    result->second_bytes[second] += bytes;
}

// Move data until the time is up, sending or receiving, while watching the control connection for an abort
static int move_data(int control, int data, int seconds, bool reverse, struct iperf_result *result, char *note,
                     size_t note_len)
{
  u64 start = gettime();
  while (diff_msec(start, gettime()) < (u64)seconds * 1000 && !suite_cancelled()) {
    // Stop when the server ends the test early
    if (sock_wait_readable(control, 0) > 0) {
      snprintf(note, note_len, "server ended the test early, state %d", read_state(control));
      return -ECONNABORTED;
    }

    // Receive in reverse, counting each block
    if (reverse) {
      if (sock_wait_readable(data, DATA_WAIT_MS) <= 0)
        continue;
      s32 received = net_recv(data, block, BLOCK_LEN, 0);
      if (received <= 0) {
        snprintf(note, note_len, "data connection closed");
        return received < 0 ? received : -ECONNRESET;
      }
      result->bytes += (uint64_t)received;
      count_second(result, start, (uint32_t)received);
      continue;
    }

    // Send otherwise, counting each block
    int error = sock_send_all(data, block, BLOCK_LEN);
    if (error < 0) {
      snprintf(note, note_len, "data send failed");
      return error;
    }
    result->bytes += BLOCK_LEN;
    count_second(result, start, BLOCK_LEN);
  }

  // Record the length of the test
  result->ms = diff_msec(start, gettime());

  // Count the last second only when it ran in full
  uint32_t whole  = result->ms / 1000;
  result->seconds = whole < IPERF_SECONDS_MAX ? (int)whole : IPERF_SECONDS_MAX;
  return 0;
}

int iperf_run(const char *host, int port, int seconds, bool reverse, struct iperf_result *result, char *note,
              size_t note_len)
{
  char cookie[COOKIE_LEN];
  memset(result, 0, sizeof *result);
  note[0] = 0;

  // Resolve the server
  uint32_t ip = sock_resolve(host);
  if (ip == 0) {
    snprintf(note, note_len, "could not resolve %s", host);
    return -EHOSTUNREACH;
  }

  // Open the control connection with the cookie, then wait for the server to ask for the test parameters
  int control = sock_connect(ip, (uint16_t)port);
  if (control < 0) {
    snprintf(note, note_len, "could not connect to %s:%d", host, port);
    return control;
  }
  int data = -1;
  make_cookie(cookie);
  int error = sock_send_all(control, cookie, COOKIE_LEN);
  if (error < 0) {
    snprintf(note, note_len, "cookie send failed");
    goto done;
  }
  if ((error = expect_state(control, PARAM_EXCHANGE, note, note_len)) < 0)
    goto done;

  // Send the parameters for one TCP stream
  char params[256];
  snprintf(params, sizeof params,
           "{\"tcp\":true,\"omit\":0,\"time\":%d,\"num\":0,\"blockcount\":0,\"parallel\":1,\"len\":%d,%s"
           "\"pacing_timer\":1000,\"client_version\":\"3.17\"}",
           seconds, BLOCK_LEN, reverse ? "\"reverse\":true," : "");
  if ((error = send_json(control, params)) < 0) {
    snprintf(note, note_len, "parameter send failed");
    goto done;
  }
  if ((error = expect_state(control, CREATE_STREAMS, note, note_len)) < 0)
    goto done;

  // Open the data connection, introduced by the same cookie, and wait for the test to start
  data = sock_connect(ip, (uint16_t)port);
  if (data < 0) {
    error = data;
    snprintf(note, note_len, "data connection failed");
    goto done;
  }
  if ((error = sock_send_all(data, cookie, COOKIE_LEN)) < 0) {
    snprintf(note, note_len, "data cookie send failed");
    goto done;
  }
  if ((error = expect_state(control, TEST_START, note, note_len)) < 0 ||
      (error = expect_state(control, TEST_RUNNING, note, note_len)) < 0)
    goto done;

  // Move the data
  memset(block, 0x5a, sizeof block);
  if ((error = move_data(control, data, seconds, reverse, result, note, note_len)) < 0)
    goto done;

  // End the test from the client side
  if ((error = send_state(control, TEST_END)) < 0) {
    snprintf(note, note_len, "test end send failed");
    goto done;
  }

  // Drain the data connection in reverse until the server answers, since it may still be sending
  u64 drain_start = gettime();
  while (reverse && sock_wait_readable(control, 0) == 0) {
    if (diff_msec(drain_start, gettime()) > CONTROL_WAIT_MS || suite_cancelled()) {
      snprintf(note, note_len, "no results from server after the test");
      error = -ETIMEDOUT;
      goto done;
    }
    if (sock_wait_readable(data, DATA_WAIT_MS) > 0 && net_recv(data, block, BLOCK_LEN, 0) <= 0)
      break;
  }
  if ((error = expect_state(control, EXCHANGE_RESULTS, note, note_len)) < 0)
    goto done;

  // Swap byte counts with the server
  char results[384];
  snprintf(results, sizeof results,
           "{\"cpu_util_total\":0,\"cpu_util_user\":0,\"cpu_util_system\":0,\"sender_has_retransmits\":0,"
           "\"streams\":[{\"id\":1,\"bytes\":%llu,\"retransmits\":0,\"jitter\":0,\"errors\":0,\"packets\":0,"
           "\"start_time\":0,\"end_time\":%u.%03u}]}",
           (unsigned long long)result->bytes, (unsigned)(result->ms / 1000), (unsigned)(result->ms % 1000));
  if ((error = send_json(control, results)) < 0 || (error = skip_json(control)) < 0) {
    snprintf(note, note_len, "results exchange failed");
    goto done;
  }

  // Finish the test
  if ((error = expect_state(control, DISPLAY_RESULTS, note, note_len)) < 0)
    goto done;
  if ((error = send_state(control, IPERF_DONE)) < 0)
    snprintf(note, note_len, "done send failed");

done:
  if (data >= 0)
    net_close(data);
  net_close(control);
  return error;
}
