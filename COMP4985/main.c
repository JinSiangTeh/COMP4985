#include "protocol.h"
#include "ui.h"
#include "manager.h"
#include "client.h"

#include <ncurses.h>

// ===========================================================================
// Helpers
// ===========================================================================

uint32_t get_my_ip(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(53)
    };
    inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);
    connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(sock, (struct sockaddr *)&local, &len);
    close(sock);
    return local.sin_addr.s_addr;
}

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: %s <Port> <Mgr_IP> <Mgr_Port>\n", argv[0]);
        return 1;
    }

    my_server_ip = get_my_ip();

    initscr(); start_color(); cbreak(); noecho(); curs_set(0);
    init_pair(1, COLOR_CYAN, COLOR_BLACK);

    int cols = COLS / 3;
    int rows = LINES - 2;
    win_srv = newwin(rows, cols - 1, 1, 0);
    win_cli = newwin(rows, cols - 1, 1, cols);
    win_mgr = newwin(rows, cols - 1, 1, cols * 2);
    scrollok(win_srv, TRUE);
    scrollok(win_cli, TRUE);
    scrollok(win_mgr, TRUE);

    attron(A_REVERSE);
    mvprintw(0, 1,            " SERVER  ");
    mvprintw(0, cols + 1,     " CLIENTS ");
    mvprintw(0, cols * 2 + 1, " MANAGER ");
    attroff(A_REVERSE);
    refresh();

    ManagerInfo *info = malloc(sizeof(ManagerInfo));
    strncpy(info->ip, argv[2], sizeof(info->ip) - 1);
    info->port    = atoi(argv[3]);
    info->my_port = atoi(argv[1]);

    pthread_t mgr_tid;
    pthread_create(&mgr_tid, NULL, manager_connection_thread, info);

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in saddr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(info->my_port),
        .sin_addr.s_addr = INADDR_ANY
    };
    bind(srv_fd, (struct sockaddr *)&saddr, sizeof(saddr));
    listen(srv_fd, 10);

    server_log("Server online — port %d  (Protocol v0.2)", info->my_port);

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int csock = accept(srv_fd, (struct sockaddr *)&caddr, &clen);
        if (csock >= 0) {
            int *p = malloc(sizeof(int));
            *p = csock;
            pthread_t t;
            pthread_create(&t, NULL, handle_client, p);
            pthread_detach(t);
        }
    }

    endwin();
    return 0;
}