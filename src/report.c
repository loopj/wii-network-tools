#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/usbgecko.h>

#include "screen.h"

#include "report.h"

// Longest line, on the screen or in the log
#define LINE_MAX_LEN 256

// EXI channel of memory card slot B, where the USB Gecko sits
#define GECKO_CHANNEL EXI_CHANNEL_1

// Attempts to send one line before giving up on it, so a Gecko nobody reads cannot stall a test
#define GECKO_RETRIES 100000

// Check mark from the console font, code page 437
#define GLYPH_PASS "\xfb"

// Mark for a failed step
#define GLYPH_FAIL "x"

// Mark for a cancelled step
#define GLYPH_CANCEL "-"

// Middle dot from the console font, for a step running or left out
#define GLYPH_DOT "\xfa"

// Width of a row's name, so the times line up
#define NAME_WIDTH 11

// Width of a row's time
#define TIME_WIDTH 6

// Column a row's mark starts in, which lines the rows up with the header and footer
#define MARK_COLUMN 3

// Column a row's note starts in, after the mark, the name and the time
#define NOTE_COLUMN (MARK_COLUMN + 1 + 2 + NAME_WIDTH + 1 + TIME_WIDTH + 3)

// Words for the results in the log
static const char *const result_words[] = {
  [RESULT_PASS]   = "PASS",
  [RESULT_FAIL]   = "FAIL",
  [RESULT_CANCEL] = "CANCEL",
  [RESULT_SKIP]   = "SKIP",
};

// A USB Gecko answered at startup
static bool gecko_present;

// Rows that passed in this run
static int rows_passed;

// Rows that failed in this run
static int rows_failed;

// Rows left out in this run
static int rows_skipped;

// Rows the current test drew
static int rows_in_test;

// The running step's row is on screen with no line end yet
static bool row_open;

// The starting line is on screen, waiting for the first output to take its place
static bool starting_shown;

// Name of the running step, for its progress
static char running_name[24];

// The text report_duration returns
static char duration_text[16];

// Send text to the USB Gecko, if there is one
static void write_log(const char *text)
{
  if (gecko_present)
    usb_sendbuffer_safe_ex(GECKO_CHANNEL, text, strlen(text), GECKO_RETRIES);
}

// Format a line and send it to the log
static void print_log(const char *format, ...)
{
  char text[LINE_MAX_LEN];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof text, format, args);
  va_end(args);
  write_log(text);
}

// Print to the screen, first ending an open row and making room at the bottom
static void print_screen(const char *format, ...)
{
  char text[LINE_MAX_LEN];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof text, format, args);
  va_end(args);

  // Clear the starting line, so the first output takes its place
  if (starting_shown) {
    starting_shown = false;
    fputs("\r\x1b[2K", stdout);
  }

  if (row_open) {
    row_open = false;
    fputs("\n", stdout);
  }
  fputs(text, stdout);
}

// Count the characters of text that fit in width, ending at a line break or the last space when it runs over
static int fit_note(const char *text, int width)
{
  int len = (int)strcspn(text, "\n");
  if (len <= width)
    return len;
  for (int i = width; i > 0; i--) {
    if (text[i] == ' ')
      return i;
  }
  return width;
}

// Print text from a column on lines of its own, wrapping at spaces
static void print_lines(int column, const char *color, const char *text)
{
  int columns, rows;
  CON_GetMetrics(&columns, &rows);
  int width = columns - 1 - column;
  while (*text) {
    while (*text == ' ' || *text == '\n')
      text++;
    if (!*text)
      break;
    int len = fit_note(text, width);
    print_screen("%*s%s%.*s" CONSOLE_RESET "\n", column, "", color, len, text);
    text += len;
  }
}

