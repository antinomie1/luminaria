/* A libwayland xdg-shell client for tests/display.mica: a 64x64 red shm
 * window. It waits for the configure, draws, then for a frame callback,
 * a pointer enter and a click, and prints "ok". */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm;
static int configured, frames, entered, clicked;

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
    if (strcmp(iface, "wl_compositor") == 0)
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (strcmp(iface, "wl_shm") == 0)
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (strcmp(iface, "xdg_wm_base") == 0)
        wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (strcmp(iface, "wl_seat") == 0)
        seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};

static void ping(void *data, struct xdg_wm_base *w, uint32_t serial) { xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_listener = {ping};

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
    xdg_surface_ack_configure(xs, serial);
    configured = 1;
}
static const struct xdg_surface_listener xs_listener = {xs_configure};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {}
static void top_close(void *data, struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener top_listener = {top_configure, top_close};

static void done(void *data, struct wl_callback *cb, uint32_t time) {
    frames++;
    wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_listener = {done};

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) { entered = 1; }
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t s) { clicked = 1; }
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static void p_frame(void *d, struct wl_pointer *p) {}
static void p_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void p_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void p_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static const struct wl_pointer_listener pointer_listener = {p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_source, p_stop, p_discrete};

int main(void) {
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) {
        fprintf(stderr, "connect failed\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    if (wl_display_roundtrip(d) < 0 || !compositor || !shm || !wm || !seat) {
        fprintf(stderr, "globals missing\n");
        return 1;
    }
    xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    struct wl_pointer *ptr = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(ptr, &pointer_listener, NULL);
    struct wl_surface *s = wl_compositor_create_surface(compositor);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, s);
    xdg_surface_add_listener(xs, &xs_listener, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_title(top, "red");
    wl_surface_commit(s);
    while (!configured)
        if (wl_display_dispatch(d) < 0)
            return 1;
    int w = 64, h = 64, size = w * h * 4;
    int fd = memfd_create("window", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0)
        return 1;
    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < w * h; i++)
        px[i] = 0xffff0000; /* opaque red, ARGB */
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
    wl_surface_attach(s, buf, 0, 0);
    wl_surface_damage_buffer(s, 0, 0, w, h);
    struct wl_callback *cb = wl_surface_frame(s);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    wl_surface_commit(s);
    while (frames == 0 || !entered || !clicked)
        if (wl_display_dispatch(d) < 0) {
            fprintf(stderr, "dispatch failed\n");
            return 1;
        }
    printf("ok\n");
    fflush(stdout);
    xdg_toplevel_destroy(top);
    xdg_surface_destroy(xs);
    wl_surface_destroy(s);
    wl_display_roundtrip(d);
    wl_display_disconnect(d);
    return 0;
}
