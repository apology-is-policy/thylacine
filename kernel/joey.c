// /joey — first userspace process; the long-running init.
//
// At P5-joey-from-ramfs (this chunk), /joey is loaded from the initrd
// cpio via devramfs_lookup instead of an embedded 9-instruction blob in
// the kernel image. The orchestration is unchanged: kernel rforks a
// child, exec_setup's the joey ELF, userland_enter into EL0, wait_pid
// for completion. What changed is the source of the ELF — and therefore
// what /joey can do: an arbitrary userspace binary instead of a
// hand-encoded SVC SYS_PUTS+SYS_EXITS sequence.
//
// The blob is owned by the devramfs file table for the kernel's
// lifetime; exec_setup copies what it needs into the child's address
// space, so the pointer's lifetime is comfortably longer than the
// child Proc's reference to it.
//
// Boot flow:
//   boot_main() ... all bring-up ...
//     test_run_all()                       in-kernel tests (kproc context)
//     fault_test_run()                     hardening proof
//     joey_run()                           ← P5-joey-from-ramfs
//       devramfs_lookup("joey")            obtain ELF bytes from initrd
//       rfork(RFPROC, joey_thunk, args)    child Proc on kthread kstack
//         joey_thunk:
//           exec_setup(p, blob, size)      populate child's address space
//           userland_enter(entry, sp)      eret to EL0 (never returns)
//         child user code runs:
//           t_putstr(...)                  → SYS_PUTS to UART
//           main returns 0 → _start        → SVC SYS_EXITS
//       wait_pid(&status)                  block; reap child
//     "Thylacine boot OK"                  TOOLING.md §10 ABI
//
// At later chunks /joey becomes the long-running supervisor (per
// ARCHITECTURE.md §5.1 + CORVUS-DESIGN.md §3 D4): forks stratumd-system,
// mounts /sysroot via 9P, pivots root, starts /sbin/corvus and login.
// joey_run keeps its current shape (one-call, wait-and-extinct) only
// until the orchestrator extension lands.
//
// Spec posture: no new TLA+ at P5-joey-from-ramfs. The change is the
// source of the ELF, not the orchestration shape; the prior invariants
// (rfork → exec_setup → userland_enter → wait_pid) hold unchanged.

#include <thylacine/joey.h>

#include <thylacine/dev.h>
#include <thylacine/devramfs.h>
#include <thylacine/devsrv.h>
#include <thylacine/elf.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/stalk.h>
#include <thylacine/territory.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"

// File name in the initrd cpio. Built by usr/joey/CMakeLists.txt and
// copied into the cpio root by tools/build.sh::build_ramfs.
#define JOEY_RAMFS_NAME "joey"

// Cpio newc data is only 4-byte-aligned, but the ELF Ehdr cast in
// elf_load (kernel/elf.c::elf_load — R5-G F61) requires 8-byte
// alignment. Copy into an 8-aligned static buffer before handing the
// blob to exec_setup. Sized to comfortably cover joey's growing
// orchestration surface: at P5-corvus-bringup-c joey embedded inline
// USER_CREATE + AUTH(bad) + AUTH(good) + SESSION_CLOSE wire codec
// (~36 KiB total); at P5-stratumd-stub-bringup-e2 the stub-bringup
// adds an SYS_CHROOT + SYS_WALK_OPEN(FROM_ROOT) sequence, pushing the
// blob over 65 KiB. The A-1b corvus identity-DB harness (RESOLVE/
// GROUP_CREATE round-trips) pushes it past 128 KiB; 256 KiB gives
// headroom for the remaining orchestration (RECOVER, USER_DELETE, etc.).
#define JOEY_BLOB_MAX (256u * 1024u)
static _Alignas(struct Elf64_Ehdr) u8 g_joey_elf_blob[JOEY_BLOB_MAX];

// Arguments passed via rfork's `arg` to the child entry. Lives on the
// caller (boot CPU) stack for the duration of joey_run(); the child
// reads it once before transitioning to EL0, after which the parent
// blocks in wait_pid().
struct joey_args {
    const void *blob;
    size_t      blob_size;
};

