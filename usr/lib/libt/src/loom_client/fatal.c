// A broken invariant inside the Loom client cores: name the site on fd 2, then
// end every thread of the process. Never ignored, and never assert or abort,
// which freestanding libt does not have.
#include <thyla/syscall.h>
#include <thyla/loom_client/base.h>

#define LOOM_CLIENT_FATAL_STATUS 134

static size_t put(char *buf, size_t at, size_t cap, const char *s) {
    while (*s && at < cap) buf[at++] = *s++;
    return at;
}

void t_loom_client_fatal(const char *func, unsigned line) {
    char buf[128];
    char digits[10];
    size_t n = 0;
    size_t at = put(buf, 0, sizeof buf - 12, "loom-client: broken invariant in ");
    at = put(buf, at, sizeof buf - 12, func);
    buf[at++] = ':';
    do {
        digits[n++] = (char)('0' + line % 10);
        line /= 10;
    } while (line && n < sizeof digits);
    while (n) buf[at++] = digits[--n];
    buf[at++] = '\n';
    (void)t_write(2, buf, at);
    t_exit_group(LOOM_CLIENT_FATAL_STATUS);
}
