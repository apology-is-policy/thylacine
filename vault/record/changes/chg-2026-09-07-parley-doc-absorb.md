---
id: chg-2026-09-07-parley-doc-absorb
type: chg
title: "absorb docs/reference/141-parley (the LSP/DAP client substrate): fold the four UNOWNED probes + the live-server round-trips into sub-parley"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: [sub-parley]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
parley (the LSP/DAP client substrate) + the nora wiring + the diagnostics UI.
quaestor owner: usr/lib/parley/src/* -> sub-parley (audit:light, was fresh
2026-08-03); usr/nora/src/{lsp_host,dap_host}.rs -> sub-nora-host (audit:light,
fresh 2026-09-06); usr/nora/src/{diag,view}.rs -> sub-nora-view (audit:none,
fresh 2026-09-06); usr/{parley-probe,parley-echo,lsp-probe,dap-probe}/src/main.rs
-> UNOWNED. Verified atom-by-atom.

ALREADY COVERED (verified, sub-parley deep): the 7-module stack (json/frame/
jsonrpc/lsp/dap/dapc/transport); policy-is-pure (every traffic method returns the
Value); LSP latest-wins vs DAP unique-seq; negotiated UTF-8/UTF-16 position
encoding + clamp-up-to-boundary; full-not-incremental doc sync; json::Value Int/
Float split for exact id round-trip; the decoder caps (MAX_HEADER 8KiB/MAX_BODY
64MiB/MAX_DEPTH 128); transport::Mux PollSet-rebuilt-per-call; the parser-hazard
prosecution; the #120 body-cap-16x-heap / #121 manifest-snapshot / #122
parse_position-wrap caveats + the DAP pending-map never-answered-leak asymmetry.
The nora poll-loop + Option lifecycle + typing-boundary sync + :-debug verbs ->
sub-nora-host. The protocol-free diag model + past-end-drop + gutter/status/tally
render -> sub-nora-view.

THE FOLD (genuine gaps -> sub-parley, depth rich; updated 2026-08-03 -> 09-07):
- The FOUR PROBE BINARIES were UNOWNED orphans the dossier's proof story relies
  on (Seams: "proven end to end in-guest by parley-probe... the claim that the
  coverage lives elsewhere is one that checks out"). Added
  usr/{parley-probe,parley-echo,lsp-probe,dap-probe}/src/main.rs to code:.
- sub-parley (2026-08-03) predated the LIVE-SERVER round-trips. It carried
  parley-probe (the TRANSPORT proof) but NOT lsp-probe/dap-probe (the CLIENT-
  POLICY-against-a-real-server proofs that close the synthetic-tests-validate-
  their-own-assumptions gap). Code-verified anti-hollow: lsp-probe drives real
  /goroot/bin/gopls over LSP stdio + asserts the planted diagnostic at the
  planted LINE (the line proves the range decoded); dap-probe spawns real
  /ambush dap-stdio + drives the VS-Code launch sequence against /ambush-child +
  asserts the exact memory sentinel 0x0AABB00DCAFE0001. Folded as a new
  "In-guest proofs" section incl. the dap-stdio-rides-the-same-transport-as-LSP
  detail + the vacuous-green lesson (the first LSP wiring printed FAIL + reaped
  status=1 but had NO exit-0 gate, so test.sh passed until the revert-probe
  caught it -- a probe that cannot fail the build is a vacuous green).

Multi-redirect stub: parley crate -> sub-parley; nora wiring -> sub-nora-host;
diagnostics UI -> sub-nora-view. Render + lint verified NO double-claim. Zero
code change.
