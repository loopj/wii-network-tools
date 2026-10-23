#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <network.h>
#include <ogc/ipc.h>
#include <ogc/lwp_watchdog.h>

#include "iperf.h"
#include "netconfig.h"
#include "report.h"
#include "settings.h"
#include "sock.h"
#include "suite.h"

#include "steps.h"

// IOS socket device that libogc's network calls go through
#define SO_DEVICE "/dev/net/ip/top"

// SO request that returns the interface address, 0 while it has none
#define SO_IOCTL_GET_HOST_ID 0x10

// SO request that reads an interface option
#define SO_IOCTL_GET_INTERFACE_OPT 0x1c

// Level of the interface options
#define IFOPT_LEVEL 0xfffe

// Interface option with the address, the netmask and the broadcast address
#define IFOPT_ADDRESS 0x4003

// Interface option with up to two DNS servers
#define IFOPT_DNS_SERVERS 0xb003

// NCD's management device
#define NCD_DEVICE "/dev/net/ncd/manage"

// NCD request for the wireless link status
#define NCD_IOCTL_LINK_STATUS 0x07

// NCD link status for a wired profile
#define NCD_STATUS_WIRED 3

// NCD link status for a wireless link that is up
#define NCD_STATUS_WIFI_UP 5

// KD's request device
#define KD_DEVICE "/dev/net/kd/request"

// KD request that stops the socket layer like the SDK's SOCleanup, so the next KD start runs DHCP again
#define KD_IOCTL_CLEANUP 0x07

// Time allowed for a server to answer
#define REPLY_TIMEOUT_MS 5000

// HTTP server port
#define HTTP_PORT 80

// NTP server port
#define NTP_PORT 123

// Size of an NTP packet without extensions
#define NTP_PACKET_LEN 48

// NTP requests sent before giving up, since UDP can lose a datagram
#define NTP_ATTEMPTS 3

// Seconds from the NTP epoch in 1900 to the Unix epoch in 1970
#define NTP_UNIX_OFFSET 2208988800u

// Name the DNS step resolves
#define DNS_HOST "example.com"

// HTTP server the TCP step requests from
#define TCP_HOST "connectivitycheck.gstatic.com"

// Path the TCP step requests
#define TCP_PATH "/generate_204"

// HTTP status the TCP step expects
#define TCP_STATUS 204

// NTP server the UDP step queries
#define NTP_HOST "pool.ntp.org"

// iperf3 server port
#define IPERF_PORT 5201

// Throughput runs per direction
#define IPERF_RUNS 3

// Seconds of each run that count, after the warm up
#define IPERF_SECONDS 30

// Seconds at the start of each run left out of the figures, while TCP is still ramping up
#define IPERF_WARMUP_SECONDS 2
#if IPERF_SECONDS + IPERF_WARMUP_SECONDS > IPERF_SECONDS_MAX
#error "A throughput run must fit in the seconds iperf_run counts"
#endif

// Tries of an iperf3 run that fails before any data moved
#define IPERF_ATTEMPTS 2

// Wait before each iperf3 run, since servers restart their listener after each test, in microseconds
#define IPERF_SETTLE_US 1000000

// Time a busy iperf3 server takes to turn a connection away, in milliseconds
#define SERVER_PROBE_MS 500

// The link is up from a step, so a test that stops early can take it down
static bool link_is_up;

// What the last step found or where it stopped, shown beside its result
static char note_text[128];

// KD's reply to the cleanup request, which IOS writes by DMA
static s32 cleanup_reply[8] ATTRIBUTE_ALIGN(32);

// The HTTP request and the start of its reply
static char http_buffer[512];

// The NTP request and its reply
static u8 ntp_packet[NTP_PACKET_LEN];

// The last iperf3 run's result
static struct iperf_result iperf_result;

// Each counted second of every iperf3 run, in tenths of a Mbit/s
static uint32_t samples[IPERF_RUNS * IPERF_SECONDS_MAX];

// NCD's link status reply, which IOS writes by DMA
static u32 link_status[8] ATTRIBUTE_ALIGN(32);

