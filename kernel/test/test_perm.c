// Kernel rwx-permission enforcement tests (A-2d; IDENTITY-DESIGN.md 3.7.1 + 9.6).
//
// Unit tables for perm_check / proc_in_group / perm_want_for_omode /
// perm_wstat_check (the policy helper -- dormant in production at v1.0 since
// devramfs has no wstat_native and dev9p is deferred, so the unit test is its
// coverage), plus a real-metadata integration test against the devramfs initrd
// (enforcement is LIVE there -- system-owned, world-r/x) and a guard on the
// Dev.perm_enforced flags (devramfs true, dev9p deferred-to-A-3 false).

#include "test.h"

#include <thylacine/perm.h>
#include <thylacine/proc.h>
#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>

extern struct Dev devramfs;
extern struct Dev dev9p;

void test_perm_check_owner_group_other(void);
void test_perm_check_owner_first_authoritative(void);
void test_perm_check_hostowner_override(void);
void test_perm_in_group(void);
void test_perm_want_for_omode(void);
void test_perm_rights_for_omode(void);
void test_perm_oexec_no_read_leak(void);
void test_perm_wstat_policy(void);
void test_perm_devramfs_enforced_real_metadata(void);
void test_perm_dev_flags(void);
void test_perm_check_dac_override_cap(void);
void test_perm_wstat_chown_cap(void);

// =============================================================================
// Helpers.
// =============================================================================

// A synthetic Proc carrying only the identity + caps the perm functions read.
static void mkproc(struct Proc *p, u32 principal, u32 pgid, u64 caps) {
    for (size_t i = 0; i < sizeof(*p); i++) ((u8 *)p)[i] = 0;
    p->principal_id   = principal;
    p->primary_gid    = pgid;
    p->supp_gid_count = 0;
    p->caps           = caps;
}

static struct t_stat mkstat(u32 mode, u32 uid, u32 gid) {
    struct t_stat s;
    for (size_t i = 0; i < sizeof(s); i++) ((u8 *)&s)[i] = 0;
    s.mode = mode;
    s.uid  = uid;
    s.gid  = gid;
    return s;
}

// =============================================================================
// perm_check — owner / group / other selection.
// =============================================================================

void test_perm_check_owner_group_other(void) {
    // 0640: owner rw-, group r--, other ---. uid 100, gid 200.
    struct t_stat st = mkstat(0640u, 100u, 200u);
    struct Proc p;

    // Owner (principal == uid) judged on owner bits (rw).
    mkproc(&p, 100u, 999u, CAP_NONE);
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R), 0,  "owner R allowed");
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_W), 0,  "owner W allowed");
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_X), -1, "owner X denied (no x bit)");

    // Group member (primary_gid == gid) judged on group bits (r only).
    mkproc(&p, 555u, 200u, CAP_NONE);
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R), 0,  "group R allowed");
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_W), -1, "group W denied");

    // Other (not owner, not in group) judged on other bits (none).
    mkproc(&p, 555u, 888u, CAP_NONE);
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R), -1, "other R denied");

    // Supplementary group membership also selects the group branch.
    mkproc(&p, 555u, 888u, CAP_NONE);
    p.supp_gids[0] = 200u;
    p.supp_gid_count = 1;
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R), 0, "supp-group R allowed");
}

void test_perm_check_owner_first_authoritative(void) {
    // 0046: owner ---, group r--, other rw-. The OWNER is more restricted than
    // group/other -- owner-first POSIX means the owner is judged on owner bits
    // ONLY, even when group/other would grant more.
    struct t_stat st = mkstat(0046u, 100u, 200u);
    struct Proc p;

    mkproc(&p, 100u, 999u, CAP_NONE);             // the owner
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R), -1,
                   "owner R denied despite group/other r (owner-first)");

    mkproc(&p, 555u, 200u, CAP_NONE);             // a group member
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R), 0, "group member R allowed");

    mkproc(&p, 555u, 888u, CAP_NONE);             // other
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_W), 0, "other W allowed (0046)");
}

void test_perm_check_hostowner_override(void) {
    // mode 0000 -- nobody gets anything by identity. CAP_HOSTOWNER is the
    // DAC-override (a capability, never an identity: I-22).
    struct t_stat st = mkstat(0000u, 100u, 200u);
    struct Proc p;

    mkproc(&p, 555u, 888u, CAP_NONE);
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R), -1, "no-cap denied on 0000");

    mkproc(&p, 555u, 888u, CAP_HOSTOWNER);
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R | PERM_W | PERM_X), 0,
                   "CAP_HOSTOWNER overrides 0000");

    // PRINCIPAL_SYSTEM is NOT special-cased -- only the capability is (I-22).
    mkproc(&p, PRINCIPAL_SYSTEM, GID_SYSTEM, CAP_NONE);
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R), -1,
                   "PRINCIPAL_SYSTEM gets no ambient override");
}

