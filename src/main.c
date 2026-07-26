#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>
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
#include <sys/mman.h>
#include <limits.h>

#include <wayland-client.h>
#include "idle-protocol.h"
#include "idle-inhibit-protocol.h"

#define MAX_GAMEPADS 32
#define GAMEPAD_IDLE_DEFAULT 15

typedef struct {
    uint32_t seconds;
    char *command;
    struct ext_idle_notification_v1 *notification;
} idle_rule_t;

static struct {
    idle_rule_t *rules;
    int rule_count;
    char *resume_cmd;
    uint32_t gamepad_idle_timeout;
} config = {0};

static struct {
    struct wl_seat *seat;
    struct ext_idle_notifier_v1 *notifier;
} state = {0};

static struct {
    int fd;
    int device_num;
    char name[64];
} gamepads[MAX_GAMEPADS];
static int gamepad_count = 0;
static time_t last_gamepad_activity = 0;
static int inotify_fd = -1;
static bool is_idle = false;

static struct {
    struct zwp_idle_inhibit_manager_v1 *manager;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_surface *surface;
    struct zwp_idle_inhibitor_v1 *inhibitor;
    bool active;
} inhibit = {0};

static void run_command(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("sh", "sh", "-c", cmd, NULL);
        fprintf(stderr, "[error] failed to exec `%s`\n", cmd);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr, "[warn] command exited with %d: %s\n",
                    WEXITSTATUS(status), cmd);
        }
    }
}

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

static void inhibit_set(bool enable);
static const struct ext_idle_notification_v1_listener notification_listener;

static void remove_gamepad(int index) {
    close(gamepads[index].fd);
    printf("[gamepad] removed: %s (event%d)\n",
           gamepads[index].name, gamepads[index].device_num);
    for (int i = index; i < gamepad_count - 1; i++)
        gamepads[i] = gamepads[i + 1];
    gamepad_count--;
}

static void scan_gamepads(void) {
    DIR *dir = opendir("/dev/input");
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
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
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

static void read_gamepad_events(int index) {
    struct input_event ev;
    while (read(gamepads[index].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        last_gamepad_activity = time(NULL);
    }

    inhibit_set(true);

    if (is_idle) {
        is_idle = false;
        printf("[gamepad] activity while idle, waking up\n");
        if (config.resume_cmd) {
            printf("[event] WAKE -> running: %s\n", config.resume_cmd);
            run_command(config.resume_cmd);
        }
        for (int i = 0; i < config.rule_count; i++) {
            if (config.rules[i].notification)
                ext_idle_notification_v1_destroy(config.rules[i].notification);
            uint32_t ms = config.rules[i].seconds * 1000;
            config.rules[i].notification = ext_idle_notifier_v1_get_idle_notification(
                state.notifier, ms, state.seat);
            ext_idle_notification_v1_add_listener(
                config.rules[i].notification, &notification_listener,
                (void *)(intptr_t)i);
        }
        printf("[gamepad] re-registered %d notifications\n", config.rule_count);
    }
}

static void inhibit_set(bool enable) {
    if (!inhibit.manager || !inhibit.surface) return;

    if (enable && !inhibit.active) {
        inhibit.inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
            inhibit.manager, inhibit.surface);
        inhibit.active = true;
        printf("[inhibit] compositor idle inhibited\n");
    } else if (!enable && inhibit.active) {
        if (inhibit.inhibitor) {
            zwp_idle_inhibitor_v1_destroy(inhibit.inhibitor);
            inhibit.inhibitor = NULL;
        }
        inhibit.active = false;
        printf("[inhibit] compositor idle uninhibited\n");
    }
}

static void handle_inotify(void) {
    char buf[4096];
    ssize_t len;
    while ((len = read(inotify_fd, buf, sizeof(buf))) > 0) {
        (void)len;
    }

    for (int i = gamepad_count - 1; i >= 0; i--) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/input/event%d",
                 gamepads[i].device_num);
        struct stat st;
        if (stat(path, &st) != 0)
            remove_gamepad(i);
    }

    usleep(50000);
    scan_gamepads();
}

