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

//Binary Protocol

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  status;
    uint8_t  padding;
    uint32_t msg_length;
} GlobalHeader;

typedef struct __attribute__((packed)) {
    uint32_t server_ip;
    uint8_t  server_id;
} RegisterPayload;

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

// Global
uint8_t my_server_id = 0x01;
uint32_t my_server_ip = 0;
int manager_socket = -1;
int manager_connected = 0;
pthread_mutex_t manager_mutex = PTHREAD_MUTEX_INITIALIZER;



uint8_t make_version(uint8_t major, uint8_t minor) { return ((major & 0x0F) << 4) | (minor & 0x0F); }

uint8_t make_msg_type(uint8_t res, uint8_t crud, uint8_t ack) {
    return ((res & 0x1F) << 3) | ((crud & 0x03) << 1) | (ack & 0x01);
}

void parse_msg_type(uint8_t msg_type, uint8_t *res, uint8_t *crud, uint8_t *ack) {
    *res = (msg_type >> 3) & 0x1F;
    *crud = (msg_type >> 1) & 0x03;
    *ack = msg_type & 0x01;
}

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

// networking

int send_binary_msg(int sock, uint8_t res, uint8_t crud, uint8_t ack, uint8_t status, const void *pay, uint32_t len) {
    GlobalHeader h = { make_version(PROTOCOL_MAJOR, PROTOCOL_MINOR), make_msg_type(res, crud, ack), status, 0, htonl(len) };
    if (send(sock, &h, sizeof(h), MSG_NOSIGNAL) <= 0) return -1;
    if (len > 0 && pay) if (send(sock, pay, len, MSG_NOSIGNAL) <= 0) return -1;
    return 0;
}

int recv_binary_msg(int sock, GlobalHeader *h, void *pay, uint32_t max) {
    ssize_t n = recv(sock, h, sizeof(GlobalHeader), MSG_WAITALL);


    if (n == 0) return -1;


    if (n < 0) return -1;


    if (n != sizeof(GlobalHeader)) return -1;

    uint32_t len = ntohl(h->msg_length);
    if (len > max) return -2;

    if (len > 0) {
        n = recv(sock, pay, len, MSG_WAITALL);
        if (n != (ssize_t)len) return -1;
    }

    return len;
}

// logging

void send_log_to_manager(const char *log_msg) {
    pthread_mutex_lock(&manager_mutex);
    if (manager_connected && manager_socket >= 0) {
        uint16_t log_len = strlen(log_msg);
        uint8_t buffer[BUFFER_SIZE];
        LogPayload *lp = (LogPayload*)buffer;
        lp->server_id = my_server_id;
        lp->log_length = htons(log_len);
        memcpy(buffer + 3, log_msg, log_len);
        send_binary_msg(manager_socket, RESOURCE_LOG, CRUD_CREATE, ACK_REQUEST, 0x00, buffer, 3 + log_len);
    }
    pthread_mutex_unlock(&manager_mutex);
}

void server_log(const char *fmt, ...) { char buf[1024]; va_list a; va_start(a, fmt); vsnprintf(buf, 1024, fmt, a); va_end(a); w_log_core(win_srv, buf); }
void manager_log(const char *fmt, ...) { char buf[1024]; va_list a; va_start(a, fmt); vsnprintf(buf, 1024, fmt, a); va_end(a); w_log_core(win_mgr, buf); }
void client_log(const char *fmt, ...) {
    char buf[1024]; va_list a; va_start(a, fmt); vsnprintf(buf, 1024, fmt, a); va_end(a);
    w_log_core(win_cli, buf); send_log_to_manager(buf);
}


uint32_t get_my_ip() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 0;
    }


    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),  // DNS port
    };
    inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 0;
    }

    // Get the local address that was selected
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(sock, (struct sockaddr*)&local_addr, &addr_len) < 0) {
        perror("getsockname");
        close(sock);
        return 0;
    }

    close(sock);

    // Log the detected IP
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local_addr.sin_addr, ip_str, sizeof(ip_str));
    printf("Detected server IP: %s\n", ip_str);

    return local_addr.sin_addr.s_addr;  // Already in network byte order
}

// thread

