// loom.native_probe_rust and loom.native_probe_c -- both native Loom adapters
// driven from EL0 against an EL0 9P server (docs/ASYNC-SERVICE-ABI.md, NA-4d).
//
// The test makes a registry of its own and gives its walk-only root to
// /bin/loom-fixture, which posts in it, and to the probe, which names it in
// TARGET. The probe holds the control pipe's write end, so its exit ends the
// fixture; the notify pipe tells the probe the fixture has posted. Every
// expected value comes from THIS kernel's lever, never from what the programs
// saw. Every private ring the probe made must retire, every service connection
// the run made must be freed, and the registry must be destroyed once the test
// lets go of it.
#include "test.h"
#include <thylacine/dev.h>
#include <thylacine/devramfs.h>
#include <thylacine/devsrv.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/loom.h>
#include <thylacine/page.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/territory.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include "../../arch/arm64/timer.h"
#include "../../arch/arm64/uart.h"
#include "../../mm/phys.h"

void test_loom_native_probe_rust(void);
void test_loom_native_probe_c(void);

#ifdef LOOM_PRIVATE_QUALIFICATION
#define NP_FIXTURE_STATUS 77
#define NP_RUST_STATUS    73
#define NP_C_STATUS       75
#define NP_RINGS          1ull
#define NP_CONNECTS       3ull
#else
#define NP_FIXTURE_STATUS 76
#define NP_RUST_STATUS    72
#define NP_C_STATUS       74
#define NP_RINGS          0ull
#define NP_CONNECTS       0ull
#endif

// Each program gets pages of its own: cpio data is 4-aligned where the ELF
// loader needs 8, and both children read their copy while they exec.
#define NP_BLOB_ORDER 8u

struct np_blob {
    struct page *pages;
    u8          *bytes;
    size_t       size;
};

struct np_child {
    const struct np_blob *blob;
    struct Spoor         *fd[3];
    u32                   rights[3];
    bool                  posts;
    const char           *label;
};

static const char *np_load(const char *name, struct np_blob *b) {
    const void *src = NULL;
    size_t size = 0;
    if (devramfs_lookup(name, &src, &size) != 0) return "the program is in ramfs";
    if (size > (PAGE_SIZE << NP_BLOB_ORDER)) return "the program fits its pages";
    b->pages = alloc_pages(NP_BLOB_ORDER, KP_ZERO);
    if (!b->pages) return "pages for the program";
    b->bytes = pa_to_kva(page_to_pa(b->pages));
    for (size_t i = 0; i < size; i++) b->bytes[i] = ((const u8 *)src)[i];
    b->size = size;
    return NULL;
}

static void np_free(struct np_blob *b) {
    if (b->pages) free_pages(b->pages, NP_BLOB_ORDER);
    *b = (struct np_blob){0};
}

__attribute__((noreturn))
static void np_exec_thunk(void *arg) {
    struct np_child *c = arg;
    struct Thread *t = current_thread();
    struct Proc *p = t ? t->proc : NULL;
    if (!p) extinction("np_exec_thunk: no current proc");
    for (int i = 0; i < 3; i++) {
        hidx_t fd = handle_alloc(p, KOBJ_SPOOR, c->rights[i], c->fd[i]);
        if (fd != i) {
            // A refused install kept its reference; exit closes what was taken.
            for (int j = fd < 0 ? i : i + 1; j < 3; j++) spoor_clunk(c->fd[j]);
            uart_puts("    ");
            uart_puts(c->label);
            uart_puts(": a descriptor installed at the wrong number\n");
            exits("fail-fd");
        }
    }
    if (c->posts) proc_mark_may_post_service(p);
    struct Spoor *ramfs_root = devramfs.attach(NULL);
    if (!ramfs_root) extinction("np_exec_thunk: devramfs.attach failed");
    if (territory_chroot(p->territory, ramfs_root) != 0)
        extinction("np_exec_thunk: territory_chroot failed");
    spoor_clunk(ramfs_root);
    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, c->blob->bytes, c->blob->size, &entry, &sp);
    if (rc != 0) {
        uart_puts("    ");
        uart_puts(c->label);
        uart_puts(": exec_setup rc=");
        uart_putdec((u64)rc);
        uart_puts("\n");
        exits("fail-exec");
    }
    userland_enter(entry, sp);
}

static void np_say(const char *what, u64 got, u64 want) {
    uart_puts("    ");
    uart_puts(what);
    uart_puts(" ");
    uart_putdec(got);
    uart_puts(" expected ");
    uart_putdec(want);
    uart_puts("\n");
}

