#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <unity.h>
#include <ogc/lwp_watchdog.h>

#include "advanced.h"
#include "report.h"

#include "wd_suite.h"

// NCD's manage device, which lends the wireless driver out
#define NCD_MANAGE "/dev/net/ncd/manage"

// Time NCD is given to close its own session after the lock
#define NCD_RELEASE_MS 1000

// The driver device under test
#define WD_DEVICE "/dev/net/wd/command"

// Open mode bit for raw buffers rather than iobufs
#define WD_OPEN_RAW (1 << 16)

// Size of the info block
#define WD_INFO_LEN 0x90

// Size of the config block
#define WD_CONFIG_LEN 0x180

// Size of the scan parameters
#define WD_SCAN_PARAMS_LEN 0x4e

// Size of the scan result buffer
#define WD_SCAN_RESULT_LEN 0x1000

// Size of the WL status
#define WD_WL_STATUS_LEN 0x10

// Size of the notification buffer, which holds every record including the stock WD's 0x94 byte type 4
#define WD_NOTIFY_LEN 0x800

// Offset of the SSID length in the config block
#define CONFIG_SSID_LEN 0x9c

// Length of a disconnected record
#define NOTIFY_DISCONNECTED_LEN 0x14

// Length of a connected record
#define NOTIFY_CONNECTED_LEN 0x34

// NCD's connect mask for essSta
#define MASK_ESS_STA 0x00ff01fc

// NCD's scan dwell time per channel, in milliseconds
#define SCAN_CHANNEL_TIME 40

// Time NCD gives a join
#define LINK_WAIT_MS 7000

// Time allowed for a scan after an aborted join
#define SCAN_WAIT_MS 5000

// How long a parked notification is watched to show nothing arrives
#define QUIET_MS 500

// Fill byte that shows how much of the notification buffer an answer wrote
#define NOTIFY_FILL 0xee

// Requests on NCD_MANAGE that lend the driver out and take it back
enum ncd_request {
  NCD_LOCK_DRIVER   = 1,
  NCD_UNLOCK_DRIVER = 2,
};

// Requests under test, with the stock WD's numbers
enum wd_request {
  WD_GET_STATE            = 0x1001,
  WD_SET_LINK_STATE       = 0x1002,
  WD_GET_STATUS           = 0x1003,
  WD_GET_CONFIG           = 0x1005,
  WD_SEND_BEACON          = 0x1006,
  WD_DISASSOC             = 0x1007,
  WD_MP_SEND_FRAME        = 0x1008,
  WD_SCAN                 = 0x100a,
  WD_MEASURE_CHANNEL      = 0x100b,
  WD_CALL_WL              = 0x100c,
  WD_GET_WL_STATUS        = 0x100d,
  WD_GET_INFO             = 0x100e,
  WD_SET_BEACON           = 0x100f,
  WD_CHANGE_VTSF          = 0x1010,
  WD_UNKNOWN              = 0x1011,
  WD_RECEIVE_NOTIFICATION = 0x8001,
};

// Result codes the stock WD returns
enum wd_result {
  WD_ECLOSED = (s32)0x80008000,
  WD_EINVAL  = (s32)0x80008001,
  WD_ESTATE  = (s32)0x80008002,
  WD_ESHORT  = (s32)0x80008004,
  WD_EFAIL   = -4,
};

// Notification record types
enum notify_type {
  NOTIFY_CONNECTED    = 0,
  NOTIFY_DISCONNECTED = 1,
  NOTIFY_IGNORED      = 4,
};

// The request's input, which IOS reads by DMA
static u8 input_buffer[0x80] ATTRIBUTE_ALIGN(32);

// The request's output, which IOS writes by DMA
static u8 output_buffer[WD_SCAN_RESULT_LEN] ATTRIBUTE_ALIGN(32);

// The parked notification's output
static u8 notify_buffer[WD_NOTIFY_LEN] ATTRIBUTE_ALIGN(32);

// The scan parameters NCD sends
static u8 scan_params[WD_SCAN_PARAMS_LEN + 2] ATTRIBUTE_ALIGN(32);

