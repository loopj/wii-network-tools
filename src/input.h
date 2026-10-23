#pragma once

#include <stdint.h>

// Menu buttons, the same whichever controller pressed them
enum input_button {
  INPUT_UP    = 1 << 0,
  INPUT_DOWN  = 1 << 1,
  INPUT_LEFT  = 1 << 2,
  INPUT_RIGHT = 1 << 3,
  INPUT_A     = 1 << 4,
  INPUT_B     = 1 << 5,
  INPUT_HOME  = 1 << 6,
  INPUT_START = 1 << 7,
  INPUT_SHIFT = 1 << 8,
};

// Start the Wii Remote and GameCube controller drivers and the power button callbacks
void input_init(void);

// Buttons newly pressed on any controller since the last call, plus directions held long enough to repeat
uint32_t input_pressed(void);

// Wait a frame at a time until a button is pressed and return the buttons, still answering the power buttons
uint32_t input_wait_press(void);

// Wait until one of the buttons is pressed and return which of them were
uint32_t input_wait_for(uint32_t buttons);

// Shut the console down if a power button on the console or a Wii Remote has been pressed
void input_check_power_off(void);

// Name of a button on the controller pressed last, for the button hints
const char *input_button_name(enum input_button button);
