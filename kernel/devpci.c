// devpci -- the kernel-mediated PCI topology, published as a walkable tree
// (Menagerie 6b).
//
// Per docs/MENAGERIE.md section 7/16-6b + ARCHITECTURE.md section 9.4. The Plan 9
// `#$`/devpci idiom: the kernel enumerated the PCIe bus at boot (virtio_pci.c ->
// g_virtio_pci_devs[]); devpci re-publishes that already-enumerated topology as a
// read-only namespace so the warden's in-process PciSource (the DtbSource analog)
// can discover PCI functions WITHOUT touching config space. The kernel poked the
// bus once, at boot; userspace reads the mediated result here.
//
//   /hw/pci                      -> the PCI root          (QTDIR)
//   /hw/pci/<bus.dev.fn>         -> one function's dir    (QTDIR)
//   /hw/pci/<bus.dev.fn>/ctl     -> that function's info  (QTFILE; one text line)
//
// The `ctl` line (one line, newline-terminated):
//   v1 bus=<hex> dev=<hex> fn=<hex> vendor=<hex> device=<hex> virtio=<dec> intid=<hex|none>
// bus/dev/fn/vendor/device/intid are bare lowercase hex; `virtio` is the derived
// virtio device-id in decimal (matching the libdriver `virtio:N` / `virtio-pci:N`
// id convention); `intid` is the INTx-routed GIC INTID (`none` if the function
// declares no INTx pin or the DTB has no route).
//
// I-5 / I-15 posture: devpci is READ-ONLY (write / create / wstat / remove / power
// all fail) and exposes ONLY the bounded topology -- NEVER raw ECAM and NO
// config-space WRITE surface (the pci-3 "userspace never gets raw ECAM" property).
// The interrupt pin is read from config space KERNEL-SIDE (via the already-mapped
// d->cfg KVA) and only the derived INTID is reported. Like devhw it is
// `perm_enforced = false` -- visibility, not authority: the privilege boundary is
// the hardware ALLOWANCE (I-34), not this tree. devpci mounts at /hw/pci via a
// synthetic `pci` mount-point child of devhw (kernel/joey.c).
//
// Qid encoding:
//   path == 0            -> the PCI root directory (the conventional Dev-root path).
//   path even, != 0      -> a function directory; index = (path >> 1) - 1.
//   path odd             -> a function's `ctl` file; index = (path >> 1) - 1.
// i.e. function i (0-based, an index into g_virtio_pci_devs[]) owns dir (i+1)*2 and
// ctl (i+1)*2 + 1. The device array is built once at boot and immutable, so an
// index minted by a successful walk stays valid for the kernel's lifetime.

#include <thylacine/dev.h>
#include <thylacine/dtb.h>           // dtb_pci_intx_route
#include <thylacine/pci_handle.h>    // PCI_INT_PIN_INTA (unused; INTA is the pin we read)
#include <thylacine/proc.h>          // PRINCIPAL_SYSTEM / GID_SYSTEM
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>       // struct t_stat + T_S_IFDIR / T_S_IFREG
#include <thylacine/types.h>
#include <thylacine/virtio_pci.h>    // virtio_pci_dev_count / _get + PCI_CFG_INT_PIN

// =============================================================================
// Qid encoding.
// =============================================================================

#define PCI_QID_ROOT  0ULL

static inline bool pci_qid_is_root(u64 p) { return p == PCI_QID_ROOT; }
static inline bool pci_qid_is_ctl(u64 p)  { return p != 0 && (p & 1ULL); }
static inline bool pci_qid_is_dir(u64 p)  { return p != 0 && (p & 1ULL) == 0; }
// Index into g_virtio_pci_devs[] for a function dir OR its ctl ((i+1)*2[+1] >> 1
// drops the ctl low bit and recovers i+1).
static inline int  pci_qid_index(u64 p)   { return (int)(p >> 1) - 1; }
static inline u64  pci_qid_dir(int i)     { return ((u64)i + 1) << 1; }
static inline u64  pci_qid_ctl(int i)     { return (((u64)i + 1) << 1) | 1ULL; }

