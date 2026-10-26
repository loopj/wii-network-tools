#include <stdbool.h>
#include <stdio.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "input.h"
#include "profile.h"
#include "report.h"
#include "screen.h"
#include "settings.h"
#include "steps.h"

#include "suite.h"

// Most suites the menu lists
#define SUITES_MAX 12

// Header text naming the connection the suites run on, the label in gray and the connection in white
#define ACTIVE_FORMAT CONSOLE_GRAY "Active: " CONSOLE_RESET CONSOLE_WHITE "%s" CONSOLE_RESET

// B was pressed while a test ran, kept until the next run starts
static bool cancel;

// Run each test of a suite in turn past failures, so one run reports on all of them, with B cancelling the rest
static void run_suite(const char *title, const struct suite *suite, const char *connection)
{
  // Draw the header with the suite's path and the connection, holding the hints while the rows print
  char path[64];
  char right[96];
  snprintf(path, sizeof path, "%s > %s", title, suite->name);
  snprintf(right, sizeof right, ACTIVE_FORMAT, connection);
  screen_begin(path, right);
  screen_hold_buttons("B Cancel");

  // Forget an earlier B press, reading the controllers once so a press from the menu does not count
  input_pressed();
  cancel = false;

  // Start the report and log the connection
  report_run(suite->name);
  report_log("  %s", connection);

  // Run each test, stopping at a cancel
  u64 start      = gettime();
  bool cancelled = false;
  for (size_t i = 0; i < suite->test_count && !cancelled; i++) {
    const struct test *test = &suite->tests[i];
    report_test_start(test->name);
    u64 test_start     = gettime();
    enum result result = test->run();
    report_test_end(test->name, result, diff_msec(test_start, gettime()));
    cancelled = result == RESULT_CANCEL;
  }

  // Take the link down after each suite, since every suite starts from the link down
  steps_finish();
  report_summary(cancelled, diff_msec(start, gettime()));
}

bool suite_cancelled(void)
{
  // Keep a B press until the next run clears it
  if (!cancel && (input_pressed() & INPUT_B))
    cancel = true;
  return cancel;
}

void suite_menu_open(const char *title, const struct suite *suites, size_t count)
{
  // Read the connection in the background, so the menu draws at once and the header fills in
  char connection[64] = "Reading...";
  profile_request(0);

  // List the suites
  struct screen_row rows[SUITES_MAX];
  int row_count = 0;
  for (size_t i = 0; i < count && i < SUITES_MAX; i++)
    rows[row_count++] = (struct screen_row){SCREEN_ROW_ACTION, suites[i].name, NULL};

  // Pick a suite with the D-pad, redrawing on a press and when a connection request finishes
  int cursor  = 0;
  bool redraw = true;
  while (true) {
    if (profile_poll(connection, sizeof connection))
      redraw = true;
    if (redraw) {
      char right[96];
      snprintf(right, sizeof right, ACTIVE_FORMAT, connection);
      screen_begin(title, right);
      screen_draw_rows(rows, row_count, cursor);
      screen_draw_buttons("A Run   B Back   " GLYPH_LEFT " " GLYPH_RIGHT " Connection");
      redraw = false;
    }

    // Wait a frame for a press, still answering the power buttons
    input_check_power_off();
    uint32_t buttons = input_pressed();
    if (!buttons) {
      VIDEO_WaitVSync();
      continue;
    }
    redraw = true;

    // Go back, or move the cursor
    if (buttons & INPUT_B)
      return;
    cursor = screen_move_cursor(rows, row_count, cursor, buttons);

    // Move NCD's working copy to another connection on left and right, without writing NAND
    if (buttons & (INPUT_LEFT | INPUT_RIGHT))
      profile_request(buttons & INPUT_RIGHT ? 1 : -1);
    if (!(buttons & INPUT_A))
      continue;

    // Run the suite, except while a switch runs so a run reports the connection it used, or without a server it needs
    const struct suite *suite = &suites[cursor];
    if (profile_busy())
      continue;
    if (suite->needs_server && !iperf_server[0]) {
      screen_set_status(SCREEN_STATUS_ERROR, "%s needs an iperf3 server. Set one in Options.", suite->name);
      continue;
    }

    run_suite(title, suite, connection);

    // Keep the results on screen until A
    screen_draw_buttons("A Back");
    input_wait_for(INPUT_A);
    profile_request(0);
  }
}
