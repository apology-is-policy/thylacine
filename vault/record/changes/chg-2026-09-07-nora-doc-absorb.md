---
id: chg-2026-09-07-nora-doc-absorb
type: chg
title: "absorb docs/reference/113-nora (the native modal editor): fold the I-39-authorized kstack read into sub-nora-host"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: [sub-nora-host]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
nora (the native modal editor) -- the master reference spanning the engine, the
renderer, the process half, and the parley protocol substrate. quaestor owner:
text/editor/lib/wrap -> sub-nora-engine (audit:light, deep, dated 2026-08-03 but
already describing the 8f dashboard routing); view/theme/syntax/debug/vartree ->
sub-nora-view (audit:none, fresh 2026-09-06); main/lsp_host/dap_host ->
sub-nora-host (audit:light, fresh 2026-09-06); usr/lib/parley -> sub-parley.
ALL nora paths OWNED. Verified atom-by-atom.

ALREADY COVERED (verified): char-addressed text + byte<->char bridge; editor-
requests-not-does (the 3 async axes); modal FSM + Normal's four-things (hot-keys/
tile-nav/multi-cursor/count); pending-prefix machine; multi-cursor apply-then-
shift; soft-wrap shared coords; multi-buffer park/restore; undo-one-action; rev
O(1) sync; readonly-at-doors; 238 host tests -> sub-nora-engine. The render +
runtime Palette (Bonfire default, s7a session adoption) + native syntax lexer +
DebugView + nested-lazy vartree + the visual-only kernel divider + StackRow.kernel
-> sub-nora-view. Console I-27 abstention + palette adoption + CPR sizing + one-
poll + child-lifecycle + language table + gofmt-pipe-not-rewrite + twice-both-
directions conversion -> sub-nora-host. The LSP/DAP clients -> sub-parley.

THE FOLD (genuine gap -> sub-nora-host, depth rich; updated 2026-09-06 -> 09-07):
the KERNEL-STACK READ's I-39 authorization. sub-nora-host's data-structures noted
the pid is "used to read the kernel half" but carried NO mechanism for
dap_host::refresh_kernel_frames, and its Invariants said "no kernel object" --
now false. Code-grounded (dap_host.rs:809): the host reads /proc/<pid>/kstack on
each stop (the 8b settled-thread inspect), I-39-AUTHORIZED via the OWNER AXIS
(nora/Ambush/debuggee share the login principal; /proc reachable because Ambush --
spawned by nora -- opens the same file), best-effort/fail-open (no pid /
unreachable-or-denied /proc / unparseable -> empty kernel half, Go frames alone,
never a hang; the pid arrives via parley's DAP `process` event decode). The frame
read is the target's HEAD thread (not the stopped goroutine's M -- the deferred
Ambush 8c-3 stitch). Folded as a new "The kernel half of the stack is an
I-39-authorized /proc read" mechanism subsection + an "I-39 is CONSUMED not
enforced" Invariants entry ([[inv-i39]], which resolves). The parse (parse_kstack,
debug.rs) + the divider render were already sub-nora-view's.

NOTED not folded: usr/nora-demo/nora-demo.go is UNOWNED -- a baked DWARF-retained
demo asset for the dashboard, not a mechanism surface; no dossier owed.

Four-way redirect stub: engine -> sub-nora-engine; renderer -> sub-nora-view;
process half -> sub-nora-host; protocol -> sub-parley. Render + lint verified NO
double-claim. Zero code change.
