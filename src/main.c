#include <stdbool.h>
#include <string.h>

#include <gccore.h>

#include "advanced.h"
#include "connections.h"
#include "input.h"
#include "network_tests.h"
#include "report.h"
#include "screen.h"
#include "settings.h"
#include "suite.h"

// Device that only Dolphin has
#define DOLPHIN_DEVICE "/dev/dolphin"

// Address of the STUBHAXX signature in a loader's reload stub, which libogc's exit jumps to
#define LOADER_STUB_SIGNATURE 0x80001804

// Rows of the top menu, in screen order
enum entry {
  ENTRY_CONNECTIONS,
  ENTRY_NETWORK_TESTS,
  ENTRY_IOS_MODULE_TESTS,
  ENTRY_OPTIONS,
  ENTRY_EXIT,
  ENTRY_COUNT,
};

// The top menu's rows
static const struct screen_row entry_rows[ENTRY_COUNT] = {
  {SCREEN_ROW_ACTION, "Connection Settings", NULL},
  {SCREEN_ROW_ACTION, "Network Tests", NULL},
  {SCREEN_ROW_ACTION, "IOS Module Tests", NULL},
  {SCREEN_ROW_ACTION, "Options", NULL},
  {SCREEN_ROW_ACTION, "Exit", NULL},
};

// Set up the video mode and a text console on the screen
static void init_video(void)
{
  // Put a console on a frame buffer in the preferred mode
  VIDEO_Init();
  GXRModeObj *video_mode = VIDEO_GetPreferredMode(NULL);
  void *frame_buffer     = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video_mode));
  console_init(frame_buffer, 20, 20, video_mode->fbWidth, video_mode->xfbHeight,
               video_mode->fbWidth * VI_DISPLAY_PIX_SZ);

  // Show the frame buffer
  VIDEO_Configure(video_mode);
  VIDEO_SetNextFramebuffer(frame_buffer);
  VIDEO_SetBlack(false);
  VIDEO_Flush();

  // Wait for the first frame, and a second one on a progressive mode
  VIDEO_WaitVSync();
  if (video_mode->viTVMode & VI_NON_INTERLACE)
    VIDEO_WaitVSync();
}

// Stay on the IOS the loader booted rather than the one libogc would reload, so the tests run where a game would
s32 __IOS_LoadStartupIOS(void)
{
  return 0;
}

int main(void)
{
  // Start the screen, the controllers, the log and the settings
  init_video();
  input_init();
  report_init();
  settings_init();

  // Pick an entry with the D-pad and open it with A, until Exit
  int cursor = 0;
  while (true) {
    screen_begin("Wii Network Tools", "");
    screen_draw_rows(entry_rows, ENTRY_COUNT, cursor);
    screen_draw_buttons("A Select");

    // Move the cursor, and act on A
    uint32_t buttons = input_wait_press();
    cursor           = screen_move_cursor(entry_rows, ENTRY_COUNT, cursor, buttons);
    if (!(buttons & INPUT_A))
      continue;
    switch (cursor) {
      case ENTRY_CONNECTIONS:
        // Edit the console's connections
        connections_open();
        break;

      case ENTRY_NETWORK_TESTS:
        // List the network test suites
        suite_menu_open("Network Tests", network_suites, network_suite_count);
        break;

      case ENTRY_IOS_MODULE_TESTS:
        // List the IOS module test suites
        suite_menu_open("IOS Module Tests", advanced_suites, advanced_suite_count);
        break;

      case ENTRY_OPTIONS:
        // Show the app's own settings
        settings_open();
        break;

      case ENTRY_EXIT:
        // Power off under Dolphin, which crashes in libogc's exit path
        if (IOS_Open(DOLPHIN_DEVICE, 0) >= 0)
          SYS_ResetSystem(SYS_POWEROFF, 0, 0);

        // Return to a loader that left its reload stub, and otherwise to the Wii Menu, since a disc has no stub
        if (memcmp((const void *)LOADER_STUB_SIGNATURE, "STUBHAXX", 8) == 0)
          return 0;
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
        break;
    }
  }
}
