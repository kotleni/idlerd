#include "inhibit.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

struct zwp_idle_inhibit_manager_v1 *inhibit_manager = NULL;
struct wl_surface *inhibit_surface = NULL;
bool inhibit_active = false;

static struct zwp_idle_inhibitor_v1 *inhibit_inhibitor = NULL;

void inhibit_init(struct wl_compositor *compositor, struct wl_shm *shm) {
    if (!inhibit_manager || !compositor || !shm) return;

    inhibit_surface = wl_compositor_create_surface(compositor);
    if (!inhibit_surface) return;

    int width = 1, height = 1, stride = width * 4;
    int size = stride * height;
    char tmppath[] = "/tmp/idlerd-shm-XXXXXX";
    int shm_fd = mkstemp(tmppath);
    if (shm_fd < 0) return;

    unlink(tmppath);
    ftruncate(shm_fd, size);
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        close(shm_fd);
        return;
    }

    memset(ptr, 0, size);
    munmap(ptr, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, shm_fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(shm_fd);

    wl_surface_attach(inhibit_surface, buf, 0, 0);
    wl_surface_commit(inhibit_surface);
    printf("[init] idle inhibit surface created\n");
}

void inhibit_set(bool enable) {
    if (!inhibit_manager || !inhibit_surface) return;

    if (enable && !inhibit_active) {
        inhibit_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
            inhibit_manager, inhibit_surface);
        inhibit_active = true;
        printf("[inhibit] compositor idle inhibited\n");
    } else if (!enable && inhibit_active) {
        if (inhibit_inhibitor) {
            zwp_idle_inhibitor_v1_destroy(inhibit_inhibitor);
            inhibit_inhibitor = NULL;
        }
        inhibit_active = false;
        printf("[inhibit] compositor idle uninhibited\n");
    }
}

void inhibit_cleanup(void) {
    inhibit_set(false);
    if (inhibit_surface)
        wl_surface_destroy(inhibit_surface);
    if (inhibit_manager)
        zwp_idle_inhibit_manager_v1_destroy(inhibit_manager);
}