// An interface option request's level and option, which IOS reads by DMA
static u32 option_request[8] ATTRIBUTE_ALIGN(32);

// An interface option's value, which IOS writes by DMA
static u32 option_value[8] ATTRIBUTE_ALIGN(32);

// An interface option's length, which IOS reads and writes by DMA
static u32 option_len[8] ATTRIBUTE_ALIGN(32);

// NCD's working copy of the settings, too large for the stack
static struct netconfig working;

// SO, opened on first use and never closed, since Dolphin stops serving a device once any handle to it closes
static s32 so_fd = -1;

// Write the note shown beside the step's result
static void note(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vsnprintf(note_text, sizeof note_text, format, args);
  va_end(args);
}

// Resolve name to its first IPv4 address, noting a failure, 0 when it has none
static u32 resolve(const char *name)
{
  u32 ip = sock_resolve(name);
  if (ip == 0)
    note("could not resolve %s  %s", name, sock_resolve_note());
  return ip;
}

// Convert bytes over ms milliseconds to tenths of a Mbit/s
static uint32_t to_rate_tenths(uint64_t bytes, uint32_t ms)
{
  return ms ? (uint32_t)(bytes * 8 / ((uint64_t)ms * 100)) : 0;
}

// Sort samples in place, smallest first
static void sort_samples(uint32_t *values, int count)
{
  for (int i = 1; i < count; i++) {
    uint32_t value = values[i];
    int j          = i;
    for (; j > 0 && values[j - 1] > value; j--)
      values[j] = values[j - 1];
    values[j] = value;
  }
}

// Run one iperf3 test, trying once more when it fails before any data moved
static int run_iperf_once(bool reverse, char *reason, size_t reason_len)
{
  int error = 0;
  for (int attempt = 0; attempt < IPERF_ATTEMPTS; attempt++) {
    // Wait for the server's listener to come back from the run before
    usleep(IPERF_SETTLE_US);
    error = iperf_run(iperf_server, IPERF_PORT, IPERF_SECONDS + IPERF_WARMUP_SECONDS, reverse, &iperf_result, reason,
                      reason_len);
    if (error >= 0 || iperf_result.bytes > 0)
      break;
  }
  return error;
}

// Run the iperf3 tests and note the median and percentiles of every counted second
static int measure_throughput(bool reverse)
{
  int count       = 0;
  char reason[96] = "";

  // Show the first run
  report_step_progress("run 1 of %d", IPERF_RUNS);
  for (int i = 0; i < IPERF_RUNS; i++) {
    // Run once, stopping at a failure or a cancel, which ends a run early without failing it
    int error = run_iperf_once(reverse, reason, sizeof reason);
    if (suite_cancelled())
      return -ECANCELED;
    if (error < 0) {
      note("run %d of %d, %s", i + 1, IPERF_RUNS, reason);
      return error;
    }

    // Keep the seconds after the warm up as samples, and log every second
    char line[256];
    int len = snprintf(line, sizeof line, "  %s run %d, Mbit/s each second", reverse ? "download" : "upload", i + 1);
    for (int second = 0; second < iperf_result.seconds; second++) {
      uint32_t rate = to_rate_tenths(iperf_result.second_bytes[second], 1000);
      if (second >= IPERF_WARMUP_SECONDS)
        samples[count++] = rate;
      len += snprintf(line + len, sizeof line - len, " %u.%u", (unsigned)(rate / 10), (unsigned)(rate % 10));
    }
    report_log("%s", line);

    // Show the run to come and the rate of this one
    uint32_t rate = to_rate_tenths(iperf_result.bytes, iperf_result.ms);
    if (i + 1 < IPERF_RUNS)
      report_step_progress("run %d of %d, last run %u.%u Mbit/s", i + 2, IPERF_RUNS, (unsigned)(rate / 10),
                           (unsigned)(rate % 10));
  }

  // Note the median and the 10th and 90th percentiles, in full in the log
  sort_samples(samples, count);
  uint32_t low    = samples[count / 10];
  uint32_t median = samples[count / 2];
  uint32_t high   = samples[count * 9 / 10];
  note("%u.%u Mbit/s, %u.%u to %u.%u", (unsigned)(median / 10), (unsigned)(median % 10), (unsigned)(low / 10),
       (unsigned)(low % 10), (unsigned)(high / 10), (unsigned)(high % 10));
  report_log("  median %s, 10th to 90th percentile, %d runs of %d s, %d seconds counted", note_text, IPERF_RUNS,
             IPERF_SECONDS, count);
  return 0;
}

