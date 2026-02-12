#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>
#include <ncurses.h>
#include <pthread.h>
#include <errno.h>

#define PROTOCOL_VERSION 0x01

#define BUFFER_SIZE 65536

// Resource Types
#define RES_SERVER   0x00
#define RES_ACTIVATE 0x01
#define RES_ACCOUNT  0x02
#define RES_LOG      0x03

// CRUD Operations
#define CRUD_CREATE  0x00
#define CRUD_READ    0x01
#define CRUD_UPDATE  0x02
#define CRUD_DELETE  0x03

// ACK bits
#define ACK_REQ      0x00
#define ACK_RES      0x01

typedef struct __attribute__((packed)) {
    uint32_t server_ip;
    uint8_t  server_id;
} RegisterPayload;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  status;
    uint8_t  padding;
    uint32_t msg_length;
} GlobalHeader;

typedef struct __attribute__((packed)) {
    char     username[16];
    char     password[16];
    uint8_t  client_id;
    uint8_t  status;
} AccountPayload;

// UI Globals
WINDOW *win_main, *win_log, *win_status;
pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;

// Global
int sock_fd = -1;
volatile int connected = 0;
volatile int should_run = 1;
struct sockaddr_in srv_addr_copy;



void log_msg(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    pthread_mutex_lock(&ui_mutex);
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    wprintw(win_log, "[%02d:%02d:%02d] %s\n", t->tm_hour, t->tm_min, t->tm_sec, buf);
    wrefresh(win_log);
    pthread_mutex_unlock(&ui_mutex);
}

void update_status(const char *msg) {
    pthread_mutex_lock(&ui_mutex);
    werase(win_status);
    box(win_status, 0, 0);
    mvwprintw(win_status, 1, 2, "Status: %s", connected ? "CONNECTED" : "RECONNECTING...");
    if(msg) mvwprintw(win_status, 3, 2, "Note: %s", msg);
    wrefresh(win_status);
    pthread_mutex_unlock(&ui_mutex);
}

int send_msg(uint8_t res, uint8_t crud, uint8_t ack, uint8_t status, const void *pay, uint32_t len) {
    if (!connected) return -1;

    uint8_t type = ((res & 0x1F) << 3) | ((crud & 0x03) << 1) | (ack & 0x01);

    GlobalHeader h = {
        .version = PROTOCOL_VERSION,
        .msg_type = type,
        .status = status,
        .padding = 0x00,
        .msg_length = htonl(len)
    };

    if (send(sock_fd, &h, sizeof(h), 0) <= 0) {
        connected = 0;
        return -1;
    }
    if (len > 0 && pay) {
        if (send(sock_fd, pay, len, 0) <= 0) {
            connected = 0;
            return -1;
        }
    }
    return 0;
}

//thread

void* receiver_thread(void* arg) {
    GlobalHeader h;
    uint8_t buf[BUFFER_SIZE];

    while (should_run) {
        if (!connected) {
            usleep(100000);
            continue;
        }

        ssize_t n = recv(sock_fd, &h, sizeof(h), MSG_DONTWAIT);

        if (n == sizeof(h)) {
            uint32_t len = ntohl(h.msg_length);
            if (len > 0 && len < BUFFER_SIZE) {
                recv(sock_fd, buf, len, MSG_WAITALL);
            }

            uint8_t res = (h.msg_type >> 3) & 0x1F;
            uint8_t crud = (h.msg_type >> 1) & 0x03;
            uint8_t ack = h.msg_type & 0x01;

            if (res == RES_ACTIVATE && crud == CRUD_READ && ack == ACK_RES) {
                if (len == sizeof(RegisterPayload)) {
                    RegisterPayload *rp = (RegisterPayload*)buf;
                    log_msg("Server Active: ID 0x%02X", rp->server_id);
                }
            }
            else if (res == RES_ACCOUNT && ack == ACK_RES) {
                if (crud == CRUD_CREATE) {
                    log_msg("Account created successfully");
                } else if (crud == CRUD_UPDATE) {
                    log_msg("Login/Logout confirmed");
                }
            }
        }
        else if (n == 0) {
            connected = 0;
            log_msg("Server hung up.");
        }
        else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
            connected = 0;
        }
        usleep(10000);
    }
    return NULL;
}

