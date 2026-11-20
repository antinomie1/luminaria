/* A libwayland client for tests/libwayland.mica: binds wl_compositor and
 * wl_shm, shares a pool fd, attaches a buffer and waits for its frame
 * callback. Prints "ok" and exits 0 when the server behaved. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static int formats, frames;

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
    if (strcmp(iface, "wl_compositor") == 0)
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (strcmp(iface, "wl_shm") == 0)
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {global, global_remove};

static void format(void *data, struct wl_shm *s, uint32_t f) {
    if (f == WL_SHM_FORMAT_ARGB8888 || f == WL_SHM_FORMAT_XRGB8888)
        formats++;
}

static const struct wl_shm_listener shm_listener = {format};

static void done(void *data, struct wl_callback *cb, uint32_t time) {
    frames++;
    wl_callback_destroy(cb);
}

static const struct wl_callback_listener frame_listener = {done};

int main(void) {
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) {
        fprintf(stderr, "connect failed\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    if (wl_display_roundtrip(d) < 0 || !compositor || !shm) {
        fprintf(stderr, "globals missing\n");
        return 1;
    }
    wl_shm_add_listener(shm, &shm_listener, NULL);
    wl_display_roundtrip(d);
    if (formats != 2) {
        fprintf(stderr, "formats: %d\n", formats);
        return 1;
    }
    int size = 64 * 64 * 4;
    int fd = memfd_create("client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0)
        return 1;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    close(fd);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 64, 64, 256, WL_SHM_FORMAT_ARGB8888);
    struct wl_surface *s = wl_compositor_create_surface(compositor);
    wl_surface_attach(s, buf, 0, 0);
    wl_surface_damage_buffer(s, 0, 0, 64, 64);
    struct wl_callback *cb = wl_surface_frame(s);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    wl_surface_set_buffer_transform(s, WL_OUTPUT_TRANSFORM_90);
    wl_surface_commit(s);
    while (frames == 0)
        if (wl_display_dispatch(d) < 0) {
            fprintf(stderr, "dispatch failed\n");
            return 1;
        }
    wl_buffer_destroy(buf);
    wl_shm_pool_destroy(pool);
    wl_surface_destroy(s);
    if (wl_display_roundtrip(d) < 0)
        return 1;
    wl_display_disconnect(d);
    printf("ok\n");
    return 0;
}
