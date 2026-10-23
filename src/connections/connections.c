#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <network.h>

#include "input.h"
#include "keyboard.h"
#include "netconfig.h"
#include "screen.h"
#include "sock.h"
#include "steps.h"

#include "connections.h"

// Text shown in place of a password that is set
#define PASSWORD_MASK "********"

// Longest text a field holds, the WPA passphrase and its terminator
#define FIELD_TEXT_MAX 65

// Most rows the editor has, every field and the two gaps
#define FIELD_ROWS_MAX (FIELD_COUNT + 2)

// Fields of Change Settings, in screen order
enum field {
  FIELD_TYPE,
  FIELD_SSID,
  FIELD_SECURITY,
  FIELD_PASSWORD,
  FIELD_DHCP_IP,
  FIELD_IP,
  FIELD_NETMASK,
  FIELD_GATEWAY,
  FIELD_DHCP_DNS,
  FIELD_DNS1,
  FIELD_DNS2,
  FIELD_MTU,
  FIELD_SAVE,
  FIELD_COUNT,
};

// Actions on one connection, in screen order
enum action {
  ACTION_TEST,
  ACTION_CHANGE,
  ACTION_CLEAR,
  ACTION_USE,
  ACTION_COUNT,
};

// Labels of the fields, with the System Menu's wording
static const char *const field_labels[FIELD_COUNT] = {
  "Connection Type", "SSID",        "Security", "Password",        "Auto-Obtain IP Address",
  "IP Address",      "Subnet Mask", "Gateway",  "Auto-Obtain DNS", "Primary DNS",
  "Secondary DNS",   "MTU Value",   "Save",
};

// Rows of the actions, with the System Menu's wording
static const struct screen_row action_rows[ACTION_COUNT] = {
  {SCREEN_ROW_ACTION, "Connection Test", NULL},
  {SCREEN_ROW_ACTION, "Change Settings", NULL},
  {SCREEN_ROW_ACTION, "Clear Settings", NULL},
  {SCREEN_ROW_ACTION, "Use This Connection", NULL},
};

// The settings Connection Settings reads from and writes to NAND, too large for the stack
static struct netconfig settings;

// A slot holds no connection
static bool is_empty(int index)
{
  const struct netconfig_profile *profile = &settings.profile[index];
  return profile->flags == 0 && profile->ssid_len == 0;
}

// A slot holds the connection the console uses
static bool is_active(int index)
{
  return settings.profile[index].flags & NETCONFIG_FLAG_SELECTED;
}

// Describe a slot as wired, wireless with its SSID, or none, and whether it is the active one
static void describe(int index, char *text, size_t len)
{
  const struct netconfig_profile *profile = &settings.profile[index];
  const char *active                      = is_active(index) ? "   In Use" : "";
  if (is_empty(index)) {
    snprintf(text, len, "None");
  } else if (profile->flags & NETCONFIG_FLAG_WIRED) {
    snprintf(text, len, "Wired%s", active);
  } else {
    snprintf(text, len, "Wireless  %.*s%s", netconfig_ssid_len(profile), (const char *)profile->ssid, active);
  }
}

// Write the settings to NAND, which also makes them NCD's working copy, and say so on a failure
static bool write_settings(void)
{
  s32 error = netconfig_save(&settings);
  if (error < 0) {
    screen_set_status(SCREEN_STATUS_ERROR, "Writing the settings to NAND failed (%d)", (int)error);
    return false;
  }
  return true;
}

// Mark one slot as the connection in use and every other slot as not
static void select_slot(int index)
{
  for (int i = 0; i < NETCONFIG_PROFILES; i++)
    settings.profile[i].flags &= ~NETCONFIG_FLAG_SELECTED;
  settings.profile[index].flags |= NETCONFIG_FLAG_SELECTED;
}

// Find whether a field applies to the form's choices
static bool field_applies(const struct netconfig_form *form, enum field field)
{
  switch (field) {
    case FIELD_SSID:
    case FIELD_SECURITY:
      // Show the wireless fields only for a wireless connection
      return !form->wired;

    case FIELD_PASSWORD:
      // Show the password only for a wireless connection with security
      return !form->wired && form->encryption != NETCONFIG_ENC_OPEN;

    case FIELD_IP:
    case FIELD_NETMASK:
    case FIELD_GATEWAY:
      // Show the manual addresses only when they are not obtained automatically
      return !form->dhcp_ip;

    case FIELD_DNS1:
    case FIELD_DNS2:
      // Show the manual DNS servers only when they are not obtained automatically
      return !form->dhcp_dns;

    default:
      // Show every other field always
      return true;
  }
}

