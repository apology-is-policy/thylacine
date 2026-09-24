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
// clearance-grantable cross-identity axis on the /proc/<pid> debug surface
// (owner-on-the-0600-ctl OR CAP_HOSTOWNER OR CAP_DEBUG — the I-26 analog for the
// read/write/run-control axis, where CAP_HOSTOWNER is the host-owner/eve axis
// and CAP_DEBUG the domain cap, exactly as kill is owner OR HOSTOWNER OR KILL).
// A holder
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

// CAP_JIT — elevation-only. The authority to create a CODE Burrow: the
// dual-mapped executable region (RW at VA_w, RX at VA_x) that is the ONLY
// path by which userspace-emitted bytes ever become executable (I-42;
// docs/JIT-ON-WX-DESIGN.md, realized by the Clade arc — LLVM-DESIGN.md §8).
// A holder may call SYS_JIT_CREATE; nothing else in the tree consults it.
//
// ELEVATION-ONLY, and that is load-bearing, not stylistic: I-42's own text
// requires the capability be "non-heritable", so it MUST be rfork-stripped —
// which is exactly what membership in CAP_ELEVATION_ONLY (and exclusion from
// CAP_ALL) delivers. Clearance-grantable like CAP_DEBUG, so a Proc that needs
// to JIT acquires it through a corvus-mediated, scope- and time-bounded
// legate rather than by inheritance.
//
// NOTE on the scripture wording: JIT-ON-WX-DESIGN.md, LLVM-DESIGN.md §8 and
// ARCH §28 I-42 each describe CAP_JIT as "elevation-only, non-rfork-grantable,
// the CAP_HW_CREATE class". The first two properties are unambiguous and are
// what is implemented here. The trailing phrase is a loose analogy meaning
// "gates a powerful create operation" — read literally it is WRONG, because
// CAP_HW_CREATE is fork-grantable (a member of CAP_ALL), which would directly
// contradict both "non-rfork-grantable" beside it and I-42's "non-heritable"
// clause. Do not "fix" this bit toward CAP_ALL on the strength of that phrase.
//
// What it does NOT confer (the JIT-ON-WX caveat #1, as policy): CAP_JIT
// controls WHO MAY EMIT code and WHERE (a specific Burrow, never arbitrary
// process memory). It says nothing about what the emitted code may DO — JITed
// code runs with the Proc's own capability set and namespace, no more. For
// untrusted JIT the confinement instrument is the namespace, not this bit.
#define CAP_JIT             (1ull << 11)

// CAP_AUDIO_GRAPH — elevation-only. The Nocturne whole-sink authority (I-46
// candidate; docs/NOCTURNE.md §6.8): the clearance-grantable axis on the
// SYSTEM-owned parts of the audio graph. A holder may operate on the shared
// sink beyond its own voices — set the sink `volume`/`default`, insert a
// descant at a sink's input (the "system EQ" case), and read a tap on the
// sink (`/dev/nocturne/audio` loopback; recording is eavesdropping otherwise).
// Own-voice work (mint/write/gain/tap your OWN voice) is the owner axis and
// needs NO clearance — this bit is only the whole-sink, cross-owner authority.
// The two-axis rule of I-26/I-39: the sink volume is (console-owner OR this
// clearance); the tap/insert is (owner-of-the-target OR this clearance).
// Clearance-grantable (a member of CAP_GRANTABLE_CLEARANCE) so a system-level
// audio program acquires it via a corvus-mediated, scope-bounded legate —
// exactly like CAP_DEBUG / CAP_JIT — never by inheritance. Elevation-only
// (rfork-stripped): whole-sink authority must not leak to a child. Checked by
// nocturned via SYS_SRV_PEER's live caps word; nothing in the kernel consults
// it (the audio authority lives in nocturned, the sink's owner).
#define CAP_AUDIO_GRAPH     (1ull << 12)

// CAP_POST_SERVICE -- authority to post a service into /srv, as a CAPABILITY
// rather than the spawn-time PROC_FLAG_MAY_POST_SERVICE mark (IMPERIUM-DESIGN
// 6.5). Elevation-only and a member of CAP_GRANTABLE_IMPERIUM: an operator
// confers it on one command through the lex curiata (haul posts its mount
// this way), and it flows to rfork children only inside that propagating
// scope. Never held at creation, never fork-grantable.
#define CAP_POST_SERVICE    (1ull << 13)

