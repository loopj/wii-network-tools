#include <string.h>

#include <gccore.h>
#include <unity.h>

#include "advanced.h"
#include "report.h"
#include "steps.h"

#include "so_suite.h"

// The socket device under test
#define SO_DEVICE "/dev/net/ip/top"

// A descriptor no socket uses, since the table holds 24
#define FD_UNUSED 99

// The port the bind tests take
#define TEST_PORT 50123

// Size of a hostent answer
#define HOSTENT_LEN 0x460

// Offset of the name in a hostent answer
#define HOSTENT_NAME 0x10

// Offset of the addresses in a hostent answer
#define HOSTENT_ADDRESSES 0x110

// Offset of the address list in a hostent answer
#define HOSTENT_ADDRESS_LIST 0x340

// Size of an addrinfo answer
#define ADDRINFO_LEN 0x840

// Offset of the packed addresses in an addrinfo answer
#define ADDRINFO_ADDRESSES 0x460

// Level of the interface options
#define IFOPT_LEVEL 0xfffe

// Size of the address parsers' output
#define TEXT_OUTPUT_LEN 32

// Requests under test, with the stock SO's numbers
enum so_request {
  SO_ACCEPT          = 0x1,
  SO_BIND            = 0x2,
  SO_CLOSE           = 0x3,
  SO_FCNTL           = 0x5,
  SO_GETSOCKNAME     = 0x7,
  SO_LISTEN          = 0xa,
  SO_POLL            = 0xb,
  SO_RECVFROM        = 0xc,
  SO_SENDTO          = 0xd,
  SO_SHUTDOWN        = 0xe,
  SO_SOCKET          = 0xf,
  SO_GETHOSTID       = 0x10,
  SO_GETHOSTBYNAME   = 0x11,
  SO_GETHOSTBYADDR   = 0x12,
  SO_GETNAMEINFO     = 0x13,
  SO_INET_ATON       = 0x15,
  SO_INET_PTON       = 0x16,
  SO_INET_NTOP       = 0x17,
  SO_GETADDRINFO     = 0x18,
  SO_GETINTERFACEOPT = 0x1c,
};

// Result codes the stock SO returns
enum so_result {
  SO_EFAIL        = -0x4,
  SO_EAFNOSUPPORT = -0x5,
  SO_EWOULDBLOCK  = -0x6,
  SO_EBADF        = -0x8,
  SO_ENOPROTOOPT  = -0x33,
  SO_ENOTCONN     = -0x38,
  SO_EOPNOTSUPP   = -0x3f,
  SO_EPROTOTYPE   = -0x45,
};

// Socket families, types, fcntl commands and flags, with the stock SO's numbers
enum so_constant {
  AF_INET      = 2,
  SOCK_STREAM  = 1,
  SOCK_DGRAM   = 2,
  F_GETFL      = 3,
  F_SETFL      = 4,
  O_NONBLOCK   = 4,
  MSG_DONTWAIT = 4,
  POLLIN       = 1,
};

// Interface options under test
enum interface_option {
  IFOPT_MAC     = 0x1004,
  IFOPT_LINK_UP = 0x1005,
  IFOPT_FAMILY  = 0x4002,
  IFOPT_ADDRESS = 0x4003,
  IFOPT_UNKNOWN = 0x4ffe,
};

// The request's input, which IOS reads by DMA
static u8 input_buffer[0x100] ATTRIBUTE_ALIGN(32);

// The request's output, which IOS writes by DMA
static u8 output_buffer[ADDRINFO_LEN] ATTRIBUTE_ALIGN(32);

// A second vector, for requests that take one
static u8 extra_buffer[0x100] ATTRIBUTE_ALIGN(32);

// A third vector, for requests that take one
static u8 extra_buffer_2[0x100] ATTRIBUTE_ALIGN(32);

// The request's vectors
static ioctlv vectors[4] ATTRIBUTE_ALIGN(32);

// The address parsers' output, in MEM2, since the stock SO can answer them with a byte wrong in MEM1
static u8 *text_output;

// The device handle, never closed since Dolphin stops serving a device once any handle to it closes
static s32 so_fd = -1;

// The socket a test opened, closed after it whatever the result
static s32 test_socket = -1;

