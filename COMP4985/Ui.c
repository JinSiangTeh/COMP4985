//
// Created by Jin Siang Teh on 2026-03-01.
//
#include "ui.h"
#include "protocol.h"
#include "manager.h"

#include <stdarg.h>
#include <time.h>
#include <string.h>

// ---------------------------------------------------------------------------
// UI globals
// ---------------------------------------------------------------------------
WINDOW *win_srv, *win_cli, *win_mgr;
pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;

// ===========================================================================
// UI & logging
// ===========================================================================

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

void server_log(const char *fmt, ...) {
    char buf[1024]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    w_log_core(win_srv, buf);
}
void manager_log(const char *fmt, ...) {
    char buf[1024]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    w_log_core(win_mgr, buf);
}
void client_log(const char *fmt, ...) {
    char buf[1024]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    w_log_core(win_cli, buf);
    send_log_to_manager(buf);
}