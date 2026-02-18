#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <fcntl.h>

#define PROTOCOL_VERSION 0x01
#define RES_ACCOUNT  0x02
#define CRUD_CREATE  0x00
#define ACK_REQ      0x00

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

void dump_hex(const char *label, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    printf("%s (%zu bytes):\n", label, len);
    for (size_t i = 0; i < len; i++) {
        printf("%02x ", p[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (len % 16 != 0) printf("\n");
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <server_ip> <server_port>\n", argv[0]);
        return 1;
    }

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    // Set TCP_NODELAY
    int flag = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));

    struct sockaddr_in srv_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(atoi(argv[2]))
    };
    inet_pton(AF_INET, argv[1], &srv_addr.sin_addr);

    printf("Connecting to %s:%s...\n", argv[1], argv[2]);
    if (connect(sock_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) != 0) {
        perror("connect");
        close(sock_fd);
        return 1;
    }
    printf("Connected!\n\n");

    // Wait a moment
    usleep(500000);

    // Check if server sends anything first
    printf("Checking if server sends welcome message...\n");
    fcntl(sock_fd, F_SETFL, O_NONBLOCK);
    uint8_t welcome_buf[1024];
    ssize_t n = recv(sock_fd, welcome_buf, sizeof(welcome_buf), 0);
    if (n > 0) {
        dump_hex("SERVER WELCOME MESSAGE", welcome_buf, n);
    } else {
        printf("No welcome message from server\n");
    }
    fcntl(sock_fd, F_SETFL, fcntl(sock_fd, F_GETFL) & ~O_NONBLOCK);

    // Prepare account creation packet
    printf("\nPreparing account creation packet...\n");

    AccountPayload acc;
    memset(&acc, 0, sizeof(AccountPayload));
    strcpy(acc.username, "testuser");
    strcpy(acc.password, "testpass");
    acc.client_id = 0x00;
    acc.status = 0x01;

    uint8_t type = ((RES_ACCOUNT & 0x1F) << 3) | ((CRUD_CREATE & 0x03) << 1) | (ACK_REQ & 0x01);
    GlobalHeader h = {
        .version = PROTOCOL_VERSION,
        .msg_type = type,
        .status = 0x00,
        .padding = 0x00,
        .msg_length = htonl(sizeof(AccountPayload))
    };

    printf("Message type calculation:\n");
    printf("  RES_ACCOUNT=%d (0x%02x) CRUD_CREATE=%d ACK_REQ=%d\n",
           RES_ACCOUNT, RES_ACCOUNT, CRUD_CREATE, ACK_REQ);
    printf("  Encoded type byte: 0x%02x = %08b\n", type, type);
    printf("  Resource bits (7-3): %05b = %d\n", (type >> 3) & 0x1F, (type >> 3) & 0x1F);
    printf("  CRUD bits (2-1): %02b = %d\n", (type >> 1) & 0x03, (type >> 1) & 0x03);
    printf("  ACK bit (0): %01b = %d\n\n", type & 0x01, type & 0x01);

    // Create full packet
    uint8_t packet[sizeof(GlobalHeader) + sizeof(AccountPayload)];
    memcpy(packet, &h, sizeof(GlobalHeader));
    memcpy(packet + sizeof(GlobalHeader), &acc, sizeof(AccountPayload));

    dump_hex("FULL PACKET TO SEND", packet, sizeof(packet));

    printf("\nSending packet...\n");
    ssize_t sent = send(sock_fd, packet, sizeof(packet), 0);
    if (sent < 0) {
        perror("send");
        close(sock_fd);
        return 1;
    }
    printf("Sent %zd bytes\n\n", sent);

    // Wait for response
    printf("Waiting for response (5 seconds)...\n");
    fcntl(sock_fd, F_SETFL, fcntl(sock_fd, F_GETFL) & ~O_NONBLOCK);

    uint8_t response[1024];
    n = recv(sock_fd, response, sizeof(response), 0);
    if (n > 0) {
        dump_hex("SERVER RESPONSE", response, n);
    } else if (n == 0) {
        printf("Server closed connection\n");
    } else {
        perror("recv");
    }

    close(sock_fd);
    return 0;
}