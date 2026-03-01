#include "protocol.h"
#include "ui.h"
#include "manager.h"

// ===========================================================================
// Globals
// ===========================================================================

uint8_t  my_server_id      = 0;
uint32_t my_server_ip      = 0;
int      manager_socket    = -1;
int      manager_connected = 0;
pthread_mutex_t manager_mutex = PTHREAD_MUTEX_INITIALIZER;

// ===========================================================================
// send_binary_msg / recv_binary_msg
// ===========================================================================

int send_binary_msg(int sock,
                    uint8_t res_type, uint8_t crud, uint8_t ack,
                    const void *pay, uint32_t len)
{
    GlobalHeader h = {
        .version_major  = PROTO_VER_MAJOR,
        .version_minor  = PROTO_VER_MINOR,
        .resource_type  = res_type,
        .crud           = crud,
        .ack            = ack,
        .status_major   = 0,
        .status_minor   = 0,
        .padding        = 0,
        .message_length = htonl(len)
    };
    if (send(sock, &h, sizeof(GlobalHeader), MSG_NOSIGNAL) <= 0) return -1;
    if (len > 0 && pay) {
        if (send(sock, pay, len, MSG_NOSIGNAL) <= 0) return -1;
    }
    return 0;
}

int recv_binary_msg(int sock, GlobalHeader *h, void *pay, uint32_t max) {
    ssize_t n = recv(sock, h, sizeof(GlobalHeader), MSG_WAITALL);
    if (n <= 0) return -1;

    uint32_t len = ntohl(h->message_length);
    if (len > max) return -2;

    if (len > 0) {
        n = recv(sock, pay, len, MSG_WAITALL);
        if (n != (ssize_t)len) return -1;
    }
    return (int)len;
}

// send_error_response — sends a header-only reply with the given status code
// and no payload. Used to reject bad requests without crashing the connection.
int send_error_response(int sock,
                        uint8_t res_type, uint8_t crud,
                        uint8_t status_code)
{
    GlobalHeader h = {
        .version_major  = PROTO_VER_MAJOR,
        .version_minor  = PROTO_VER_MINOR,
        .resource_type  = res_type,
        .crud           = crud,
        .ack            = IS_ACK,
        .status_major   = (status_code >> 4) & 0xF,
        .status_minor   = status_code & 0xF,
        .padding        = 0,
        .message_length = 0
    };
    return send(sock, &h, sizeof(GlobalHeader), MSG_NOSIGNAL) > 0 ? 0 : -1;
}

// ===========================================================================
// Per-interaction handlers
// ===========================================================================

// spec row 2 — Register as a server
// SEND: res=00000  crud=00  ack=0
void send_server_register(int sock) {
    RegisterPayload reg = {
        .server_ip = my_server_ip,
        .server_id = my_server_id
    };
    send_binary_msg(sock, RES_SYSTEM, CRUD_CREATE, IS_REQ,
                    &reg, sizeof(reg));
}

// spec row 3 — Register ACK
// RECV: res=00000  crud=00  ack=1
void handle_register_ack(uint8_t *buf) {
    my_server_id = ((RegisterPayload *)buf)->server_id;
    manager_log("[REG ACK] Server ID: 0x%02X", my_server_id);
}

// spec row 4/5 — Activate Server / Activate Server ACK
// RECV: res=00000  crud=10  ack=0
// SEND: res=00000  crud=10  ack=1
void handle_activate_server(int sock) {
    RegisterPayload act = {
        .server_ip = my_server_ip,
        .server_id = my_server_id
    };
    send_binary_msg(sock, RES_SYSTEM, CRUD_UPDATE, IS_ACK,
                    &act, sizeof(act));
    manager_log("[ACTIVATE ACK] Server is now Live!");
}

// spec row 14 — Forward Logs
// SEND: res=00011  crud=00  ack=0
// Payload: server_id[1] | log_length[2 LE] | log text[variable]
void send_log_to_manager(const char *log_msg) {
    pthread_mutex_lock(&manager_mutex);
    if (manager_connected && manager_socket >= 0) {
        uint16_t msg_len = (uint16_t)strlen(log_msg);
        uint8_t  buf[BUFFER_SIZE];

        LogPayload *lp = (LogPayload *)buf;
        lp->server_id  = my_server_id;
        lp->log_length = htole16(msg_len);   // LITTLE-ENDIAN per spec
        memcpy(buf + sizeof(LogPayload), log_msg, msg_len);

        send_binary_msg(manager_socket, RES_LOG, CRUD_CREATE, IS_REQ,
                        buf, sizeof(LogPayload) + msg_len);
    }
    pthread_mutex_unlock(&manager_mutex);
}

// ===========================================================================
// Connection loop — connects to manager and dispatches the above handlers
// ===========================================================================
void* manager_connection_thread(void *arg) {
    ManagerInfo *info = (ManagerInfo *)arg;
    while (1) {
        manager_log(">>> Connecting to Manager %s:%d", info->ip, info->port);
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in mgr_addr = {
            .sin_family = AF_INET,
            .sin_port   = htons(info->port)
        };
        inet_pton(AF_INET, info->ip, &mgr_addr.sin_addr);

        if (connect(sock, (struct sockaddr *)&mgr_addr, sizeof(mgr_addr)) == 0) {
            manager_log(">>> Connected");

            send_server_register(sock);

            pthread_mutex_lock(&manager_mutex);
            manager_socket    = sock;
            manager_connected = 1;
            pthread_mutex_unlock(&manager_mutex);

            GlobalHeader h;
            uint8_t buf[BUFFER_SIZE];

            while (recv_binary_msg(sock, &h, buf, BUFFER_SIZE) >= 0) {
                if      (h.resource_type == RES_SYSTEM && h.crud == CRUD_CREATE && h.ack == IS_ACK)
                    handle_register_ack(buf);
                else if (h.resource_type == RES_SYSTEM && h.crud == CRUD_UPDATE && h.ack == IS_REQ)
                    handle_activate_server(sock);
                else
                    manager_log("[WARN] Unknown frame from Manager: res=%d crud=%d ack=%d",
                                h.resource_type, h.crud, h.ack);
            }
        }

        pthread_mutex_lock(&manager_mutex);
        manager_connected = 0;
        pthread_mutex_unlock(&manager_mutex);
        close(sock);
        sleep(5);
    }
    return NULL;
}