// The asynchronous scan's output
static u8 async_scan_buffer[WD_SCAN_RESULT_LEN] ATTRIBUTE_ALIGN(32);

// The config read's masks, mpParent then essSta
static u32 config_masks[2] ATTRIBUTE_ALIGN(32);

// NCD's answer to the lock and unlock
static s32 lock_reply[8] ATTRIBUTE_ALIGN(32);

// The lock id sent back with the unlock
static s32 lock_id[8] ATTRIBUTE_ALIGN(32);

// The request's vectors
static ioctlv vectors[2] ATTRIBUTE_ALIGN(32);

// The parked notification's vector
static ioctlv notify_vector[1] ATTRIBUTE_ALIGN(32);

// The asynchronous scan's vectors
static ioctlv async_scan_vectors[2] ATTRIBUTE_ALIGN(32);

// The lock and unlock's vectors
static ioctlv lock_vectors[2] ATTRIBUTE_ALIGN(32);

// The session's handle for the run
static s32 wd_fd = -1;

// The second open of the session
static s32 second_fd = -1;

// The info block read early, whose channel bitmap every scan uses
static u8 info[WD_INFO_LEN];

// A notification is parked, cleared from the IPC callback
static volatile bool notify_pending;

// The parked notification's answer
static volatile s32 notify_result;

// An asynchronous scan is running, cleared from the IPC callback
static volatile bool scan_pending;

// The asynchronous scan's answer
static volatile s32 scan_result;

// Output length of the next parks, the whole buffer unless a test probes a shorter one
static u32 notify_len = WD_NOTIFY_LEN;

// Parks refused as too short by wait_for_event while a test probes a short output
static u32 refused_count;

// The failure message for a notification that came early
static char early_message[48];

// Send one request on a handle with an optional input and output, the output cleared first
static s32 send_request_on(s32 fd, u32 number, const void *in, u32 in_len, u32 out_len)
{
  memset(output_buffer, 0, sizeof output_buffer);

  // Pass the input as the first vector when there is one
  int in_count = 0;
  if (in_len) {
    memcpy(input_buffer, in, in_len);
    vectors[0].data = input_buffer;
    vectors[0].len  = in_len;
    in_count        = 1;
  }

  // Pass the output after it
  if (out_len) {
    vectors[in_count].data = output_buffer;
    vectors[in_count].len  = out_len;
  }
  return IOS_Ioctlv(fd, number, in_count, out_len ? 1 : 0, vectors);
}

// Send one request on the session
static s32 send_request(u32 number, const void *in, u32 in_len, u32 out_len)
{
  return send_request_on(wd_fd, number, in, in_len, out_len);
}

// Read a big endian word
static u32 read_word(const u8 *at)
{
  return ((u32)at[0] << 24) | ((u32)at[1] << 16) | ((u32)at[2] << 8) | at[3];
}

// Ask for the link or drop it
static s32 set_link_state(u32 up)
{
  u8 value[4] = {0, 0, 0, (u8)up};
  return send_request(WD_SET_LINK_STATE, value, 4, 0);
}

// Read the link status, which the stock WD answers as the result
static s32 read_link_status(void)
{
  return send_request(WD_GET_STATUS, NULL, 0, 0);
}

// Fill the scan parameters NCD sends, every enabled channel, the broadcast BSSID and any SSID
static void fill_scan_params(void)
{
  memset(scan_params, 0, sizeof scan_params);
  scan_params[0] = info[6];
  scan_params[1] = info[7];
  scan_params[3] = SCAN_CHANNEL_TIME;
  memset(scan_params + 4, 0xff, 6);
}

// Record a notification's answer
static s32 handle_notification(s32 result, void *arg)
{
  (void)arg;
  notify_result  = result;
  notify_pending = false;
  return 0;
}

