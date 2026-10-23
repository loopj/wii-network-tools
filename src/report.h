#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <ogc/console.h>

// The console's plain gray, which console.h does not name
#define CONSOLE_GRAY CONSOLE_ESC(37m)

// Outcome of a test or step
enum result {
  RESULT_PASS,
  RESULT_FAIL,
  RESULT_CANCEL,
  RESULT_SKIP,
};

// Find the USB Gecko in memory card slot B, which carries the log
void report_init(void);

// Print a plain line to the screen and the log
void report_line(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Write a line to the log only, for raw values that would clutter the screen
void report_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Format a duration as milliseconds below ten seconds and as seconds above, in text the next call reuses
const char *report_duration(uint32_t ms);

// Start a run, clearing the counts, and log its title
void report_run(const char *name);

// Log the start of a test
void report_test_start(const char *name);

// Log the test's result, and give it a row when it reported no steps of its own
void report_test_end(const char *name, enum result result, uint32_t ms);

// Draw the row of a step that is running, which its result replaces
void report_step_start(const char *name);

// Show how far the running step has got in its row
void report_step_progress(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Report a step that passed, with its duration and a note. A duration over slow_ms shows in yellow, 0 never does
void report_step_ok(const char *name, uint32_t ms, uint32_t slow_ms, const char *note);

// Report a step that failed, with its error, duration and a note
void report_step_fail(const char *name, int error, uint32_t ms, const char *note);

// Report a step that B cancelled, with its duration
void report_step_cancel(const char *name, uint32_t ms);

// Report a step left out, with the reason
void report_step_skip(const char *name, const char *reason);

// Print a cause line under the failed step
void report_cause(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Put the counts and the run's duration in the status line, and log them
void report_summary(bool cancelled, uint32_t ms);