// =============================================================================
// Text formatting (self-contained, the devctl/devproc idiom -- each formatting
// Dev keeps its own bounded append helpers). All return the number of bytes
// written, or 0 if they would not fit (the caller treats 0 as "stop").
// =============================================================================

// Bare lowercase hex (no 0x prefix -- the libdriver push_hex/parse_hex convention).
static size_t pf_hex(char *buf, size_t cap, size_t off, u64 v) {
    char tmp[16];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < (int)sizeof(tmp)) {
            int d = (int)(v & 0xF);
            tmp[n++] = (char)((d < 10) ? ('0' + d) : ('a' + d - 10));
            v >>= 4;
        }
    }
    if (off + (size_t)n > cap) return 0;
    for (int i = 0; i < n; i++) buf[off + i] = tmp[n - 1 - i];
    return (size_t)n;
}

static size_t pf_dec(char *buf, size_t cap, size_t off, u64 v) {
    char tmp[24];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (int)(v % 10));
            v /= 10;
        }
    }
    if (off + (size_t)n > cap) return 0;
    for (int i = 0; i < n; i++) buf[off + i] = tmp[n - 1 - i];
    return (size_t)n;
}

static size_t pf_str(char *buf, size_t cap, size_t off, const char *s) {
    size_t n = 0;
    while (s[n]) {
        if (off + n >= cap) return 0;
        buf[off + n] = s[n];
        n++;
    }
    return n;
}

// Append one `key=<hex>` field; returns the new offset (unchanged on overflow,
// which the line-builder treats as truncation -> a short, still-valid line).
static size_t pf_kv_hex(char *buf, size_t cap, size_t off, const char *key, u64 v) {
    size_t r = pf_str(buf, cap, off, key);
    if (r == 0) return off;
    off += r;
    r = pf_hex(buf, cap, off, v);
    return r ? off + r : off;
}

// =============================================================================
// The per-function `ctl` line + the function's directory name (its bdf).
// =============================================================================

// Format function i's directory name "<bus>.<dev>.<fn>" (bare hex) into `buf`
// (>= 16 bytes). Returns the length, or 0 if i names no function.
static size_t format_bdf(int i, char *buf, size_t cap) {
    struct virtio_pci_dev *d = virtio_pci_dev_get(i);
    if (!d) return 0;
    size_t off = 0;
    size_t r;
    r = pf_hex(buf, cap, off, d->bus);  if (!r) return 0; off += r;
    r = pf_str(buf, cap, off, ".");     if (!r) return 0; off += r;
    r = pf_hex(buf, cap, off, d->dev);  if (!r) return 0; off += r;
    r = pf_str(buf, cap, off, ".");     if (!r) return 0; off += r;
    r = pf_hex(buf, cap, off, d->fn);   if (!r) return 0; off += r;
    return off;
}

// Build function i's ctl line into `buf`. Returns the length (no trailing NUL),
// or 0 if i names no function. The INTID is read KERNEL-SIDE from the function's
// own (already-mapped) config space -- userspace never sees config space.
static size_t build_ctl(int i, char *buf, size_t cap) {
    struct virtio_pci_dev *d = virtio_pci_dev_get(i);
    if (!d) return 0;

    size_t off = 0;
    size_t r;
    r = pf_str(buf, cap, off, "v1 ");                       if (!r) return 0; off += r;
    off = pf_kv_hex(buf, cap, off, "bus=",    d->bus);
    off = pf_kv_hex(buf, cap, off, " dev=",   d->dev);
    off = pf_kv_hex(buf, cap, off, " fn=",    d->fn);
    off = pf_kv_hex(buf, cap, off, " vendor=", d->vendor_id);
    off = pf_kv_hex(buf, cap, off, " device=", d->device_id);
    r = pf_str(buf, cap, off, " virtio=");                  if (r) { off += r; r = pf_dec(buf, cap, off, d->virtio_device_id); if (r) off += r; }
    r = pf_str(buf, cap, off, " intid=");                   if (r) off += r;

    // INTx -> GIC INTID, resolved kernel-side. pin 0 = no INTx; else swizzle the
    // function's (dev, pin) through the DTB interrupt-map.
    u8 pin = virtio_pci_cfg_read8(d, PCI_CFG_INT_PIN);
    u32 intid = 0;
    bool has_intid = (pin != 0) && dtb_pci_intx_route(d->dev, pin, &intid);
    if (has_intid) {
        r = pf_hex(buf, cap, off, intid);
    } else {
        r = pf_str(buf, cap, off, "none");
    }
    if (r) off += r;

    r = pf_str(buf, cap, off, "\n");
    if (r) off += r;
    return off;
}

