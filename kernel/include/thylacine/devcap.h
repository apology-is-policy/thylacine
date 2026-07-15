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
// no devcap.c change is needed.
#define CAP_GRANTABLE_CLEARANCE  (CAP_DAC_OVERRIDE | CAP_CHOWN | CAP_KILL | CAP_DEBUG)

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
               "legate scope members stay UNELEVATED (I-25). Adding a fork-grantable "
               "cap here would let an elevated scope member outlive the scope.");

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

// cap_redeem_grant_for_writer — the /use write core. Does ONE locked lookup
// of the pending grant for the writer's stripes (so the grant's kind is read
// atomically -- no peek/redeem TOCTOU), then branches on the kind:
//
//   HOSTOWNER grant: requires PROC_FLAG_CONSOLE_ATTACHED AND the requested
//     cap-set EQUALS the granted cap_mask; ORs it into writer->caps. Unchanged
//     v1.0 hostowner semantics.
//   CLEARANCE grant (A-4a): NO console gate (auth was corvus-side before the
//     grant was registered); the requested cap-set must be a non-empty SUBSET
//     of the granted set (the self-restriction, I-2); makes the writer a legate
//     root via proc_become_legate (stamps the cleared caps + the scope context).
//
// Either kind consumes the grant on success (one-shot). Returns
// CAP_USE_WRITE_LEN on success, -1 on: writer stripes == 0, requested == 0, no
// matching pending grant (none / expired), or a per-kind gate failure (no
// console for hostowner, cap mismatch, requested escapes the granted set). A
// kind/gate failure does NOT consume the grant (the legitimate holder may still
// redeem).
//
// Tests call this directly; production path is devcap_write on a /use Spoor.
long cap_redeem_grant_for_writer(struct Proc *writer, caps_t cap_mask);

#endif  // THYLACINE_DEVCAP_H