void test_perm_in_group(void) {
    struct Proc p;
    mkproc(&p, 1u, 50u, CAP_NONE);
    p.supp_gids[0] = 60u;
    p.supp_gids[1] = 70u;
    p.supp_gid_count = 2;

    TEST_ASSERT(proc_in_group(&p, 50u),  "primary gid is a member");
    TEST_ASSERT(proc_in_group(&p, 60u),  "supp gid[0] is a member");
    TEST_ASSERT(proc_in_group(&p, 70u),  "supp gid[1] is a member");
    TEST_ASSERT(!proc_in_group(&p, 80u), "unrelated gid is not a member");
    TEST_ASSERT(!proc_in_group(&p, GID_INVALID),
                "GID_INVALID is never a member");
}

void test_perm_want_for_omode(void) {
    TEST_EXPECT_EQ(perm_want_for_omode(0u), PERM_R,            "OREAD -> R");
    TEST_EXPECT_EQ(perm_want_for_omode(1u), PERM_W,            "OWRITE -> W");
    TEST_EXPECT_EQ(perm_want_for_omode(2u), PERM_R | PERM_W,   "ORDWR -> R|W");
    TEST_EXPECT_EQ(perm_want_for_omode(3u), PERM_R | PERM_X,   "OEXEC -> R|X (handle is read-capable; RW-3 R3-F1)");
    TEST_EXPECT_EQ(perm_want_for_omode(0u | 0x10u), PERM_R | PERM_W,
                   "OREAD|OTRUNC -> R|W (truncate writes)");
    TEST_EXPECT_EQ(perm_want_for_omode(1u | 0x10u), PERM_W,
                   "OWRITE|OTRUNC -> W");
}

void test_perm_rights_for_omode(void) {
    // A-3b/F1: the handle RIGHT_* envelope derived from omode (capability-axis
    // analog of want_for_omode). Note OEXEC -> RIGHT_READ (no RIGHT_EXEC; the
    // handle loads the binary via read).
    TEST_EXPECT_EQ(rights_for_omode(0u), RIGHT_READ,               "OREAD -> R");
    TEST_EXPECT_EQ(rights_for_omode(1u), RIGHT_WRITE,              "OWRITE -> W");
    TEST_EXPECT_EQ(rights_for_omode(2u), RIGHT_READ | RIGHT_WRITE, "ORDWR -> R|W");
    TEST_EXPECT_EQ(rights_for_omode(3u), RIGHT_READ,               "OEXEC -> R (read-implied)");
    TEST_EXPECT_EQ(rights_for_omode(0u | 0x10u), RIGHT_READ | RIGHT_WRITE,
                   "OREAD|OTRUNC -> R|W (truncate writes)");
    // RIGHT_TRANSFER is caller policy (sys_walk_open_handler), never from the map.
    TEST_ASSERT((rights_for_omode(2u) & RIGHT_TRANSFER) == 0,
                "rights_for_omode never sets TRANSFER");
}

// RW-3 R3-F1 regression: an OEXEC open mints a RIGHT_READ handle
// (rights_for_omode), so perm_check on the OEXEC `want` MUST require read --
// else execute-only (--x) permission yields a read-capable handle, the
// execute->read leak on the I-22 chokepoint. Fails pre-fix (perm_want_for_omode
// returned PERM_X, so the x-only file passed and a readable handle was minted).
void test_perm_oexec_no_read_leak(void) {
    struct Proc p;
    unsigned want = perm_want_for_omode(3u);   // OEXEC

    // Owner of an execute-only file (0100 = --x------): pre-fix this passed
    // (PERM_X satisfied) and the open minted a readable handle; post-fix denied.
    struct t_stat x_only = mkstat(0100u, 100u, 200u);
    mkproc(&p, 100u, 999u, CAP_NONE);
    TEST_EXPECT_EQ(perm_check(&p, &x_only, want), -1,
                   "OEXEC on an execute-only file is denied (no execute->read leak)");

    // Owner of an r-x file (0500 = r-x------): a normal executable -- OEXEC
    // allowed (read present, so the read-capable handle is legitimate).
    struct t_stat rx = mkstat(0500u, 100u, 200u);
    mkproc(&p, 100u, 999u, CAP_NONE);
    TEST_EXPECT_EQ(perm_check(&p, &rx, want), 0,
                   "OEXEC on an r-x file is allowed");

    // Owner of a read-only file (0400 = r--------): read present but no execute
    // bit -- denied (execute intent unmet; confirms want includes PERM_X).
    struct t_stat r_only = mkstat(0400u, 100u, 200u);
    mkproc(&p, 100u, 999u, CAP_NONE);
    TEST_EXPECT_EQ(perm_check(&p, &r_only, want), -1,
                   "OEXEC on a read-only file is denied (no execute bit)");
}

