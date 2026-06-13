# Kaua — the native console TUI substrate

**Status: binding design — SIGNED OFF 2026-06-13.** The first-class console
presentation layer for Thylacine: a native (`no_std` + `alloc`, libthyla-rs)
immediate-mode text-UI library over `/dev/cons` + `/dev/consctl`, plus its first
consumer, the `nora` editor. This document is the canonical scripture; ARCH
§23.12 and LIFE-SUPPORT.md LS-7 point here. Builds as a small arc of audited
sub-chunks (§9); the cons/consctl backend + the ut raw-mode dance are
audit-bearing (I-27); the buffer/widget core is pure, testable userspace.

Origin: a design conversation (2026-06-13) that began with porting one editor
(Stratum's `tui/src/editor.rs`) and converged on the realization that *building
the editor well already forces ~90% of a reusable substrate* — so we build the
substrate once, beautifully, and let the editor be its first consumer.

---

## 1. Naming

The Kauaʻi ʻōʻō (*Moho braccatus*) was a small black Hawaiian forest bird with
yellow leg-feathers. It was the **last surviving member of the entire family
Mohoidae** — when it went extinct (last heard 1987; the famous Cornell recording
is a lone male singing a duet for a mate who would never answer), a whole lineage
of birdsong ceased to exist with it. We name the layer through which the system
*speaks to its first users* after that lost song: a console TUI is the OS's
voice, and the thylacine's own story — extinct, then perhaps not — is the same
story this bird tells.

**`kaua`** is the library (ASCII identifier, from *Kauaʻi*). **`nora`** is the
editor built on it (the established name from the auxiliary-track plan).

### 1.1 Why Kaua is *not* in the weave family (the Loom question, decided)

There is a deliberate naming distinction from the **Loom** family:

| Name | Role | Woven on Loom? |
|---|---|---|
| **Loom** (`docs/LOOM.md`) | the instrument: the io_uring-inverted 9P ring | — |
| **Tapestry** (`docs/TAPESTRY.md`) | the *graphics* weave: GPU framebuffer scanout (Phase 8) | **yes** |
| **Kaua** (this doc) | the *text* weave: console presentation (v1.0) | **no** |

The weave names (`Tapestry`, the reserved `Weft`) are *earned* by genuinely
operating a Loom ring — Tapestry's present/input/vsync are real Loom ops. We
asked whether a console TUI should leverage Loom and answered **no, for v1.0**,
on three honest grounds:

1. **The console is not 9P-backed.** As Loom is built (Loom-6), every op
   dispatches to a 9P-client fid (a registered handle resolving to a
   `dev9p (client, fid)`). `/dev/cons` is a *local kernel Dev* (devcons/devdev),
   not a 9P session — Loom cannot drive it without a kernel extension to dispatch
   ops to local Devs (real, audit-bearing Loom-surface scope).
2. **The perf does not need it.** A TUI runs at human-typing speed; syscall
   amortization is irrelevant — the opposite of Tapestry's 60fps present, where
   Loom genuinely hides the IRQ behind the next frame.
3. **LS-8c already is the v1.0 async loop.** The pollable console (LS-8a) + the
   multi-fd poll (LS-8c) already deliver "wait on cons + notes in one loop."

Forcing Loom in would be unverified complexity bought for decorative coherence —
exactly what the prime directive ("complexity only where verified") forbids. So
Kaua stands on its own name, and its event-loop abstraction is left behind a
**seam** (§4.4) so it *can* move onto Loom later. **`Weft` is reserved** for the
day a console substrate genuinely weaves on Loom (e.g. a unified Loom event loop
when Tapestry/Halcyon makes one natural, or a deliberate Loom-everywhere
investment). Until then, the text weave is Kaua, and it earns its own face.

---

## 2. Thesis

Thylacine's thesis is *the shell is sufficient as a UI*. For a console-first OS,
the **TUI is the product** — it is the first thing a user sees and the medium of
every interaction. A janky TUI undercuts the whole pitch; a beautiful one sells
it. So the console presentation layer is not a utility to be hand-rolled per
program; it is load-bearing for the system's identity, and it deserves to be
built once, well, and reused.

The rendering model is *solved* — ratatui's **immediate-mode + double-buffered
cell-diff** architecture is the proven SOTA, used ubiquitously. Kaua brings that
model **native**, over Thylacine's own device model (`/dev/cons` + `/dev/consctl`
rather than libc/termios), with the capability discipline (the console is
acquired through the inherited consctl fd — the #94-B-b controlling-terminal
handshake — never ambient `/dev/tty`) and the committed visual identity (the
*Bonfire* palette, `UTOPIA-VISUAL.md`). "Complexity permitted only where
verified" applies cleanly: we implement a verified-by-ubiquity model well; we do
not invent a rendering paradigm.