// Text a field shows for its value
static const char *field_value(const struct netconfig_form *form, enum field field)
{
  switch (field) {
    case FIELD_TYPE:
      // Name the connection type
      return form->wired ? "Wired" : "Wireless";

    case FIELD_SSID:
      // Show the SSID as typed
      return form->ssid;

    case FIELD_SECURITY:
      // Name the security type
      return netconfig_encryption_name(form->encryption);

    case FIELD_PASSWORD:
      // Hide a password that is set
      return form->key[0] ? PASSWORD_MASK : "";

    case FIELD_DHCP_IP:
      // Say whether DHCP gives the address
      return form->dhcp_ip ? "Yes" : "No";

    case FIELD_IP:
      // Show the address as typed
      return form->ip;

    case FIELD_NETMASK:
      // Show the subnet mask as typed
      return form->netmask;

    case FIELD_GATEWAY:
      // Show the gateway as typed
      return form->gateway;

    case FIELD_DHCP_DNS:
      // Say whether DHCP gives the DNS servers
      return form->dhcp_dns ? "Yes" : "No";

    case FIELD_DNS1:
      // Show the primary DNS server as typed
      return form->dns1;

    case FIELD_DNS2:
      // Show the secondary DNS server as typed
      return form->dns2;

    case FIELD_MTU:
      // Show the MTU, or the default when none is set
      return form->mtu[0] ? form->mtu : "Default";

    default:
      // Save has no value
      return "";
  }
}

// Kind of row a field takes
static enum screen_row_kind field_kind(enum field field)
{
  switch (field) {
    case FIELD_TYPE:
    case FIELD_SECURITY:
    case FIELD_DHCP_IP:
    case FIELD_DHCP_DNS:
      // Step through the values
      return SCREEN_ROW_CHOICE;

    case FIELD_SAVE:
      // Act on A
      return SCREEN_ROW_ACTION;

    default:
      // Edit on the keyboard
      return SCREEN_ROW_TEXT;
  }
}

// Build the editor's rows from the fields that apply, with gaps before the addresses and Save, returning how many
static int build_field_rows(const struct netconfig_form *form, struct screen_row *rows, enum field *row_fields)
{
  int count = 0;
  for (enum field field = 0; field < FIELD_COUNT; field++) {
    if (!field_applies(form, field))
      continue;

    // Mark a gap with FIELD_COUNT, since it has no field
    if (field == FIELD_DHCP_IP || field == FIELD_SAVE) {
      row_fields[count] = FIELD_COUNT;
      rows[count++]     = (struct screen_row){SCREEN_ROW_GAP, NULL, NULL};
    }

    // Add the field's row
    row_fields[count] = field;
    rows[count++]     = (struct screen_row){field_kind(field), field_labels[field], field_value(form, field)};
  }
  return count;
}

// Edit text on the keyboard, leaving it alone on a cancel
static void edit_text(const char *path, const char *name, char *text, int capacity, enum keyboard_mode mode)
{
  // Title the keyboard with the path to the field
  char title[96];
  snprintf(title, sizeof title, "%s > %s", path, name);

  // Edit a copy and keep it only when the user accepts it
  char copy[FIELD_TEXT_MAX];
  snprintf(copy, sizeof copy, "%s", text);
  if (keyboard_edit(title, copy, capacity, mode))
    strcpy(text, copy);
}