// Park a ReceiveNotification, unless one is still parked from a failed test
static void park_notification(void)
{
  if (notify_pending)
    return;

  // Fill the buffer so the answer's length shows
  memset(notify_buffer, NOTIFY_FILL, sizeof notify_buffer);
  DCFlushRange(notify_buffer, sizeof notify_buffer);

  // Park the request with the current output length
  notify_vector[0].data = notify_buffer;
  notify_vector[0].len  = notify_len;
  notify_pending        = true;
  IOS_IoctlvAsync(wd_fd, WD_RECEIVE_NOTIFICATION, 0, 1, notify_vector, handle_notification, NULL);
}

// Wait up to ms for an IPC callback to clear a pending flag
static void wait_for_answer(volatile bool *pending, u32 ms)
{
  u64 start = gettime();
  while (*pending && diff_msec(start, gettime()) < ms)
    usleep(10000);
}

// Wait up to ms for the parked notification, true when it was answered
static bool wait_for_notification(u32 ms)
{
  // Wait for the answer, then drop any stale cache lines over what IOS wrote
  wait_for_answer(&notify_pending, ms);
  DCInvalidateRange(notify_buffer, sizeof notify_buffer);
  return !notify_pending;
}

// Bytes of the notification buffer the last answer wrote, from the fill left behind
static u32 count_notify_written(void)
{
  u32 written = sizeof notify_buffer;
  while (written && notify_buffer[written - 1] == NOTIFY_FILL)
    written--;
  return written;
}

// Wait up to ms for a notification other than type 4, parking again after each type 4, like NCD, and each refusal
static bool wait_for_event(u32 ms)
{
  u64 start = gettime();
  while (wait_for_notification(ms - diff_msec(start, gettime()))) {
    // Count a refusal of a short output, and stop at any answer but type 4
    if (notify_result == WD_ESHORT && notify_len < WD_NOTIFY_LEN) {
      refused_count++;
    } else if (notify_result < 0 || read_word(notify_buffer) != NOTIFY_IGNORED) {
      return true;
    }

    // Park again unless the time is up
    park_notification();
    if (diff_msec(start, gettime()) >= ms)
      return false;
  }
  return false;
}

// Record a scan's answer
static s32 handle_scan(s32 result, void *arg)
{
  (void)arg;
  scan_result  = result;
  scan_pending = false;
  return 0;
}

// Refuse an open in a state above 6
static void test_open_refuses_state_above_6(void)
{
  // Open in a state above 6, which the stock WD takes from the mode's low bits and refuses with -4
  s32 fd = IOS_Open(WD_DEVICE, WD_OPEN_RAW | 7);
  if (fd >= 0)
    IOS_Close(fd);
  TEST_ASSERT_EQUAL_INT(WD_EFAIL, fd);
}

// Open in the state the mode names
static void test_open_sets_state_from_mode(void)
{
  // Open raw in state 3, like the System Menu, and read the state back as GetState's result
  wd_fd = IOS_Open(WD_DEVICE, WD_OPEN_RAW | 3);
  TEST_ASSERT_TRUE(wd_fd >= 0);
  TEST_ASSERT_EQUAL_INT(3, send_request(WD_GET_STATE, NULL, 0, 0));
}

// Share the session with a second open
static void test_second_open_shares_session(void)
{
  // A second open while the session exists gets the same session rather than an error
  second_fd = IOS_Open(WD_DEVICE, WD_OPEN_RAW | 3);
  TEST_ASSERT_TRUE(second_fd >= 0);
  TEST_ASSERT_EQUAL_INT(3, send_request_on(second_fd, WD_GET_STATE, NULL, 0, 0));
}

// Keep the config NCD set after NCD closes its session
static void test_config_kept_from_ncd(void)
{
  // Read the config NCD set, which survives NCD's close and which the link tests below rely on
  config_masks[0] = 0;
  config_masks[1] = MASK_ESS_STA;
  memset(output_buffer, 0, WD_CONFIG_LEN);
  vectors[0].data = output_buffer;
  vectors[0].len  = WD_CONFIG_LEN;
  vectors[1].data = config_masks;
  vectors[1].len  = sizeof config_masks;
  TEST_ASSERT_EQUAL_INT(0, IOS_Ioctlv(wd_fd, WD_GET_CONFIG, 2, 0, vectors));

  // Check NCD left an SSID
  DCInvalidateRange(output_buffer, WD_CONFIG_LEN);
  TEST_ASSERT_NOT_EQUAL(0, output_buffer[CONFIG_SSID_LEN]);
}

