// Per-Proc capability bits (P4-Ib).
//
// Per ARCHITECTURE.md §13 (capabilities) + specs/handles.tla. A capability
// is a per-Proc unforgeable bit that gates access to privileged kernel
// operations (creating hardware handles, modifying namespace mounts,
// signaling other procs, etc.). Capabilities monotonically REDUCE per
// ARCH §28 I-2: a Proc can drop bits (rfork mask AND) but never gain
// them post-creation.
//
// Initial allocation: kproc (PID 0) starts with CAP_ALL — the kernel
// is the root of trust. Plain rfork() confers CAP_NONE; rfork_with_caps
// confers (parent->caps & caps_mask) — a subset of the parent's caps
// (the v1.0 boot path uses it to hand joey CAP_ALL so joey can delegate
// caps to the children it spawns). A userspace capability-grant syscall
// for parent→child delegation is a Phase 5+ item. Drivers (P4-Ic) are
// spawned with CAP_HW_CREATE via rfork_with_caps; at v1.0 the only proc
// that creates hw handles is kproc-context kernel test code.

#ifndef THYLACINE_CAPS_H
#define THYLACINE_CAPS_H

#include <thylacine/types.h>

typedef u64 caps_t;

// CAP_HW_CREATE — required to call SYS_MMIO_CREATE / SYS_IRQ_CREATE /
// (future) SYS_DMA_CREATE. Holders can claim hardware resources (PA
// ranges, INTIDs, DMA channels). Maps to specs/handles.tla::CapHwCreate.
#define CAP_HW_CREATE   (1ull << 0)

// CAP_LOCK_PAGES — required to call SYS_MLOCKALL (P5-corvus-syscalls;
// CORVUS-DESIGN.md §4.1.1). Holders can pin pages to prevent swap-out.
// v1.0 has no swap; the cap + syscall are forward-looking scaffolding
// consumed by corvus + per-user stratumd at startup. kproc + corvus
// + per-user stratumd hold this cap; ordinary user procs do not.
#define CAP_LOCK_PAGES  (1ull << 1)

// CAP_CSPRNG_READ — required to call SYS_GETRANDOM (P5-corvus-syscalls;
// CORVUS-DESIGN.md §4.1.1). Holders can read from the kernel CSPRNG.
// Granted broadly at v1.0 (most userspace processes have legitimate
// use for randomness — session tokens, AEAD nonces, salts). The cap
// exists for forward-compat (a future hardened-deployment may revoke
// it from specific procs).
#define CAP_CSPRNG_READ (1ull << 2)

// CAP_HOSTOWNER — admin authority (CORVUS-DESIGN.md §3 D5). Gates the
// corvus admin verbs (user-create / user-delete / snapshot / kernel-
// update). Unlike the caps above, CAP_HOSTOWNER is *elevation-only*:
// it is deliberately NOT part of CAP_ALL, so no Proc — not even kproc —
// holds it at creation, and rfork's mask-AND can never confer it. The
// only path to CAP_HOSTOWNER is corvus's ADMIN_ELEVATE verb, which
// grants it to a Proc after verifying the system passphrase from a
// console-attached session (specs/corvus.tla AdminElevate; the
// HostownerRequiresConsole invariant ties it to the kernel-stamped
// PROC_FLAG_CONSOLE_ATTACHED bit — see <thylacine/proc.h>).
// P5-hostowner-a defines the bit + the console-attachment gate; the
// grant mechanism + the ADMIN_ELEVATE verb land at P5-hostowner-b.
#define CAP_HOSTOWNER   (1ull << 3)

// CAP_GRANT_HOSTOWNER — authorizes writing the `cap` device's `grant`
// file (registering a pending CAP_HOSTOWNER grant for a peer Proc).
// Unlike CAP_HOSTOWNER — the elevation *result* — this is an ordinary
// FORK-GRANTABLE capability, a member of CAP_ALL. joey holds it (via
// CAP_ALL) and confers it on corvus alone in corvus's spawn mask; no
// ordinary user Proc receives it. The two-capability split keeps "who
// may register a grant" (CAP_GRANT_HOSTOWNER — corvus) distinct from
// "who has been elevated" (CAP_HOSTOWNER — a console session). See
// CORVUS-DESIGN.md §5.5.1 + specs/handles.tla. P5-hostowner-b adds the
// `cap` device that consumes it; the bit is defined here as foundation.
#define CAP_GRANT_HOSTOWNER (1ull << 4)