// Open SO the first time and return the handle, or a negative error
static s32 open_so(void)
{
  if (so_fd < 0)
    so_fd = IOS_Open(SO_DEVICE, 0);
  return so_fd;
}

// Read an interface option from SO into option_value, returning its length or a negative error
static int read_interface_option(u32 option, u32 capacity)
{
  s32 fd = open_so();
  if (fd < 0)
    return fd;

  // Send the level and option, with the capacity in the length word, which SO replaces with the length written
  option_request[0] = IFOPT_LEVEL;
  option_request[1] = option;
  option_len[0]     = capacity;
  ioctlv vectors[3] = {{option_request, 8}, {option_value, capacity}, {option_len, 4}};
  s32 error         = IOS_Ioctlv(fd, SO_IOCTL_GET_INTERFACE_OPT, 1, 2, vectors);
  return error < 0 ? error : (int)option_len[0];
}

// Read the connection in use from NCD's working copy, NULL when NCD cannot say
static const struct netconfig_profile *find_selected_profile(void)
{
  int selected = netconfig_load_working(&working) < 0 ? -1 : netconfig_find_selected(&working);
  return selected < 0 ? NULL : &working.profile[selected];
}

// Describe NCD's link status word
static const char *describe_ncd_status(int status)
{
  switch (status) {
    case 1:
      // A fault in the Wi-Fi driver
      return "Wi-Fi fault";

    case 2:
      // No interface is selected
      return "no network interface";

    case NCD_STATUS_WIRED:
      // A wired profile
      return "wired network";

    case 4:
      // A wireless link that is down
      return "Wi-Fi link down";

    case NCD_STATUS_WIFI_UP:
      // A wireless link that is up
      return "Wi-Fi link up";

    default:
      // Anything else
      return "Wi-Fi state unknown";
  }
}

int step_link_up(void)
{
  note_text[0] = 0;
  int error    = net_init();
  link_is_up   = error >= 0;

  // Stop KD's stack after a failed bring up, which can leave it started, so the next run starts from down
  if (error < 0) {
    step_link_down();
    note("could not bring the link up");
    return error;
  }

  // Note the network the link joined, wired or the SSID
  const struct netconfig_profile *profile = find_selected_profile();
  if (profile && profile->flags & NETCONFIG_FLAG_WIRED) {
    note("wired");
  } else if (profile) {
    note("joined %.*s", netconfig_ssid_len(profile), (const char *)profile->ssid);
  }
  return 0;
}

int step_address(void)
{
  note_text[0] = 0;
  u32 ip       = net_gethostip();
  if (ip == 0) {
    note("no address");
    return -1;
  }

  // Note the address and how it came
  const struct netconfig_profile *profile = find_selected_profile();
  const char *kind = !profile ? "" : profile->flags & NETCONFIG_FLAG_DHCP_IP ? " via DHCP" : " set manually";
  int len          = snprintf(note_text, sizeof note_text, "IP:      %s%s", sock_ip_text(ip), kind);

  // Add the DNS servers and the netmask on lines of their own when SO answers for them
  int dns_len = read_interface_option(IFOPT_DNS_SERVERS, 8);
  for (int i = 0; i < dns_len / 4 && option_value[i] != 0; i++)
    len +=
      snprintf(note_text + len, sizeof note_text - len, i ? " %s" : "\nDNS:     %s", sock_ip_text(option_value[i]));
  if (read_interface_option(IFOPT_ADDRESS, 12) >= 12)
    snprintf(note_text + len, sizeof note_text - len, "\nNetmask: %s", sock_ip_text(option_value[1]));
  return 0;
}

