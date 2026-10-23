#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "report.h"

// One test, a named group of steps
struct test {
  // Label in the report
  const char *name;

  // Runs the steps and reports each one
  enum result (*run)(void);
};

// One menu entry and the tests it runs in order
struct suite {
  // Label in the menu and the run title
  const char *name;

  // Tests to run
  const struct test *tests;

  // Number of entries in tests
  size_t test_count;

  // The suite needs the iperf3 server
  bool needs_server;
};

// B has been pressed since the run started, checked by a running test between steps
bool suite_cancelled(void);

// Show a menu of suites to run on the connection left and right pick, until the user goes back
void suite_menu_open(const char *title, const struct suite *suites, size_t count);
