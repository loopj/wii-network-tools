#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>

#include "input.h"
#include "report.h"
#include "screen.h"

#include "keyboard.h"

// Most character rows a layout has
#define ROWS_MAX 4

// Number of entries in an array
#define ARRAY_COUNT(array) (int)(sizeof(array) / sizeof((array)[0]))

// Keys on the bottom row
enum special_key {
  KEY_SHIFT,
  KEY_SPACE,
  KEY_BACKSPACE,
  KEY_DONE,
  KEY_CANCEL,
};

// Character rows over a bottom row of special keys
struct layout {
  const char *const *rows;
  int row_count;
  const enum special_key *special;
  int special_count;
};

// Character rows of the text layout, unshifted and shifted
static const char *const text_rows[2][ROWS_MAX] = {
  {"1234567890-=", "qwertyuiop[]", "asdfghjkl;'\\", "zxcvbnm,./`"},
  {"!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"|", "ZXCVBNM<>?~"},
};

// Character row of the numeric layout
static const char *const numeric_rows[] = {"0123456789."};

// Names of the special keys
static const char *const special_names[] = {"Shift", "Space", "Backspace", "Done", "Cancel"};

// Bottom row of the text layout
static const enum special_key text_special[] = {KEY_SHIFT, KEY_SPACE, KEY_BACKSPACE, KEY_DONE, KEY_CANCEL};

// Bottom row of the numeric layout
static const enum special_key numeric_special[] = {KEY_BACKSPACE, KEY_DONE, KEY_CANCEL};

// Pick the layout for a mode and shift state
static struct layout get_layout(enum keyboard_mode mode, bool shift)
{
  if (mode == KEYBOARD_MODE_NUMERIC)
    return (struct layout){numeric_rows, ARRAY_COUNT(numeric_rows), numeric_special, ARRAY_COUNT(numeric_special)};
  return (struct layout){text_rows[shift ? 1 : 0], ROWS_MAX, text_special, ARRAY_COUNT(text_special)};
}

// Count the keys on a row, the bottom row included
static int count_keys(const struct layout *layout, int row)
{
  return row < layout->row_count ? (int)strlen(layout->rows[row]) : layout->special_count;
}

// Draw the text being edited and the keys, with the selected key in brackets
static void draw_keyboard(const char *title, const char *text, int capacity, const struct layout *layout, int row,
                          int column)
{
  int columns, rows;
  CON_GetMetrics(&columns, &rows);

  // Right align the length against the most the text holds
  char count[24];
  snprintf(count, sizeof count, "%d/%d", (int)strlen(text), capacity - 1);
  int padding = columns - 2 - 3 - (int)strlen(text) - 1 - (int)strlen(count);

  // Show the text with a cursor after it
  screen_begin(title, "");
  printf("   %s_%*s" CONSOLE_GRAY "%s" CONSOLE_RESET "\n\n", text, padding > 1 ? padding : 1, "", count);

  // Draw the character rows
  for (int r = 0; r < layout->row_count; r++) {
    printf("  ");
    for (int c = 0; layout->rows[r][c] != 0; c++)
      printf(r == row && c == column ? "[%c]" : " %c ", layout->rows[r][c]);
    printf("\n");
  }

  // Draw the bottom row the same way
  printf("\n  ");
  for (int c = 0; c < layout->special_count; c++)
    printf(row == layout->row_count && c == column ? "[%s]" : " %s ", special_names[layout->special[c]]);
  printf("\n");

  // Name the buttons of the controller in use
  char hints[80];
  snprintf(hints, sizeof hints, "A Type   B Delete   %s Done   %s Shift   %s Cancel", input_button_name(INPUT_START),
           input_button_name(INPUT_SHIFT), input_button_name(INPUT_HOME));
  screen_draw_buttons(hints);
}

// Add a character to the text when there is room for it
static void append_char(char *text, int capacity, char character)
{
  int len = (int)strlen(text);
  if (len + 1 < capacity) {
    text[len]     = character;
    text[len + 1] = 0;
  }
}

// Remove the text's last character
static void delete_last_char(char *text)
{
  int len = (int)strlen(text);
  if (len > 0)
    text[len - 1] = 0;
}

bool keyboard_edit(const char *title, char *text, int capacity, enum keyboard_mode mode)
{
  bool shift = false;
  int row    = 0;
  int column = 0;
  while (true) {
    // Draw the keyboard for the current shift state
    struct layout layout = get_layout(mode, shift);
    draw_keyboard(title, text, capacity, &layout, row, column);
    uint32_t buttons = input_wait_press();

    // Move the cursor, wrapping at the edges and keeping it on a key after a vertical move
    if (buttons & INPUT_UP)
      row = row > 0 ? row - 1 : layout.row_count;
    if (buttons & INPUT_DOWN)
      row = row < layout.row_count ? row + 1 : 0;
    if (buttons & INPUT_LEFT)
      column = column > 0 ? column - 1 : count_keys(&layout, row) - 1;
    if (buttons & INPUT_RIGHT)
      column = column < count_keys(&layout, row) - 1 ? column + 1 : 0;
    if (column >= count_keys(&layout, row))
      column = count_keys(&layout, row) - 1;

    // Handle the buttons that act without selecting a key
    if (buttons & INPUT_B)
      delete_last_char(text);
    if (buttons & INPUT_SHIFT)
      shift = !shift;
    if (buttons & INPUT_START)
      return true;
    if (buttons & INPUT_HOME)
      return false;
    if (!(buttons & INPUT_A))
      continue;

    // Type the selected character
    if (row < layout.row_count) {
      append_char(text, capacity, layout.rows[row][column]);
      continue;
    }

    // Or act on the selected bottom row key
    switch (layout.special[column]) {
      case KEY_SHIFT:
        // Switch between the unshifted and shifted rows
        shift = !shift;
        break;

      case KEY_SPACE:
        // Type a space
        append_char(text, capacity, ' ');
        break;

      case KEY_BACKSPACE:
        // Remove the last character
        delete_last_char(text);
        break;

      case KEY_DONE:
        // Accept the text
        return true;

      case KEY_CANCEL:
        // Throw the edit away
        return false;
    }
  }
}
