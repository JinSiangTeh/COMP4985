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

#define MAX_CLIENTS 100
#define BUFFER_SIZE 65536
#define MAX_LOG_SIZE 65535

// Protocol Version
#define PROTOCOL_MAJOR 0
#define PROTOCOL_MINOR 1

// Resource Types
#define RESOURCE_SERVER   0x00
#define RESOURCE_ACTIVATE 0x01
#define RESOURCE_ACCOUNT  0x02
#define RESOURCE_LOG      0x03

// CRUD Operations
#define CRUD_CREATE  0x00
#define CRUD_READ    0x01
#define CRUD_UPDATE  0x02
#define CRUD_DELETE  0x03

// ACK bit
#define ACK_REQUEST  0x00
#define ACK_RESPONSE 0x01

// UI Globals
WINDOW *win_srv, *win_cli, *win_mgr;
pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;

// Global Header (8 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  status;
    uint8_t  padding;
    uint32_t msg_length;
} GlobalHeader;

// Message payloads
typedef struct __attribute__((packed)) {
    uint32_t server_ip;
    uint8_t  server_id;
} RegisterPayload;

typedef struct __attribute__((packed)) {
    uint32_t server_ip;
    uint8_t  server_id;
} ActivatePayload;

typedef struct __attribute__((packed)) {
    char     username[16];
    char     password[16];
    uint8_t  client_id;
    uint8_t  status;
} AccountPayload;

typedef struct __attribute__((packed)) {
    uint8_t  server_id;
    uint16_t log_length;
} LogPayload;

typedef struct {
    char ip[20];
    int port;
    int my_port;
} ManagerInfo;

// Globals
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t manager_mutex = PTHREAD_MUTEX_INITIALIZER;
int client_sockets[MAX_CLIENTS] = {0};
int manager_connected = 0;
int manager_socket = -1;
char manager_ip[20];
int manager_port;
uint8_t my_server_id = 0x00;
uint32_t my_server_ip = 0;

// --- Helper Functions ---

uint8_t make_version(uint8_t major, uint8_t minor) {
    return ((major & 0x0F) << 4) | (minor & 0x0F);
}

uint8_t make_msg_type(uint8_t resource, uint8_t crud, uint8_t ack) {
    return ((resource & 0x1F) << 3) | ((crud & 0x03) << 1) | (ack & 0x01);
}

void parse_msg_type(uint8_t msg_type, uint8_t *resource, uint8_t *crud, uint8_t *ack) {
    *resource = (msg_type >> 3) & 0x1F;
    *crud = (msg_type >> 1) & 0x03;
    *ack = msg_type & 0x01;
}

void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%H:%M:%S", t);
}

// --- Ncurses Drawing Logic ---

void w_log_core(WINDOW *win, const char *msg) {
    pthread_mutex_lock(&ui_mutex);
    char ts[16];
    get_timestamp(ts, sizeof(ts));

    wattron(win, COLOR_PAIR(1));
    wprintw(win, "[%s] ", ts);
    wattroff(win, COLOR_PAIR(1));

    wprintw(win, "%s\n", msg);
    wrefresh(win);
    pthread_mutex_unlock(&ui_mutex);
}

// --- Binary Protocol Functions (Logic Unchanged) ---

int send_binary_msg(int sock, uint8_t resource, uint8_t crud, uint8_t ack,
                    uint8_t status, const void *payload, uint32_t payload_len) {
    GlobalHeader header;
    header.version = make_version(PROTOCOL_MAJOR, PROTOCOL_MINOR);
    header.msg_type = make_msg_type(resource, crud, ack);
    header.status = status;
    header.padding = 0x00;
    header.msg_length = htonl(payload_len);

    if (send(sock, &header, sizeof(GlobalHeader), MSG_NOSIGNAL) != sizeof(GlobalHeader)) return -1;
    if (payload_len > 0 && payload != NULL) {
        if (send(sock, payload, payload_len, MSG_NOSIGNAL) != payload_len) return -1;
    }
    return 0;
}

int recv_binary_msg(int sock, GlobalHeader *header, void *payload, uint32_t max_payload_len) {
    ssize_t received = recv(sock, header, sizeof(GlobalHeader), MSG_WAITALL);
    if (received != sizeof(GlobalHeader)) return -1;
    uint32_t payload_len = ntohl(header->msg_length);
    if (payload_len > max_payload_len) return -2;
    if (payload_len > 0 && payload != NULL) {
        received = recv(sock, payload, payload_len, MSG_WAITALL);
        if (received != payload_len) return -1;
    }
    return payload_len;
}

void send_log_to_manager(const char *log_msg) {
    pthread_mutex_lock(&manager_mutex);
    if (manager_connected && manager_socket >= 0) {
        uint16_t log_len = strlen(log_msg);
        if (log_len > MAX_LOG_SIZE) log_len = MAX_LOG_SIZE;
        uint8_t buffer[3 + MAX_LOG_SIZE];
        LogPayload *lp = (LogPayload*)buffer;
        lp->server_id = my_server_id;
        lp->log_length = htons(log_len);
        memcpy(buffer + 3, log_msg, log_len);
        if (send_binary_msg(manager_socket, RESOURCE_LOG, CRUD_CREATE, ACK_REQUEST, 0x00, buffer, 3 + log_len) < 0) {
            manager_connected = 0;
            close(manager_socket);
            manager_socket = -1;
        }
    }
    pthread_mutex_unlock(&manager_mutex);
}

// --- New Variadic Logging Wrappers (Fixed Compiler Errors) ---

