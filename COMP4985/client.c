#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>
#include <ncurses.h>
#include <pthread.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/fcntl.h>
#include <signal.h>

#define PROTOCOL_VERSION 0x01
#define BUFFER_SIZE 65536
#define MAX_MESSAGE_LEN 512

// Resource Types
#define RES_SERVER   0x00
#define RES_ACTIVATE 0x01
#define RES_ACCOUNT  0x02
#define RES_LOG      0x03
#define RES_MESSAGE  0x04

// CRUD Operations
#define CRUD_CREATE  0x00
#define CRUD_READ    0x01
#define CRUD_UPDATE  0x02
#define CRUD_DELETE  0x03

// ACK bits
#define ACK_REQ      0x00
#define ACK_RES      0x01

// Strict Protocol Structs
typedef struct __attribute__((packed)) {
    uint32_t server_ip;
    uint8_t  server_id;
} RegisterPayload;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  status;
    uint8_t  padding;    // Byte 3: Mandatory padding per Wireshark spec
    uint32_t msg_length; // Bytes 4-7
} GlobalHeader;

typedef struct __attribute__((packed)) {
    char     username[16];
    char     password[16];
    uint8_t  account_id;
} AccountCreatePayload; // 33 bytes total

typedef struct __attribute__((packed)) {
    char     password[16];
    uint8_t  account_id;
    uint8_t  account_status;
    uint32_t ip_address;
} AccountUpdatePayload; // 22 bytes total

typedef struct __attribute__((packed)) {
    char     from_user[16];
    char     to_user[16];
    uint16_t msg_length;
} MessagePayload;

// UI Globals
WINDOW *win_main, *win_log, *win_status, *win_chat, *win_input;
pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;

// Global
int sock_fd = -1;
volatile int connected = 0;
volatile int should_run = 1;
volatile int login_response_received = 0;
volatile int login_success = 0;
<<<<<<< HEAD
volatile int account_create_response_received = 0;
volatile int account_create_success = 0;
volatile int logout_response_received = 0;
=======
>>>>>>> origin/main
struct sockaddr_in srv_addr;
char my_username[16] = {0};
char saved_password[16] = {0};
int logged_in = 0;
uint8_t last_server_id = 0;
uint8_t my_client_id = 0;

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

<<<<<<< HEAD
=======
//chat message
>>>>>>> origin/main
void chat_msg(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    pthread_mutex_lock(&ui_mutex);
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    wprintw(win_chat, "[%02d:%02d:%02d] %s\n", t->tm_hour, t->tm_min, t->tm_sec, buf);
    wrefresh(win_chat);
    pthread_mutex_unlock(&ui_mutex);
}

<<<<<<< HEAD
=======
//status log the ui at the bottom
>>>>>>> origin/main
void update_status(const char *msg) {
    pthread_mutex_lock(&ui_mutex);
    werase(win_status);
    box(win_status, 0, 0);
    mvwprintw(win_status, 1, 2, "Status: %s", connected ? "CONNECTED" : "RECONNECTING...");
    if (logged_in) mvwprintw(win_status, 2, 2, "User: %s (ID: %d)", my_username, my_client_id);
    if (last_server_id != 0) mvwprintw(win_status, 2, COLS - 20, "Server: 0x%02X", last_server_id);
    if(msg) mvwprintw(win_status, 3, 2, "Note: %s", msg);
    wrefresh(win_status);
    pthread_mutex_unlock(&ui_mutex);
}

void dump_hex(const char *label, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    char hex_str[1024] = {0};
    for (size_t i = 0; i < len && i < 64; i++) {
        sprintf(hex_str + strlen(hex_str), "%02x ", p[i]);
        if ((i + 1) % 16 == 0) sprintf(hex_str + strlen(hex_str), "\n");
    }
    log_msg("%s (%zu bytes):\n%s", label, len, hex_str);
}