//main

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <IP> <Port>\n", argv[0]);
        return 1;
    }

    srv_addr_copy.sin_family = AF_INET;
    srv_addr_copy.sin_port = htons(atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &srv_addr_copy.sin_addr) <= 0) {
        printf("Invalid IP address: %s\n", argv[1]);
        return 1;
    }

    initscr(); start_color(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    init_pair(1, COLOR_CYAN, COLOR_BLACK);

    win_main = newwin(LINES-8, COLS/2, 0, 0);
    win_log = newwin(LINES-8, COLS/2, 0, COLS/2);
    win_status = newwin(8, COLS, LINES-8, 0);
    scrollok(win_log, 1);
    box(win_main, 0, 0); box(win_log, 0, 0); mvwprintw(win_log, 0, 2, " Activity Log ");

    pthread_t r_tid;
    pthread_create(&r_tid, NULL, receiver_thread, NULL);

    while (should_run) {
        if (!connected) {
            update_status("Searching for server...");
            if (sock_fd >= 0) close(sock_fd);
            sock_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (connect(sock_fd, (struct sockaddr *)&srv_addr_copy, sizeof(srv_addr_copy)) == 0) {
                connected = 1;
                log_msg("Connected to server.");
                send_msg(RES_ACTIVATE, CRUD_READ, ACK_REQ, 0, NULL, 0);
            }
            sleep(2);
            continue;
        }

        pthread_mutex_lock(&ui_mutex);
        werase(win_main); box(win_main, 0, 0);
        mvwprintw(win_main, 0, 2, " MENU ");
        mvwprintw(win_main, 2, 2, "1. Refresh Status");
        mvwprintw(win_main, 3, 2, "2. Create Account");
        mvwprintw(win_main, 4, 2, "3. Login");
        mvwprintw(win_main, 5, 2, "4. Logout");
        mvwprintw(win_main, 6, 2, "5. Exit");
        wrefresh(win_main);
        pthread_mutex_unlock(&ui_mutex);

        timeout(500);
        int ch = getch();

        if (ch == '1') {
            send_msg(RES_ACTIVATE, CRUD_READ, ACK_REQ, 0, NULL, 0);
        }

        if (ch == '2') {
            AccountPayload acc = {0};
            echo(); curs_set(1);
            pthread_mutex_lock(&ui_mutex);
            mvwprintw(win_main, 8, 2, "User: "); wrefresh(win_main);
            wgetnstr(win_main, acc.username, 15);
            mvwprintw(win_main, 9, 2, "Pass: "); wrefresh(win_main);
            wgetnstr(win_main, acc.password, 15);
            pthread_mutex_unlock(&ui_mutex);
            noecho(); curs_set(0);
            acc.status = 0x01;
            send_msg(RES_ACCOUNT, CRUD_CREATE, ACK_REQ, 0, &acc, sizeof(acc));
        }

        if (ch == '3') {
            AccountPayload acc = {0};
            echo(); curs_set(1);
            pthread_mutex_lock(&ui_mutex);
            mvwprintw(win_main, 8, 2, "User: "); wrefresh(win_main);
            wgetnstr(win_main, acc.username, 15);
            mvwprintw(win_main, 9, 2, "Pass: "); wrefresh(win_main);
            wgetnstr(win_main, acc.password, 15);
            pthread_mutex_unlock(&ui_mutex);
            noecho(); curs_set(0);
            acc.status = 0x01;
            send_msg(RES_ACCOUNT, CRUD_UPDATE, ACK_REQ, 0, &acc, sizeof(acc));
        }

        if (ch == '4') {
            AccountPayload acc = {0};
            echo(); curs_set(1);
            pthread_mutex_lock(&ui_mutex);
            mvwprintw(win_main, 8, 2, "User: "); wrefresh(win_main);
            wgetnstr(win_main, acc.username, 15);
            mvwprintw(win_main, 9, 2, "Pass: "); wrefresh(win_main);
            wgetnstr(win_main, acc.password, 15);
            pthread_mutex_unlock(&ui_mutex);
            noecho(); curs_set(0);
            acc.status = 0x00; // Logout
            send_msg(RES_ACCOUNT, CRUD_UPDATE, ACK_REQ, 0, &acc, sizeof(acc));
        }

        if (ch == '5' || ch == 'q') {
            should_run = 0;
        }
    }

    endwin();
    should_run = 0;
    pthread_join(r_tid, NULL);
    if (sock_fd >= 0) close(sock_fd);
    return 0;
}