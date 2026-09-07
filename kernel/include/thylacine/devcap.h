// devcap — the hostowner-elevation `cap` device (P5-hostowner-b-a).
//
// Per CORVUS-DESIGN.md §5.5 + §5.5.1. The kernel-side counterpart of
// corvus's factotum-pattern elevation: a two-phase, file-mediated grant
// of CAP_HOSTOWNER (or any future elevation-only capability).
//
//   PHASE 1 — REGISTER
//     corvus writes /cap/grant { cap_mask, target_stripes }
//       gate: writer holds CAP_GRANT_HOSTOWNER (a fork-grantable cap
//             joey confers on corvus via the spawn mask)
//       effect: a pending grant is recorded in the table, keyed by
//               target_stripes, with a short expiry
//
//   PHASE 2 — REDEEM
//     the target Proc writes /cap/use { cap_mask }
//       gate: writer holds PROC_FLAG_CONSOLE_ATTACHED, AND a non-
//             expired pending grant exists for the WRITER's own stripes
//             with a matching cap_mask
//       effect: current->caps |= cap_mask; pending grant consumed
//               (one-shot)
//
// Two independent gates in two trust domains: corvus verifies the
// system passphrase (the kernel has no notion of it); the kernel
// verifies console attachment at redemption time (holds even if corvus
// is buggy or compromised). A compromised corvus can register grants
// for arbitrary stripes, but the kernel only lets a *console-attached*
// writer redeem — a corvus compromise is structurally bounded to the
// local physical console.
//
// At v1.0, only CAP_HOSTOWNER is grantable. The device is general so
// future elevation-only capabilities reuse the same machinery.
//
// Spec: specs/corvus.tla — HostownerGrant + HostownerRequiresConsole
// (handles.tla pins the ElevationOnly / RforkStripsElevation axis).

#ifndef THYLACINE_DEVCAP_H
#define THYLACINE_DEVCAP_H

#include <thylacine/caps.h>
#include <thylacine/types.h>

struct Dev;
struct Proc;
struct Spoor;

// Pending-grant table capacity. A pending grant is short-lived (one
// elevation in flight per console session, expires in tens of seconds);
// 16 is generous headroom for a multi-console / multi-elevation race
// without growing the kernel BSS meaningfully.
#define CAP_GRANT_MAX  16u

// Pending-grant expiry, in nanoseconds. The window between corvus's
// /cap/grant write and joey's /cap/use redemption is bounded — joey
// reads corvus's OK response, then immediately writes /use. 30 seconds
// comfortably covers any plausible scheduling jitter while keeping a
// stale grant from persisting indefinitely.
#define CAP_GRANT_EXPIRY_NS  (30ull * 1000ull * 1000ull * 1000ull)

// Hostowner grantable mask (the legacy console-gated path). Only
// CAP_HOSTOWNER flows through the 16-byte /grant message.
#define CAP_GRANTABLE  (CAP_HOSTOWNER)

// A-4a clearance grantable mask (the legate path). The elevation-only
// fs-admin caps split out of CAP_HOSTOWNER -- a clearance grant confers a
// SUBSET of these. Excludes CAP_HOSTOWNER itself (that stays on the
// console-gated path) and every fork-grantable cap. A clearance grant whose
// cap_mask escapes this mask is rejected at register. CAP_DEBUG (Stage-8a) is
// clearance-grantable so a dev-session debugger acquires it via the same
// scope- and time-bounded legate the fs-admin caps use (docs/DEBUG-FS-DESIGN.md
// section 7.1) -- the register-gate + redeem-subset check are mask-driven, so
// no devcap.c change is needed. CAP_JIT (CL-7k / I-42) joins on the same
// footing: a Proc that must emit code (an llvmpipe-backed GL app) acquires the
// authority through a bounded legate rather than by inheritance, which is what
// keeps I-42's "non-heritable" clause true of every path, not just rfork.
// CAP_AUDIO_GRAPH (Nocturne N-3a / I-46; docs/NOCTURNE.md §6.8) joins on the
// SAME mask-driven footing -- a system-level audio program (a whole-sink EQ,
// the sink-loopback recorder, a non-console volume setter) acquires the
// whole-sink authority through a bounded legate, never by inheritance; no
// devcap.c change is needed.
#define CAP_GRANTABLE_CLEARANCE  (CAP_DAC_OVERRIDE | CAP_CHOWN | CAP_KILL | CAP_DEBUG | CAP_JIT | CAP_AUDIO_GRAPH)