int send_msg(uint8_t res, uint8_t crud, uint8_t ack, uint8_t status, const void *pay, uint32_t len) {
    pthread_mutex_lock(&sock_mutex);
    if (!connected) {
        pthread_mutex_unlock(&sock_mutex);
        return -1;
    }

    uint8_t type = ((res & 0x1F) << 3) | ((crud & 0x03) << 1) | (ack & 0x01);
    GlobalHeader h = {
        .version = PROTOCOL_VERSION,
        .msg_type = type,
        .status = status,
        .padding = 0x00,
        .msg_length = htonl(len)
    };

    uint8_t total_packet[sizeof(GlobalHeader) + len];
    memcpy(total_packet, &h, sizeof(GlobalHeader));
    if (len > 0 && pay) {
        memcpy(total_packet + sizeof(GlobalHeader), pay, len);
    }

    dump_hex("SENDING", total_packet, sizeof(GlobalHeader) + len);

    ssize_t sent = send(sock_fd, total_packet, sizeof(GlobalHeader) + len, 0);
    if (sent <= 0) {
        log_msg("SEND FAILED: %s", strerror(errno));
        connected = 0;
        pthread_mutex_unlock(&sock_mutex);
        return -1;
    }
    pthread_mutex_unlock(&sock_mutex);
    return 0;
}

<<<<<<< HEAD
void get_text_input(const char *prompt, char *buffer, int max_len) {
    pthread_mutex_lock(&ui_mutex);
    int win_height = 5, win_width = 60;
    if (win_width > COLS - 4) win_width = COLS - 4;
    int start_y = (LINES - win_height) / 2, start_x = (COLS - win_width) / 2;
    if (win_input) delwin(win_input);
    win_input = newwin(win_height, win_width, start_y, start_x);
=======
//input area
void get_text_input(const char *prompt, char *buffer, int max_len) {
    pthread_mutex_lock(&ui_mutex);

    int win_height = 5;
    int win_width = 60;
    if (win_width > COLS - 4) win_width = COLS - 4;

    int start_y = (LINES - win_height) / 2;
    int start_x = (COLS - win_width) / 2;

    if (win_input) {
        delwin(win_input);
    }

    win_input = newwin(win_height, win_width, start_y, start_x);

>>>>>>> origin/main
    wbkgd(win_input, COLOR_PAIR(1));
    box(win_input, 0, 0);
    mvwprintw(win_input, 0, 2, " %s ", prompt);
    mvwprintw(win_input, 2, 2, "> ");
<<<<<<< HEAD
    keypad(win_input, TRUE);
    wmove(win_input, 2, 4);
    wrefresh(win_input);
    curs_set(1);
    pthread_mutex_unlock(&ui_mutex);

    int i = 0, ch;
    memset(buffer, 0, max_len + 1);
=======

    keypad(win_input, TRUE);

    wmove(win_input, 2, 4);
    wrefresh(win_input);

    curs_set(1);

    pthread_mutex_unlock(&ui_mutex);

    nodelay(win_input, FALSE);

    int i = 0;
    int ch;
    memset(buffer, 0, max_len + 1);

    pthread_mutex_lock(&ui_mutex);
>>>>>>> origin/main
    while (i < max_len && (ch = wgetch(win_input)) != '\n' && ch != KEY_ENTER) {
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (i > 0) {
                i--; buffer[i] = '\0';
                int y, x; getyx(win_input, y, x);
                mvwaddch(win_input, y, x - 1, ' '); wmove(win_input, y, x - 1);
                wrefresh(win_input);
            }
        } else if (ch >= 32 && ch < 127) {
            buffer[i++] = ch;
            waddch(win_input, ch);
            wrefresh(win_input);
        }
    }
    buffer[i] = '\0';
<<<<<<< HEAD
    curs_set(0);
    delwin(win_input); win_input = NULL;
    touchwin(win_main); touchwin(win_log); touchwin(win_chat); touchwin(win_status);
    wrefresh(win_main); wrefresh(win_log); wrefresh(win_chat); wrefresh(win_status);
}