// Send one ioctl with text as the input
static s32 send_text_ioctl(u32 request, const char *text, u8 *out, u32 out_len)
{
  // Copy the text with its terminator into the input and send it
  size_t len = strlen(text) + 1;
  memcpy(input_buffer, text, len);
  return IOS_Ioctl(so_fd, request, input_buffer, (s32)len, out, (s32)out_len);
}

// Read a big endian word
static u32 read_word(const u8 *at)
{
  return ((u32)at[0] << 24) | ((u32)at[1] << 16) | ((u32)at[2] << 8) | at[3];
}

// Write a big endian word
static void write_word(u8 *at, u32 value)
{
  at[0] = (u8)(value >> 24);
  at[1] = (u8)(value >> 16);
  at[2] = (u8)(value >> 8);
  at[3] = (u8)value;
}

// Parse a dotted address
static void test_inet_aton_dotted(void)
{
  TEST_ASSERT_EQUAL_INT(1, send_text_ioctl(SO_INET_ATON, "10.0.0.54", text_output, 4));
  TEST_ASSERT_EQUAL_HEX32(0x0a000036, read_word(text_output));
}

// Parse octal and hex parts, like inet_aton
static void test_inet_aton_octal_and_hex_parts(void)
{
  TEST_ASSERT_EQUAL_INT(1, send_text_ioctl(SO_INET_ATON, "012.0x10.1.1", text_output, 4));
  TEST_ASSERT_EQUAL_HEX32(0x0a100101, read_word(text_output));
}

// Parse the short form, whose last part fills the rest of the address
static void test_inet_aton_short_form(void)
{
  TEST_ASSERT_EQUAL_INT(1, send_text_ioctl(SO_INET_ATON, "127.1", text_output, 4));
  TEST_ASSERT_EQUAL_HEX32(0x7f000001, read_word(text_output));
}

// Answer 0 for a name
static void test_inet_aton_name_is_zero(void)
{
  // The SDK takes any nonzero answer as parsed and uses the output as the address
  TEST_ASSERT_EQUAL_INT(0, send_text_ioctl(SO_INET_ATON, "example.com", text_output, 4));
  TEST_ASSERT_EQUAL_HEX32(0, read_word(text_output));
}

// Answer 0 for an address with text after it
static void test_inet_aton_trailing_text_is_zero(void)
{
  TEST_ASSERT_EQUAL_INT(0, send_text_ioctl(SO_INET_ATON, "10.0.0.54x", text_output, 4));
}

// Parse an IPv4 address after the family word
static void test_inet_pton_v4(void)
{
  write_word(text_output, 2);
  TEST_ASSERT_EQUAL_INT(1, send_text_ioctl(SO_INET_PTON, "192.168.1.2", text_output, 8));
  TEST_ASSERT_EQUAL_HEX32(0xc0a80102, read_word(text_output + 4));
}

// Refuse a family other than IPv4
static void test_inet_pton_bad_family(void)
{
  write_word(text_output, 7);
  TEST_ASSERT_EQUAL_INT(SO_EAFNOSUPPORT, send_text_ioctl(SO_INET_PTON, "192.168.1.2", text_output, 8));
}

// Answer 0 for a name
static void test_inet_pton_name_is_zero(void)
{
  write_word(text_output, 2);
  TEST_ASSERT_EQUAL_INT(0, send_text_ioctl(SO_INET_PTON, "example.com", text_output, 8));
}

// Format an IPv4 address as text
static void test_inet_ntop_v4(void)
{
  // Send the family, the length word and the address
  write_word(input_buffer, 2);
  write_word(input_buffer + 4, 1);
  write_word(input_buffer + 8, 0x0a000036);
  TEST_ASSERT_EQUAL_INT(0, IOS_Ioctl(so_fd, SO_INET_NTOP, input_buffer, 12, output_buffer, 16));

  // Check the text
  TEST_ASSERT_EQUAL_STRING("10.0.0.54", (const char *)output_buffer);
}

// Refuse an address whose length word is not 1
static void test_inet_ntop_bad_word(void)
{
  write_word(input_buffer, 2);
  write_word(input_buffer + 4, 2);
  write_word(input_buffer + 8, 0x0a000036);
  TEST_ASSERT_EQUAL_INT(SO_EFAIL, IOS_Ioctl(so_fd, SO_INET_NTOP, input_buffer, 12, output_buffer, 16));
}

