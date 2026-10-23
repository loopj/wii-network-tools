#pragma once

// Longest iperf3 server name accepted
#define SETTINGS_NAME_MAX 128

// The iperf3 server Speed Test uses, empty until one is entered
extern char iperf_server[SETTINGS_NAME_MAX];

// Read the saved settings when the app runs as a disc, which also turns saving on
void settings_init(void);

// Show the app's own settings until the user goes back
void settings_open(void);
