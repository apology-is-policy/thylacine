# NORA-IDE-UX — the Nora Go IDE debugging UX (Stage 8e/8f)

Design-first scripture for the **flagship TUI debugging experience**: how the Go
IDE (LSP editing intelligence + DAP debugging + the Thylacine cross-boundary
superpowers) is presented and driven inside `nora`, on the Kaua substrate.

**Status:** design-only, no code. Kernel **byte-unchanged** — this arc consumes
the already-landed-and-audited surfaces (8a `/proc/<pid>` debug-fs + I-39, 8b
cross-boundary stack, 8c **Ambush** [the `dlv` port], 8d **gopls**). Ratified
decision: **layout = the IDE dashboard** (user-voted 2026-07-20). No new ARCH
§28 invariant (§10).

Covers **8e** (the Nora plugin architecture + the LSP/DAP clients) and **8f**
(the Kaua debug UI). References: `docs/GO-IDE-DESIGN.md` (the charter, §3.3/§8),
`docs/KAUA.md` (the substrate), `docs/UT-NORA-ERGONOMICS.md` (Nora's established
idiom), `docs/DEBUG-FS-DESIGN.md` (the kernel surface), `docs/GOPLS-PORT-DESIGN.md`
+ `docs/DELVE-PORT-DESIGN.md` (the engines).

---

## 0. The bar

