//
// Created by Jin Siang Teh on 2026-03-01.
//

#ifndef COMP4985_CLIENT_H
#define COMP4985_CLIENT_H

#include "protocol.h"

// ---------------------------------------------------------------------------
// Per-interaction handlers  (spec rows 8–23, Client ↔ Server)
// Each function receives the already-read payload in buffer and replies.
// ---------------------------------------------------------------------------

// spec row  8/9  — res=00010 crud=00 ack=0  →  ack=1
void handle_create_account(int sock, uint8_t *buffer);

// spec row 10/11 — res=00010 crud=10 ack=0  status=0x00  →  ack=1
// spec row 12/13 — res=00010 crud=10 ack=0  status=0x01  →  ack=1
void handle_login_logout(int sock, uint8_t *buffer);

// spec row 20/21 — res=00010 crud=01 ack=0  →  ack=1
void handle_user_read(int sock, uint8_t *buffer);

// spec row 15/16 — res=00100 crud=01 ack=0  →  ack=1
void handle_channel_read(int sock, uint8_t *buffer, uint32_t plen);

// spec row 22/23 — res=00101 crud=10 ack=0  →  ack=1
void handle_channels_read(int sock, uint8_t *buffer, uint32_t plen);

// spec row 17   — res=00110 crud=00 ack=0  (no ACK)
void handle_message_create(int sock, uint8_t *buffer);

// spec row 18/19 — res=00110 crud=01 ack=0  →  ack=1
void handle_message_read(int sock, uint8_t *buffer, uint32_t plen);

// ---------------------------------------------------------------------------
// Dispatch loop — called once per accepted client socket
// ---------------------------------------------------------------------------
void* handle_client(void *arg);

#endif //COMP4985_CLIENT_H