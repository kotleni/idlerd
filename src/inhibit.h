#ifndef IDLERD_INHIBIT_H
#define IDLERD_INHIBIT_H

#include <stdbool.h>
#include <wayland-client.h>
#include "idle-inhibit-protocol.h"

void inhibit_init(struct wl_compositor *compositor, struct wl_shm *shm);
void inhibit_set(bool enable);
void inhibit_cleanup(void);

extern struct zwp_idle_inhibit_manager_v1 *inhibit_manager;
extern struct wl_surface *inhibit_surface;
extern bool inhibit_active;

#endif