// =============================================================================
// Walk.
// =============================================================================

static bool name_eq(const char *a, const char *b) {
    int j = 0;
    while (a[j] && b[j] && a[j] == b[j]) j++;
    return a[j] == 0 && b[j] == 0;
}

// Single-step walk dispatch. `cur_path` is the current Spoor's qid.path; fills
// *out + returns true on a hit, false on a miss. (`.` / `..` normally never reach
// a Dev -- stalk resolves them against the trail -- but devpci handles them for
// the direct-call path + robustness, matching devhw / devdev / devramfs.)
static bool walk_one(u64 cur_path, const char *name, struct Qid *out) {
    out->path = 0; out->vers = 0; out->type = 0;
    out->pad[0] = out->pad[1] = out->pad[2] = 0;

    if (name[0] == '.' && name[1] == '\0') {
        out->path = cur_path;
        out->type = pci_qid_is_ctl(cur_path) ? QTFILE : QTDIR;
        return true;
    }
    // ".." -- the tree is two levels (root -> function dir -> ctl). A ctl's parent
    // is its function dir; a function dir's (and the root's) parent is the root.
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        if (pci_qid_is_ctl(cur_path)) {
            out->path = pci_qid_dir(pci_qid_index(cur_path));
        } else {
            out->path = PCI_QID_ROOT;
        }
        out->type = QTDIR;
        return true;
    }

    // A ctl file is a leaf -- nothing descends from it.
    if (pci_qid_is_ctl(cur_path)) return false;

    if (pci_qid_is_root(cur_path)) {
        // root -> a function directory named by its bdf.
        int count = virtio_pci_dev_count();
        for (int i = 0; i < count; i++) {
            char bdf[16];
            size_t n = format_bdf(i, bdf, sizeof(bdf) - 1);
            if (n == 0) continue;
            bdf[n] = '\0';
            if (name_eq(bdf, name)) {
                out->path = pci_qid_dir(i);
                out->type = QTDIR;
                return true;
            }
        }
        return false;
    }

    // A function directory -> its sole `ctl` leaf.
    if (pci_qid_is_dir(cur_path)) {
        if (name_eq("ctl", name)) {
            out->path = pci_qid_ctl(pci_qid_index(cur_path));
            out->type = QTFILE;
            return true;
        }
        return false;
    }

    return false;
}

// =============================================================================
// Vtable.
// =============================================================================

static void devpci_reset(void)    { /* no-op */ }
static void devpci_init(void)     { /* no-op */ }
static void devpci_shutdown(void) { /* no-op */ }

static struct Spoor *devpci_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devpci, QTDIR);
}

