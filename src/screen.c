#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>

#include "input.h"
#include "report.h"

#include "screen.h"

// Width of a row's label, so the values line up
#define LABEL_WIDTH 24

// Text between the screens of a path
#define PATH_SEPARATOR " > "

// Horizontal line from the console font, code page 437
#define GLYPH_RULE "\xc4"

// Rows the status line, the rule and the button hints take at the bottom
#define BOTTOM_ROWS 3

// Colors of the status kinds
static const char *const status_colors[] = {
  [SCREEN_STATUS_INFO]  = CONSOLE_WHITE,
  [SCREEN_STATUS_OK]    = CONSOLE_GREEN,
  [SCREEN_STATUS_ERROR] = CONSOLE_RED,
};

// The message the status line shows next, empty for none
static char status_text[160];

// The kind of that message
static enum screen_status status_kind;

// Row the body starts on, below the header
static int body_row;

// Button hints are held at the bottom while the body prints above them
static bool buttons_held;

// Move the cursor to the start of a row
static void move_to_row(int target)
{
  int column, row;
  CON_GetPosition(&column, &row);
  if (target > row) {
    printf("\x1b[%dB", target - row);
  } else if (target < row) {
    printf("\x1b[%dA", row - target);
  }
  printf("\r");
}

// Draw a gray rule across the screen and end the row
static void draw_rule(void)
{
  int columns, rows;
  CON_GetMetrics(&columns, &rows);

  // Stop short of the last column, so the console does not wrap
  printf(CONSOLE_GRAY " ");
  for (int i = 0; i < columns - 2; i++)
    printf(GLYPH_RULE);
  printf(CONSOLE_RESET "\n");
}

// Count the characters text shows, leaving out the escape sequences that set colors
static int count_visible(const char *text)
{
  int count = 0;
  for (; *text; text++) {
    if (*text == '\x1b') {
      while (text[1] && text[1] != 'm')
        text++;
      if (text[1])
        text++;
    } else {
      count++;
    }
  }
  return count;
}

// The row can take the cursor
static bool is_selectable(const struct screen_row *row)
{
  return row->kind != SCREEN_ROW_GAP;
}

void screen_begin(const char *path, const char *right)
{
  int columns, rows, column;
  CON_GetMetrics(&columns, &rows);
  buttons_held = false;

  // Clear the screen and move to its top row, below the overscan
  printf("\x1b[2J\x1b[2;0H");

  // Draw the parents in gray and the screen itself in white
  const char *name = path;
  for (const char *next = strstr(path, PATH_SEPARATOR); next; next = strstr(next + 1, PATH_SEPARATOR))
    name = next + strlen(PATH_SEPARATOR);
  printf(" " CONSOLE_GRAY "%.*s" CONSOLE_RESET CONSOLE_WHITE "%s" CONSOLE_RESET, (int)(name - path), path, name);

  // Right align the text on the right, keeping the last column clear
  int padding = columns - 2 - (int)strlen(path) - count_visible(right);
  printf("%*s" CONSOLE_GRAY "%s" CONSOLE_RESET "\n", padding > 1 ? padding : 1, "", right);

  // Rule off the header and leave a blank row above the body
  draw_rule();
  printf("\n");
  CON_GetPosition(&column, &body_row);
}

void screen_draw_rows(const struct screen_row *rows, int count, int cursor)
{
  for (int i = 0; i < count; i++) {
    const struct screen_row *row = &rows[i];
    const char *color            = i == cursor ? CONSOLE_CYAN : "";
    const char *marker           = i == cursor ? ">" : " ";
    switch (row->kind) {
      case SCREEN_ROW_GAP:
        // Leave the row blank
        printf("\n");
        break;

      case SCREEN_ROW_CHOICE:
        // Put the arrows that step it around the value
        printf("%s %s %-*s " GLYPH_LEFT " %s " GLYPH_RIGHT CONSOLE_RESET "\n", color, marker, LABEL_WIDTH, row->label,
               row->value);
        break;

      default:
        // Show the label and any value
        printf("%s %s %-*s %s" CONSOLE_RESET "\n", color, marker, LABEL_WIDTH, row->label,
               row->value ? row->value : "");
        break;
    }
  }
}

