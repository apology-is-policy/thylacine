// /ctl Dev tests (P4-D).
//
// Covers registration, walks, per-leaf reads, write rejection.

#include "test.h"


#include <thylacine/addrspace.h>  // prowl-6: the counters the TABLES column reads
#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/errno.h>
#include <thylacine/exec.h>      // EXEC_USER_BURROW_BASE
#include <thylacine/handle.h>
#include <thylacine/image.h>     // image_cache_evict_idle_for_test
#include <thylacine/territory.h>
#include <thylacine/vivarium.h>  // VIV_PROT_READ
#include <thylacine/vma.h>
#include <thylacine/page.h>       // prowl-6: PAGE_SIZE
#include <thylacine/proc.h>
#include <thylacine/sched.h>     // V-4c-2b: sched_cpu_ctxt
#include <thylacine/smp.h>       // V-4c-2b: smp_cpu_count
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>   // #210: a live conn for the 9p-sessions read
#include <thylacine/syscall.h>   // V-4b-5: struct t_stat + T_S_IF*
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/gic.h"      // V-4c-2b: gic_cpu_irq_count
#include "../../arch/arm64/fault.h"    // prowl-6: userland_demand_page
#include "../../arch/arm64/hwfeat.h"   // V-4c-2b: hw_cpu_ident
#include "../../mm/slub.h"     // kfree_large_watch: the snapshot's page is freed
#include <thylacine/devctl.h>

// =============================================================================
// Helpers.
// =============================================================================

static bool contains(const char *haystack, size_t hlen, const char *needle) {
    size_t nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0) return true;
    if (nlen > hlen) return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && haystack[i + j] == needle[j]) j++;
        if (j == nlen) return true;
    }
    return false;
}

static bool ends_with(const char *buf, size_t n, const char *s) {
    size_t m = 0;
    while (s[m]) m++;
    if (m > n) return false;
    for (size_t i = 0; i < m; i++) {
        if (buf[n - m + i] != s[i]) return false;
    }
    return true;
}

// The line that begins with `head`, and its length without the newline.
static const char *line_of(const char *buf, size_t n, const char *head, size_t *len) {
    size_t h = 0;
    while (head[h]) h++;
    for (size_t i = 0; i + h <= n; i++) {
        if (i && buf[i - 1] != '\n') continue;
        size_t j = 0;
        while (j < h && buf[i + j] == head[j]) j++;
        if (j < h) continue;
        size_t e = i;
        while (e < n && buf[e] != '\n') e++;
        *len = e - i;
        return buf + i;
    }
    return NULL;
}

static struct Spoor *walk_one(struct Spoor *c, const char *name) {
    const char *names[1] = { name };
    struct Walkqid *wq = devctl.walk(c, NULL, names, 1);
    if (!wq) return NULL;
    if (wq->nqid != 1) {
        spoor_unref(wq->spoor);
        walkqid_free(wq);
        return NULL;
    }
    struct Spoor *r = wq->spoor;
    walkqid_free(wq);
    return r;
}

// Open /ctl/<name>; caller spoor_clunk's the result.
static struct Spoor *open_ctl_leaf(const char *name) {
    struct Spoor *root = devctl.attach("");
    if (!root) return NULL;
    struct Spoor *leaf = walk_one(root, name);
    spoor_unref(root);
    if (!leaf) return NULL;
    if (!devctl.open(leaf, 0)) {
        spoor_unref(leaf);
        return NULL;
    }
    return leaf;
}

// =============================================================================
// Tests.
// =============================================================================

void test_devctl_bestiary_smoke(void) {
    TEST_EXPECT_EQ(dev_lookup_by_dc('C'),       &devctl, "lookup 'C' = devctl");
    TEST_EXPECT_EQ(dev_lookup_by_name("ctl"),   &devctl, "lookup 'ctl' = devctl");
    TEST_EXPECT_EQ(devctl.dc, 'C',                       "devctl.dc = 'C'");
}

void test_devctl_attach_returns_dir(void) {
    struct Spoor *c = devctl.attach("");
    TEST_ASSERT(c != NULL, "attach OK");
    TEST_EXPECT_EQ(c->qid.path, (u64)0, "root qid.path = 0");
    TEST_EXPECT_EQ(c->qid.type, QTDIR, "root QTDIR");
    spoor_unref(c);
}

void test_devctl_walk_to_each_leaf(void) {
    // Mirrors g_ctl_leaves in devctl.c. This list is hand-kept, so a new leaf
    // that is not added here is simply not walk-covered -- add yours.
    static const char *leaf_names[] = {
        "procs", "memory", "devices", "kernel-base", "sched", "cpu", "cons",
        "9p-sessions",
    };
    for (size_t i = 0; i < sizeof(leaf_names) / sizeof(leaf_names[0]); i++) {
        struct Spoor *root = devctl.attach("");
        struct Spoor *leaf = walk_one(root, leaf_names[i]);
        spoor_unref(root);
        TEST_ASSERT(leaf != NULL, "walk to leaf succeeds");
        TEST_EXPECT_EQ(leaf->qid.type, QTFILE, "leaf is QTFILE");
        TEST_ASSERT(leaf->qid.path != 0, "leaf path != root");
        spoor_unref(leaf);
    }
}

void test_devctl_walk_unknown_misses(void) {
    struct Spoor *root = devctl.attach("");
    const char *names[1] = { "does-not-exist" };
    struct Walkqid *wq = devctl.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL, "walk allocates");
    TEST_EXPECT_EQ(wq->nqid, 0, "walk to unknown leaf misses");
    spoor_unref(wq->spoor);
    walkqid_free(wq);
    spoor_unref(root);
}

void test_devctl_read_procs_format(void) {
    struct Spoor *c = open_ctl_leaf("procs");
    TEST_ASSERT(c != NULL, "open /ctl/procs");

    char buf[512];
    long got = devctl.read(c, buf, 512, 0);
    TEST_ASSERT(got > 0, "procs read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "PID"),     "header has PID column");
    TEST_ASSERT(contains(buf, (size_t)got, "PPID"),    "prowl-4: header has the PPID (tree) column");
    TEST_ASSERT(contains(buf, (size_t)got, "TABLES"),  "prowl-6: header has the TABLES (page-table) column");
    TEST_ASSERT(contains(buf, (size_t)got, "PAGES    TABLES    CHILDREN"),
                "prowl-6: TABLES sits between PAGES and CHILDREN (the consumers parse by position)");
    TEST_ASSERT(contains(buf, (size_t)got, "STATE"),   "header has STATE");
    TEST_ASSERT(contains(buf, (size_t)got, "ALIVE"),   "kproc shows ALIVE");

    spoor_clunk(c);
}

// prowl-3b: /ctl/cpu -- the per-CPU meter denominator (cpus + per-CPU idle_ns +
// capacity). World-readable; idle_ns, ctxt and intr read "-" to a reader that is
// neither the system principal nor a hostowner (devctl.counters_gated).
void test_devctl_read_cpu_format(void) {
    struct Spoor *c = open_ctl_leaf("cpu");
    TEST_ASSERT(c != NULL, "open /ctl/cpu");

    char buf[1024];
    long got = devctl.read(c, buf, sizeof buf, 0);
    TEST_ASSERT(got > 0, "cpu read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "cpus:"),    "has the cpus: count");
    TEST_ASSERT(contains(buf, (size_t)got, "idle_ns"),  "has the idle_ns column");
    TEST_ASSERT(contains(buf, (size_t)got, "capacity"), "has the capacity column");

    // V-4c-2b (VIVARIUM section 6.17): the diorama's /proc/stat + /proc/cpuinfo
    // sources. The header names them, and the hwcap line is a two-token line
    // (so prowl's three-token row parse skips it, exactly as it skips "cpus:").
    TEST_ASSERT(contains(buf, (size_t)got, "hwcap:"),    "V-4c-2b: has the hwcap line");
    TEST_ASSERT(contains(buf, (size_t)got, "ctxt"),      "V-4c-2b: has the ctxt column");
    TEST_ASSERT(contains(buf, (size_t)got, "intr"),      "V-4c-2b: has the intr column");
    TEST_ASSERT(contains(buf, (size_t)got, "cacheline"), "V-4c-2b: has the cacheline column");
    TEST_ASSERT(contains(buf, (size_t)got, "midr"),      "V-4c-2b: has the midr column");

    spoor_clunk(c);
}

// V-4c-2b (docs/VIVARIUM.md section 6.17): the four per-CPU kernel sources the
// diorama needs, checked at the source rather than through the text -- a column
// that renders but reports nothing is the failure this catches. Each value is
// asserted for the property the diorama depends on, not merely for presence.
void test_devctl_cpu_sources_live(void) {
    // ctxt: the per-CPU context-switch count ADVANCES. Same forced-yield vehicle
    // as prowl's per-thread nsched test -- a yield switches this thread out and
    // back in, so the CPU that runs us must have counted switches. Summed over
    // CPUs because a work-steal can land the resume on a different CPU than the
    // one we started on, which would make a single-CPU delta legitimately zero.
    u64 ctxt0 = 0;
    for (unsigned i = 0; i < smp_cpu_count(); i++) ctxt0 += sched_cpu_ctxt(i);
    for (int i = 0; i < 8; i++) {
        for (volatile int j = 0; j < 500000; j++) { /* burn a measurable slice */ }
        sched();
    }
    u64 ctxt1 = 0;
    for (unsigned i = 0; i < smp_cpu_count(); i++) ctxt1 += sched_cpu_ctxt(i);
    TEST_ASSERT(ctxt1 > ctxt0, "V-4c-2b: per-CPU ctxt advances across forced yields");

    // intr: counted at gic_dispatch, the universal entry -- so the timer PPI
    // alone guarantees a nonzero count by the time the test phase runs. This is
    // exactly what distinguishes it from kobj_irq_total_fires, which counts only
    // the userspace-driver-forwarded subset and can legitimately still be 0 here.
    u64 intr = 0;
    for (unsigned i = 0; i < smp_cpu_count(); i++) intr += gic_cpu_irq_count(i);
    TEST_ASSERT(intr > 0, "V-4c-2b: per-CPU intr counts timer/UART, not just forwarded IRQs");

    // The boot CPU always records an identity (per_cpu_main does the same for
    // each secondary; a PSCI-failed CPU legitimately has none, hence the guard).
    const struct hw_cpu_ident *id = hw_cpu_ident(0);
    TEST_ASSERT(id != NULL, "V-4c-2b: CPU 0 recorded a hardware identity");

    // cacheline: CTR_EL0.DminLine decoded to bytes. ARM ARM bounds DminLine to
    // 4 bits, so the decode (4 << n) lands in [4, 32768]; every real part is a
    // power of two of at least 16 bytes, which is what a consumer sizing an
    // allocation off /sys/.../coherency_line_size relies on.
    TEST_ASSERT(id->dcache_line >= 16 && id->dcache_line <= 2048,
                "V-4c-2b: dcache line size is architecturally sane");
    TEST_ASSERT((id->dcache_line & (id->dcache_line - 1)) == 0,
                "V-4c-2b: dcache line size is a power of two");

    // midr: the implementer field (bits 31:24) is never 0 on a real part -- 0 is
    // reserved -- so a zero here means the register was never read, which is the
    // exact failure a boot-CPU-only or never-called detect would produce.
    // midr: the discriminator has to be a property that is true of EVERY part we
    // can run on, not one that merely looks diagnostic. "implementer != 0" fails
    // that test and was WRONG: QEMU's TCG `-cpu max` reports 0x000f0510, whose
    // implementer IS 0x00 -- it deliberately does not claim to be an
    // ARM-implemented part, and the interactive harness runs exactly that CPU by
    // default. What actually distinguishes a read register from an unread one:
    // an unread slot is BSS zero, while ARMv8 REQUIRES MIDR.Architecture (19:16)
    // to read 0xF ("use the ID registers"), so a real part can never be all-zero.
    TEST_ASSERT(id->midr != 0, "V-4c-2b: MIDR was actually read (unread reads 0)");
    TEST_ASSERT(((id->midr >> 16) & 0xFu) == 0xFu,
                "V-4c-2b: MIDR.Architecture is the ARMv8 0xF sentinel");
}

// The /ctl/procs row whose first token is `pid`: its tokens as unsigned values
// in out[0..max) (a token that is not a number reads 0). Returns the token
// count, or 0 when no row starts with that pid.
static int procs_row_of(const char *buf, size_t len, int pid, unsigned long *out, int max) {
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && buf[e] != '\n') e++;
        int n = 0;
        bool mine = false;
        size_t k = i;
        while (k < e) {
            while (k < e && buf[k] == ' ') k++;
            if (k >= e) break;
            size_t t = k;
            while (k < e && buf[k] != ' ') k++;
            unsigned long v = 0;
            bool num = true;
            for (size_t q = t; q < k; q++) {
                if (buf[q] < '0' || buf[q] > '9') { num = false; break; }
                v = v * 10u + (unsigned long)(buf[q] - '0');
            }
            if (n == 0) {
                mine = num && v == (unsigned long)pid;
                if (!mine) break;
            }
            if (n < max) out[n] = num ? v : 0ul;
            n++;
        }
        if (mine) return n;
        i = e + 1;
    }
    return 0;
}

