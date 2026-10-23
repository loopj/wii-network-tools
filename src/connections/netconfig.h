#pragma once

#include <stdbool.h>

#include <gctypes.h>

// Number of connection profiles in the file
#define NETCONFIG_PROFILES 3

// Profile flag bits
#define NETCONFIG_FLAG_WIRED    0x01
#define NETCONFIG_FLAG_DHCP_IP  0x02
#define NETCONFIG_FLAG_DHCP_DNS 0x04
#define NETCONFIG_FLAG_PROXY    0x10
#define NETCONFIG_FLAG_TESTED   0x20
#define NETCONFIG_FLAG_PMTU     0x40
#define NETCONFIG_FLAG_SELECTED 0x80

// Wireless security types stored in a profile
enum netconfig_encryption {
  NETCONFIG_ENC_OPEN     = 0,
  NETCONFIG_ENC_WEP64    = 1,
  NETCONFIG_ENC_WEP128   = 2,
  NETCONFIG_ENC_WPA_TKIP = 4,
  NETCONFIG_ENC_WPA2_AES = 5,
  NETCONFIG_ENC_WPA_AES  = 6,
};

// HTTP proxy settings, stored once for HTTP and once for SSL
struct netconfig_proxy {
  // 1 when the proxy is used
  u8 enabled;

  // 1 when the username and password are sent
  u8 use_auth;

  u8 pad0[2];
  u8 host[255];
  u8 pad1;
  u16 port;
  u8 username[32];
  u8 pad2;
  u8 password[32];
} __attribute__((packed));

// One connection profile, 0x91c bytes
struct netconfig_profile {
  // NETCONFIG_FLAG bits
  u8 flags;

  u8 pad0[3];

  // Manual addresses
  u8 ip[4];
  u8 netmask[4];
  u8 gateway[4];
  u8 dns1[4];
  u8 dns2[4];

  // 0 for the default, otherwise 68 to 1500
  u32 mtu;

  // 0 for the default
  u32 tcp_timeout;

  u8 pad1[4];
  struct netconfig_proxy proxy;
  u8 pad2;
  struct netconfig_proxy proxy_ssl;
  u8 pad3[1293];

  // Wireless rate set, 0 for the default
  u16 rateset;

  // 0 for a normal profile, 1 for the Nintendo USB access point
  u8 method;

  u8 pad4;

  // SSID without a terminator
  u8 ssid[32];

  u16 ssid_len;
  u16 pad5;

  // enum netconfig_encryption
  u16 encryption;

  u16 pad6;

  // Passphrase length for WPA, WEP key index for WEP
  u16 key_len;

  u8 pad7[2];

  // WPA passphrase, or the WEP key bytes repeated four times
  u8 key[64];

  u8 pad8[236];
} __attribute__((packed));

// The whole file, 0x1b5c bytes
struct netconfig {
  // 0
  u32 version;

  // 0 none, 1 wireless, 2 wired
  u8 media;

  // WiiConnect24 permission bits, 0 to 7
  u8 nwc24;

  // Link timeout in seconds, 7 on a stock console
  u8 link_timeout;

  u8 pad;
  struct netconfig_profile profile[NETCONFIG_PROFILES];
} __attribute__((packed));

// Editable view of one profile with every value as text
struct netconfig_form {
  // The profile is the one the console uses
  bool selected;

  bool wired;
  char ssid[33];

  // enum netconfig_encryption
  int encryption;

  // WPA passphrase, or 10 or 26 hex digits for WEP
  char key[65];

  // DHCP gives the address
  bool dhcp_ip;

  char ip[16];
  char netmask[16];
  char gateway[16];

  // DHCP gives the DNS servers
  bool dhcp_dns;

  char dns1[16];
  char dns2[16];

  // Empty or 0 for the default
  char mtu[5];
};

// Read the settings saved on NAND, or an empty configuration when there are none
s32 netconfig_load(struct netconfig *config);

// Read the configuration NCD uses right now, which a connection test or a switch can change
s32 netconfig_load_working(struct netconfig *config);

// Make NCD use the configuration until IOS reloads, without writing NAND
s32 netconfig_apply(struct netconfig *config);

// Have NCD check the configuration, write it to NAND and use it
s32 netconfig_save(struct netconfig *config);

// Index of the profile marked selected, or -1 when none is
int netconfig_find_selected(const struct netconfig *config);

// Length of a profile's SSID, which has no terminator when it fills its field
int netconfig_ssid_len(const struct netconfig_profile *profile);

// Fill a form from a profile
void netconfig_profile_to_form(const struct netconfig_profile *profile, struct netconfig_form *form);

// Write a form into a profile and return NULL, or return an error message and leave the profile alone
const char *netconfig_form_to_profile(const struct netconfig_form *form, struct netconfig_profile *profile);

// Name a security type for display
const char *netconfig_encryption_name(int encryption);

// Return the security type step entries on from the given one in the menu order, wrapping at the ends
int netconfig_encryption_step(int encryption, int step);