static void np_run(const char *path, const char *label, int want) {
    const void *probe_blob = NULL;
    size_t probe_size = 0;
    if (devramfs_lookup(path, &probe_blob, &probe_size) != 0) {
        // Only a kernel booted without userspace may skip; a ramfs that has init
        // but not the probe is stale, and skipping would pass it.
        const void *init_blob = NULL;
        size_t init_size = 0;
        TEST_ASSERT(devramfs_lookup("bin/joey", &init_blob, &init_size) != 0,
                    "an image that carries userspace carries the native probes");
        uart_puts("    [skip] the native probes are not in ramfs (build with: tools/build.sh all)\n");
        return;
    }

    const char *error = NULL;
    struct np_blob fixture = {0}, probe = {0};
    struct SrvRegistry *reg = NULL;
    struct Spoor *root = NULL, *ctl_rd = NULL, *ctl_wr = NULL, *note_rd = NULL, *note_wr = NULL;
    int fixture_pid = -1, probe_pid = -1, fixture_status = -42, probe_status = -42;
    u64 made0 = loom_private_created(), gone0 = loom_private_retired();
    u64 conns0 = srvconn_total_created(), freed0 = srvconn_total_freed();
    u64 regs0 = srv_registry_total_destroyed();
#define NP(cond, msg) do { if (!error && !(cond)) error = (msg); } while (0)
    error = np_load("bin/loom-fixture", &fixture);
    if (!error) error = np_load(path, &probe);
    reg = error ? NULL : srv_registry_create();
    NP(reg, "a registry for the run");
    root = reg ? devsrv_attach_registry(reg) : NULL;
    NP(root, "the registry's root");
    if (reg) srv_registry_unref(reg); // the root holds it now
    reg = NULL;
    if (root) root->flag |= CWALKONLY;
    NP(!error && pipe_create(&ctl_rd, &ctl_wr) == 0, "the control pipe");
    NP(!error && pipe_create(&note_rd, &note_wr) == 0, "the notify pipe");
    if (!error) {
        // Each install takes one reference; the test keeps its own on the root.
        spoor_ref(root);
        spoor_ref(root);
        // Posting is a create in the registry (sys_walk_create wants
        // RIGHT_WRITE on the parent); TARGET takes any handle.
        struct np_child server = {
            .blob = &fixture, .fd = { root, ctl_rd, note_wr },
            .rights = { RIGHT_READ | RIGHT_WRITE, RIGHT_READ | RIGHT_WRITE, RIGHT_READ | RIGHT_WRITE },
            .posts = true, .label = "loom-fixture",
        };
        struct np_child client = {
            .blob = &probe, .fd = { root, ctl_wr, note_rd },
            .rights = { RIGHT_READ, RIGHT_READ | RIGHT_WRITE, RIGHT_READ | RIGHT_WRITE },
            .posts = false, .label = label,
        };
        fixture_pid = rfork(RFPROC, np_exec_thunk, &server);
        if (fixture_pid > 0) ctl_rd = note_wr = NULL;
        else spoor_clunk(root);
        probe_pid = rfork(RFPROC, np_exec_thunk, &client);
        if (probe_pid > 0) ctl_wr = note_rd = NULL;
        else spoor_clunk(root);
        NP(fixture_pid > 0 && probe_pid > 0, "rfork the fixture and the probe");
        // A child that never started leaves its ends here: drop them now, so
        // the other reads EOF instead of waiting forever.
        if (ctl_rd) spoor_clunk(ctl_rd);
        if (ctl_wr) spoor_clunk(ctl_wr);
        if (note_rd) spoor_clunk(note_rd);
        if (note_wr) spoor_clunk(note_wr);
        ctl_rd = ctl_wr = note_rd = note_wr = NULL;
        for (int n = (fixture_pid > 0) + (probe_pid > 0); n > 0; n--) {
            int status = -42;
            int pid = wait_pid(&status);
            if (pid == fixture_pid) fixture_status = status;
            else if (pid == probe_pid) probe_status = status;
            else NP(false, "wait_pid reaps only the run's children");
        }
    }

    // The retirer is its own thread, and connections and the registry go with
    // their last holders: wait on the monotonic counts, never on a guess.
    u64 deadline = timer_now_ns() + 5000000000ull;
    while (loom_private_retired() - gone0 < loom_private_created() - made0 &&
           timer_now_ns() < deadline)
        sched();
    if (root) spoor_clunk(root);
    root = NULL;
    while ((srvconn_total_freed() - freed0 < srvconn_total_created() - conns0 ||
            srv_registry_total_destroyed() == regs0) && timer_now_ns() < deadline)
        sched();
    u64 made = loom_private_created() - made0, gone = loom_private_retired() - gone0;
    u64 conns = srvconn_total_created() - conns0, freed = srvconn_total_freed() - freed0;
    u64 regs = srv_registry_total_destroyed() - regs0;
    if (ctl_rd) spoor_clunk(ctl_rd);
    if (ctl_wr) spoor_clunk(ctl_wr);
    if (note_rd) spoor_clunk(note_rd);
    if (note_wr) spoor_clunk(note_wr);
    np_free(&fixture);
    np_free(&probe);

    uart_puts("    ");
    uart_puts(label);
    uart_puts(":\n");
    np_say("  fixture status", (u64)fixture_status, NP_FIXTURE_STATUS);
    np_say("  probe status", (u64)probe_status, (u64)want);
    np_say("  rings made", made, NP_RINGS);
    np_say("  rings retired", gone, made);
    np_say("  connections made", conns, NP_CONNECTS);
    np_say("  connections freed", freed, conns);
    np_say("  registries destroyed", regs, 1);
    NP(fixture_status == NP_FIXTURE_STATUS,
       "the fixture's verdict matches this kernel's qualification lever");
    NP(probe_status == want, "the probe's verdict matches this kernel's qualification lever");
    NP(made == NP_RINGS && gone == made, "the probe made its private ring and it retired");
    NP(conns == NP_CONNECTS && freed == conns, "every scope's connection was made and freed");
    NP(regs == 1, "the run's registry was destroyed once the test let go");
#undef NP
    TEST_ASSERT(!error, error);
}

void test_loom_native_probe_rust(void) {
    np_run("bin/loom-native-probe", "loom-native-probe", NP_RUST_STATUS);
}

void test_loom_native_probe_c(void) {
    np_run("bin/loom-c-probe", "loom-c-probe", NP_C_STATUS);
}