// Reverse an address to a name, or refuse when DNS cannot
static void test_gethostbyaddr_name_or_refused(void)
{
  // Pass 0 with a name, or -4 when the network's DNS cannot reverse the address, and nothing else
  write_word(input_buffer, 4);
  write_word(input_buffer + 4, 2);
  write_word(input_buffer + 12, 0x0a000001);
  s32 result = IOS_Ioctl(so_fd, SO_GETHOSTBYADDR, input_buffer, 0x10, output_buffer, HOSTENT_LEN);

  // Check for a name on success
  if (result == 0) {
    TEST_ASSERT_NOT_EQUAL(0, output_buffer[HOSTENT_NAME]);
    return;
  }
  TEST_ASSERT_EQUAL_INT(SO_EFAIL, result);
}

// Format an address and port as numbers
static void test_getnameinfo_numeric(void)
{
  // Flags 2 for numeric, the address word 1, then the 8 byte address with port 80
  write_word(input_buffer, 2);
  write_word(input_buffer + 4, 1);
  input_buffer[8]  = 8;
  input_buffer[9]  = 2;
  input_buffer[10] = 0;
  input_buffer[11] = 80;
  write_word(input_buffer + 12, 0x0a000036);

  // Send the input with the host and service outputs
  vectors[0].data = input_buffer;
  vectors[0].len  = 16;
  vectors[1].data = extra_buffer;
  vectors[1].len  = sizeof extra_buffer;
  vectors[2].data = extra_buffer_2;
  vectors[2].len  = sizeof extra_buffer_2;
  TEST_ASSERT_EQUAL_INT(0, IOS_Ioctlv(so_fd, SO_GETNAMEINFO, 1, 2, vectors));

  // Check the host and service text
  TEST_ASSERT_EQUAL_STRING("10.0.0.54", (const char *)extra_buffer);
  TEST_ASSERT_EQUAL_STRING("80", (const char *)extra_buffer_2);
}

// Answer the interface address while the link is up
static void test_gethostid_nonzero_while_up(void)
{
  TEST_ASSERT_NOT_EQUAL(0, IOS_Ioctl(so_fd, SO_GETHOSTID, NULL, 0, NULL, 0));
}

// Resolve a name into the hostent layout that libogc and the SDK read
static void test_gethostbyname_layout(void)
{
  TEST_ASSERT_EQUAL_INT(0, send_text_ioctl(SO_GETHOSTBYNAME, "example.com", output_buffer, HOSTENT_LEN));

  // Every pointer is relative to the base the name pointer gives, as libogc and the SDK relocate them
  u32 base = read_word(output_buffer) - HOSTENT_NAME;
  TEST_ASSERT_EQUAL_STRING("example.com", (const char *)output_buffer + HOSTENT_NAME);
  TEST_ASSERT_EQUAL_HEX32(base + HOSTENT_ADDRESS_LIST, read_word(output_buffer + 0xc));
  TEST_ASSERT_EQUAL_HEX32(base + HOSTENT_ADDRESSES, read_word(output_buffer + HOSTENT_ADDRESS_LIST));
  TEST_ASSERT_EQUAL_UINT16(2, (output_buffer[8] << 8) | output_buffer[9]);
  TEST_ASSERT_EQUAL_UINT16(4, (output_buffer[10] << 8) | output_buffer[11]);
  TEST_ASSERT_NOT_EQUAL(0, read_word(output_buffer + HOSTENT_ADDRESSES));
}

// Fail to resolve a name that does not exist
static void test_gethostbyname_unknown_fails(void)
{
  TEST_ASSERT_EQUAL_INT(SO_EFAIL,
                        send_text_ioctl(SO_GETHOSTBYNAME, "no-such-host.invalid", output_buffer, HOSTENT_LEN));
}

