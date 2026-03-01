//
// Created by Jin Siang Teh on 2026-03-01.
//

#ifndef COMP4985_UI_H
#define COMP4985_UI_H


#include <ncurses.h>
#include <pthread.h>

// ---------------------------------------------------------------------------
// UI globals — defined in ui.c
// ---------------------------------------------------------------------------
extern WINDOW *win_srv;
extern WINDOW *win_cli;
extern WINDOW *win_mgr;
extern pthread_mutex_t ui_mutex;

// ---------------------------------------------------------------------------
// Logging functions — defined in ui.c
// ---------------------------------------------------------------------------
void w_log_core(WINDOW *win, const char *msg);
void server_log(const char *fmt, ...);
void manager_log(const char *fmt, ...);
void client_log(const char *fmt, ...);



#endif //COMP4985_UI_H