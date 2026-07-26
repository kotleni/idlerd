#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#include <wayland-client.h>
#include "idle-protocol.h"
#include "idle-inhibit-protocol.h"

#include "constants.h"
#include "command.h"
#include "gamepad.h"
#include "inhibit.h"
#include "idle.h"

static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;

static void registry_global(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface, uint32_t version) {
    (void)data;
    (void)version;
    if (strcmp(interface, "wl_seat") == 0) {
        idle_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        printf("[bind] wl_seat bound (v%u)\n", version);
    } else if (strcmp(interface, "ext_idle_notifier_v1") == 0) {
        idle_notifier = wl_registry_bind(registry, name,
                                         &ext_idle_notifier_v1_interface, 1);
        printf("[bind] ext_idle_notifier_v1 bound (v%u)\n", version);
    } else if (strcmp(interface, "zwp_idle_inhibit_manager_v1") == 0) {
        inhibit_manager = wl_registry_bind(registry, name,
                                           &zwp_idle_inhibit_manager_v1_interface, 1);
        printf("[bind] zwp_idle_inhibit_manager_v1 bound (v%u)\n", version);
    } else if (strcmp(interface, "wl_compositor") == 0) {
        compositor = wl_registry_bind(registry, name,
                                      &wl_compositor_interface, 4);
    } else if (strcmp(interface, "wl_shm") == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -t, --timeout <seconds> <command>   Run command after seconds of inactivity (repeatable)\n"
        "  -r, --resume <command>               Run command when user becomes active\n"
        "  -h, --help                           Print this help\n",
        prog);
}

int main(int argc, char *argv[]) {
    static const struct option long_opts[] = {
        {"timeout", required_argument, NULL, 't'},
        {"resume",  required_argument, NULL, 'r'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:r:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 't':
            if (optind >= argc) {
                fprintf(stderr, "[error] --timeout requires two arguments: <seconds> <command>\n");
                return 1;
            }
            idle_config.rules = realloc(idle_config.rules, sizeof(idle_rule_t) * (idle_config.rule_count + 1));
            idle_config.rules[idle_config.rule_count].seconds = (uint32_t)atoi(optarg);
            idle_config.rules[idle_config.rule_count].command = argv[optind++];
            idle_config.rules[idle_config.rule_count].notification = NULL;
            idle_config.rule_count++;
            break;
        case 'r':
            idle_config.resume_cmd = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (idle_config.rule_count == 0) {
        fprintf(stderr, "[error] no --timeout rules given, nothing to do\n");
        return 1;
    }

    for (int i = 0; i < idle_config.rule_count; i++)
        printf("[config] after %us -> `%s`\n", idle_config.rules[i].seconds, idle_config.rules[i].command);
    if (idle_config.resume_cmd)
        printf("[config] on resume -> `%s`\n", idle_config.resume_cmd);
    printf("[config] gamepad idle threshold: %ds\n", GAMEPAD_TIMEOUT);

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "[error] failed to connect to wayland display\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!idle_notifier) {
        fprintf(stderr, "[error] compositor does not support ext-idle-notify-v1\n");
        return 1;
    }
    if (!idle_seat) {
        fprintf(stderr, "[error] no wl_seat found\n");
        return 1;
    }

    inhibit_init(compositor, shm);
    idle_register_notifications();
    gamepad_scan();
    gamepad_init_inotify();

    printf("[init] listening for idle/resume events... (Ctrl+C to quit)\n");

    signal(SIGCHLD, SIG_IGN);

    int wl_fd = wl_display_get_fd(display);

    while (1) {
        wl_display_flush(display);

        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);

        struct pollfd pfd[1 + MAX_GAMEPADS + 1];
        nfds_t nfds = 0;

        pfd[nfds].fd = wl_fd;
        pfd[nfds].events = POLLIN;
        nfds++;

        for (int i = 0; i < gamepad_count; i++) {
            pfd[nfds].fd = gamepads[i].fd;
            pfd[nfds].events = POLLIN;
            nfds++;
        }

        if (inotify_fd >= 0) {
            pfd[nfds].fd = inotify_fd;
            pfd[nfds].events = POLLIN;
            nfds++;
        }

        int poll_timeout = -1;
        if (inhibit_active && last_gamepad_activity > 0) {
            time_t now = time(NULL);
            time_t clear_at = last_gamepad_activity + GAMEPAD_TIMEOUT;
            if (now >= clear_at) {
                inhibit_set(false);
            } else {
                poll_timeout = (int)(clear_at - now) * 1000;
                if (poll_timeout < 100) poll_timeout = 100;
            }
        }

        poll(pfd, nfds, poll_timeout);

        wl_display_read_events(display);
        wl_display_dispatch_pending(display);

        for (int i = 0; i < gamepad_count; i++) {
            if (pfd[1 + i].revents & (POLLERR | POLLHUP)) {
                gamepad_remove(i);
                i--;
            } else if (pfd[1 + i].revents & POLLIN) {
                gamepad_read_events(i);
            }
        }

        if (inotify_fd >= 0 && pfd[1 + gamepad_count].revents & POLLIN)
            gamepad_handle_inotify();
    }

    fprintf(stderr, "[error] wayland display disconnected\n");

    idle_cleanup_notifications();
    wl_registry_destroy(registry);
    wl_display_disconnect(display);

    inhibit_cleanup();
    gamepad_cleanup();
    free(idle_config.rules);

    return 0;
}
