#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "input.h"
#include "report.h"
#include "steps.h"
#include "suite.h"

#include "network_tests.h"

// Number of entries in an array
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

// Duration above which a step counts as slow
#define SLOW_MS 1000

// Duration above which a link up counts as slow, since NCD gives a join seven seconds
#define SLOW_LINK_MS 5000

// Duration above which a reconnect cycle counts as slow
#define SLOW_CYCLE_MS 7000

// Link up and down cycles in Reconnects
#define RECONNECTS 10

// Longest random pause before each link up in Reconnects, 0 for none
#define PAUSE_MAX_MS 10000

// Interval at which a pause checks the buttons, in milliseconds
#define PAUSE_SLICE_MS 100

// One step of a test
struct step {
  // Label in the report
  const char *name;

  // Runs the step, returning 0 or a negative error
  int (*run)(void);

  // Duration above which it shows in yellow, 0 for a step that runs for a set time
  uint32_t slow_ms;
};

// One part of a reconnect cycle
struct cycle_part {
  // Label in the cycle's note
  const char *name;

  // What the cycle's row shows while the part runs
  const char *progress;

  // Runs the part, returning 0 or a negative error
  int (*run)(void);
};

// Steps that bring the link up and check the address
static const struct step link_up_steps[] = {
  {"Link up", step_link_up, SLOW_LINK_MS},
  {"Address", step_address, SLOW_MS},
};

// Step that resolves the configured name
static const struct step dns_steps[] = {{"DNS", step_dns, SLOW_MS}};

// Step that fetches the configured URL
static const struct step tcp_steps[] = {{"TCP", step_tcp, SLOW_MS}};

// Step that queries the configured NTP server
static const struct step udp_steps[] = {{"UDP", step_udp, SLOW_MS}};

// Step that checks the iperf3 server is listening and free
static const struct step server_steps[] = {{"Server", step_server, SLOW_MS}};

// Step that sends to iperf3
static const struct step upload_steps[] = {{"Upload", step_upload, 0}};

// Step that receives from iperf3
static const struct step download_steps[] = {{"Download", step_download, 0}};

// Parts of one reconnect cycle, reported together on one line
static const struct cycle_part cycle_parts[] = {
  {"up", "bringing the link up", step_link_up},
  {"address", "reading the address", step_address},
  {"down", "taking the link down", step_link_down},
};

// Link up failed earlier in this run, so the tests after it skip
static bool link_failed;

// The server step failed earlier in this run, so the throughput tests skip
static bool server_failed;

// Report steps as left out for a reason
static enum result skip_steps(const struct step *steps, size_t count, const char *reason)
{
  for (size_t i = 0; i < count; i++)
    report_step_skip(steps[i].name, reason);
  return RESULT_SKIP;
}

// Run steps in order and report each, stopping at a failure or when B is pressed
static enum result run_steps(const struct step *steps, size_t count)
{
  for (size_t i = 0; i < count; i++) {
    // Stop on B
    input_check_power_off();
    if (suite_cancelled())
      return RESULT_CANCEL;

    // Run the step and time it
    report_step_start(steps[i].name);
    u64 start = gettime();
    int error = steps[i].run();
    u32 ms    = diff_msec(start, gettime());

    // Report a step that B stopped as cancelled
    if (error < 0 && suite_cancelled()) {
      report_step_cancel(steps[i].name, ms);
      return RESULT_CANCEL;
    }

    // Report a failure with the link state, and the steps after it as left out
    if (error < 0) {
      report_step_fail(steps[i].name, error, ms, steps_note());
      steps_report_link_state();
      skip_steps(steps + i + 1, count - i - 1, "skipped");
      return RESULT_FAIL;
    }

    // Report the step as passed with its note
    report_step_ok(steps[i].name, ms, steps[i].slow_ms, steps_note());
  }
  return RESULT_PASS;
}

// Bring the link up unless an earlier test left it up
static enum result ensure_link(void)
{
  if (steps_link_is_up())
    return RESULT_PASS;
  return run_steps(link_up_steps, ARRAY_COUNT(link_up_steps));
}

// Run steps once the link is up, or leave them out when link up failed earlier in the run
static enum result run_with_link(const struct step *steps, size_t count)
{
  if (link_failed)
    return skip_steps(steps, count, "skipped, no link");
  enum result result = ensure_link();
  return result == RESULT_PASS ? run_steps(steps, count) : result;
}

// Run throughput steps, or leave them out when the server step failed earlier in the run
static enum result run_with_server(const struct step *steps, size_t count)
{
  if (server_failed)
    return skip_steps(steps, count, "skipped, no server");
  return run_with_link(steps, count);
}

// Sleep for ms milliseconds, still answering the power buttons, returning false when B cancels the wait
static bool pause_ms(int ms)
{
  // Sleep a slice at a time, stopping on B
  for (int waited = 0; waited < ms; waited += PAUSE_SLICE_MS) {
    input_check_power_off();
    if (suite_cancelled())
      return false;
    usleep(PAUSE_SLICE_MS * 1000);
  }
  return true;
}