// RW-5 SA-2 -- pin the I-25 member-unelevated invariant at compile time. BOTH
// grantable sets MUST be entirely elevation-only. The load-bearing consequence:
// a clearance grant can confer ONLY rfork-stripped caps, so a legate scope
// MEMBER (it inherits scope_id but rfork strips the elevated caps) is always
// UNELEVATED -- which is exactly why a teardown-missed straggler is benign (a4a)
// and why I-25's "no elevated Proc outlives the scope" rests on the ROOT alone.
// A future FORK-GRANTABLE cap added to either set would let a legate confer it,
// a child inherit it (not stripped), and an elevated straggler survive the
// sweep -- silently breaking I-25. The runtime register-gates (cap_mask & ~MASK)
// bound the VALUES; these asserts bound the MASKS themselves.
_Static_assert((CAP_GRANTABLE & ~(caps_t)CAP_ELEVATION_ONLY) == 0,
               "CAP_GRANTABLE must be a subset of CAP_ELEVATION_ONLY -- a "
               "hostowner grant may confer only elevation-only (rfork-stripped) caps.");
_Static_assert((CAP_GRANTABLE_CLEARANCE & ~(caps_t)CAP_ELEVATION_ONLY) == 0,
               "CAP_GRANTABLE_CLEARANCE must be a subset of CAP_ELEVATION_ONLY so "
               "the scope teardown is the ONLY way a redeemed cap leaves a Proc "
               "(I-25): a fork-grantable cap here would cross rfork by the ordinary "
               "mask path, outside the IM-2 propagation carve, and an elevated "
               "descendant could outlive the scope.");

// IM-2 (IMPERIUM-DESIGN.md 11.4; I-25 STRENGTHENED): the PROPAGATING-grantable
// subset -- the caps a CAP_GRANT_FLAG_PROPAGATING grant may carry, i.e. the
// caps that may FLOW to a legate root's rfork descendants. Exactly the
// imperium level (11.5): the fs-admin pair + the kill axis. Deliberately NOT
// the whole clearance set: CAP_DEBUG propagating would hand a debugger's own
// debuggee the debug authority (I-39's two-axis gate is per-grant), CAP_JIT is
// "non-heritable" by I-42's letter (a bounded legate, never inheritance), and
// CAP_AUDIO_GRAPH is the whole-sink authority I-46 grants per program. Those
// three stay plain (non-propagating) clearances, so their heritability clauses
// hold BY CONSTRUCTION here, not by corvus's policy alone. A PROPAGATING grant
// whose cap_mask escapes this mask is rejected at register.
#define CAP_GRANTABLE_IMPERIUM  (CAP_DAC_OVERRIDE | CAP_CHOWN | CAP_KILL)
_Static_assert((CAP_GRANTABLE_IMPERIUM & ~(caps_t)CAP_GRANTABLE_CLEARANCE) == 0,
               "CAP_GRANTABLE_IMPERIUM must be a subset of CAP_GRANTABLE_CLEARANCE: "
               "a propagating grant is a clearance grant with a flag, never a "
               "wider one.");

// /grant flags -- the 40-byte form's fifth word (SYS_CAP_GRANT_IMPERIUM x4).
//   CAP_GRANT_FLAG_PROPAGATING: the redeemed caps FLOW to the root's rfork
//   descendants (LEGATE_FLAG_PROPAGATING on the scope); redeemable only by an
//   UNSCOPED Proc (propagating never nests). Any other bit is rejected.
#define CAP_GRANT_FLAG_PROPAGATING  (1ull << 0)
#define CAP_GRANT_FLAGS_VALID       (CAP_GRANT_FLAG_PROPAGATING)

