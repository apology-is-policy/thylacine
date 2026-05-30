// Kernel rwx-permission enforcement (A-2d; IDENTITY-DESIGN.md sections 3.7.1 +
// 9.6). The Linux-VFS model: the kernel enforces per-file owner/group/other rwx
// at the FS-access chokepoint, because no backing is trusted to (Stratum
// enforces dataset-scope only, not file rwx). I-22 holds: no principal_id
// bypasses the check; only CAP_HOSTOWNER (a console-gated capability, never an
// identity) is the DAC-override.
//
// At v1.0 enforcement is live only for Devs with perm_enforced == true
// (devramfs -- system-owned). dev9p enforcement is deferred to A-3 (its stored
// uids are the connection identity / host-baked, not yet reconciled with the
// PRINCIPAL_SYSTEM boot chain); the chokepoint skips the check when the Dev's
// flag is false. See <thylacine/dev.h>::Dev.perm_enforced.

#ifndef THYLACINE_PERM_H
#define THYLACINE_PERM_H

#include <thylacine/types.h>

struct Proc;
struct t_stat;

// Permission bits, positioned to match the rwx triple in a POSIX mode so a
// `(mode_triple & want) == want` test reads directly. NOT a public ABI.
#define PERM_X  1u
#define PERM_W  2u
#define PERM_R  4u

// proc_in_group — is `gid` one of p's groups (primary or supplementary)?
// GID_INVALID is never a member.
bool proc_in_group(const struct Proc *p, u32 gid);

// perm_check — owner-first POSIX check of p against the file described by st.
// `want` is a subset of PERM_R|PERM_W|PERM_X. Returns 0 (allowed) / -1 (denied).
//   - (p->caps & CAP_HOSTOWNER) short-circuits to 0 (the DAC-override; I-22:
//     a capability, never an identity -- no principal_id bypasses).
//   - else owner bits if p->principal_id == st->uid; else group bits if
//     proc_in_group(p, st->gid); else other bits. Owner-first is authoritative.
int perm_check(const struct Proc *p, const struct t_stat *st, unsigned want);

// perm_want_for_omode — map a SYS_WALK_OPEN omode (Plan 9: OREAD=0 / OWRITE=1 /
// ORDWR=2 / OEXEC=3, OTRUNC=0x10) to the rwx bits an open requires.
unsigned perm_want_for_omode(u32 omode);

// perm_wstat_check — the SYS_WSTAT ownership-change policy (chmod/chown/chgrp).
// cur_uid is the file's CURRENT owner; valid is the T_WSTAT_* field mask; new_gid
// is the requested group (meaningful only when T_WSTAT_GID is set). Returns 0
// (allowed) / -1 (denied):
//   MODE -> file owner OR CAP_HOSTOWNER
//   UID  -> CAP_HOSTOWNER only (no give-away)
//   GID  -> (owner AND member of new_gid) OR CAP_HOSTOWNER
int perm_wstat_check(const struct Proc *p, u32 cur_uid, u32 valid, u32 new_gid);

#endif // THYLACINE_PERM_H
