// Kernel rwx-permission enforcement (A-2d; IDENTITY-DESIGN.md sections 3.7.1 +
// 9.6). The Linux-VFS model: the kernel enforces per-file owner/group/other rwx
// at the FS-access chokepoint, because no backing is trusted to (Stratum
// enforces dataset-scope only, not file rwx). I-22 holds: no principal_id
// bypasses the check; only CAP_HOSTOWNER (a console-gated capability, never an
// identity) is the DAC-override.
//
// Enforcement is live for Devs with perm_enforced == true: devramfs
// (system-owned, A-2d) and dev9p (A-3b -- after the host-bake stamps the pool
// PRINCIPAL_SYSTEM-owned + SO_PEERCRED carries the connecting principal, so the
// server-reported uids reconcile with the PRINCIPAL_SYSTEM boot chain). The
// chokepoint skips the check when a Dev's flag is false. See
// <thylacine/dev.h>::Dev.perm_enforced.

#ifndef THYLACINE_PERM_H
#define THYLACINE_PERM_H

#include <thylacine/types.h>
#include <thylacine/handle.h>   // rights_t (rights_for_omode)

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

// rights_for_omode — map an omode to the handle RIGHT_* envelope (A-3b/F1). The
// capability-axis analog of perm_want_for_omode: the returned handle's rights must
// not exceed the access perm_check validated. OEXEC -> RIGHT_READ (no RIGHT_EXEC;
// the handle loads the binary via read). RIGHT_TRANSFER + the T_OPATH born-R|W
// base are caller policy (sys_walk_open_handler), not derived here.
rights_t rights_for_omode(u32 omode);

// perm_wstat_check — the SYS_WSTAT metadata policy (chmod/chown/chgrp/times).
// cur is the file's CURRENT stat (owner, group, mode); valid is the T_WSTAT_*
// field mask; new_gid is the requested group (meaningful only when T_WSTAT_GID
// is set). Returns 0 (allowed) / -1 (denied):
//   MODE -> file owner OR CAP_HOSTOWNER
//   UID  -> CAP_HOSTOWNER or CAP_CHOWN only (no give-away)
//   GID  -> (owner AND member of new_gid) OR CAP_HOSTOWNER or CAP_CHOWN
//   any time other than the touch below -> file owner OR CAP_HOSTOWNER
//   ATIME | MTIME without a _SET bit (both "now") -> also perm_check(p, cur, PERM_W)
//   SIZE -> no policy here (the fd's RIGHT_WRITE, at the syscall layer)
int perm_wstat_check(const struct Proc *p, const struct t_stat *cur, u32 valid,
                     u32 new_gid);

// perm_create_mode — the rwx bits a create sends (IDENTITY-DESIGN 9.7 M8).
// `mode` is the asked mode, `umask` the creator's mask, `dir` the parent's stat,
// or NULL where the Dev does not enforce rwx. First the umask; then Plan 9
// open(5)'s bound: a file's rw bits are ANDed with the directory's and its
// execute bits pass, a directory's rwx are ANDed with the directory's. Only the
// low nine bits of each input count, and only they are returned.
u32 perm_create_mode(u32 mode, bool is_dir, u32 umask, const struct t_stat *dir);

#endif // THYLACINE_PERM_H