static struct Walkqid *devpci_walk(struct Spoor *c, struct Spoor *nc,
                                   const char **name, int nname) {
    if (!c) return NULL;
    if (nname < 0) return NULL;

    struct Walkqid *wq = walkqid_alloc(nname);
    if (!wq) return NULL;

    // Reuse-nc contract (#57a lesson): a non-NULL nc is the caller's pre-clone and
    // MUST be the returned wq->spoor (a 0-element walk returns nc unchanged with
    // nqid == 0 -- the shape clone_walk_zero needs to cross the /hw/pci mount).
    // nc == NULL is the legacy direct-call shape (kernel tests).
    struct Spoor *cur;
    if (nc) {
        cur = nc;
        cur->qid = c->qid;
    } else {
        cur = spoor_clone(c);
        if (!cur) { walkqid_free(wq); return NULL; }
    }

    int n = 0;
    for (int i = 0; i < nname; i++) {
        struct Qid next;
        if (!walk_one(cur->qid.path, name[i], &next)) break;
        cur->qid = next;
        wq->qid[n++] = next;
    }

    wq->spoor = cur;
    wq->nqid  = n;
    return wq;
}

static int devpci_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;     // 9P stat not served (native fstat is, below)
}

// SYS_FSTAT surface. A directory (root or function): T_S_IFDIR | 0555. A ctl
// file: T_S_IFREG | 0444, size = the ctl line length. System-owned, world-r(/x)
// -- the visibility-not-authority posture (perm_enforced = false), matching devhw.
static int devpci_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || !out) return -1;
    for (size_t i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;

    u64 path = c->qid.path;
    if (pci_qid_is_ctl(path)) {
        char tmp[96];
        size_t len = build_ctl(pci_qid_index(path), tmp, sizeof(tmp));
        if (len == 0) return -1;   // a stale ctl qid (no such function)
        out->size     = (u64)len;
        out->mode     = T_S_IFREG | 0444u;
        out->nlink    = 1;
        out->qid_path = path;
        out->qid_type = QTFILE;
        out->blksize  = 4096;
        out->blocks   = (u64)((len + 511u) / 512u);
        out->uid      = PRINCIPAL_SYSTEM;
        out->gid      = GID_SYSTEM;
        return 0;
    }

    // A function directory must name a real function; the root always exists.
    if (pci_qid_is_dir(path) && !virtio_pci_dev_get(pci_qid_index(path)))
        return -1;
    out->mode     = T_S_IFDIR | 0555u;
    out->nlink    = 1;
    out->qid_path = path;
    out->qid_type = QTDIR;
    out->blksize  = 4096;
    out->uid      = PRINCIPAL_SYSTEM;
    out->gid      = GID_SYSTEM;
    return 0;
}

static struct Spoor *devpci_open(struct Spoor *c, int omode) {
    // Read-only by construction (write / create return -1 below); any omode is
    // accepted at open, the rights envelope is omode-derived at the syscall layer
    // (matches devhw / devramfs).
    return dev_simple_open(c, omode);
}

static struct Spoor *devpci_create(struct Spoor *c, const char *name, int omode,
                                   u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    return NULL;     // read-only inventory
}

static void devpci_close(struct Spoor *c) {
    dev_simple_close(c);
}

static long devpci_read(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n < 0 || off < 0) return -1;
    if (n == 0) return 0;

    u64 path = c->qid.path;
    // Directories don't byte-read (readdir is the path -- same as devhw/devctl).
    if (!pci_qid_is_ctl(path)) return -1;

    char tmp[96];
    size_t len = build_ctl(pci_qid_index(path), tmp, sizeof(tmp));
    if (len == 0) return -1;             // stale ctl qid (no such function)

    if ((u64)off >= len) return 0;       // EOF
    u64 avail = (u64)len - (u64)off;
    u64 copy  = avail > (u64)n ? (u64)n : avail;
    u8 *out = (u8 *)buf;
    for (u64 i = 0; i < copy; i++) out[i] = (u8)tmp[(u64)off + i];
    return (long)copy;
}

static struct Block *devpci_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devpci_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return -1;     // read-only; NO config-space write surface
}

static long devpci_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