// Resolve a name and a service into an addrinfo node
static void test_getaddrinfo_nodes(void)
{
  // Send the name, the service, hints for family 2 and the output, since without hints the stock SO tries IPv6 too
  strcpy((char *)input_buffer, "example.com");
  strcpy((char *)extra_buffer, "80");
  write_word(extra_buffer_2 + 4, 2);
  write_word(extra_buffer_2 + 8, 1);

  // Send the three inputs with the output
  vectors[0].data = input_buffer;
  vectors[0].len  = 12;
  vectors[1].data = extra_buffer;
  vectors[1].len  = 3;
  vectors[2].data = extra_buffer_2;
  vectors[2].len  = 0x20;
  vectors[3].data = output_buffer;
  vectors[3].len  = ADDRINFO_LEN;
  TEST_ASSERT_EQUAL_INT(0, IOS_Ioctlv(so_fd, SO_GETADDRINFO, 3, 1, vectors));

  // The first node names the family and length, and its address carries the port
  TEST_ASSERT_EQUAL_HEX32(2, read_word(output_buffer + 4));
  TEST_ASSERT_EQUAL_HEX32(8, read_word(output_buffer + 0x10));
  const u8 *address = output_buffer + ADDRINFO_ADDRESSES;
  TEST_ASSERT_EQUAL_UINT8(8, address[0]);
  TEST_ASSERT_EQUAL_UINT8(2, address[1]);
  TEST_ASSERT_EQUAL_UINT16(80, (address[2] << 8) | address[3]);
  TEST_ASSERT_NOT_EQUAL(0, read_word(address + 4));
}

// Read an interface option into extra_buffer, with its length in extra_buffer_2
static s32 read_interface_option(u32 option, u32 capacity)
{
  // Name the option and the room for it
  write_word(input_buffer, IFOPT_LEVEL);
  write_word(input_buffer + 4, option);
  write_word(extra_buffer_2, capacity);

  // Send the name with the value and length outputs
  vectors[0].data = input_buffer;
  vectors[0].len  = 8;
  vectors[1].data = extra_buffer;
  vectors[1].len  = capacity;
  vectors[2].data = extra_buffer_2;
  vectors[2].len  = 4;
  return IOS_Ioctlv(so_fd, SO_GETINTERFACEOPT, 1, 2, vectors);
}

// Answer 2 for the address family option
static void test_ifopt_family_is_two(void)
{
  TEST_ASSERT_EQUAL_INT(0, read_interface_option(IFOPT_FAMILY, 4));
  TEST_ASSERT_EQUAL_HEX32(2, read_word(extra_buffer));
  TEST_ASSERT_EQUAL_HEX32(4, read_word(extra_buffer_2));
}

// Answer the MAC address in six bytes
static void test_ifopt_mac_six_bytes(void)
{
  TEST_ASSERT_EQUAL_INT(0, read_interface_option(IFOPT_MAC, 6));
  TEST_ASSERT_EQUAL_HEX32(6, read_word(extra_buffer_2));
  TEST_ASSERT_TRUE(extra_buffer[0] | extra_buffer[1] | extra_buffer[2] | extra_buffer[3] | extra_buffer[4] |
                   extra_buffer[5]);
}

// Answer the address block with an address and a netmask
static void test_ifopt_address_block(void)
{
  TEST_ASSERT_EQUAL_INT(0, read_interface_option(IFOPT_ADDRESS, 12));
  TEST_ASSERT_EQUAL_HEX32(12, read_word(extra_buffer_2));
  TEST_ASSERT_NOT_EQUAL(0, read_word(extra_buffer));
  TEST_ASSERT_NOT_EQUAL(0, read_word(extra_buffer + 4));
}

// Answer 1 for the link option while the link is up
static void test_ifopt_link_up(void)
{
  TEST_ASSERT_EQUAL_INT(0, read_interface_option(IFOPT_LINK_UP, 4));
  TEST_ASSERT_EQUAL_HEX32(1, read_word(extra_buffer));
}

// Write nothing for an option too long for its buffer
static void test_ifopt_short_buffer_takes_nothing(void)
{
  // Read the 12 byte address block into 4 bytes, which succeeds with nothing written, like stock SO
  TEST_ASSERT_EQUAL_INT(0, read_interface_option(IFOPT_ADDRESS, 4));
  TEST_ASSERT_EQUAL_HEX32(0, read_word(extra_buffer_2));
}