// Answer link down before a join
static void test_status_link_down(void)
{
  TEST_ASSERT_EQUAL_INT(0, read_link_status());
}

// Answer the info block with the MAC address and the channels
static void test_info_has_mac_and_channels(void)
{
  // NCD reads the MAC and the enabled channel bitmap from here once at startup
  TEST_ASSERT_EQUAL_INT(0, send_request(WD_GET_INFO, NULL, 0, WD_INFO_LEN));

  // Keep the block for the scans and check its MAC address and channels
  memcpy(info, output_buffer, sizeof info);
  TEST_ASSERT_TRUE(info[0] | info[1] | info[2] | info[3] | info[4] | info[5]);
  TEST_ASSERT_NOT_EQUAL(0, (info[6] << 8) | info[7]);
}

// Answer the WL status
static void test_wl_status_reads(void)
{
  TEST_ASSERT_EQUAL_INT(0, send_request(WD_GET_WL_STATUS, NULL, 0, WD_WL_STATUS_LEN));
}

// Find networks in a scan
static void test_scan_finds_networks(void)
{
  // Scan up to three times, since a scan may come back empty from the stock driver too and NCD scans again on a timer
  u32 count = 0;
  for (int attempt = 0; attempt < 3 && count == 0; attempt++) {
    fill_scan_params();
    TEST_ASSERT_EQUAL_INT(0, send_request(WD_SCAN, scan_params, WD_SCAN_PARAMS_LEN, WD_SCAN_RESULT_LEN));
    count = (output_buffer[0] << 8) | output_buffer[1];
  }
  TEST_ASSERT_NOT_EQUAL(0, count);
}

// Refuse a scan into an output too short for it
static void test_scan_short_buffer_refused(void)
{
  // Scan into a raw output too short for the result, which is refused whole, like stock WD
  fill_scan_params();
  TEST_ASSERT_EQUAL_HEX32(WD_ESHORT, send_request(WD_SCAN, scan_params, WD_SCAN_PARAMS_LEN, 0x40));
}

// Notify a connection after a link up
static void test_link_up_notifies_connected(void)
{
  // Ask for the link, which answers at once, before the notification comes
  park_notification();
  TEST_ASSERT_EQUAL_INT(0, set_link_state(1));
  TEST_ASSERT_TRUE(notify_pending);

  // Wait for the connected notification inside NCD's limit
  TEST_ASSERT_TRUE(wait_for_event(LINK_WAIT_MS));
  TEST_ASSERT_GREATER_OR_EQUAL_INT32(0, notify_result);
  TEST_ASSERT_EQUAL_HEX32(NOTIFY_CONNECTED, read_word(notify_buffer));

  // Check the record is 0x34 bytes, like stock WD, and the status follows it
  TEST_ASSERT_EQUAL_UINT32(NOTIFY_CONNECTED_LEN, read_word(notify_buffer + 4));
  TEST_ASSERT_EQUAL_UINT32(NOTIFY_CONNECTED_LEN, count_notify_written());
  TEST_ASSERT_EQUAL_INT(1, read_link_status());
}

// Refuse a scan while the link is up
static void test_scan_refused_while_link_up(void)
{
  // Scan with the link up, which is refused as bad state, like stock WD
  fill_scan_params();
  TEST_ASSERT_EQUAL_HEX32(WD_ESTATE, send_request(WD_SCAN, scan_params, WD_SCAN_PARAMS_LEN, WD_SCAN_RESULT_LEN));
}

