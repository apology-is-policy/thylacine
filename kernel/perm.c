#include <thylacine/perm.h>
#include <thylacine/proc.h>
#include <thylacine/caps.h>
#include <thylacine/syscall.h>
#include <thylacine/handle.h>

bool proc_in_group(const struct Proc *p, u32 gid) {
    if (!p)                  return false;
    if (gid == GID_INVALID)  return false;
    if (gid == p->primary_gid) return true;
    u8 n = p->supp_gid_count;
    if (n > PROC_SUPP_GIDS_MAX) n = PROC_SUPP_GIDS_MAX;
    for (u8 i = 0; i < n; i++)
        if (p->supp_gids[i] == gid) return true;
    return false;
}

int perm_check(const struct Proc *p, const struct t_stat *st, unsigned want) {
    if (!p || !st)           return -1;
    want &= (PERM_R | PERM_W | PERM_X);

    // The DAC-override: a capability, NEVER an identity (I-22). No principal_id
    // -- not even PRINCIPAL_SYSTEM -- is special-cased here. CAP_HOSTOWNER is
    // the unified fs-admin authority; CAP_DAC_OVERRIDE (A-4a) is the finer
    // rwx-bypass split out of it, conferred via a legate clearance grant. Either
    // bypasses the rwx check.
    if (p->caps & (CAP_HOSTOWNER | CAP_DAC_OVERRIDE)) return 0;

    // Owner-first POSIX: an owner is judged on owner bits ONLY (even when group/
    // other would grant more -- it can always chmod itself the bit). The file's
    // uid is PRINCIPAL_INVALID (0) when a Dev could not vouch for it (dev9p F2
    // fail-closed) -- a real principal never matches it, so the owner branch is
    // not taken and the check falls through to group/other.
    unsigned bits;
    if (p->principal_id == st->uid)        bits = (st->mode >> 6) & 7u;  // owner
    else if (proc_in_group(p, st->gid))    bits = (st->mode >> 3) & 7u;  // group
    else                                   bits = st->mode & 7u;         // other

    return (bits & want) == want ? 0 : -1;
}

unsigned perm_want_for_omode(u32 omode) {
    unsigned want;
    switch (omode & 0x3u) {
        case 0:  want = PERM_R;            break;  // OREAD
        case 1:  want = PERM_W;            break;  // OWRITE
        case 2:  want = PERM_R | PERM_W;   break;  // ORDWR
        // OEXEC mints a RIGHT_READ handle (rights_for_omode below), so the
        // identity check MUST require read too -- else execute-only (--x)
        // permission would mint a read-capable handle (RW-3 R3-F1: the
        // execute->read leak on the I-22 chokepoint). Require read AND execute:
        // read because the handle reads the file, execute for the open intent.
        default: want = PERM_R | PERM_X;   break;  // OEXEC (3)
    }
    if (omode & 0x10u) want |= PERM_W;             // OTRUNC truncates -> write
    return want;
}

// rights_for_omode -- map a SYS_WALK_OPEN omode to the handle RIGHT_* envelope
// (A-3b/F1). Parallel to perm_want_for_omode but in the capability bit-space:
// the handle's rights must not exceed the access perm_check validated. OEXEC
// grants RIGHT_READ (the handle loads the binary via read; there is no
// RIGHT_EXEC) -- and perm_want_for_omode(OEXEC) accordingly requires PERM_R
// (RW-3 R3-F1), so the granted right never exceeds the checked access.
// RIGHT_TRANSFER + the T_OPATH born-R|W base are caller policy
// (sys_walk_open_handler), NOT derived here.
rights_t rights_for_omode(u32 omode) {
    rights_t r;
    switch (omode & 0x3u) {
        case 0:  r = RIGHT_READ;                break;  // OREAD
        case 1:  r = RIGHT_WRITE;               break;  // OWRITE
        case 2:  r = RIGHT_READ | RIGHT_WRITE;  break;  // ORDWR
        default: r = RIGHT_READ;                break;  // OEXEC -> read-implied
    }
    if (omode & 0x10u) r |= RIGHT_WRITE;                // OTRUNC -> write
    return r;
}

int perm_wstat_check(const struct Proc *p, u32 cur_uid, u32 valid, u32 new_gid) {
    if (!p) return -1;
    // chmod-any authority: CAP_HOSTOWNER only. There is no finer CAP_FOWNER
    // split at v1.0 -- chmod-by-non-owner stays in the unified authority (the
    // A-4a clearance set is DAC_OVERRIDE/CHOWN/KILL, none of which is chmod).
    bool fowner    = (p->caps & CAP_HOSTOWNER) != 0;
    // chown/chgrp-to-any authority: CAP_HOSTOWNER OR the A-4a CAP_CHOWN (the
    // finer no-give-away chown right split out of CAP_HOSTOWNER, conferable via
    // a legate clearance grant).
    bool chown_any = (p->caps & (CAP_HOSTOWNER | CAP_CHOWN)) != 0;
    bool owner     = (p->principal_id == cur_uid);
    // chmod: only the owner may change a file's mode bits (or chmod-any authority).
    if ((valid & T_WSTAT_MODE) && !owner && !fowner)     return -1;
    // chown(uid): no give-away -- the owner may NOT hand a file to another
    // principal; only chown-any authority may (Plan 9 fileserver-owner /
    // Linux CAP_CHOWN).
    if ((valid & T_WSTAT_UID)  && !chown_any)            return -1;
    // chgrp: the owner may move a file to a group they belong to; chown-any
    // authority to any group.
    if ((valid & T_WSTAT_GID)  && !chown_any &&
        !(owner && proc_in_group(p, new_gid)))           return -1;
    return 0;
}