// Refuse an unknown option
static void test_ifopt_unknown_refused(void)
{
  TEST_ASSERT_EQUAL_INT(SO_ENOPROTOOPT, read_interface_option(IFOPT_UNKNOWN, 4));
}

// Send one ioctl whose input is count big endian words and whose output is out_len bytes of output_buffer
static s32 send_words(u32 request, const u32 *words, int count, u32 out_len)
{
  // Write the words into the input
  for (int i = 0; i < count; i++)
    write_word(input_buffer + i * 4, words[i]);

  // Send them with the output when there is one
  return IOS_Ioctl(so_fd, request, input_buffer, count * 4, out_len ? output_buffer : NULL, out_len);
}

// Close a socket by its descriptor
static s32 close_socket(u32 descriptor)
{
  const u32 words[1] = {descriptor};
  return send_words(SO_CLOSE, words, 1, 0);
}

// Open a socket of a type, kept in test_socket for the teardown
static s32 open_socket(u32 type)
{
  const u32 words[3] = {AF_INET, type, 0};
  test_socket        = send_words(SO_SOCKET, words, 3, 0);
  return test_socket;
}

// Bind test_socket to the test port on any address
static s32 bind_test_port(void)
{
  // fd, the address present word, then SOSockAddrIn as length, family, port and address
  const u32 words[4] = {(u32)test_socket, 1, (8u << 24) | (AF_INET << 16) | TEST_PORT, 0};
  return send_words(SO_BIND, words, 4, 0);
}

// Make test_socket nonblocking
static s32 set_nonblocking(void)
{
  const u32 words[3] = {(u32)test_socket, F_SETFL, O_NONBLOCK};
  return send_words(SO_FCNTL, words, 3, 0);
}

// Open a TCP socket and close it
static void test_socket_tcp_gives_descriptor(void)
{
  // Descriptors run 0 to 23, and a close of a live one answers 0
  TEST_ASSERT_TRUE(open_socket(SOCK_STREAM) >= 0 && test_socket < 24);
  TEST_ASSERT_EQUAL_INT(0, close_socket((u32)test_socket));
  test_socket = -1;
}

// Refuse a socket type SO does not have
static void test_socket_bad_type_refused(void)
{
  TEST_ASSERT_EQUAL_INT(SO_EPROTOTYPE, open_socket(3));
}

// Refuse to close a descriptor no socket uses
static void test_close_unused_descriptor_refused(void)
{
  TEST_ASSERT_EQUAL_INT(SO_EBADF, close_socket(FD_UNUSED));
}

// Refuse to close a socket twice
static void test_close_twice_refused(void)
{
  // Open a socket and close it
  TEST_ASSERT_TRUE(open_socket(SOCK_DGRAM) >= 0);
  u32 descriptor = (u32)test_socket;
  close_socket(descriptor);
  test_socket = -1;

  // Close it again
  TEST_ASSERT_EQUAL_INT(SO_EBADF, close_socket(descriptor));
}

// Read back the nonblocking flag fcntl set
static void test_fcntl_nonblocking_reads_back(void)
{
  // cmd 4 stores the flag byte and cmd 3 returns it
  TEST_ASSERT_TRUE(open_socket(SOCK_DGRAM) >= 0);
  TEST_ASSERT_EQUAL_INT(0, set_nonblocking());

  // Read the flags back
  const u32 words[3] = {(u32)test_socket, F_GETFL, 0};
  TEST_ASSERT_EQUAL_INT(O_NONBLOCK, send_words(SO_FCNTL, words, 3, 0));
}

// Answer the bound port for a bound socket
static void test_getsockname_after_bind(void)
{
  TEST_ASSERT_TRUE(open_socket(SOCK_DGRAM) >= 0);
  TEST_ASSERT_EQUAL_INT(0, bind_test_port());

  // Read the name back into an address of length 8
  output_buffer[0]   = 8;
  const u32 words[1] = {(u32)test_socket};
  TEST_ASSERT_EQUAL_INT(0, send_words(SO_GETSOCKNAME, words, 1, 8));
  TEST_ASSERT_EQUAL_HEX32(TEST_PORT, (output_buffer[2] << 8) | output_buffer[3]);
}