void* manager_connection_thread(void* arg) {
    ManagerInfo* info = (ManagerInfo*)arg;
    while(1) {
        manager_log(">>> Attempting Manager connection %s:%d", info->ip, info->port);
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in mgr_addr = { .sin_family = AF_INET, .sin_port = htons(info->port) };
        inet_pton(AF_INET, info->ip, &mgr_addr.sin_addr);

        if (connect(sock, (struct sockaddr *)&mgr_addr, sizeof(mgr_addr)) == 0) {
            manager_log(">>> Connected to Manager!");

            // Step 1: Register with manager
            RegisterPayload reg = { .server_id = my_server_id, .server_ip = my_server_ip };
            send_binary_msg(sock, RESOURCE_SERVER, CRUD_CREATE, ACK_REQUEST, 0x00, &reg, sizeof(reg));

            pthread_mutex_lock(&manager_mutex);
            manager_socket = sock;
            manager_connected = 1;
            pthread_mutex_unlock(&manager_mutex);

            GlobalHeader h;
            uint8_t buf[BUFFER_SIZE];

            while (recv_binary_msg(sock, &h, buf, BUFFER_SIZE) >= 0) {
                uint8_t res, crud, ack;
                parse_msg_type(h.msg_type, &res, &crud, &ack);


                if (res == RESOURCE_SERVER && crud == CRUD_CREATE && ack == ACK_RESPONSE) {
                    my_server_id = ((RegisterPayload*)buf)->server_id;
                    manager_log("[ACK] Server ID assigned: 0x%02X", my_server_id);
                }

                else if (res == RESOURCE_ACTIVATE && crud == CRUD_CREATE && ack == ACK_REQUEST) {
                    manager_log("[ACTIVATE REQUEST] Manager requesting activation");


                    RegisterPayload activate_response = {
                        .server_id = my_server_id,
                        .server_ip = my_server_ip
                    };
                    send_binary_msg(sock, RESOURCE_ACTIVATE, CRUD_CREATE, ACK_RESPONSE, 0x00,
                                    &activate_response, sizeof(activate_response));
                    manager_log("[ACTIVE] Sent activation response - Server is now ACTIVE!");
                }
            }
        }

        manager_log("[ERR] Connection lost. Retrying in 5s...");
        pthread_mutex_lock(&manager_mutex);
        manager_connected = 0;
        close(sock);
        pthread_mutex_unlock(&manager_mutex);
        sleep(5);
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


    while(1) {
        int recv_status = recv_binary_msg(sock, &h, buffer, BUFFER_SIZE);


        if (recv_status == -1) {
            client_log("[DISCONNECT] %s (connection closed)", inet_ntoa(addr.sin_addr));
            break;
        }


        if (recv_status == -2) {
            client_log("[ERROR] Payload too large from %s", inet_ntoa(addr.sin_addr));
            continue;
        }

        uint8_t res, crud, ack;
        parse_msg_type(h.msg_type, &res, &crud, &ack);


        if (res == RESOURCE_ACTIVATE) {
            if (crud == CRUD_READ && ack == ACK_REQUEST) {
                client_log("[REQ] Activation request from %s", inet_ntoa(addr.sin_addr));
                RegisterPayload rp = { .server_ip = my_server_ip, .server_id = my_server_id };

                if (send_binary_msg(sock, RESOURCE_ACTIVATE, CRUD_READ, ACK_RESPONSE, 0x00, &rp, sizeof(rp)) < 0) {
                    client_log("[ERROR] Failed to send activation response");
                    break;
                }

                client_log("[SENT] Activation response (ID=0x%02X)", my_server_id);
            }
        }

        else if (res == RESOURCE_ACCOUNT) {
            AccountPayload *acc = (AccountPayload*)buffer;

            if (crud == CRUD_CREATE && ack == ACK_REQUEST) {
                client_log("[ACC-CREATE] User=%.16s", acc->username);

                if (send_binary_msg(sock, RESOURCE_ACCOUNT, CRUD_CREATE, ACK_RESPONSE, 0x00, buffer, sizeof(AccountPayload)) < 0) {
                    client_log("[ERROR] Failed to send create ACK");
                    break;
                }
            }
            else if (crud == CRUD_UPDATE && ack == ACK_REQUEST) {
                const char *action = (acc->status == 0x01) ? "LOGIN" : "LOGOUT";
                client_log("[ACC-%s] User=%.16s", action, acc->username);

                if (send_binary_msg(sock, RESOURCE_ACCOUNT, CRUD_UPDATE, ACK_RESPONSE, 0x00, buffer, sizeof(AccountPayload)) < 0) {
                    client_log("[ERROR] Failed to send login/logout ACK");
                    break;
                }
            }
        }
        else {
            client_log("[WARN] Unknown message: res=0x%02X crud=0x%02X ack=0x%02X", res, crud, ack);
        }
    }

    close(sock);
    free(arg);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) { printf("Usage: %s <Port> <Mgr_IP> <Mgr_Port>\n", argv[0]); return 1; }

    // Dynamically detect server IP BEFORE initializing ncurses
    my_server_ip = get_my_ip();
    if (my_server_ip == 0) {
        fprintf(stderr, "ERROR: Could not detect server IP address!\n");
        return 1;
    }

    initscr(); start_color(); cbreak(); noecho(); curs_set(0);
    init_pair(1, COLOR_CYAN, COLOR_BLACK);
    int w = COLS / 3; int h = LINES - 2;
    win_srv = newwin(h, w - 1, 1, 0);
    win_cli = newwin(h, w - 1, 1, w);
    win_mgr = newwin(h, w - 1, 1, w * 2);
    scrollok(win_srv, 1); scrollok(win_cli, 1); scrollok(win_mgr, 1);

    attron(A_REVERSE);
    mvprintw(0, 1, " SERVER LOG "); mvprintw(0, w + 1, " CLIENT LOG "); mvprintw(0, (w * 2) + 1, " MANAGER LOG ");
    attroff(A_REVERSE); refresh();

    ManagerInfo *info = malloc(sizeof(ManagerInfo));
    strcpy(info->ip, argv[2]); info->port = atoi(argv[3]); info->my_port = atoi(argv[1]);

    pthread_t mgr_tid; pthread_create(&mgr_tid, NULL, manager_connection_thread, info);

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in saddr = { .sin_family = AF_INET, .sin_port = htons(info->my_port), .sin_addr.s_addr = INADDR_ANY };
    bind(srv_fd, (struct sockaddr *)&saddr, sizeof(saddr));
    listen(srv_fd, 10);
    fcntl(srv_fd, F_SETFL, O_NONBLOCK);

    server_log("Server online on port %d", info->my_port);

    while(1) {
        struct sockaddr_in caddr; socklen_t clen = sizeof(caddr);
        int csock = accept(srv_fd, (struct sockaddr *)&caddr, &clen);
        if (csock >= 0) {

            int flags = fcntl(csock, F_GETFL, 0);
            fcntl(csock, F_SETFL, flags & ~O_NONBLOCK);

            int *p = malloc(sizeof(int)); *p = csock;
            pthread_t t; pthread_create(&t, NULL, handle_client, p); pthread_detach(t);
        }
        usleep(100000);
    }

    endwin();
    return 0;
}