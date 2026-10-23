#pragma once

#include <stddef.h>

#include "suite.h"

// Suites of the Advanced menu, in menu order
extern const struct suite advanced_suites[];

// Number of entries in advanced_suites
extern const size_t advanced_suite_count;

// List the failure lines of the Unity run that just ended, then forget them
void advanced_report_failures(void);
