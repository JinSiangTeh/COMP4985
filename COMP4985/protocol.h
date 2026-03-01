//
// Created by Jin Siang Teh on 2026-03-01.
//

#ifndef COMP4985_PROTOCOL_H
#define COMP4985_PROTOCOL_H

#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htole16(x) OSSwapHostToLittleInt16(x)
#else
#include <endian.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>

#define BUFFER_SIZE 65536

// ---------------------------------------------------------------------------
// Protocol version (all messages use these values)
// ---------------------------------------------------------------------------
#define PROTO_VER_MAJOR  0x0    // 4-bit: 0000
#define PROTO_VER_MINOR  0x2    // 4-bit: 0010  → v0.2

// ---------------------------------------------------------------------------
// Resource Type  (5-bit field)
// ---------------------------------------------------------------------------
#define RES_SYSTEM    0x00   // 00000
#define RES_USER      0x02   // 00010
#define RES_LOG       0x03   // 00011
#define RES_CHANNEL   0x04   // 00100
#define RES_CHANNELS  0x05   // 00101
#define RES_MESSAGE   0x06   // 00110

// ---------------------------------------------------------------------------
// CRUD  (2-bit field)
// ---------------------------------------------------------------------------
#define CRUD_CREATE  0x0   // 00
#define CRUD_READ    0x1   // 01
#define CRUD_UPDATE  0x2   // 10

// ---------------------------------------------------------------------------
// ACK  (1-bit field)
// ---------------------------------------------------------------------------
#define IS_REQ  0x0
#define IS_ACK  0x1

// ---------------------------------------------------------------------------
// Login/Logout status byte values
// ---------------------------------------------------------------------------
#define STATUS_LOGIN   0x00   // 0000 0000
#define STATUS_LOGOUT  0x01   // 0000 0001

// ---------------------------------------------------------------------------
// Status codes (byte 2 of GlobalHeader)
// ---------------------------------------------------------------------------
#define STATUS_OK                  0x00

// Sender errors — problem is with what the client sent
#define STATUS_INVALID_VERSION     0x40
#define STATUS_INVALID_TYPE        0x41
#define STATUS_INVALID_SIZE        0x42
#define STATUS_MALFORMED_REQUEST   0x43
#define STATUS_INVALID_CREDENTIALS 0x44   // needs database
#define STATUS_NOT_FOUND           0x45   // needs database
#define STATUS_ALREADY_EXISTS      0x46   // needs database
#define STATUS_NOT_REGISTERED      0x47   // needs database
#define STATUS_FORBIDDEN           0x48   // needs database
#define STATUS_NOT_CHANNEL_MEMBER  0x49   // needs database

// Receiver errors — problem is on the server side
#define STATUS_INTERNAL_ERROR      0x80
#define STATUS_SERVICE_UNAVAILABLE 0x81
#define STATUS_RESOURCE_EXHAUSTED  0x82
#define STATUS_MESSAGE_TOO_LARGE   0x83
#define STATUS_TIMEOUT             0x84

// ---------------------------------------------------------------------------
// Max message payload size
// ---------------------------------------------------------------------------
#define MAX_MESSAGE_SIZE  65535   // refuse any message payload larger than this (uint16_t max)

// ===========================================================================
// GlobalHeader — 8 bytes packed, exact spec bit widths
//
//  Byte 0:  version_major[4] | version_minor[4]
//  Byte 1:  resource_type[5] | crud[2] | ack[1]
//  Byte 2:  status_major[4]  | status_minor[4]
//  Byte 3:  padding[8]
//  Bytes 4-7: message_length[32]  network byte order
// ===========================================================================
typedef struct __attribute__((packed)) {
    uint8_t  version_major : 4;
    uint8_t  version_minor : 4;

    uint8_t  resource_type : 5;
    uint8_t  crud          : 2;
    uint8_t  ack           : 1;

    uint8_t  status_major  : 4;
    uint8_t  status_minor  : 4;

    uint8_t  padding;

    uint32_t message_length;   // network byte order
} GlobalHeader;   // 8 bytes

// ===========================================================================
// Payload structures (all packed)
// ===========================================================================

// --- System resource (RES_SYSTEM = 00000) ---