// Emit one 9P2000.L dirent (the devramfs/devhw wire format the SYS_READDIR handler
// parses): qid(13) + offset(8 LE) + type(1) + name_len(2 LE) + name. `cookie` is
// the resume cursor handed back -- STRICTLY INCREASING and never 0 (the handler
// contract; 0 means end-of-directory). Returns the entry size, or 0 if it does
// not fit (whole entries only).
static long emit_dirent(u8 *out, long cap, long pos, u64 qpath, u8 qtype,
                        u64 cookie, const char *name, size_t nlen) {
    long entry = 24 + (long)nlen;
    if (pos + entry > cap) return 0;
    out[pos + 0] = qtype;
    out[pos + 1] = 0; out[pos + 2] = 0; out[pos + 3] = 0; out[pos + 4] = 0;
    for (int b = 0; b < 8; b++) out[pos + 5 + b]  = (u8)(qpath >> (8 * b));
    for (int b = 0; b < 8; b++) out[pos + 13 + b] = (u8)(cookie >> (8 * b));
    out[pos + 21] = qtype;
    out[pos + 22] = (u8)(nlen & 0xffu);
    out[pos + 23] = (u8)((nlen >> 8) & 0xffu);
    for (size_t i = 0; i < nlen; i++) out[pos + 24 + (long)i] = (u8)name[i];
    return entry;
}

// Enumerate a directory. The root lists one `<bus.dev.fn>` directory per function
// (the PciSource's read_dir target); a function directory lists its sole `ctl`
// leaf. The cookie is a 1-based ordinal (function index + 1 at the root; 1 for the
// single ctl), so it is strictly increasing + never 0.
static long devpci_readdir(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n <= 0) return 0;
    if (off < 0) return -1;     // an invalid resume cursor (symmetric with devpci_read)

    u64 path = c->qid.path;
    if (pci_qid_is_ctl(path)) return -1;   // a file does not enumerate

    u8 *out = (u8 *)buf;
    long pos = 0;

    if (pci_qid_is_root(path)) {
        int count = virtio_pci_dev_count();
        // off is the last-returned cookie (a 1-based function ordinal); resume at
        // function `off` (0-based) on the next call.
        for (int i = (int)off; i < count; i++) {
            char bdf[16];
            size_t nlen = format_bdf(i, bdf, sizeof(bdf));
            if (nlen == 0) continue;
            long e = emit_dirent(out, n, pos, pci_qid_dir(i), QTDIR,
                                 (u64)(i + 1), bdf, nlen);
            if (e == 0) {
                if (pos == 0) return -1;   // first entry too big for the buffer
                break;                     // resume next call
            }
            pos += e;
        }
        return pos;
    }

    // A function directory: a single `ctl` entry (cookie 1).
    if (pci_qid_is_dir(path)) {
        if (!virtio_pci_dev_get(pci_qid_index(path))) return -1;  // stale dir qid
        if (off >= 1) return 0;            // already past the single entry
        long e = emit_dirent(out, n, 0, pci_qid_ctl(pci_qid_index(path)), QTFILE,
                             1, "ctl", 3);
        if (e == 0) return -1;             // ctl entry too big for the buffer
        return e;
    }

    return -1;
}

static void devpci_remove(struct Spoor *c) {
    (void)c;
}

static int devpci_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devpci_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devpci = {
    .dc            = 'P',
    .name          = "pci",

    .perm_enforced = false,    // visibility, not authority -- the boundary is I-34
    .seekable      = false,    // sequential ctl reads + readdir cursor; no lseek

    .reset         = devpci_reset,
    .init          = devpci_init,
    .shutdown      = devpci_shutdown,

    .attach        = devpci_attach,
    .walk          = devpci_walk,
    .stat          = devpci_stat,
    .stat_native   = devpci_stat_native,

    .open          = devpci_open,
    .create        = devpci_create,
    .close         = devpci_close,

    .read          = devpci_read,
    .bread         = devpci_bread,
    .write         = devpci_write,
    .bwrite        = devpci_bwrite,
    .readdir       = devpci_readdir,

    .remove        = devpci_remove,
    .wstat         = devpci_wstat,
    .power         = devpci_power,
};
