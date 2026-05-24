// Dev vtable + bestiary registry — kernel device infrastructure (P4-A).
//
// Per ARCHITECTURE.md §9.2 + ROADMAP §6.1. The bestiary is a
// sentinel-terminated array; dev_register appends; dev_init walks and
// invokes each ->init() hook once. devnone (kernel/devnone.c) is the
// no-op stub registered first; tests rely on it being lookup-able by
// dc='-' or name="none".
//
// Lookup is linear-scan (v1.0 has < 16 devs; cache-friendly). When
// dev count grows past the cache-line frontier (Phase 5+), upgrade
// to a small hash or RB-tree.

#include <thylacine/dev.h>
#include <thylacine/devcap.h>
#include <thylacine/devsrv.h>
#include <thylacine/extinction.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"

// bestiary[] backing storage — sized to BESTIARY_MAX + 1 so the sentinel
// (NULL pointer at index dev_count) is always present. Iterators stop
// at the first NULL.
struct Dev *bestiary[BESTIARY_MAX + 1];

static int  g_dev_count;
static bool g_dev_init_done;

// Tiny string-equality (no libc; no strcmp in the kernel surface).
// Returns true iff a and b name-equal as zero-terminated C strings.
static bool name_eq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

int dev_register(struct Dev *d) {
    if (!d)
        extinction("dev_register(NULL)");
    if (g_dev_count >= BESTIARY_MAX)
        extinction("dev_register: bestiary full (BESTIARY_MAX exceeded)");

    // Collision check — both dc and name must be unique. The bestiary's
    // primary lookup is by dc (the on-wire identity that walks/syscalls
    // dispatch through); name collisions are a separate confusion class.
    for (int i = 0; i < g_dev_count; i++) {
        if (bestiary[i]->dc == d->dc) {
            extinction("dev_register: dc collision (duplicate device character)");
        }
        if (name_eq(bestiary[i]->name, d->name)) {
            extinction("dev_register: name collision (duplicate device name)");
        }
    }

    int idx = g_dev_count;
    bestiary[idx] = d;
    g_dev_count++;
    bestiary[g_dev_count] = NULL;       // maintain the sentinel
    return idx;
}

struct Dev *dev_lookup_by_dc(int dc) {
    for (int i = 0; i < g_dev_count; i++) {
        if (bestiary[i]->dc == dc) return bestiary[i];
    }
    return NULL;
}

struct Dev *dev_lookup_by_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_dev_count; i++) {
        if (name_eq(bestiary[i]->name, name)) return bestiary[i];
    }
    return NULL;
}

int dev_count(void) {
    return g_dev_count;
}

// Helper for dev_init's banner — print a small unsigned without going
// through uart_putdec's u64 widening (cheap enough at boot, but this
// keeps the banner string short).
static void dev_init_banner(int n) {
    uart_puts("  dev:  ");
    uart_putdec((u64)n);
    uart_puts(" registered (");
    for (int i = 0; i < g_dev_count; i++) {
        uart_puts(bestiary[i]->name);
        if (i + 1 < g_dev_count) uart_puts(", ");
    }
    uart_puts(")\n");
}

void dev_init(void) {
    if (g_dev_init_done)
        extinction("dev_init called twice");

    // Sequence per dev.h:
    //   1. spoor_init() — Spoor SLUB cache.
    //   2. dev_register(&devnone) — devnone is always first.
    //   3. dev_register the kernel-internal trivial Devs (P4-B + P6-pouch-
    //      devnodes for devfull). Order: cons first (so the boot banner
    //      could route through it; not yet, but the slot is reserved);
    //      then null / zero / full / random in alphabetical order for
    //      predictable bestiary iteration.
    //   4. Walk bestiary[] calling each non-NULL dev->init().
    spoor_init();

    dev_register(&devnone);
    dev_register(&devcons);
    dev_register(&devnull);
    dev_register(&devzero);
    dev_register(&devfull);
    dev_register(&devrandom);
    dev_register(&devnotes);        // P6-pouch-signals-impl: per-Proc note fd (dc='n')
    dev_register(&devproc);
    dev_register(&devctl);
    dev_register(&devramfs);
    dev_register(&devsrv);          // P5-corvus-srv: /srv service registry (dc='s')
    dev_register(&devcap);          // P5-hostowner-b: /cap elevation device (dc='C')

    // Walk bestiary: dev->init() may itself dev_register additional
    // devs (e.g., a virtio probe that fans out to multiple instances).
    // Re-read g_dev_count each iteration so newly-registered devs get
    // their init() called too. We DO NOT call init() on a dev that
    // dev_register added during another dev's init() AGAIN — each dev
    // is initialized exactly once (i tracks the watermark).
    int initialized = 0;
    while (initialized < g_dev_count) {
        struct Dev *d = bestiary[initialized];
        if (d && d->init) {
            d->init();
        }
        initialized++;
    }

    g_dev_init_done = true;

    dev_init_banner(g_dev_count);
}

// =============================================================================
// Shared helpers for leaf-file Devs (P4-B).
// =============================================================================
//
// dev_simple_attach: alloc Spoor + populate qid. Caller picks the
// qtype — typically QTFILE for a single-file leaf, QTDIR for a Dev
// whose root is a directory.
struct Spoor *dev_simple_attach(struct Dev *d, u8 qtype) {
    struct Spoor *c = spoor_alloc(d);
    if (!c) return NULL;
    c->qid.path = 0;
    c->qid.vers = 0;
    c->qid.type = qtype;
    return c;
}

// dev_simple_open: mark COPEN + record omode. Idempotent — re-opening
// an already-open Spoor just updates omode (matches Plan 9 idiom).
struct Spoor *dev_simple_open(struct Spoor *c, int omode) {
    if (!c) return NULL;
    c->flag |= COPEN;
    c->mode = omode;
    return c;
}

// dev_simple_close: clear COPEN. Per-Dev close hooks that need to
// release aux state should call dev_simple_close last (or first;
// order doesn't matter at v1.0 since aux is dev-private).
void dev_simple_close(struct Spoor *c) {
    if (!c) return;
    c->flag &= ~COPEN;
}
