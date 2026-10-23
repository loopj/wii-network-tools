#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ogc/ipc.h>

#include "netconfig.h"

// NCD, which holds the working copy of the configuration the console uses
#define NCD_PATH "/dev/net/ncd/manage"

// NCD requests on its working copy and on the file, all ioctlv
#define NCD_IOCTL_GET_CONFIG   3
#define NCD_IOCTL_SET_CONFIG   4
#define NCD_IOCTL_READ_CONFIG  5
#define NCD_IOCTL_WRITE_CONFIG 6

// NCD's result when it could not read the file from NAND, as on a console that never saved its settings
#define NCD_EREAD -6

// Time allowed for NCD to register its device after IOS reloads, which it has not done yet when an app starts
#define NCD_OPEN_WAIT_MS 5000

// Interval between tries to open NCD, in milliseconds
#define NCD_OPEN_SLICE_MS 50

// IOS reads PowerPC buffers by DMA, so the configuration is staged in whole cache lines
#define STAGE_SIZE ((sizeof(struct netconfig) + 31) & ~31)

// Words in NCD's result buffer, which starts with NCD's result word
#define RESULT_WORDS 8

// Order the security types cycle through in the menu
static const int encryption_order[] = {NETCONFIG_ENC_OPEN,     NETCONFIG_ENC_WEP64,    NETCONFIG_ENC_WEP128,
                                       NETCONFIG_ENC_WPA_TKIP, NETCONFIG_ENC_WPA2_AES, NETCONFIG_ENC_WPA_AES};

// Names of the security types for display
static const char *const encryption_names[] = {
  [NETCONFIG_ENC_OPEN]     = "None",
  [NETCONFIG_ENC_WEP64]    = "WEP 64",
  [NETCONFIG_ENC_WEP128]   = "WEP 128",
  [NETCONFIG_ENC_WPA_TKIP] = "WPA-PSK (TKIP)",
  [NETCONFIG_ENC_WPA2_AES] = "WPA2-PSK (AES)",
  [NETCONFIG_ENC_WPA_AES]  = "WPA-PSK (AES)",
};

// Open NCD, waiting while its device is not there yet
static s32 open_ncd(void)
{
  int waited = 0;
  while (true) {
    // Return the descriptor, or the error once NCD is not just missing or the wait is over
    s32 fd = IOS_Open(NCD_PATH, 0);
    if (fd != IPC_ENOENT || waited >= NCD_OPEN_WAIT_MS)
      return fd;

    // Wait a slice and try again
    usleep(NCD_OPEN_SLICE_MS * 1000);
    waited += NCD_OPEN_SLICE_MS;
  }
}

// Send NCD a request with the stage and the result buffer, the first input_count of them as input
static s32 call_ncd(u32 request, u8 *stage, s32 *result, int input_count)
{
  s32 fd = open_ncd();
  if (fd < 0)
    return fd;

  // Send the request and close NCD again
  ioctlv vectors[2] = {{stage, STAGE_SIZE}, {result, RESULT_WORDS * sizeof *result}};
  s32 error         = IOS_Ioctlv(fd, request, input_count, 2 - input_count, vectors);
  IOS_Close(fd);
  return error;
}

// Send a configuration to NCD with a request and return NCD's result word
static s32 send_config(u32 request, const struct netconfig *config)
{
  // Clear the result first, since an emulated NCD can leave it unwritten
  s32 result[RESULT_WORDS] __attribute__((aligned(32))) = {0};
  u8 *stage                                             = memalign(32, STAGE_SIZE);
  if (!stage)
    return IPC_ENOMEM;
  memcpy(stage, config, sizeof *config);

  // Send it
  s32 error = call_ncd(request, stage, result, 1);
  free(stage);
  return error < 0 ? error : result[0];
}

// Read a configuration from NCD with a request, giving NCD's result word in status and returning any IOS error
static s32 get_config(u32 request, struct netconfig *config, s32 *status)
{
  // Clear the result first, since an emulated NCD can leave it unwritten
  s32 result[RESULT_WORDS] __attribute__((aligned(32))) = {0};
  u8 *stage                                             = memalign(32, STAGE_SIZE);
  if (!stage)
    return IPC_ENOMEM;

  // Ask for it
  s32 error = call_ncd(request, stage, result, 0);

  // Copy out what NCD answered
  if (error >= 0 && result[0] >= 0)
    memcpy(config, stage, sizeof *config);
  free(stage);
  *status = result[0];
  return error < 0 ? error : 0;
}

