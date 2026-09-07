# 113 — nora: the native modal editor [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-nora-doc-absorb`).
`usr/nora` — the native (`no_std` + `alloc`, libthyla-rs) modal editor in the
Helix/vim lineage, the runtime editor for v1.0 and the first full-screen consumer
of the Kaua console-TUI substrate. Built as one library-plus-binary crate (the
pure engine host-tests; the binary is device-only behind the `backend` feature).
Its content lives, code-verified and current, across four dossiers:

- **the editor engine** (`text.rs` / `editor.rs` / `lib.rs` / `wrap.rs`) — the
  char-addressed `TextBuffer` (every position a character index, never a byte
  offset), the "editor requests, does not do" discipline (file I/O as a
  `Request`; LSP and DAP as separate `LspRequest`/`DapRequest` axes so a
  never-answering child cannot wedge a save), the modal FSM and its Normal-mode
  "four things resolved before the key" (debug hot-keys, focused-tile nav,
  multi-cursor collapse, count prefix), the pending-prefix two-key machine,
  multi-cursor apply-then-shift, soft-wrap as a shared coordinate system,
  multi-buffer park/restore, undo-is-one-action, `rev` as the O(1) sync detector,
  read-only-at-the-doors, and the 238 host tests:

      vault/system/userspace/shell-tui/sub-nora-engine.md   (audit: light)

- **the renderer + display models** (`view.rs` / `theme.rs` / `syntax.rs` /
  `debug.rs` / `vartree.rs`) — the layout/gutter/selection/status render, the
  runtime `Palette` (Bonfire the compiled default, adopted from the session via
  the role vocabulary), the native lexer syntax highlighter (UT + Go), the
  protocol-free `DebugView` snapshot, the nested-lazy `vartree` forest, and the
  cross-boundary `── kernel ──` divider (visual-only row; selection is a *frame*
  index that maps past it; `StackRow.kernel` drives the divider position + dim):

      vault/system/userspace/shell-tui/sub-nora-view.md   (audit: none)

- **the process half** (`main.rs` / `lsp_host.rs` / `dap_host.rs`) — the console
  discipline (I-27 abstention: nora takes the screen, never the line discipline),
  the session-palette adoption, the CPR sizing round-trip, the one poll over
  keyboard+notes+both children, the neither-child-outlives-the-editor lifecycle,
  the language table, format-on-save (the gofmt pipe with its deadlock-freedom +
  all-stdio-piped isolation), the twice-in-both-directions coordinate conversion,
  and the **I-39-authorized `/proc/<pid>/kstack` read** (now folded here):

      vault/system/userspace/shell-tui/sub-nora-host.md   (audit: light)

- **the LSP/DAP protocol substrate** (`usr/lib/parley`) — the clients nora drives
  (`gopls` over LSP, Ambush over DAP), the negotiated position encoding, the live
  round-trip probes:

      vault/system/userspace/shell-tui/sub-parley.md   (audit: light)

**What this file got WRONG or MISSED by the time it was absorbed:**

- **One code-grounded fold — the kernel-stack read's I-39 authorization.**
  sub-nora-host's data-structures noted the debuggee pid is "used to read the
  kernel half" but carried no *mechanism* for `dap_host::refresh_kernel_frames`,
  and its Invariants section claimed "no kernel object". The layer in fact reads
  `/proc/<pid>/kstack` on each stop, authorized by the **I-39 owner axis** (nora,
  Ambush and the debuggee share the login principal; `/proc` is reachable because
  Ambush — spawned by nora — opens the same file), best-effort/fail-open (no pid /
  unreachable /proc / unparseable → empty kernel half, never a hang). Folded as a
  new mechanism subsection + an I-39-is-*consumed* Invariants entry
  (`chg-2026-09-07-nora-doc-absorb`; `updated:` bumped to 2026-09-07). The parse
  and the divider render were already sub-nora-view's.
- **The `nora-demo` Go program is uncovered, and that is fine.** `usr/nora-demo`
  is a baked, DWARF-retained demo (`/goroot/bin/nora-demo`) for exercising the
  debugger dashboard — a demo asset, not a mechanism surface, so no dossier is
  owed. Noted here rather than filed.
- **Everything else was covered** — the whole 8f debugger-dashboard arc (the
  split/collapse/`Tab` focus, navigable tiles, tile actions, the nested-lazy
  Variables tree, the hot-keys, the `[Space]d` panel toggles), the gofmt
  format-on-save, the LSP editor affordances, the theme/palette work, and all the
  caveats (#124/#125/#130/#131/#133 and the find/find_all doc slips) are as-built
  across the four dossiers. Zero code change.
