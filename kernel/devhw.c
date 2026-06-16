// devhw — the DTB hardware inventory, published as a walkable tree (Menagerie).
//
// Per docs/MENAGERIE.md section 7 + ARCHITECTURE.md section 22.7. The one
// discovery SOURCE the kernel provides (every other source is a userspace
// driver): a synthetic Dev (Plan 9 `#H` shape, dc='H') that exposes the parsed
// flattened device tree as a navigable namespace. Each FDT node becomes a
// directory; each property a read-only file holding the property's raw bytes
// (big-endian, exactly as on the wire). The warden + userspace drivers WALK
// this tree to discover hardware -- the honest enforcement of I-15 ("the
// hardware view derives entirely from the DTB").
//
//   /hw                       -> the FDT root node (QTDIR)
//   /hw/<node>                -> a sub-node       (QTDIR)
//   /hw/<node>/<prop>         -> a property file  (QTFILE; raw bytes)
//
// This Dev is the NAMESPACE-MAPPING layer; the FDT format knowledge lives in
// lib/dtb.c (the dtb_node_at / dtb_node_iter / dtb_prop_at / dtb_node_parent
// tree-walk API). devhw is read-only (an inventory): create / write / rename /
// unlink all fail. It is `perm_enforced = false` -- visibility, not authority:
// reading the hardware layout is not a privileged act (a driver must see its
// device's reg/interrupts to bind), and the DTB is not secret. The PRIVILEGE
// boundary is the hardware ALLOWANCE (I-34, the next build-arc chunk): what
// gates actually MINTING a KObj_MMIO/IRQ/DMA handle over a discovered range --
// not what gates reading this tree.
//
// Qid encoding (a structure-block byte offset + a type bit):
//   bit 63 = 0 -> a NODE directory; low bits = the node's BEGIN_NODE offset.
//                 The root node is offset 0, so the root qid.path is 0 (the
//                 conventional Dev-root path -- dev_simple_attach gives it).
//   bit 63 = 1 -> a PROPERTY file;  low bits = the property's FDT_PROP offset.
// Both offsets are < size_struct (< 4 GiB; realistically < 1 MiB), so the
// 63-bit field never overflows. The offsets are stable for the kernel's
// lifetime (the relocated DTB buffer is immutable).

#include <thylacine/dev.h>
#include <thylacine/dtb.h>
#include <thylacine/proc.h>      // PRINCIPAL_SYSTEM / GID_SYSTEM
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>   // struct t_stat + T_S_IFDIR / T_S_IFREG
#include <thylacine/types.h>

// =============================================================================
// Qid encoding.
// =============================================================================

#define HW_PROP_BIT  (1ULL << 63)
#define HW_OFF_MASK  (HW_PROP_BIT - 1ULL)   // low 63 bits

static inline bool hw_qid_is_prop(u64 path) { return (path & HW_PROP_BIT) != 0; }
static inline u32  hw_qid_off(u64 path)     { return (u32)(path & HW_OFF_MASK); }
static inline u64  hw_qid_for_node(u32 off) { return (u64)off; }
static inline u64  hw_qid_for_prop(u32 off) { return HW_PROP_BIT | (u64)off; }

// The synthetic /hw/pci mount-point child (Menagerie 6b). devpci mounts over it
// (kernel/joey.c). Bit 62 marks it -- distinct from every FDT node/prop offset
// (those are < 2^31) -- while bit 63 stays 0 so hw_qid_is_prop() reports false
// (it is a directory, never byte-read as a property). Special-cased in walk /
// stat / readdir BEFORE the FDT-offset logic so its qid is never decoded as an
// FDT offset.
#define HW_SYNTH_BIT  (1ULL << 62)
#define HW_SYNTH_PCI  (HW_SYNTH_BIT | 1ULL)
static inline bool hw_qid_is_synth(u64 path) { return (path & HW_SYNTH_BIT) != 0; }

// =============================================================================
// Walk.
// =============================================================================

