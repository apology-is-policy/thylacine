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

// v1.0 grantable mask. Only CAP_HOSTOWNER may be granted via this
// device. /grant and /use both reject any other cap bits.
#define CAP_GRANTABLE  (CAP_HOSTOWNER)

// /grant write payload — fixed-size 16-byte message:
//   bytes [0..8)   cap_mask        u64 LE
//   bytes [8..16)  target_stripes  u64 LE
#define CAP_GRANT_WRITE_LEN  16u

// /use write payload — fixed-size 8-byte message:
//   bytes [0..8)   cap_mask  u64 LE   (the cap the writer is redeeming)
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

// cap_redeem_grant_for_writer — the /use write core. Validates the
// writer holds PROC_FLAG_CONSOLE_ATTACHED and a non-expired pending
// grant exists for the writer's stripes with a matching cap_mask, then
// ORs cap_mask into writer->caps and consumes the grant.
//
// Returns CAP_USE_WRITE_LEN on success, -1 on: writer not console-
// attached, writer stripes == 0, no matching pending grant (none, or
// expired, or different cap_mask).
//
// Tests call this directly; production path is devcap_write on a /use
// Spoor.
long cap_redeem_grant_for_writer(struct Proc *writer, caps_t cap_mask);

#endif  // THYLACINE_DEVCAP_H