// Draw a row with its mark, name, time and note, replacing the running row when it is open, and leaving it open
// for a running step
static void print_row(const char *mark_color, const char *mark, const char *name, const char *time_color,
                      const char *time, const char *note_color, const char *note, bool open)
{
  // Return to the start of the running row, so this one covers it
  if (row_open) {
    row_open = false;
    fputs("\r", stdout);
  }

  // Draw the first line, padded to cover a longer row under it
  int columns, rows;
  CON_GetMetrics(&columns, &rows);
  int width = columns - 1 - NOTE_COLUMN;
  int len   = fit_note(note, width);
  print_screen("%*s%s%s" CONSOLE_RESET "  %s%-*s" CONSOLE_RESET " %s%*s" CONSOLE_RESET "   %s%.*s" CONSOLE_RESET "%*s",
               MARK_COLUMN, "", mark_color, mark, open ? CONSOLE_WHITE : "", NAME_WIDTH, name, time_color, TIME_WIDTH,
               time, note_color, len, note, width - len, "");
  if (open) {
    row_open = true;
    return;
  }

  // End the row and wrap the rest of the note under it
  print_screen("\n");
  print_lines(NOTE_COLUMN, note_color, note + len);
  rows_in_test++;
}

// Name an error the app's own code returns, NULL for one that has no name
static const char *describe_error(int error)
{
  switch (error) {
    case -ETIMEDOUT:
      // A wait ran out
      return "timed out";

    case -ECONNREFUSED:
      // The server turned the connection away
      return "refused";

    case -ECONNRESET:
      // The server dropped the connection
      return "reset";

    case -EHOSTUNREACH:
      // No route to the server
      return "unreachable";

    case -EBUSY:
      // The server is running another test
      return "busy";

    default:
      // Any other error shows as its number
      return NULL;
  }
}

void report_init(void)
{
  // Log to the USB Gecko only when one answers, so the app runs on the screen alone without it
  gecko_present = usb_isgeckoalive(GECKO_CHANNEL);
}

void report_line(const char *format, ...)
{
  char text[LINE_MAX_LEN];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof text, format, args);
  va_end(args);

  print_lines(MARK_COLUMN, "", text);
  write_log(text);
}

void report_log(const char *format, ...)
{
  char text[LINE_MAX_LEN];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof text, format, args);
  va_end(args);

  write_log(text);
  write_log("\n");
}

const char *report_duration(uint32_t ms)
{
  if (ms < 10000) {
    snprintf(duration_text, sizeof duration_text, "%ums", (unsigned)ms);
  } else {
    snprintf(duration_text, sizeof duration_text, "%u.%us", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
  }
  return duration_text;
}

void report_run(const char *name)
{
  // Clear the counts and log the title
  rows_passed  = 0;
  rows_failed  = 0;
  rows_skipped = 0;
  row_open     = false;
  print_log("\nRUN  %s\n", name);

  // Say the run is starting until a test prints
  print_screen("%*s" CONSOLE_GRAY "Starting tests..." CONSOLE_RESET, MARK_COLUMN, "");
  starting_shown = true;
}

void report_test_start(const char *name)
{
  // Clear the test's row count and log its name
  rows_in_test = 0;
  print_log("%s\n", name);
}

void report_test_end(const char *name, enum result result, uint32_t ms)
{
  print_log("%s %s %s\n", result_words[result], name, report_duration(ms));

  // Give a test that reported no steps a row of its own
  if (rows_in_test > 0)
    return;
  switch (result) {
    case RESULT_PASS:
      // Show it as a step that passed
      report_step_ok(name, ms, 0, "");
      break;

    case RESULT_FAIL:
      // Leave the error out, since the test's own lines above say what failed
      rows_failed++;
      print_row(CONSOLE_RED, GLYPH_FAIL, name, CONSOLE_GRAY, report_duration(ms), CONSOLE_RED, "failed", false);
      break;

    case RESULT_CANCEL:
      // Show it as a step that B cancelled
      report_step_cancel(name, ms);
      break;

    case RESULT_SKIP:
      // Show it as a step left out
      report_step_skip(name, "skipped");
      break;
  }
}

void report_step_start(const char *name)
{
  // Keep the name for the progress and draw the open row
  snprintf(running_name, sizeof running_name, "%s", name);
  print_row(CONSOLE_WHITE, GLYPH_DOT, running_name, "", "", "", "", true);
}

void report_step_progress(const char *format, ...)
{
  char text[LINE_MAX_LEN];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof text, format, args);
  va_end(args);

  // Redraw the running row with the progress, only while nothing has printed under it
  if (row_open)
    print_row(CONSOLE_WHITE, GLYPH_DOT, running_name, "", "", CONSOLE_GRAY, text, true);
}