// Child entry. Runs in EL1 on the rfork'd kthread's kstack, in the
// child Proc's context (current_thread()->proc is the new Proc).
// Calls exec_setup + userland_enter; never returns from userland_enter
// (transitions to EL0). On exec_setup failure, exits("fail-exec") so
// the parent's wait_pid observes a non-zero exit_status.
__attribute__((noreturn))
static void joey_thunk(void *arg) {
    struct joey_args *ia = (struct joey_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("joey_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("joey_thunk: no proc");

    // P5-hostowner-a: joey is the root of the console-login chain — it
    // owns /dev/cons and is the local-console trust anchor. Stamp the
    // console-attachment bit here, in joey's own context, before exec
    // (race-free: joey's own thread setting joey's own Proc flag). joey
    // is the sole console-attached Proc at v1.0; P5-login will extend
    // the chain by marking the per-user shells it spawns. Maps to
    // specs/corvus.tla MarkConsoleAttached.
    proc_mark_console_attached(p);
    if (!proc_is_console_attached(p))
        extinction("joey_thunk: console-attachment stamp did not take");

    // A-4c-1: joey is the boot console owner -- the target the console_mgr
    // kthread posts the `interrupt` note (Ctrl-C) to. proc_become_zombie_locked
    // clears the owner when joey exits (so it never dangles); A-4c-2's SAK grants
    // the console-ATTACH to corvus (RW-7 R2-F1: NOT ownership -- the owner stays
    // NULL until login spawns the session shell, SPAWN_PERM_CONSOLE_OWNER).
    proc_set_console_owner(p);

    // A-5b (#827b): joey is ALSO the root of the service-posting chain. It
    // grants SPAWN_PERM_MAY_POST_SERVICE to the OS servers it stands up
    // (corvus, the boot stratumd, /sbin/login). Granting that bit requires the
    // granter be console-attached OR already hold it (the one-hop delegation
    // gate). joey relinquishes its console-attach at the bringup->session
    // boundary (I-27), but the getty loop then keeps spawning fresh /sbin/login
    // instances that must each receive the bit -- so joey must remain a *holder*
    // past that relinquish. Stamp it here, in joey's own context, so init is the
    // persistent grant-root. The bit is a perm-flag, never rfork-propagated, so
    // joey holding it grants nothing to its children automatically (each spawn
    // decides per perm_flags) -- I-2 untouched; CONSOLE_TRUSTED stays
    // console-attach-only so I-27 is untouched.
    proc_mark_may_post_service(p);

    // 2B-F3: publish this Proc as init -- the orphan-adopter (ARCH section
    // 7.9 step 6: orphans reparent to init; kproc is only the pre-init
    // fallback). Stamped in the child's own context like the console-attach
    // bit above. joey is the FIRST user Proc; its numeric pid is 1 only on a
    // test-free boot (the in-kernel test phase consumes pids first), which is
    // why the adopter is this pointer, not a pid. Any Proc that orphans
    // children before this line falls back to kproc -- benign, since the
    // pre-joey boot has no orphan producers (the test phase reaps everything,
    // counter-asserted per test).
    proc_publish_init(p);

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ia->blob, ia->blob_size, &entry, &sp);
    if (rc != 0) {
        uart_puts("  joey: exec_setup failed rc=");
        uart_putdec((u64)rc);
        uart_puts("; exits(fail-exec)\n");
        exits("fail-exec");
    }

    userland_enter(entry, sp);
    // userland_enter is __noreturn; control transfers to EL0 atomically.
}

// joey_root_kproc_at_devramfs -- stamp the boot Proc's Territory with a
// root_spoor pointing at the devramfs root. The child Territory clone inherits
// it (territory_clone deep-copies + spoor_refs), so joey + every descendant has
// a sane FROM_ROOT base AND (post-#58) a namespace for SYS_SPAWN binary
// resolution (exec_load_from_namespace -> stalk). Idempotent: an already-rooted
// Territory is left as-is -- the kernel test harness calls this before the
// spawn-resolution tests, so joey_run's later call finds it done. devramfs.
// attach returns ref=1; territory_chroot takes its own ref; we unref to leave
// the territory's ref only.
void joey_root_kproc_at_devramfs(void) {
    struct Thread *kt = current_thread();
    if (!kt || !kt->proc || !kt->proc->territory)
        extinction("joey: no kproc territory to root at devramfs");
    struct Spoor *existing = territory_root_ref(kt->proc->territory);
    if (existing) {                 // already rooted -- idempotent no-op
        spoor_clunk(existing);
        return;
    }
    struct Spoor *ramfs_root = devramfs.attach(NULL);
    if (!ramfs_root) extinction("joey: devramfs.attach for root_spoor failed");
    if (territory_chroot(kt->proc->territory, ramfs_root) != 0)
        extinction("joey: territory_chroot to devramfs root failed");
    spoor_unref(ramfs_root);
    uart_puts("  joey: kproc territory rooted at devramfs (FROM_ROOT walks live)\n");
}

// #57: graft a static single-instance kernel Dev's root onto a synthetic
// devramfs mount-point dir in the kproc territory (the /srv idiom, generalized).
// `mp` resolves WITHOUT crossing (STALK_MOUNT) so the mount keys on the synth
// dir's own (dc, devno, qid.path); MREPL so a re-run replaces. The dev->attach
// ref is dropped after mount() takes its own (success) or after the failure
// path (mount took none). Returns 0 on success, -1 on any step's failure --
// the caller extincts with a Dev-specific message (a boot that cannot mount
// its introspection layer is unrecoverable). joey + every descendant inherits
// the mount via territory_clone; the pivot drops it (synth-dir-keyed), so the
// long-running session re-grafts post-pivot (usr/joey/joey.c).
static int joey_mount_static_dev(struct Thread *kt, struct Dev *dev,
                                 const char *mp, int mp_len) {
    struct Spoor *root_dir = territory_root_ref(kt->proc->territory);
    if (!root_dir)
        return -1;
    struct Spoor *mp_spoor = stalk(kt->proc, root_dir, mp, mp_len, STALK_MOUNT, 0);
    spoor_clunk(root_dir);
    if (!mp_spoor)
        return -1;
    struct Spoor *dev_root = dev->attach(NULL);
    if (!dev_root) {
        spoor_clunk(mp_spoor);
        return -1;
    }
    int rc = mount(kt->proc->territory, dev_root, mp_spoor, MREPL);
    spoor_clunk(dev_root);   // mount holds its own ref on success; none on failure
    spoor_clunk(mp_spoor);   // transient identity probe
    return rc;
}

void joey_run(void) {
    // One-call guard. v1.0 invariant — joey_run is called exactly once
    // per boot from boot_main. The guard catches accidental double-call
    // (e.g., a future supervisor refactor that tries to re-run joey_run
    // instead of re-execing).
    static bool g_joey_run_called = false;
    if (g_joey_run_called) {
        extinction("joey_run: double call (v1.0 single-use invariant)");
    }
    g_joey_run_called = true;

    const void *cpio_blob = NULL;
    size_t blob_size = 0;
    if (devramfs_lookup(JOEY_RAMFS_NAME, &cpio_blob, &blob_size) != 0) {
        // Missing /joey is unrecoverable at v1.0: boot path requires
        // it. Surfaces as EXTINCTION: in the boot log so tools/test.sh
        // reports failure.
        extinction("joey: /joey not found in initrd (devramfs_lookup failed)");
    }
    if (blob_size == 0) {
        extinction("joey: /joey in initrd has zero size");
    }
    if (blob_size > JOEY_BLOB_MAX) {
        extinction_with_addr("joey: /joey ELF exceeds JOEY_BLOB_MAX", (u64)blob_size);
    }

    // Copy cpio's 4-aligned bytes into the 8-aligned static buffer the
    // ELF loader requires.
    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < blob_size; i++) g_joey_elf_blob[i] = src[i];

    // P6-pouch-stratumd-boot 16b-gamma: stamp kproc's Territory with a
    // root_spoor pointing at devramfs root, BEFORE rforking joey. The
    // child Territory clone inherits root_spoor (territory.c::territory_clone
    // deep-copies + spoor_refs), so joey + every descendant Proc has a
    // sane default for SYS_WALK_OPEN(FROM_ROOT, ...) walks. Without this,
    // joey's territory->root_spoor is NULL and FROM_ROOT walks return -1
    // until the Proc itself calls SYS_CHROOT. stratumd's keyfile load
    // path (open("/system.key", O_RDONLY) via the pouch openat-over-
    // walk_open patch) needs FROM_ROOT to resolve, and stratumd has no
    // chroot of its own to issue.
    //
    // Refcount discipline: devramfs_attach returns ref=1 (the dev_simple_-
    // attach pattern); territory_chroot takes its own ref (ref=2); we
    // spoor_unref to drop back to ref=1 (territory's ref only). On
    // Territory destruction the ref is released; for the long-lived
    // kproc Territory at v1.0 this happens at shutdown.
    {
        // #58: root kproc at devramfs (idempotent; the kernel test harness roots
        // it earlier so the post-#58 SYS_SPAWN resolution tests have a namespace).
        joey_root_kproc_at_devramfs();
        struct Thread *kt = current_thread();
        if (!kt || !kt->proc || !kt->proc->territory)
            extinction("joey: no kproc territory");

        // stalk-3a: mount the boot service registry's /srv root on the
        // kproc territory's /srv synthetic dir. joey + every descendant
        // inherits the mount via territory_clone, so all share the ONE
        // namespace-resident boot registry. Posting (create=post) and
        // connecting (open=connect) resolve the boot registry through THIS
        // mount; srv_proc_exit_notify_in + the in-kernel tests reach it via
        // srv_boot_registry. STALK-DESIGN §5.1.
        struct SrvRegistry *boot_reg = srv_boot_registry();
        if (!boot_reg)
            extinction("joey: boot service registry not initialized");
        // RW-4 SA-F1: use territory_root_ref (atomic read+ref under ns_lock) so
        // the "no bare root_spoor read outside territory.c" invariant stays total
        // and uniform with the syscall FROM_ROOT readers -- even though kproc's
        // territory has a single mutator at boot. Released after the stalk base use.
        struct Spoor *srv_root_dir = territory_root_ref(kt->proc->territory);
        if (!srv_root_dir)
            extinction("joey: kproc territory has no root_spoor for /srv mount");

        // Resolve /srv (the devramfs synthetic mount-point dir) WITHOUT
        // crossing it (STALK_MOUNT), mirroring sys_mount_handler. The mount
        // keys on /srv's own (dc, devno, qid.path) identity.
        struct Spoor *srv_mp = stalk(kt->proc, srv_root_dir, "srv", 3,
                                     STALK_MOUNT, 0);
        spoor_clunk(srv_root_dir);   // SA-F1: release the root ref (stalk borrowed it)
        if (!srv_mp)
            extinction("joey: stalk(/srv) for boot devsrv mount failed");

        // Mint the devsrv root over the boot registry (takes one registry
        // ref), then graft it onto /srv. MREPL so a re-run replaces rather
        // than duplicates (defensive; boot mounts once).
        struct Spoor *devsrv_root = devsrv_attach_registry(boot_reg);
        if (!devsrv_root) {
            spoor_clunk(srv_mp);
            extinction("joey: devsrv_attach_registry(boot) failed");
        }
        if (mount(kt->proc->territory, devsrv_root, srv_mp, MREPL) != 0) {
            // mount failed (no ref taken): clunk our attach ref -- last
            // drop runs devsrv_close -> srv_registry_unref (boot reg keeps
            // the global ref; it is not freed).
            spoor_clunk(devsrv_root);
            spoor_clunk(srv_mp);
            extinction("joey: mount(devsrv -> /srv) failed");
        }
        // mount() took its own ref on devsrv_root; drop ours (the mount
        // table is now the holder). srv_mp was a transient identity probe.
        spoor_clunk(devsrv_root);
        spoor_clunk(srv_mp);
        uart_puts("  joey: /srv mounted (namespace-resident service registry)\n");

        // #57: graft the kernel introspection Devs onto their synthetic
        // mount-point dirs -- /proc (devproc: per-pid status/cmdline/ctl/ns)
        // and /ctl (devctl: procs/memory/devices/sched). ARCH 9.4 "v1.0
        // target" layout. Inherited by joey + descendants; the long-running
        // session re-grafts post-pivot. devctl_write is -1 (read-only), and
        // /proc/<pid>/ctl writes stay I-26 two-axis-gated (owner OR
        // CAP_HOSTOWNER/CAP_KILL) independent of namespace reachability -- so
        // the mount widens visibility, never authority.
        if (joey_mount_static_dev(kt, &devproc, "proc", 4) != 0)
            extinction("joey: /proc mount (devproc) failed");
        if (joey_mount_static_dev(kt, &devctl, "ctl", 3) != 0)
            extinction("joey: /ctl mount (devctl) failed");
        uart_puts("  joey: /proc + /ctl mounted (kernel introspection Devs)\n");

        // #57b: graft the /dev char-device directory (devdev: null/zero/full/
        // random/urandom + cons/consctl). ARCH 9.4. The trivial leaves are
        // world-rw; cons/consctl are I-27-gated at devdev.open (the namespace
        // front-door enforces the SAME console-attach check as SYS_CONSOLE_OPEN,
        // so /dev/cons adds no ungated console reader -- IDENTITY-DESIGN 9.8).
        if (joey_mount_static_dev(kt, &devdev, "dev", 3) != 0)
            extinction("joey: /dev mount (devdev) failed");
        uart_puts("  joey: /dev mounted (kernel char devices)\n");

        // Menagerie devhw: graft the DTB hardware inventory at /hw (devhw: the
        // FDT node tree as a walkable namespace -- the one discovery source the
        // kernel provides; the warden + userspace drivers enumerate hardware
        // here). Read-only + perm_enforced=false -> visibility, not authority
        // (the privilege boundary is the allowance/I-34, not this tree).
        if (joey_mount_static_dev(kt, &devhw, "hw", 2) != 0)
            extinction("joey: /hw mount (devhw) failed");
        uart_puts("  joey: /hw mounted (DTB hardware inventory)\n");

        // Menagerie 6b devpci: graft the kernel-mediated PCI topology at /hw/pci
        // (the read-only <bus.dev.fn>/ctl tree -- the warden's in-process PciSource
        // reads it, never raw ECAM). MUST mount AFTER /hw: "hw/pci" resolves by
        // crossing the /hw mount into devhw, then STALK_MOUNT-resolving devhw's
        // synthetic `pci` child (the mount-point). The v1.0 warden reads /hw/pci
        // PRE-pivot; the post-pivot re-graft of this nested mount is a v1.x seam.
        if (joey_mount_static_dev(kt, &devpci, "hw/pci", 6) != 0)
            extinction("joey: /hw/pci mount (devpci) failed");
        uart_puts("  joey: /hw/pci mounted (mediated PCI topology)\n");
    }

    uart_puts("  joey: rforking child for /joey (");
    uart_putdec((u64)blob_size);
    uart_puts(" byte ELF from initrd)\n");

    struct joey_args args = {
        .blob      = g_joey_elf_blob,
        .blob_size = blob_size,
    };

    // P5-corvus-bringup-a: joey is init; it needs to grant
    // CAP_LOCK_PAGES + CAP_CSPRNG_READ to /sbin/corvus (and similarly
    // delegated caps to future child Procs). Plain rfork() would give
    // joey CAP_NONE; spawn-with-caps's AND would then return 0 for
    // every cap bit. rfork_with_caps(CAP_ALL) gives joey the full v1.0
    // capability ceiling so it can grant any subset to children. This
    // matches the production-init pattern: joey is the trusted
    // delegate that distributes caps per child's role.
    int pid = rfork_with_caps(RFPROC, joey_thunk, &args, CAP_ALL);
    if (pid < 0) {
        extinction("joey: rfork_with_caps(RFPROC, joey_thunk) failed");
    }

    int status = -42;
    int reaped = wait_pid(&status);
    if (reaped != pid) {
        extinction_with_addr("joey: wait_pid returned wrong pid", (u64)reaped);
    }
    if (status != 0) {
        extinction_with_addr("joey: /joey exited non-zero", (u64)status);
    }

    uart_puts("  joey: /joey pid=");
    uart_putdec((u64)pid);
    uart_puts(" exited cleanly (status=0)\n");
}
