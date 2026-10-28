#pragma once

#include <stdbool.h>
#include <stdint.h>

// Left arrow from the console font, code page 437
#define GLYPH_LEFT "\x11"

// Right arrow from the console font, code page 437
#define GLYPH_RIGHT "\x10"

// Kinds of status message, which pick its color
enum screen_status {
  SCREEN_STATUS_INFO,
  SCREEN_STATUS_OK,
  SCREEN_STATUS_ERROR,
};

// Kinds of list row
enum screen_row_kind {
  SCREEN_ROW_ACTION,
  SCREEN_ROW_CHOICE,
  SCREEN_ROW_TEXT,
  SCREEN_ROW_GAP,
};

// One row of a list
struct screen_row {
  enum screen_row_kind kind;
  const char *label;
  const char *value;
};

// Clear the screen and draw the header, with the path to the screen on the left and text on the right, gray unless it
// sets its own colors
void screen_begin(const char *path, const char *right);

// Draw the rows of a list, with the row under the cursor in cyan
void screen_draw_rows(const struct screen_row *rows, int count, int cursor);

// Draw a gray line of reference information just above the status line
void screen_draw_info(const char *label, const char *value);

// Move a list cursor on up and down in the buttons pressed, past gaps, wrapping at the ends
int screen_move_cursor(const struct screen_row *rows, int count, int cursor, uint32_t buttons);

// Draw the status line and the button hints at the bottom of the screen, then clear the status
void screen_draw_buttons(const char *text);

// Draw the button hints for the row under the cursor, with the verbs A does on an action row and B does
void screen_draw_row_buttons(const struct screen_row *row, const char *action, const char *back);

// Draw the button hints and return to the top of the body, for a body that prints as it runs
void screen_hold_buttons(const char *text);

// Show a question in the body and return true when the user answers A with the verb, false on B
bool screen_confirm(const char *path, const char *question, const char *verb);

// Set the message the status line shows the next time the bottom of a screen is drawn
void screen_set_status(enum screen_status kind, const char *format, ...) __attribute__((format(printf, 2, 3)));
