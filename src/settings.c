#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/es.h>
#include <ogc/isfs.h>

#include "input.h"
#include "keyboard.h"
#include "screen.h"

#include "settings.h"

// Upper half of a disc title's ID, the titles whose saves the System Menu manages
#define DISC_TITLE_TYPE 0x00010000

// ISFS result for a file that already exists
#define ISFS_EEXIST -105

// Characters in the banner's title and subtitle
#define BANNER_TEXT_LEN 32

// Width of the banner image
#define BANNER_WIDTH 192

// Height of the banner image
#define BANNER_HEIGHT 64

// Width and height of the icon
#define ICON_SIZE 48

// Banner magic, WIBN
#define BANNER_MAGIC 0x5749424e

// Display time of the first icon frame, with no frame after it
#define BANNER_ONE_ICON 0x0001

// Banner color, an opaque light blue in RGB5A3
#define BANNER_COLOR 0xef9f

// Icon color, an opaque Wii blue in RGB5A3
#define ICON_COLOR 0x825f

// Rows of the menu, in screen order
enum setting {
  SETTING_IPERF_SERVER,
  SETTING_COUNT,
};

// The settings kept in settings.bin
struct saved_settings {
  char iperf_server[SETTINGS_NAME_MAX];
};

// The save's banner.bin with one icon, which names the save in Data Management, text in UTF-16, images in RGB5A3
struct save_banner {
  uint32_t magic;

  // 0, a save that can be copied
  uint32_t flags;

  // Display time of each icon frame, two bits each, 0 after the last
  uint16_t animation_speed;

  uint8_t reserved[22];
  uint16_t title[BANNER_TEXT_LEN];
  uint16_t subtitle[BANNER_TEXT_LEN];
  uint16_t banner[BANNER_WIDTH * BANNER_HEIGHT];
  uint16_t icon[ICON_SIZE * ICON_SIZE];
};

// The iperf3 server, empty until one is entered or loaded
char iperf_server[SETTINGS_NAME_MAX];

// The disc title's data directory, which ES writes by DMA
static char data_dir[64] ATTRIBUTE_ALIGN(32);

// The app runs as a disc and has a data directory to save in
static bool saves_on;

// settings.bin as read or written, which ISFS moves by DMA
static struct saved_settings saved ATTRIBUTE_ALIGN(32);

// banner.bin as written, which ISFS reads by DMA
static struct save_banner banner ATTRIBUTE_ALIGN(32);

// Copy ASCII text into a UTF-16 field
static void write_text(uint16_t *field, const char *text)
{
  for (int i = 0; i < BANNER_TEXT_LEN && text[i]; i++)
    field[i] = (uint8_t)text[i];
}

// Fill in the banner with the app's name and plain images
static void fill_banner(void)
{
  memset(&banner, 0, sizeof banner);
  banner.magic           = BANNER_MAGIC;
  banner.animation_speed = BANNER_ONE_ICON;
  write_text(banner.title, "Wii Network Tools");
  write_text(banner.subtitle, "iperf3 server");

  // Fill both images with one color, so the tile order does not matter
  for (int i = 0; i < BANNER_WIDTH * BANNER_HEIGHT; i++)
    banner.banner[i] = BANNER_COLOR;
  for (int i = 0; i < ICON_SIZE * ICON_SIZE; i++)
    banner.icon[i] = ICON_COLOR;
}

// Write a file in the data directory, or leave one already there alone when keep is set
static int write_file(const char *name, const void *data, uint32_t len, bool keep)
{
  char path[96];
  snprintf(path, sizeof path, "%s/%s", data_dir, name);

  // Create the file, which may be there from an earlier save
  int error = ISFS_CreateFile(path, 0, ISFS_OPEN_RW, ISFS_OPEN_RW, 0);
  if (error == ISFS_EEXIST && keep)
    return 0;
  if (error < 0 && error != ISFS_EEXIST)
    return error;

  // Write it whole
  s32 fd = ISFS_Open(path, ISFS_OPEN_WRITE);
  if (fd < 0)
    return fd;
  error = ISFS_Write(fd, data, len);
  ISFS_Close(fd);
  return error < 0 ? error : 0;
}

// Write the settings to the disc's save, doing nothing from the Homebrew Channel, returning 0 or a negative ISFS error
static int write_save(void)
{
  if (!saves_on)
    return 0;

  // Write the banner the first time, so the save shows in Data Management
  fill_banner();
  int error = write_file("banner.bin", &banner, sizeof banner, true);
  if (error < 0)
    return error;

  // Write the settings
  memset(&saved, 0, sizeof saved);
  snprintf(saved.iperf_server, sizeof saved.iperf_server, "%s", iperf_server);
  return write_file("settings.bin", &saved, sizeof saved, false);
}

// Edit the iperf3 server on the keyboard and save it
static void edit_server(void)
{
  // Edit a copy, so a cancel keeps the server as it was
  char server[SETTINGS_NAME_MAX];
  snprintf(server, sizeof server, "%s", iperf_server);
  if (!keyboard_edit("Options > iperf3 Server", server, sizeof server, KEYBOARD_MODE_TEXT))
    return;
  snprintf(iperf_server, sizeof iperf_server, "%s", server);

  // Keep it for the next session when the app runs as a disc
  int error = write_save();
  if (error < 0)
    screen_set_status(SCREEN_STATUS_ERROR, "Saving the server failed (%d)", error);
}

void settings_init(void)
{
  // Save only as a disc title, which has a data directory of its own
  u64 title_id;
  if (ES_GetTitleID(&title_id) < 0 || (title_id >> 32) != DISC_TITLE_TYPE)
    return;
  if (ES_GetDataDir(title_id, data_dir) < 0 || ISFS_Initialize() < 0)
    return;
  saves_on = true;

  // Read settings.bin back, keeping the defaults when there is none yet
  char path[96];
  snprintf(path, sizeof path, "%s/settings.bin", data_dir);
  s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
  if (fd < 0)
    return;
  s32 len = ISFS_Read(fd, &saved, sizeof saved);
  ISFS_Close(fd);
  if (len == sizeof saved)
    snprintf(iperf_server, sizeof iperf_server, "%.*s", (int)sizeof saved.iperf_server, saved.iperf_server);
}

void settings_open(void)
{
  int cursor = 0;
  while (true) {
    // Draw the settings with their values
    struct screen_row rows[SETTING_COUNT] = {
      [SETTING_IPERF_SERVER] = {SCREEN_ROW_TEXT, "iperf3 Server", iperf_server[0] ? iperf_server : "Not set"},
    };
    screen_begin("Options", "");
    screen_draw_rows(rows, SETTING_COUNT, cursor);
    screen_draw_row_buttons(&rows[cursor], "Select", "Back");

    // Move the cursor, edit a setting or go back
    uint32_t buttons = input_wait_press();
    if (buttons & INPUT_B)
      return;
    cursor = screen_move_cursor(rows, SETTING_COUNT, cursor, buttons);
    if (buttons & INPUT_A)
      edit_server();
  }
}
