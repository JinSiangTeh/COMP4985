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

#define MAX_CLIENTS 100
#define BUFFER_SIZE 65536
#define MAX_LOG_SIZE 65535

// Protocol Version
#define PROTOCOL_MAJOR 0
#define PROTOCOL_MINOR 1

// Resource Types (5 bits)
#define RESOURCE_SERVER   0x00
#define RESOURCE_ACTIVATE 0x01
#define RESOURCE_ACCOUNT  0x02
#define RESOURCE_LOG      0x03

// CRUD Operations (2 bits)
#define CRUD_CREATE  0x00
#define CRUD_READ    0x01
#define CRUD_UPDATE  0x02
#define CRUD_DELETE  0x03

// ACK bit
#define ACK_REQUEST  0x00
#define ACK_RESPONSE 0x01

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
    uint32_t server_ip;     // 4 bytes - server IP
    uint8_t  server_id;     // 1 byte - server ID
} RegisterPayload;

typedef struct __attribute__((packed)) {
    uint32_t server_ip;     // 4 bytes - server IP
    uint8_t  server_id;     // 1 byte - server ID
} ActivatePayload;

typedef struct __attribute__((packed)) {
    char     username[16];  // 16 bytes - username
    char     password[16];  // 16 bytes - password
    uint8_t  client_id;     // 1 byte - client ID
    uint8_t  status;        // 1 byte - status
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

pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t manager_mutex = PTHREAD_MUTEX_INITIALIZER;
int client_sockets[MAX_CLIENTS] = {0};


int manager_connected = 0;
int manager_socket = -1;
char manager_ip[20];
int manager_port;
uint8_t my_server_id = 0x00;
uint32_t my_server_ip = 0;

// Helper: Create version byte
uint8_t make_version(uint8_t major, uint8_t minor) {
    return ((major & 0x0F) << 4) | (minor & 0x0F);
}

// Helper: Create message type byte
uint8_t make_msg_type(uint8_t resource, uint8_t crud, uint8_t ack) {
    return ((resource & 0x1F) << 3) | ((crud & 0x03) << 1) | (ack & 0x01);
}

// Helper: Parse message type
void parse_msg_type(uint8_t msg_type, uint8_t *resource, uint8_t *crud, uint8_t *ack) {
    *resource = (msg_type >> 3) & 0x1F;
    *crud = (msg_type >> 1) & 0x03;
    *ack = msg_type & 0x01;
}

// Get timestamp string
void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
}

// Binary protocol send function
int send_binary_msg(int sock, uint8_t resource, uint8_t crud, uint8_t ack,
                    uint8_t status, const void *payload, uint32_t payload_len) {
    GlobalHeader header;
    header.version = make_version(PROTOCOL_MAJOR, PROTOCOL_MINOR);
    header.msg_type = make_msg_type(resource, crud, ack);
    header.status = status;
    header.padding = 0x00;
    header.msg_length = htonl(payload_len);

    // Send 8-byte header
    ssize_t sent = send(sock, &header, sizeof(GlobalHeader), MSG_NOSIGNAL);
    if (sent != sizeof(GlobalHeader)) {
        return -1;
    }

    // Send payload if exists
    if (payload_len > 0 && payload != NULL) {
        sent = send(sock, payload, payload_len, MSG_NOSIGNAL);
        if (sent != payload_len) {
            return -1;
        }
    }

    return 0;
}

// Binary protocol receive function
int recv_binary_msg(int sock, GlobalHeader *header, void *payload, uint32_t max_payload_len) {
    // Receive exactly 8 bytes for header
    ssize_t received = recv(sock, header, sizeof(GlobalHeader), MSG_WAITALL);
    if (received != sizeof(GlobalHeader)) {
        return -1;
    }

    // Convert message length from network byte order
    uint32_t payload_len = ntohl(header->msg_length);

    // Validate payload length
    if (payload_len > max_payload_len) {
        return -2;  // Payload too large
    }

    // Receive payload if present
    if (payload_len > 0 && payload != NULL) {
        received = recv(sock, payload, payload_len, MSG_WAITALL);
        if (received != payload_len) {
            return -1;
        }
    }

    return payload_len;
}

// Safe logging with timestamp
void safe_log(const char *msg) {
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    pthread_mutex_lock(&log_mutex);
    printf("[%s] %s\n", timestamp, msg);
    fflush(stdout);
    pthread_mutex_unlock(&log_mutex);
}

// Send log to manager
void send_log_to_manager(const char *log_msg) {
    pthread_mutex_lock(&manager_mutex);

    if (manager_connected && manager_socket >= 0) {
        uint16_t log_len = strlen(log_msg);

        // uint16_t max is 65535, so truncate if needed
        if (strlen(log_msg) > MAX_LOG_SIZE) {
            log_len = MAX_LOG_SIZE;
        }

        // Build log payload
        uint8_t buffer[3 + MAX_LOG_SIZE];
        LogPayload *log_payload = (LogPayload*)buffer;
        log_payload->server_id = my_server_id;
        log_payload->log_length = htons(log_len);
        memcpy(buffer + 3, log_msg, log_len);

        uint32_t total_len = 3 + log_len;

        if (send_binary_msg(manager_socket, RESOURCE_LOG, CRUD_CREATE, ACK_REQUEST,
                           0x00, buffer, total_len) < 0) {
            manager_connected = 0;
            close(manager_socket);
            manager_socket = -1;
        }
    }

    pthread_mutex_unlock(&manager_mutex);
}

