---
id: chg-2026-10-01-terminal-admission
type: chg
title: "Sealed terminal bindings and ordered compositor admission"
date: 2026-10-01
arc: arc-astra-halcyon-followup
commits: []
touched: [sub-tapestryd, sub-kaua-term, sub-halcyond, sub-libhalcyon, sub-libtapestry]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
Halcyon starts its sealed Kaua host with explicit interaction binding. The
host binds its actual master to Tapestry's exact poster and announces only a
locator on the dedicated UP pipe. Subtag 9 preserves the integrated 7/8
synchronized-output meanings. Halcyon supplies its real child PID and leaf;
Tapestry validates the kernel binder and exact owned surface incarnation.

HIA1 carries Bind, Publish, Check and Unbind on the renderer's declared ctl
connection. Observer watches revoke stale scopes before requests; fresh kernel
STATE/CHECK covers undelivered notifications. Publish clears old context before
ACK. Check compares the complete stored scope, actual keyboard focus and normal
seat. Every request samples seat generation, including changes since the main
loop's sample. Connection and surface IDs refuse exhaustion, layout epochs
saturate, and saturated admission fails closed. The bounded per-fid receipt is
immutable and the client reads it from offset zero after its request write.

This is the terminal admission checkpoint, not live application clipboard
support. Asynchronous broker operations and pending-reply cancellation, direct
graphical-client ownership, complete aggregate budgets, app clients and the mode
widget remain. The synchronous helper serves setup and native qualification;
it is not suitable for clipboard operations on the renderer's UI hot path.

Evidence is retained in `work/oct1-hi-admission/`: 57 Kaua, 149 libhalcyon and
112 pure Tapestry tests; 14 actual-source admission/codec tests plus an extracted
production identity-exhaustion test; six intended named mutation failures.
The native probe exercises real kernel ownership, focus away/back, context
replacement, failed nomination and retirement. The independent existing native
observer scenario also passed (40 seconds each). Earlier harness failures remain
recorded: host ENOSPC, then self-delivered carrier loss, then premature polling
before the asynchronous clunk generated that carrier notification.

The ordinary CPU1 boot passed 1830/1830 before the probe-only cleanup fixes and
final surface-ID exhaustion guard. Final guarded graphical image passes session media (76.29s) and physical F10
SAK (94.48s). Both real terminal tiles register successfully, with no refusal.
View, PNG/JPEG Gallery, manual navigation/theme, confer/abdicate/wrong-key/cancel
all pass. Workspace and trusted-prompt captures were inspected at 1280x800;
no new Pi, 800x720 or graphical failure-recovery qualification is claimed. No 50-boot, SMP, ASan or UBSan result is claimed under the
operator's October 1-2 waiver. Review is single-agent, not independent.
