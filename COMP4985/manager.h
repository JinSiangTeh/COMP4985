//
// Created by Jin Siang Teh on 2026-03-01.
//

#ifndef COMP4985_MANAGER_H
#define COMP4985_MANAGER_H

#include "protocol.h"

// ---------------------------------------------------------------------------
// Per-interaction handlers  (spec rows 2–5 + 14, Server ↔ Manager)
// ---------------------------------------------------------------------------

// spec row 2   — SEND Register as a server
// res=00000  crud=00  ack=0
void send_server_register(int sock);

// spec row 3   — RECV Register ACK  →  saves assigned server_id
// res=00000  crud=00  ack=1
void handle_register_ack(uint8_t *buf);

// spec row 4/5 — RECV Activate Server  →  SEND Activate Server ACK
// res=00000  crud=10  ack=0  →  ack=1
void handle_activate_server(int sock);

// spec row 14  — SEND Forward Logs
// res=00011  crud=00  ack=0
void send_log_to_manager(const char *log_msg);

// ---------------------------------------------------------------------------
// Connection loop — connects to manager and dispatches the above handlers
// ---------------------------------------------------------------------------
void* manager_connection_thread(void *arg);



#endif //COMP4985_MANAGER_H