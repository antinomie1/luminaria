/* libinput's log handler takes a va_list, which Mica cannot: format the
 * message here and pass it on without its trailing newline. */
#include <libinput.h>
#include <stdio.h>
#include <string.h>

typedef void (*mc_libinput_log_fn)(struct libinput *, int, const char *);

static mc_libinput_log_fn handler;

static void forward(struct libinput *li, enum libinput_log_priority priority, const char *fmt, va_list args) {
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, args);
    size_t n = strlen(msg);
    if (n > 0 && msg[n - 1] == '\n')
        msg[n - 1] = 0;
    if (handler)
        handler(li, (int)priority, msg);
}

void mc_libinput_log_set_handler(struct libinput *li, mc_libinput_log_fn fn) {
    handler = fn;
    libinput_log_set_handler(li, forward);
}
