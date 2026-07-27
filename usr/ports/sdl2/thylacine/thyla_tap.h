/* thyla_tap — the C tapestry client (the SDL seam's substrate; G-7a).
 *
 * A minimal mirror of the native usr/lib/libtapestry client over plain
 * blocking file ops. The native client drives the same fids through Loom
 * (LOOM_OP_WRITE/READ on registered handles), which is WIRE-IDENTICAL to
 * a synchronous t_write/t_read — the server cannot tell the two apart —
 * so this client needs no Loom ring at all:
 *
 *   - present  = one blocking t_write of the 32-byte tpresent (+ inline
 *     rects). The Rwrite IS the completion (tapestryd presents
 *     synchronously inside one dispatch — the recycle gate), so a
 *     returned present means the compositor is DONE reading the pixels.
 *   - events   = blocking t_read of 24-byte tevent records (the server
 *     parks an empty read and delivers when an event arrives). Callers
 *     that must not block (SDL PumpEvents) run the read on a dedicated
 *     pump thread.
 *
 * Single-slot discipline: the weave carries WEAVE_SLOTS(3) slots for
 * pipelined native clients, but a synchronous client draws and presents
 * slot 0 only — the blocking present means the compositor never reads a
 * slot the client is still drawing, so one slot is tear-free by
 * construction.
 *
 * Everything here is unprivileged client code over the audited
 * /dev/tapestry protocol (docs/reference/139-tapestryd.md): the kernel +
 * tapestryd validate every descriptor, so a buggy caller corrupts only
 * its own surface.
 */
#ifndef THYLA_TAP_H
#define THYLA_TAP_H

#include <stdint.h>

#define THYLA_TEV_KEY       1
#define THYLA_TEV_PTR_MOVE  2
#define THYLA_TEV_PTR_BTN   3
#define THYLA_TEV_SCROLL    4
#define THYLA_TEV_FRAME     5
#define THYLA_TEV_CONFIGURE 6
#define THYLA_TEV_FOCUS     7
#define THYLA_TEV_CLOSE     8
#define THYLA_TEV_PTR_REL   9

#define THYLA_TEVENT_LEN    24
#define THYLA_TPRESENT_LEN  32
#define THYLA_TRECT_LEN     16
#define THYLA_TPRESENT_V1   1

/* A decoded 24-byte tevent record (TAPESTRY.md section 18.4). */
typedef struct ThylaEvent {
    uint16_t kind;
    uint16_t code;   /* KEY: evdev keycode; CONFIGURE: resize serial */
    uint32_t value;  /* KEY: 1 press / 0 release; CONFIGURE: (w<<16)|h */
    uint32_t rune;   /* KEY: resolved UTF-32 (0 = none) */
    uint16_t mods;
    uint16_t flags;
    uint64_t tick;
} ThylaEvent;

typedef struct ThylaRect {
    uint32_t x, y, w, h;
} ThylaRect;

typedef struct ThylaTap {
    long root;       /* the private session (open=connect on /srv/tapestry) */
    long ctl;        /* surface ctl (minted via surface/new) */
    long weave_fd;   /* the map-capability fid (generation-pinned) */
    long present_fd;
    long event_fd;
    uint32_t id;     /* surface id in this session */
    uint32_t w, h;
    uint32_t stride;      /* bytes per row (w*4) */
    uint64_t slot_stride; /* bytes per slot */
    uint32_t nslots;
    uint64_t map_va;      /* the mapped weave; slot 0 pixels at map_va */
} ThylaTap;

/* The display dimensions from the global ctl ("display W H"). Opens and
 * closes a throwaway session. Returns 0 or -1. */
int thyla_tap_display_size(uint32_t *w, uint32_t *h);

/* Connect a fresh session and create a w x h surface (mapped, slot 0
 * zeroed by the server). Returns 0 or -1 (everything rolled back). */
int thyla_tap_open(ThylaTap *t, uint32_t w, uint32_t h);

/* Pixels of the draw slot: w*h little-endian 0xAARRGGBB words. */
static inline uint32_t *thyla_tap_pixels(ThylaTap *t)
{
    return (uint32_t *)(uintptr_t)t->map_va;
}

/* Present slot 0. nrects == 0 = full-surface damage; rect 0 rides the
 * header, the rest inline (the server caps at 64). Blocks until the
 * compositor has consumed the pixels. Returns 0 or -1. */
int thyla_tap_present(ThylaTap *t, const ThylaRect *rects, int nrects);

/* Blocking read of up to `max` events (parks when none pending; returns
 * on the first delivery). Returns the record count, 0 on stream EOF
 * (surface retired under us), -1 on error. */
int thyla_tap_read_events(ThylaTap *t, ThylaEvent *evs, int max);

/* Ack a CONFIGURE resize offer (serial = ev.code) and swap onto the new
 * weave generation. Returns 0, -2 on a stale serial (E_AGAIN — drain
 * events, a newer CONFIGURE carries the current offer), -1 fatal. */
int thyla_tap_reweave(ThylaTap *t, uint32_t w, uint32_t h, uint16_t serial);

/* Request the compositor RETIRE this surface (ctl "destroy"). tapestryd
 * then reads the surface's event fid EMPTY (stream end) — the wake a
 * parked event reader (the SDL pump thread) needs at shutdown. Closing
 * event_fd from a sibling does NOT cancel a parked t_read (#844: the
 * reader holds its own ref-held Spoor across the blocking read, so the
 * Dev close hook never runs), so the retire's EOF — not a close — is the
 * bounded, frame-clock-independent wake. Best-effort (a failed write
 * leaves the periodic FRAME event as the fallback wake). */
void thyla_tap_request_close(ThylaTap *t);

void thyla_tap_close(ThylaTap *t);

#endif /* THYLA_TAP_H */
