#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htole16(x) OSSwapHostToLittleInt16(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
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
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <ncurses.h>
#include <stdarg.h>

#define BUFFER_SIZE 65536
#define PROTOCOL_VERSION 0x01

// Explicit IDs from Wireshark Dissector
#define TYPE_SERVER_REG_REQ    0x00
#define TYPE_SERVER_REG_RES    0x01
#define TYPE_SERVER_ACT_REQ    0x08
#define TYPE_SERVER_ACT_RES    0x09
#define TYPE_GET_ACT_REQ       0x0A
#define TYPE_GET_ACT_RES       0x0B
#define TYPE_ACC_REG_REQ       0x10
#define TYPE_ACC_REG_RES       0x11
#define TYPE_ACC_UPDATE_REQ    0x14
#define TYPE_ACC_UPDATE_RES    0x15
#define TYPE_LOG_REQ           0x18
#define TYPE_LOG_RES           0x19

// UI Globals
WINDOW *win_srv, *win_cli, *win_mgr;
pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Binary Protocol Structures (Packed) ---

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  status;
    uint8_t  padding;
    uint32_t msg_length; // Big Endian per protocol standard
} GlobalHeader;

typedef struct __attribute__((packed)) {
    uint32_t server_ip;
    uint8_t  server_id;
} RegisterPayload;

typedef struct __attribute__((packed)) {
    char     username[16];
    char     password[16];
    uint8_t  account_id;
} AccountCreatePayload; // 33 bytes

typedef struct __attribute__((packed)) {
    char     password[16];
    uint8_t  account_id;
    uint8_t  account_status;
    uint32_t ip_address;
} AccountUpdatePayload; // 22 bytes

typedef struct __attribute__((packed)) {
    uint8_t  server_id;
    uint8_t  padding;
    uint16_t log_length; // MUST BE LITTLE ENDIAN for dissector
} LogPayload;

typedef struct {
    char ip[20];
    int port;
    int my_port;
} ManagerInfo;

// Globals
uint8_t my_server_id = 0x01;
uint32_t my_server_ip = 0;
int manager_socket = -1;
int manager_connected = 0;
pthread_mutex_t manager_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- NEW: Client ID Counter and Mutex ---
uint8_t next_account_id = 1;
pthread_mutex_t acc_id_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Networking Functions ---

int send_binary_msg(int sock, uint8_t type, uint8_t status, const void *pay, uint32_t len) {
    GlobalHeader h = {
        .version = PROTOCOL_VERSION,
        .msg_type = type,
        .status = status,
        .padding = 0,
        .msg_length = htonl(len)
    };
    if (send(sock, &h, sizeof(h), MSG_NOSIGNAL) <= 0) return -1;
    if (len > 0 && pay) {
        if (send(sock, pay, len, MSG_NOSIGNAL) <= 0) return -1;
    }
    return 0;
}

int recv_binary_msg(int sock, GlobalHeader *h, void *pay, uint32_t max) {
    ssize_t n = recv(sock, h, sizeof(GlobalHeader), MSG_WAITALL);
    if (n <= 0) return -1;

    uint32_t len = ntohl(h->msg_length);
    if (len > max) return -2;

    if (len > 0) {
        n = recv(sock, pay, len, MSG_WAITALL);
        if (n != (ssize_t)len) return -1;
    }
    return (int)len;
}

// --- UI & Logging ---

