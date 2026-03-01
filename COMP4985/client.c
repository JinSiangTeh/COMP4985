#include "protocol.h"
#include "ui.h"
#include "client.h"

// ===========================================================================
// Client ID counter
// ===========================================================================

uint8_t next_account_id = 1;
pthread_mutex_t acc_id_mutex = PTHREAD_MUTEX_INITIALIZER;

// ===========================================================================
// Per-interaction handlers
// ===========================================================================

// spec row 8/9 — Create Account
// RECV: res=00010  crud=00  ack=0
// SEND: res=00010  crud=00  ack=1
void handle_create_account(int sock, uint8_t *buffer) {
    AccountCreatePayload *acc = (AccountCreatePayload *)buffer;

    pthread_mutex_lock(&acc_id_mutex);
    acc->client_id = next_account_id++;
    pthread_mutex_unlock(&acc_id_mutex);

    client_log("[CREATE ACCOUNT] User: %.16s → ID: %d",
               acc->username, acc->client_id);

    send_binary_msg(sock, RES_USER, CRUD_CREATE, IS_ACK,
                    buffer, sizeof(AccountCreatePayload));
}

// spec row 10/11 (Login) and 12/13 (Logout)
// RECV: res=00010  crud=10  ack=0  — both share these header bits
// SEND: res=00010  crud=10  ack=1
// Differentiated by status byte: 0x00=Login, 0x01=Logout
void handle_login_logout(int sock, uint8_t *buffer) {
    LoginLogoutPayload *lp = (LoginLogoutPayload *)buffer;

    if (lp->status == STATUS_LOGIN) {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &lp->client_ip, ip_str, sizeof(ip_str));
        client_log("[LOGIN]  User: %.16s  IP: %s", lp->username, ip_str);
    } else if (lp->status == STATUS_LOGOUT) {
        client_log("[LOGOUT] User: %.16s", lp->username);
    } else {
        client_log("[LOGIN/LOGOUT] User: %.16s  Unknown status: 0x%02X",
                   lp->username, lp->status);
    }

    send_binary_msg(sock, RES_USER, CRUD_UPDATE, IS_ACK,
                    buffer, sizeof(LoginLogoutPayload));
}

// spec row 20/21 — User Read
// RECV: res=00010  crud=01  ack=0
// SEND: res=00010  crud=01  ack=1
void handle_user_read(int sock, uint8_t *buffer) {
    UserReadPayload *ur = (UserReadPayload *)buffer;
    client_log("[USER READ] Auth: %.16s  Lookup: %.16s",
               ur->username, ur->username_for_user_id);

    // TODO: ur->user_id = lookup_user_id(ur->username_for_user_id);

    send_binary_msg(sock, RES_USER, CRUD_READ, IS_ACK,
                    buffer, sizeof(UserReadPayload));
}

// spec row 15/16 — Channel Read
// RECV: res=00100  crud=01  ack=0
// SEND: res=00100  crud=01  ack=1
void handle_channel_read(int sock, uint8_t *buffer, uint32_t plen) {
    ChannelReadHeader *cr = (ChannelReadHeader *)buffer;
    client_log("[CHANNEL READ] Auth: %.16s  Channel: %.16s  ID: %d",
               cr->username, cr->channel_name, cr->channel_id);

    // TODO: fill in user_id_array_length and user_id_array

    send_binary_msg(sock, RES_CHANNEL, CRUD_READ, IS_ACK, buffer, plen);
}

// spec row 22/23 — Channels Read
// RECV: res=00101  crud=10  ack=0
// SEND: res=00101  crud=10  ack=1
void handle_channels_read(int sock, uint8_t *buffer, uint32_t plen) {
    ChannelsReadHeader *cr = (ChannelsReadHeader *)buffer;
    client_log("[CHANNELS READ] Auth: %.16s", cr->username);

    // TODO: fill cr->channel_list_length and channel list bytes

    send_binary_msg(sock, RES_CHANNELS, CRUD_UPDATE, IS_ACK, buffer, plen);
}

// spec row 17 — Message Create  (no ACK)
// RECV: res=00110  crud=00  ack=0
void handle_message_create(int sock, uint8_t *buffer) {
    (void)sock;
    MessageCreateHeader *mc = (MessageCreateHeader *)buffer;
    client_log("[MSG CREATE] Auth: %.16s  Channel: %d  MsgLen: %d",
               mc->username, mc->channel_id, mc->message_length);

    // TODO: store message
}

// spec row 18/19 — Message Read
// RECV: res=00110  crud=01  ack=0
// SEND: res=00110  crud=01  ack=1
void handle_message_read(int sock, uint8_t *buffer, uint32_t plen) {
    MessageReadHeader *mr = (MessageReadHeader *)buffer;
    client_log("[MSG READ] Auth: %.16s  Channel: %d  Sender: %d",
               mr->username, mr->channel_id, mr->user_id_of_sender);

    // TODO: retrieve message from store and fill buffer

    send_binary_msg(sock, RES_MESSAGE, CRUD_READ, IS_ACK, buffer, plen);
}

