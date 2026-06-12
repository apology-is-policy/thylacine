// Namespace-layout tests (#57): the kernel introspection Devs grafted into the
// boot namespace per ARCH 9.4.
//
// The mount-table primitive is covered by test_territory_mount; stalk's
// single-tree resolution + the per-component X-search by test_stalk. This file
// proves the #57 WIRING end-to-end: devproc @ /proc and devctl @ /ctl, grafted
// onto devramfs synthetic mount-point dirs, RESOLVE THROUGH THE MOUNT -- the
// boot path (kernel/joey.c joey_mount_static_dev) mirrored without the pivot.
//
// The synthetic Proc is PRINCIPAL_SYSTEM (the boot chain's identity), so the
// per-component X-search on the 0555 synth dirs passes; devproc/devctl carry
// perm_enforced == false (Plan 9 all-pids-visible introspection), so the cross
// itself is ungated -- a logged-in user reaches /proc + /ctl identically.

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/stalk.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

extern struct Dev devramfs;
extern struct Dev devproc;
extern struct Dev devctl;

void test_namespace_layout_proc_ctl_cross(void);

// A synthetic SYSTEM Proc carrying a real Territory (stalk needs p->territory
// for mount-crossing; test_stalk's fixtures use a NULL territory and so never
// exercise a cross).
static void mkproc_system_ns(struct Proc *p) {
    for (size_t i = 0; i < sizeof(*p); i++) ((u8 *)p)[i] = 0;
    p->principal_id   = PRINCIPAL_SYSTEM;
    p->primary_gid    = GID_SYSTEM;
    p->supp_gid_count = 0;
    p->caps           = CAP_NONE;
}

void test_namespace_layout_proc_ctl_cross(void) {
    struct Proc pr;
    mkproc_system_ns(&pr);
    pr.territory = territory_alloc();
    TEST_ASSERT(pr.territory != NULL, "territory_alloc");

    // Root the territory at a fresh devramfs root (ships synth /proc + /ctl).
    struct Spoor *ramfs_root = devramfs.attach(NULL);
    TEST_ASSERT(ramfs_root != NULL, "devramfs.attach");
    TEST_EXPECT_EQ(territory_chroot(pr.territory, ramfs_root), 0, "chroot at devramfs");
    spoor_unref(ramfs_root);   // territory holds its own ref

    // (1) Pre-mount: /ctl is an empty synth dir -> /ctl/procs MISSES.
    struct Spoor *base = territory_root_ref(pr.territory);
    TEST_ASSERT(base != NULL, "territory_root_ref");
    struct Spoor *miss = stalk(&pr, base, "ctl/procs", 9, STALK_WALK, 0);
    spoor_clunk(base);
    TEST_ASSERT(miss == NULL, "pre-mount /ctl/procs misses (empty synth dir)");

    // (2) Mount devctl onto /ctl; /ctl/procs now resolves THROUGH the mount.
    base = territory_root_ref(pr.territory);
    struct Spoor *ctl_mp = stalk(&pr, base, "ctl", 3, STALK_MOUNT, 0);
    spoor_clunk(base);
    TEST_ASSERT(ctl_mp != NULL, "stalk(ctl, STALK_MOUNT) -> the synth mount point");
    struct Spoor *ctl_root = devctl.attach(NULL);
    TEST_ASSERT(ctl_root != NULL, "devctl.attach");
    TEST_EXPECT_EQ(mount(pr.territory, ctl_root, ctl_mp, MREPL), 0, "mount devctl @ /ctl");
    spoor_clunk(ctl_root);   // mount holds its own ref
    spoor_clunk(ctl_mp);

    base = territory_root_ref(pr.territory);
    struct Spoor *procs = stalk(&pr, base, "ctl/procs", 9, STALK_WALK, 0);
    spoor_clunk(base);
    TEST_ASSERT(procs != NULL, "post-mount /ctl/procs resolves through the mount");
    TEST_EXPECT_EQ((u64)procs->dc, (u64)'C', "/ctl/procs served by devctl (dc 'C')");
    spoor_clunk(procs);

    // (3) Mount devproc onto /proc; /proc crosses to the devproc root.
    base = territory_root_ref(pr.territory);
    struct Spoor *proc_mp = stalk(&pr, base, "proc", 4, STALK_MOUNT, 0);
    spoor_clunk(base);
    TEST_ASSERT(proc_mp != NULL, "stalk(proc, STALK_MOUNT) -> the synth mount point");
    struct Spoor *proc_root = devproc.attach(NULL);
    TEST_ASSERT(proc_root != NULL, "devproc.attach");
    TEST_EXPECT_EQ(mount(pr.territory, proc_root, proc_mp, MREPL), 0, "mount devproc @ /proc");
    spoor_clunk(proc_root);
    spoor_clunk(proc_mp);

    base = territory_root_ref(pr.territory);
    struct Spoor *proc_q = stalk(&pr, base, "proc", 4, STALK_WALK, 0);
    spoor_clunk(base);
    TEST_ASSERT(proc_q != NULL, "/proc crosses to the devproc root (not the synth dir)");
    TEST_EXPECT_EQ((u64)proc_q->dc, (u64)'p', "/proc served by devproc (dc 'p')");
    TEST_ASSERT((proc_q->qid.type & QTDIR) != 0, "devproc root is a directory");
    spoor_clunk(proc_q);

    territory_unref(pr.territory);   // drops both mount entries + the root
}
