#pragma once

#include <stddef.h>
#include <stdint.h>

#include <network.h>

// Length of IOS SO's IPv4 address with no padding, which libogc's sendto passes through and IOS insists on
#define SOCK_ADDR_LEN 8

// Resolve a name or dotted address to an IPv4 address, 0 when it has none
uint32_t sock_resolve(const char *name);

// What the last resolve found beyond its address, an empty string when nothing
const char *sock_resolve_note(void);

// Fill sa with an IPv4 address and port in the form IOS SO expects
void sock_address(struct sockaddr_in *sa, uint32_t ip, uint16_t port);

// Open a TCP connection to ip and port, returning the socket or a negative error
int sock_connect(uint32_t ip, uint16_t port);

// Wait for the socket to become readable, 1 when it is, 0 on timeout, a negative error otherwise
int sock_wait_readable(int socket_fd, int timeout_ms);

// Send all len bytes, returning 0 or a negative error
int sock_send_all(int socket_fd, const void *data, size_t len);

// Receive exactly len bytes within timeout_ms, returning 0, a negative error, or -ETIMEDOUT
int sock_recv_all(int socket_fd, void *data, size_t len, int timeout_ms);

// Format an IPv4 address as dotted decimal into a static buffer
const char *sock_ip_text(uint32_t ip);