// =============================================================================
// perm_wstat_check — the chmod/chown/chgrp ownership-change policy.
// =============================================================================

void test_perm_wstat_policy(void) {
    struct Proc p;

    // chmod: owner allowed, non-owner denied, CAP_HOSTOWNER allowed.
    mkproc(&p, 100u, 200u, CAP_NONE);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_MODE, 0u), 0,
                   "owner may chmod");
    mkproc(&p, 999u, 200u, CAP_NONE);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_MODE, 0u), -1,
                   "non-owner may not chmod");
    mkproc(&p, 999u, 200u, CAP_HOSTOWNER);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_MODE, 0u), 0,
                   "CAP_HOSTOWNER may chmod any file");

    // chown(uid): no give-away -- owner denied, only CAP_HOSTOWNER.
    mkproc(&p, 100u, 200u, CAP_NONE);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_UID, 0u), -1,
                   "owner may NOT give a file away");
    mkproc(&p, 100u, 200u, CAP_HOSTOWNER);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_UID, 0u), 0,
                   "CAP_HOSTOWNER may chown");

    // chgrp: owner -> a group they belong to; foreign group denied; hostowner any.
    mkproc(&p, 100u, 200u, CAP_NONE);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_GID, 200u), 0,
                   "owner may chgrp to its primary group");
    mkproc(&p, 100u, 200u, CAP_NONE);
    p.supp_gids[0] = 300u; p.supp_gid_count = 1;
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_GID, 300u), 0,
                   "owner may chgrp to a supplementary group");
    mkproc(&p, 100u, 200u, CAP_NONE);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_GID, 777u), -1,
                   "owner may NOT chgrp to a foreign group");
    mkproc(&p, 999u, 200u, CAP_HOSTOWNER);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_GID, 777u), 0,
                   "CAP_HOSTOWNER may chgrp to any group");
}

// =============================================================================
// Integration: enforcement against the real devramfs initrd metadata.
// =============================================================================

void test_perm_devramfs_enforced_real_metadata(void) {
    struct Spoor *root = devramfs.attach("");
    TEST_ASSERT(root != NULL, "devramfs attach OK");
    TEST_ASSERT(root->dev->stat_native != NULL, "devramfs has stat_native");

    struct t_stat rst;
    TEST_EXPECT_EQ(root->dev->stat_native(root, &rst), 0, "stat root OK");
    TEST_EXPECT_EQ(rst.uid, (u32)PRINCIPAL_SYSTEM, "root owned by SYSTEM");
    TEST_EXPECT_EQ(rst.gid, (u32)GID_SYSTEM,       "root group SYSTEM");
    TEST_EXPECT_EQ(rst.mode & 0777u, 0555u,        "root mode 0555 (world r-x)");

    struct Proc sysp, userp;
    mkproc(&sysp,  PRINCIPAL_SYSTEM, GID_SYSTEM, CAP_NONE);
    mkproc(&userp, 1000u,            1000u,      CAP_NONE);

    // 0555: owner == other == r-x. Everyone may traverse + read; NOBODY may
    // write (it is a read-only FS -- even the system owner has no w bit).
    TEST_EXPECT_EQ(perm_check(&sysp,  &rst, PERM_X), 0,  "system traverses root");
    TEST_EXPECT_EQ(perm_check(&userp, &rst, PERM_X), 0,  "user traverses root");
    TEST_EXPECT_EQ(perm_check(&userp, &rst, PERM_R), 0,  "user reads root");
    TEST_EXPECT_EQ(perm_check(&sysp,  &rst, PERM_W), -1, "system cannot write root");
    TEST_EXPECT_EQ(perm_check(&userp, &rst, PERM_W), -1, "user cannot write root");

    // Walk a real file and prove the owner-vs-other branch selection runs off
    // its actual stored mode (robust to the exact cpio mode).
    const char *names[1] = { "welcome" };
    struct Walkqid *wq = devramfs.walk(root, NULL, names, 1);
    spoor_unref(root);
    TEST_ASSERT(wq != NULL && wq->nqid == 1, "walk welcome OK");
    struct Spoor *f = wq->spoor;
    walkqid_free(wq);

    struct t_stat fst;
    TEST_EXPECT_EQ(f->dev->stat_native(f, &fst), 0, "stat welcome OK");
    spoor_unref(f);
    TEST_EXPECT_EQ(fst.uid, (u32)PRINCIPAL_SYSTEM, "welcome owned by SYSTEM");

    unsigned owner_r = ((fst.mode >> 6) & PERM_R) ? 0 : -1;  // expected for owner
    unsigned other_r = (fst.mode & PERM_R)        ? 0 : -1;  // expected for other
    TEST_EXPECT_EQ((unsigned)perm_check(&sysp,  &fst, PERM_R), owner_r,
                   "system (owner) R matches owner bits");
    TEST_EXPECT_EQ((unsigned)perm_check(&userp, &fst, PERM_R), other_r,
                   "user (other) R matches other bits");
}

