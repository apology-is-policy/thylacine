/* thyla_tap — the C tapestry client. See thyla_tap.h for the design.
 *
 * The call sequence mirrors usr/lib/libtapestry::Surface::open_on 1:1
 * (connect -> mint -> create -> weave geometry -> SYS_WEFT_MAP -> present
 * + event fids); the reweave mirrors Surface::reweave (ack -> fresh fid ->
 * map-new-before-clunk-old). Divergence from the native client is a bug.
 */
#include "thyla_tap.h"

#include <string.h>

#include <thyla/syscall.h>

/* ---- tiny text helpers (the protocol's ctl/geometry lines) ---------- */

static int tap_parse_u32(const char **p, const char *end, uint32_t *out)
{
    const char *s = *p;
    uint64_t v = 0;
    int any = 0;
    while (s < end && (*s == ' ' || *s == '\n' || *s == '\t'))
        s++;
    while (s < end && *s >= '0' && *s <= '9') {
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > 0xffffffffULL)
            return -1;
        s++;
        any = 1;
    }
    if (!any)
        return -1;
    *p = s;
    *out = (uint32_t)v;
    return 0;
}

static char *tap_fmt_u32(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    return p;
}

static long tap_read_all(long fd, char *buf, unsigned long cap)
{
    long n = t_read(fd, buf, cap);
    return n < 0 ? -1 : n;
}

/* ---- the surface path (surface/<id>/<leaf>) ------------------------- */

static long tap_open_leaf(const ThylaTap *t, const char *leaf,
                          unsigned long omode)
{
    char path[48];
    char *p = path;
    memcpy(p, "surface/", 8);
    p += 8;
    p = tap_fmt_u32(p, t->id);
    *p++ = '/';
    while (*leaf)
        *p++ = *leaf++;
    return t_open(t->root, path, (size_t)(p - path), omode);
}

/* Read + parse the weave geometry line "W H stride slot_stride nslots
 * b8g8r8a8" from an open weave fid. */
static int tap_read_geom(long weave_fd, uint32_t *w, uint32_t *h,
                         uint32_t *stride, uint64_t *slot_stride,
                         uint32_t *nslots)
{
    char buf[128];
    long n = tap_read_all(weave_fd, buf, sizeof(buf));
    if (n <= 0)
        return -1;
    const char *p = buf, *end = buf + n;
    uint32_t ss;
    if (tap_parse_u32(&p, end, w) || tap_parse_u32(&p, end, h) ||
        tap_parse_u32(&p, end, stride) || tap_parse_u32(&p, end, &ss) ||
        tap_parse_u32(&p, end, nslots))
        return -1;
    *slot_stride = ss;
    return 0;
}

int thyla_tap_display_size(uint32_t *w, uint32_t *h)
{
    long root = t_open(T_WALK_OPEN_FROM_ROOT, "/srv/tapestry", 13, T_OREAD);
    if (root < 0)
        return -1;
    long ctl = t_open(root, "ctl", 3, T_OREAD);
    if (ctl < 0) {
        t_close(root);
        return -1;
    }
    char buf[256];
    long n = tap_read_all(ctl, buf, sizeof(buf));
    t_close(ctl);
    t_close(root);
    if (n <= 8 || memcmp(buf, "display ", 8) != 0)
        return -1;
    const char *p = buf + 8, *end = buf + n;
    if (tap_parse_u32(&p, end, w) || tap_parse_u32(&p, end, h))
        return -1;
    return 0;
}

int thyla_tap_open(ThylaTap *t, uint32_t w, uint32_t h)
{
    memset(t, 0, sizeof(*t));
    t->root = t->ctl = t->weave_fd = t->present_fd = t->event_fd = -1;

    t->root = t_open(T_WALK_OPEN_FROM_ROOT, "/srv/tapestry", 13, T_OREAD);
    if (t->root < 0)
        goto fail;

    /* Mint: opening surface/new rebinds the fid onto the new surface's
     * ctl (the netd clone idiom); its read yields the id. */
    t->ctl = t_open(t->root, "surface/new", 11, T_ORDWR);
    if (t->ctl < 0)
        goto fail;
    {
        char idbuf[16];
        long n = tap_read_all(t->ctl, idbuf, sizeof(idbuf));
        if (n <= 0)
            goto fail;
        const char *p = idbuf, *end = idbuf + n;
        if (tap_parse_u32(&p, end, &t->id))
            goto fail;
    }

    /* create W H */
    {
        char cmd[40];
        char *p = cmd;
        memcpy(p, "create ", 7);
        p += 7;
        p = tap_fmt_u32(p, w);
        *p++ = ' ';
        p = tap_fmt_u32(p, h);
        if (t_write(t->ctl, cmd, (size_t)(p - cmd)) < 0)
            goto fail;
    }

    /* The weave: geometry + the zero-copy map (the kernel issues the lazy
     * Tweft under SYS_WEFT_MAP). */
    t->weave_fd = tap_open_leaf(t, "weave", T_OREAD);
    if (t->weave_fd < 0)
        goto fail;
    if (tap_read_geom(t->weave_fd, &t->w, &t->h, &t->stride,
                      &t->slot_stride, &t->nslots))
        goto fail;
    if (t->w != w || t->h != h || t->stride != w * 4 || t->nslots == 0)
        goto fail;
    {
        long va = t_weft_map(t->weave_fd, 0);
        if (va <= 0)
            goto fail;
        t->map_va = (uint64_t)va;
    }

    t->present_fd = tap_open_leaf(t, "present", T_OWRITE);
    t->event_fd = tap_open_leaf(t, "event", T_OREAD);
    if (t->present_fd < 0 || t->event_fd < 0)
        goto fail;
    return 0;

fail:
    thyla_tap_close(t);
    return -1;
}

