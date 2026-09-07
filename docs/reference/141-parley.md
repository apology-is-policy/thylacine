# 141 — parley: the LSP/DAP client substrate [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-parley-doc-absorb`).
`usr/lib/parley` — the dialogue layer between the editor (nora) and the two
servers it drives, `gopls` over LSP and Ambush over DAP. Seven pure protocol
modules (`json` / `frame` / `jsonrpc` / `lsp` / `dap` / `dapc`) with one
platform-touching layer (`transport`); a *parley* is a formal exchange between
parties under an agreed protocol. Its content lives, code-verified and current,
across three dossiers:

- **the crate** (`usr/lib/parley`, plus the four in-guest probe binaries now
  folded here) — the pure-policy discipline (every traffic-producing method
  *returns* the message, so both whole clients are host-testable with no
  process), LSP latest-wins supersession vs. DAP unique-seq, the negotiated
  UTF-8/UTF-16 position encoding, full-not-incremental document sync, the
  `json::Value` `Int`/`Float` split for exact id round-trip, the `frame` decoder
  caps (`MAX_HEADER_BYTES` 8 KiB / `MAX_BODY_BYTES` 64 MiB / `MAX_DEPTH` 128),
  the `transport::Mux` PollSet-rebuilt-per-call, the parser-hazard prosecution,
  and the **in-guest proofs** (`parley-probe`/`parley-echo` for the transport,
  `lsp-probe` for the live gopls round-trip, `dap-probe` for the live
  `ambush dap-stdio` round-trip) that close the synthetic-test gap:

      vault/system/userspace/shell-tui/sub-parley.md   (audit: light)

- **the nora wiring** (`usr/nora/src/lsp_host.rs` + `dap_host.rs`) — the single
  `poll(2)` over `{ fd 0, gopls.stdout/stderr, ambush.stdout/stderr }` (so an
  arriving diagnostic or `stopped` event wakes the loop exactly like a
  keystroke), the `Option` lifecycle (a missing/dead server is a supported
  state), document sync at typing boundaries via the O(1) `rev()` check, the
  outbound cursor-request drain, the `:`-driven headless debug verbs, and the
  launch-as-a-handshake sequencing:

      vault/system/userspace/shell-tui/sub-nora-host.md   (audit: light)

- **the diagnostics in the editor** (`usr/nora/src/diag.rs` + `view.rs`) — the
  protocol-free `nora::diag` model (line/byte-columns/severity, no LSP), the
  past-end diagnostic *dropped* not clamped, and the render behaviour (gutter
  recolor confined to the marked line, the status-line message, the `NE MW`
  off-screen tally, clear-on-buffer-switch):

      vault/system/userspace/shell-tui/sub-nora-view.md   (audit: none)

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The four probe binaries were UNOWNED orphans — now folded into sub-parley.**
  `usr/parley-probe`, `usr/parley-echo`, `usr/lsp-probe`, `usr/dap-probe` are
  parley's in-guest proof harness (the dossier's whole "the coverage lives
  elsewhere" argument rests on them), yet none was claimed by any dossier. Added
  to sub-parley's code list (`chg-2026-09-07-parley-doc-absorb`).
- **sub-parley predated the live-server round-trips.** The dossier (dated
  2026-08-03) covered the crate and `parley-probe` (the *transport* proof) but
  not `lsp-probe`/`dap-probe` — the proofs that the *client policy* survives
  contact with a real gopls/Ambush, and the vacuous-green lesson (a probe with no
  exit-0 gate let `test.sh` pass on a failing diagnostic until the revert-probe
  caught it). Folded as a new "In-guest proofs" section; `updated:` bumped to
  2026-09-07.
- **Everything else was covered** — the seven-module stack, the pure-policy
  property, the encoding negotiation, the decoder caps, the #120/#121/#122
  caveats, and the pending-map bound asymmetry are all as-built in sub-parley;
  the nora poll-loop and diagnostics rendering are in sub-nora-host/sub-nora-view.
  The `textDocument/references`-unimplemented, cross-file-column-approximate,
  one-server-one-language, no-cancellation, and no-restart-on-crash seams are
  nora-wiring seams the nora dossiers hold. Zero code change.