void screen_draw_info(const char *label, const char *value)
{
  int columns, rows;
  CON_GetMetrics(&columns, &rows);

  // Draw it on the row above the status line, lined up with the rows
  move_to_row(rows - BOTTOM_ROWS - 1);
  printf(CONSOLE_GRAY "   %-*s %s" CONSOLE_RESET, LABEL_WIDTH, label, value);
}

int screen_move_cursor(const struct screen_row *rows, int count, int cursor, uint32_t buttons)
{
  // Step one row at a time, wrapping, until the cursor lands on a row that takes it
  int step = buttons & INPUT_UP ? -1 : buttons & INPUT_DOWN ? 1 : 0;
  if (step == 0)
    return cursor;
  cursor = (cursor + step + count) % count;
  while (!is_selectable(&rows[cursor]))
    cursor = (cursor + step + count) % count;
  return cursor;
}

void screen_draw_buttons(const char *text)
{
  int columns, rows, column, row;
  CON_GetMetrics(&columns, &rows);
  CON_GetPosition(&column, &row);
  buttons_held = false;

  // Scroll the body up when it reaches into the bottom rows
  int overlap = row - (rows - BOTTOM_ROWS);
  if (overlap > 0) {
    move_to_row(rows - 1);
    for (int i = 0; i < overlap; i++)
      printf("\n");
  }

  // Draw the status message once, in its color
  move_to_row(rows - BOTTOM_ROWS);
  printf(" %s%s" CONSOLE_RESET, status_colors[status_kind], status_text);
  status_text[0] = 0;

  // Rule off the hints and draw them on the last row
  move_to_row(rows - 2);
  draw_rule();
  printf(CONSOLE_GRAY " %s" CONSOLE_RESET, text);
}

void screen_draw_row_buttons(const struct screen_row *row, const char *action, const char *back)
{
  char hints[80];
  switch (row->kind) {
    case SCREEN_ROW_CHOICE:
      // Step the value with left and right
      snprintf(hints, sizeof hints, "B %s   " GLYPH_LEFT " " GLYPH_RIGHT " Change", back);
      break;

    case SCREEN_ROW_TEXT:
      // Edit the value on the keyboard
      snprintf(hints, sizeof hints, "A Edit   B %s", back);
      break;

    default:
      // Act on the row
      snprintf(hints, sizeof hints, "A %s   B %s", action, back);
      break;
  }
  screen_draw_buttons(hints);
}

void screen_hold_buttons(const char *text)
{
  screen_draw_buttons(text);
  move_to_row(body_row);
  buttons_held = true;
}

void screen_make_room(void)
{
  int columns, rows, column, row;
  CON_GetMetrics(&columns, &rows);
  CON_GetPosition(&column, &row);
  if (!buttons_held || row < rows - BOTTOM_ROWS)
    return;

  // Blank the body, leaving the last column alone so the console does not scroll, and start again at its top
  for (int i = body_row; i < rows - BOTTOM_ROWS; i++) {
    move_to_row(i);
    printf("%*s", columns - 1, "");
  }
  move_to_row(body_row);
}

bool screen_confirm(const char *path, const char *question, const char *verb)
{
  // Ask the question in the body
  screen_begin(path, "");
  printf("   %s\n", question);

  // Wait for A or B
  char hints[48];
  snprintf(hints, sizeof hints, "A %s   B Cancel", verb);
  screen_draw_buttons(hints);
  return input_wait_for(INPUT_A | INPUT_B) & INPUT_A;
}

void screen_set_status(enum screen_status kind, const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vsnprintf(status_text, sizeof status_text, format, args);
  va_end(args);
  status_kind = kind;
}