static bool name_eq(const char *a, const char *b) {
    int j = 0;
    while (a[j] && b[j] && a[j] == b[j]) j++;
    return a[j] == 0 && b[j] == 0;
}

// Single-step walk dispatch. `cur` is the current Spoor's qid; fills *out and
// returns true on success, false on a miss. (`.` / `..` normally never reach a
// Dev -- stalk resolves them against the trail -- but devhw handles them for
// the direct-call path + robustness, matching devdev / devramfs.)
static bool walk_one(struct Qid cur, const char *name, struct Qid *out) {
    out->path = 0; out->vers = 0; out->type = 0;
    out->pad[0] = out->pad[1] = out->pad[2] = 0;

    // The synthetic /hw/pci mount-point (Menagerie 6b): an empty directory until
    // devpci mounts over it. `.` -> self; `..` -> the FDT root; no real children
    // (devpci serves /hw/pci/<bdf> via the mount-cross). Handled BEFORE the FDT
    // decode so its sentinel qid is never passed to dtb_node_iter as an offset.
    if (hw_qid_is_synth(cur.path)) {
        if (name[0] == '.' && name[1] == '\0') { *out = cur; return true; }
        if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
            out->path = hw_qid_for_node(0);    // parent is the FDT root
            out->type = QTDIR;
            return true;
        }
        return false;
    }

    // A property file is a leaf -- no walk descends from it.
    if (hw_qid_is_prop(cur.path)) return false;
    u32 node_off = hw_qid_off(cur.path);

    if (name[0] == '.' && name[1] == '\0') { *out = cur; return true; }

    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        u32 parent;
        if (!dtb_node_parent(node_off, &parent)) return false;
        out->path = hw_qid_for_node(parent);
        out->type = QTDIR;
        return true;
    }

    // The synthetic /hw/pci mount-point child (Menagerie 6b) -- only at the FDT
    // root. devpci mounts over it. No real FDT node is named "pci" (QEMU-virt /
    // RPi expose "pcie@..."), so this shadows nothing.
    if (node_off == 0 && name_eq("pci", name)) {
        out->path = HW_SYNTH_PCI;
        out->type = QTDIR;
        return true;
    }

    // Scan the node's direct contents (sub-nodes + properties) for `name`.
    u32 cursor = 0;
    struct dtb_node_entry e;
    while (dtb_node_iter(node_off, &cursor, &e)) {
        if (name_eq(e.name, name)) {
            if (e.is_node) { out->path = hw_qid_for_node(e.off); out->type = QTDIR; }
            else           { out->path = hw_qid_for_prop(e.off); out->type = QTFILE; }
            return true;
        }
    }
    return false;
}

// =============================================================================
// Vtable.
// =============================================================================

static void devhw_reset(void)    { /* no-op */ }
static void devhw_init(void)     { /* no-op */ }
static void devhw_shutdown(void) { /* no-op */ }

static struct Spoor *devhw_attach(const char *spec) {
    (void)spec;
    // qid.path = 0 = DTB_NODE_ROOT (the FDT root node). dev_simple_attach sets
    // path 0 + QTDIR; the tree is reachable even if dtb_init never ran (every
    // op then degrades to an empty/absent node -- attach still yields a root).
    return dev_simple_attach(&devhw, QTDIR);
}

static struct Walkqid *devhw_walk(struct Spoor *c, struct Spoor *nc,
                                  const char **name, int nname) {
    if (!c) return NULL;
    if (nname < 0) return NULL;

    struct Walkqid *wq = walkqid_alloc(nname);
    if (!wq) return NULL;

    // Reuse-nc contract (#57a lesson): a non-NULL nc is the caller's pre-clone
    // and MUST be the returned wq->spoor (a 0-element walk returns nc unchanged
    // with nqid == 0 -- the shape clone_walk_zero needs to cross the /hw mount).
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
        if (!walk_one(cur->qid, name[i], &next)) break;
        cur->qid = next;
        wq->qid[n++] = next;
    }

    wq->spoor = cur;
    wq->nqid  = n;
    return wq;
}