// Answer would block for a nonblocking accept with nothing waiting
static void test_accept_nonblocking_would_block(void)
{
  // Open a nonblocking TCP socket listening on the test port
  TEST_ASSERT_TRUE(open_socket(SOCK_STREAM) >= 0);
  TEST_ASSERT_EQUAL_INT(0, bind_test_port());
  const u32 listen_words[2] = {(u32)test_socket, 1};
  TEST_ASSERT_EQUAL_INT(0, send_words(SO_LISTEN, listen_words, 2, 0));
  TEST_ASSERT_EQUAL_INT(0, set_nonblocking());

  // Accept into an address of length 8
  output_buffer[0]   = 8;
  const u32 words[1] = {(u32)test_socket};
  TEST_ASSERT_EQUAL_INT(SO_EWOULDBLOCK, send_words(SO_ACCEPT, words, 1, 8));
}

// Refuse to listen on a UDP socket
static void test_listen_on_udp_refused(void)
{
  TEST_ASSERT_TRUE(open_socket(SOCK_DGRAM) >= 0);
  const u32 words[2] = {(u32)test_socket, 1};
  TEST_ASSERT_EQUAL_INT(SO_EOPNOTSUPP, send_words(SO_LISTEN, words, 2, 0));
}

// Answer would block for a receive with nothing queued
static void test_recvfrom_dontwait_would_block(void)
{
  // A bound UDP socket with nothing queued answers at once with flag 4
  TEST_ASSERT_TRUE(open_socket(SOCK_DGRAM) >= 0);
  TEST_ASSERT_EQUAL_INT(0, bind_test_port());

  // Receive into the output with MSG_DONTWAIT
  write_word(input_buffer, (u32)test_socket);
  write_word(input_buffer + 4, MSG_DONTWAIT);
  vectors[0].data = input_buffer;
  vectors[0].len  = 8;
  vectors[1].data = output_buffer;
  vectors[1].len  = 0x100;
  vectors[2].data = NULL;
  vectors[2].len  = 0;
  TEST_ASSERT_EQUAL_INT(SO_EWOULDBLOCK, IOS_Ioctlv(so_fd, SO_RECVFROM, 1, 2, vectors));
}

// Send an empty datagram
static void test_sendto_empty_datagram(void)
{
  // Send an empty datagram, which answers 0 bytes sent, like stock SO
  TEST_ASSERT_TRUE(open_socket(SOCK_DGRAM) >= 0);

  // Send no data to port 9 of 10.0.0.1
  write_word(input_buffer, (u32)test_socket);
  write_word(input_buffer + 4, 0);
  write_word(input_buffer + 8, 1);
  write_word(input_buffer + 12, (8u << 24) | (AF_INET << 16) | 9);
  write_word(input_buffer + 16, 0x0a000001);
  vectors[0].data = output_buffer;
  vectors[0].len  = 0;
  vectors[1].data = input_buffer;
  vectors[1].len  = 0x14;
  TEST_ASSERT_EQUAL_INT(0, IOS_Ioctlv(so_fd, SO_SENDTO, 2, 0, vectors));
}

// Answer no entries ready for an idle socket polled without waiting
static void test_poll_zero_timeout_idle(void)
{
  // An idle socket polled for reading with a zero timeout answers 0 entries ready
  TEST_ASSERT_TRUE(open_socket(SOCK_DGRAM) >= 0);
  TEST_ASSERT_EQUAL_INT(0, bind_test_port());

  // Send a zero timeout and the socket's entry
  memset(input_buffer, 0, 8);
  write_word(output_buffer, (u32)test_socket);
  write_word(output_buffer + 4, POLLIN);
  write_word(output_buffer + 8, 0);
  TEST_ASSERT_EQUAL_INT(0, IOS_Ioctl(so_fd, SO_POLL, input_buffer, 8, output_buffer, 12));
}

// Refuse to shut down a TCP socket that is not connected
static void test_shutdown_unconnected_tcp_refused(void)
{
  TEST_ASSERT_TRUE(open_socket(SOCK_STREAM) >= 0);
  const u32 words[2] = {(u32)test_socket, 2};
  TEST_ASSERT_EQUAL_INT(SO_ENOTCONN, send_words(SO_SHUTDOWN, words, 2, 0));
}