typedef struct __attribute__((packed)) {
    uint32_t server_ip;
    uint8_t  server_id;
} RegisterPayload;   // 5 bytes
// Used by: Register REQ/ACK, Activate REQ/ACK

// For Get Active Server REQ the spec says server_ip=0, server_id=0
typedef RegisterPayload GetActiveServerPayload;

// --- User resource (RES_USER = 00010) ---

// Create Account REQ: client_id must be 0
// Create Account ACK: server fills in client_id
typedef struct __attribute__((packed)) {
    char    username[16];
    char    password[16];
    uint8_t client_id;
} AccountCreatePayload;   // 33 bytes

// Login  REQ/ACK: status = STATUS_LOGIN  (0x00)
// Logout REQ/ACK: status = STATUS_LOGOUT (0x01)
typedef struct __attribute__((packed)) {
    char     username[16];
    char     password[16];
    uint32_t client_ip;
    uint8_t  status;
} LoginLogoutPayload;   // 37 bytes

// User Read REQ: username_for_user_id = target name, user_id = 0
// User Read ACK: server fills in user_id
typedef struct __attribute__((packed)) {
    char    username[16];
    char    password[16];
    char    username_for_user_id[16];
    uint8_t user_id;
} UserReadPayload;   // 49 bytes

// --- Log resource (RES_LOG = 00011) ---

// Forward Logs: log_length is LITTLE-ENDIAN (Wireshark dissector requirement)
typedef struct __attribute__((packed)) {
    uint8_t  server_id;
    uint16_t log_length;   // little-endian!
    // followed by log_length bytes of log text
} LogPayload;   // 3 bytes header + variable

// --- Channel resource (RES_CHANNEL = 00100) ---

// Channel Read REQ/ACK (variable length due to user_id_array)
typedef struct __attribute__((packed)) {
    char    username[16];
    char    password[16];
    char    channel_name[16];
    uint8_t channel_id;
    uint8_t user_id_array_length;
    // followed by user_id_array_length × uint8_t user IDs
} ChannelReadHeader;   // 50 bytes + variable

// --- Channels resource (RES_CHANNELS = 00101) ---

// Channels Read REQ: channel_list_length=0, no list
// Channels Read ACK: server fills length + list
typedef struct __attribute__((packed)) {
    char    username[16];
    char    password[16];
    uint8_t channel_list_length;
    // followed by channel_list_length × uint8_t channel IDs
} ChannelsReadHeader;   // 33 bytes + variable

// --- Message resource (RES_MESSAGE = 00110) ---

// Message Create REQ (no ACK in spec)
typedef struct __attribute__((packed)) {
    char     username[16];
    char     password[16];
    uint64_t timestamp;        // 8 bytes
    uint16_t message_length;
    uint8_t  channel_id;
    // followed by message_length bytes of message text
} MessageCreateHeader;   // 43 bytes + variable

// Message Read REQ / Message Read ACK
typedef struct __attribute__((packed)) {
    char     username[16];
    char     password[16];
    uint64_t timestamp;
    uint16_t message_length;
    uint8_t  channel_id;
    uint8_t  user_id_of_sender;
    // followed by message_length bytes of message text
} MessageReadHeader;   // 44 bytes + variable

// ---------------------------------------------------------------------------

typedef struct {
    char ip[20];
    int  port;
    int  my_port;
} ManagerInfo;

// ===========================================================================
// Globals
// ===========================================================================

extern uint8_t  my_server_id;
extern uint32_t my_server_ip;
extern int      manager_socket;
extern int      manager_connected;
extern pthread_mutex_t manager_mutex;

extern uint8_t next_account_id;
extern pthread_mutex_t acc_id_mutex;

// ===========================================================================
// send_binary_msg / recv_binary_msg — declarations
// ===========================================================================

int send_binary_msg(int sock,
                    uint8_t res_type, uint8_t crud, uint8_t ack,
                    const void *pay, uint32_t len);

int recv_binary_msg(int sock, GlobalHeader *h, void *pay, uint32_t max);

// send_error_response — sends a header-only reply with the given status code
// and no payload. Used to reject bad requests.
int send_error_response(int sock,
                        uint8_t res_type, uint8_t crud,
                        uint8_t status_code);


#endif //COMP4985_PROTOCOL_H