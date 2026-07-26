#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>

#include <wayland-client.h>
#include "idle-protocol.h"

typedef struct {
    uint32_t seconds;
    char *command;
} timeout_rule_t;

static struct {
    timeout_rule_t *rules;
    int rule_count;
    char *resume_cmd;
} config = {0};

static struct {
    struct wl_seat *seat;
    struct ext_idle_notifier_v1 *notifier;
} state = {0};

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

static void notification_idled(void *data, struct ext_idle_notification_v1 *notif) {
    (void)notif;
    const char *cmd = data;
    printf("[event] IDLED -> running: %s\n", cmd);
    run_command(cmd);
}

static void notification_resumed(void *data, struct ext_idle_notification_v1 *notif) {
    (void)notif;
    const char *cmd = data;
    printf("[event] RESUMED (rule: `%s`)\n", cmd);
    if (config.resume_cmd) {
        printf("[event] RESUMED -> running: %s\n", config.resume_cmd);
        run_command(config.resume_cmd);
    }
}

static const struct ext_idle_notification_v1_listener notification_listener = {
    .idled = notification_idled,
    .resumed = notification_resumed,
};

static void registry_global(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, "wl_seat") == 0) {
        state.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        printf("[bind] wl_seat bound (v%u)\n", version);
    } else if (strcmp(interface, "ext_idle_notifier_v1") == 0) {
        state.notifier = wl_registry_bind(registry, name,
                                          &ext_idle_notifier_v1_interface, 1);
        printf("[bind] ext_idle_notifier_v1 bound (v%u)\n", version);
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
    fprintf(stderr, "Usage: %s --timeout <seconds> <command> [--resume <command>]\n", prog);
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
            config.rules = realloc(config.rules, sizeof(timeout_rule_t) * (config.rule_count + 1));
            config.rules[config.rule_count].seconds = (uint32_t)atoi(optarg);
            config.rules[config.rule_count].command = argv[optind++];
            config.rule_count++;
            break;
        case 'r':
            config.resume_cmd = optarg;
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

    for (int i = 0; i < config.rule_count; i++) {
        printf("[config] after %us -> `%s`\n", config.rules[i].seconds, config.rules[i].command);
    }
    if (config.resume_cmd) {
        printf("[config] on resume -> `%s`\n", config.resume_cmd);
    }

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

    struct ext_idle_notification_v1 **notifications = calloc(config.rule_count, sizeof(void *));
    for (int i = 0; i < config.rule_count; i++) {
        uint32_t ms = config.rules[i].seconds * 1000;
        printf("[init] registering timeout: %ums -> `%s`\n", ms, config.rules[i].command);
        notifications[i] = ext_idle_notifier_v1_get_idle_notification(
            state.notifier, ms, state.seat);
        ext_idle_notification_v1_add_listener(notifications[i],
                                              &notification_listener,
                                              config.rules[i].command);
    }

    printf("[init] listening for idle/resume events... (Ctrl+C to quit)\n");

    signal(SIGCHLD, SIG_IGN);

    while (wl_display_dispatch(display) != -1) {
    }

    fprintf(stderr, "[error] wayland display disconnected\n");

    ext_idle_notifier_v1_destroy(state.notifier);
    wl_seat_destroy(state.seat);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    free(notifications);
    free(config.rules);

    return 0;
}