static void notification_idled(void *data, struct ext_idle_notification_v1 *notif) {
    (void)notif;
    int rule_idx = (int)(intptr_t)data;
    idle_rule_t *rule = &config.rules[rule_idx];

    time_t now = time(NULL);
    if (last_gamepad_activity > 0 &&
        (now - last_gamepad_activity) < (time_t)config.gamepad_idle_timeout) {
        printf("[event] IDLED but gamepad active %lds ago, skipping: %s\n",
               (long)(now - last_gamepad_activity), rule->command);

        ext_idle_notification_v1_destroy(rule->notification);
        uint32_t ms = rule->seconds * 1000;
        rule->notification = ext_idle_notifier_v1_get_idle_notification(
            state.notifier, ms, state.seat);
        ext_idle_notification_v1_add_listener(
            rule->notification, &notification_listener,
            (void *)(intptr_t)rule_idx);
        return;
    }

    printf("[event] IDLED -> running: %s\n", rule->command);
    is_idle = true;
    run_command(rule->command);
}

static void notification_resumed(void *data, struct ext_idle_notification_v1 *notif) {
    (void)notif;
    (void)data;
    if (is_idle) {
        printf("[event] RESUMED\n");
        is_idle = false;
        if (config.resume_cmd) {
            printf("[event] RESUMED -> running: %s\n", config.resume_cmd);
            run_command(config.resume_cmd);
        }
    }
}

static const struct ext_idle_notification_v1_listener notification_listener = {
    .idled = notification_idled,
    .resumed = notification_resumed,
};

static void registry_global(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface, uint32_t version) {
    (void)data;
    (void)version;
    if (strcmp(interface, "wl_seat") == 0) {
        state.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        printf("[bind] wl_seat bound (v%u)\n", version);
    } else if (strcmp(interface, "ext_idle_notifier_v1") == 0) {
        state.notifier = wl_registry_bind(registry, name,
                                          &ext_idle_notifier_v1_interface, 1);
        printf("[bind] ext_idle_notifier_v1 bound (v%u)\n", version);
    } else if (strcmp(interface, "zwp_idle_inhibit_manager_v1") == 0) {
        inhibit.manager = wl_registry_bind(registry, name,
                                           &zwp_idle_inhibit_manager_v1_interface, 1);
        printf("[bind] zwp_idle_inhibit_manager_v1 bound (v%u)\n", version);
    } else if (strcmp(interface, "wl_compositor") == 0) {
        inhibit.compositor = wl_registry_bind(registry, name,
                                              &wl_compositor_interface, 4);
    } else if (strcmp(interface, "wl_shm") == 0) {
        inhibit.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
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
        "  -g, --gamepad-idle <seconds>         Gamepad activity threshold (default: %d)\n"
        "  -h, --help                           Print this help\n",
        prog, GAMEPAD_IDLE_DEFAULT);
}