// /grant write payload -- hostowner form, fixed-size 16-byte message:
//   bytes [0..8)   cap_mask        u64 LE
//   bytes [8..16)  target_stripes  u64 LE
#define CAP_GRANT_WRITE_LEN  16u

// /grant write payload -- A-4a clearance form, fixed-size 32-byte message.
// The /grant file is length-discriminated: a 16-byte write is the hostowner
// grant above; a 32-byte write is a clearance grant (creates a legate at
// redeem). The two forms never collide (distinct lengths).
//   bytes [0..8)    cap_mask        u64 LE   (subset of CAP_GRANTABLE_CLEARANCE)
//   bytes [8..16)   target_stripes  u64 LE
//   bytes [16..24)  valid_for_ns    u64 LE   (legate lifetime duration; 0 = no
//                                             time bound -- scope ends only on
//                                             the legate root's exit)
//   bytes [24..32)  session_id      u64 LE   (corvus audit tag; must be nonzero
//                                             and fit in u32)
#define CAP_GRANT_CLEARANCE_WRITE_LEN  32u

// /grant write payload -- IM-2 imperium form, fixed-size 40-byte message: the
// clearance form + a flags word. Length-discriminated like the others (16 /
// 32 / 40 never collide). A 40-byte write with flags == 0 is exactly a
// clearance grant.
//   bytes [0..8)    cap_mask        u64 LE   (subset of CAP_GRANTABLE_CLEARANCE;
//                                             of CAP_GRANTABLE_IMPERIUM when
//                                             PROPAGATING is set)
//   bytes [8..16)   target_stripes  u64 LE
//   bytes [16..24)  valid_for_ns    u64 LE
//   bytes [24..32)  session_id      u64 LE
//   bytes [32..40)  flags           u64 LE   (CAP_GRANT_FLAGS_VALID bits only)
#define CAP_GRANT_IMPERIUM_WRITE_LEN  40u

// /use write payload -- fixed-size 8-byte message (both kinds):
//   bytes [0..8)   cap_mask  u64 LE   (the cap-set the writer is redeeming;
//                                      for a clearance grant this is the
//                                      self-restriction: a non-empty subset
//                                      of the granted set)
#define CAP_USE_WRITE_LEN  8u

// The devcap Dev. dc='k' (for "kapability"; both 'c' (cons) and 'C'
// (ctl) are taken, so we drop the c-mnemonic and use k instead).
// Registered by dev_init().
extern struct Dev devcap;

// =============================================================================
// Pending-grant table.
// =============================================================================
//
// Internal API exposed for tests + the proc-exit notify hook. The
// production paths go through the Dev's write op (devcap_write).

// cap_pending_count — number of non-FREE entries in the pending-grant
// table. Tests + diagnostics; takes the table lock.
int cap_pending_count(void);

// cap_proc_exit_notify — called from exits() for every exiting Proc.
// Drops any pending grant targeting `p` (matched by stripes). A Proc
// that never had a pending grant is a cheap no-op scan. Defense in
// depth: stripes are fresh per Proc (immutable, never recycled while
// the Proc lives), so a grant for an exited Proc cannot accidentally
// elevate a different Proc with a recycled pid; but cleanup frees the
// slot. Maps to CORVUS-DESIGN.md §5.5.1 "pending grants are dropped on
// the target Proc's death."
void cap_proc_exit_notify(struct Proc *p);

// cap_reset_table — drop all pending grants. Test-only; takes the
// table lock. Production paths never call this.
void cap_reset_table(void);

// cap_register_grant_for_writer — the /grant write core. Validates the
// writer holds CAP_GRANT_HOSTOWNER and the cap_mask is a subset of
// CAP_GRANTABLE, then records a pending grant {cap_mask, target_stripes,
// timer_now_ns() + CAP_GRANT_EXPIRY_NS}. A re-register for the same
// target_stripes replaces the previous grant.
//
// Returns CAP_GRANT_WRITE_LEN on success, -1 on: writer lacks
// CAP_GRANT_HOSTOWNER, cap_mask outside CAP_GRANTABLE, target_stripes
// is the reserved 0 sentinel, table full of non-expired entries.
//
// Tests call this directly; production path is devcap_write on a /grant
// Spoor.
long cap_register_grant_for_writer(struct Proc *writer,
                                   caps_t cap_mask, u64 target_stripes);