### 2.1 Why a substrate, not nora-private layers

Building `nora` *properly* already forces three layers: (1) raw-console I/O, (2)
a cell-buffer + diff renderer, (3) the modal editing logic. Layers 1 and 2 *are*
a TUI substrate. The only delta between "nora-private modules" and "`kaua`, a
reusable library" is: make the API clean and general, and add a handful of
widgets nora alone would not need. **Small delta, large payoff** — the difference
between throwaway code and the system's presentation identity, reused by:

- **`nora`** — the editor (the first real consumer; §8).
- **`ut`** — a richer prompt, completion menu, and `jobs`/history overlay (a
  fast-follow consumer; the shell already owns the console line discipline since
  #94-B-b).
- **The SAK overlay** — the I-27 elevation prompt, rendered through Kaua for a
  coherent, beautiful trusted-path UI (a *kept-in-mind* consumer; §7).
- **Future native TUI apps** — a file manager, `top`/`ps` viewer, a `/ctl`
  admin TUI, the network configurator.

Per *system-over-cost*, that is not a close call: the substrate is the asset.

---

## 3. Architecture

Three thin layers, Plan-9-shaped at the bottom (devices, not ioctls) and
ratatui-shaped in the middle (immediate-mode cells), with the modal editor on
top:

```
  ┌─ nora (the editor) ──────────────────────────────────────────────┐
  │  modal core (Normal/Insert/Visual/Command, from editor.rs)        │  app
  │  + a native text/edit buffer (replaces tui-textarea)              │
  ├─ kaua::widget / layout / event ──────────────────────────────────┤
  │  Widget trait; Text/Paragraph, Block, List, StatusLine; Rect      │  middle
  │  + simple Layout; KeyEvent/KeyCode; the event-source seam (§4.4)  │  (immediate-mode)
  ├─ kaua::buffer ───────────────────────────────────────────────────┤
  │  Buffer (grid of Cell{grapheme,Style}); double-buffer + cell-diff │
  ├─ kaua::term (the backend) ───────────────────────────────────────┤
  │  cons/consctl I/O: escape-seq input parse -> KeyEvent;            │  backend
  │  damage -> escape emit (alt-screen, cursor, SGR, cells) -> fd 1   │  (device + capability)
  └───────────────────────────────────────────────────────────────────┘
         reads fd 0 (raw)  /  writes fd 1   — the inherited console
                  ▲ raw-mode set by ut via /dev/consctl (the dance, §5)
```

`kaua` lives at `usr/lib/kaua/`; `nora` at `usr/nora/`. Both native libthyla-rs
(`no_std` + `alloc`), built via the `aarch64-thylacine` target.

### 3.1 `kaua::buffer` — the cell model and the diff

The heart of immediate-mode rendering, lifted in *architecture* from ratatui:

- **`Style`** — `fg: Color`, `bg: Color`, `attrs: Attr` (a bitset: BOLD, DIM,
  ITALIC, UNDERLINE, REVERSE). `Color` is a truecolor `Rgb(u8,u8,u8)` (the
  *Bonfire* palette is hex; the host terminal under QEMU supports `ESC[38;2;r;g;bm`)
  plus a `Reset`/default variant. No 16/256-color indirection at v1.0 — truecolor
  is the target; an indexed fallback is a documented seam.
- **`Cell`** — a `grapheme` (a small inline string: one `char` for the common
  case, a short buffer for wide/combining clusters) + a `Style`. Width-aware: an
  East-Asian-wide cluster occupies two columns (the trailing column is a marked
  continuation cell). v1.0 ships a *minimal* width table (ASCII + box-drawing = 1,
  common CJK ranges = 2, combining = 0); a full `unicode-width` table is a seam.
- **`Buffer`** — a `Rect`-sized, row-major `Vec<Cell>` plus the area origin. The
  app draws into it through widgets; it is a *pure value* (testable without any
  terminal).
- **The diff** — the `Terminal` (in `kaua::term`) holds two Buffers, `front`
  (on-screen) and `back` (the frame under construction). Each frame the app
  clears `back` and re-draws *everything*; `Terminal::flush()` walks the two
  buffers cell-by-cell, and for each changed cell emits a minimal escape run
  (a cursor move only when the column is non-contiguous, an SGR only when the
  style changes), then swaps `front`/`back`. This is what makes app code simple
  ("redraw the world") and the wire efficient + **flicker-free** (one batched
  `write(fd 1)` per frame, only changed cells).

### 3.2 `kaua::layout` — Rect splitting

A `Rect{x,y,w,h}` and a *simple* constraint splitter: `Layout::vertical` /
`Layout::horizontal` over a slice of `Constraint::{Length(n), Min(n), Pct(n),
Fill}`, returning sub-`Rect`s. No cassowary-class solver at v1.0 — a single-axis
greedy pass (fixed lengths first, then distribute the remainder to Fill/Min/Pct)
covers every v1.0 layout (an editor body + a status line; a list + a detail
pane). The richer solver is a seam.

### 3.3 `kaua::widget` — the trait and the v1.0 set

```
trait Widget { fn render(self, area: Rect, buf: &mut Buffer); }
```

The scoped v1.0 set (exactly what nora + ut + a SAK prompt need):

- **`Text` / `Paragraph`** — styled, optionally wrapped, optionally scrolled
  lines. The editor body and any message pane.
- **`Block`** — an optional border (box-drawing, *Bonfire* `border` colour) +
  optional title; an inner-area helper. The frame around panes/popups.
- **`List`** — a vertical list with a selected row (selection styled
  `surface`/`ember`). Completion menus, file pickers, `jobs`.
- **`StatusLine`** — a one-row left/center/right segmented bar (mode indicator,
  filename, position). Every TUI's bottom line.

More widgets (Table, Tabs, Gauge, a scrollbar) accrete later, on the same trait.

### 3.4 `kaua::event` — keys

A terminal-agnostic event model the backend produces:

- **`KeyEvent { code: KeyCode, mods: Mods }`** — `KeyCode::{Char(char), Enter,
  Esc, Backspace, Tab, BackTab, Left, Right, Up, Down, Home, End, PageUp,
  PageDown, Delete, Insert, F(u8)}`; `Mods` = a bitset (CTRL/ALT/SHIFT) recovered
  where the encoding allows (C0 control bytes -> CTRL+letter; CSI modifier
  params).
- **`Event::{Key(KeyEvent), Resize(u16,u16), Tick}`** — the loop's message type.
  `Resize` is a v1.0 seam (no `SIGWINCH` analog yet — the console size is read
  once at startup; a `/dev/cons` size-query + a resize note is a documented
  follow-on). `Tick` is an optional timer message (for cursor blink / animations;
  v1.0 may omit).

### 3.5 `kaua::term` — the backend (audit-bearing)

The only device/capability-touching layer, and the only audit-bearing one. It:

- **Acquires the screen, not the line discipline.** On init it emits the
  *screen-control* escapes to **fd 1** — enter alt-screen (`ESC[?1049h`), hide
  cursor (`ESC[?25l`), clear — and reads the console size. The *line discipline*
  (cooked vs raw termios) is **not** Kaua's to set: it is owned by `ut` via the
  consctl fd and established by the dance (§5) *before* nora is spawned. Kaua
  reads fd 0 assuming the bytes already arrive raw.
- **Parses input.** Reads available bytes from fd 0 (the pollable cons, LS-8a)
  and runs a VT/ANSI state machine: UTF-8 text → `Char`; C0 controls → `Enter`
  (CR/LF), `Backspace` (DEL/BS), `Tab`, `Esc`, `Ctrl+letter`; CSI sequences
  (`ESC[A/B/C/D` arrows, `ESC[H/F` or `ESC[1~/4~` home/end, `ESC[5~/6~`
  page, `ESC[3~` delete, with optional `;mod` modifier params) → the
  corresponding `KeyCode`. A lone `ESC` with no following bytes within the read
  is `KeyCode::Esc` (the editor's mode-exit).
- **Emits frames.** `Terminal::flush()` (§3.1) writes one batched escape buffer
  to fd 1 per frame.
- **Restores on teardown.** On clean exit (`Terminal::leave()` / `Drop`) it
  emits leave-alt-screen (`ESC[?1049l`) + show-cursor (`ESC[?25h`). Because
  `no_std` apps run `panic = abort` (no unwinding → `Drop` does **not** run on a
  panic/kill), **`ut`'s restore (§5) is the authoritative backstop**: ut re-emits
  both the screen-reset escapes and the cooked-termios consctl write after
  reaping the child, so a crashed editor never leaves a wedged console.

The backend targets a **VT100/xterm-class terminal**. At v1.0 that is the host
terminal driving the serial console (`-serial mon:stdio`, a real xterm/iTerm
supporting alt-screen + truecolor + CSI). Halcyon (Phase 8) renders the same
escapes natively on Thylacine's own framebuffer terminal.

---

## 4. The event loop and the Loom seam

### 4.1 The v1.0 loop

```
let mut term = Term::enter()?;            // alt-screen, hide cursor, read size
let mut app  = App::new(...);
loop {
    term.draw(|buf| app.render(buf))?;    // clear back, widgets draw, diff+flush
    match app.poll_event(&mut term)? {    // poll fd 0 (+ other sources, §4.3)
        Event::Key(k) => if app.on_key(k).is_quit() { break },
        Event::Resize(w,h) => term.on_resize(w,h),
        Event::Tick => app.on_tick(),
    }
}
// term.leave() via Drop; ut's restore is the crash backstop
```

### 4.2 Polling is LS-8c, not a busy-read

`poll_event` blocks in a `poll(2)` over fd 0 (the pollable cons, LS-8a) — never a
spin. This is the proven LS-8c mechanism (which already polls cons + the shell
note fd). The console read is **#811 death-interruptible**, so a dying editor
unwinds cleanly.

### 4.3 Multiple event sources (still v1.0)

The loop generalizes to poll fd 0 *plus* other fds in one `PollSet` — exactly
how LS-8c polls cons + notes. A TUI app that wants async events (a background job
finishing, a watched 9P file changing, a timer) adds those fds to the set and
dispatches on which is ready. This is the v1.0 unified event loop; it needs no
Loom.

### 4.4 The Loom seam (left open, not built)

The event source is an internal trait (`EventSource: produces Event, exposes the
fds to poll`). v1.0 has exactly one implementation, `PollSource` (the LS-8c
poll). The trait is the seam: a future `LoomSource` (input as a multishot
`LOOM_OP_READ`, frames as async `LOOM_OP_WRITE`, all event sources draining one
CQ) can replace it *with zero change to widget/buffer/app code* — the same
decoupling Tapestry uses (`MockLoom` → `ThylaLoom`). Building it requires
extending Loom to drive local Devs (§1.1); it is deferred until a verified need
exists. When it lands, the substrate may earn the reserved `Weft` name.

---

## 5. The ut raw-mode dance (the LS-7 other half; I-27-preserving)

A full-screen editor needs the console in **raw** mode (every keystroke as a
byte; no canonical line-buffering; no echo; and crucially `ISIG` off so `Ctrl-C`
is a keystroke the editor handles, not the `interrupt` note that would kill it).
Per #94-B-b, the consctl fd that sets the line discipline is owned **privately by
`ut`** — the editor (a user child) does not hold it, and `ut` must not re-forward
it (I-27). So `ut` is the **controlling-terminal agent** for its raw children:

1. **Before spawn** — `ut` detects the foreground command is a raw-mode TUI app
   (v1.0: a small `is_raw_command(name)` set — `nora`; extensible) and writes the
   **raw** mode string to its consctl fd: `-icanon -echo -isig -icrnl -onlcr`
   (the only delta from ut's own prompt default `CONSOLE_MODE_DEFAULT` =
   `-icanon -echo +isig -icrnl -onlcr` is `ISIG` off). New
   `Repl::console_raw()` / `Repl::console_cooked()` on the #94-B-b
   `console_apply_default` primitive.
2. **Spawn** — the child's stdin is switched from the shell's pipeline default to
   `Inherit` (raw bytes flow to the editor's fd 0); the editor is **never** given
   the consctl fd. `stmt.rs:84` already marks interactive-child stdin as the
   LS-5/LS-8 touch point.
3. **After the child exits or dies** — `ut` (the survivor, still in its poll
   loop, reaping the child) **restores**: it re-emits the screen-reset escapes
   (`ESC[?1049l ESC[?25h`, leave alt-screen + show cursor) to the console **and**
   writes `CONSOLE_MODE_DEFAULT` back to consctl. This restore is the
   crash-safety net — it runs whether the editor exited cleanly or was killed
   mid-edit, so the console is never left raw + alt-screened with no editor.

**I-27 is preserved**: the editor is not console-attached and never holds
consctl (the SAK/elevation gate keys on `PROC_FLAG_CONSOLE_ATTACHED`, which the
editor lacks); consctl stays inside the trusted chain (joey → login → ut); the
inherited consctl fd remains the capability, never re-forwarded.

**Considered and rejected — forwarding consctl to nora (D1b).** Letting `ut`
inherit-forward the consctl fd to the editor so the editor sets its own raw mode
(the Plan 9 "app opens /dev/consctl" idiom) was rejected: it would *reverse* the
audited #94-B-b property ("ut never re-forwards consctl to a user child") and
widen the consctl-holding set from the trusted session chain to an arbitrary
editor. The ut-as-agent model (D1a) keeps the capability where it belongs and
makes ut the reliable restorer. The v1.x general mechanism (a per-exec "raw"
attribute, or a capability-scoped consctl-for-the-controlling-terminal) is a
documented seam.

---

## 6. The aesthetic — *Bonfire* (love at first sight)

The substrate's defaults are the system's first impression, so they are not
left to each app. Kaua's default theme **is** the committed *Bonfire* identity
(`UTOPIA-VISUAL.md`, the U-2 palette), consumed from `libutopia::palette` where
the types allow and mirrored in `kaua::style` otherwise:

| Kaua role | Bonfire | Use |
|---|---|---|
| background | `bg #0e0c0c` | the canvas — warm near-black |
| pane surface / selection | `surface #180f0e` | popups, selected rows |
| borders / dividers | `gutter #2a1f1c` / `border #3a2a26` | `Block` borders, the status rule |
| primary text | `fg #e4ddd8` | all content — warm off-white |
| secondary / chrome | `fg_dim #c8bdb8` / `fg_muted #9a8f8a` | inactive panes, mode indicators |
| decoration | `fg_subtle #5a4e48` | indent guides, gutter annotations |
| accent / cursor | `ember #e07840` | the cursor, active indicator, the `⊢` glyph |

Plus: clean box-drawing (the `border` colour, not bright lines); flicker-free
diff rendering (no full-screen repaints); a considered cursor (the ember block in
Normal mode, a dimmer bar in Insert — Kaua exposes cursor-shape escapes). The bar
for v1.0: a first-time user opening `nora` should think *this is lovely* before
they think about features.

---

## 7. SAK overlay — a kept-in-mind consumer, not v1.0

Rendering the SAK elevation prompt through Kaua is a worthy target (a coherent,
beautiful trusted-path UI) and a good forcing function for a clean API — but SAK
is **I-27 security-critical**: the prompt must be *unspoofable*. Kaua must be
usable **by** the trusted renderer without Kaua itself becoming a spoofing
vector (a user app must never be able to draw a convincing fake SAK). That is its
own careful, security-audited design. So at v1.0 SAK is a consumer we **keep the
API honest for** (no console-attach assumption baked into the backend; the
backend works equally for a trusted or untrusted caller because it only touches
fd 0/1, never the attach bit) — the actual SAK-via-Kaua rework is a separate
follow-on, not part of the Kaua arc.

---

## 8. `nora` — the first consumer

A modal, full-screen editor, native, on Kaua. Its modal *logic* is adapted ~1:1
from Stratum's `tui/src/editor.rs` (a clean, proven 311-line vim/helix-style
layer); its *engine* (the text buffer beneath the modal logic) is native, because
`editor.rs` rests on `tui-textarea` + `ratatui` + `crossterm` (all std + Unix-
terminal-coupled, none portable).

**v1.0 scope** (the `editor.rs` set + what a standalone editor needs + a few
fitting extras):

- **Modes**: Normal / Insert / Visual / Command, with the ember-coloured mode
  indicator in the status line.
- **Navigation**: `h j k l` + arrows, `0 $` line ends, `g G` top/bottom, `w b`
  word motion, PageUp/Down.
- **Editing**: `i a o A` insert entries, `x` delete-char, `d` delete-line, `u`
  undo (a small undo ring), Insert-mode text/Backspace/Enter.
- **Visual**: `v` select, motion extends, `d`/`x` cut, `y` yank (an *internal*
  register — the macOS `pbcopy`/`pbpaste` clipboard of `editor.rs` is dropped;
  a system clipboard is a Halcyon-era concept).
- **Commands**: `:w` (save), `:q` / `:q!` (quit / discard), `:wq`, with the
  unsaved-changes guard.
- **Standalone file I/O** (what `editor.rs` got from its host app): open a named
  file via the namespace (`nora <path>`), read it through `kaua`/libthyla-rs fs,
  `:w` writes via the FS-gamma durable path (write-temp + rename-swap where the
  backing FS supports it, else direct write + fsync).
- **Fitting extras** (the "as we see fit" latitude): a line-number gutter
  (toggle), `/` incremental search, `:e <path>` open-another-file. Kept modest;
  more is v1.x.

The editing **engine** (`nora::buffer`: a line-vector text model with a cursor,
viewport/scroll, a selection range, and a bounded undo ring) is the native
replacement for `tui-textarea` and exposes the operations the modal layer calls
(cursor moves, insert/delete, undo, selection). It is pure userspace, unit-tested
without a terminal.

---

## 9. The arc

One small arc of focused sub-chunks; the backend + dance sub-chunk is
audit-bearing, the rest is pure userspace:

| Chunk | Scope |
|---|---|
| **T-0 — scripture** (this doc) | `docs/KAUA.md` + ARCH §23.12 pointer + §25.4 audit row + LIFE-SUPPORT LS-7 reconciliation + memory. No code. |
| **T-1 — the core** | `usr/lib/kaua` `buffer` (Cell/Style/Buffer/diff) + `term` (cons/consctl backend: input parse, escape emit, the double-buffer `Terminal`). Hello-world: a styled bordered box + text, flicker-free. *Audit-bearing* (the backend; but no dance yet — driven by a test harness). |
| **T-2 — widgets + layout + event** | `layout` (Rect split), `widget` (the trait + Text/Block/List/StatusLine), `event` (KeyEvent + the `EventSource` seam + `PollSource`). |
| **T-3 — nora** | `usr/nora`: the modal core (from `editor.rs`) + `nora::buffer` (the native text engine) + standalone file I/O, on Kaua. |
| **T-4 — the ut dance + integration** | `Repl::console_raw`/`console_cooked` + the `is_raw_command` spawn path + the restore-on-exit backstop (`stmt.rs`); the `ls-7` LS-CI E2E (open a file, edit, save, `cat` shows the edit). *Audit-bearing* (the dance touches I-27). |
| **audit** | one focused Opus-4.8-max round over T-1's backend + T-4's dance (the I-27 surface) + a self-audit; the buffer/widget layers are covered by unit tests. |

**Test posture**: the buffer/widget/layout/event layers are pure and unit-tested
(cell-diff correctness, layout splits, the input-parser truth table, the editor
state machine) without any terminal. The backend + dance are validated by the
`ls-7` LS-CI (expect/PTY: launch `nora`, send keys, save, assert the file
changed) + boot OK + the focused audit. No SMP gate is *required* by the
userspace layers, but the #94-B-b dance touches the kernel consctl path
(byte-identical — no kernel change), so the gate is run if any kernel file moves.

---

## 10. Invariants and audit posture

**No new ARCH §28 invariant.** Kaua *consumes* existing scripture:

- **I-27 (trusted path)** — the ut dance (§5) is the console *capability*
  handshake; the editor never holds consctl and is never console-attached, so the
  SAK/elevation gate is untouched. This is the audit-bearing property.
- **I-9 (no lost wakeup)** — the event loop polls via LS-8c's pollable cons (the
  LS-8a deferred poll-wake, `cons_poll.tla`); Kaua adds no new wait/wake
  mechanism, only a consumer.
- **The LS-8b consctl mechanism** — the raw/cooked mode strings Kaua's dance
  writes are the already-audited `cons_set_mode_cmd` flags under `g_cons.lock`
  (no new kernel state).

The **audit-bearing surface** is `kaua::term` (the cons/consctl backend: bounds
on the input parser, no OOB on the escape emit, the fd 0/1 discipline) + the ut
dance (the I-27 properties above + the restore-on-every-exit-path backstop). The
buffer/widget/layout layers are pure userspace — a bug there corrupts only the
app's own screen, never a privilege or safety boundary — so they are
test-covered, not audit-gated. The §25.4 audit-trigger row records this.

---

## 11. Coordination with the auxiliary track

The auxiliary track (`aux/userspace-apps`, which owns `usr/apps/**`) carries a
roadmap item "the nora editor arc (ratatui forked native)". **That folds into
Kaua**: MAIN owns the console TUI substrate (`usr/lib/kaua`) and the runtime
editor (`usr/nora`); the aux ratatui-fork plan is superseded by the focused
native Kaua core (an editor needs ~10% of a general TUI framework, so a from-
ratatui fork is more surface than warranted). If the aux track later produces a
richer native TUI substrate or widget set, Kaua can adopt pieces — but v1.0 does
not wait for it, and nora is built directly on Kaua. (CLAUDE.md records this
boundary so it is common knowledge for the aux agent, which reads it.) Distinct
from the aux `libtapestry` POC, which is the *graphics* weave (Tapestry,
`docs/TAPESTRY.md`) — a different layer entirely.

---

## 12. Status

| Item | State |
|---|---|
| T-0 scripture | this doc (SIGNED OFF 2026-06-13) |
| T-1 core | **landed** -- `style`/`rect`/`buffer`/`event`/`input`/`encode` (pure, host-tested) + `term` (the cons backend); `docs/reference/112-kaua.md` |
| T-2 widgets | not started |
| T-3 nora | not started |
| T-4 dance + LS-CI | not started |
| audit | not started |

The `Weft` name is reserved (§1.1). The full-`unicode-width` table, the richer
layout solver, `Resize`/`SIGWINCH`, an indexed-color fallback, the Loom event
source (§4.4), and the SAK-via-Kaua rework (§7) are documented seams.

**Syntax highlighting** is a v1.x `nora` feature, not v1.0, and its substrate is
a deliberate fork in the road:

- **A native lexer highlighter (the v1.x default).** A small per-language
  tokenizer (keywords / strings / comments / numbers / identifiers → *Bonfire*
  styles) in pure Rust, `no_std`, tiny. Less precise than a real grammar (no AST,
  no error recovery) but the standard pre-tree-sitter approach and entirely
  native — no new toolchain. This is the recommended first step.
- **tree-sitter (a deliberate follow-on).** *Feasible, gated on a toolchain
  decision, not on tree-sitter being hard.* A grammar is generated C
  (`parser.c`, occasionally a `scanner.c`) that compiles cleanly for
  `aarch64-thylacine`; the `libtree-sitter` runtime is a small, self-contained C
  library needing only a ~20–30-function libc shim (the `alloc` family →
  Thylacine's allocator, the `mem*`/ctype/`wctype` set, `assert`/`abort`);
  highlight queries (`.scm`) are just data nora ships and maps to Kaua `Style`s.
  The real cost is the **substrate boundary**: nora is native `no_std` libthyla-
  rs, tree-sitter is C, and linking C into a native `no_std` binary is the
  "native program links a ported library" case CLAUDE.md flags as *not v1.0 +
  escalation-worthy* (it needs a native-target C-build path + the shim — a
  meaningful new direction). It pairs naturally with the **Phase-8 on-system
  toolchain** (#67), which establishes exactly that native-C story; C++ external
  scanners (a minority of grammars) add a C++-runtime wrinkle and can be excluded
  at first. So: native lexer at v1.x; tree-sitter when the toolchain direction is
  taken deliberately, with the grammars as a natural first consumer.
