#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_CLIENTS 100
#define BUFFER_SIZE 1024
#define MAX_LOGS 1000


typedef struct {
    char ip[20];
    int port;
    int my_port;
} ManagerInfo;


WINDOW *log_win, *input_win;
pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t manager_mutex = PTHREAD_MUTEX_INITIALIZER;
int client_sockets[MAX_CLIENTS] = {0};

// Manager connection info
int manager_connected = 0;
int manager_socket = -1;
char manager_ip[20];
int manager_port;

// Log history
char log_history[MAX_LOGS][256];
int log_count = 0;

//UI
void init_interface() {
    initscr();
    cbreak();
    noecho();
    curs_set(0);

    log_win = newwin(LINES - 3, COLS, 0, 0);
    input_win = newwin(3, COLS, LINES - 3, 0);

    scrollok(log_win, TRUE);
    box(log_win, 0, 0);
    mvwprintw(log_win, 0, 2, " Server Logs ");

    box(input_win, 0, 0);
    mvwprintw(input_win, 0, 2, " Console Input ");

    wrefresh(log_win);
    wrefresh(input_win);
}

// --- 2. REDRAW ALL LOGS ---
void redraw_logs() {
    pthread_mutex_lock(&ui_mutex);

    wclear(log_win);
    box(log_win, 0, 0);
    mvwprintw(log_win, 0, 2, " Server Logs ");

    // Display all logs from history
    int start = (log_count > LINES - 5) ? (log_count - (LINES - 5)) : 0;
    for (int i = start; i < log_count; i++) {
        wprintw(log_win, "  %s\n", log_history[i]);
    }

    wrefresh(log_win);
    pthread_mutex_unlock(&ui_mutex);
}

//Send message to server manager
void send_to_manager(const char *msg) {
    pthread_mutex_lock(&manager_mutex);

    if (manager_connected && manager_socket >= 0) {
        char formatted_msg[512];
        snprintf(formatted_msg, sizeof(formatted_msg), "LOG: %s\n", msg);

        ssize_t sent = send(manager_socket, formatted_msg, strlen(formatted_msg), MSG_NOSIGNAL);
        if (sent < 0) {
            manager_connected = 0;
            close(manager_socket);
            manager_socket = -1;
        }
    }

    pthread_mutex_unlock(&manager_mutex);
}

//history log
void safe_log(const char *msg) {
    if (log_count < MAX_LOGS) {
        strncpy(log_history[log_count], msg, 255);
        log_history[log_count][255] = '\0';
        log_count++;
    } else {

        for (int i = 0; i < MAX_LOGS - 1; i++) {
            strcpy(log_history[i], log_history[i + 1]);
        }
        strncpy(log_history[MAX_LOGS - 1], msg, 255);
        log_history[MAX_LOGS - 1][255] = '\0';
    }


    redraw_logs();


    send_to_manager(msg);
}

// --- 5. MANAGER CONNECTION MAINTAINER ---
void* manager_connection_thread(void* arg) {
    ManagerInfo* info = (ManagerInfo*)arg;

    strcpy(manager_ip, info->ip);
    manager_port = info->port;
    int my_port = info->my_port;

    sleep(1);

    safe_log(">>> Attempting to connect to Manager...");

    // Try to connect
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        safe_log("[ERROR] Socket creation failed");
        free(info);
        return NULL;
    }

    safe_log(">>> Socket created");

    struct sockaddr_in mgr_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(info->port)
    };

    if (inet_pton(AF_INET, info->ip, &mgr_addr.sin_addr) <= 0) {
        safe_log("[ERROR] Invalid manager IP address");
        close(sock);
        free(info);
        return NULL;
    }

    safe_log(">>> IP address converted, attempting connection...");

    if (connect(sock, (struct sockaddr *)&mgr_addr, sizeof(mgr_addr)) == 0) {
        safe_log(">>> Connected to Manager!");

        // Send registration
        char reg_msg[64];
        sprintf(reg_msg, "REGISTER_SERVER %d\n", my_port);

        safe_log(">>> Sending registration message...");

        ssize_t sent = send(sock, reg_msg, strlen(reg_msg), 0);

        if (sent > 0) {
            char success_msg[128];
            sprintf(success_msg, "[SUCCESS] Registered with Manager (%zd bytes sent)", sent);
            safe_log(success_msg);
        } else {
            safe_log("[ERROR] Failed to send registration");
        }


        pthread_mutex_lock(&manager_mutex);
        manager_socket = sock;
        manager_connected = 1;
        pthread_mutex_unlock(&manager_mutex);

        safe_log(">>> Manager connection established and active");


        char buffer[BUFFER_SIZE];
        while (recv(sock, buffer, BUFFER_SIZE, 0) > 0) {
            // Manager might send commands here in the future
        }


        pthread_mutex_lock(&manager_mutex);
        manager_connected = 0;
        manager_socket = -1;
        pthread_mutex_unlock(&manager_mutex);

        safe_log("[WARNING] Manager connection lost");
        close(sock);
    } else {
        char error_msg[256];
        sprintf(error_msg, "[ERROR] Cannot connect to Manager: %s", strerror(errno));
        safe_log(error_msg);
        safe_log("[INFO] Running in standalone mode");
        close(sock);
    }

    free(info);
    return NULL;
}

