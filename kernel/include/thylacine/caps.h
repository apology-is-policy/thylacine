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

// Reserved for Phase 5+ (one bit per capability domain):
//   CAP_NS_MOUNT     — bind/mount in /proc and /ctl (kernel admin Devs).
//   CAP_NS_BIND      — bind in any namespace (forward-looking).
//   CAP_SIGNAL_ANY   — signal any Proc (vs. signal-children-only default).
//   CAP_NET_RAW      — open raw network sockets / Ethernet frames.
//   CAP_TIME_SET     — modify system clock.
//   CAP_REBOOT       — initiate kernel reboot / extinction.
// Each lands when the corresponding subsystem matures.

// CAP_ALL — the FORK-GRANTABLE capability ceiling: every capability a
// Proc may legitimately hold from creation, and the mask kproc gets at
// proc_init. Elevation-only capabilities (CAP_HOSTOWNER) are
// deliberately excluded — see CAP_HOSTOWNER above. A new fork-grantable
// CAP_* bit MUST be added here; an elevation-only one MUST NOT.
#define CAP_ALL         (CAP_HW_CREATE | CAP_LOCK_PAGES | CAP_CSPRNG_READ)

// _Static_assert pins CAP_ALL — adding a new fork-grantable CAP_* bit
// requires bumping this expression so kproc's initial mask includes it.
_Static_assert(CAP_ALL == (CAP_HW_CREATE | CAP_LOCK_PAGES | CAP_CSPRNG_READ),
               "caps.h drift: when adding a new FORK-GRANTABLE CAP_* bit, "
               "update CAP_ALL so kproc's initial mask reflects it. "
               "Elevation-only caps (CAP_HOSTOWNER) are deliberately "
               "excluded from CAP_ALL.");

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
