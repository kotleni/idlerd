#include "idle.h"
#include "command.h"
#include "gamepad.h"
#include "inhibit.h"

#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

idle_config_t idle_config = {0};
struct wl_seat *idle_seat = NULL;
struct ext_idle_notifier_v1 *idle_notifier = NULL;
bool is_idle = false;

static void notification_idled(void *data, struct ext_idle_notification_v1 *notif) {
    (void)notif;
    int rule_idx = (int)(intptr_t)data;
    idle_rule_t *rule = &idle_config.rules[rule_idx];

    time_t now = time(NULL);
    if (last_gamepad_activity > 0 &&
        (now - last_gamepad_activity) < (time_t)GAMEPAD_TIMEOUT) {
        printf("[event] IDLED but gamepad active %lds ago, skipping: %s\n",
               (long)(now - last_gamepad_activity), rule->command);

        ext_idle_notification_v1_destroy(rule->notification);
        uint32_t ms = rule->seconds * 1000;
        rule->notification = ext_idle_notifier_v1_get_idle_notification(
            idle_notifier, ms, idle_seat);
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
        if (idle_config.resume_cmd) {
            printf("[event] RESUMED -> running: %s\n", idle_config.resume_cmd);
            run_command(idle_config.resume_cmd);
        }
    }
}

const struct ext_idle_notification_v1_listener notification_listener = {
    .idled = notification_idled,
    .resumed = notification_resumed,
};

void idle_register_notifications(void) {
    for (int i = 0; i < idle_config.rule_count; i++) {
        uint32_t ms = idle_config.rules[i].seconds * 1000;
        printf("[init] registering timeout: %ums -> `%s`\n", ms, idle_config.rules[i].command);
        idle_config.rules[i].notification = ext_idle_notifier_v1_get_idle_notification(
            idle_notifier, ms, idle_seat);
        ext_idle_notification_v1_add_listener(
            idle_config.rules[i].notification, &notification_listener,
            (void *)(intptr_t)i);
    }
}

void idle_reregister_notifications(void) {
    is_idle = false;
    if (idle_config.resume_cmd) {
        printf("[event] WAKE -> running: %s\n", idle_config.resume_cmd);
        run_command(idle_config.resume_cmd);
    }
    for (int i = 0; i < idle_config.rule_count; i++) {
        if (idle_config.rules[i].notification)
            ext_idle_notification_v1_destroy(idle_config.rules[i].notification);
        uint32_t ms = idle_config.rules[i].seconds * 1000;
        idle_config.rules[i].notification = ext_idle_notifier_v1_get_idle_notification(
            idle_notifier, ms, idle_seat);
        ext_idle_notification_v1_add_listener(
            idle_config.rules[i].notification, &notification_listener,
            (void *)(intptr_t)i);
    }
    printf("[gamepad] re-registered %d notifications\n", idle_config.rule_count);
}

void idle_cleanup_notifications(void) {
    for (int i = 0; i < idle_config.rule_count; i++)
        ext_idle_notification_v1_destroy(idle_config.rules[i].notification);
    if (idle_notifier)
        ext_idle_notifier_v1_destroy(idle_notifier);
    if (idle_seat)
        wl_seat_destroy(idle_seat);
}