// CAP_TCB_DIAL -- the authority to CONNECT to a TCB byte service in /srv
// (STALK-DESIGN.md section 5.2 / D8; ARCH section 28 I-1). A byte-mode connect
// hands the client the RAW transport, so from that point the kernel is a pipe
// and cannot bound what the client asks the server for -- admission has to be
// decided at the connect. A byte-mode service posted under the
// PROC_FLAG_MAY_POST_SERVICE TCB mark (SrvService.cap_posted == false) is
// therefore connectable only by a holder of this bit. A user's own scoped post
// (cap_posted == true, via CAP_POST_SERVICE -- haul --post) and every 9P-mode
// service are unaffected, as is a Proc dialling a service it posted itself.
//
// FORK-GRANTABLE (a member of CAP_ALL), deliberately NOT elevation-only: it
// flows down the vetted boot chain exactly as CAP_SET_IDENTITY does --
// kproc -> joey -> /sbin/login -> the per-user home proxy login spawns. joey
// and login are HOLDERS in their own right, not just carriers: joey dials
// /srv/stratum-fs for the boot readiness handshake and login dials byte-mode
// /srv/stratum-ctl for the DEK lifecycle. login omits the bit from the shell's
// spawn mask, and caps never grow post-creation (I-2), so no user Proc can
// acquire it.
//
// The proxy runs as the USER (so the coordinator attributes the user's home
// files to them), which is why THIS gate is a capability and not an identity
// check -- the proxy and the user's shell are the same principal.
//
// But do NOT read that as "only a capability can separate them" in general: it
// is false, and the audit caught the claim. The I-39 debug surface separates on
// IDENTITY (devproc_debug_authorized's owner axis), so the same principal can
// attach to the proxy and drive its transport without ever holding this bit.
// That route predates this gate and is closed, in the same chunk but not by
// anything here, by SPAWN_PERM_SEAL: login spawns the proxy with the bit and
// the kernel stamps PROC_FLAG_NOTRACE before its first EL0 instruction. The
// general tension -- same principal, different authority -- survives, and the
// PLANNED /proc/<pid>/fd/ surface (deferred at devproc.c:27) would reopen it on
// the owner axis, where NOTRACE does not reach.
//
// Being fork-grantable, this bit is NOT auto-stripped at fork: every spawn mask
// that must not confer the dial has to omit it deliberately. That is a standing
// obligation on anyone defining a new user-facing spawn mask, with no static
// guard behind it.
// Requiring a corvus clearance instead would be the wrong shape -- the proxy is
// SPAWNED, not elevated.
#define CAP_TCB_DIAL        (1ull << 14)

// Reserved for Phase 5+ (one bit per capability domain; next free bit is
// 1<<15):
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
// ~CAP_ELEVATION_ONLY (A-4-pre). Every member is acquired ONLY through the
// `cap` device: CAP_HOSTOWNER (the unified fs-admin authority); the A-4
// finer caps split out of it — CAP_DAC_OVERRIDE, CAP_CHOWN, CAP_KILL;
// CAP_DEBUG (the Stage-8a cross-Proc debug authority); CAP_JIT (the CL-7k
// code-emission authority; I-42 requires it be non-heritable, so its
// membership here is an invariant obligation, not a style choice);
// CAP_AUDIO_GRAPH (whole-sink audio authority); CAP_POST_SERVICE. The macro
// below is the authority for the set; a count written in prose has been
// wrong four times.
// Maps to specs/handles.tla::ElevationOnly.
#define CAP_ELEVATION_ONLY  (CAP_AUDIO_GRAPH | CAP_POST_SERVICE | CAP_HOSTOWNER | CAP_DAC_OVERRIDE | CAP_CHOWN | CAP_KILL | CAP_DEBUG | CAP_JIT)

// CAP_ALL — the FORK-GRANTABLE capability ceiling: every capability a
// Proc may legitimately hold from creation, and the mask kproc gets at
// proc_init. Elevation-only capabilities (every bit of CAP_ELEVATION_ONLY,
// above) are deliberately excluded. A new fork-grantable CAP_* bit MUST be added
// here; an elevation-only one MUST NOT.
#define CAP_ALL         (CAP_HW_CREATE | CAP_LOCK_PAGES | CAP_CSPRNG_READ | CAP_GRANT_HOSTOWNER | CAP_SET_IDENTITY | CAP_GRANT_CLEARANCE | CAP_TCB_DIAL)

// CAP_DEFINED — every capability bit this header defines. The COVERAGE assert
// below cross-checks it against the two class sets, so a bit must be listed
// here AND in exactly one class or the build fails.
//
// This replaces an assert that read as the coverage check and was not one: it
// compared `CAP_ALL` against `CAP_ALL`'s own definition, token for token, so it
// was true unconditionally and could not fail (measured in
// vault abi-caps: a standalone reproduction defining a new fork-grantable bit
// and deliberately omitting it from CAP_ALL compiled clean). That is task #35,
// closed here because (U) adds exactly the kind of bit it failed to protect —
// a new FORK-GRANTABLE one. The old guard's own comment described the drift it
// did not catch.
//
// Why this one can fail where that one could not: the two sides are INDEPENDENT
// lists. Omit the new bit from CAP_ALL and the union loses it while CAP_DEFINED
// keeps it; omit it from CAP_DEFINED and the union gains a bit the mask lacks.
// Either way the comparison is between two different expressions, not one
// expression with itself.
#define CAP_DEFINED     (CAP_HW_CREATE | CAP_LOCK_PAGES | CAP_CSPRNG_READ | \
                         CAP_HOSTOWNER | CAP_GRANT_HOSTOWNER | CAP_SET_IDENTITY | \
                         CAP_GRANT_CLEARANCE | CAP_DAC_OVERRIDE | CAP_CHOWN | \
                         CAP_KILL | CAP_DEBUG | CAP_JIT | CAP_AUDIO_GRAPH | \
                         CAP_POST_SERVICE | CAP_TCB_DIAL)

_Static_assert((CAP_ALL | CAP_ELEVATION_ONLY) == CAP_DEFINED,
               "caps.h drift: a defined CAP_* bit is in NEITHER CAP_ALL nor "
               "CAP_ELEVATION_ONLY (it would be defined, documented and DEAD -- "
               "kproc never holds it, so rfork's mask-AND clears it at every hop "
               "and the gate it guards refuses everyone), or a bit is in a class "
               "set without being listed in CAP_DEFINED. Every capability is "
               "fork-grantable XOR elevation-only; add a new bit to CAP_DEFINED "
               "and to exactly one of the two class sets.");

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
