// Internal CONNECT engine. Not wired to private syscalls or the ring executor.
#ifndef THYLACINE_LOOM_SERVICE_CONNECT_H
#define THYLACINE_LOOM_SERVICE_CONNECT_H
#include <thylacine/loom_service_protocol.h>

// Caller retains l. The destination is an already published RESERVED SCOPE.
// Reserves request storage + exact scope pins before native allocation. One
// CONNECT owns a destination; duplicate preparations fail. Output unchanged on
// failure. Returned owner is exclusive until publish; rollback consumes it.
int loom_private_connect_prepare(struct Loom *, struct loom_service_ref scope,
    struct loom_service_ref target, u64 user_data, u64 deadline, u64 now,
    struct loom_service_protocol **out);
int loom_private_connect_rollback(struct loom_service_protocol *);
// Locked atomic native backlog publication + request admission. Caller keeps
// the owner exclusively through publish (including its unlocked accept wake).
// A failure leaves PREPARED ownership for rollback; no bytes have been sent.
int loom_private_connect_publish(struct loom_service_protocol *, u64 now);
// An indexed installer may also accept abort before publication; it must then
// drive rejection to a terminal CQE and retirement, not roll back that obligation.
// Caller keeps storage alive across ALL concurrent entries
// and excludes new entry before final release. No raw pointer is a reference.
// Abort may race step. At most one step driver; returns BUSY for a second one.
// This checkpoint permits ONLY the handshake: no external client/RPC/fid/buffer
// borrows. The future ring executor must provide lookup, entry pins and queues.
// Step returns 0 pending, 1 READY, or negative terminal reason. It does not post
// CQEs; use loom_private_post_terminal(owner, request), retaining on CQ full.
int loom_private_connect_step(struct loom_service_protocol *, u64 now);
// First reason wins. 0 accepted, 1 already aborting/retired. Tears transport
// down outside Ll; acknowledgement is NOT local retirement. reason<0 required.
int loom_private_connect_abort(struct loom_service_protocol *, s32 reason);
// Requires actual local retirement, zero users, and terminal delivered/discarded.
// This owner alone reaps its CONNECT request; generic CQ cleanup must not reap
// it first, even after delivery. Reaps its request and drops its scope pin; consumes p. The RETIRED scope stays
// for explicit metadata reap. A server-held endpoint retains native credits.
int loom_private_connect_release(struct loom_service_protocol *);
int loom_private_connect_reap(struct Loom *, struct loom_service_ref scope);
#endif