int get_server_assignment(const char *mgr_ip, int mgr_port, struct sockaddr_in *assigned_srv) {
    int mgr_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in mgr_addr = {.sin_family = AF_INET, .sin_port = htons(mgr_port)};
    inet_pton(AF_INET, mgr_ip, &mgr_addr.sin_addr);
    if (connect(mgr_sock, (struct sockaddr *)&mgr_addr, sizeof(mgr_addr)) != 0) {
        close(mgr_sock); return -1;
    }
    uint8_t type = ((RES_ACTIVATE & 0x1F) << 3) | ((CRUD_READ & 0x03) << 1) | (ACK_REQ & 0x01);
    GlobalHeader h = {.version = PROTOCOL_VERSION, .msg_type = type, .padding = 0, .msg_length = 0};
    send(mgr_sock, &h, sizeof(h), 0);
    if (recv(mgr_sock, &h, sizeof(h), MSG_WAITALL) != sizeof(h)) { close(mgr_sock); return -1; }
    uint32_t len = ntohl(h.msg_length);
    uint8_t buf[BUFFER_SIZE];
    recv(mgr_sock, buf, len, MSG_WAITALL);
    RegisterPayload *rp = (RegisterPayload*)buf;
    assigned_srv->sin_family = AF_INET;
    assigned_srv->sin_addr.s_addr = rp->server_ip;
    assigned_srv->sin_port = htons(42069);
    last_server_id = rp->server_id;
    close(mgr_sock);
    return 0;
}

=======

    curs_set(0);

    delwin(win_input);
    win_input = NULL;

    touchwin(win_main);
    touchwin(win_log);
    touchwin(win_chat);
    touchwin(win_status);
    wrefresh(win_main);
    wrefresh(win_log);
    wrefresh(win_chat);
    wrefresh(win_status);
    pthread_mutex_unlock(&ui_mutex);
}

// Get server assignment from server manager
int get_server_assignment(const char *mgr_ip, int mgr_port, struct sockaddr_in *assigned_srv) {
    int mgr_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in mgr_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(mgr_port)
    };
    inet_pton(AF_INET, mgr_ip, &mgr_addr.sin_addr);

    log_msg("Contacting server manager at %s:%d", mgr_ip, mgr_port);

    if (connect(mgr_sock, (struct sockaddr *)&mgr_addr, sizeof(mgr_addr)) != 0) {
        close(mgr_sock);
        return -1;
    }

    log_msg("Connected to manager, requesting server assignment...");

    // Send activation request
    uint8_t type = ((RES_ACTIVATE & 0x1F) << 3) | ((CRUD_READ & 0x03) << 1) | (ACK_REQ & 0x01);
    GlobalHeader h = {
        .version = PROTOCOL_VERSION,
        .msg_type = type,
        .status = 0,
        .padding = 0x00,
        .msg_length = 0
    };

    if (send(mgr_sock, &h, sizeof(h), 0) <= 0) {
        close(mgr_sock);
        return -1;
    }

    // Receive response
    uint8_t buf[BUFFER_SIZE];
    if (recv(mgr_sock, &h, sizeof(h), MSG_WAITALL) != sizeof(h)) {
        close(mgr_sock);
        return -1;
    }

    uint32_t len = ntohl(h.msg_length);
    if (len != sizeof(RegisterPayload)) {
        close(mgr_sock);
        return -1;
    }

    if (recv(mgr_sock, buf, len, MSG_WAITALL) != len) {
        close(mgr_sock);
        return -1;
    }

    RegisterPayload *rp = (RegisterPayload*)buf;

    // Check if valid server was assigned
    if (rp->server_ip == 0 || rp->server_id == 0) {
        log_msg("ERROR: No active servers available");
        close(mgr_sock);
        return -1;
    }

    // Set up assigned server address
    assigned_srv->sin_family = AF_INET;
    assigned_srv->sin_addr.s_addr = rp->server_ip;
    assigned_srv->sin_port = htons(9000); // Fixed game server port

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &assigned_srv->sin_addr, ip_str, sizeof(ip_str));
    log_msg("Assigned to server: %s:9000 (ID: 0x%02X)", ip_str, rp->server_id);

    close(mgr_sock);
    return 0;
}

