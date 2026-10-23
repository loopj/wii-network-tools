#include <stdbool.h>
#include <string.h>

#include "report.h"
#include "so_suite.h"
#include "suite.h"
#include "unity_config.h"
#include "wd_suite.h"

#include "advanced.h"

// Number of entries in an array
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

// Failure lines kept for the list after Unity's summary
#define FAILURES_KEPT 16

// Tests of the SO Test Suite
static const struct test so_tests[] = {{"SO requests", so_suite_run}};

// Tests of the WD Test Suite
static const struct test wd_tests[] = {{"WD requests", wd_suite_run}};

// Unity's characters gathered into a line for the report
static char line[160];

// Characters in line
static size_t line_len;

// Failure lines of the run, listed again after Unity's summary since the screen scrolls past them
static char failures_kept[FAILURES_KEPT][sizeof line];

// Entries used in failures_kept
static int failures_kept_count;

const struct suite advanced_suites[] = {
  {"Sockets (SO)", so_tests, ARRAY_COUNT(so_tests), false},
  {"Wi-Fi Driver (WD)", wd_tests, ARRAY_COUNT(wd_tests), false},
};

const size_t advanced_suite_count = ARRAY_COUNT(advanced_suites);

void unity_output_char(int c)
{
  // Report a line at its end or when it fills, keeping it when it is a failure
  if (c == '\n' || line_len == sizeof line - 1) {
    line[line_len] = 0;
    report_line("%s\n", line);
    if (strstr(line, ":FAIL") && failures_kept_count < FAILURES_KEPT)
      memcpy(failures_kept[failures_kept_count++], line, sizeof line);
    line_len = 0;
    return;
  }

  // Add the character to the line
  line[line_len++] = (char)c;
}

void advanced_report_failures(void)
{
  // List the kept failures under a heading
  if (failures_kept_count)
    report_line("Failed\n");
  for (int i = 0; i < failures_kept_count; i++)
    report_line("  %s\n", failures_kept[i]);

  // Forget them before the next run
  failures_kept_count = 0;
}
