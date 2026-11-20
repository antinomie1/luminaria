/* libseat's log handler takes a va_list, which Mica cannot: format the
 * message here and pass it on without its trailing newline. */
#include <libseat.h>
#include <stdio.h>
#include <string.h>

typedef void (*mc_libseat_log_fn)(int, const char *);

static mc_libseat_log_fn handler;

static void forward(enum libseat_log_level level, const char *fmt, va_list args) {
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, args);
    size_t n = strlen(msg);
    if (n > 0 && msg[n - 1] == '\n')
        msg[n - 1] = 0;
    if (handler)
        handler((int)level, msg);
}

void mc_libseat_set_log_handler(mc_libseat_log_fn fn) {
    handler = fn;
    libseat_set_log_handler(forward);
}
