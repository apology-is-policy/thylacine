---
id: view-closed-sub-netd-server
type: view
title: "Do-not-re-report preamble — sub-netd-server"
query: closed:sub-netd-server
---
# Do-not-re-report preamble — sub-netd-server

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-netd-server`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.

<!-- generated:begin -->
17 closed findings on [[sub-netd-server]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-294-r1-f3]] [P3] Claimed: netd must cancel a parked readiness Tread on the ready-fd clunk (already handled) (withdrawn) — WITHDRAWN — verified already handled at the round, and re-verified at
- [[fnd-net2d-r1-f1]] [P2] h_readdir's budget omitted the Rreaddir frame overhead — a small-msize client could receive an over-msize reply (fixed) — Fixed in the close: the `rreaddir_budget(count, msize)` helper =
- [[fnd-net2d-r1-f2]] [P3] h_attach accepted fid == P9_NOFID — the no-fid sentinel bindable as a live fid (fixed) — Fixed in the close: `h_attach` rejects `fid == P9_NOFID` (E_INVAL) and
- [[fnd-net2d-r1-f3]] [P3] A rejected connect burned an ephemeral port; a rolled-back clone over-counted `opened` (fixed) — Fixed in the close: peek-then-commit on `next_local_port` (the
- [[fnd-net2d-r1-f4]] [P3] Cross-call Treaddir coherency: a slot freed between paginated reads renumbers entries (documented) — Closed justified: by-design, matching the kernel readdir-cookie
- [[fnd-net2d-r1-f5]] [P3] The ephemeral-port rotation is not liveness-checked (documented) — Closed justified: documented in-code as the v1.x liveness-checked
- [[fnd-net2d-r1-sf4]] [P3] Cross-session connection liveness: any session that can name /net/<proto>/N holds it live (documented) — Closed justified: this IS the Plan 9 shared-namespace model, bounded by
- [[fnd-net3d-r1-f1]] [P1] A clunked half-open listen fid stranded its PendingAccept — cross-proto slot re-mint type-confused the typed get (smoltcp panic → network DoS) (fixed) — Fixed by FOUR complementary layers: the per-slot monotonic mint `gen`
- [[fnd-net3d-r1-f2]] [P2] poll_accepts gated its typed get on liveness only, not proto (fixed) — Fixed — subsumed by the F1 poll_accepts proto+gen guard: the typed
- [[fnd-net3d-r1-f3]] [P3] The ICMP Echo-ident rotation is not liveness-checked (dup idents mis-route a reply) (documented) — Closed justified: the v1.x liveness-checked allocator, documented
- [[fnd-net3d-r1-f4]] [P3] A slot-table-full deferred accept buffers the inbound call indefinitely (documented) — Closed justified: documented as the #65 resource-floor behavior (the
- [[fnd-net3d-r2-f1]] [P3] register_accept's out-of-range else-branch is unreachable — kept as a fail-safe, now documented (documented) — Kept DELIBERATELY as a fail-safe (gen 0 is never a live slot's gen, so
- [[fnd-net3d-r2-f2]] [P3] The gen-guard comment overstated necessity — it is the belt against a FUTURE refcount-pin regression (documented) — The comment now states the truth: the proto arm makes the typed get
- [[fnd-net4d-r1-f1]] [P2] A held deferred cs/dns read tag could be LOST — the single deferred slot clobbered by a concurrent read or re-write (fixed) — Fixed with two MINIMAL guards (deliberately not a wait/wake
- [[fnd-net4d-r1-f2]] [P3] Content[128] left a ~6-byte margin under the widest status render (and the commit message claimed 256) (fixed) — Fixed: the buffer bumped to the documented 256 (inert — realistic
- [[fnd-net4d-r1-f3]] [P3] The shared dns socket's queries Vec is a bounded reused high-water, not a leak (documented) — Closed justified: documented as a [[sub-netd-server]] caveat beside
- [[fnd-weft7-r1-f4]] [P3] The netd raw-pointer ring sites' single-threadedness preconditions were undocumented (fixed) — Fixed (doc hardening): explicit "INVARIANT (Weft-7 F4)" notes at the
<!-- generated:end -->