// Keep a single selected profile and name its media type in the header, as NCD's checks require
static void normalize(struct netconfig *config)
{
  // Reset the version and the media type
  config->version = 0;
  config->media   = 0;

  // Keep the first selected profile and take its media type
  for (int i = 0; i < NETCONFIG_PROFILES; i++) {
    struct netconfig_profile *profile = &config->profile[i];
    if (!(profile->flags & NETCONFIG_FLAG_SELECTED))
      continue;
    if (config->media != 0) {
      profile->flags &= ~NETCONFIG_FLAG_SELECTED;
      continue;
    }
    config->media = (profile->flags & NETCONFIG_FLAG_WIRED) ? 2 : 1;
  }

  // Fill in the stock link timeout when none is set
  if (config->link_timeout == 0)
    config->link_timeout = 7;
}

// Format an address as dotted text, empty when it is unset
static void format_address(const u8 address[4], char *text)
{
  if (address[0] == 0 && address[1] == 0 && address[2] == 0 && address[3] == 0) {
    text[0] = 0;
  } else {
    sprintf(text, "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
  }
}

// Parse dotted text into an address, an empty field as unset, returning false when it is not one
static bool parse_address(const char *text, u8 address[4])
{
  memset(address, 0, 4);
  if (text[0] == 0)
    return true;

  // Parse four dotted octets with nothing after them
  for (int i = 0; i < 4; i++) {
    char *end;
    long octet = strtol(text, &end, 10);
    if (end == text || octet < 0 || octet > 255)
      return false;
    address[i] = octet;
    text       = end;
    if (i < 3) {
      if (*text != '.')
        return false;
      text++;
    }
  }
  return *text == 0;
}

// Return the length in bytes of a WEP key, or 0 when the security type is not WEP
static int get_wep_key_bytes(int encryption)
{
  return encryption == NETCONFIG_ENC_WEP64 ? 5 : encryption == NETCONFIG_ENC_WEP128 ? 13 : 0;
}

s32 netconfig_load(struct netconfig *config)
{
  // Read the file through NCD, starting a console with no file yet from an empty one, like the System Menu
  memset(config, 0, sizeof *config);
  s32 status;
  s32 error = get_config(NCD_IOCTL_READ_CONFIG, config, &status);
  if (error < 0)
    return error;
  if (status == NCD_EREAD)
    return 0;
  return status < 0 ? status : 0;
}

s32 netconfig_load_working(struct netconfig *config)
{
  s32 status;
  s32 error = get_config(NCD_IOCTL_GET_CONFIG, config, &status);
  if (error < 0)
    return error;
  return status < 0 ? status : 0;
}

s32 netconfig_apply(struct netconfig *config)
{
  normalize(config);

  // Have NCD check the configuration, replace its working copy and rebuild its network list, leaving NAND alone
  return send_config(NCD_IOCTL_SET_CONFIG, config);
}

s32 netconfig_save(struct netconfig *config)
{
  normalize(config);

  // Have NCD check the configuration and write it, like the System Menu
  s32 error = send_config(NCD_IOCTL_WRITE_CONFIG, config);
  if (error < 0)
    return error;

  // Make the same copy the one in use
  return send_config(NCD_IOCTL_SET_CONFIG, config);
}

int netconfig_find_selected(const struct netconfig *config)
{
  for (int i = 0; i < NETCONFIG_PROFILES; i++) {
    if (config->profile[i].flags & NETCONFIG_FLAG_SELECTED)
      return i;
  }
  return -1;
}

int netconfig_ssid_len(const struct netconfig_profile *profile)
{
  return profile->ssid_len < sizeof profile->ssid ? profile->ssid_len : (int)sizeof profile->ssid;
}

void netconfig_profile_to_form(const struct netconfig_profile *profile, struct netconfig_form *form)
{
  // Pull the flags apart, treating an empty slot as a fresh DHCP profile
  memset(form, 0, sizeof *form);
  form->selected = profile->flags & NETCONFIG_FLAG_SELECTED;
  form->wired    = profile->flags & NETCONFIG_FLAG_WIRED;
  form->dhcp_ip  = profile->flags & NETCONFIG_FLAG_DHCP_IP;
  form->dhcp_dns = profile->flags & NETCONFIG_FLAG_DHCP_DNS;
  if (profile->flags == 0) {
    form->dhcp_ip  = true;
    form->dhcp_dns = true;
  }

  // Copy the SSID, which is stored without a terminator
  memcpy(form->ssid, profile->ssid, netconfig_ssid_len(profile));
  form->encryption = profile->encryption;

  // Show a WEP key as hex digits and a WPA key as its passphrase
  int key_bytes = get_wep_key_bytes(form->encryption);
  if (key_bytes) {
    for (int i = 0; i < key_bytes; i++)
      sprintf(form->key + i * 2, "%02X", profile->key[i]);
  } else if (form->encryption != NETCONFIG_ENC_OPEN) {
    memcpy(form->key, profile->key, profile->key_len < sizeof profile->key ? profile->key_len : sizeof profile->key);
  }

  // Format the addresses and the MTU
  format_address(profile->ip, form->ip);
  format_address(profile->netmask, form->netmask);
  format_address(profile->gateway, form->gateway);
  format_address(profile->dns1, form->dns1);
  format_address(profile->dns2, form->dns2);
  if (profile->mtu != 0)
    sprintf(form->mtu, "%u", (unsigned)(profile->mtu > 9999 ? 9999 : profile->mtu));
}

const char *netconfig_form_to_profile(const struct netconfig_form *form, struct netconfig_profile *profile)
{
  // Work on a copy, so a form that fails its checks leaves the profile alone
  struct netconfig_profile result = *profile;

  // Rebuild the flags, dropping the test result since the settings changed
  result.flags &= NETCONFIG_FLAG_PROXY | NETCONFIG_FLAG_PMTU;
  if (form->selected)
    result.flags |= NETCONFIG_FLAG_SELECTED;
  if (form->wired)
    result.flags |= NETCONFIG_FLAG_WIRED;
  if (form->dhcp_ip)
    result.flags |= NETCONFIG_FLAG_DHCP_IP;
  if (form->dhcp_dns)
    result.flags |= NETCONFIG_FLAG_DHCP_DNS;

  // Parse the addresses and check that the manual modes have what they need
  if (!parse_address(form->ip, result.ip) || !parse_address(form->netmask, result.netmask) ||
      !parse_address(form->gateway, result.gateway))
    return "IP address, subnet mask or gateway is not valid";
  if (!parse_address(form->dns1, result.dns1) || !parse_address(form->dns2, result.dns2))
    return "DNS address is not valid";
  if (!form->dhcp_ip && (form->ip[0] == 0 || form->netmask[0] == 0))
    return "A manual IP address needs an address and a subnet mask";
  if (!form->dhcp_dns && form->dns1[0] == 0)
    return "Manual DNS needs a primary DNS address";

  // Parse the MTU, where 0 or an empty field means the default
  result.mtu = 0;
  if (form->mtu[0] != 0) {
    char *end;
    long mtu = strtol(form->mtu, &end, 10);
    if (*end != 0 || mtu < 0 || (mtu != 0 && (mtu < 68 || mtu > 1500)))
      return "MTU must be 0 or between 68 and 1500";
    result.mtu = mtu;
  }

  // Clear the wireless fields, which stay zero on a wired profile
  result.rateset = 0;
  result.method  = 0;
  memset(result.ssid, 0, sizeof result.ssid);
  result.ssid_len   = 0;
  result.encryption = 0;
  result.key_len    = 0;
  memset(result.key, 0, sizeof result.key);
  if (form->wired) {
    *profile = result;
    return NULL;
  }

  // Store the SSID without a terminator
  size_t ssid_len = strlen(form->ssid);
  if (ssid_len == 0 || ssid_len > 32)
    return "SSID must be 1 to 32 characters";
  memcpy(result.ssid, form->ssid, ssid_len);
  result.ssid_len   = ssid_len;
  result.encryption = form->encryption;

  // Store a WEP key as raw bytes in all four key slots, or a WPA key as its passphrase
  size_t key_len = strlen(form->key);
  int key_bytes  = get_wep_key_bytes(form->encryption);
  if (key_bytes) {
    if (key_len != (size_t)key_bytes * 2)
      return key_bytes == 5 ? "WEP 64 key must be 10 hex digits" : "WEP 128 key must be 26 hex digits";
    for (int i = 0; i < key_bytes; i++) {
      if (!isxdigit((unsigned char)form->key[i * 2]) || !isxdigit((unsigned char)form->key[i * 2 + 1]))
        return "WEP key must be hex digits only";
      char pair[3]  = {form->key[i * 2], form->key[i * 2 + 1], 0};
      result.key[i] = strtol(pair, NULL, 16);
    }
    for (int i = 1; i < 4; i++)
      memcpy(result.key + i * key_bytes, result.key, key_bytes);
  } else if (form->encryption != NETCONFIG_ENC_OPEN) {
    if (key_len < 8 || key_len > 63)
      return "WPA passphrase must be 8 to 63 characters";
    memcpy(result.key, form->key, key_len);
    result.key_len = key_len;
  }

  // Store the checked profile
  *profile = result;
  return NULL;
}

const char *netconfig_encryption_name(int encryption)
{
  // Look the type up, naming any type outside the table unknown
  int count = sizeof encryption_names / sizeof encryption_names[0];
  if (encryption < 0 || encryption >= count || !encryption_names[encryption])
    return "Unknown";
  return encryption_names[encryption];
}

int netconfig_encryption_step(int encryption, int step)
{
  // Step through the menu order, wrapping at the ends
  int count = sizeof encryption_order / sizeof encryption_order[0];
  for (int i = 0; i < count; i++) {
    if (encryption_order[i] == encryption)
      return encryption_order[(i + step + count) % count];
  }
  return NETCONFIG_ENC_OPEN;
}