// cap_register_clearance_grant_for_writer — the A-4a clearance /grant core
// (32-byte form). The legate analog of cap_register_grant_for_writer:
// records a pending CLEARANCE grant {cap_mask, target_stripes, valid_for_ns,
// session_id, expiry} keyed by stripes. Gated on the writer holding
// CAP_GRANT_CLEARANCE (corvus). A clearance grant differs from a hostowner
// grant at redeem: it does NOT require console attachment, and it CREATES a
// legate (proc_become_legate). A re-register for the same target_stripes
// replaces the previous grant (any kind) in place.
//
// Returns CAP_GRANT_CLEARANCE_WRITE_LEN on success, -1 on: writer lacks
// CAP_GRANT_CLEARANCE; cap_mask is 0 or not a subset of CAP_GRANTABLE_CLEARANCE;
// target_stripes is the reserved 0 sentinel; session_id is 0 (collides with
// the not-a-legate sentinel) or exceeds u32; table full of non-expired entries.
// session_id is passed wide (u64) and validated to fit in u32.
//
// Tests call this directly; production path is the 32-byte devcap_write on a
// /grant Spoor.
long cap_register_clearance_grant_for_writer(struct Proc *writer,
                                             caps_t cap_mask, u64 target_stripes,
                                             u64 valid_for_ns, u64 session_id);

// cap_register_imperium_grant_for_writer — the IM-2 /grant core (40-byte form;
// SYS_CAP_GRANT_IMPERIUM). The clearance core with a `flags` word: everything
// cap_register_clearance_grant_for_writer checks, PLUS flags must be within
// CAP_GRANT_FLAGS_VALID, and a PROPAGATING grant's cap_mask must be within
// CAP_GRANTABLE_IMPERIUM. The clearance core IS this with flags == 0.
//
// Returns CAP_GRANT_IMPERIUM_WRITE_LEN on success, -1 on any clearance-core
// failure or: flags carries an unknown bit; PROPAGATING with a cap outside
// CAP_GRANTABLE_IMPERIUM.
long cap_register_imperium_grant_for_writer(struct Proc *writer,
                                            caps_t cap_mask, u64 target_stripes,
                                            u64 valid_for_ns, u64 session_id,
                                            u64 flags);

// cap_redeem_grant_for_writer — the /use write core. Does ONE locked lookup
// of the pending grant for the writer's stripes (so the grant's kind is read
// atomically -- no peek/redeem TOCTOU), then branches on the kind:
//
//   HOSTOWNER grant: requires PROC_FLAG_CONSOLE_ATTACHED AND the requested
//     cap-set EQUALS the granted cap_mask; ORs it into writer->caps. Unchanged
//     v1.0 hostowner semantics.
//   CLEARANCE grant (A-4a): NO console gate (auth was corvus-side before the
//     grant was registered); the requested cap-set must be a non-empty SUBSET
//     of the granted set (the self-restriction, I-2); stamps the writer via
//     proc_become_legate -- a FRESH legate root for an unscoped writer, a
//     FURTHER redeem (caps OR'd, tag kept) for one already in a scope (IM-2,
//     G6). A PROPAGATING grant (CAP_GRANT_FLAG_PROPAGATING) redeemed by a
//     writer already in ANY scope is REFUSED without consuming: propagating
//     never nests. The stamp runs under the table lock, which serializes two
//     redeems by peer threads of one Proc.
//
// Either kind consumes the grant on success (one-shot). Returns
// CAP_USE_WRITE_LEN on success, -1 on: writer stripes == 0, requested == 0, no
// matching pending grant (none / expired), or a per-kind gate failure (no
// console for hostowner, cap mismatch, requested escapes the granted set, the
// propagating-nest refusal). A kind/gate failure does NOT consume the grant
// (the legitimate holder may still redeem).
//
// Tests call this directly; production path is devcap_write on a /use Spoor.
long cap_redeem_grant_for_writer(struct Proc *writer, caps_t cap_mask);

#endif  // THYLACINE_DEVCAP_H