void server_log(const char *fmt, ...) {
    char buf[1024];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    w_log_core(win_srv, buf);
}

void manager_log(const char *fmt, ...) {
    char buf[1024];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    w_log_core(win_mgr, buf);
}

void client_log(const char *fmt, ...) {
    char buf[1024];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    w_log_core(win_cli, buf);
    send_log_to_manager(buf);
}

// --- Threads ---

void* manager_connection_thread(void* arg) {
    ManagerInfo* info = (ManagerInfo*)arg;
    manager_log(">>> Attempting connection to Manager %s:%d", info->ip, info->port);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in mgr_addr = { .sin_family = AF_INET, .sin_port = htons(info->port) };
    inet_pton(AF_INET, info->ip, &mgr_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&mgr_addr, sizeof(mgr_addr)) == 0) {
        manager_log(">>> Connected to Manager!");

        RegisterPayload reg = { .server_id = my_server_id, .server_ip = my_server_ip };
        send_binary_msg(sock, RESOURCE_SERVER, CRUD_CREATE, ACK_REQUEST, 0x00, &reg, sizeof(reg));

        pthread_mutex_lock(&manager_mutex);
        manager_socket = sock; manager_connected = 1;
        pthread_mutex_unlock(&manager_mutex);

        GlobalHeader header;
        uint8_t buffer[BUFFER_SIZE];
        while (recv_binary_msg(sock, &header, buffer, BUFFER_SIZE) >= 0) {
            uint8_t res, crud, ack;
            parse_msg_type(header.msg_type, &res, &crud, &ack);

            if (res == RESOURCE_SERVER && ack == ACK_RESPONSE) {
                my_server_id = ((RegisterPayload*)buffer)->server_id;
                manager_log("[ACK] Server ID assigned: 0x%02X", my_server_id);
            } else if (res == RESOURCE_ACTIVATE) {
                manager_log("[CMD] Received ACTIVATE command");
                send_binary_msg(sock, RESOURCE_ACTIVATE, CRUD_CREATE, ACK_RESPONSE, 0x00, buffer, sizeof(ActivatePayload));
            }
        }
    } else {
        manager_log("[ERROR] Connection failed: %s", strerror(errno));
    }
    close(sock); free(info);
    return NULL;
}

void* handle_client(void* arg) {
    int sock = *(int*)arg;
    struct sockaddr_in addr; socklen_t len = sizeof(addr);
    getpeername(sock, (struct sockaddr*)&addr, &len);

    client_log("[CONNECT] %s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

    GlobalHeader header;
    uint8_t buffer[BUFFER_SIZE];
    while(recv_binary_msg(sock, &header, buffer, BUFFER_SIZE) >= 0) {
        uint8_t res, crud, ack;
        parse_msg_type(header.msg_type, &res, &crud, &ack);

        if (res == RESOURCE_ACCOUNT) {
            AccountPayload *acc = (AccountPayload*)buffer;
            client_log("[ACC] User:%.16s Type:%d", acc->username, crud);
            send_binary_msg(sock, RESOURCE_ACCOUNT, crud, ACK_RESPONSE, 0x00, acc, sizeof(AccountPayload));
            if(crud == CRUD_UPDATE && acc->status == 0x00) break;
        }
    }
    client_log("[DISCONNECT] %s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    close(sock); free(arg);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: %s <My_Port> <Manager_IP> <Manager_Port>\n", argv[0]);
        return 1;
    }

    // Initialize Ncurses UI
    initscr(); start_color(); cbreak(); noecho(); curs_set(0);
    init_pair(1, COLOR_CYAN, COLOR_BLACK);

    int w = COLS / 3; int h = LINES - 2;
    win_srv = newwin(h, w - 1, 1, 0);
    win_cli = newwin(h, w - 1, 1, w);
    win_mgr = newwin(h, w - 1, 1, w * 2);
    scrollok(win_srv, TRUE); scrollok(win_cli, TRUE); scrollok(win_mgr, TRUE);

    attron(A_REVERSE);
    mvprintw(0, 1, " SERVER LOG ");
    mvprintw(0, w + 1, " CLIENT LOG ");
    mvprintw(0, (w * 2) + 1, " MANAGER LOG ");
    attroff(A_REVERSE);
    refresh();

    ManagerInfo *info = malloc(sizeof(ManagerInfo));
    strcpy(info->ip, argv[2]);
    info->port = atoi(argv[3]);
    info->my_port = atoi(argv[1]);

    server_log("Binary Server v0.1 starting on port %d...", info->my_port);

    pthread_t mgr_tid;
    pthread_create(&mgr_tid, NULL, manager_connection_thread, (void*)info);
    pthread_detach(mgr_tid);

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in srv_addr = { .sin_family = AF_INET, .sin_port = htons(info->my_port), .sin_addr.s_addr = INADDR_ANY };

    if(bind(srv_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        server_log("[FATAL] Bind failed!");
        refresh(); sleep(2); endwin(); return 1;
    }
    listen(srv_fd, 10);
    fcntl(srv_fd, F_SETFL, fcntl(srv_fd, F_GETFL, 0) | O_NONBLOCK);

    while(1) {
        struct sockaddr_in c_addr; socklen_t c_len = sizeof(c_addr);
        int client_sock = accept(srv_fd, (struct sockaddr *)&c_addr, &c_len);
        if (client_sock >= 0) {
            int *new_sock = malloc(sizeof(int)); *new_sock = client_sock;
            pthread_t t; pthread_create(&t, NULL, handle_client, (void*)new_sock);
            pthread_detach(t);
        }
        usleep(100000);
    }

    endwin();
    return 0;
}