//receiver thread
>>>>>>> origin/main
void* receiver_thread(void* arg) {
    GlobalHeader h;
    uint8_t buf[BUFFER_SIZE];
    while (should_run) {
        if (!connected) { usleep(100000); continue; }

        ssize_t n = recv(sock_fd, &h, sizeof(h), MSG_WAITALL);
        if (n == sizeof(h)) {
            uint32_t len = ntohl(h.msg_length);
            if (len > 0 && len < BUFFER_SIZE) {
                recv(sock_fd, buf, len, MSG_WAITALL);
            }

            uint8_t res = (h.msg_type >> 3) & 0x1F;
            uint8_t crud = (h.msg_type >> 1) & 0x03;
            uint8_t ack = h.msg_type & 0x01;

            if (res == RES_ACCOUNT && ack == ACK_RES) {
                if (crud == CRUD_CREATE) {
                    account_create_response_received = 1;
                    account_create_success = (h.status == 0x00);
                    if (account_create_success && len >= 33) {
                        AccountCreatePayload *ap = (AccountCreatePayload*)buf;
                        my_client_id = ap->account_id;
                        log_msg("Account created! ID: %d", my_client_id);
                    }
                }
                else if (crud == CRUD_UPDATE) {
                    AccountUpdatePayload *ap = (AccountUpdatePayload*)buf;
                    if (ap->account_status == 0x01) { // Login
                        login_response_received = 1;
                        login_success = (h.status == 0x00);
                        if (login_success) {
                            my_client_id = ap->account_id;
                        }
                    } else if (ap->account_status == 0x00) { // Logout
                        logout_response_received = 1;
                    }
                }
            } else if (res == RES_MESSAGE && crud == CRUD_CREATE) {
                MessagePayload *mp = (MessagePayload*)buf;
<<<<<<< HEAD
                uint16_t m_len = ntohs(mp->msg_length);
                char message[MAX_MESSAGE_LEN + 1] = {0};
                memcpy(message, buf + sizeof(MessagePayload), m_len);
                chat_msg("[%s]: %s", mp->from_user, message);
            }
        } else if (n <= 0) {
=======
                uint16_t msg_len = ntohs(mp->msg_length);
                char message[MAX_MESSAGE_LEN + 1];

                if (msg_len > MAX_MESSAGE_LEN) msg_len = MAX_MESSAGE_LEN;
                memcpy(message, buf + sizeof(MessagePayload), msg_len);
                message[msg_len] = '\0';

                if (mp->to_user[0] == '\0') {
                    chat_msg("[ALL] %.16s: %s", mp->from_user, message);
                } else {
                    chat_msg("[DM] %.16s: %s", mp->from_user, message);
                }
            }
            else if (res == RES_MESSAGE && ack == ACK_RES) {
                if (h.status == 0x00) {
                    log_msg("Message delivered");
                } else {
                    log_msg("Message failed: user not found");
                }
            }
        }
        else if (n == 0) {
            connected = 0;
            log_msg("Server disconnected");
        }
        else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
>>>>>>> origin/main
            connected = 0;
            log_msg("Disconnected from server.");
        }
        usleep(10000);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
<<<<<<< HEAD
        printf("Usage: %s <manager_ip> <manager_port>\n", argv[0]);
=======
        printf("Usage: %s <Server_Manager_IP> <Server_Manager_Port>\n", argv[0]);
