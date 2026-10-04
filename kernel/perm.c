#include <thylacine/perm.h>
#include <thylacine/proc.h>
#include <thylacine/caps.h>
#include <thylacine/syscall.h>
#include <thylacine/handle.h>

bool perm_identity_from_proc(const struct Proc *p, struct ProcAccessIdentity *out) {
    if (!out) return false;
    *out = (struct ProcAccessIdentity){0};
    if (!p) return false;
    out->caps = __atomic_load_n(&p->caps, __ATOMIC_ACQUIRE);
    out->principal_id = p->principal_id;
    out->primary_gid = p->primary_gid;
    u8 n = p->supp_gid_count;
    if (n > PROC_SUPP_GIDS_MAX) n = PROC_SUPP_GIDS_MAX;
    out->supp_gid_count = n;
    for (u8 i = 0; i < n; i++) out->supp_gids[i] = p->supp_gids[i];
    return true;
}

bool perm_identity_in_group(const struct ProcAccessIdentity *id, u32 gid) {
    if (!id || gid == GID_INVALID) return false;
    if (gid == id->primary_gid) return true;
    u8 n = id->supp_gid_count;
    if (n > PROC_SUPP_GIDS_MAX) n = PROC_SUPP_GIDS_MAX;
    for (u8 i = 0; i < n; i++)
        if (id->supp_gids[i] == gid) return true;
    return false;
}

bool proc_in_group(const struct Proc *p, u32 gid) {
    struct ProcAccessIdentity id;
    return perm_identity_from_proc(p, &id) && perm_identity_in_group(&id, gid);
}

int perm_check_identity(const struct ProcAccessIdentity *id,
                        const struct t_stat *st, unsigned want) {
    if (!id || !st) return -1;
    want &= (PERM_R | PERM_W | PERM_X);
    // RW-5 R4-F1: an empty permission request fails closed, including for a
    // DAC-override holder. Keep this policy identical for synchronous opens
    // and immutable asynchronous admission snapshots.
    if (want == 0) return -1;
    // I-22: authority is a capability, never a special principal. The Proc
    // wrapper samples caps atomically; a private operation keeps that one
    // admission-time value, never the SQPOLL worker's or a successor's caps.
    if (id->caps & (CAP_HOSTOWNER | CAP_DAC_OVERRIDE)) return 0;
    // POSIX owner-first: do not fall through to more permissive group/other.
    unsigned bits;
    if (id->principal_id == st->uid)             bits = (st->mode >> 6) & 7u;
    else if (perm_identity_in_group(id, st->gid)) bits = (st->mode >> 3) & 7u;
    else                                        bits = st->mode & 7u;
    return (bits & want) == want ? 0 : -1;
}

int perm_check(const struct Proc *p, const struct t_stat *st, unsigned want) {
    struct ProcAccessIdentity id;
    if (!perm_identity_from_proc(p, &id)) return -1;
    return perm_check_identity(&id, st, want);
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
    // RW-5 R4-F2: self-defend. This policy gates exactly {MODE,UID,GID}; a future
    // T_WSTAT_* growth must not pass ungated through here just because the syscall
    // layer happens to reject unknown bits upstream. Fail closed on any bit outside
    // the set this function actually adjudicates.
    if (valid & ~(u32)T_WSTAT_VALID) return -1;
    // caps read ATOMICALLY (RW-5 F2): proc_become_legate is a cross-thread writer
    // of p->caps since A-4a; a plain load is C11-racy.
    caps_t caps = __atomic_load_n(&p->caps, __ATOMIC_ACQUIRE);
    // chmod-any authority: CAP_HOSTOWNER only. There is no finer CAP_FOWNER
    // split at v1.0 -- chmod-by-non-owner stays in the unified authority (the
    // A-4a clearance set is DAC_OVERRIDE/CHOWN/KILL, none of which is chmod).
    bool fowner    = (caps & CAP_HOSTOWNER) != 0;
    // chown/chgrp-to-any authority: CAP_HOSTOWNER OR the A-4a CAP_CHOWN (the
    // finer no-give-away chown right split out of CAP_HOSTOWNER, conferable via
    // a legate clearance grant).
    bool chown_any = (caps & (CAP_HOSTOWNER | CAP_CHOWN)) != 0;
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
    // T_WSTAT_SIZE (Go Stage 5) deliberately has NO policy arm here: a
    // truncate is a CONTENT mutation whose write authority is the fd's
    // RIGHT_WRITE (enforced at the syscall layer) + the open-time perm_check
    // W axis -- the POSIX ftruncate model, not an identity-policy op. The
    // self-defend mask above admits it (it is inside T_WSTAT_VALID); it
    // passes through untainted while the metadata policy applies to any
    // combined bits.
    return 0;
}
