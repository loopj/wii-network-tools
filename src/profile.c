#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/cond.h>

#include "netconfig.h"

#include "profile.h"

// NCD's working copy, shared by the two calls since it is too large for the stack
static struct netconfig config;

// Worker that makes the NCD calls, since each blocks until NCD answers
static lwp_t worker = LWP_THREAD_NULL;

// Guards the request and the description shared with the worker
static mutex_t lock;

// Wakes the worker for a request
static cond_t wake;

// Step of the request waiting or running
static int request_step;

// A request is waiting or running
static bool request_busy;

// The description the menu shows
static char shown[64];

// The description changed since the menu last read it
static bool shown_changed;

// Describe the selected connection in the working copy as its number and wired or its SSID
static void describe(char *text, size_t len)
{
  int selected = netconfig_load_working(&config) < 0 ? -1 : netconfig_find_selected(&config);
  if (selected < 0) {
    snprintf(text, len, "None selected");
    return;
  }

  // Write its number, then wired or its SSID
  const struct netconfig_profile *profile = &config.profile[selected];
  if (profile->flags & NETCONFIG_FLAG_WIRED) {
    snprintf(text, len, "Connection %d  Wired", selected + 1);
  } else {
    snprintf(text, len, "Connection %d  %.*s", selected + 1, netconfig_ssid_len(profile), (const char *)profile->ssid);
  }
}

// Mark the next configured profile selected in NCD's working copy, or the one before for a negative step
static int switch_profile(int step)
{
  s32 error = netconfig_load_working(&config);
  if (error < 0)
    return error;

  // Walk from the selected profile, or from before the first when none is, to the next slot that holds a profile
  int selected = netconfig_find_selected(&config);
  int index    = selected >= 0 ? selected : (step > 0 ? -1 : NETCONFIG_PROFILES);
  for (int tries = 0; tries < NETCONFIG_PROFILES; tries++) {
    index = (index + step + NETCONFIG_PROFILES) % NETCONFIG_PROFILES;
    if (config.profile[index].flags != 0)
      break;
  }
  if (index == selected || config.profile[index].flags == 0)
    return 0;

  // Leave only the chosen profile selected, and have NCD take the copy as its working one
  if (selected >= 0)
    config.profile[selected].flags &= ~NETCONFIG_FLAG_SELECTED;
  config.profile[index].flags |= NETCONFIG_FLAG_SELECTED;
  return netconfig_apply(&config);
}

// Wait for each request, run it and publish the new description
static void *run_worker(void *arg)
{
  (void)arg;
  while (true) {
    // Wait for a request
    LWP_MutexLock(lock);
    while (!request_busy)
      LWP_CondWait(wake, lock);
    int step = request_step;
    LWP_MutexUnlock(lock);

    // Switch NCD first when asked, then read it back
    if (step)
      switch_profile(step);
    char text[sizeof shown];
    describe(text, sizeof text);

    // Publish the description and finish the request
    LWP_MutexLock(lock);
    memcpy(shown, text, sizeof shown);
    shown_changed = true;
    request_busy  = false;
    LWP_MutexUnlock(lock);
  }
  return NULL;
}

void profile_request(int step)
{
  // Start the worker on the first request
  if (worker == LWP_THREAD_NULL) {
    LWP_MutexInit(&lock, false);
    LWP_CondInit(&wake);
    LWP_CreateThread(&worker, run_worker, NULL, NULL, 16 * 1024, 50);
  }

  // Hand the worker the request, showing a switch at once and keeping the description for a read until the answer
  LWP_MutexLock(lock);
  if (!request_busy) {
    request_step = step;
    request_busy = true;
    if (step) {
      snprintf(shown, sizeof shown, "Switching...");
      shown_changed = true;
    }
    LWP_CondSignal(wake);
  }
  LWP_MutexUnlock(lock);
}

bool profile_poll(char *text, size_t len)
{
  // Copy the description out when it changed, and mark it read
  LWP_MutexLock(lock);
  bool changed = shown_changed;
  if (changed)
    snprintf(text, len, "%s", shown);
  shown_changed = false;
  LWP_MutexUnlock(lock);
  return changed;
}

bool profile_busy(void)
{
  LWP_MutexLock(lock);
  bool busy = request_busy;
  LWP_MutexUnlock(lock);
  return busy;
}
