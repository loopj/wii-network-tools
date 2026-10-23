#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>

#include "sock.h"

// Longest single poll while waiting, since a poll can return before its own timeout
#define POLL_SLICE_MS 100

// Time allowed for a connection to finish opening
#define CONNECT_WAIT_MS 5000

// Size of the hostent block IOS SO returns
#define HOSTENT_LEN 0x460

// What the last resolve found beyond its address, empty when nothing
static char resolve_note[64];

// The text sock_ip_text returns
static char ip_text[16];

// Wait for the socket to report any of events, returning 1 when it does, 0 on timeout or a negative error
static int wait_for_events(int socket_fd, u32 events, int timeout_ms)
{
  // Poll in slices against a deadline kept here
  u64 start = gettime();
  do {
    struct pollsd entry = {socket_fd, events, 0};
    int ready           = net_poll(&entry, 1, timeout_ms < POLL_SLICE_MS ? timeout_ms : POLL_SLICE_MS);
    if (ready < 0)
      return ready;

    // Fail on an error even with events set, since a failed connect also reports writable, and on a bare hang up
    if (ready > 0) {
      if (entry.revents & POLLERR)
        return -ECONNREFUSED;
      if (entry.revents & events)
        return 1;
      return -ECONNRESET;
    }
  } while (diff_msec(start, gettime()) < (u64)timeout_ms);
  return 0;
}

uint32_t sock_resolve(const char *name)
{
  // Take a dotted address as it is, since IOS's resolver refuses one
  resolve_note[0] = 0;
  struct in_addr address;
  if (inet_aton(name, &address))
    return address.s_addr;

  // Resolve the name
  struct hostent *host = net_gethostbyname(name);
  if (!host)
    return 0;

  // Note a pointer outside the 0x460 byte block instead of following it, since a torn header relocates it there
  uint32_t base = (uint32_t)host;
  uint32_t list = (uint32_t)host->h_addr_list;
  if (list < base || list >= base + HOSTENT_LEN) {
    snprintf(resolve_note, sizeof resolve_note, "hostent list at +0x%x", (unsigned)(list - base));
    return 0;
  }
  uint32_t first = (uint32_t)host->h_addr_list[0];
  if (first == 0)
    return 0;
  if (first < base || first >= base + HOSTENT_LEN) {
    snprintf(resolve_note, sizeof resolve_note, "hostent address at +0x%x", (unsigned)(first - base));
    return 0;
  }

  // Read the first address
  uint32_t ip;
  memcpy(&ip, (const void *)first, sizeof ip);
  return ip;
}

const char *sock_resolve_note(void)
{
  return resolve_note;
}

void sock_address(struct sockaddr_in *sa, uint32_t ip, uint16_t port)
{
  memset(sa, 0, sizeof *sa);
  sa->sin_len         = SOCK_ADDR_LEN;
  sa->sin_family      = AF_INET;
  sa->sin_port        = htons(port);
  sa->sin_addr.s_addr = ip;
}

int sock_connect(uint32_t ip, uint16_t port)
{
  s32 socket_fd = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (socket_fd < 0)
    return socket_fd;

  // Connect to the address
  struct sockaddr_in sa;
  sock_address(&sa, ip, port);
  s32 result = net_connect(socket_fd, (struct sockaddr *)&sa, SOCK_ADDR_LEN);

  // Wait until the socket can be written, since connect can return while the handshake is still under way
  if (result >= 0) {
    result = wait_for_events(socket_fd, POLLOUT, CONNECT_WAIT_MS);
    result = result > 0 ? 0 : result < 0 ? result : -ETIMEDOUT;
  }

  // Close it on a failure
  if (result < 0) {
    net_close(socket_fd);
    return result;
  }
  return socket_fd;
}

int sock_wait_readable(int socket_fd, int timeout_ms)
{
  return wait_for_events(socket_fd, POLLIN, timeout_ms);
}

int sock_send_all(int socket_fd, const void *data, size_t len)
{
  // Send until every byte is out, failing on an error or a closed connection
  const char *next = data;
  while (len > 0) {
    s32 sent = net_send(socket_fd, next, (s32)len, 0);
    if (sent < 0)
      return sent;
    if (sent == 0)
      return -ECONNRESET;
    next += sent;
    len -= (size_t)sent;
  }
  return 0;
}

int sock_recv_all(int socket_fd, void *data, size_t len, int timeout_ms)
{
  char *next = data;
  while (len > 0) {
    // Wait for data, failing on an error or the timeout
    int ready = sock_wait_readable(socket_fd, timeout_ms);
    if (ready < 0)
      return ready;
    if (ready == 0)
      return -ETIMEDOUT;

    // Take what arrived, failing on an error or a closed connection
    s32 received = net_recv(socket_fd, next, (s32)len, 0);
    if (received < 0)
      return received;
    if (received == 0)
      return -ECONNRESET;
    next += received;
    len -= (size_t)received;
  }
  return 0;
}

const char *sock_ip_text(uint32_t ip)
{
  snprintf(ip_text, sizeof ip_text, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 0xff,
           (unsigned)(ip >> 8) & 0xff, (unsigned)ip & 0xff);
  return ip_text;
}