// ===========================================================================
// Dispatch loop — reads header, routes to the correct handler above
// ===========================================================================
void* handle_client(void *arg) {
    int sock = *(int *)arg;
    free(arg);

    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    getpeername(sock, (struct sockaddr *)&addr, &alen);
    char peer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, peer, sizeof(peer));
    client_log("[CONNECT] %s", peer);

    GlobalHeader h;
    uint8_t buffer[BUFFER_SIZE];

    while (recv_binary_msg(sock, &h, buffer, BUFFER_SIZE) >= 0) {
        uint32_t plen = ntohl(h.message_length);

        // ------------------------------------------------------------------
        // Check 1: version must be v0.2  (status 0x40 SenderInvalidVersion)
        // ------------------------------------------------------------------
        if (h.version_major != PROTO_VER_MAJOR || h.version_minor != PROTO_VER_MINOR) {
            client_log("[REJECT] %s — wrong version %d.%d",
                       peer, h.version_major, h.version_minor);
            send_error_response(sock, h.resource_type, h.crud, STATUS_INVALID_VERSION);
            continue;
        }

        // ------------------------------------------------------------------
        // Check 2: server only accepts REQ frames from clients  (status 0x41 SenderInvalidType)
        // ------------------------------------------------------------------
        if (h.ack != IS_REQ) {
            client_log("[REJECT] %s — client sent an ACK frame", peer);
            send_error_response(sock, h.resource_type, h.crud, STATUS_INVALID_TYPE);
            continue;
        }

        // ------------------------------------------------------------------
        // Check 3: payload must not exceed buffer  (status 0x42 SenderInvalidSize)
        // ------------------------------------------------------------------
        if (plen > BUFFER_SIZE) {
            client_log("[REJECT] %s — payload too large: %u bytes", peer, plen);
            send_error_response(sock, h.resource_type, h.crud, STATUS_INVALID_SIZE);
            continue;
        }

        // ------------------------------------------------------------------
        // Check 4: total payload must not exceed BUFFER_SIZE  (status 0x83 ReceiverMessageTooLarge)
        // plen already covers the full payload including variable message bytes
        // ------------------------------------------------------------------
        if (h.resource_type == RES_MESSAGE && plen > MAX_MESSAGE_SIZE) {
            client_log("[REJECT] %s — message payload too large: %u bytes", peer, plen);
            send_error_response(sock, h.resource_type, h.crud, STATUS_MESSAGE_TOO_LARGE);
            continue;
        }

        // ------------------------------------------------------------------
        // Check 5: unknown resource_type+crud combination  (status 0x41 SenderInvalidType)
        // ------------------------------------------------------------------
        int known = (h.resource_type == RES_USER     && h.crud == CRUD_CREATE) ||
                    (h.resource_type == RES_USER     && h.crud == CRUD_UPDATE) ||
                    (h.resource_type == RES_USER     && h.crud == CRUD_READ)   ||
                    (h.resource_type == RES_CHANNEL  && h.crud == CRUD_READ)   ||
                    (h.resource_type == RES_CHANNELS && h.crud == CRUD_UPDATE) ||
                    (h.resource_type == RES_MESSAGE  && h.crud == CRUD_CREATE) ||
                    (h.resource_type == RES_MESSAGE  && h.crud == CRUD_READ);
        if (!known) {
            client_log("[REJECT] %s — unknown type: res=%d crud=%d",
                       peer, h.resource_type, h.crud);
            send_error_response(sock, h.resource_type, h.crud, STATUS_INVALID_TYPE);
            continue;
        }

        // ------------------------------------------------------------------
        // Dispatch
        // ------------------------------------------------------------------
        if      (h.resource_type == RES_USER     && h.crud == CRUD_CREATE)
            handle_create_account(sock, buffer);
        else if (h.resource_type == RES_USER     && h.crud == CRUD_UPDATE)
            handle_login_logout(sock, buffer);
        else if (h.resource_type == RES_USER     && h.crud == CRUD_READ)
            handle_user_read(sock, buffer);
        else if (h.resource_type == RES_CHANNEL  && h.crud == CRUD_READ)
            handle_channel_read(sock, buffer, plen);
        else if (h.resource_type == RES_CHANNELS && h.crud == CRUD_UPDATE)
            handle_channels_read(sock, buffer, plen);
        else if (h.resource_type == RES_MESSAGE  && h.crud == CRUD_CREATE)
            handle_message_create(sock, buffer);
        else if (h.resource_type == RES_MESSAGE  && h.crud == CRUD_READ)
            handle_message_read(sock, buffer, plen);
    }

    client_log("[DISCONNECT] %s", peer);
    close(sock);
    return NULL;
}