int step_link_down(void)
{
  note_text[0] = 0;

  // Stop the socket layer through KD, then release libogc's socket handle, the order SOCleanup uses
  s32 fd = IOS_Open(KD_DEVICE, 0);
  if (fd < 0)
    return fd;
  s32 error = IOS_Ioctl(fd, KD_IOCTL_CLEANUP, NULL, 0, cleanup_reply, sizeof cleanup_reply);
  IOS_Close(fd);
  net_deinit();
  link_is_up = false;

  // Fail on a failure in KD's first reply word, which can come with a request that succeeded
  if (error >= 0 && cleanup_reply[0] != 0) {
    note("KD cleanup reply %d", (int)cleanup_reply[0]);
    return cleanup_reply[0];
  }
  return error;
}

int step_dns(void)
{
  note_text[0] = 0;
  u32 ip       = resolve(DNS_HOST);
  if (ip == 0)
    return -1;

  note("%s %s  %s", DNS_HOST, sock_ip_text(ip), sock_resolve_note());
  return 0;
}

int step_tcp(void)
{
  note_text[0] = 0;

  // Connect to the server
  u32 ip = resolve(TCP_HOST);
  if (ip == 0)
    return -1;
  s32 socket_fd = sock_connect(ip, HTTP_PORT);
  if (socket_fd < 0) {
    note("connect to %s failed", sock_ip_text(ip));
    return socket_fd;
  }

  // Send a minimal HTTP/1.1 request, which the server answers and then closes
  int len   = snprintf(http_buffer, sizeof http_buffer, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                       TCP_PATH, TCP_HOST);
  s32 error = sock_send_all(socket_fd, http_buffer, (size_t)len);
  if (error < 0) {
    note("send failed");
    goto done;
  }

  // Read the first piece of the reply, which holds the status line
  error = sock_wait_readable(socket_fd, REPLY_TIMEOUT_MS);
  if (error <= 0) {
    note(error < 0 ? "poll failed" : "no reply within %d ms", REPLY_TIMEOUT_MS);
    error = error < 0 ? error : -ETIMEDOUT;
    goto done;
  }
  error = net_recv(socket_fd, http_buffer, sizeof http_buffer - 1, 0);
  if (error <= 0) {
    note(error < 0 ? "receive failed" : "closed without a reply");
    error = error < 0 ? error : -ECONNRESET;
    goto done;
  }
  http_buffer[error] = 0;

  // Check the status against the one expected
  int status = 0;
  if (sscanf(http_buffer, "HTTP/%*d.%*d %d", &status) != 1) {
    note("%s answered something other than HTTP", TCP_HOST);
    error = -1;
    goto done;
  }
  note("%s answered %d", TCP_PATH, status);
  error = status == TCP_STATUS ? 0 : -1;

done:
  net_close(socket_fd);
  return error;
}

