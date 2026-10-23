#include <stdbool.h>
#include <stdint.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "input.h"

// Time a direction is held before it starts repeating, in milliseconds
#define REPEAT_DELAY_MS 333

// Time between repeats of a held direction, in milliseconds
#define REPEAT_INTERVAL_MS 67

// GameCube stick deflection that counts as a direction
#define STICK_THRESHOLD 40

// Buttons that repeat while held
#define DIRECTIONS (INPUT_UP | INPUT_DOWN | INPUT_LEFT | INPUT_RIGHT)

// Set by the power buttons and read from the main flow, since the callbacks run in interrupts
static volatile bool power_pressed;

// Buttons held at the last read
static uint32_t previous_held;

// When the held directions next repeat, in ticks
static u64 next_repeat;

// The last press came from a GameCube controller
static bool gamecube_last;

// Note a press of the console's power button
static void handle_power_button(void)
{
  power_pressed = true;
}

// Note a press of a Wii Remote's power button
static void handle_wpad_power_button(s32 channel)
{
  (void)channel;
  power_pressed = true;
}

// Map Wii Remote buttons onto the menu buttons
static uint32_t map_wpad_buttons(u32 wpad)
{
  uint32_t buttons = 0;
  if (wpad & WPAD_BUTTON_UP)
    buttons |= INPUT_UP;
  if (wpad & WPAD_BUTTON_DOWN)
    buttons |= INPUT_DOWN;
  if (wpad & WPAD_BUTTON_LEFT)
    buttons |= INPUT_LEFT;
  if (wpad & WPAD_BUTTON_RIGHT)
    buttons |= INPUT_RIGHT;
  if (wpad & WPAD_BUTTON_A)
    buttons |= INPUT_A;
  if (wpad & WPAD_BUTTON_B)
    buttons |= INPUT_B;
  if (wpad & WPAD_BUTTON_HOME)
    buttons |= INPUT_HOME;
  if (wpad & WPAD_BUTTON_PLUS)
    buttons |= INPUT_START;
  if (wpad & WPAD_BUTTON_MINUS)
    buttons |= INPUT_SHIFT;

  return buttons;
}

// Map GameCube controller buttons onto the menu buttons, with the main stick doubling as the D-pad
static uint32_t map_pad_buttons(u16 pad, s8 x, s8 y)
{
  uint32_t buttons = 0;
  if ((pad & PAD_BUTTON_UP) || y > STICK_THRESHOLD)
    buttons |= INPUT_UP;
  if ((pad & PAD_BUTTON_DOWN) || y < -STICK_THRESHOLD)
    buttons |= INPUT_DOWN;
  if ((pad & PAD_BUTTON_LEFT) || x < -STICK_THRESHOLD)
    buttons |= INPUT_LEFT;
  if ((pad & PAD_BUTTON_RIGHT) || x > STICK_THRESHOLD)
    buttons |= INPUT_RIGHT;
  if (pad & PAD_BUTTON_A)
    buttons |= INPUT_A;
  if (pad & PAD_BUTTON_B)
    buttons |= INPUT_B;
  if (pad & PAD_TRIGGER_Z)
    buttons |= INPUT_HOME;
  if (pad & PAD_BUTTON_START)
    buttons |= INPUT_START;
  if (pad & PAD_BUTTON_Y)
    buttons |= INPUT_SHIFT;

  return buttons;
}

void input_init(void)
{
  // Start both controller drivers and take both power buttons
  WPAD_Init();
  PAD_Init();
  SYS_SetPowerCallback(handle_power_button);
  WPAD_SetPowerButtonCallback(handle_wpad_power_button);
}

uint32_t input_pressed(void)
{
  // Gather what every controller holds, since a remote that reconnects may not come back as player 1
  WPAD_ScanPads();
  PAD_ScanPads();
  uint32_t wpad_held = 0;
  uint32_t pad_held  = 0;
  for (int channel = 0; channel < WPAD_MAX_WIIMOTES; channel++)
    wpad_held |= map_wpad_buttons(WPAD_ButtonsHeld(channel));
  for (int port = 0; port < PAD_CHANMAX; port++)
    pad_held |= map_pad_buttons(PAD_ButtonsHeld(port), PAD_StickX(port), PAD_StickY(port));
  uint32_t held = wpad_held | pad_held;

  // Count the buttons that went down as presses
  uint32_t pressed = held & ~previous_held;

  // Note which controller pressed them, for the button hints
  if (pad_held & pressed) {
    gamecube_last = true;
  } else if (wpad_held & pressed) {
    gamecube_last = false;
  }

  // Restart the delay when the held directions change, and repeat them once it runs out
  u64 now = gettime();
  if ((held & DIRECTIONS) != (previous_held & DIRECTIONS)) {
    next_repeat = now + millisecs_to_ticks(REPEAT_DELAY_MS);
  } else if ((held & DIRECTIONS) && now >= next_repeat) {
    pressed |= held & DIRECTIONS;
    next_repeat = now + millisecs_to_ticks(REPEAT_INTERVAL_MS);
  }

  previous_held = held;
  return pressed;
}

uint32_t input_wait_press(void)
{
  // Read the controllers once a frame until a button goes down
  uint32_t pressed;
  do {
    VIDEO_WaitVSync();
    input_check_power_off();
    pressed = input_pressed();
  } while (pressed == 0);

  return pressed;
}

uint32_t input_wait_for(uint32_t buttons)
{
  uint32_t pressed;
  while (!(pressed = input_wait_press() & buttons))
    ;
  return pressed;
}

void input_check_power_off(void)
{
  if (power_pressed)
    SYS_ResetSystem(SYS_POWEROFF, 0, 0);
}

const char *input_button_name(enum input_button button)
{
  switch (button) {
    case INPUT_HOME:
      // HOME on the Wii Remote, Z on the GameCube controller
      return gamecube_last ? "Z" : "HOME";

    case INPUT_START:
      // Plus on the Wii Remote, START on the GameCube controller
      return gamecube_last ? "START" : "+";

    case INPUT_SHIFT:
      // Minus on the Wii Remote, Y on the GameCube controller
      return gamecube_last ? "Y" : "-";

    default:
      // Leave out the rest, which both controllers name the same
      return "";
  }
}