int main(int argc, char *argv[]) {
    static const struct option long_opts[] = {
        {"timeout",      required_argument, NULL, 't'},
        {"resume",       required_argument, NULL, 'r'},
        {"gamepad-idle", required_argument, NULL, 'g'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    config.gamepad_idle_timeout = GAMEPAD_IDLE_DEFAULT;

    int opt;
    while ((opt = getopt_long(argc, argv, "t:r:g:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 't':
            if (optind >= argc) {
                fprintf(stderr, "[error] --timeout requires two arguments: <seconds> <command>\n");
                return 1;
            }
            config.rules = realloc(config.rules, sizeof(idle_rule_t) * (config.rule_count + 1));
            config.rules[config.rule_count].seconds = (uint32_t)atoi(optarg);
            config.rules[config.rule_count].command = argv[optind++];
            config.rules[config.rule_count].notification = NULL;
            config.rule_count++;
            break;
        case 'r':
            config.resume_cmd = optarg;
            break;
        case 'g':
            config.gamepad_idle_timeout = (uint32_t)atoi(optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (config.rule_count == 0) {
        fprintf(stderr, "[error] no --timeout rules given, nothing to do\n");
        return 1;
    }

    for (int i = 0; i < config.rule_count; i++)
        printf("[config] after %us -> `%s`\n", config.rules[i].seconds, config.rules[i].command);
    if (config.resume_cmd)
        printf("[config] on resume -> `%s`\n", config.resume_cmd);
    printf("[config] gamepad idle threshold: %us\n", config.gamepad_idle_timeout);

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "[error] failed to connect to wayland display\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!state.notifier) {
        fprintf(stderr, "[error] compositor does not support ext-idle-notify-v1\n");
        return 1;
    }
    if (!state.seat) {
        fprintf(stderr, "[error] no wl_seat found\n");
        return 1;
    }

    if (inhibit.manager && inhibit.compositor && inhibit.shm) {
        inhibit.surface = wl_compositor_create_surface(inhibit.compositor);
        if (inhibit.surface) {
            int width = 1, height = 1, stride = width * 4;
            int size = stride * height;
            char tmppath[] = "/tmp/idlerd-shm-XXXXXX";
            int shm_fd = mkstemp(tmppath);
            if (shm_fd >= 0) {
                unlink(tmppath);
                ftruncate(shm_fd, size);
                void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
                if (ptr != MAP_FAILED) {
                    memset(ptr, 0, size);
                    munmap(ptr, size);
                    struct wl_shm_pool *pool = wl_shm_create_pool(inhibit.shm, shm_fd, size);
                    struct wl_buffer *buf = wl_shm_pool_create_buffer(
                        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
                    wl_shm_pool_destroy(pool);
                    close(shm_fd);
                    wl_surface_attach(inhibit.surface, buf, 0, 0);
                    wl_surface_commit(inhibit.surface);
                    printf("[init] idle inhibit surface created\n");
                } else {
                    close(shm_fd);
                }
            }
        }
    }

    for (int i = 0; i < config.rule_count; i++) {
        uint32_t ms = config.rules[i].seconds * 1000;
        printf("[init] registering timeout: %ums -> `%s`\n", ms, config.rules[i].command);
        config.rules[i].notification = ext_idle_notifier_v1_get_idle_notification(
            state.notifier, ms, state.seat);
        ext_idle_notification_v1_add_listener(
            config.rules[i].notification, &notification_listener,
            (void *)(intptr_t)i);
    }

    scan_gamepads();

    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd >= 0)
        inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE | IN_DELETE);

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
        if (inhibit.active && last_gamepad_activity > 0) {
            time_t now = time(NULL);
            time_t clear_at = last_gamepad_activity + config.gamepad_idle_timeout;
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
                remove_gamepad(i);
                i--;
            } else if (pfd[1 + i].revents & POLLIN) {
                read_gamepad_events(i);
            }
        }

        if (inotify_fd >= 0 && pfd[1 + gamepad_count].revents & POLLIN)
            handle_inotify();
    }

    fprintf(stderr, "[error] wayland display disconnected\n");

    for (int i = 0; i < config.rule_count; i++)
        ext_idle_notification_v1_destroy(config.rules[i].notification);
    ext_idle_notifier_v1_destroy(state.notifier);
    wl_seat_destroy(state.seat);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);

    inhibit_set(false);
    if (inhibit.surface)
        wl_surface_destroy(inhibit.surface);
    if (inhibit.manager)
        zwp_idle_inhibit_manager_v1_destroy(inhibit.manager);
    if (inhibit.compositor)
        wl_compositor_destroy(inhibit.compositor);
    if (inhibit.shm)
        wl_shm_destroy(inhibit.shm);

    for (int i = 0; i < gamepad_count; i++)
        close(gamepads[i].fd);
    if (inotify_fd >= 0)
        close(inotify_fd);

    free(config.rules);

    return 0;
}