// Bring the link up as a test of its own, so the tests after it start from a link that is up
static enum result test_link(void)
{
  // Start the run's skips afresh, since every suite that skips starts with this test
  server_failed      = false;
  enum result result = ensure_link();
  link_failed        = result == RESULT_FAIL;
  return result;
}

// Resolve the configured name
static enum result test_dns(void)
{
  return run_with_link(dns_steps, ARRAY_COUNT(dns_steps));
}

// Fetch the configured URL
static enum result test_tcp(void)
{
  return run_with_link(tcp_steps, ARRAY_COUNT(tcp_steps));
}

// Query the configured NTP server
static enum result test_udp(void)
{
  return run_with_link(udp_steps, ARRAY_COUNT(udp_steps));
}

// Check the iperf3 server before the throughput tests use it
static enum result test_server(void)
{
  enum result result = run_with_link(server_steps, ARRAY_COUNT(server_steps));
  server_failed      = result == RESULT_FAIL;
  return result;
}

// Send to iperf3
static enum result test_upload(void)
{
  return run_with_server(upload_steps, ARRAY_COUNT(upload_steps));
}

// Receive from iperf3
static enum result test_download(void)
{
  return run_with_server(download_steps, ARRAY_COUNT(download_steps));
}

// Run one cycle of link up, address and link down after a pause, reported on one line
static enum result run_reconnect_cycle(int cycle, int pause)
{
  // Name the cycle and draw its row
  char name[32];
  char note[128];
  snprintf(name, sizeof name, "Cycle %d/%d", cycle, RECONNECTS);
  report_step_start(name);

  // Pause first, saying how long
  if (pause > 0)
    report_step_progress("pausing for %s", report_duration((u32)pause));
  if (!pause_ms(pause)) {
    report_step_cancel(name, 0);
    return RESULT_CANCEL;
  }

  // Run each part, adding its time to the note
  u32 total = 0;
  int len   = 0;
  note[0]   = 0;
  for (size_t i = 0; i < ARRAY_COUNT(cycle_parts); i++) {
    // Stop on B
    input_check_power_off();
    if (suite_cancelled()) {
      report_step_cancel(name, total);
      return RESULT_CANCEL;
    }

    // Run the part and time it, showing what it does
    report_step_progress("%s", cycle_parts[i].progress);
    u64 start = gettime();
    int error = cycle_parts[i].run();
    u32 ms    = diff_msec(start, gettime());
    total += ms;

    // Report a failure after the pause, with what the part found or that it failed
    if (error < 0) {
      int prefix = pause > 0 ? snprintf(note, sizeof note, "after a %s pause, ", report_duration((u32)pause)) : 0;
      if (steps_note()[0]) {
        snprintf(note + prefix, sizeof note - prefix, "%s", steps_note());
      } else {
        snprintf(note + prefix, sizeof note - prefix, "%s failed", cycle_parts[i].name);
      }
      report_step_fail(name, error, total, note);
      steps_report_link_state();
      return RESULT_FAIL;
    }

    // Leave the address part's time out, since it only reads a value back
    if (cycle_parts[i].run != step_address)
      len +=
        snprintf(note + len, sizeof note - len, "%s%s %s", len ? ", " : "", cycle_parts[i].name, report_duration(ms));
  }

  // Report the cycle with the time of each part, then the pause before them
  if (pause > 0)
    snprintf(note + len, sizeof note - len, ", after a %s pause", report_duration((u32)pause));
  report_step_ok(name, total, SLOW_CYCLE_MS, note);
  return RESULT_PASS;
}

// Bring the link up and down the configured number of times, with a random pause before each link up
static enum result test_reconnect(void)
{
  // Start from the link down, since every cycle brings it up itself
  steps_finish();

  // Seed the pauses from the clock and report the seed
  int pause_max = PAUSE_MAX_MS;
  uint32_t seed = (uint32_t)gettime();
  srand(seed);
  if (pause_max > 0)
    report_log("  seed %u, pauses up to %d ms", (unsigned)seed, pause_max);

  // Run the cycles, stopping at the first that does not pass
  for (int cycle = 1; cycle <= RECONNECTS; cycle++) {
    int pause          = pause_max > 0 ? rand() % (pause_max + 1) : 0;
    enum result result = run_reconnect_cycle(cycle, pause);
    if (result != RESULT_PASS)
      return result;
  }
  return RESULT_PASS;
}

// Tests of the Connectivity suite
static const struct test connectivity_tests[] = {
  {"Link", test_link},
  {"DNS", test_dns},
  {"TCP", test_tcp},
  {"UDP", test_udp},
};

// Tests of the Speed Test suite
static const struct test speed_tests[] = {
  {"Link", test_link},
  {"Server", test_server},
  {"Upload", test_upload},
  {"Download", test_download},
};

// Tests of the Reconnects suite
static const struct test reconnect_tests[] = {{"Reconnect", test_reconnect}};

const struct suite network_suites[] = {
  {"Connectivity", connectivity_tests, ARRAY_COUNT(connectivity_tests), false},
  {"Speed Test", speed_tests, ARRAY_COUNT(speed_tests), true},
  {"Reconnects", reconnect_tests, ARRAY_COUNT(reconnect_tests), false},
};

const size_t network_suite_count = ARRAY_COUNT(network_suites);