//Client management
void manage_clients(int sock, int add) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (add && client_sockets[i] == 0) {
            client_sockets[i] = sock;
            break;
        } else if (!add && client_sockets[i] == sock) {
            client_sockets[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void broadcast(const char *msg, int sender_sock) {
    pthread_mutex_lock(&clients_mutex);
    int broadcast_count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] != 0 && client_sockets[i] != sender_sock) {
            send(client_sockets[i], msg, strlen(msg), 0);
            broadcast_count++;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    if (broadcast_count > 0) {
        char log_buf[128];
        sprintf(log_buf, "[BROADCAST] Message sent to %d client(s)", broadcast_count);
        safe_log(log_buf);
    }
}

// handle client
void* handle_client(void* arg) {
    int sock = *(int*)arg;

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(sock, (struct sockaddr*)&addr, &addr_len);

    char connect_log[128];
    sprintf(connect_log, "[CLIENT CONNECT] %s:%d",
            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    safe_log(connect_log);

    manage_clients(sock, 1);

    char buffer[BUFFER_SIZE];
    char log_buf[BUFFER_SIZE + 100];
    int n;

    while((n = recv(sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[n] = '\0';


        if (n > 0 && buffer[n-1] == '\n') buffer[n-1] = '\0';
        if (n > 1 && buffer[n-2] == '\r') buffer[n-2] = '\0';

        sprintf(log_buf, "[CLIENT MESSAGE] %s:%d says: %s",
                inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), buffer);
        safe_log(log_buf);


        char broadcast_msg[BUFFER_SIZE + 50];
        sprintf(broadcast_msg, "%s:%d> %s\n",
                inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), buffer);
        broadcast(broadcast_msg, sock);
    }

    sprintf(connect_log, "[CLIENT DISCONNECT] %s:%d",
            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    safe_log(connect_log);

    manage_clients(sock, 0);
    close(sock);
    free(arg);
    return NULL;
}

//main function
int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: %s <My_Port> <Manager_IP> <Manager_Port>\n", argv[0]);
        printf("Example: %s 8080 127.0.0.1 9000\n", argv[0]);
        return 1;
    }

    int my_port = atoi(argv[1]);


    init_interface();
    safe_log("===========================================");
    safe_log("===     CHAT SERVER STARTING            ===");
    safe_log("===========================================");


    char startup_msg[128];
    sprintf(startup_msg, ">>> Server Port: %d", my_port);
    safe_log(startup_msg);
    sprintf(startup_msg, ">>> Manager: %s:%s", argv[2], argv[3]);
    safe_log(startup_msg);

    ManagerInfo *info = malloc(sizeof(ManagerInfo));
    strcpy(info->ip, argv[2]);
    info->port = atoi(argv[3]);
    info->my_port = my_port;

    pthread_t mgr_tid;
    pthread_create(&mgr_tid, NULL, manager_connection_thread, (void*)info);
    pthread_detach(mgr_tid);

    //Setup Local Listening Socket
    safe_log("===========================================");
    safe_log(">>> Setting up server socket...");

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        safe_log("[ERROR] Failed to create socket");
        endwin();
        return 1;
    }
    safe_log(">>> Socket created successfully");

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(my_port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(srv_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        safe_log("[ERROR] Bind failed");
        endwin();
        perror("Bind failed");
        return 1;
    }
    safe_log(">>> Socket bound successfully");

    if (listen(srv_fd, 10) < 0) {
        safe_log("[ERROR] Listen failed");
        endwin();
        perror("Listen failed");
        return 1;
    }
    safe_log(">>> Socket listening");

    // Make accept non-blocking
    int flags = fcntl(srv_fd, F_GETFL, 0);
    fcntl(srv_fd, F_SETFL, flags | O_NONBLOCK);

    safe_log("===========================================");
    sprintf(startup_msg, "=== SERVER ONLINE on port %d ===", my_port);
    safe_log(startup_msg);
    safe_log("=== Waiting for client connections... ===");
    safe_log("===========================================");

    //Accept Loop
    while(1) {
        struct sockaddr_in c_addr;
        socklen_t c_len = sizeof(c_addr);
        int client_sock = accept(srv_fd, (struct sockaddr *)&c_addr, &c_len);

        if (client_sock >= 0) {
            int *new_sock = malloc(sizeof(int));
            *new_sock = client_sock;
            pthread_t t;
            pthread_create(&t, NULL, handle_client, (void*)new_sock);
            pthread_detach(t);
        }

        // UI refresh
        usleep(100000);
    }

    endwin();
    return 0;
}