// Enhanced logging that also sends to manager
void log_and_forward(const char *msg) {
    safe_log(msg);
    send_log_to_manager(msg);
}

// Print header details
void log_header_info(GlobalHeader *header) {
    uint8_t resource, crud, ack;
    parse_msg_type(header->msg_type, &resource, &crud, &ack);

    char log_msg[256];
    sprintf(log_msg, "[HEADER] Ver:0x%02X Type:0x%02X (Res:%d CRUD:%d ACK:%d) Status:0x%02X Len:%u",
            header->version, header->msg_type, resource, crud, ack,
            header->status, ntohl(header->msg_length));
    safe_log(log_msg);
}

// MANAGER CONNECTION MAINTAINER
void* manager_connection_thread(void* arg) {
    ManagerInfo* info = (ManagerInfo*)arg;

    strcpy(manager_ip, info->ip);
    manager_port = info->port;

    sleep(1);

    safe_log(">>> Attempting to connect to Manager...");

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

    // Get my own IP for registration
    struct sockaddr_in my_addr;
    socklen_t addr_len = sizeof(my_addr);
    getsockname(sock, (struct sockaddr*)&my_addr, &addr_len);
    my_server_ip = my_addr.sin_addr.s_addr;

    safe_log(">>> IP address converted, attempting connection...");

    if (connect(sock, (struct sockaddr *)&mgr_addr, sizeof(mgr_addr)) == 0) {
        safe_log(">>> Connected to Manager!");

        // Send registration message
        RegisterPayload reg_payload;

        // Get local IP address
        struct sockaddr_in local_addr;
        socklen_t local_len = sizeof(local_addr);
        if (getsockname(sock, (struct sockaddr*)&local_addr, &local_len) == 0) {
            reg_payload.server_ip = local_addr.sin_addr.s_addr;
        } else {
            reg_payload.server_ip = inet_addr("127.0.0.1");
        }

        reg_payload.server_id = my_server_id;

        safe_log(">>> Sending registration (Protocol v0.1, 8-byte header)...");

        if (send_binary_msg(sock, RESOURCE_SERVER, CRUD_CREATE, ACK_REQUEST,
                           0x00, &reg_payload, sizeof(RegisterPayload)) == 0) {
            char success_msg[128];
            sprintf(success_msg, "[SUCCESS] Sent REGISTER (Resource:0x%02X CRUD:0x%02X ACK:0x%02X)",
                    RESOURCE_SERVER, CRUD_CREATE, ACK_REQUEST);
            safe_log(success_msg);
        } else {
            safe_log("[ERROR] Failed to send registration");
        }

        pthread_mutex_lock(&manager_mutex);
        manager_socket = sock;
        manager_connected = 1;
        pthread_mutex_unlock(&manager_mutex);

        safe_log(">>> Manager connection established and active");

        // Receive loop
        GlobalHeader header;
        uint8_t buffer[BUFFER_SIZE];
        int result;

        while ((result = recv_binary_msg(sock, &header, buffer, BUFFER_SIZE)) >= 0) {
            log_header_info(&header);

            uint8_t resource, crud, ack;
            parse_msg_type(header.msg_type, &resource, &crud, &ack);

            // Handle registration ACK
            if (resource == RESOURCE_SERVER && crud == CRUD_CREATE && ack == ACK_RESPONSE) {
                RegisterPayload *reg_ack = (RegisterPayload*)buffer;
                my_server_id = reg_ack->server_id;

                char ack_msg[128];
                sprintf(ack_msg, "[MANAGER ACK] Registered! Assigned Server ID: 0x%02X",
                        my_server_id);
                safe_log(ack_msg);
            }
            // Handle activate server command
            else if (resource == RESOURCE_ACTIVATE && crud == CRUD_CREATE && ack == ACK_REQUEST) {
                ActivatePayload *activate = (ActivatePayload*)buffer;
                safe_log("[MANAGER CMD] Received ACTIVATE command");

                // Send ACK back
                send_binary_msg(sock, RESOURCE_ACTIVATE, CRUD_CREATE, ACK_RESPONSE,
                               0x00, activate, sizeof(ActivatePayload));
                safe_log("[RESPONSE] Sent ACTIVATE ACK");
            }
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

// Client management
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

// Handle client
void* handle_client(void* arg) {
    int sock = *(int*)arg;

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(sock, (struct sockaddr*)&addr, &addr_len);

    char connect_log[128];
    sprintf(connect_log, "[CLIENT CONNECT] %s:%d",
            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    log_and_forward(connect_log);

    manage_clients(sock, 1);

    GlobalHeader header;
    uint8_t buffer[BUFFER_SIZE];
    int result;

    while((result = recv_binary_msg(sock, &header, buffer, BUFFER_SIZE)) >= 0) {
        uint8_t resource, crud, ack;
        parse_msg_type(header.msg_type, &resource, &crud, &ack);

        char log_buf[256];
        sprintf(log_buf, "[CLIENT] %s:%d | Res:%d CRUD:%d ACK:%d Len:%d",
                inet_ntoa(addr.sin_addr), ntohs(addr.sin_port),
                resource, crud, ack, result);
        log_and_forward(log_buf);

        // Handle account creation
        if (resource == RESOURCE_ACCOUNT && crud == CRUD_CREATE) {
            AccountPayload *account = (AccountPayload*)buffer;
            sprintf(log_buf, "[ACCOUNT CREATE] User: %.16s", account->username);
            log_and_forward(log_buf);

            // Send ACK
            send_binary_msg(sock, RESOURCE_ACCOUNT, CRUD_CREATE, ACK_RESPONSE,
                           0x00, account, sizeof(AccountPayload));
        }
        // Handle login
        else if (resource == RESOURCE_ACCOUNT && crud == CRUD_UPDATE &&
                 ((AccountPayload*)buffer)->status == 0x01) {
            AccountPayload *account = (AccountPayload*)buffer;
            sprintf(log_buf, "[LOGIN] User: %.16s ID: 0x%02X",
                    account->username, account->client_id);
            log_and_forward(log_buf);

            // Send ACK
            send_binary_msg(sock, RESOURCE_ACCOUNT, CRUD_UPDATE, ACK_RESPONSE,
                           0x00, account, sizeof(AccountPayload));
        }
        // Handle logout
        else if (resource == RESOURCE_ACCOUNT && crud == CRUD_UPDATE &&
                 ((AccountPayload*)buffer)->status == 0x00) {
            AccountPayload *account = (AccountPayload*)buffer;
            sprintf(log_buf, "[LOGOUT] User: %.16s ID: 0x%02X",
                    account->username, account->client_id);
            log_and_forward(log_buf);
            break;
        }
    }

    sprintf(connect_log, "[CLIENT DISCONNECT] %s:%d",
            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    log_and_forward(connect_log);

    manage_clients(sock, 0);
    close(sock);
    free(arg);
    return NULL;
}

// Main function
int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: %s <My_Port> <Manager_IP> <Manager_Port>\n", argv[0]);
        printf("Example: %s 8080 127.0.0.1 9000\n", argv[0]);
        printf("\nProtocol v0.1 (8-byte header):\n");
        printf("  Byte 0: Version (4-bit major + 4-bit minor)\n");
        printf("  Byte 1: Message Type (5-bit resource + 2-bit CRUD + 1-bit ACK)\n");
        printf("  Byte 2: Status\n");
        printf("  Byte 3: Padding\n");
        printf("  Bytes 4-7: Message Length (network byte order)\n");
        return 1;
    }

    int my_port = atoi(argv[1]);

    printf("===========================================\n");
    printf("===  BINARY CHAT SERVER v0.1            ===\n");
    printf("===  Protocol: 8-byte header             ===\n");
    printf("===========================================\n");

    char startup_msg[128];
    sprintf(startup_msg, "Server Port: %d", my_port);
    safe_log(startup_msg);
    sprintf(startup_msg, "Manager: %s:%s", argv[2], argv[3]);
    safe_log(startup_msg);
    sprintf(startup_msg, "Protocol Version: %d.%d", PROTOCOL_MAJOR, PROTOCOL_MINOR);
    safe_log(startup_msg);
    sprintf(startup_msg, "Header Size: %zu bytes", sizeof(GlobalHeader));
    safe_log(startup_msg);

    ManagerInfo *info = malloc(sizeof(ManagerInfo));
    strcpy(info->ip, argv[2]);
    info->port = atoi(argv[3]);
    info->my_port = my_port;

    pthread_t mgr_tid;
    pthread_create(&mgr_tid, NULL, manager_connection_thread, (void*)info);
    pthread_detach(mgr_tid);

    safe_log("===========================================");
    safe_log(">>> Setting up server socket...");

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        safe_log("[ERROR] Failed to create socket");
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
        perror("Bind failed");
        return 1;
    }
    safe_log(">>> Socket bound successfully");

    if (listen(srv_fd, 10) < 0) {
        safe_log("[ERROR] Listen failed");
        perror("Listen failed");
        return 1;
    }
    safe_log(">>> Socket listening");

    int flags = fcntl(srv_fd, F_GETFL, 0);
    fcntl(srv_fd, F_SETFL, flags | O_NONBLOCK);

    safe_log("===========================================");
    sprintf(startup_msg, "BINARY SERVER ONLINE on port %d", my_port);
    safe_log(startup_msg);
    safe_log("Waiting for client connections...");
    safe_log("===========================================");

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

        usleep(100000);
    }

    return 0;
}