static int devhw_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;     // 9P stat not served (native fstat is, below)
}

// SYS_FSTAT surface. A node directory: T_S_IFDIR | 0555, size 0. A property
// file: T_S_IFREG | 0444, size = the property length. System-owned, world-
// r(/x) -- the visibility-not-authority posture (perm_enforced = false).
static int devhw_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || !out) return -1;
    for (size_t i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;

    // The synthetic /hw/pci mount-point (Menagerie 6b): a directory.
    if (hw_qid_is_synth(c->qid.path)) {
        out->mode     = T_S_IFDIR | 0555u;
        out->nlink    = 1;
        out->qid_path = c->qid.path;
        out->qid_type = QTDIR;
        out->blksize  = 4096;
        out->uid      = PRINCIPAL_SYSTEM;
        out->gid      = GID_SYSTEM;
        return 0;
    }

    if (hw_qid_is_prop(c->qid.path)) {
        u32 prop_off = hw_qid_off(c->qid.path);
        u32 len;
        if (!dtb_prop_at(prop_off, NULL, NULL, &len)) return -1;
        out->size     = (u64)len;
        out->mode     = T_S_IFREG | 0444u;
        out->nlink    = 1;
        out->qid_path = c->qid.path;
        out->qid_type = QTFILE;
        out->blksize  = 4096;
        out->blocks   = (u64)((len + 511u) / 512u);
        out->uid      = PRINCIPAL_SYSTEM;
        out->gid      = GID_SYSTEM;
        return 0;
    }

    u32 node_off = hw_qid_off(c->qid.path);
    if (!dtb_node_at(node_off, NULL, NULL)) return -1;
    out->mode     = T_S_IFDIR | 0555u;
    out->nlink    = 1;
    out->qid_path = c->qid.path;
    out->qid_type = QTDIR;
    out->blksize  = 4096;
    out->uid      = PRINCIPAL_SYSTEM;
    out->gid      = GID_SYSTEM;
    return 0;
}

static struct Spoor *devhw_open(struct Spoor *c, int omode) {
    // Read-only by construction (write / create return -1 below), so any
    // omode is accepted at open; the rights envelope is omode-derived at the
    // syscall layer. (Matches devramfs.)
    return dev_simple_open(c, omode);
}

static struct Spoor *devhw_create(struct Spoor *c, const char *name, int omode,
                                  u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    return NULL;     // read-only inventory
}

static void devhw_close(struct Spoor *c) {
    dev_simple_close(c);
}

static long devhw_read(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n < 0 || off < 0) return -1;
    if (n == 0) return 0;

    // Directories don't byte-read (readdir held -- same as devramfs/devctl).
    if (!hw_qid_is_prop(c->qid.path)) return -1;

    u32 prop_off = hw_qid_off(c->qid.path);
    const u8 *data;
    u32 len;
    if (!dtb_prop_at(prop_off, NULL, &data, &len)) return -1;

    if ((u64)off >= len) return 0;            // EOF
    u64 avail = (u64)len - (u64)off;
    u64 copy  = avail > (u64)n ? (u64)n : avail;
    u8 *out = (u8 *)buf;
    for (u64 i = 0; i < copy; i++) out[i] = data[(u64)off + i];
    return (long)copy;
}

static struct Block *devhw_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devhw_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return -1;     // read-only
}

static long devhw_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