void report_step_ok(const char *name, uint32_t ms, uint32_t slow_ms, const char *note)
{
  rows_passed++;

  // Show the time in yellow when it runs over slow_ms
  const char *time_color = slow_ms && ms > slow_ms ? CONSOLE_YELLOW : CONSOLE_GRAY;
  print_row(CONSOLE_GREEN, GLYPH_PASS, name, time_color, report_duration(ms), CONSOLE_GRAY, note, false);
  print_log("  ok   %s %s  %s\n", name, report_duration(ms), note);
}

void report_step_fail(const char *name, int error, uint32_t ms, const char *note)
{
  rows_failed++;

  // Say what failed, adding the error unless it is the bare -1 a note already explains
  char text[LINE_MAX_LEN];
  const char *word = describe_error(error);
  if (error == -1) {
    snprintf(text, sizeof text, "%s", note[0] ? note : "failed");
  } else if (word) {
    snprintf(text, sizeof text, note[0] ? "%s (%s)" : "%s%s", note, word);
  } else {
    snprintf(text, sizeof text, note[0] ? "%s (error %d)" : "%serror %d", note, error);
  }

  // Show the row and log the raw error
  print_row(CONSOLE_RED, GLYPH_FAIL, name, CONSOLE_GRAY, report_duration(ms), CONSOLE_RED, text, false);
  print_log("  FAIL %s %s  error %d  %s\n", name, report_duration(ms), error, note);
}

void report_step_cancel(const char *name, uint32_t ms)
{
  print_row(CONSOLE_YELLOW, GLYPH_CANCEL, name, CONSOLE_GRAY, report_duration(ms), CONSOLE_YELLOW, "cancelled", false);
  print_log("  CANCEL %s %s\n", name, report_duration(ms));
}

void report_step_skip(const char *name, const char *reason)
{
  rows_skipped++;
  print_row(CONSOLE_GRAY, GLYPH_DOT, name, "", "", CONSOLE_GRAY, reason, false);
  print_log("  skip %s  %s\n", name, reason);
}

void report_cause(const char *format, ...)
{
  char text[LINE_MAX_LEN];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof text, format, args);
  va_end(args);

  print_lines(NOTE_COLUMN, CONSOLE_RED, text);
  print_log("       -> %s\n", text);
}

void report_summary(bool cancelled, uint32_t ms)
{
  // List the counts that are not zero, then the duration
  char text[160];
  int len = snprintf(text, sizeof text, CONSOLE_GREEN "%d passed" CONSOLE_RESET, rows_passed);
  if (rows_failed)
    len += snprintf(text + len, sizeof text - len, "  " CONSOLE_RED "%d failed" CONSOLE_RESET, rows_failed);
  if (rows_skipped)
    len += snprintf(text + len, sizeof text - len, "  " CONSOLE_GRAY "%d skipped" CONSOLE_RESET, rows_skipped);
  if (cancelled)
    len += snprintf(text + len, sizeof text - len, "  " CONSOLE_YELLOW "cancelled" CONSOLE_RESET);
  snprintf(text + len, sizeof text - len, "  " CONSOLE_GRAY "%s" CONSOLE_RESET, report_duration(ms));
  screen_set_status(SCREEN_STATUS_INFO, "%s", text);

  print_log("\n%d passed, %d failed, %d skipped%s\nDuration %s\n", rows_passed, rows_failed, rows_skipped,
            cancelled ? ", cancelled" : "", report_duration(ms));
}