int thyla_tap_present(ThylaTap *t, const ThylaRect *rects, int nrects)
{
    /* 32-byte header + up to 63 inline rects (the client-side bound; the
     * server independently caps at 64 total). */
    unsigned char buf[THYLA_TPRESENT_LEN + 63 * THYLA_TRECT_LEN];
    if (nrects < 0 || nrects > 63)
        nrects = 0; /* degrade to full-surface damage */

    ThylaRect r0 = { 0, 0, 0, 0 };
    if (nrects > 0)
        r0 = rects[0];

    uint32_t hdr[8] = {
        THYLA_TPRESENT_V1,
        0,                   /* slot 0 — the single-slot discipline */
        0,                   /* flags */
        (uint32_t)nrects,    /* 0 = full surface */
        r0.x, r0.y, r0.w, r0.h,
    };
    memcpy(buf, hdr, THYLA_TPRESENT_LEN);
    unsigned long len = THYLA_TPRESENT_LEN;
    for (int i = 1; i < nrects; i++) {
        memcpy(buf + len, &rects[i], THYLA_TRECT_LEN);
        len += THYLA_TRECT_LEN;
    }
    return t_write(t->present_fd, buf, len) < 0 ? -1 : 0;
}

int thyla_tap_read_events(ThylaTap *t, ThylaEvent *evs, int max)
{
    unsigned char buf[8 * THYLA_TEVENT_LEN];
    unsigned long cap = (unsigned long)(max < 8 ? max : 8) * THYLA_TEVENT_LEN;
    if (max <= 0)
        return 0;
    long n = t_read(t->event_fd, buf, cap);
    if (n < 0)
        return -1;
    if (n == 0)
        return 0; /* stream EOF: the surface retired under us */
    int count = (int)(n / THYLA_TEVENT_LEN);
    for (int i = 0; i < count; i++) {
        const unsigned char *r = buf + (unsigned long)i * THYLA_TEVENT_LEN;
        ThylaEvent *e = &evs[i];
        memcpy(&e->kind, r + 0, 2);
        memcpy(&e->code, r + 2, 2);
        memcpy(&e->value, r + 4, 4);
        memcpy(&e->rune, r + 8, 4);
        memcpy(&e->mods, r + 12, 2);
        memcpy(&e->flags, r + 14, 2);
        memcpy(&e->tick, r + 16, 8);
    }
    return count;
}

int thyla_tap_reweave(ThylaTap *t, uint32_t w, uint32_t h, uint16_t serial)
{
    /* resize W H <serial>: the Rwrite is the server's generation fence. */
    {
        char cmd[48];
        char *p = cmd;
        memcpy(p, "resize ", 7);
        p += 7;
        p = tap_fmt_u32(p, w);
        *p++ = ' ';
        p = tap_fmt_u32(p, h);
        *p++ = ' ';
        p = tap_fmt_u32(p, serial);
        long rc = t_write(t->ctl, cmd, (size_t)(p - cmd));
        if (rc < 0)
            return rc == -11 ? -2 : -1; /* -EAGAIN = stale serial */
    }

    /* Fresh weave fid (the old fid's kernel-side map binding is pinned to
     * the old generation); map new BEFORE clunking old so the client stays
     * mapped throughout. */
    long new_fd = tap_open_leaf(t, "weave", T_OREAD);
    if (new_fd < 0)
        return -1;
    uint32_t gw, gh, stride, nslots;
    uint64_t slot_stride;
    if (tap_read_geom(new_fd, &gw, &gh, &stride, &slot_stride, &nslots) ||
        gw != w || gh != h || stride != w * 4 || nslots != t->nslots) {
        t_close(new_fd);
        return -1;
    }
    long va = t_weft_map(new_fd, 0);
    if (va <= 0) {
        t_close(new_fd);
        return -1;
    }
    t_close(t->weave_fd); /* drops the old generation's client mapping */
    t->weave_fd = new_fd;
    t->map_va = (uint64_t)va;
    t->w = w;
    t->h = h;
    t->stride = stride;
    t->slot_stride = slot_stride;
    return 0;
}

void thyla_tap_close(ThylaTap *t)
{
    /* The weave clunk drops the client mapping; the session close (root)
     * retires every surface minted on it. */
    if (t->event_fd >= 0)
        t_close(t->event_fd);
    if (t->present_fd >= 0)
        t_close(t->present_fd);
    if (t->weave_fd >= 0)
        t_close(t->weave_fd);
    if (t->ctl >= 0)
        t_close(t->ctl);
    if (t->root >= 0)
        t_close(t->root);
    t->root = t->ctl = t->weave_fd = t->present_fd = t->event_fd = -1;
    t->map_va = 0;
}