// prowl-6: the probe child's report -- filled by the child on its own Proc,
// read by the parent after the reap (the pgrp tests' idiom). /ctl/procs walks
// the TREE from kproc, so the probe must be a real child of the test's Proc: an
// orphan from proc_alloc is never listed.
struct tbl_report {
    s64           reserve;
    int           handled;
    u32           tables, pages;
    long          got;
    int           ntok;
    unsigned long tok5, tok6;
};
static struct tbl_report g_tbl;

static void procs_tables_thunk(void *arg) {
    (void)arg;
    struct Proc *self = current_thread()->proc;
    g_tbl.reserve = sys_burrow_reserve_for_proc(self, PAGE_SIZE,
                                                (u64)(BURROW_PROT_READ | BURROW_PROT_WRITE), 0);
    if (g_tbl.reserve > 0) {
        struct fault_info fi = { 0 };
        fi.vaddr          = (u64)g_tbl.reserve;
        fi.ec             = 0x24;          // EC_DATA_ABORT_LOWER
        fi.fsc            = 0x07;          // FSC_TRANS_FAULT_L3
        fi.fault_level    = 3;
        fi.from_user      = true;
        fi.is_write       = true;
        fi.is_translation = true;
        g_tbl.handled = (userland_demand_page(self, &fi) == FAULT_HANDLED);
    }
    g_tbl.tables = self->as ? __atomic_load_n(&self->as->pgtable_pages, __ATOMIC_ACQUIRE) : 0u;
    g_tbl.pages  = self->as ? __atomic_load_n(&self->as->page_count, __ATOMIC_ACQUIRE) : 0u;

    static char buf[2048];
    struct Spoor *c = open_ctl_leaf("procs");
    if (c) {
        g_tbl.got = devctl.read(c, buf, sizeof buf, 0);
        spoor_clunk(c);
        unsigned long f[9] = { 0 };
        if (g_tbl.got > 0) g_tbl.ntok = procs_row_of(buf, (size_t)g_tbl.got, self->pid, f, 9);
        g_tbl.tok5 = f[5];
        g_tbl.tok6 = f[6];
    }
    exits("ok");
}

// prowl-6: TABLES is the address space's page-table count and PAGES the holder
// count that contains it. A child of the test's Proc reserves one lazy page and
// touches it: the touch costs the page and the three tables of a fresh path (a
// one-page map is an inline leaf, so no node -- PAGEMAP_INLINE_MAX), and its
// own row reads the two counters back as the kernel holds them, 4 and 3. A
// renderer that printed page_count twice would read 4 4 here. The child exits
// and is reaped BEFORE the assertions, so a red leg leaves nothing behind.
void test_devctl_procs_tables_column(void) {
    g_tbl = (struct tbl_report){ 0 };
    int pid = rfork(RFPROC, procs_tables_thunk, NULL);
    int st = -1;
    int reaped = (pid > 0) ? wait_pid_for(pid, 0, &st) : -1;

    TEST_ASSERT(pid > 0, "rfork the probe child under the test's Proc");
    TEST_ASSERT(reaped == pid, "reap the probe child");
    TEST_ASSERT(g_tbl.reserve > 0, "one lazily reserved page");
    TEST_ASSERT(g_tbl.handled, "the write touch is handled");
    TEST_EXPECT_EQ(g_tbl.tables, 3u, "one touch costs its L1, L2 and L3");
    TEST_EXPECT_EQ(g_tbl.pages, 4u, "the holder count is the page plus its three tables (an inline leaf charges no node)");
    TEST_ASSERT(g_tbl.got > 0, "procs read positive (from the child)");
    TEST_EXPECT_EQ((u32)g_tbl.ntok, 9u, "the probe's row has nine columns (PID PPID NAME STATE THREADS PAGES TABLES CHILDREN CPU_NS)");
    TEST_EXPECT_EQ((u32)g_tbl.tok5, g_tbl.pages,  "PAGES is the holder count");
    TEST_EXPECT_EQ((u32)g_tbl.tok6, g_tbl.tables, "TABLES is the page-table count, not PAGES again");
}