void test_perm_dev_flags(void) {
    TEST_ASSERT(devramfs.perm_enforced,
                "devramfs enforces rwx (system-owned, live at v1.0)");
    TEST_ASSERT(dev9p.perm_enforced,
                "dev9p enforces rwx (A-3b: pool SYSTEM-owned + SO_PEERCRED-principal)");
}

// =============================================================================
// A-4a: the finer fs-admin caps split out of CAP_HOSTOWNER. Pin each cap to
// exactly the axis it authorizes (DAC_OVERRIDE = rwx bypass; CHOWN =
// chown/chgrp-any; neither grants the other's authority; chmod-any stays
// CAP_HOSTOWNER-only -- no CAP_FOWNER split at v1.0).
// =============================================================================

void test_perm_check_dac_override_cap(void) {
    // mode 0000 -- no identity grants anything. CAP_DAC_OVERRIDE is the rwx
    // bypass split out of CAP_HOSTOWNER; it overrides like CAP_HOSTOWNER.
    struct t_stat st = mkstat(0000u, 100u, 200u);
    struct Proc p;

    mkproc(&p, 555u, 888u, CAP_DAC_OVERRIDE);
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R | PERM_W | PERM_X), 0,
                   "CAP_DAC_OVERRIDE overrides rwx like CAP_HOSTOWNER");

    // CAP_CHOWN authorizes chown/chgrp, NOT an rwx bypass.
    mkproc(&p, 555u, 888u, CAP_CHOWN);
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R), -1,
                   "CAP_CHOWN does NOT override the rwx check");

    // CAP_KILL is a kill axis, never an fs DAC override.
    mkproc(&p, 555u, 888u, CAP_KILL);
    TEST_EXPECT_EQ(perm_check(&p, &st, PERM_R), -1,
                   "CAP_KILL does NOT override the rwx check");
}

void test_perm_wstat_chown_cap(void) {
    struct Proc p;

    // chown(uid): CAP_CHOWN authorizes it (the no-give-away chown right), like
    // CAP_HOSTOWNER.
    mkproc(&p, 999u, 200u, CAP_CHOWN);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_UID, 0u), 0,
                   "CAP_CHOWN may chown");
    // chgrp to any group: CAP_CHOWN authorizes it.
    mkproc(&p, 999u, 200u, CAP_CHOWN);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_GID, 777u), 0,
                   "CAP_CHOWN may chgrp to any group");

    // chmod-any stays CAP_HOSTOWNER-only: neither CAP_CHOWN nor CAP_DAC_OVERRIDE
    // grants chmod of another principal's file (no CAP_FOWNER split at v1.0).
    mkproc(&p, 999u, 200u, CAP_CHOWN);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_MODE, 0u), -1,
                   "CAP_CHOWN does NOT grant chmod of another's file");
    mkproc(&p, 999u, 200u, CAP_DAC_OVERRIDE);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_MODE, 0u), -1,
                   "CAP_DAC_OVERRIDE does NOT grant chmod of another's file");
    // CAP_DAC_OVERRIDE does NOT grant chown (that authority is CAP_CHOWN/HOSTOWNER).
    mkproc(&p, 999u, 200u, CAP_DAC_OVERRIDE);
    TEST_EXPECT_EQ(perm_wstat_check(&p, 100u, T_WSTAT_UID, 0u), -1,
                   "CAP_DAC_OVERRIDE does NOT grant chown");
}