// Change the field under the cursor, by toggling it, stepping it or editing it on the keyboard
static void change_field(const char *path, struct netconfig_form *form, enum field field, int step)
{
  switch (field) {
    case FIELD_TYPE:
      // Switch between wired and wireless
      form->wired = !form->wired;
      break;

    case FIELD_SSID:
      // Type the SSID
      edit_text(path, "SSID", form->ssid, sizeof form->ssid, KEYBOARD_MODE_TEXT);
      break;

    case FIELD_SECURITY:
      // Step through the security types
      form->encryption = netconfig_encryption_step(form->encryption, step);
      break;

    case FIELD_PASSWORD:
      // Type the password, as hex digits for WEP
      edit_text(path,
                form->encryption == NETCONFIG_ENC_WEP64 || form->encryption == NETCONFIG_ENC_WEP128
                  ? "Password, as hex digits"
                  : "Password",
                form->key, sizeof form->key, KEYBOARD_MODE_TEXT);
      break;

    case FIELD_DHCP_IP:
      // Toggle whether DHCP gives the address
      form->dhcp_ip = !form->dhcp_ip;
      break;

    case FIELD_IP:
      // Type the address
      edit_text(path, "IP Address", form->ip, sizeof form->ip, KEYBOARD_MODE_NUMERIC);
      break;

    case FIELD_NETMASK:
      // Type the subnet mask
      edit_text(path, "Subnet Mask", form->netmask, sizeof form->netmask, KEYBOARD_MODE_NUMERIC);
      break;

    case FIELD_GATEWAY:
      // Type the gateway
      edit_text(path, "Gateway", form->gateway, sizeof form->gateway, KEYBOARD_MODE_NUMERIC);
      break;

    case FIELD_DHCP_DNS:
      // Toggle whether DHCP gives the DNS servers
      form->dhcp_dns = !form->dhcp_dns;
      break;

    case FIELD_DNS1:
      // Type the primary DNS server
      edit_text(path, "Primary DNS", form->dns1, sizeof form->dns1, KEYBOARD_MODE_NUMERIC);
      break;

    case FIELD_DNS2:
      // Type the secondary DNS server
      edit_text(path, "Secondary DNS", form->dns2, sizeof form->dns2, KEYBOARD_MODE_NUMERIC);
      break;

    case FIELD_MTU:
      // Type the MTU
      edit_text(path, "MTU Value", form->mtu, sizeof form->mtu, KEYBOARD_MODE_NUMERIC);
      break;

    default:
      // Save is handled by the editor itself
      break;
  }
}

// Edit one connection's settings until the user saves them or goes back without saving
static void change_settings(int index)
{
  // Fill the form from the slot
  struct netconfig_form form;
  netconfig_profile_to_form(&settings.profile[index], &form);

  // Edit the form until the user saves it or goes back
  struct screen_row rows[FIELD_ROWS_MAX];
  enum field row_fields[FIELD_ROWS_MAX];
  char path[64];
  snprintf(path, sizeof path, "Connection Settings > Connection %d > Change Settings", index + 1);
  int cursor = 0;
  while (true) {
    // Draw the fields that apply, which change with the choices
    int count = build_field_rows(&form, rows, row_fields);
    if (cursor >= count)
      cursor = count - 1;
    screen_begin(path, "");
    screen_draw_rows(rows, count, cursor);
    screen_draw_row_buttons(&rows[cursor], "Save", "Back Without Saving");

    // Move the cursor or leave without saving
    uint32_t buttons = input_wait_press();
    cursor           = screen_move_cursor(rows, count, cursor, buttons);
    if (buttons & INPUT_B)
      return;

    // Step a choice on left and right
    enum field field = row_fields[cursor];
    if ((buttons & (INPUT_LEFT | INPUT_RIGHT)) && rows[cursor].kind == SCREEN_ROW_CHOICE)
      change_field(path, &form, field, buttons & INPUT_LEFT ? -1 : 1);
    if (!(buttons & INPUT_A))
      continue;

    // Change the field under the cursor
    if (field != FIELD_SAVE) {
      change_field(path, &form, field, 1);
      continue;
    }

    // Check and store the form in its slot, then write it
    const char *problem = netconfig_form_to_profile(&form, &settings.profile[index]);
    if (problem) {
      screen_set_status(SCREEN_STATUS_ERROR, "%s", problem);
      continue;
    }
    if (write_settings())
      screen_set_status(SCREEN_STATUS_OK, "Settings saved");
    return;
  }
}

// Test a connection like the System Menu, making it the one in use and marking it tested on a pass
static void test_connection(int index)
{
  // Make it the connection in use, since the console connects only with that one
  select_slot(index);
  if (!write_settings())
    return;

  // Show the wait, since the link can take up to a minute
  char path[64];
  snprintf(path, sizeof path, "Connection Settings > Connection %d > Connection Test", index + 1);
  screen_begin(path, "");
  printf("   Connecting with Connection %d. This can take up to a minute.\n", index + 1);
  screen_set_status(SCREEN_STATUS_INFO, "Connecting...");
  screen_draw_buttons("");

  // Bring the link up, read the address and take the link down again
  int error = step_link_up();
  u32 ip    = error < 0 ? 0 : net_gethostip();
  steps_finish();
  if (error < 0 || ip == 0) {
    screen_set_status(SCREEN_STATUS_ERROR, "Connection test failed (%d)", error < 0 ? error : -1);
    return;
  }

  // Mark the connection tested, like the System Menu
  settings.profile[index].flags |= NETCONFIG_FLAG_TESTED;
  if (write_settings())
    screen_set_status(SCREEN_STATUS_OK, "Connection test passed, IP address %s", sock_ip_text(ip));
}

