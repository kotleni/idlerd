#ifndef IDLERD_IDLE_H
#define IDLERD_IDLE_H

#include <stdint.h>
#include <stdbool.h>
#include <wayland-client.h>
#include "idle-protocol.h"

typedef struct {
    uint32_t seconds;
    char *command;
    struct ext_idle_notification_v1 *notification;
} idle_rule_t;

typedef struct {
    idle_rule_t *rules;
    int rule_count;
    char *resume_cmd;
} idle_config_t;

extern idle_config_t idle_config;
extern struct wl_seat *idle_seat;
extern struct ext_idle_notifier_v1 *idle_notifier;
extern bool is_idle;
extern const struct ext_idle_notification_v1_listener notification_listener;

void idle_register_notifications(void);
void idle_reregister_notifications(void);
void idle_cleanup_notifications(void);

#endif