>>>>>>> origin/main
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    initscr(); start_color(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    init_pair(1, COLOR_CYAN, COLOR_BLACK);

<<<<<<< HEAD
    win_main = newwin(LINES/3, COLS/2, 0, 0);
    win_log = newwin(LINES/3, COLS/2, 0, COLS/2);
    win_chat = newwin(LINES/3, COLS, LINES/3, 0);
    win_status = newwin(4, COLS, (LINES/3)*2, 0);
    scrollok(win_log, 1); scrollok(win_chat, 1);
=======
    int half_w = COLS/2;
    int top_h = LINES / 3;
    int chat_h = LINES - top_h - 4;
    int status_h = 4;

    win_main = newwin(top_h, half_w, 0, 0);
    win_log = newwin(top_h, half_w, 0, half_w);
    win_chat = newwin(chat_h, COLS, top_h, 0);
    win_status = newwin(status_h, COLS, top_h + chat_h, 0);
    win_input = NULL;

    scrollok(win_log, 1);
    scrollok(win_chat, 1);

    box(win_main, 0, 0);
    box(win_log, 0, 0);
    box(win_chat, 0, 0);

    mvwprintw(win_log, 0, 2, " Activity Log ");
    mvwprintw(win_chat, 0, 2, " Chat Messages ");

    wrefresh(win_main);
    wrefresh(win_log);
    wrefresh(win_chat);
>>>>>>> origin/main

    pthread_t r_tid;
    pthread_create(&r_tid, NULL, receiver_thread, NULL);

    int assignment_retry = 0;

    while (should_run) {
        if (!connected) {
<<<<<<< HEAD
            if (get_server_assignment(argv[1], atoi(argv[2]), &srv_addr) == 0) {
                sock_fd = socket(AF_INET, SOCK_STREAM, 0);
                int flag = 1;
                setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
                if (connect(sock_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) == 0) {
                    connected = 1;
                    log_msg("Connected to server.");
                }
            }
            sleep(2); continue;
=======

            if (get_server_assignment(argv[1], atoi(argv[2]), &srv_addr) != 0) {
                assignment_retry++;
                log_msg("Failed to get server assignment (attempt %d), retrying in 5s...", assignment_retry);
                sleep(5);
                continue;
            }

            assignment_retry = 0;
            update_status("Connecting to game server...");

            if (sock_fd >= 0) close(sock_fd);
            sock_fd = socket(AF_INET, SOCK_STREAM, 0);

            if (connect(sock_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) == 0) {
                connected = 1;
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &srv_addr.sin_addr, ip_str, sizeof(ip_str));
                log_msg("Connected to game server at %s:9000", ip_str);
            } else {
                log_msg("Failed to connect to game server, retrying...");
                sleep(2);
                continue;
            }
>>>>>>> origin/main
        }

        pthread_mutex_lock(&ui_mutex);
        werase(win_main); box(win_main, 0, 0);
        mvwprintw(win_main, 1, 2, "=== MENU ===");
        mvwprintw(win_main, 2, 2, "2. Create Account");
        mvwprintw(win_main, 3, 2, "3. Login");
        mvwprintw(win_main, 4, 2, "4. Logout");
        mvwprintw(win_main, 5, 2, "5. Send Message");
        mvwprintw(win_main, 9, 2, "9. Exit");
        wrefresh(win_main);
        pthread_mutex_unlock(&ui_mutex);

        timeout(500);
        int ch = getch();

        if (ch == '2') {
            AccountCreatePayload acc;
            memset(&acc, 0, sizeof(acc));
            get_text_input("Username:", acc.username, 15);
            get_text_input("Password:", acc.password, 15);

            account_create_response_received = 0;
            if (send_msg(RES_ACCOUNT, CRUD_CREATE, ACK_REQ, 0x00, &acc, sizeof(acc)) == 0) {
                int timeout = 200;
                while (!account_create_response_received && timeout-- > 0 && connected) usleep(100000);
            }
        }

<<<<<<< HEAD
        if (ch == '3' && !logged_in) {
            AccountUpdatePayload login;
            memset(&login, 0, sizeof(login));
            char user[16] = {0};
            get_text_input("Username:", user, 15);
            get_text_input("Password:", login.password, 15);
            login.account_status = 0x01; // Online

            login_response_received = 0;
            if (send_msg(RES_ACCOUNT, CRUD_UPDATE, ACK_REQ, 0x00, &login, sizeof(login)) == 0) {
                int timeout = 200;
                while (!login_response_received && timeout-- > 0 && connected) usleep(100000);
                if (login_success) {
                    logged_in = 1; strncpy(my_username, user, 15);
                    log_msg("Logged in!");
                }
            }
        }

        if (ch == '4' && logged_in) {
            AccountUpdatePayload logout;
            memset(&logout, 0, sizeof(logout));
            logout.account_id = my_client_id;
            logout.account_status = 0x00; // Offline
            send_msg(RES_ACCOUNT, CRUD_UPDATE, ACK_REQ, 0x00, &logout, sizeof(logout));
            logged_in = 0;
=======
        if (ch == '3') {
            AccountPayload acc = {0};
            get_text_input("Username:", acc.username, 15);
            get_text_input("Password:", acc.password, 15);
            acc.status = 0x01;

            login_response_received = 0;
            login_success = 0;
            send_msg(RES_ACCOUNT, CRUD_UPDATE, ACK_REQ, 0, &acc, sizeof(acc));

            int timeout_count = 0;
            while (!login_response_received && timeout_count < 50) {
                usleep(100000);
                timeout_count++;
            }

            if (login_response_received && login_success) {
                strncpy(my_username, acc.username, 15);
                my_username[15] = '\0';
                logged_in = 1;
                update_status(NULL);
            } else if (login_response_received && !login_success) {
                log_msg("Login failed - invalid username or password");
            } else {
                log_msg("Login timeout - no response from server");
            }
        }

        if (ch == '4') {
            if (!logged_in) {
                log_msg("You are not logged in!");
                continue;
            }

            AccountPayload acc = {0};
            strncpy(acc.username, my_username, 15);
            acc.status = 0x00;

            logged_in = 0;
            memset(my_username, 0, 16);
            update_status(NULL);

            send_msg(RES_ACCOUNT, CRUD_UPDATE, ACK_REQ, 0, &acc, sizeof(acc));
        }

        if (ch == '5') {
            if (!logged_in) {
                log_msg("Please login first!");
                continue;
            }

            char message[MAX_MESSAGE_LEN];
            get_text_input("Broadcast Message:", message, MAX_MESSAGE_LEN - 1);

            if (strlen(message) > 0) {
                uint8_t buffer[BUFFER_SIZE];
                MessagePayload *mp = (MessagePayload*)buffer;
                strncpy(mp->from_user, my_username, 15);
                mp->from_user[15] = '\0';
                memset(mp->to_user, 0, 16);

                uint16_t msg_len = strlen(message);
                mp->msg_length = htons(msg_len);
                memcpy(buffer + sizeof(MessagePayload), message, msg_len);

                send_msg(RES_MESSAGE, CRUD_CREATE, ACK_REQ, 0, buffer,
                        sizeof(MessagePayload) + msg_len);

                chat_msg("[ME->ALL] %s", message);
            }
        }

        if (ch == '6') {
            if (!logged_in) {
                log_msg("Please login first!");
                continue;
            }

            char to_user[16];
            char message[MAX_MESSAGE_LEN];

            get_text_input("To User:", to_user, 15);
            get_text_input("Message:", message, MAX_MESSAGE_LEN - 1);

            if (strlen(message) > 0 && strlen(to_user) > 0) {
                uint8_t buffer[BUFFER_SIZE];
                MessagePayload *mp = (MessagePayload*)buffer;
                strncpy(mp->from_user, my_username, 15);
                mp->from_user[15] = '\0';
                strncpy(mp->to_user, to_user, 15);
                mp->to_user[15] = '\0';

                uint16_t msg_len = strlen(message);
                mp->msg_length = htons(msg_len);
                memcpy(buffer + sizeof(MessagePayload), message, msg_len);

                send_msg(RES_MESSAGE, CRUD_CREATE, ACK_REQ, 0, buffer,
                        sizeof(MessagePayload) + msg_len);

                chat_msg("[ME->%.16s] %s", to_user, message);
            }
>>>>>>> origin/main
        }

        if (ch == '5' && logged_in) {
            char to[16] = {0}, msg[MAX_MESSAGE_LEN] = {0};
            get_text_input("To:", to, 15);
            get_text_input("Message:", msg, MAX_MESSAGE_LEN-1);

            MessagePayload mp;
            memset(&mp, 0, sizeof(mp));
            strncpy(mp.from_user, my_username, 15);
            strncpy(mp.to_user, to, 15);
            mp.msg_length = htons(strlen(msg));

            uint8_t full[sizeof(MessagePayload) + MAX_MESSAGE_LEN];
            memcpy(full, &mp, sizeof(mp));
            memcpy(full + sizeof(mp), msg, strlen(msg));
            send_msg(RES_MESSAGE, CRUD_CREATE, ACK_REQ, 0x00, full, sizeof(mp) + strlen(msg));
        }

<<<<<<< HEAD
        if (ch == '9') should_run = 0;
=======
>>>>>>> origin/main
        update_status(NULL);
    }

    pthread_join(r_tid, NULL);
    endwin();
    return 0;
}