void w_log_core(WINDOW *win, const char *msg) {
    pthread_mutex_lock(&ui_mutex);
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    wattron(win, COLOR_PAIR(1));
    wprintw(win, "[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);
    wattroff(win, COLOR_PAIR(1));
    wprintw(win, "%s\n", msg);
    wrefresh(win);
    pthread_mutex_unlock(&ui_mutex);
}

void send_log_to_manager(const char *log_msg) {
    pthread_mutex_lock(&manager_mutex);
    if (manager_connected && manager_socket >= 0) {
        uint16_t msg_len = (uint16_t)strlen(log_msg);
        uint8_t buffer[BUFFER_SIZE];
        LogPayload *lp = (LogPayload*)buffer;

        lp->server_id = my_server_id;
        lp->padding = 0;
        lp->log_length = htole16(msg_len); // Fix: Little Endian for Lua Dissector

        memcpy(buffer + sizeof(LogPayload), log_msg, msg_len);
        send_binary_msg(manager_socket, TYPE_LOG_REQ, 0x00, buffer, sizeof(LogPayload) + msg_len);
    }
    pthread_mutex_unlock(&manager_mutex);
}

void server_log(const char *fmt, ...) { char buf[1024]; va_list a; va_start(a, fmt); vsnprintf(buf, 1024, fmt, a); va_end(a); w_log_core(win_srv, buf); }
void manager_log(const char *fmt, ...) { char buf[1024]; va_list a; va_start(a, fmt); vsnprintf(buf, 1024, fmt, a); va_end(a); w_log_core(win_mgr, buf); }
void client_log(const char *fmt, ...) {
    char buf[1024]; va_list a; va_start(a, fmt); vsnprintf(buf, 1024, fmt, a); va_end(a);
    w_log_core(win_cli, buf); send_log_to_manager(buf);
}

// --- Business Logic ---

void* manager_connection_thread(void* arg) {
    ManagerInfo* info = (ManagerInfo*)arg;
    while(1) {
        manager_log(">>> Attempting Manager connection %s:%d", info->ip, info->port);
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in mgr_addr = { .sin_family = AF_INET, .sin_port = htons(info->port) };
        inet_pton(AF_INET, info->ip, &mgr_addr.sin_addr);

        if (connect(sock, (struct sockaddr *)&mgr_addr, sizeof(mgr_addr)) == 0) {
            manager_log(">>> Connected to Manager!");
            RegisterPayload reg = { .server_id = my_server_id, .server_ip = my_server_ip };
            send_binary_msg(sock, TYPE_SERVER_REG_REQ, 0x00, &reg, sizeof(reg));

            pthread_mutex_lock(&manager_mutex);
            manager_socket = sock;
            manager_connected = 1;
            pthread_mutex_unlock(&manager_mutex);

            GlobalHeader h;
            uint8_t buf[BUFFER_SIZE];
            while (recv_binary_msg(sock, &h, buf, BUFFER_SIZE) >= 0) {
                if (h.msg_type == TYPE_SERVER_REG_RES) {
                    my_server_id = ((RegisterPayload*)buf)->server_id;
                    manager_log("[ACK] Server ID: 0x%02X", my_server_id);
                } else if (h.msg_type == TYPE_SERVER_ACT_REQ) {
                    RegisterPayload act = { .server_id = my_server_id, .server_ip = my_server_ip };
                    send_binary_msg(sock, TYPE_SERVER_ACT_RES, 0x00, &act, sizeof(act));
                    manager_log("[ACTIVE] Server is now Live!");
                }
            }
        }
        manager_connected = 0; close(sock); sleep(5);
    }
}

void* handle_client(void* arg) {
    int sock = *(int*)arg;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    getpeername(sock, (struct sockaddr*)&addr, &alen);
    client_log("[CONNECT] %s", inet_ntoa(addr.sin_addr));

    GlobalHeader h;
    uint8_t buffer[BUFFER_SIZE];

    while(recv_binary_msg(sock, &h, buffer, BUFFER_SIZE) >= 0) {
        if (h.msg_type == TYPE_ACC_REG_REQ) {
            AccountCreatePayload *acc = (AccountCreatePayload*)buffer;

            // --- ASSIGN CLIENT ID ---
            pthread_mutex_lock(&acc_id_mutex);
            acc->account_id = next_account_id++;
            pthread_mutex_unlock(&acc_id_mutex);

            client_log("[REG] User: %.16s | Assigned ID: %d", acc->username, acc->account_id);

            // Send back the modified buffer containing the new ID
            send_binary_msg(sock, TYPE_ACC_REG_RES, 0x00, buffer, sizeof(AccountCreatePayload));
        }
        else if (h.msg_type == TYPE_ACC_UPDATE_REQ) {
            AccountUpdatePayload *acc = (AccountUpdatePayload*)buffer;
            client_log("[AUTH] ID: %d Status: %d", acc->account_id, acc->account_status);
            send_binary_msg(sock, TYPE_ACC_UPDATE_RES, 0x00, buffer, sizeof(AccountUpdatePayload));
        }
    }
    close(sock); free(arg); return NULL;
}

uint32_t get_my_ip() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53) };
    inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);
    connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    getsockname(sock, (struct sockaddr*)&local_addr, &addr_len);
    close(sock);
    return local_addr.sin_addr.s_addr;
}

int main(int argc, char *argv[]) {
    if (argc < 4) { printf("Usage: %s <Port> <Mgr_IP> <Mgr_Port>\n", argv[0]); return 1; }
    my_server_ip = get_my_ip();

    initscr(); start_color(); cbreak(); noecho(); curs_set(0);
    init_pair(1, COLOR_CYAN, COLOR_BLACK);
    int w = COLS / 3; int h = LINES - 2;
    win_srv = newwin(h, w - 1, 1, 0);
    win_cli = newwin(h, w - 1, 1, w);
    win_mgr = newwin(h, w - 1, 1, w * 2);
    scrollok(win_srv, 1); scrollok(win_cli, 1); scrollok(win_mgr, 1);
    attron(A_REVERSE);
    mvprintw(0, 1, " SERVER "); mvprintw(0, w + 1, " CLIENTS "); mvprintw(0, (w * 2) + 1, " MANAGER ");
    attroff(A_REVERSE); refresh();

    ManagerInfo *info = malloc(sizeof(ManagerInfo));
    strcpy(info->ip, argv[2]); info->port = atoi(argv[3]); info->my_port = atoi(argv[1]);

    pthread_t mgr_tid; pthread_create(&mgr_tid, NULL, manager_connection_thread, info);

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in saddr = { .sin_family = AF_INET, .sin_port = htons(info->my_port), .sin_addr.s_addr = INADDR_ANY };
    bind(srv_fd, (struct sockaddr *)&saddr, sizeof(saddr));
    listen(srv_fd, 10);

    server_log("Server online on port %d", info->my_port);

    while(1) {
        struct sockaddr_in caddr; socklen_t clen = sizeof(caddr);
        int csock = accept(srv_fd, (struct sockaddr *)&caddr, &clen);
        if (csock >= 0) {
            int *p = malloc(sizeof(int)); *p = csock;
            pthread_t t; pthread_create(&t, NULL, handle_client, p); pthread_detach(t);
        }
    }
    endwin();
    return 0;
}