// Notify a disconnection after a link down
static void test_link_down_notifies_disconnected(void)
{
  // Check no notification but type 4 arrives while the link stays up, since the stock WD sends type 4 after a join
  park_notification();
  if (wait_for_event(QUIET_MS)) {
    snprintf(early_message, sizeof early_message, "answered early with %d, type %08x", (int)notify_result,
             (unsigned)read_word(notify_buffer));
    TEST_FAIL_MESSAGE(early_message);
  }

  // Drop the link and wait for the disconnected notification
  TEST_ASSERT_EQUAL_INT(0, set_link_state(0));
  TEST_ASSERT_TRUE(wait_for_event(LINK_WAIT_MS));
  TEST_ASSERT_EQUAL_HEX32(NOTIFY_DISCONNECTED, read_word(notify_buffer));
  TEST_ASSERT_EQUAL_INT(0, read_link_status());
}

// Fit a disconnection into an output of exactly its length
static void test_disconnect_fits_record_length(void)
{
  // Bring the link up, since a raw output is checked against the record's own length, like stock WD
  park_notification();
  TEST_ASSERT_EQUAL_INT(0, set_link_state(1));
  TEST_ASSERT_TRUE(wait_for_event(LINK_WAIT_MS));
  TEST_ASSERT_EQUAL_HEX32(NOTIFY_CONNECTED, read_word(notify_buffer));

  // Park an output of exactly the record's length and drop the link
  notify_len    = NOTIFY_DISCONNECTED_LEN;
  refused_count = 0;
  park_notification();
  TEST_ASSERT_EQUAL_INT(0, set_link_state(0));

  // Wait for the disconnection and restore the full length for later parks
  bool answered = wait_for_event(LINK_WAIT_MS);
  notify_len    = WD_NOTIFY_LEN;

  // Check the record fills the output exactly and no park was refused as too short
  TEST_ASSERT_TRUE(answered);
  TEST_ASSERT_EQUAL_INT(0, notify_result);
  TEST_ASSERT_EQUAL_HEX32(NOTIFY_DISCONNECTED, read_word(notify_buffer));
  TEST_ASSERT_EQUAL_UINT32(NOTIFY_DISCONNECTED_LEN, count_notify_written());
  TEST_ASSERT_EQUAL_UINT32(0, refused_count);
  TEST_ASSERT_EQUAL_INT(0, read_link_status());
}

// Refuse Disassoc in state 3
static void test_disassoc_refused_in_state_3(void)
{
  // Send Disassoc, which drops a DS child by MAC and which the stock WD takes only in states 1, 5 and 6
  const u8 mac[8] = {0};
  TEST_ASSERT_EQUAL_HEX32(WD_ESTATE, send_request(WD_DISASSOC, mac, 8, 0));
}

// Answer a scan right after a join asked for and dropped
static void test_scan_answers_after_aborted_join(void)
{
  // Scan right after a join asked for and dropped, which leaves the radio busy, and pass on any answer at all
  set_link_state(1);
  set_link_state(0);
  fill_scan_params();
  async_scan_vectors[0].data = scan_params;
  async_scan_vectors[0].len  = WD_SCAN_PARAMS_LEN;
  async_scan_vectors[1].data = async_scan_buffer;
  async_scan_vectors[1].len  = sizeof async_scan_buffer;
  scan_pending               = true;
  IOS_IoctlvAsync(wd_fd, WD_SCAN, 1, 1, async_scan_vectors, handle_scan, NULL);

  // Wait for the scan's answer
  wait_for_answer(&scan_pending, SCAN_WAIT_MS);
  TEST_ASSERT_FALSE(scan_pending);
}

// Refuse the DS master mode requests
static void test_master_mode_refused(void)
{
  // Send the DS master mode requests, refused as bad state
  TEST_ASSERT_EQUAL_HEX32(WD_ESTATE, send_request(WD_SEND_BEACON, NULL, 0, 0));
  TEST_ASSERT_EQUAL_HEX32(WD_ESTATE, send_request(WD_MP_SEND_FRAME, NULL, 0, 0));
}

// Refuse ChangeVTSF in state 3
static void test_change_vtsf_refused_in_state_3(void)
{
  // Send ChangeVTSF in state 3, refused as bad state, like stock WD
  const u8 value[2] = {0};
  TEST_ASSERT_EQUAL_HEX32(WD_ESTATE, send_request(WD_CHANGE_VTSF, value, 2, 0));
}