int step_udp(void)
{
  note_text[0] = 0;

  // Open a datagram socket to the server
  u32 ip = resolve(NTP_HOST);
  if (ip == 0)
    return -1;
  s32 socket_fd = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (socket_fd < 0) {
    note("could not open a socket");
    return socket_fd;
  }
  struct sockaddr_in sa;
  sock_address(&sa, ip, NTP_PORT);

  // Ask again after a missing or malformed reply
  s32 error = -ETIMEDOUT;
  for (int attempt = 0; attempt < NTP_ATTEMPTS; attempt++) {
    // Send a client request, leap indicator 0, version 4, mode 3
    memset(ntp_packet, 0, sizeof ntp_packet);
    ntp_packet[0] = 0x23;
    error         = net_sendto(socket_fd, ntp_packet, sizeof ntp_packet, 0, (struct sockaddr *)&sa, SOCK_ADDR_LEN);
    if (error < 0) {
      note("send to %s failed", NTP_HOST);
      break;
    }

    // Wait for the reply
    error = sock_wait_readable(socket_fd, REPLY_TIMEOUT_MS);
    if (error < 0) {
      note("poll failed");
      break;
    }
    if (error == 0) {
      note("no reply from %s", NTP_HOST);
      error = -ETIMEDOUT;
      continue;
    }

    // Read it
    struct sockaddr_in from;
    socklen_t from_len = sizeof from;
    error              = net_recvfrom(socket_fd, ntp_packet, sizeof ntp_packet, 0, (struct sockaddr *)&from, &from_len);
    if (error < 0) {
      note("receive failed");
      break;
    }

    // Check it is a server reply, mode 4 with a stratum from 1 to 15
    if (error < NTP_PACKET_LEN || (ntp_packet[0] & 7) != 4 || ntp_packet[1] == 0 || ntp_packet[1] > 15) {
      note("malformed reply from %s", NTP_HOST);
      error = -1;
      continue;
    }

    // Note the transmit timestamp's seconds, big endian at offset 40
    u32 seconds =
      ((u32)ntp_packet[40] << 24) | ((u32)ntp_packet[41] << 16) | ((u32)ntp_packet[42] << 8) | ntp_packet[43];
    note("%s stratum %u, unix time %u", NTP_HOST, ntp_packet[1], (unsigned)(seconds - NTP_UNIX_OFFSET));
    error = 0;
    break;
  }

  net_close(socket_fd);
  return error;
}

int step_server(void)
{
  note_text[0] = 0;

  // Connect to the server
  u32 ip = resolve(iperf_server);
  if (ip == 0)
    return -1;
  s32 socket_fd = sock_connect(ip, IPERF_PORT);
  if (socket_fd < 0) {
    note("could not connect to %s:%d", iperf_server, IPERF_PORT);
    return socket_fd;
  }

  // A busy server writes its refusal at once, while a free one waits for the client's cookie
  int ready = sock_wait_readable(socket_fd, SERVER_PROBE_MS);
  net_close(socket_fd);
  if (ready > 0) {
    note("%s busy with another test", iperf_server);
    return -EBUSY;
  }
  if (ready < 0) {
    note("%s closed the connection", iperf_server);
    return ready;
  }
  note("%s:%d listening", iperf_server, IPERF_PORT);
  return 0;
}

int step_upload(void)
{
  note_text[0] = 0;
  return measure_throughput(false);
}

int step_download(void)
{
  note_text[0] = 0;
  return measure_throughput(true);
}

bool steps_link_is_up(void)
{
  return link_is_up;
}

const char *steps_note(void)
{
  return note_text;
}

void steps_finish(void)
{
  if (link_is_up)
    step_link_down();
}

void steps_report_link_state(void)
{
  int ncd = -1;
  u32 ip  = 0;

  // Read NCD's view of the wireless link, from the second reply word
  s32 fd = IOS_Open(NCD_DEVICE, 0);
  if (fd >= 0) {
    ioctlv vector = {link_status, sizeof link_status};
    if (IOS_Ioctlv(fd, NCD_IOCTL_LINK_STATUS, 0, 1, &vector) >= 0 && link_status[0] == 0)
      ncd = (int)link_status[1];
    IOS_Close(fd);
  }

  // Read the address SO holds, 0 while DHCP has not completed
  if (open_so() >= 0)
    ip = (u32)IOS_Ioctl(so_fd, SO_IOCTL_GET_HOST_ID, NULL, 0, NULL, 0);

  // Name the layer that stopped on screen, only when the link is at fault, and log the raw values
  bool link_up = ncd == NCD_STATUS_WIRED || ncd == NCD_STATUS_WIFI_UP;
  if (!link_up && ip) {
    report_cause("%s, IP address %s", describe_ncd_status(ncd), sock_ip_text(ip));
  } else if (!ip) {
    report_cause("%s, no IP address", describe_ncd_status(ncd));
  }
  report_log("       NCD link status %d, SO host id %08x", ncd, (unsigned)ip);
}
