#include "gamepad.h"
#include "idle.h"
#include "inhibit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <sys/inotify.h>

gamepad_t gamepads[MAX_GAMEPADS];
int gamepad_count = 0;
time_t last_gamepad_activity = 0;
int inotify_fd = -1;

static bool is_gamepad_fd(int fd) {
    uint8_t key_bitmap[128];
    memset(key_bitmap, 0, sizeof(key_bitmap));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bitmap)), key_bitmap) < 0)
        return false;

    #define KEYBIT(bit) ((key_bitmap)[(bit) / 8] & (1 << ((bit) % 8)))
    bool result = KEYBIT(BTN_GAMEPAD) || KEYBIT(BTN_JOYSTICK);
    #undef KEYBIT
    return result;
}

void gamepad_remove(int index) {
    close(gamepads[index].fd);
    printf("[gamepad] removed: %s (event%d)\n",
           gamepads[index].name, gamepads[index].device_num);
    for (int i = index; i < gamepad_count - 1; i++)
        gamepads[i] = gamepads[i + 1];
    gamepad_count--;
}

void gamepad_scan(void) {
    DIR *dir = opendir(DEV_INPUT_PATH);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        int dev_num = atoi(entry->d_name + 5);

        bool already_open = false;
        for (int i = 0; i < gamepad_count; i++) {
            if (gamepads[i].device_num == dev_num) {
                already_open = true;
                break;
            }
        }
        if (already_open) continue;

        char path[256];
        snprintf(path, sizeof(path), "%s/%s", DEV_INPUT_PATH, entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        if (!is_gamepad_fd(fd)) {
            close(fd);
            continue;
        }

        char name[64] = "Unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        if (gamepad_count < MAX_GAMEPADS) {
            gamepads[gamepad_count].fd = fd;
            gamepads[gamepad_count].device_num = dev_num;
            strncpy(gamepads[gamepad_count].name, name,
                    sizeof(gamepads[0].name) - 1);
            gamepads[gamepad_count].name[sizeof(gamepads[0].name) - 1] = '\0';
            gamepad_count++;
            printf("[gamepad] found: %s (%s)\n", path, name);
        } else {
            fprintf(stderr, "[warn] max gamepads reached, ignoring %s\n", path);
            close(fd);
        }
    }
    closedir(dir);
}

void gamepad_read_events(int index) {
    struct input_event ev;
    while (read(gamepads[index].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        last_gamepad_activity = time(NULL);
    }

    inhibit_set(true);

    if (is_idle) {
        printf("[gamepad] activity while idle, waking up\n");
        idle_reregister_notifications();
    }
}

void gamepad_init_inotify(void) {
    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd >= 0)
        inotify_add_watch(inotify_fd, DEV_INPUT_PATH, IN_CREATE | IN_DELETE);
}

void gamepad_handle_inotify(void) {
    char buf[4096];
    ssize_t len;
    while ((len = read(inotify_fd, buf, sizeof(buf))) > 0) {
        (void)len;
    }

    for (int i = gamepad_count - 1; i >= 0; i--) {
        char path[256];
        snprintf(path, sizeof(path), DEV_INPUT_CLASS, gamepads[i].device_num);
        struct stat st;
        if (stat(path, &st) != 0)
            gamepad_remove(i);
    }

    usleep(50000);
    gamepad_scan();
}

void gamepad_cleanup(void) {
    for (int i = 0; i < gamepad_count; i++)
        close(gamepads[i].fd);
    if (inotify_fd >= 0)
        close(inotify_fd);
}