// Refuse an unknown request
static void test_unknown_refused(void)
{
  // Send an unknown request, refused with the stock dispatcher's -4
  TEST_ASSERT_EQUAL_INT(WD_EFAIL, send_request(WD_UNKNOWN, NULL, 0, 0));
}

// Answer a parked notification when the session closes
static void test_close_releases_parked_notification(void)
{
  // Close the session, which answers every parked request with the closed result NCD's helper threads exit on
  park_notification();
  IOS_Close(wd_fd);
  wd_fd = -1;

  // Wait for the closed answer
  TEST_ASSERT_TRUE(wait_for_notification(QUIET_MS));
  TEST_ASSERT_EQUAL_HEX32(WD_ECLOSED, notify_result);
}

// Send one manage request to NCD with the vectors given
static s32 send_manage(u32 number, u32 in_count, u32 out_count, ioctlv *manage_vectors)
{
  // Open the manage device for this request alone
  s32 fd = IOS_Open(NCD_MANAGE, 0);
  if (fd < 0)
    return fd;

  // Send the request and close the device
  s32 error = IOS_Ioctlv(fd, number, in_count, out_count, manage_vectors);
  IOS_Close(fd);
  return error;
}

enum result wd_suite_run(void)
{
  // Borrow the driver from NCD, which closes its own session, with the lock id in the second reply word
  lock_vectors[0].data = lock_reply;
  lock_vectors[0].len  = 8;
  s32 error            = send_manage(NCD_LOCK_DRIVER, 0, 1, lock_vectors);
  if (error < 0 || lock_reply[0] != 0) {
    report_line("NCD lock failed, %d, result %d\n", (int)error, (int)lock_reply[0]);
    return RESULT_FAIL;
  }

  // Give NCD time to close its session
  usleep(NCD_RELEASE_MS * 1000);

  // Run the tests in order, since the session, the link and the close each build on the one before
  UnityBegin("wd_suite.c");
  RUN_TEST(test_open_refuses_state_above_6);
  RUN_TEST(test_open_sets_state_from_mode);
  RUN_TEST(test_second_open_shares_session);
  RUN_TEST(test_config_kept_from_ncd);
  RUN_TEST(test_status_link_down);
  RUN_TEST(test_info_has_mac_and_channels);
  RUN_TEST(test_wl_status_reads);
  RUN_TEST(test_scan_finds_networks);
  RUN_TEST(test_scan_short_buffer_refused);
  RUN_TEST(test_link_up_notifies_connected);
  RUN_TEST(test_scan_refused_while_link_up);
  RUN_TEST(test_link_down_notifies_disconnected);
  RUN_TEST(test_disconnect_fits_record_length);
  RUN_TEST(test_disassoc_refused_in_state_3);
  RUN_TEST(test_scan_answers_after_aborted_join);
  RUN_TEST(test_master_mode_refused);
  RUN_TEST(test_change_vtsf_refused_in_state_3);
  RUN_TEST(test_unknown_refused);
  RUN_TEST(test_close_releases_parked_notification);

  // End the run and list its failures again below Unity's summary
  int failures = UNITY_END();
  advanced_report_failures();

  // Close handles a failed test left open, the second one last since it shares the session
  if (wd_fd >= 0)
    IOS_Close(wd_fd);
  if (second_fd >= 0)
    IOS_Close(second_fd);
  wd_fd     = -1;
  second_fd = -1;

  // Hand the driver back to NCD with the lock id
  lock_id[0]           = lock_reply[1];
  lock_vectors[0].data = lock_id;
  lock_vectors[0].len  = 4;
  lock_vectors[1].data = lock_reply;
  lock_vectors[1].len  = 4;
  error                = send_manage(NCD_UNLOCK_DRIVER, 1, 1, lock_vectors);
  if (error < 0 || lock_reply[0] != 0) {
    report_line("NCD unlock failed, %d, result %d\n", (int)error, (int)lock_reply[0]);
    return RESULT_FAIL;
  }
  return failures == 0 ? RESULT_PASS : RESULT_FAIL;
}
