#pragma once

#include <stdbool.h>

// Bring the network up from the console's profile, which joins through WD and runs DHCP
int step_link_up(void);

// Read back the address DHCP assigned and report it
int step_address(void);

// Stop the network interface and take the WD link down, like a game that leaves online play
int step_link_down(void);

// Resolve the configured DNS name and report its address
int step_dns(void);

// Request the configured URL over HTTP on port 80 and check the status code
int step_tcp(void);

// Query the configured NTP server and check that a valid reply comes back
int step_udp(void);

// Check that the configured iperf3 server is listening and not busy with another test
int step_server(void);

// Send to the configured iperf3 server for the configured time and report the rate
int step_upload(void);

// Receive from the configured iperf3 server for the configured time and report the rate
int step_download(void);

// The link is up from an earlier step
bool steps_link_is_up(void);

// What the last step found or where it stopped, empty when it has nothing to add
const char *steps_note(void);

// Take the link down if a step left it up, so the next test starts from a known state
void steps_finish(void);

// Report under a failed step whether the Wi-Fi link is up and whether SO holds an address
void steps_report_link_state(void);
