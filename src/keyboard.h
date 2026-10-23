#pragma once

#include <stdbool.h>

// Character sets the keyboard offers, all of them or digits and a dot
enum keyboard_mode {
  KEYBOARD_MODE_TEXT,
  KEYBOARD_MODE_NUMERIC,
};

// Let the user edit text in place, up to capacity bytes with the terminator, returning true when they accept it
bool keyboard_edit(const char *title, char *text, int capacity, enum keyboard_mode mode);