// Shut down a UDP socket, which does nothing
static void test_shutdown_udp_does_nothing(void)
{
  TEST_ASSERT_TRUE(open_socket(SOCK_DGRAM) >= 0);
  const u32 words[2] = {(u32)test_socket, 2};
  TEST_ASSERT_EQUAL_INT(0, send_words(SO_SHUTDOWN, words, 2, 0));
}

void setUp(void)
{
  // Clear the request buffers so no test sees the last one's data
  memset(input_buffer, 0, sizeof input_buffer);
  memset(output_buffer, 0, sizeof output_buffer);
  memset(extra_buffer, 0, sizeof extra_buffer);
  memset(extra_buffer_2, 0, sizeof extra_buffer_2);

  // Clear the MEM2 output only once it exists, since Unity also runs this before the WD suite's tests
  if (text_output)
    memset(text_output, 0, TEXT_OUTPUT_LEN);
}

void tearDown(void)
{
  // Close a socket a failed assertion left, so the next test starts with a free table
  if (test_socket >= 0)
    close_socket((u32)test_socket);
  test_socket = -1;
}

enum result so_suite_run(void)
{
  // Bring the link up, which the lookups and the interface options need
  if (!steps_link_is_up() && step_link_up() < 0)
    return RESULT_FAIL;

  // Open the device on the first run
  if (so_fd < 0)
    so_fd = IOS_Open(SO_DEVICE, 0);
  if (so_fd < 0) {
    report_line("could not open %s, %d\n", SO_DEVICE, (int)so_fd);
    return RESULT_FAIL;
  }

  // Take the address parsers' output from MEM2 once
  if (!text_output) {
    text_output = (u8 *)(((u32)SYS_GetArena2Hi() - TEXT_OUTPUT_LEN) & ~31u);
    SYS_SetArena2Hi(text_output);
  }

  // Run the tests under the bare file name, since UNITY_BEGIN would print the compiler's full path on every line
  UnityBegin("so_suite.c");
  RUN_TEST(test_inet_aton_dotted);
  RUN_TEST(test_inet_aton_octal_and_hex_parts);
  RUN_TEST(test_inet_aton_short_form);
  RUN_TEST(test_inet_aton_name_is_zero);
  RUN_TEST(test_inet_aton_trailing_text_is_zero);
  RUN_TEST(test_inet_pton_v4);
  RUN_TEST(test_inet_pton_bad_family);
  RUN_TEST(test_inet_pton_name_is_zero);
  RUN_TEST(test_inet_ntop_v4);
  RUN_TEST(test_inet_ntop_bad_word);
  RUN_TEST(test_gethostbyaddr_name_or_refused);
  RUN_TEST(test_getnameinfo_numeric);
  RUN_TEST(test_gethostid_nonzero_while_up);
  RUN_TEST(test_gethostbyname_layout);
  RUN_TEST(test_gethostbyname_unknown_fails);
  RUN_TEST(test_getaddrinfo_nodes);
  RUN_TEST(test_ifopt_family_is_two);
  RUN_TEST(test_ifopt_mac_six_bytes);
  RUN_TEST(test_ifopt_address_block);
  RUN_TEST(test_ifopt_link_up);
  RUN_TEST(test_ifopt_short_buffer_takes_nothing);
  RUN_TEST(test_ifopt_unknown_refused);
  RUN_TEST(test_socket_tcp_gives_descriptor);
  RUN_TEST(test_socket_bad_type_refused);
  RUN_TEST(test_close_unused_descriptor_refused);
  RUN_TEST(test_close_twice_refused);
  RUN_TEST(test_fcntl_nonblocking_reads_back);
  RUN_TEST(test_getsockname_after_bind);
  RUN_TEST(test_accept_nonblocking_would_block);
  RUN_TEST(test_listen_on_udp_refused);
  RUN_TEST(test_recvfrom_dontwait_would_block);
  RUN_TEST(test_sendto_empty_datagram);
  RUN_TEST(test_poll_zero_timeout_idle);
  RUN_TEST(test_shutdown_unconnected_tcp_refused);
  RUN_TEST(test_shutdown_udp_does_nothing);

  // End the run and list its failures again below Unity's summary
  int failures = UNITY_END();
  advanced_report_failures();
  return failures == 0 ? RESULT_PASS : RESULT_FAIL;
}