void test_devctl_read_memory_format(void) {
    struct Spoor *c = open_ctl_leaf("memory");
    TEST_ASSERT(c != NULL, "open /ctl/memory");

    char buf[512];
    long got = devctl.read(c, buf, 512, 0);
    TEST_ASSERT(got > 0, "memory read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "total:"),    "has total:");
    TEST_ASSERT(contains(buf, (size_t)got, "free:"),     "has free:");
    TEST_ASSERT(contains(buf, (size_t)got, "reserved:"), "has reserved:");
    TEST_ASSERT(contains(buf, (size_t)got, "pages"),     "uses page units");
    TEST_ASSERT(contains(buf, (size_t)got, "ledger:"),   "has ledger:");
    TEST_ASSERT(contains(buf, (size_t)got, "unpaired: 0\n"), "no pairing has broken");

    spoor_clunk(c);
}

void test_devctl_read_devices_format(void) {
    struct Spoor *c = open_ctl_leaf("devices");
    TEST_ASSERT(c != NULL, "open /ctl/devices");

    char buf[256];
    long got = devctl.read(c, buf, 256, 0);
    TEST_ASSERT(got > 0, "devices read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "DC"),     "header has DC column");
    TEST_ASSERT(contains(buf, (size_t)got, "NAME"),   "header has NAME column");
    TEST_ASSERT(contains(buf, (size_t)got, "none"),   "lists devnone");
    TEST_ASSERT(contains(buf, (size_t)got, "cons"),   "lists devcons");
    TEST_ASSERT(contains(buf, (size_t)got, "ctl"),    "lists devctl itself");
    TEST_ASSERT(contains(buf, (size_t)got, "proc"),   "lists devproc");

    spoor_clunk(c);
}

void test_devctl_read_kernel_base_format(void) {
    // #57a F1: /ctl/kernel-base is CAP_HOSTOWNER-gated (the KASLR slide, an
    // I-16 secret; CAP_HOSTOWNER is elevation-only -- not even kproc holds it
    // by default). Temporarily elevate the in-kernel test thread to exercise
    // the format through the REAL gated read path (an elevated admin reading
    // the slide). Restore BEFORE the content asserts so a failing assert can
    // never leave kproc elevated. The deny path is test_devctl_kernel_base_gated.
    struct Thread *t = current_thread();
    u64 saved = __atomic_load_n(&t->proc->caps, __ATOMIC_ACQUIRE);

    // The unelevated read first: the gate as wired, and its refusal's value.
    struct Spoor *d = open_ctl_leaf("kernel-base");
    char dbuf[256];
    long denied = d ? devctl.read(d, dbuf, sizeof dbuf, 0) : 0;
    if (d) spoor_clunk(d);
    TEST_EXPECT_EQ(denied, (long)-T_E_ACCES,
                   "an unprivileged read of /ctl/kernel-base is refused (EACCES)");

    __atomic_store_n(&t->proc->caps, saved | CAP_HOSTOWNER, __ATOMIC_RELEASE);

    struct Spoor *c = open_ctl_leaf("kernel-base");
    char buf[256];
    long got = c ? devctl.read(c, buf, 256, 0) : -1;

    __atomic_store_n(&t->proc->caps, saved, __ATOMIC_RELEASE);  // restore first

    TEST_ASSERT(c != NULL, "open /ctl/kernel-base");
    TEST_ASSERT(got > 0, "kernel-base read positive (elevated)");
    TEST_ASSERT(contains(buf, (size_t)got, "kernel_base:"),  "has kernel_base:");
    TEST_ASSERT(contains(buf, (size_t)got, "kaslr_offset:"), "has kaslr_offset:");
    TEST_ASSERT(contains(buf, (size_t)got, "seed_source:"),  "has seed_source:");
    TEST_ASSERT(contains(buf, (size_t)got, "0x"),            "uses 0x hex prefix");

    if (c) spoor_clunk(c);
}

// #57a F1: /ctl/kernel-base discloses the live KASLR slide (I-16). Now that
// /ctl is world-reachable, that ONE leaf is gated on CAP_HOSTOWNER -- an
// unprivileged caller (a logged-in user, stripped of the elevation-only caps
// at rfork) cannot read it and defeat KASLR. The predicate is leaf-specific;
// the other leaves stay world-readable, with IMPERIUM 11.3 item 10's field gates.
// (The format test above passes only because it temporarily elevates the test
// thread to CAP_HOSTOWNER; kproc's CAP_ALL does NOT include the elevation-only
// CAP_HOSTOWNER -- caps.h pins CAP_ALL & CAP_ELEVATION_ONLY == 0.)
void test_devctl_kernel_base_gated(void) {

    struct Proc admin, user;
    for (size_t i = 0; i < sizeof(admin); i++) ((u8 *)&admin)[i] = 0;
    for (size_t i = 0; i < sizeof(user);  i++) ((u8 *)&user)[i]  = 0;
    admin.caps = CAP_HOSTOWNER;
    user.caps  = CAP_NONE;

    TEST_ASSERT(devctl_kernel_base_readable(&admin),
                "CAP_HOSTOWNER reads /ctl/kernel-base");
    TEST_ASSERT(!devctl_kernel_base_readable(&user),
                "F1: an unprivileged caller is denied the KASLR slide");
    TEST_ASSERT(!devctl_kernel_base_readable(NULL),
                "NULL caller denied");
}

// ARCH 8.12 audit F2 REGRESSION. /ctl/kstack is CAP_HOSTOWNER-gated for a
// different reason than kernel-base: it discloses no address, but its
// formatter walks every live Proc under g_proc_table_lock WITH IRQS MASKED
// and scans each thread's 16 KiB stack, recomputed on EVERY read. Left
// world-readable it is an unprivileged masked-window lever (I-32).
//
// THE DENY LEG IS THE POINT and it must go through the REAL read path: the
// predicate is shared with kernel-base, so a predicate-only test would pass
// whether or not the gate is WIRED for this kind. Pre-fix the unelevated read
// returns a positive count; post-fix it returns -T_E_ACCES (a refusal, ERRORS.md).
void test_devctl_kstack_gated(void) {
    struct Thread *t = current_thread();
    u64 saved = __atomic_load_n(&t->proc->caps, __ATOMIC_ACQUIRE);

    // DENY: kproc's CAP_ALL excludes the elevation-only CAP_HOSTOWNER.
    struct Spoor *c = open_ctl_leaf("kstack");
    char buf[512];
    long denied = c ? devctl.read(c, buf, sizeof buf, 0) : 0;
    if (c) spoor_clunk(c);

    // ALLOW: the same read, elevated. Restore BEFORE the asserts so a failing
    // assert can never leave kproc holding CAP_HOSTOWNER.
    __atomic_store_n(&t->proc->caps, saved | CAP_HOSTOWNER, __ATOMIC_RELEASE);
    struct Spoor *e = open_ctl_leaf("kstack");
    long allowed = e ? devctl.read(e, buf, sizeof buf, 0) : -1;
    __atomic_store_n(&t->proc->caps, saved, __ATOMIC_RELEASE);

    TEST_EXPECT_EQ(denied, (long)-T_E_ACCES,
                   "F2: an unprivileged caller is DENIED /ctl/kstack (EACCES)");
    TEST_ASSERT(e != NULL, "open /ctl/kstack (elevated)");
    TEST_ASSERT(allowed > 0, "kstack read positive (elevated)");
    TEST_ASSERT(contains(buf, (size_t)allowed, "usable:"), "has usable:");
    TEST_ASSERT(contains(buf, (size_t)allowed, "peak:"),   "has peak:");
    if (e) spoor_clunk(e);

    // The mode must not lie about a file the caller cannot in fact read.
    struct t_stat st;
    struct Spoor *k = open_ctl_leaf("kstack");
    TEST_ASSERT(k != NULL, "open /ctl/kstack for stat");
    TEST_EXPECT_EQ(devctl.stat_native(k, &st), 0, "stat_native(kstack) ok");
    TEST_EXPECT_EQ(st.mode, (u32)(T_S_IFREG | 0400u),
                   "kstack = S_IFREG|0400 (the CAP_HOSTOWNER gate, stated)");
    if (k) spoor_clunk(k);
}

// The shape of one /ctl/kstack render: its rows' pids, whether the cut line
// follows them, and the peak line's figures. The shape, not the bytes: a
// running thread's own figure deepens between two renders.
#define KSW_PIDS 32
struct ksw_shape {
    int  rows;
    int  pids[KSW_PIDS];
    bool cut;
    bool trunc;       // carries the scan-budget line
    bool bad;         // not the layout: a torn line, a row without five columns, no peak line last
    u32  peak;
    int  peak_pid;
};

static bool ksw_line_is(const char *buf, size_t ls, size_t le, const char *want) {
    size_t k = 0;
    for (; want[k]; k++)
        if (ls + k >= le || buf[ls + k] != want[k]) return false;
    return ls + k == le;
}

static bool ksw_line_starts(const char *buf, size_t ls, size_t le, const char *want) {
    for (size_t k = 0; want[k]; k++)
        if (ls + k >= le || buf[ls + k] != want[k]) return false;
    return true;
}

// The decimal at token `want` (0-based) of the line, and how many tokens it has.
static int ksw_tokens(const char *buf, size_t ls, size_t le, int want, u32 *out) {
    int ntok = 0;
    for (size_t k = ls; k < le; k++) {
        if (buf[k] == ' ' || (k != ls && buf[k - 1] != ' ')) continue;
        if (ntok == want) {
            u32 v = 0;
            for (size_t d = k; d < le && buf[d] >= '0' && buf[d] <= '9'; d++)
                v = v * 10u + (u32)(buf[d] - '0');
            *out = v;
        }
        ntok++;
    }
    return ntok;
}

static void ksw_shape_of(const char *buf, size_t got, struct ksw_shape *o) {
    o->rows = 0;
    o->cut = false;
    o->trunc = false;
    o->bad = false;
    o->peak = 0;
    o->peak_pid = -1;
    if (got == 0 || buf[got - 1] != '\n') { o->bad = true; return; }
    bool table = false, peak_seen = false;
    size_t ls = 0;
    for (size_t i = 0; i < got; i++) {
        if (buf[i] != '\n') continue;
        u32 v = 0;
        if (peak_seen) {
            o->bad = true;                       // the peak line is the last
        } else if (!table) {
            if (ksw_line_is(buf, ls, i, "PID    NAME    PEAK    TID")) table = true;
        } else if (ls < i && buf[ls] >= '0' && buf[ls] <= '9') {
            if (o->cut || o->trunc || ksw_tokens(buf, ls, i, 0, &v) != 5) o->bad = true;
            if (o->rows < KSW_PIDS) o->pids[o->rows] = (int)v;
            o->rows++;
        } else if (ksw_line_is(buf, ls, i, "(cut: more processes than the table holds)")) {
            if (o->cut || o->trunc) o->bad = true;
            o->cut = true;
        } else if (ksw_line_starts(buf, ls, i, "peak:")) {
            if (ksw_tokens(buf, ls, i, 1, &o->peak) != 6) o->bad = true;
            (void)ksw_tokens(buf, ls, i, 3, &v);
            o->peak_pid = (int)v;
            peak_seen = true;
        } else if (ksw_line_is(buf, ls, i, "(scan budget exhausted -- rows above are partial and "
                                           "peak is a LOWER BOUND)")) {
            if (o->trunc) o->bad = true;
            o->trunc = true;
        } else if (ls != i) {
            o->bad = true;
        }
        ls = i + 1;
    }
    if (!peak_seen) o->bad = true;
}

static bool ksw_has(const struct ksw_shape *o, int pid) {
    for (int i = 0; i < o->rows && i < KSW_PIDS; i++)
        if (o->pids[i] == pid) return true;
    return false;
}

// /ctl/kstack ends whole at every buffer size: the whole table's first rows,
// the cut line exactly when a row is missing, and the peak line last -- over
// every Proc, its row shown or not. A table that stopped at the first row
// that did not fit said nothing and lost its peak.
void test_devctl_kstack_ends_whole(void) {
    static char buf[2048];
    static char whole[2048];

    // The deepest stack there can be, on a Proc of the test's own, and a
    // shallow one linked after it: kproc's children are walked newest first,
    // so a row always comes before the deepest Proc's, whatever kproc shows.
    struct Proc *deep = proc_alloc();
    TEST_ASSERT(deep != NULL, "proc_alloc for the deepest Proc");
    struct Proc *ahead = proc_alloc();
    if (!ahead) {
        deep->state = PROC_STATE_ZOMBIE;
        proc_free(deep);
    }
    TEST_ASSERT(ahead != NULL, "proc_alloc for the Proc ahead of the deepest");
    deep->state = PROC_STATE_ALIVE;
    deep->reaped_kstack_peak = THREAD_KSTACK_SIZE;
    deep->reaped_kstack_tid = 1;
    proc_test_link(deep);
    ahead->state = PROC_STATE_ALIVE;
    ahead->reaped_kstack_peak = 64;
    ahead->reaped_kstack_tid = 1;
    proc_test_link(ahead);
    int deep_pid = deep->pid;
    int ahead_pid = ahead->pid;

    struct ksw_shape ref, sh;
    size_t full = devctl_format_kstack_for_test(NULL, whole, sizeof whole);
    ksw_shape_of(whole, full, &ref);
    bool ref_ok = full > 0 && !ref.bad && !ref.cut && !ref.trunc && ref.rows >= 2 &&
                  ksw_has(&ref, deep_pid) &&
                  ref.peak == THREAD_KSTACK_SIZE && ref.peak_pid == deep_pid;
    // The row ahead of the deepest Proc's is what makes a walk that stops at
    // its first cut row lose the peak.
    bool ahead_ok = false;
    for (int i = 0; i + 1 < ref.rows && i + 1 < KSW_PIDS; i++)
        if (ref.pids[i] == ahead_pid && ref.pids[i + 1] == deep_pid) ahead_ok = true;

    // Past `full` by the room the end reserves beyond what it used: there the
    // whole table fits again.
    int bad = 0, not_prefix = 0, silent = 0, lying = 0, low_peak = 0;
    int sized = 0, cut_before_deep = 0, uncut = 0;
    for (size_t cap = 1; ref_ok && cap <= sizeof buf && cap <= full + 256; cap++) {
        size_t got = devctl_format_kstack_for_test(NULL, buf, cap);
        if (got == 0) continue;                  // no room for the header and the table's end
        sized++;
        ksw_shape_of(buf, got, &sh);
        if (sh.bad) { bad++; continue; }
        if (sh.rows > ref.rows) { not_prefix++; continue; }
        for (int i = 0; i < sh.rows && i < KSW_PIDS; i++)
            if (sh.pids[i] != ref.pids[i]) { not_prefix++; break; }
        if (!sh.cut && sh.rows != ref.rows) silent++;
        if (sh.cut && sh.rows == ref.rows) lying++;
        if (sh.peak != THREAD_KSTACK_SIZE || sh.peak_pid != deep_pid) low_peak++;
        if (sh.cut && !ksw_has(&sh, deep_pid)) cut_before_deep++;
        if (!sh.cut) uncut++;
    }

    proc_test_unlink(ahead);
    ahead->state = PROC_STATE_ZOMBIE;
    proc_free(ahead);
    proc_test_unlink(deep);
    deep->state = PROC_STATE_ZOMBIE;
    proc_free(deep);

    TEST_ASSERT(ref_ok, "the whole kstack table, the deepest Proc's row and peak in it, fits 2 KiB uncut (enlarge the buffers if not)");
    TEST_ASSERT(ahead_ok, "a row of the test's own comes directly before the deepest Proc's");
    TEST_ASSERT(sized > 1, "some sizes fit the kstack header and the table's end");
    TEST_EXPECT_EQ(bad, 0, "every size of /ctl/kstack ends on its peak line, each row whole");
    TEST_EXPECT_EQ(not_prefix, 0, "the rows of a cut kstack table are the whole table's first rows");
    TEST_EXPECT_EQ(silent, 0, "a kstack table short of a row ends its rows with the cut line");
    TEST_EXPECT_EQ(lying, 0, "a kstack table with every row carries no cut line");
    TEST_EXPECT_EQ(low_peak, 0, "the kstack peak is the deepest Proc's at every size, its row shown or not");
    TEST_ASSERT(cut_before_deep > 0, "some size cuts the deepest Proc's row and still names it in the peak");
    TEST_ASSERT(uncut > 0, "some size past the header holds the whole kstack table uncut");
}

// /ctl/kstack's walk is paid for by the Proc as well as by the word scanned.
// A Proc that has reaped every thread costs no scan and the walk goes on past
// the cut, so without a price on the visit a farm of such Procs is walked
// whole, IRQs masked, however long it is. Past what the budget pays for, the
// walk stops and the table says its peak is a lower bound -- at every size,
// the cut line and the budget line both fitting the smallest.
#define KSF_MAX 1100
void test_devctl_kstack_says_truncated(void) {
    static char buf[2048];
    static struct Proc *farm[KSF_MAX];
    struct ksw_shape sh;
    u32 visit = devctl_kstack_visit_words_for_test();
    u32 whole_budget = devctl_kstack_budget_words_for_test();

    size_t got = devctl_format_kstack_budget_for_test(buf, sizeof buf, 0);
    ksw_shape_of(buf, got, &sh);
    bool zero_says = got > 0 && !sh.bad && sh.trunc && sh.rows == 0;

    // The smallest power-of-two budget that scans the system as it stands
    // whole. A stack only deepens and a deeper stack costs fewer words, so
    // that budget stays enough for the system; the farm is what it cannot pay.
    u32 enough = 0;
    for (u32 b = 2u * visit; b != 0 && b <= whole_budget && enough == 0; b *= 2u) {
        got = devctl_format_kstack_budget_for_test(buf, sizeof buf, b);
        ksw_shape_of(buf, got, &sh);
        if (got > 0 && !sh.bad && !sh.trunc) enough = b;
    }

    // One visit more than `enough` pays for, each Proc with a reaped peak and
    // no thread: a zombie's shape, free to scan.
    int want = enough ? (int)(enough / visit) + 1 : 0;
    int have = 0;
    for (; have < want && have < KSF_MAX; have++) {
        farm[have] = proc_alloc();
        if (!farm[have]) break;
        farm[have]->state = PROC_STATE_ALIVE;
        farm[have]->reaped_kstack_peak = 64;
        farm[have]->reaped_kstack_tid = 1;
        proc_test_link(farm[have]);
    }

    int sized = 0, bad = 0, unsaid = 0, both = 0;
    for (size_t cap = 1; have == want && want > 0 && cap <= sizeof buf; cap += (cap < 400 ? 1 : 97)) {
        got = devctl_format_kstack_budget_for_test(buf, cap, enough);
        if (got == 0) continue;                  // no room for the header and the table's end
        sized++;
        ksw_shape_of(buf, got, &sh);
        if (sh.bad) { bad++; continue; }
        if (!sh.trunc) unsaid++;
        if (sh.cut && sh.trunc) both++;
    }

    for (int i = 0; i < have; i++) {
        proc_test_unlink(farm[i]);
        farm[i]->state = PROC_STATE_ZOMBIE;
        proc_free(farm[i]);
    }

    got = enough ? devctl_format_kstack_budget_for_test(buf, sizeof buf, enough) : 0;
    ksw_shape_of(buf, got, &sh);
    bool control_ok = got > 0 && !sh.bad && !sh.trunc;

    TEST_ASSERT(zero_says, "a kstack walk with no budget shows no row and says its peak is a lower bound");
    TEST_ASSERT(enough != 0, "the kstack scan budget covers the suite's own system (raise KSTACK_SCAN_BUDGET_WORDS if not)");
    TEST_ASSERT(want <= KSF_MAX && have == want, "proc_alloc for a farm one visit longer than the budget pays for");
    TEST_ASSERT(sized > 1, "some sizes fit the kstack header and the table's end");
    TEST_EXPECT_EQ(bad, 0, "every size of a kstack table out of budget ends on its peak line, the budget line before it");
    TEST_EXPECT_EQ(unsaid, 0, "a kstack walk over more Procs than its budget pays for says its peak is a lower bound, at every size");
    TEST_ASSERT(both > 0, "some size is both cut and out of budget, and both lines fit");
    TEST_ASSERT(control_ok, "without the farm the same budget scans the system whole");
}

void test_devctl_read_sched_format(void) {
    struct Spoor *c = open_ctl_leaf("sched");
    TEST_ASSERT(c != NULL, "open /ctl/sched");

    char buf[512];
    long got = devctl.read(c, buf, sizeof buf, 0);
    TEST_ASSERT(got > 0, "sched read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "runnable:"), "has runnable:");
    // V-4c-2b: the /proc/stat `processes` source -- the one field in section
    // 6.17's set with no per-CPU form, so it lives in the global block.
    TEST_ASSERT(contains(buf, (size_t)got, "created:"), "V-4c-2b: has created:");

    spoor_clunk(c);
}

// #95: /ctl/cons is the human-readable surface for console byte loss, both
// directions. It is only useful if it can actually be walked to and read -- a
// leaf added to the table but unreachable would look exactly like "no drops"
// to anyone who went looking, which is the failure mode #95 is about.
void test_devctl_read_cons_format(void) {
    struct Spoor *c = open_ctl_leaf("cons");
    TEST_ASSERT(c != NULL, "open /ctl/cons");

    char buf[512];
    long got = devctl.read(c, buf, sizeof buf, 0);
    TEST_ASSERT(got > 0, "cons read positive");
    // #129 renamed two of these: the ring-full sites now count BACK-PRESSURE,
    // not loss. Asserting the new labels is what keeps this surface honest --
    // had the labels been left alone, /ctl/cons would still say "rx_drop_raw"
    // for an event where nothing was dropped.
    TEST_ASSERT(contains(buf, (size_t)got, "rx_bp_raw:"),     "has rx_bp_raw:");
    TEST_ASSERT(contains(buf, (size_t)got, "rx_bp_flush:"),   "has rx_bp_flush:");
    TEST_ASSERT(contains(buf, (size_t)got, "rx_drop_line:"),  "has rx_drop_line:");
    TEST_ASSERT(contains(buf, (size_t)got, "rx_drop_ring:"),  "has rx_drop_ring:");
    TEST_ASSERT(contains(buf, (size_t)got, "rx_drop_modeflush:"), "has rx_drop_modeflush:");
    TEST_ASSERT(contains(buf, (size_t)got, "rx_drop_side:"),  "has rx_drop_side:");
    TEST_ASSERT(contains(buf, (size_t)got, "tx_dropped:"),    "has tx_dropped:");
    TEST_ASSERT(contains(buf, (size_t)got, "tx_room_waits:"), "has tx_room_waits:");

    spoor_clunk(c);
}

// #210: /ctl/9p-sessions end to end through the Dev vtable -- a live conn
// with known counters must render as a `conn` row, and the row must be
// gone after the last unref (nothing stale in the registry).
void test_devctl_read_9p_sessions_format(void) {
    struct SrvConn *cn = srvconn_create(0xBBBBu, 31337, 0xA11CEu, false, 0, 0xB0B0u,
                                        SRVCONN_MSIZE);
    TEST_ASSERT(cn != NULL, "srvconn_create");
    const u8 bytes[3] = { 9, 9, 9 };
    TEST_EXPECT_EQ(srvconn_client_send(cn, bytes, 3), (long)3, "3 bytes in");

    struct Spoor *c = open_ctl_leaf("9p-sessions");
    TEST_ASSERT(c != NULL, "open /ctl/9p-sessions");
    char buf[2048];
    long got = devctl.read(c, buf, sizeof buf, 0);
    TEST_ASSERT(got > 0, "9p-sessions read positive with a live conn");
    TEST_ASSERT(contains(buf, (size_t)got, "conn peer=31337"),
                "the live conn renders by peer pid");
    // Assert the WHOLE row through its TAIL, not a prefix: wedge run 1
    // passed the prefix assertion while the formatter aborted mid-row
    // (fmt_str("") returns 0 == the overflow sentinel), losing every
    // field after c2s_buffered and every later row.
    TEST_ASSERT(contains(buf, (size_t)got,
                "c2s=3/0+3 s2c=0/0+0 sframes=0"),
                "the full conn row renders through its tail");
    spoor_clunk(c);

    // IMPERIUM-DESIGN 11.3 item 10: the ring counters are the conn's ends', the
    // system principal's or a hostowner's. Any other reader sees the row with "-".
    {
        struct Proc r;
        for (size_t i = 0; i < sizeof(r); i++) ((u8 *)&r)[i] = 0;
        r.principal_id = 0xC0FFEEu;
        size_t n = devctl_format_9p_sessions_for_test(&r, buf, sizeof buf);
        TEST_ASSERT(contains(buf, n, "conn peer=31337 msize="), "an ordinary reader sees the conn row");
        TEST_ASSERT(contains(buf, n, "admission count="), "an ordinary reader sees the admission summary");
        // The global figures are the summary's alone; a row carries its own domain's.
        size_t rl = 0;
        const char *row = line_of(buf, n, "conn peer=31337 ", &rl);
        TEST_ASSERT(row != NULL && contains(row, rl, " credits="), "the conn row carries its own domain's credits");
        TEST_ASSERT(row != NULL && !contains(row, rl, " guests=") && !contains(row, rl, " all=") &&
                    !contains(row, rl, " domains=") && !contains(row, rl, "guestcredits=") &&
                    !contains(row, rl, "allcredits="), "a conn row repeats no global figure");
        // One row is live, so a buffer that holds the summary and not a row cuts,
        // and one that does not hold the summary is the cut line alone.
        {
            static const char cut[] = "(cut: more rows than the table holds)\n";
            char small[192];
            size_t s = devctl_format_9p_sessions_for_test(&r, small, sizeof small);
            TEST_ASSERT(s <= sizeof small && contains(small, s, "admission count="), "a short read keeps the summary");
            TEST_ASSERT(ends_with(small, s, cut), "a render that left a row out ends with the cut line");
            TEST_ASSERT(!contains(small, s, "conn peer="), "a row that does not fit is left out whole");
            s = devctl_format_9p_sessions_for_test(&r, small, 64);
            TEST_ASSERT(s == sizeof cut - 1 && ends_with(small, s, cut), "a read too short for the summary is the cut line alone");
            TEST_ASSERT(!ends_with(buf, n, cut), "a render that holds every row has no cut line");
        }
        TEST_ASSERT(contains(buf, n, " c2s=- s2c=- sframes=-\n"), "an ordinary reader sees its counters as '-'");
        TEST_ASSERT(!contains(buf, n, "c2s=3/0+3"), "an ordinary reader does not see the byte counts");
        r.caps = CAP_HOSTOWNER;
        n = devctl_format_9p_sessions_for_test(&r, buf, sizeof buf);
        TEST_ASSERT(contains(buf, n, "c2s=3/0+3 s2c=0/0+0 sframes=0"), "a hostowner sees the counters");
        r.caps = 0;
        r.principal_id = 0xA11CEu;
        n = devctl_format_9p_sessions_for_test(&r, buf, sizeof buf);
        TEST_ASSERT(contains(buf, n, "c2s=3/0+3 s2c=0/0+0 sframes=0"), "the client end sees the counters");
        r.principal_id = 0xB0B0u;
        n = devctl_format_9p_sessions_for_test(&r, buf, sizeof buf);
        TEST_ASSERT(contains(buf, n, "c2s=3/0+3 s2c=0/0+0 sframes=0"), "the server end sees the counters");
        // A none reader gets no row (Plan 9's nonone); the ordinary reader above is
        // the control one variable away, and a none hostowner is the wall's exemption.
        r.principal_id = PRINCIPAL_NONE;
        n = devctl_format_9p_sessions_for_test(&r, buf, sizeof buf);
        TEST_ASSERT(!contains(buf, n, "conn peer=31337"), "a none reader sees no conn row");
        TEST_ASSERT(!contains(buf, n, "admission"), "a none reader sees no admission summary");
        TEST_ASSERT(n == 0, "a none reader reads an empty file");
        r.caps = CAP_HOSTOWNER;
        n = devctl_format_9p_sessions_for_test(&r, buf, sizeof buf);
        TEST_ASSERT(contains(buf, n, "conn peer=31337 msize="), "a none hostowner sees the conn row");
        TEST_ASSERT(contains(buf, n, "admission count="), "a none hostowner sees the admission summary");
        r.caps = 0;
    }

    srvconn_teardown(cn);
    srvconn_unref(cn);

    c = open_ctl_leaf("9p-sessions");
    TEST_ASSERT(c != NULL, "re-open /ctl/9p-sessions");
    got = devctl.read(c, buf, sizeof buf, 0);
    TEST_ASSERT(got >= 0, "9p-sessions read ok after teardown");
    TEST_ASSERT(!contains(buf, (size_t)(got > 0 ? got : 0), "peer=31337"),
                "the freed conn no longer renders");
    spoor_clunk(c);
}

void test_devctl_write_rejected(void) {
    struct Spoor *c = open_ctl_leaf("procs");
    TEST_ASSERT(c != NULL, "open /ctl/procs");

    const char cmd[] = "kill all";
    long n = (long)sizeof(cmd) - 1;
    TEST_EXPECT_EQ(devctl.write(c, cmd, n, 0), (long)-1,
                   "v1.0 ctl writes rejected (admin commands deferred)");

    spoor_clunk(c);
}

void test_devctl_read_dir_returns_neg1(void) {
    struct Spoor *root = devctl.attach("");
    TEST_ASSERT(devctl.open(root, 0) != NULL, "open root");

    char buf[16];
    TEST_EXPECT_EQ(devctl.read(root, buf, 16, 0), (long)-1,
                   "directory read returns -1 (readdir deferred)");

    spoor_clunk(root);
}

// stat_native: the apex is a directory, the leaves are regular files, and the
// FILE-TYPE bits are present (VIVARIUM V-4b-5). /ctl had no stat_native at all,
// so spoor_stat_native returned -1 -> EIO for stat("/ctl") AND for realpath()
// of anything under it (musl's resolver walks each prefix and treats any errno
// but EINVAL as fatal).
void test_devctl_stat_native_shapes(void) {
    struct t_stat st;

    struct Spoor *root = devctl.attach("");
    TEST_ASSERT(root != NULL, "attach /ctl");
    TEST_ASSERT(devctl.stat_native != NULL, "/ctl has a stat_native slot");
    TEST_EXPECT_EQ(devctl.stat_native(root, &st), 0, "stat_native(/ctl) ok");
    TEST_EXPECT_EQ(st.mode & (u32)T_S_IFMT, (u32)T_S_IFDIR,
                   "/ctl S_IFMT = S_IFDIR (S_ISDIR is true)");
    TEST_EXPECT_EQ(st.mode & ~(u32)T_S_IFMT, (u32)0555u, "/ctl perms = 0555");
    TEST_EXPECT_EQ(st.qid_type, QTDIR,            "/ctl is QTDIR");
    TEST_EXPECT_EQ(st.uid, (u32)PRINCIPAL_SYSTEM, "/ctl uid = SYSTEM");
    TEST_EXPECT_EQ(st.gid, (u32)GID_SYSTEM,       "/ctl gid = SYSTEM");
    spoor_unref(root);

    struct Spoor *procs = open_ctl_leaf("procs");
    TEST_ASSERT(procs != NULL, "open /ctl/procs");
    TEST_EXPECT_EQ(devctl.stat_native(procs, &st), 0, "stat_native(procs) ok");
    TEST_EXPECT_EQ(st.mode, (u32)(T_S_IFREG | 0444u), "procs = S_IFREG|0444");
    TEST_EXPECT_EQ(st.qid_type, QTFILE,               "procs is QTFILE");
    // Generated at read time from the live process table, so no size can be
    // promised in advance -- a caller that fstat'd, malloc'd, and read exactly
    // that many bytes would truncate a table that grew in between. Linux
    // reports 0 for /proc/meminfo for the same reason.
    TEST_EXPECT_EQ(st.size, (u64)0, "a generated report advertises no size");
    spoor_clunk(procs);

    // The mode DOCUMENTS the read-site gate: kernel-base needs CAP_HOSTOWNER
    // (#57a F1 -- it discloses the live KASLR slide), so advertising it
    // world-readable would have the mode lie about a file most callers cannot
    // in fact read.
    struct Spoor *kb = open_ctl_leaf("kernel-base");
    TEST_ASSERT(kb != NULL, "open /ctl/kernel-base");
    TEST_EXPECT_EQ(devctl.stat_native(kb, &st), 0, "stat_native(kernel-base) ok");
    TEST_EXPECT_EQ(st.mode, (u32)(T_S_IFREG | 0400u),
                   "kernel-base = S_IFREG|0400 (the CAP_HOSTOWNER gate, stated)");
    spoor_clunk(kb);
}

// IMPERIUM-DESIGN 11.3 item 10: CPU time is its owner's and the scheduler's
// counters the system principal's. The in-kernel runner is kproc, which IS the
// system principal and the owner of itself, so it can only see the allow side;
// the deny side needs another reader. A real child of the test's Proc (so
// /ctl/procs lists it) takes a principal of its own and reads every gated
// surface through the real read paths -- devctl_read and devproc_read_cb, whose
// wiring of the reader is what a gate check alone cannot prove -- first as an
// ordinary reader, then holding CAP_HOSTOWNER. The child exits and is reaped
// before any assertion, so a red leg leaves nothing behind.
#define GATE_PRINCIPAL 0xC0FFEEu
struct gate_read {
    long          got;
    int           own_ntok, k_ntok;
    char          own_cpu[24], k_cpu[24];    // the CPU_NS token of each row
    bool          cpu_row0_dash3;            // /ctl/cpu row 0: idle_ns ctxt intr all "-"
    bool          cpu_row0_num3;             // ... all numbers
    bool          cpu_capacity_num;          // capacity a number either way
    bool          sched_runnable_dash, sched_runnable_num;
    bool          sched_wc_dash, sched_wc_num;
    bool          sched_cpus_num, sched_created_num;
    bool          st_k_dash, st_k_num, st_own_num;
};
static struct gate_read g_gate[2];          // [0] ordinary, [1] CAP_HOSTOWNER
static int g_gate_self;

// The idx'th space-separated token of the line starting at `line`, NUL-copied.
static void line_token(const char *line, const char *end, int idx, char *out, size_t cap) {
    out[0] = '\0';
    const char *k = line;
    for (int n = 0; k < end; n++) {
        while (k < end && *k == ' ') k++;
        const char *t = k;
        while (k < end && *k != ' ' && *k != '\n') k++;
        if (k == t) return;
        if (n == idx) {
            size_t len = (size_t)(k - t);
            if (len >= cap) len = cap - 1;
            for (size_t i = 0; i < len; i++) out[i] = t[i];
            out[len] = '\0';
            return;
        }
    }
}
static const char *line_after(const char *buf, size_t len, const char *prefix, const char **end) {
    size_t pl = 0;
    while (prefix[pl]) pl++;
    for (size_t i = 0; i < len; ) {
        size_t e = i;
        while (e < len && buf[e] != '\n') e++;
        if (e - i >= pl) {
            size_t j = 0;
            while (j < pl && buf[i + j] == prefix[j]) j++;
            if (j == pl) { *end = buf + e; return buf + i + pl; }
        }
        i = e + 1;
    }
    *end = NULL;
    return NULL;
}
static bool all_digits(const char *t) {
    if (!t[0]) return false;
    for (size_t i = 0; t[i]; i++) if (t[i] < '0' || t[i] > '9') return false;
    return true;
}
static bool is_dash(const char *t) { return t[0] == '-' && t[1] == '\0'; }

// Every key=value token on a line: how many there are, and how many values
// read "-" and how many are numbers.
static void kv_values(const char *line, const char *end, int *all, int *dash, int *num) {
    char tok[40];
    for (int i = 0; ; i++) {
        line_token(line, end, i, tok, sizeof tok);
        if (!tok[0]) return;
        const char *v = tok;
        while (*v && *v != '=') v++;
        if (*v == '=') v++;
        (*all)++;
        if (is_dash(v))          (*dash)++;
        else if (all_digits(v))  (*num)++;
    }
}

static long read_status_of(int pid, char *buf, size_t cap) {
    char name[12];
    int n = 0;
    if (pid == 0) name[n++] = '0';
    else { char tmp[12]; int tn = 0; while (pid > 0) { tmp[tn++] = (char)('0' + pid % 10); pid /= 10; }
           while (tn > 0) name[n++] = tmp[--tn]; }
    name[n] = '\0';
    struct Spoor *root = devproc.attach("");
    if (!root) return -1;
    const char *n1[1] = { name };
    struct Walkqid *wq = devproc.walk(root, NULL, n1, 1);
    spoor_unref(root);
    if (!wq) return -1;
    struct Spoor *piddir = wq->nqid == 1 ? wq->spoor : NULL;
    if (!piddir) spoor_unref(wq->spoor);
    walkqid_free(wq);
    if (!piddir) return -1;
    const char *n2[1] = { "status" };
    wq = devproc.walk(piddir, NULL, n2, 1);
    spoor_unref(piddir);
    if (!wq) return -1;
    struct Spoor *st = wq->nqid == 1 ? wq->spoor : NULL;
    if (!st) spoor_unref(wq->spoor);
    walkqid_free(wq);
    if (!st) return -1;
    if (!devproc.open(st, 0)) { spoor_unref(st); return -1; }
    long got = devproc.read(st, buf, (long)cap, 0);
    spoor_clunk(st);
    return got;
}

static void gate_read_all(struct gate_read *r, int self) {
    static char buf[2048];
    const char *e;
    char tok[24];

    struct Spoor *c = open_ctl_leaf("procs");
    r->got = c ? devctl.read(c, buf, sizeof buf, 0) : -1;
    if (c) spoor_clunk(c);
    if (r->got > 0) {
        unsigned long f[9] = { 0 };
        r->own_ntok = procs_row_of(buf, (size_t)r->got, self, f, 9);
        r->k_ntok   = procs_row_of(buf, (size_t)r->got, 0, f, 9);
        // procs_row_of folds a non-number to 0, so take the CPU_NS token as text.
        for (int which = 0; which < 2; which++) {
            int pid = which ? 0 : self;
            char *dst = which ? r->k_cpu : r->own_cpu;
            dst[0] = '\0';
            for (size_t i = 0; i < (size_t)r->got; ) {
                size_t le = i;
                while (le < (size_t)r->got && buf[le] != '\n') le++;
                line_token(buf + i, buf + le, 0, tok, sizeof tok);
                unsigned long v = 0;
                for (size_t q = 0; tok[q]; q++) v = v * 10u + (unsigned long)(tok[q] - '0');
                if (all_digits(tok) && v == (unsigned long)pid) {
                    line_token(buf + i, buf + le, 8, dst, 24);
                    break;
                }
                i = le + 1;
            }
        }
    }

    c = open_ctl_leaf("cpu");
    long got = c ? devctl.read(c, buf, sizeof buf, 0) : -1;
    if (c) spoor_clunk(c);
    if (got > 0) {
        const char *row = line_after(buf, (size_t)got, "0 ", &e);
        if (row) {
            char idle[24], capy[24], ctxt[24], intr[24];
            line_token(row, e, 0, idle, sizeof idle);
            line_token(row, e, 1, capy, sizeof capy);
            line_token(row, e, 2, ctxt, sizeof ctxt);
            line_token(row, e, 3, intr, sizeof intr);
            r->cpu_row0_dash3   = is_dash(idle) && is_dash(ctxt) && is_dash(intr);
            r->cpu_row0_num3    = all_digits(idle) && all_digits(ctxt) && all_digits(intr);
            r->cpu_capacity_num = all_digits(capy);
        }
    }

    c = open_ctl_leaf("sched");
    got = c ? devctl.read(c, buf, sizeof buf, 0) : -1;
    if (c) spoor_clunk(c);
    if (got > 0) {
        const char *v;
        if ((v = line_after(buf, (size_t)got, "runnable: ", &e))) {
            line_token(v, e, 0, tok, sizeof tok);
            r->sched_runnable_dash = is_dash(tok);
            r->sched_runnable_num  = all_digits(tok);
        }
        // Every value on both lines, so a field that loses its gate is caught; a
        // new field changes the count of nine and fails until it is judged.
        int all = 0, dash = 0, num = 0;
        if ((v = line_after(buf, (size_t)got, "wc: ", &e)))          kv_values(v, e, &all, &dash, &num);
        if ((v = line_after(buf, (size_t)got, "wc-tickless: ", &e))) kv_values(v, e, &all, &dash, &num);
        r->sched_wc_dash = all == 9 && dash == 9;
        r->sched_wc_num  = all == 9 && num == 9;
        if ((v = line_after(buf, (size_t)got, "cpus: ", &e))) {
            line_token(v, e, 0, tok, sizeof tok);
            r->sched_cpus_num = all_digits(tok);
        }
        if ((v = line_after(buf, (size_t)got, "created: ", &e))) {
            line_token(v, e, 0, tok, sizeof tok);
            r->sched_created_num = all_digits(tok);
        }
    }

    got = read_status_of(0, buf, sizeof buf);
    if (got > 0) {
        const char *v = line_after(buf, (size_t)got, "cpu_ns:  ", &e);
        if (v) { line_token(v, e, 0, tok, sizeof tok); r->st_k_dash = is_dash(tok); r->st_k_num = all_digits(tok); }
    }
    got = read_status_of(self, buf, sizeof buf);
    if (got > 0) {
        const char *v = line_after(buf, (size_t)got, "cpu_ns:  ", &e);
        if (v) { line_token(v, e, 0, tok, sizeof tok); r->st_own_num = all_digits(tok); }
    }
}

static void counters_gated_thunk(void *arg) {
    (void)arg;
    struct Proc *self = current_thread()->proc;
    g_gate_self = self->pid;
    __atomic_store_n(&self->principal_id, GATE_PRINCIPAL, __ATOMIC_RELEASE);
    u64 caps = __atomic_load_n(&self->caps, __ATOMIC_ACQUIRE);
    __atomic_store_n(&self->caps, caps & ~CAP_HOSTOWNER, __ATOMIC_RELEASE);
    gate_read_all(&g_gate[0], self->pid);
    __atomic_store_n(&self->caps, caps | CAP_HOSTOWNER, __ATOMIC_RELEASE);
    gate_read_all(&g_gate[1], self->pid);
    exits("ok");
}

void test_devctl_counters_gated(void) {
    g_gate[0] = (struct gate_read){ 0 };
    g_gate[1] = (struct gate_read){ 0 };
    g_gate_self = 0;
    int pid = rfork(RFPROC, counters_gated_thunk, NULL);
    int st = -1;
    int reaped = (pid > 0) ? wait_pid_for(pid, 0, &st) : -1;

    // The system principal: kproc itself, through the same paths.
    struct gate_read sysr = { 0 };
    gate_read_all(&sysr, 0);

    TEST_ASSERT(pid > 0, "rfork the reader child under the test's Proc");
    TEST_ASSERT(reaped == pid, "reap the reader child");
    TEST_EXPECT_EQ(g_gate_self, pid, "the child read as itself");

    const struct gate_read *o = &g_gate[0], *h = &g_gate[1];
    TEST_ASSERT(o->got > 0 && o->own_ntok == 9 && o->k_ntok == 9,
                "an ordinary reader sees every row, all nine columns");
    TEST_ASSERT(all_digits(o->own_cpu), "an ordinary reader sees its own CPU_NS");
    TEST_ASSERT(is_dash(o->k_cpu), "another principal's CPU_NS is '-' (/ctl/procs)");
    TEST_ASSERT(o->st_own_num, "its own status cpu_ns is a number");
    TEST_ASSERT(o->st_k_dash, "another principal's status cpu_ns is '-'");
    TEST_ASSERT(o->cpu_row0_dash3, "/ctl/cpu idle_ns, ctxt and intr are '-' to an ordinary reader");
    TEST_ASSERT(o->cpu_capacity_num, "the capacity class stays visible");
    TEST_ASSERT(o->sched_runnable_dash, "/ctl/sched runnable is '-' to an ordinary reader");
    TEST_ASSERT(o->sched_wc_dash, "the work-conservation counts are '-' to an ordinary reader");
    TEST_ASSERT(o->sched_cpus_num && o->sched_created_num, "cpus: and created: stay visible");

    TEST_ASSERT(all_digits(h->k_cpu), "a hostowner sees another principal's CPU_NS");
    TEST_ASSERT(h->st_k_num, "a hostowner sees another principal's status cpu_ns");
    TEST_ASSERT(h->cpu_row0_num3, "a hostowner sees idle_ns, ctxt and intr");
    TEST_ASSERT(h->sched_runnable_num && h->sched_wc_num, "a hostowner sees the scheduler counters");

    TEST_ASSERT(all_digits(sysr.k_cpu), "the system principal sees its own CPU_NS");
    TEST_ASSERT(sysr.cpu_row0_num3, "the system principal sees idle_ns, ctxt and intr");
    TEST_ASSERT(sysr.sched_runnable_num && sysr.sched_wc_num, "the system principal sees the scheduler counters");

    // The predicate on its own, including the NULL reader: one zeroed Proc,
    // its principal and caps set per case.
    struct Proc r;
    for (size_t i = 0; i < sizeof(r); i++) ((u8 *)&r)[i] = 0;
    r.principal_id = PRINCIPAL_SYSTEM;
    bool sys_ok = devctl_system_counters_readable(&r);
    r.principal_id = GATE_PRINCIPAL;
    bool user_ok = devctl_system_counters_readable(&r);
    r.caps = CAP_HOSTOWNER;
    bool ho_ok = devctl_system_counters_readable(&r);
    TEST_ASSERT(sys_ok,  "PRINCIPAL_SYSTEM reads the system counters");
    TEST_ASSERT(ho_ok,   "CAP_HOSTOWNER reads the system counters");
    TEST_ASSERT(!user_ok, "an ordinary principal does not");
    TEST_ASSERT(!devctl_system_counters_readable(NULL), "no reader does not");
}

// The shape of one /ctl/procs render: its rows' pids, whether it ends with the
// cut line, and whether every line of it is whole. The shape, not the bytes:
// CPU_NS moves between two renders of the same table.
#define PRW_PIDS 32
struct prw_shape {
    int  rows;
    int  pids[PRW_PIDS];
    bool cut;
    bool torn;        // does not end at a line end
    bool short_row;   // a row without nine columns
};

static void prw_shape_of(const char *buf, size_t got, struct prw_shape *o) {
    static const char cutline[] =
        "(cut: more processes than the table holds; /proc lists them all)\n";
    const size_t cl = sizeof cutline - 1;
    o->rows = 0;
    o->torn = false;
    o->short_row = false;
    o->cut = got >= cl;
    for (size_t i = 0; o->cut && i < cl; i++)
        if (buf[got - cl + i] != cutline[i]) o->cut = false;
    size_t body = o->cut ? got - cl : got;
    if (body == 0 || buf[body - 1] != '\n') { o->torn = true; return; }
    size_t ls = 0;
    bool header = true;
    for (size_t i = 0; i < body; i++) {
        if (buf[i] != '\n') continue;
        if (!header) {
            int ntok = 0, pid = 0;
            for (size_t k = ls; k < i; k++)
                if (buf[k] != ' ' && (k == ls || buf[k - 1] == ' ')) ntok++;
            if (ntok != 9) o->short_row = true;
            for (size_t k = ls; k < i && buf[k] >= '0' && buf[k] <= '9'; k++)
                pid = pid * 10 + (buf[k] - '0');
            if (o->rows < PRW_PIDS) o->pids[o->rows] = pid;
            o->rows++;
        }
        header = false;
        ls = i + 1;
    }
}

// A /ctl/procs row is committed whole or not at all, and a table that does not
// hold every row says so. At every buffer size the output is the header, the
// whole table's first rows, each of nine columns, and -- exactly when a row is
// missing -- the cut line last: a row cut mid-number would hand a parser a
// plausible smaller figure, and a table short without a sign reads as "no
// other Proc".
void test_devctl_procs_rows_whole(void) {
    static char buf[2048];
    static char whole[2048];
    const struct Proc *self = current_thread()->proc;

    // Two rows of the test's own beside kproc's: with three rows some buffer
    // size holds a row and the cut line while a row is still owed, whatever
    // else is alive.
    struct Proc *own[2] = { proc_alloc(), proc_alloc() };
    TEST_ASSERT(own[0] != NULL && own[1] != NULL, "proc_alloc for the table's own rows");
    for (int i = 0; i < 2; i++) {
        own[i]->state = PROC_STATE_ALIVE;
        proc_test_link(own[i]);
    }

    struct prw_shape ref, sh;
    size_t full = devctl_format_procs_for_test(self, whole, sizeof whole);
    prw_shape_of(whole, full, &ref);
    bool ref_ok = full > 0 && !ref.torn && !ref.short_row && !ref.cut && ref.rows >= 3;

    int torn = 0, short_rows = 0, silent = 0, lying = 0, not_prefix = 0;
    int sized = 0, cuts_with_rows = 0;
    for (size_t cap = 1; ref_ok && cap <= full; cap++) {
        size_t got = devctl_format_procs_for_test(self, buf, cap);
        if (got == 0) continue;                  // no room for the header and the cut line
        sized++;
        prw_shape_of(buf, got, &sh);
        if (sh.torn) { torn++; continue; }
        if (sh.short_row) short_rows++;
        if (sh.rows > ref.rows) { not_prefix++; continue; }
        for (int i = 0; i < sh.rows && i < PRW_PIDS; i++)
            if (sh.pids[i] != ref.pids[i]) { not_prefix++; break; }
        if (!sh.cut && sh.rows != ref.rows) silent++;
        if (sh.cut && sh.rows == ref.rows) lying++;
        if (sh.cut && sh.rows > 0) cuts_with_rows++;
    }
    size_t again = devctl_format_procs_for_test(self, buf, sizeof buf);
    prw_shape_of(buf, again, &sh);

    for (int i = 0; i < 2; i++) {
        proc_test_unlink(own[i]);
        own[i]->state = PROC_STATE_ZOMBIE;
        proc_free(own[i]);
    }

    TEST_ASSERT(ref_ok, "the whole table, three rows or more, fits 2 KiB uncut (enlarge the buffers if not)");
    TEST_ASSERT(sized > 1, "some sizes fit the header and more");
    TEST_EXPECT_EQ(torn, 0, "no buffer size leaves a row cut mid-field");
    TEST_EXPECT_EQ(short_rows, 0, "every row carries nine columns");
    TEST_EXPECT_EQ(not_prefix, 0, "the rows of a cut table are the whole table's first rows");
    TEST_EXPECT_EQ(silent, 0, "a table short of a row ends with the cut line");
    TEST_EXPECT_EQ(lying, 0, "a table with every row carries no cut line");
    TEST_ASSERT(cuts_with_rows > 0, "some size holds rows and the cut line");
    TEST_ASSERT(again > 0 && !sh.cut && !sh.torn && sh.rows == ref.rows,
                "the whole table again, at full size, is whole and uncut");
}

// Whether a row of a /ctl/procs render starts with `pid`.
static bool prw_has_pid(const char *buf, size_t got, int pid) {
    size_t ls = 0;
    for (size_t i = 0; i < got; i++) {
        if (buf[i] != '\n') continue;
        int v = 0;
        size_t k = ls;
        while (k < i && buf[k] >= '0' && buf[k] <= '9') v = v * 10 + (buf[k++] - '0');
        if (k > ls && v == pid) return true;
        ls = i + 1;
    }
    return false;
}

// Read the open file `c` from `got` to end of file in `piece`-byte reads.
// Returns the total length read, or -1 on an error or a table past `cap`.
static long prw_read_rest(struct Spoor *c, char *out, size_t cap, size_t got, long piece) {
    for (;;) {
        if (got + (size_t)piece > cap) return -1;
        long r = devctl.read(c, out + got, piece, (s64)got);
        if (r < 0) return -1;
        if (r == 0) return (long)got;
        got += (size_t)r;
    }
}

// The race leg's second reader: a Proc joins, then a read of the same open file
// at offset 0 arrives while the first read's rendering waits to be published.
struct psnap_race {
    struct Spoor *c;
    struct Proc  *join;
    char         *out;
    long          got;
    bool          linked;
};
static void psnap_race_read(void *arg) {
    struct psnap_race *r = arg;
    proc_test_link(r->join);
    r->linked = true;
    r->got = devctl.read(r->c, r->out, 4096, 0);
}

// /ctl/procs is rendered once per open file (vote 108). The first read keeps
// the rendering, a whole read too, and its next read is end of file. A reader
// taking 16 bytes at a time reads the table a whole read had just shown, even
// when a Proc joins between its first piece and the rest -- the Proc a fresh
// open does show, so the join happened. A second read of the same open file
// arriving at offset 0 before the first publishes is served the first
// published table, and the loser's copy is freed. A walk clone carries no
// snapshot, and the clone opened from a read-into file (a mount crossing does
// that) starts at offset 0 and reads its own whole table. The last close frees
// the snapshot's page; one-byte reads read a whole table. The table can change
// between the reference read and the pieced one (a Proc reaped), so the pieced
// read is taken again until it matches, within four tries.
void test_devctl_procs_snapshot(void) {
    static char whole[4096];   // devctl's own cap: a larger buffer reads no more
    static char pieced[4096 + 16];
    static char fresh[4096];
    static char ones[4096 + 1];
    static char outer[4096], nested[4096];

    struct Proc *own[3] = { proc_alloc(), proc_alloc(), proc_alloc() };
    TEST_ASSERT(own[0] != NULL && own[1] != NULL && own[2] != NULL,
                "proc_alloc for the table's own rows");
    for (int i = 0; i < 3; i++) own[i]->state = PROC_STATE_ALIVE;
    for (int i = 0; i < 2; i++) proc_test_link(own[i]);
    const u64 held0 = devctl_procs_snaps_held_for_test();

    struct prw_shape ref = { 0 }, sh = { 0 };
    long full = -1, w_next = -1, got = -1, fl = -1;
    bool w_kept = false, same = false, fresh_joined = false, pieced_joined = true;
    u64 held_open = 0;
    struct Spoor *p = NULL;
    for (int attempt = 0; attempt < 4 && !same; attempt++) {
        struct Spoor *w = open_ctl_leaf("procs");
        if (!w) break;
        full = devctl.read(w, whole, (long)sizeof whole, 0);
        w_kept = w->aux != NULL;
        w_next = devctl.read(w, fresh, (long)sizeof fresh, full > 0 ? full : 1);
        spoor_clunk(w);
        if (full <= 16) break;
        prw_shape_of(whole, (size_t)full, &ref);

        if (p) spoor_clunk(p);
        p = open_ctl_leaf("procs");
        if (!p) break;
        long first = devctl.read(p, pieced, 16, 0);
        proc_test_link(own[2]);
        got = first == 16 ? prw_read_rest(p, pieced, sizeof pieced, 16, 16) : -1;
        held_open = devctl_procs_snaps_held_for_test();
        struct Spoor *f = open_ctl_leaf("procs");
        fl = f ? devctl.read(f, fresh, (long)sizeof fresh, 0) : -1;
        if (f) spoor_clunk(f);
        proc_test_unlink(own[2]);
        if (got <= 0 || fl <= 0) break;

        prw_shape_of(pieced, (size_t)got, &sh);
        fresh_joined = prw_has_pid(fresh, (size_t)fl, own[2]->pid);
        pieced_joined = prw_has_pid(pieced, (size_t)got, own[2]->pid);
        same = got == full && !sh.torn && !sh.short_row && sh.rows == ref.rows;
        for (int i = 0; same && i < ref.rows && i < PRW_PIDS; i++)
            if (sh.pids[i] != ref.pids[i]) same = false;
    }

    // The clones, both walk shapes: devctl's own spoor_clone, and the caller's
    // pre-clone. A clone found carrying the snapshot is detached before its
    // close, so the failure is the assertion and not a page freed twice.
    bool clone_bare = false, nc_bare = false, opened_rewound = false;
    u64 held_opened = 0;
    long opened_len = -1;
    struct prw_shape osh2 = { 0 };
    if (p) {
        struct Walkqid *wq = devctl.walk(p, NULL, (const char **)0, 0);
        if (wq) {
            struct Spoor *cl = wq->spoor;
            clone_bare = cl != NULL && cl != p && cl->aux == NULL;
            if (cl && cl != p) {
                cl->aux = NULL;
                spoor_clunk(cl);
            }
            walkqid_free(wq);
        }
        struct Spoor *nc = spoor_clone(p);
        if (nc) {
            wq = devctl.walk(p, nc, (const char **)0, 0);
            nc_bare = wq != NULL && wq->spoor == nc && nc->aux == NULL;
            if (wq) walkqid_free(wq);
            nc->aux = NULL;
            spoor_clunk(nc);
        }

        // A mount crossing clones the mounted Spoor and opens the clone; p's
        // cursor stands where its reader left it.
        p->offset = got > 0 ? got : 16;
        struct Spoor *mc = spoor_clone(p);
        if (mc) {
            wq = devctl.walk(p, mc, (const char **)0, 0);
            if (wq) walkqid_free(wq);
            mc->aux = NULL;
            if (devctl.open(mc, 0)) {
                opened_rewound = mc->offset == 0;
                opened_len = devctl.read(mc, fresh, (long)sizeof fresh, mc->offset);
                if (opened_len > 0) prw_shape_of(fresh, (size_t)opened_len, &osh2);
                held_opened = devctl_procs_snaps_held_for_test();
            }
            spoor_clunk(mc);
        }
    }
    long past = p && got > 0 ? devctl.read(p, fresh, (long)sizeof fresh, got) : -1;
    bool page_freed = false;
    if (p) {
        if (p->aux) kfree_large_watch_arm_for_test(p->aux);
        spoor_clunk(p);
        page_freed = kfree_large_watch_fired_for_test();
        kfree_large_watch_disarm_for_test();
    }

    // The race: the second reader publishes first, so the first read must serve
    // its table (the joined Proc in it) and free its own copy.
    struct psnap_race race = { .c = open_ctl_leaf("procs"), .join = own[2], .out = nested,
                               .got = -1, .linked = false };
    long out_len = -1;
    u64 held_race = 0;
    if (race.c) {
        devctl_procs_snap_race_for_test(psnap_race_read, &race);
        out_len = devctl.read(race.c, outer, (long)sizeof outer, 0);
        devctl_procs_snap_race_for_test(NULL, NULL);
        held_race = devctl_procs_snaps_held_for_test();
        spoor_clunk(race.c);
        if (race.linked) proc_test_unlink(own[2]);
    }
    bool race_same = out_len > 0 && out_len == race.got;
    for (long i = 0; race_same && i < out_len; i++)
        if (outer[i] != nested[i]) race_same = false;
    bool race_joined = race.got > 0 && prw_has_pid(nested, (size_t)race.got, own[2]->pid);

    struct Spoor *o = open_ctl_leaf("procs");
    long one = o ? devctl.read(o, ones, 1, 0) : -1;
    long onelen = one == 1 ? prw_read_rest(o, ones, sizeof ones, 1, 1) : -1;
    struct prw_shape osh = { 0 };
    if (onelen > 0) prw_shape_of(ones, (size_t)onelen, &osh);
    if (o) spoor_clunk(o);
    const u64 held_end = devctl_procs_snaps_held_for_test();

    for (int i = 0; i < 2; i++) proc_test_unlink(own[i]);
    for (int i = 0; i < 3; i++) {
        own[i]->state = PROC_STATE_ZOMBIE;
        proc_free(own[i]);
    }

    TEST_ASSERT(full > 16 && !ref.torn && !ref.cut && ref.rows >= 3,
                "the whole table, three rows or more, in one read");
    TEST_ASSERT(w_kept, "a whole read keeps its rendering too");
    TEST_EXPECT_EQ(w_next, 0, "after it, the next read is end of file");
    TEST_ASSERT(got > 0, "16-byte reads reach end of file");
    TEST_ASSERT(fresh_joined, "a fresh open shows the Proc that joined between the pieces");
    TEST_ASSERT(!pieced_joined, "the pieced table does not");
    TEST_ASSERT(same, "the pieced table is the whole table, row for row, within four tries");
    TEST_EXPECT_EQ(held_open, held0 + 1, "the pieced read holds one snapshot");
    TEST_ASSERT(clone_bare, "a walk clone of the open file carries no snapshot");
    TEST_ASSERT(nc_bare, "nor does the caller's pre-clone after the walk");
    TEST_ASSERT(opened_rewound, "a clone opened from a read-into file starts at offset 0");
    TEST_ASSERT(opened_len > 0 && !osh2.torn && !osh2.short_row && osh2.rows >= 3,
                "and reads a whole table of its own");
    TEST_EXPECT_EQ(held_opened, held0 + 2, "the opened clone holds its own snapshot");
    TEST_EXPECT_EQ(past, 0, "the pieced read's end is end of file");
    TEST_ASSERT(page_freed, "the open file's last close frees its snapshot's page");
    TEST_ASSERT(race_joined, "the racing read rendered the joined Proc");
    TEST_ASSERT(race_same, "the read that lost the race serves the table published first");
    TEST_EXPECT_EQ(held_race, held0 + 1, "the race leaves one snapshot: the loser's copy is freed");
    TEST_EXPECT_EQ(one, 1, "a one-byte read is answered");
    TEST_ASSERT(one == 1 && ones[0] == 'P', "with the header's first byte");
    TEST_ASSERT(onelen > 0 && !osh.torn && !osh.short_row && osh.rows >= 3,
                "one-byte reads read a whole table");
    TEST_EXPECT_EQ(held_end, held0, "every last close freed its snapshot");
}

// /ctl backs no file mapping (Dev.may_back_mapping unset). A mapping is kept in the
// Image cache with a Spoor reference, so an open /ctl/procs mapped once would
// keep its snapshot page past every close. Measure first, assert last: a broken
// gate leaves a mapping to drain before any assert returns.
void test_devctl_backs_no_mapping(void) {
    u64 held0 = devctl_procs_snaps_held_for_test();
    struct Spoor *f = open_ctl_leaf("procs");
    TEST_ASSERT(f != NULL, "open /ctl/procs");
    char b[1];
    long got = devctl.read(f, b, 1, 0);
    u64 held_read = devctl_procs_snaps_held_for_test();

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    p->territory = territory_alloc();
    spoor_ref(f); hidx_t fd  = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, f);
    spoor_ref(f); hidx_t fdx = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, f);
    s64 m = 0, mf = 0;
    if (p->territory && fd >= 0 && fdx >= 0) {
        m  = sys_mmap_file_for_proc(p, (u64)fd, PAGE_SIZE, false, 0);
        mf = sys_mmap_fixed_file_for_proc(p, EXEC_USER_BURROW_BASE + 0x200000ull,
                                          (u64)fdx, PAGE_SIZE, (u32)VIV_PROT_READ, 0);
    }
    vma_drain(p);
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
    spoor_clunk(f);
    u64 held_end = devctl_procs_snaps_held_for_test();
    // A broken gate leaves an idle Image entry holding the file; measured above,
    // dropped here so the image suite's empty-cache premise is not its casualty.
    if (m > 0 || mf > 0) (void)image_cache_evict_idle_for_test();

    TEST_ASSERT(got == 1 && b[0] == 'P', "premise: the first read took a snapshot");
    TEST_EXPECT_EQ(held_read, held0 + 1, "premise: the open file holds its snapshot");
    TEST_ASSERT(fd >= 0 && fdx >= 0, "handle_alloc");
    TEST_EXPECT_EQ(m, -(s64)T_E_NODEV, "a map of /ctl/procs: ENODEV");
    TEST_EXPECT_EQ(mf, -(s64)T_E_NODEV, "its MAP_FIXED twin: ENODEV");
    TEST_EXPECT_EQ(held_end, held0, "no mapping kept the snapshot past the close");
}
