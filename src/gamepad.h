#ifndef IDLERD_GAMEPAD_H
#define IDLERD_GAMEPAD_H

#include <stdbool.h>
#include <time.h>
#include "constants.h"

typedef struct {
    int fd;
    int device_num;
    char name[64];
} gamepad_t;

extern gamepad_t gamepads[MAX_GAMEPADS];
extern int gamepad_count;
extern time_t last_gamepad_activity;
extern int inotify_fd;

void gamepad_scan(void);
void gamepad_remove(int index);
void gamepad_read_events(int index);
void gamepad_init_inotify(void);
void gamepad_handle_inotify(void);
void gamepad_cleanup(void);

#endif
