#pragma once

#include <stdbool.h>
#include <stddef.h>

// Read NCD's selected connection in the background, first moving it by step connections without writing NAND
void profile_request(int step);

// Copy the latest description of the selected connection into text. Returns true when it changed since the last call
bool profile_poll(char *text, size_t len);

// Returns true while a request is running
bool profile_busy(void);