// Show one connection's actions until the user goes back
static void open_connection(int index)
{
  int cursor = 0;
  while (true) {
    // Draw the connection's actions
    char path[64];
    snprintf(path, sizeof path, "Connection Settings > Connection %d", index + 1);
    screen_begin(path, "");
    screen_draw_rows(action_rows, ACTION_COUNT, cursor);
    screen_draw_buttons("A Select   B Back");

    // Move the cursor or go back
    uint32_t buttons = input_wait_press();
    cursor           = screen_move_cursor(action_rows, ACTION_COUNT, cursor, buttons);
    if (buttons & INPUT_B)
      return;
    if (!(buttons & INPUT_A))
      continue;

    // Open the editor, which also sets up an empty slot
    if (cursor == ACTION_CHANGE) {
      change_settings(index);
      continue;
    }

    // Every other action needs settings to act on
    if (is_empty(index)) {
      screen_set_status(SCREEN_STATUS_INFO, "Connection %d has no settings yet", index + 1);
      continue;
    }

    // Test the connection
    if (cursor == ACTION_TEST) {
      test_connection(index);
      continue;
    }

    // Use the connection, unless it is the one in use already
    if (cursor == ACTION_USE) {
      if (is_active(index)) {
        screen_set_status(SCREEN_STATUS_INFO, "Connection %d is already in use", index + 1);
        continue;
      }
      select_slot(index);
      if (write_settings())
        screen_set_status(SCREEN_STATUS_OK, "Connection %d is now in use", index + 1);
      continue;
    }

    // Clear the settings once the user confirms
    char question[48];
    snprintf(path, sizeof path, "Connection Settings > Connection %d > Clear Settings", index + 1);
    snprintf(question, sizeof question, "Clear the settings for Connection %d?", index + 1);
    if (screen_confirm(path, question, "Clear")) {
      memset(&settings.profile[index], 0, sizeof settings.profile[index]);
      if (write_settings())
        screen_set_status(SCREEN_STATUS_OK, "Connection %d cleared", index + 1);
    }
  }
}

void connections_open(void)
{
  // Read the settings from NAND, like the System Menu
  s32 error = netconfig_load(&settings);
  if (error < 0) {
    screen_set_status(SCREEN_STATUS_ERROR, "Reading the settings from NAND failed (%d)", (int)error);
    return;
  }

  // Read the Wi-Fi module's MAC address from NCD, for setting up a router's MAC filter
  u8 mac[6] ATTRIBUTE_ALIGN(32) = {0};
  bool mac_read                 = net_get_mac_address(mac) >= 0;

  // Show it unless it is all zeros, which a console without a working module reports
  char mac_text[24] = "Not available";
  if (mac_read && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]))
    snprintf(mac_text, sizeof mac_text, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);

  // List the three connections, with the MAC address at the bottom, until the user goes back
  struct screen_row rows[NETCONFIG_PROFILES];
  char labels[NETCONFIG_PROFILES][16];
  char texts[NETCONFIG_PROFILES][64];
  int cursor = 0;
  while (true) {
    for (int i = 0; i < NETCONFIG_PROFILES; i++) {
      snprintf(labels[i], sizeof labels[i], "Connection %d", i + 1);
      describe(i, texts[i], sizeof texts[i]);
      rows[i] = (struct screen_row){SCREEN_ROW_ACTION, labels[i], texts[i]};
    }
    screen_begin("Connection Settings", "");
    screen_draw_rows(rows, NETCONFIG_PROFILES, cursor);
    screen_draw_info("Wi-Fi MAC Address", mac_text);
    screen_draw_buttons("A Select   B Back");

    // Move the cursor, open a connection or go back
    uint32_t buttons = input_wait_press();
    cursor           = screen_move_cursor(rows, NETCONFIG_PROFILES, cursor, buttons);
    if (buttons & INPUT_B)
      return;
    if (buttons & INPUT_A)
      open_connection(cursor);
  }
}
