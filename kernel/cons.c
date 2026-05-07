// /dev/cons — kernel console Dev backed by the PL011 UART (P4-B).
//
// Per ARCHITECTURE.md §9.4 + ROADMAP §6.1. v1.0 P4-B lands the
// write-side: writes go through `uart_putc` to the kernel UART. Reads
// return 0 (EOF) at v1.0 — UART RX is wired in a later sub-chunk
// (Phase 4+ when the IRQ-driven input path with a Rendez block lands).
//
// Single-file leaf Dev with dc='c'. Plan 9 conventionally pairs cons
// with consctl (mode control); consctl is held until the Phase 5 PTY +
// termios surface lands.

#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"

static void devcons_reset(void)    { /* no-op */ }
static void devcons_init(void)     { /* no-op — UART came up at boot */ }
static void devcons_shutdown(void) { /* no-op */ }

static struct Spoor *devcons_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devcons, QTFILE);
}

static struct Walkqid *devcons_walk(struct Spoor *c, struct Spoor *nc,
                                    const char **name, int nname) {
    (void)c; (void)nc; (void)name; (void)nname;
    return NULL;
}

static int devcons_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devcons_open(struct Spoor *c, int omode) {
    return dev_simple_open(c, omode);
}

static void devcons_create(struct Spoor *c, const char *name, int omode, u32 perm) {
    (void)c; (void)name; (void)omode; (void)perm;
}

static void devcons_close(struct Spoor *c) {
    dev_simple_close(c);
}

// Reads return 0 (EOF) at v1.0. UART RX with a blocking Rendez wait
// lands at a later P4 sub-chunk (likely after irqfwd in P4-G —
// userspace drivers will own the keyboard, so the kernel-side cons
// read may stay degenerate at v1.0 and rely on the userspace input
// driver delivering keystrokes via 9P).
static long devcons_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return 0;
}

static struct Block *devcons_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

// Writes forward each byte to the PL011 UART via uart_putc. Plan 9
// idiom: writes don't persist — the byte IS the message. Returns the
// number of bytes accepted (== n at v1.0; UART can't fail short).
static long devcons_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)off;
    if (!buf) return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;

    const u8 *bytes = (const u8 *)buf;
    for (long i = 0; i < n; i++) {
        uart_putc((char)bytes[i]);
    }
    return n;
}

static long devcons_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devcons_remove(struct Spoor *c) {
    (void)c;
}

static int devcons_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devcons_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devcons = {
    .dc       = 'c',
    .name     = "cons",

    .reset    = devcons_reset,
    .init     = devcons_init,
    .shutdown = devcons_shutdown,

    .attach   = devcons_attach,
    .walk     = devcons_walk,
    .stat     = devcons_stat,

    .open     = devcons_open,
    .create   = devcons_create,
    .close    = devcons_close,

    .read     = devcons_read,
    .bread    = devcons_bread,
    .write    = devcons_write,
    .bwrite   = devcons_bwrite,

    .remove   = devcons_remove,
    .wstat    = devcons_wstat,
    .power    = devcons_power,
};