// Enumerate a node directory's direct contents (sub-nodes + properties) as
// Thylacine 9P2000.L dirents (the devramfs wire format the SYS_READDIR handler
// parses): qid(13) + offset(8 LE) + type(1) + name_len(2 LE) + name.
//
// The `offset` cookie is the dtb_node_iter resume cursor -- a structure-block
// byte offset, STRICTLY INCREASING across the enumeration and never 0 (the
// node body starts past the node's own BEGIN_NODE token). That exactly meets
// the handler's load-bearing cookie contract (strictly monotonic; never 0 so
// the first call's offset-0 resume is unambiguous). `off` (0 to start, else
// the last cookie handed out) selects where to resume. Whole entries only;
// returns -1 if the FIRST entry of a run does not fit `n` (per the ABI, 0
// means end-of-directory, so a too-small buffer must NOT report 0).
static long devhw_readdir(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n <= 0) return 0;

    // The synthetic /hw/pci mount-point is an empty directory pre-mount (devpci
    // serves it post-mount via the cross). EOD.
    if (hw_qid_is_synth(c->qid.path)) return 0;

    // Only a node directory enumerates; a property file does not.
    if (hw_qid_is_prop(c->qid.path)) return -1;
    u32 node_off = hw_qid_off(c->qid.path);
    if (!dtb_node_at(node_off, NULL, NULL)) return -1;   // invalid / dtb absent

    u8 *out = (u8 *)buf;
    long cap = n;
    long pos = 0;

    u32 cursor = (off <= 0) ? 0u : (u32)off;
    for (;;) {
        struct dtb_node_entry e;
        u32 next_cursor = cursor;
        if (!dtb_node_iter(node_off, &next_cursor, &e)) break;   // EOD

        u64 nlen = e.namelen;
        // Skip a nameless entry (the root carries name "" but is never a child;
        // defensive). The cursor still advances, so no spin.
        if (nlen == 0 || nlen > 0xffffu) { cursor = next_cursor; continue; }

        u64 qpath = e.is_node ? hw_qid_for_node(e.off) : hw_qid_for_prop(e.off);
        u8  qtype = e.is_node ? QTDIR : QTFILE;

        long entry = 24 + (long)nlen;
        if (pos + entry > cap) {
            if (pos == 0) return -1;        // first entry too big for the buffer
            break;                          // a later entry -> resume next call
        }

        // qid(13): type(1) + version(4 LE, 0) + path(8 LE).
        out[pos + 0] = qtype;
        out[pos + 1] = 0; out[pos + 2] = 0; out[pos + 3] = 0; out[pos + 4] = 0;
        for (int b = 0; b < 8; b++) out[pos + 5 + b]  = (u8)(qpath >> (8 * b));
        // offset(8 LE): the resume cookie = the cursor AFTER this entry.
        for (int b = 0; b < 8; b++) out[pos + 13 + b] = (u8)((u64)next_cursor >> (8 * b));
        // type(1): mirror the qid type bits.
        out[pos + 21] = qtype;
        // name_len(2 LE).
        out[pos + 22] = (u8)(nlen & 0xffu);
        out[pos + 23] = (u8)((nlen >> 8) & 0xffu);
        // name.
        for (u64 i = 0; i < nlen; i++) out[pos + 24 + (long)i] = (u8)e.name[i];

        pos += entry;
        cursor = next_cursor;
    }
    return pos;     // 0 iff start was already at EOD
}

static void devhw_remove(struct Spoor *c) {
    (void)c;
}

static int devhw_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devhw_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devhw = {
    .dc            = 'H',
    .name          = "hw",

    .perm_enforced = false,    // visibility, not authority -- world-readable HW inventory
    // Non-seekable: SYS_LSEEK is rejected. Sequential property reads still work
    // (sys_read tracks + advances c->offset), and -- load-bearing -- a directory
    // fd's readdir cursor (a raw structure-block byte offset in c->offset)
    // cannot be lseek'd to a mid-token position and made to misparse. An
    // inventory/control Dev does not need explicit seek (matches devproc/devctl).
    .seekable      = false,

    .reset         = devhw_reset,
    .init          = devhw_init,
    .shutdown      = devhw_shutdown,

    .attach        = devhw_attach,
    .walk          = devhw_walk,
    .stat          = devhw_stat,
    .stat_native   = devhw_stat_native,

    .open          = devhw_open,
    .create        = devhw_create,
    .close         = devhw_close,

    .read          = devhw_read,
    .bread         = devhw_bread,
    .write         = devhw_write,
    .bwrite        = devhw_bwrite,
    .readdir       = devhw_readdir,

    .remove        = devhw_remove,
    .wstat         = devhw_wstat,
    .power         = devhw_power,
};
