// /ctl Dev tests (P4-D).
//
// Covers registration, walks, per-leaf reads, write rejection.

#include "test.h"


#include <thylacine/addrspace.h>  // prowl-6: the counters the TABLES column reads
#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/errno.h>
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

// prowl-6: the lazy reservation the capacity tests use (kernel/syscall.c), so the
// probe's first touch is the EL0 shape of an allocation.
extern s64 sys_burrow_reserve_for_proc(struct Proc *p, u64 length_raw, u64 prot_raw,
                                       u64 align_log2_raw);

void test_devctl_bestiary_smoke(void);
void test_devctl_attach_returns_dir(void);
void test_devctl_walk_to_each_leaf(void);
void test_devctl_walk_unknown_misses(void);
void test_devctl_read_procs_format(void);
void test_devctl_procs_tables_column(void);
void test_devctl_counters_gated(void);
void test_devctl_procs_rows_whole(void);
void test_devctl_read_memory_format(void);
void test_devctl_read_devices_format(void);
void test_devctl_read_kernel_base_format(void);
void test_devctl_kernel_base_gated(void);
void test_devctl_kstack_gated(void);
void test_devctl_read_sched_format(void);
void test_devctl_read_cons_format(void);
void test_devctl_read_cpu_format(void);
void test_devctl_write_rejected(void);
void test_devctl_read_dir_returns_neg1(void);
void test_devctl_stat_native_shapes(void);
void test_devctl_read_9p_sessions_format(void);

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
void test_devctl_cpu_sources_live(void);
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

    char buf[256];
    long got = devctl.read(c, buf, 256, 0);
    TEST_ASSERT(got > 0, "memory read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "total:"),    "has total:");
    TEST_ASSERT(contains(buf, (size_t)got, "free:"),     "has free:");
    TEST_ASSERT(contains(buf, (size_t)got, "reserved:"), "has reserved:");
    TEST_ASSERT(contains(buf, (size_t)got, "pages"),     "uses page units");

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
    extern bool devctl_kernel_base_readable(const struct Proc *caller);

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
    TEST_ASSERT(contains(buf, (size_t)got, "tx_dropped:"),    "has tx_dropped:");
    TEST_ASSERT(contains(buf, (size_t)got, "tx_room_waits:"), "has tx_room_waits:");

    spoor_clunk(c);
}

// #210: /ctl/9p-sessions end to end through the Dev vtable -- a live conn
// with known counters must render as a `conn` row, and the row must be
// gone after the last unref (nothing stale in the registry).
size_t devctl_format_9p_sessions_for_test(const struct Proc *reader, char *buf, size_t cap);
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
        r.caps = CAP_HOSTOWNER;
        n = devctl_format_9p_sessions_for_test(&r, buf, sizeof buf);
        TEST_ASSERT(contains(buf, n, "conn peer=31337 msize="), "a none hostowner sees the conn row");
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
    extern bool devctl_system_counters_readable(const struct Proc *reader);
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

// A /ctl/procs row is committed whole or not at all. At every buffer size the
// output is the header and whole rows of nine columns: a row cut mid-number
// would hand ps and prowl a plausible smaller figure.
size_t devctl_format_procs_for_test(const struct Proc *reader, char *buf, size_t cap);
void test_devctl_procs_rows_whole(void) {
    static char buf[1024];
    const struct Proc *self = current_thread()->proc;
    size_t full = devctl_format_procs_for_test(self, buf, sizeof buf);
    TEST_ASSERT(full > 0 && buf[full - 1] == '\n', "procs formats into 1 KiB");
    int cut = 0, short_rows = 0, sized = 0;
    for (size_t cap = 1; cap <= full; cap++) {
        size_t got = devctl_format_procs_for_test(self, buf, cap);
        if (got == 0) continue;                  // the header alone does not fit
        sized++;
        if (buf[got - 1] != '\n') { cut++; continue; }
        size_t ls = got - 1;
        while (ls > 0 && buf[ls - 1] != '\n') ls--;
        int ntok = 0;
        for (size_t i = ls; i < got - 1; i++)
            if (buf[i] != ' ' && (i == ls || buf[i - 1] == ' ')) ntok++;
        if (ntok != 9) short_rows++;
    }
    TEST_ASSERT(sized > 1, "some sizes fit the header and more");
    TEST_EXPECT_EQ(cut, 0, "no buffer size leaves a row cut mid-field");
    TEST_EXPECT_EQ(short_rows, 0, "every last line carries nine columns");
}
