/* xkbcommon's log function takes a va_list, which Mica cannot: format the
 * message here and pass it on without its trailing newline. */
#include <xkbcommon/xkbcommon.h>
#include <stdio.h>
#include <string.h>

typedef void (*mc_xkb_log_fn)(struct xkb_context *, int, const char *);

static mc_xkb_log_fn handler;

static void forward(struct xkb_context *ctx, enum xkb_log_level level, const char *fmt, va_list args) {
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, args);
    size_t n = strlen(msg);
    if (n > 0 && msg[n - 1] == '\n')
        msg[n - 1] = 0;
    if (handler)
        handler(ctx, (int)level, msg);
}

void mc_xkb_context_set_log_fn(struct xkb_context *ctx, mc_xkb_log_fn fn) {
    handler = fn;
    xkb_context_set_log_fn(ctx, forward);
}