// CAP_SET_IDENTITY — authorizes setting a SPAWNED child's identity
// (principal_id / primary_gid / supplementary gids) to anything other
// than the parent's inherited identity, via SPAWN_IDENTITY_SET in
// struct sys_spawn_args (SYS_SPAWN_FULL_ARGV). This is the setuid-
// equivalent: a holder can mint a process running as any user. It is
// the identity counterpart of "elevation = gain a cap" — but for the
// IDENTITY axis, not the CAPABILITY axis (the two are orthogonal per
// I-22: setting a child's identity confers no caps; caps still flow
// only through cap_mask). FORK-GRANTABLE (a member of CAP_ALL): it
// flows kproc -> joey -> /sbin/login down the vetted boot chain so
// login can spawn each user's shell *born with* that user's identity.
// An ordinary user Proc never holds it (login omits it from the
// shell's spawn cap_mask), so a user cannot spawn processes as another
// user. The gate is FAIL-CLOSED: a SPAWN_IDENTITY_SET request from a
// caller lacking this cap returns -1, never silently inherits.
// (docs/IDENTITY-DESIGN.md §3.3 + §9.1; ARCH §28 I-22.)
#define CAP_SET_IDENTITY    (1ull << 5)

// CAP_GRANT_CLEARANCE — authorizes writing the `cap` device's clearance
// grant file (registering a pending A-4 *clearance* grant for a peer
// Proc: {cap_mask, target_stripes, valid_until, session_id}). The legate
// analog of CAP_GRANT_HOSTOWNER: an ordinary FORK-GRANTABLE capability (a
// member of CAP_ALL), conferred on corvus alone via its spawn mask; no
// ordinary user Proc receives it. corvus verifies the clearance level's
// auth_required (the trusted path) BEFORE registering the grant; the
// kernel cap-stamp at redeem is the enforcement. Unlike the hostowner
// grant, redeeming a clearance grant does NOT require console attachment
// (high-stakes auth is corvus-side). CORVUS-DESIGN.md §5.5.1 + §5.7 +
// docs/IDENTITY-DESIGN.md §9.8 (A-4a).
#define CAP_GRANT_CLEARANCE (1ull << 6)

// CAP_DAC_OVERRIDE — elevation-only. The fs-admin DAC-override (the
// kernel/perm.c rwx-check bypass) split out of CAP_HOSTOWNER so it can be
// conferred as a finer clearance (IDENTITY-DESIGN.md §3.7.1 + §9.8). A
// holder may traverse/read/write any path regardless of owner/group/other
// bits. Acquired ONLY through the `cap` device (a clearance grant); never
// by rfork (it is in CAP_ELEVATION_ONLY).
#define CAP_DAC_OVERRIDE    (1ull << 7)

// CAP_CHOWN — elevation-only. chown/chgrp-to-any-owner, split out of
// CAP_HOSTOWNER (the no-give-away chown authority). Acquired ONLY through
// the `cap` device; rfork-stripped. IDENTITY-DESIGN.md §3.7.1 + §9.8.
#define CAP_CHOWN           (1ull << 8)

// CAP_KILL — elevation-only. The cross-identity kill override: the third
// authority axis on /proc/<pid>/ctl (owner-rwx OR CAP_HOSTOWNER OR
// CAP_KILL; A-4b / I-26). Deliberately elevation-only, NOT fork-grantable
// — a kill-anyone right must not leak to a legate's children; the
// supervisor/debugger Proc itself holds it, and killing your OWN children
// never needs it (parent authority covers that). Acquired ONLY through the
// `cap` device; rfork-stripped. IDENTITY-DESIGN.md §9.8 (A-4b).
#define CAP_KILL            (1ull << 9)

// CAP_DEBUG — elevation-only. The cross-Proc debug authority (I-39): the
// capability axis on the /proc/<pid> debug surface (owner-on-the-0600-ctl OR
// CAP_DEBUG), the I-26 analog for the read/write/run-control axis. A holder
// may attach a debugger to, stop, and inspect/modify the registers + memory
// of a STOPPED target it can name in its namespace. Clearance-grantable (a
// member of CAP_GRANTABLE_CLEARANCE) so a dev-session debugger acquires it via
// a corvus-mediated, scope- and time-bounded legate — exactly like CAP_KILL /
// CAP_DAC_OVERRIDE / CAP_CHOWN. Elevation-only (rfork-stripped): a
// debug-anything right must not leak to a legate's children; the debugger Proc
// itself holds it, and debugging your OWN-identity target never needs it
// (owner-on-0600 covers that). Never bypasses memory-safety (I-12 W^X + I-13
// isolation hold — breakpoints are hardware, never a software BRK patched into
// shared text). docs/DEBUG-FS-DESIGN.md; Go IDE Stage 8a.
#define CAP_DEBUG           (1ull << 10)

