#pragma once

#include "report.h"

// Borrow the wireless driver from NCD, run the WD request tests against /dev/net/wd/command, then hand it back
enum result wd_suite_run(void);