Nora is the flagship. A debugger is where a TUI either sings or collapses into
pane-soup. The bar (Kaua's own, §KAUA.md 6): a first-time user opening a debug
session should think *this is lovely* before they think about features. The UX
must be **grounded in what Kaua/Nora already are** (we inherit an interaction
language; we invent none) and **honest about the substrate** (keyboard-only,
cell-grid, a two-layer damage model, and an async-client gap that is the real
make-or-break).

---

## 1. What is already settled (the debugger speaks Nora)

We do **not** invent an interaction language. The debugger inherits Nora's:

- **Modal** — Normal / Insert / Visual / Command (`:`), `/` search, adapted ~1:1
  from Stratum's `editor.rs` (`usr/nora/src/editor.rs:24` `Mode`), with the
  ratified Helix-style **`[Space]` command-palette / which-key menu**
  (`editor.rs:1697` `MENU`) as the discoverability path and the `:` **command
  line** with a live completion popup (`editor.rs:1762` `COMMANDS`).
- **The Request seam** — the pure engine never does I/O; it raises a `Request`
  the binary executes (`editor.rs:44`). Every debug action follows this: the
  engine stays pure + host-testable; the binary drives the clients. This is the
  spine of the 8e plugin architecture (§6).
- **Bonfire** — warm near-black canvas `#0e0c0c`, warm off-white text `#e4ddd8`,
  **ember `#e07840`** for the cursor / active indicator / dividers, muted
  box-drawing (never bright lines). `usr/nora/src/theme.rs` already defines popup
  surface/border/title/selected styles.
- **Diff render** — Kaua "redraws the world" each frame but `Terminal::flush`
  (`usr/lib/kaua/src/term.rs:128`) ships **only changed cells** as one batched
  escape write to fd 1 (`buffer.rs:120` `diff`). Stable-region layouts are cheap
  on the wire — the property the whole design leans on (§7).
- **The multi-source poll loop** — `PollSet` (`usr/lib/libthyla-rs/src/poll.rs:201`)
  polls fd 0 **plus** arbitrary fds in one `poll(2)` (`add_raw`, `:258`). This is
  how the LSP + DAP clients plug in without Loom (§6). *Today* Nora's
  `PollSource` hardcodes fd 0 (`kaua::source:59/81`) — 8e generalizes it via the
  `EventSource` seam.

The three honest gaps the substrate map surfaced, which this design closes:

1. **No overlay/table/tree/tabs widget, no mouse.** Kaua's v1.0 set is
   `Paragraph`, `Block`, `List` (flat, single-select, pre-formatted rows),
   `StatusLine` (`kaua::widget`). Panels are real `Layout` split tiles or
   draw-last `Block`+`List` popups (the `popup()` helper, `nora::view:401`).
   → **§3: expand the Kaua registry.**
2. **No async client, no plugin API.** No JSON-RPC/LSP/DAP client anywhere in
   `usr/`; the `MENU`/`COMMANDS` tables are `const`. Nora's subprocess model is a
   *one-shot filter* (`gofmt_source`, `main.rs:337`: write-all → close-stdin →
   read-all → reap) that **cannot** drive a persistent server without deadlock.
   → **§6: the async client is the UX.**
3. **Two-layer damage (byte-storm).** Nora runs *inside* Aurora (the fbcon,
   `usr/aurora`), whose damage granularity is the **row** — emitted bytes cost
   twice, and there is no throttle beyond the cell-diff. → **§7: the discipline.**

---

## 2. The layout — the IDE dashboard (ratified)

The dashboard was chosen (user-voted) over the two alternatives (editor-primary
focus-dock; modal EDIT/DEBUG workspaces). Its one real cost — cramped on an
80×24 console — is answered by **collapse + the size-handshake**, so it is
editor-primary until you actually debug, and roomy when the terminal is.

### 2.1 The regions

Real non-overlapping `Layout` split tiles (`kaua::layout`), never overlays:

- **Editor** (center) — the buffer, gutter with breakpoint dots + current-line
  highlight, inline diagnostics, optional inline variable values.
- **Right sidebar** — stacked tiles: **Variables** (a `Tree`), **Call Stack** (a
  `Table` with the cross-boundary divider, §5), **Goroutines** (a `Table`).
- **Bottom Console** — the run-pane: a `Tabs` of **Program** (the debuggee's real
  pts) and **Debug** (the dlv REPL / DAP console). §8.

### 2.2 Collapse behavior (decision, 2026-07-20)

**Auto-collapse when not debugging + manual toggle.** No live DAP session →
sidebar and bottom panel are collapsed; the editor is full-width with LSP inline
only (the daily-driver editing state). Starting a session (`F5` / `[Space]d d`)
expands the dashboard; ending it collapses back. Manual toggles: `[Space]d v`
sidebar, `[Space]d c` console, `[Space]d z` **zoom** the focused tile to
full-screen (the "give me the whole goroutine table" gesture). This is
editor-primary-until-you-debug realized on top of the dashboard.

### 2.3 The two size regimes

- **Roomy** (a real xterm/iTerm/Ghostty via the ratified T1 size-handshake,
  `UT-NORA-ERGONOMICS.md` T1) — the dashboard as drawn; the editor keeps ~60%+.
- **Floor** (80×24) — sidebar `Length(28)`, editor `Fill`, bottom Console
  `Length(7)`; the collapse toggles are the escape hatch when you need the full
  width for source.

```
 main.go  Go  ⏸ main.go:42                              ● 1  ▸ DBG
┌──────────────────────────────────┬─ Variables ───────────────┐
│ 40   func work(n int) int {      │ ▾ locals                   │
│ 41       sum := 0        sum=0    │   sum = 0                  │
│ 42 ●▸    for i := 0; i < n; i++ { │   n   = 7                  │
│ 43           sum += fib(i)  i=3   │   i   = 3                  │
│ 44       }                       ├─ Call Stack ───────────────┤
│ 45       return sum              │  #0 work        main.go:42 │
│ 46   }                           │  #1 main        main.go:12 │
│                                  │ ── kernel ──               │
│                                  │  #2 sched.c:sleep  chan rx │
│                                  ├─ Goroutines ───────────────┤
│                                  │  g1 running   g7 chan-recv │
├─ Console ────────────────────────┴────────────────────────────┤
│ [Program] Debug     ./work → running under Ambush             │
│ (dlv) _                                                        │
└───────────────────────────────────────────────────────────────┘
 NOR   [Space]:menu   F5 cont  F10 over  F11 into   :dlv …
```

- `●` gutter breakpoint (ember), `▸`/highlight = current line (ember), dim
  `sum=0` = inline value (an optional layer, §4).
- **Focus** — `Tab` cycles editor ⇄ sidebar tiles ⇄ Console; the focused tile
  takes an **ember border-title** (Bonfire). Keyboard-only — Kaua has no mouse.

---

## 3. The Kaua widget registry (grows with the IDE — user directive 2026-07-20)

**Principle:** every standard widget the IDE needs is added to `kaua::widget` as
first-class — reusable across all consumers, a pure value host-testable without a
terminal, on the one `Widget::render(self, area, buf)` trait. **Never a
Nora-private hack.** This *is* the "more widgets accrete later" seam
(`KAUA.md §3.3`) being realized; the debug UI is the driving consumer, but each
widget stands alone.

The additions this arc lands (8f-1), each pure + unit-tested (a widget/data layer
→ **not** audit-gated; the only audit-bearing Kaua surface is the `term` backend,
which is unchanged — `KAUA.md §10`):

| Widget | Shape | Keyboard | Driving consumer |
|---|---|---|---|
| **`Tree`** | collapsible, indented nodes; per-node expand state; one selected node; `▾`/`▸` markers | `l`/`Enter` expand, `h` collapse, `j`/`k` move, `L`/`H` recursive | Variables / Watch / the 8g resource inspector |
| **`Table`** | fixed columns, a selected row, per-row **style override**, a full-width **divider row** variant | `j`/`k`, `Enter` select, `g`/`G` | Goroutines; Call Stack (with the `── kernel ──` divider, §5) |
| **`Tabs`** | a one-row tab strip + an active index; content is the caller's | `Tab`/`BackTab` or `[Space]`-digit | the Console (Program/Debug); future multi-view tiles |
| **`Scrollbar`** | a decoration painted on a `Paragraph`/`List`/`Table`/`Tree`'s edge; position + size from (offset, len, viewport) | (follows the host) | any long pane (Console scrollback, a deep Tree) |

Notes:
- **`Tree`** is the load-bearing one — a variable inspector *is* a tree, and the
  8g resource inspector ("fd 5 ▸ `/net/tcp/3/data` ▸ a Weft flow") is the same
  widget over a different data source. Design it data-source-agnostic (a
  `TreeNode` provider), not var-specific.
- **`Table`** subsumes the call stack (columns `#idx | func | location`, kernel
  rows styled dim/ember, the boundary a divider row) and goroutines
  (`g# | state | M | blocked-on`) — one widget, so no bespoke `StackView`.
- The **Console scrollback + REPL input** reuse existing pieces: a bounded
  `Paragraph` scrollback + the ergonomics-arc **Kaua `LineEditor`**
  (`UT-NORA-ERGONOMICS.md` P1) for the dlv REPL input line — composition, not a
  new widget.
- Truecolor-only styling + the flicker-free diff render are inherited unchanged.

Widgets beyond this set accrete on the same trait as later consumers need them
(the registry is open by design).

---

## 4. The interaction map (extends Nora's idiom, invents nothing)

Three layered surfaces, matching how Nora already works:

- **Hot keys** (muscle memory) — active whenever a live DAP session exists:
  `F5` continue, `F10` step-over, `F11` step-into, `Shift-F11` step-out,
  `Shift-F5` stop, `F9` toggle-breakpoint-at-cursor. (Kaua has `KeyCode::F(u8)`.)
- **`[Space]d` debug submenu** (discoverable which-key) — extends the `MENU`
  table: `d` start/attach, `b` breakpoint (`B` conditional), `l` breakpoints
  list, `v`/`c`/`z` panel toggles/zoom, `w` add-watch, `e` evaluate, `g` switch
  goroutine, `i` the resource inspector (8g).
- **`:` command line** (power / raw) — `:break file:line`, `:cont`, `:dlv <raw>`
  (pass-through to Ambush), and the LSP verbs `:rename`, `:refs`, `:def`.

**Per-pane keys** (when a sidebar tile is focused): the `Tree`/`Table` keys above;
in Call Stack `Enter` selects a frame → the editor jumps to it (Go frame) or the
Variables tile shows the kernel "why-blocked" (kernel frame, §5); in Goroutines
`Enter` switches the active goroutine (re-roots Stack + Variables).

**LSP in the editor** (no dashboard needed): diagnostics as gutter marks +
underline (cell styling), completion via the existing `popup()`+`List` (already
how `:`-completion renders), hover/signature as a draw-last popup, `gd` go-to-def,
`gr` references, `]d`/`[d` next/prev diagnostic.

The `MENU`/`COMMANDS`/keymap tables become an **extensible registry** (8e) so the
Go plugin — and future language plugins — register their commands, keys, and
panes rather than editing `const` tables. That registry *is* the "plugin
architecture."

---

## 5. The cross-boundary stack + the superpowers

The flagship NOVEL, and identical in every layout.

**Unified stack** (the Call Stack `Table`): Go frames on top, then an **ember
full-width `── kernel ──` divider row**, then the symbolized kernel frames
(`sched.c::sleep` etc. via the Halls HX-2 `func+offset` symtab, `GO-IDE-DESIGN §4`).
Kernel rows are dim-styled so the boundary is unmistakable but not jarring.
Selecting a kernel frame shows *why it is blocked* — the exact rendez, in kernel
terms — in the Variables tile.

**8g superpowers — reserved, with their UX hooks placed now:**
- **Resource inspector** — a `Tree` over `/proc/<pid>/{ns,fd,mounts,caps,…}`:
  "goroutine 7 blocked on fd 5" expands to "fd 5 = `/net/tcp/3/data` = a Weft
  flow to 10.0.2.2:443". Slots as a sidebar tile or `[Space]d i`. Same `Tree`
  widget, different provider — the reason `Tree` is data-source-agnostic (§3).
- **Scheduler/concurrency view** — the Goroutines `Table` extended
  (`g × M × CPU × runq`); deadlock-as-graph is a delightful stretch.
- **Kernel-aware post-mortem** — on a `snare:segv`, open the *dead* process like
  a live session (wired to the Halls infra); UX hook: `[Space]d o` open-a-corpse.
- **Snapshot-debugging (Stratum)** — `[Space]d s` snapshot-at-breakpoint; branch
  or diff the session. Reserved.

These need **no new mechanism** — they are files, read through the namespace.

---

## 6. The async client (8e) — the load-bearing decision

**The client architecture is the UX.** A laggy or deadlocking client kills the
experience no matter how lovely the panes. Nora's one-shot-filter subprocess
model (`main.rs:337`) is structurally wrong for a persistent server. 8e replaces
it:

- **Persistent children.** Spawn `gopls` and `ambush` (`libthyla-rs::process`
  `Command`/`spawn`, `process.rs:181/359`) with piped stdio kept **open for the
  process lifetime**; `try_wait` (`:537`) for liveness; reap + restart on crash;
  killed on nora exit (composes #68 close-at-exit).
- **Generalized event loop.** The loop polls a `PollSet` over
  `{ fd0, gopls.stdout, ambush.stdout(DAP) }` in one `poll(2)`
  (`poll.rs:201/258`, `add_raw(child.stdout.as_raw_fd())`). A keystroke, an LSP
  message, or a DAP event all wake it. `PollSource` is generalized from fd0-only
  via the `EventSource` seam (`kaua::source:1-13`).
- **JSON-RPC framing.** `Content-Length` framing for both LSP and DAP (a small
  shared codec; the first JSON-RPC in the tree). Requests carry ids; responses
  are matched; **async notifications** (`stopped`, `output`, `publishDiagnostics`)
  update client state + **mark the editor dirty** — critical, because there is
  **no periodic tick** (`kaua::event`: `Tick` is never produced), so a
  breakpoint-hit only repaints if the loop is polling that fd.
- **Never block the UI on a round-trip.** A request is fired; the pane renders a
  cheap "…" placeholder; the response updates state on a later poll-wake. The UI
  render is a **pure function of a client-state snapshot** (DAP:
  running/stopped/frames/scopes/vars; LSP: diagnostics/completions) — the Request
  seam (§1) generalized to an async event feed.
- **The plugin seam.** A `LspClient` + `DapClient` pair behind trait interfaces;
  the Go plugin registers commands/keys/panes into the extensible registry (§4).
  Future languages implement the same traits — "the Nora side generalizes to
  future languages" (`GO-IDE-DESIGN §7`).

This is where the design care goes, and it is a real sub-arc (8e-1) that must
land before 8f has any data to show.

---

## 7. The byte-storm / two-layer damage discipline

Nora renders *inside* Aurora (the fbcon, `usr/aurora`), whose own damage
granularity is the **row** — so every emitted byte costs twice, and there is no
throttle between an app's `flush()` and the wire beyond Kaua's cell-diff. The
dashboard shows *more* at once, so the discipline is load-bearing:

- **Stable regions.** A step changes a highlight + a handful of values = a tiny
  diff. Never reflow the whole screen on a step; never full-repaint (avoid
  `clear()`/`resize` on debug events).
- **Coalesce async output** into **one frame per poll-wake** — drain all ready
  DAP/LSP messages, update state, render **once**. A burst of `output` events
  must not produce a burst of frames.
- **Bounded** Console scrollback (a ring); **no live-watch update faster than a
  step** (watches refresh on stop, not continuously); no spinners/animations
  (there is no tick anyway).
- The payoff: the dashboard stays cheap on the wire even with everything visible
  — the property that makes the ratified layout viable on the console.

---

## 8. The run-pane / Console (decision, 2026-07-20)

**Two `Tabs`: Program and Debug** — keep the debuggee's real output clean and
separate from debugger chatter (the VS Code "Terminal" vs "Debug Console" split).

- **Program** — the debuggee's actual **pts** (the PTY arc: `ptyfs` + the
  cons/consctl infra), hosting its stdio. A bounded scrollback `Paragraph` +
  passthrough input when focused. Real terminal semantics ride the audited PTY
  surface; nora is never console-attached (I-27 unaffected).
- **Debug** — the **dlv REPL / DAP console**: a scrollback + the Kaua
  `LineEditor` for input; `p x`, `bt`, and raw `dlv` (also reachable via `:dlv`).
  Structured DAP `output` events land here too.

`[Space]` + tab digit or `Tab` (when Console is focused) switches.

---

## 9. Sub-chunk plan (8e/8f)

Sequenced so the load-bearing async substrate lands first, then the UI grows on
proven data. Each is pure-userspace, kernel byte-unchanged.

- **8e-1 — the async client substrate.** Persistent-child model + the
  `PollSet`-multiplexed loop (generalize `PollSource`) + the JSON-RPC codec +
  the client-state + dirty-on-event feed. The prerequisite for everything.
- **8e-2 — the LSP client (gopls).** Diagnostics + completion + hover + go-to-def,
  **inline in the editor** (no dashboard yet — proves the client on the editing
  surface first).
- **8e-3 — the DAP client (Ambush).** Session control + the DAP state machine,
  driven headless from `:` commands (proves the debugger loop before the UI).
- **8f-1 — the Kaua widget additions** (`Tree`, `Table`, `Tabs`, `Scrollbar`),
  pure + unit-tested (§3). **Landed.**
- **8f-2 — the dashboard.** The layout + collapse + focus + the sidebar tiles
  (Variables/Stack/Goroutines) + the Console tabs, wired to the DAP client state.
  **8f-2a landed** (the skeleton: the split + collapse + `Tab` focus + all three
  tiles + the Console rendering live DAP data at a basic level — a `DebugView`
  the DAP host pushes into the `Editor`, the renderer draws; `docs/reference/
  113-nora.md`). **8f-2b-1 landed** (the tiles are navigable — a focused tile
  takes a row cursor [`j`/`k`/`g`/`G`], `l`/`h` opens/shuts the Variables group +
  steps the Console tabs, `Esc` returns to the editor, and an overflowing tile
  scrolls to the selection with a scrollbar; pure, +10 host tests). **8f-2b-2
  landed** (the tile actions: Call Stack `Enter` raises a `SelectFrame` — the host
  jumps the editor to the frame + re-scopes Variables to it; Goroutines `Enter`
  raises a `SelectGoroutine` — the host switches thread + re-roots the stack; the
  index is clamped to the live count, resolved against the host's own list, a
  no-op when not stopped; the editor half is host-tested [+6], the host half is a
  DAP round-trip covered by `dap-nora`). **8f-2b-3 landed** (the nested-lazy
  Variables tree: the DAP host owns a `VarNode` forest + fetches a node's children
  the first time it is expanded, routing each `variables` reply by its reference
  [parley now echoes the requested `variablesReference`]; the visible tree
  flattens into `DebugView.locals` so the row cursor stays a flat index, and
  `l`/`h` on an expandable variable raise `ExpandVar`/`CollapseVar`; the pure tree
  ops are the host-tested `nora::vartree` module, +8 host tests; the parley
  reference + kaua `expandable`-marker substrate landed first as 8f-2b-3a).
  **8f-2c** wires the `F5`/`F10`/`F11` hot-keys + the `[Space]d` toggles.
- **8f-3 — polish.** The cross-boundary stack divider + select-a-frame, inline
  values, the LSP editor affordances, Bonfire pass. The "this is lovely" bar.
- **8g — the superpowers** (§5): resource inspector, scheduler view,
  post-mortem, snapshot-debugging.
- **8h — the whole-arc audit + by-default ship + docs** (the debug-authority
  invariant I-39 is the audit centerpiece; the UI layers are unit-tested).

Gates: pure widget/data layers → unit tests; the interactive LS-CI (expect/PTY)
E2E for the Console + a live-session leg; no SMP gate for the pure-userspace
chunks (the Kaua/ergonomics precedent). The kernel debug surface is already
8a-audited.

---

## 10. Invariants / discipline

- **All userspace; NO new ARCH §28 invariant.** The IDE *consumes* I-39 (debug
  authority is namespace + capability bounded, enforced in the kernel debug-fs);
  the clients are ports/DAP-LSP consumers with no privilege surface. Kernel
  **byte-unchanged**.
- **I-27 unaffected** — nora is never console-attached; the Program pts rides the
  audited PTY arc; the term backend only ever touches fd 0/1.
- **The Kaua term backend** (the sole audit-bearing Kaua surface) is unchanged.
  The new widgets + the clients are pure widget/data layers → unit-tested, not
  audit-gated. The one focused round the arc owes is **8h** (the debug-authority
  centerpiece, already-landed kernel side) — not a per-widget audit.
- **Design-first** — this doc is the binding UX scripture; 8e/8f build against it;
  a deviation updates this doc first.

---

## 11. References

- `docs/GO-IDE-DESIGN.md` — the charter (vision, §3.3 the IDE layer, §4
  cross-boundary, §5 superpowers, §8 the staged plan).
- `docs/KAUA.md` — the substrate (§3 widgets/layout/render/input, §4 the poll
  loop, §6 Bonfire, §10 the audit posture).
- `docs/UT-NORA-ERGONOMICS.md` — Nora's established idiom (the `[Space]` menu, the
  `LineEditor`, the T1 size-handshake).
- `docs/DEBUG-FS-DESIGN.md` + ARCH §28 I-39 — the kernel debug surface + the
  bounded-authority invariant.
- `docs/GOPLS-PORT-DESIGN.md`, `docs/DELVE-PORT-DESIGN.md`,
  `docs/reference/137-gopls.md`, `docs/reference/134-debug-fs.md` — the engines,
  as-built.