// Reserved for Phase 5+ (one bit per capability domain; next free bit is
// 1<<11):
//   CAP_NS_MOUNT     — bind/mount in /proc and /ctl (kernel admin Devs).
//   CAP_NS_BIND      — bind in any namespace (forward-looking).
//   CAP_NET_RAW      — open raw network sockets / Ethernet frames.
//   CAP_TIME_SET     — modify system clock.
//   CAP_REBOOT       — initiate kernel reboot / extinction.
// (CAP_SIGNAL_ANY is realized as CAP_KILL = 1<<9 above — the cross-
// identity kill override.) Each lands when its subsystem matures.

// CAP_ELEVATION_ONLY — the set of elevation-only capability bits: those
// excluded from CAP_ALL that no Proc holds at creation and that rfork
// MUST strip from every child, so an elevated parent cannot leak
// elevation across a fork. rfork_internal ANDs the child's caps with
// ~CAP_ELEVATION_ONLY (A-4-pre). All five are acquired ONLY through the
// `cap` device: CAP_HOSTOWNER (the unified fs-admin authority) plus the
// A-4 finer caps split out of it — CAP_DAC_OVERRIDE, CAP_CHOWN, CAP_KILL —
// plus CAP_DEBUG (the Stage-8a cross-Proc debug authority).
// Maps to specs/handles.tla::ElevationOnly.
#define CAP_ELEVATION_ONLY  (CAP_HOSTOWNER | CAP_DAC_OVERRIDE | CAP_CHOWN | CAP_KILL | CAP_DEBUG)

// CAP_ALL — the FORK-GRANTABLE capability ceiling: every capability a
// Proc may legitimately hold from creation, and the mask kproc gets at
// proc_init. Elevation-only capabilities (CAP_ELEVATION_ONLY:
// CAP_HOSTOWNER, CAP_DAC_OVERRIDE, CAP_CHOWN, CAP_KILL) are deliberately
// excluded — see above. A new fork-grantable CAP_* bit MUST be added
// here; an elevation-only one MUST NOT.
#define CAP_ALL         (CAP_HW_CREATE | CAP_LOCK_PAGES | CAP_CSPRNG_READ | CAP_GRANT_HOSTOWNER | CAP_SET_IDENTITY | CAP_GRANT_CLEARANCE)

// _Static_assert pins CAP_ALL — adding a new fork-grantable CAP_* bit
// requires bumping this expression so kproc's initial mask includes it.
_Static_assert(CAP_ALL == (CAP_HW_CREATE | CAP_LOCK_PAGES | CAP_CSPRNG_READ | CAP_GRANT_HOSTOWNER | CAP_SET_IDENTITY | CAP_GRANT_CLEARANCE),
               "caps.h drift: when adding a new FORK-GRANTABLE CAP_* bit, "
               "update CAP_ALL so kproc's initial mask reflects it. "
               "Elevation-only caps (CAP_ELEVATION_ONLY) are deliberately "
               "excluded from CAP_ALL.");

// CAP_ALL and CAP_ELEVATION_ONLY are disjoint by construction — a bit is
// either fork-grantable or elevation-only, never both. Pin it.
_Static_assert((CAP_ALL & CAP_ELEVATION_ONLY) == 0,
               "caps.h drift: a CAP_* bit is in BOTH CAP_ALL and "
               "CAP_ELEVATION_ONLY. Each bit is fork-grantable XOR "
               "elevation-only — never both.");

// CAP_NONE — empty capability mask. The default for rfork'd children
// at v1.0 (Phase 5+ inherits parent's mask AND'd with rfork's caps_mask
// argument).
#define CAP_NONE        0ull

// R9 F150 (P3 deferral) — forward-looking implementer note.
// specs/handles.tla::ReduceCaps precondition forbids dropping
// CAP_HW_CREATE while p holds any hw handle. At v1.0 there is no
// cap-drop syscall; the spec invariant `HwHandleImpliesCap` is
// preserved trivially. When the cap-drop / rfork-mask syscall lands
// (Phase 5+), it MUST refuse the drop with -EBUSY if `p->caps & ~mask`
// would clear CAP_HW_CREATE AND any `h \in p->handles` has
// `kobj_kind_is_hw(h->kind)`. Without that check, the impl would
// admit states the spec forbids — a proc holding hw handles after the
// cap that authorized them was dropped.

#endif // THYLACINE_CAPS_H
