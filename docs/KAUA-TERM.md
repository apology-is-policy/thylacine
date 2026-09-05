# KAUA-TERM.md -- the per-tile terminal emulator (the Halcyon terminal substrate)

The `kaua-term` is the per-tile terminal-emulator process that hosts a program
(native `ut` OR an unmodified Linux binary) on a pseudoterminal, parses its
full-xterm output into a cell grid, feeds an ordered cell-record stream to
halcyond (which rasterizes + composites -- the render seam = B, section 1b), and
re-encodes input back to the pts. It is the per-tile terminal for Halcyon's
"the terminal is the desktop" model, and it shares the full-xterm PARSER crate
(`usr/lib/vt`) with `aurora` (the trusted-console renderer).

**Status: DESIGN (ratified, unbuilt).** This is the design-conversation output,
committed BEFORE code per the design-first policy. The as-built reference
(`docs/reference/NN-kaua-term.md`) lands with the code.

**Provenance.** Ratified by the operator on 2026-09-03 ("Ratify -- write
scripture") on the single unified fork from the main<->aux co-design (yip call
`0044`). Both tracks converged independently on this design after grounding in the
tree. The compositor/tile/placement/transcript half is MAIN's scripture
(`HALCYON.md` + `TAPESTRY.md` sections 13-14); this doc is the AUX half (the
terminal emulator + the kernel/viv ioctl reach). Per the doc-ownership split, this
doc does NOT restate or edit the compositor side -- it references it.

Cross-refs: `docs/TAPESTRY.md` sections 13-14 (the compositor, placement-transparent
surfaces D5-D7, the anti-window tile model -- MAIN); `docs/KAUA.md` (the cell-diff
TUI library this reuses); `docs/AURORA.md` (the console renderer this subsumes);
`docs/PTY-DESIGN.md` (the pts substrate this hosts on); `docs/VIVARIUM.md` (the
Linux phenotype whose ioctls C2-k1c reaches).

---

## 1. The ratified decision (the fork the operator voted on)

The load-bearing fork was the per-tile terminal SUBSTRATE for N console/terminal
tiles. Three shapes were weighed (heritage + SOTA):

- **(a) compositor-served per-tile `/dev/cons`** -- the Plan 9 rio model (rio
  serves `/dev/cons` per window). Rejected: a Linux binary needs a pts anyway
  (job control + termios), so a per-tile `/dev/cons` would be a SECOND terminal
  substrate alongside the pts -- pure duplication. (Main started here and moved
  off it after grounding.)
- **(b) uniform-pts-per-tile + a shared kaua-term** -- the tmux/kitty model on the
  built ptyfs, fused with the Thylacine `kaua`/`aurora` render substrate. RATIFIED.
- **(c) kernel multi-console** -- the cons layer goes per-tile. Rejected: most
  invasive, and unnecessary since the pts substrate already ships.

Why (b): `ut` is already substrate-agnostic (it reads inherited fd 0/1/2 and does
its own raw-mode line editing; it runs identically on a pts today via `ptyhost`),
and nothing avoids a VT round-trip today (aurora is already a VT-parser -> cells
-> a tapestryd surface). So a per-tile terminal emulator on a pts is the minimal
mechanism that serves BOTH native and Linux programs, with one render substrate.

**The split (ratified).**

- **MAIN (halcyond / tapestryd):** tiles + placement + promotion + composition +
  the transcript / inline-media (`TAPESTRY.md` 13-14).
- **AUX (this doc):** the kaua-term COMPONENT -- full-xterm parse + pts master host
  + kaua `Buffer` render + input re-encode. Its PROCESS TOPOLOGY (an in-halcyond
  module vs a per-tile process vs hybrid) is an OPEN operator fork -- section 1a.
- **KERNEL / viv (aux):** C2-k1c -- the pts termios+winsize ioctl reach for a
  Linux program (section 5). C2-k3 (Linux job-control ioctls) is a follow-on.

---

## 1a. The process topology -- RATIFIED: uniform per-tile process (Y), 2026-09-03

**Every tile is its own kaua-term PROCESS** -- native `ut` and Linux binaries
alike. Each kaua-term holds its tile's pts master, parses, and feeds an ordered
cell-record stream to halcyond, which rasterizes + composites (the render seam =
B, section 1b). There is no in-halcyond terminal pane; halcyond has ONE
ingest+render path, applied uniformly. The operator ratified this on 2026-09-03
(main teed the fork; the operator "voted isolated" = uniform-Y).

The fork (for the record): (X) in-process -- halcyond holds N VT/transcript
instances via the shared `usr/lib/vt` crate, matching HALCYON 13.1's single-brain
pattern; (Y) per-tile process -- crash-isolated; HYBRID -- native in-process,
Linux isolated. Uniform-Y (chosen) subsumes the mandatory half and adds
uniformity.

Why it is sound (and why the mandatory half forced at least (Y) for Linux): a
full-xterm parser is a state machine on UNTRUSTED bytes (a Linux binary emits
anything); aurora + kaua are `no_std` with panic=abort (aurora `main.rs:870`
"panic -> no_std abort -> a dark console"; kaua `term.rs:21`), so a panicking edge
aborts the process. In (X) that would abort halcyond = the WHOLE environment. A
per-tile process confines a parser panic (or any tile-local fault) to that ONE
tile -- the same crash-isolation that motivates 13.1 (halcyon-gpu is a child for
exactly this reason), now applied to every tile. The cost the operator accepted:
a native `ut` tile is a process too (cheap) rather than an in-halcyond pane; the
gain is one uniform halcyond path + uniform isolation.

Consequences now FIRM (topology + the seam both ratified):
- The kaua-term is a per-tile PARSER+producer process; it does NOT own a surface
  or rasterize -- it feeds the ordered cell stream to halcyond (seam=B, 1b), and
  halcyond rasterizes + composites.
- The **live-screen<->transcript seam** (section 1b, the record contract:
  CellDiff/ScrollOff/Control/Mode up; Key/Resize down) is a real cross-process
  seam, defined once for EVERY tile (native + Linux) -- RATIFIED (B).
- KT-1 (section 6) is a kaua-term process hosting `ut`, native included.

The trusted CONSOLE renderer (the SAK sink on `/dev/cons`, subsuming aurora --
section 2 "console mode") is a distinct role from a tile and is unaffected by this
tile-topology decision; it renders `/dev/cons`, not a pts, and is suspended during
a framebuffer SAK episode (18.7).

---

## 1b. The live-screen<->transcript seam -- RATIFIED: (B) FEED-CELLS, 2026-09-03

The kaua-term FEEDS CELLS to halcyond; halcyond rasterizes + owns the transcript.
The operator ratified (B) (main+aux call 0045); (A) render-pixels was ruled OUT --
it hands halcyond only pixels, so the rich transcript / Helix-modal / inline media
would force fontdue + the whole transcript machinery INTO every kaua-term process
(N copies). (A)'s genuine buys (D7-purity, parallel per-process rasterization) were
noted and outweighed by the rich-transcript vision. The mandatory crash-isolation
is preserved under (B): the isolated part is the hostile-input PARSER (in the
kaua-term process); halcyond's renderer sees only TRUSTED cells.

**THE CONTRACT (firm; both tracks concur). One ordered record stream up, a small
stream down. The record ORDER is load-bearing (it delimits Beacon zones); the
kaua-term emits in VT-stream order, flushing a pending CellDiff at every boundary.**

kaua-term -> halcyond (ordered):
- `CellDiff { changed (row,col,cell)[], cursor(row,col,vis) }` -- the live screen.
  (Cells are position-keyed, so intra-batch order is irrelevant -- only the
  boundary order between records matters.)
- `ScrollOff { rows: cell[][] }` -- normal-mode lines off the top -> the transcript.
- `Control { osc1936_raw(bytes) | bell | title(str) | exit(code) | winsize_ack }`
  -- the kaua-term forwards OSC 1936 (Beacon-zone frames) RAW, uninterpreted
  (halcyond keeps the Beacon parser -- R5 + its format-fuzz surface), plus BEL,
  OSC 0/2 title, the hosted child's exit code, and a winsize ack.
- `Mode { normal | alt_screen }` -- the ?1049/47/1047 flip; `alt_screen` => a full
  live grid, NO ScrollOff; `normal` => ScrollOff appends to the transcript.

halcyond -> kaua-term:
- `Key { KeyEvent }` -- halcyond routes post-chrome-chord input; the kaua-term
  xterm-encodes honoring DECCKM/keypad -> the pts master.
- `Resize { cols, rows }` -- the kaua-term sets the pts winsize (TIOCSWINSZ) + the
  hosted app gets SIGWINCH.
- `Text { bytes }` (H-4d-2) -- a chosen verb's command line, written to the master
  verbatim as ONE record (the compositor's `^E ^U <cmd>\n`): a bounded down-queue
  drops it whole, never half a command.
- H-4d-2b: `Osc1936Raw { serial, frame }` carries the span serial the frame
  advanced the VT to; every cell record's `Cell.span` refers to one of these
  (explicit on the wire, never counted at both ends).

WIRE CELL = the shared `usr/lib/vt::Cell` (self-contained `ch` + inline style,
+ `span` since H-4d-2b: the serial of the last Beacon frame the VT forwarded, 0 =
none -- the consumer maps it to the span state after that frame, so a cell knows
its obj / em / hdr without the producer ever parsing a Beacon body);
halcyond interns per-block internally (its `TCell`) on ingest. IPC = a
halcyond-owned Loom ring per tile (H-3c-2 EventRing reuse; main's side; the kernel
primitive firms at KT-1); the contract itself is transport-agnostic. TIER = RICH
per Halcyon tile (halcyond rasterizes with fontdue); the pts advertises `BEACON=`
at kaua-term spawn (the aux producer side; no dynamic per-tile tier switch at v1.0).
AS-BUILT (H-4d-2a, 2026-09-05): `kaua-term --beacon <none|cells|rich>` writes the
tier into its own `/env/BEACON` before `spawn_on_slave`, so the hosted app inherits
it; the pts SLAVE answers `'t'` to `SYS_FD_DEVCLASS` (the kernel's pts registry,
never a qid bit) and the Beacon gate admits `'t'` beside the console's `'c'`; `ut`'s
pts branch arms its transcript zones from the inheritance iff rich AND its stdout
is that terminal. Absent = none, fail-closed. Until this every tile was plain.

Main records the same contract in HALCYON 14.3 (its scripture half).

---

## 2. Architecture -- the pipeline

```
  tile:  app (ut OR Linux binary)
AS-BUILT (H-4d-2a, 2026-09-05): `kaua-term --beacon <none|cells|rich>` writes the
tier into its own `/env/BEACON` before `spawn_on_slave`, so the hosted app inherits
it; the pts SLAVE answers `'t'` to `SYS_FD_DEVCLASS` (the kernel's pts registry,
never a qid bit) and the Beacon gate admits `'t'` beside the console's `'c'`; `ut`'s
pts branch arms its transcript zones from the inheritance iff rich AND its stdout
is that terminal. Absent = none, fail-closed. Until this every tile was plain.
           |  fd 0/1/2 = the pts SLAVE
           v
         pts slave  --(ptyfs userspace line discipline: cook/echo/isig)-->  pts master
                                                                               ^  |
                                                          held by the tile's kaua-term
           kaua-term (a PARSER+producer PROCESS; does NOT rasterize):          |  v
             master bytes --> FULL-xterm VT PARSER (usr/lib/vt) --> a cell grid
             --> ONE ordered record stream: CellDiff/ScrollOff/Control/Mode --> halcyond
             halcyond Key/Resize records --> xterm re-encode (honors DECCKM/keypad) --> master
           halcyond (the renderer + orchestrator):
             ingests the ordered cell stream --> rasterizes (fontdue) + builds the
             transcript (Helix-modal, inline media) --> composites the tile --> framebuffer
             routes input: raw kbd --> KeyEvent (chrome chords filtered) --> the focused kaua-term
```

The pipeline reflects the RATIFIED design (uniform-Y topology, section 1a; the
render-responsibility SEAM = B, section 1b): the kaua-term is a per-tile
PARSER+producer PROCESS that feeds an ordered cell-record stream to halcyond;
halcyond rasterizes + composites. The mandatory crash-isolation holds -- the
hostile-input PARSER is isolated in the kaua-term process; halcyond's renderer
sees only TRUSTED cells (a bounded grid + a font). D7 holds: tapestryd (the
compositor) stays pixels-only; halcyond is a tapestryd CLIENT that rasterizes its
own panes (HALCYON 13/14).

**Two roles, one shared PARSER crate (R2, refined by seam=B).**

| axis        | tile role (a kaua-term)              | console role (aurora, the trusted renderer) |
|-------------|--------------------------------------|---------------------------------------------|
| input       | pts master                           | `/dev/consdrain` + `/dev/consfeed`          |
| output      | the ordered cell stream -> halcyond  | rasterize (Cornucopia) -> whole-screen `Surface` -> present |
| rasterize   | NO -- halcyond does (fontdue)        | YES (Cornucopia)                            |
| trust       | untrusted (like a rio window)        | the trusted console (SAK sink)              |
| shared      | the PARSER crate `usr/lib/vt`        | the PARSER crate `usr/lib/vt`               |

Under seam=B the SHARED thing is the full-xterm PARSER crate (`usr/lib/vt`),
consumed by BOTH the kaua-term (tile producer) and aurora (console renderer);
halcyond consumes its `Cell` TYPE on ingest. The kaua-term does NOT rasterize (it
produces cells); aurora stays the trusted-console rasterizer, unchanged. During a
framebuffer SAK episode aurora is suspended (the kernel is sole painter;
`TAPESTRY.md` 18.7) -- I-27 unchanged. On QEMU/virtio-gpu the trusted path stays
on serial and the renderer is not suspended (18.7).

---

## 3. Ground truth this builds on (as-built, aux-owned surfaces)

Grounded 2026-09-03 via read-only sweeps of the aux-2 worktree. **Tree-scoping
caveat:** aux-2 does not carry MAIN's H-arc (no `usr/halcyond*`/`usr/tapestryd*`,
no `beacon` token); halcyond/beacon citations below are from MAIN's tree, referenced
not asserted from aux-2. Neither track can ground or author the other's half from
its own worktree.

- **`kaua`** (`usr/lib/kaua`): a cell-diff (ratatui-model) TUI lib. `Buffer` of
  `Cell` (`buffer.rs`), a `Terminal` front/back diff (`term.rs`), a cells->bytes
  EMITTER (`encode.rs`, a CONSTRAINED subset: absolute CUP, truecolor-or-default
  SGR, no EL, no scroll regions, autowrap off), a bytes->KeyEvent input parser
  (`input.rs`: arrows/F-keys/modifiers/SS3 -- MORE complete than aurora's key
  emitter). kaua has NO bytes->cells parser (that is aurora's vt.rs).
- **`aurora`** (`usr/aurora`): a VT-parser -> cell-grid -> framebuffer console
  emulator, and ALREADY a tapestryd client (`main.rs`: `Surface::fullscreen`,
  `present`). **Parser location (tree-divergence):** in aux-2 the parser is
  `usr/aurora/src/vt.rs`; in MAIN's tree it already moved to a SHARED crate
  `usr/lib/vt` (H-2a) that halcyond + aurora both consume (absent in aux-2). So
  "grow the parser" (KT-2) is a change to a SHARED crate both tracks consume --
  aux authors the VT logic, but H-2a must reach aux-2 or the crate is coordinated.
  Its `vt.rs` (1170 lines) is the bytes->cells parser: CUU/CUD/CUF/CUB
  /CHA/VPA/CUP/ED/EL/IL/DL/ICH/DCH/ECH/SGR(256+truecolor)/alt-screen/DECSC-RC/OSC
  /scroll. GAPS (to grow): DECSTBM scroll regions (`vt.rs:528` accepted-ignored,
  no margin fields), SU/SD, origin mode (?6), wide-char advance (`vt.rs:703`
  `cx += 1`), SGR italic/dim/blink/strike, app-cursor-keys (?1). Rasterizes the
  baked Cornucopia bitmap atlas only (no runtime TTF; `render.rs`).
- **`ptyfs`** (`usr/ptyfs`): the native `/srv` 9P pts server. The per-pts LINE
  DISCIPLINE lives HERE, in USERSPACE (`server.rs`: cook ICANON/ECHO/ISIG/ICRNL
  /ONLCR, enforced on the ring; per-pts termios + winsize on the pts ctl). A fresh
  pts is full-cooked (the Linux posture). The KERNEL (`kernel/pts.c`) owns ONLY
  session/pgrp/controlling-tty (`ct_sid`, `fg_pgid`) + `SYS_TTY_*` (89/90/94-98).
- **`ptyhost`** (`usr/ptyhost`, PTY-4b): mints a pts (ptmx), seeds winsize, spawns
  a program (default `/bin/ut`) on the slave as fd 0/1/2, and pumps master<->console
  byte-for-byte (pass-through, no transcode). This IS the per-tile host, minus the
  transcode.
- **`ut`** (`usr/utopia`): substrate-agnostic -- reads inherited fd 0/1/2, does its
  own raw-mode line editing (libutopia `line_editor`), detects a pts via `fstat(0)`
  and runs the session dance (`t_setsid`/`t_tty_acquire`/`t_tty_set_fg`). Full
  native pts job control TODAY. On the console every pts step is skipped and it is
  byte-identical to before.

---

## 4. Design resolutions (R1-R5)

- **R4 -- the kaua-term EMBEDS ptyhost's master-hold** (does not spawn a separate
  ptyhost per tile). It reuses ptyhost's mint + spawn-on-slave + master-hold, with
  the pump's two ends re-pointed: `master -> fd1` becomes `master -> parse -> a
  cell grid -> the ordered record stream (1b) to halcyond`; `console-fd0 -> master`
  becomes `halcyond Key records -> xterm-encode -> master`. The parse must sit
  where the master bytes are (and where a hostile-input panic is isolated); a
  separate host +
  transcoder would be two processes + an extra pipe hop for no gain. `ptyhost`
  stays as-is for the non-tile console-hosted `ptyhost` command.
- **R3 -- C2-k1c scope:** section 5.
- **R1 -- winsize + beacon relocate per-tile TOGETHER.** WINSIZE: the per-pts
  winsize model already exists (ptyfs carries each pts's winsize on its ctl);
  the compositor is the geometry authority (it owns the tile rect), sets the tile
  pts's winsize, and a resize raises `TTY_SIG_WINCH` -> SIGWINCH to the fg pgrp.
  BEACON (the TIER), refined by seam=B: the RENDER side is halcyond's (it
  rasterizes with fontdue -> RICH for every Halcyon tile; the kaua-term does not
  rasterize). The ADVERTISE side is the AUX producer's: the tile's pts advertises
  `BEACON=` at kaua-term SPAWN (a spawn param, no dynamic per-tile switch at v1.0),
  read by the hosted program to decide what markup to emit; it must MATCH halcyond's
  render tier. Retiring the single-renderer `/dev/winsize` + `CCONSWINSZONLY`
  console special-case for tiles moves winsize AND the beacon advertisement onto
  the per-tile pts ctl together. (The console special-case stays for the non-tile
  console/serial fallback.) **AS-BUILT (H-4d-2a):** the advertise side rides the
  spawn (`--beacon` -> the hosted program's inherited `/env/BEACON`) plus the
  kernel's `'t'` class, not a pts ctl verb -- per-tile as required, no dynamic
  switch at v1.0; winsize was already per-pts (KT-1). The console special-case
  remains.
- **R2 -- SHARED PARSER crate** (refined by seam=B): the full-xterm PARSER
  (`usr/lib/vt`) is the one shared codebase, consumed by the kaua-term (tile
  producer) AND aurora (console renderer); halcyond consumes its `Cell` type on
  ingest. Under B the kaua-term does NOT rasterize (it produces the ordered cell
  stream; halcyond rasterizes with fontdue); aurora stays the trusted-console
  rasterizer, unchanged. Growing the shared parser to full-xterm is the one real
  net-new parser piece; a "beside" model (two parsers) is the duplication the
  convergence deleted.
- **R5 -- the terminal exposes ONE narrow seam** (MAIN owns the transcript). The
  kaua-term renders the live app's SCREEN (main or alt) as one surface + reports
  cell geometry; scrollback + inline `Image`/`Embed` + the media pipeline are
  halcyond's. `cat picture.png` is NOT "cat through the pts" (PNG bytes hit the VT
  parser as garbage); v1.0 inline media is a NATIVE out-of-band seam
  (a `display`/type-aware coreutil or the shell emits `Embed`/`Image` to halcyond
  directly). A terminal-escape inline image (sixel/kitty) is v1.x, tracked.

---

## 5. C2-k1c + C2-k3 (the kernel/viv ioctl reach)

The pts cooking already runs (ptyfs, userspace) for any reader -- native ut, Pouch,
or an unmodified Linux (vivarium-phenotype) binary. What is deferred is a
PHENOTYPE program's ability to QUERY/SET its pts terminal via kernel ioctl. Today
`vivarium_ioctl_decide` (`kernel/vivarium.c`) recognizes only TCGETS/TCSETS(W/F)/
TIOCGWINSZ/TIOCSWINSZ and serves them on the CONSOLE only (gated by the unforgeable
`spoor_is_console`); on a pts fd every ioctl returns ENOTTY.

- **C2-k1c (the terminal substrate enabler):** route, on a pts fd, {TCGETS,
  TCSETS/W/F, TIOCGWINSZ, TIOCSWINSZ} from `viv_ioctl` to the ptyfs line discipline
  (the kernel walks `/dev/pts/<N>ctl` + does 9P I/O -- "the reach") instead of
  ENOTTY. This makes `vim`/`less`/interactive `sh` work in a tile. Native ut needs
  NONE of this (it self-edits + uses the native `t_tty_*` syscalls directly), so
  C2-k1c is a Linux-phenotype enabler exclusively.
- **C2-k3 (the follow-on):** the Linux job-control ioctls
  {TIOCSPGRP/TIOCGPGRP/TIOCSCTTY} -> the existing native `pts_tty_*` cores, for a
  phenotype `bash`'s fg/bg/^Z. NOT required for the substrate MVP. (SIGTTIN/SIGTTOU
  foreground-read arbitration is absent at every layer -- a separate v1.0 gap.)

**Audit posture (both are audit-trigger surfaces -- the vivarium phenotype branch,
I-43, plus the pts seam).** A phenotype ioctl must not reach a pts the fd does not
name (bound by the fd's own Spoor identity). A phenotype `TIOCSWINSZ` mirrors the
console's renderer-owned posture (the geometry authority is the compositor, not the
app). A phenotype `TIOCSPGRP` (C2-k3) must not target a pgrp outside the pts's
session -- mirror the native `tcsetpgrp` gate. Neither confers any authority beyond
what the pts fd already names (I-43: a phenotype is ABI shape, never authority).

---

## 6. The build arc

**The DESIGN is fully RATIFIED** -- topology uniform-Y (1a) + the seam = B (1b) +
the record contract. No design gates remain; the build is the next phase (a fresh
main<->aux build call for KT-1). KT-2/KT-3/KT-4 are aux-side (the parser, the
ioctl reach).

- **KT-1 -- native-ut tile (a kaua-term process) -> unblocks H-4d.**
  The welcome's two console tiles are native `ut`, which already has full pts job
  control; ZERO kernel work. A kaua-term process hosts ut on a pts + feeds the
  ordered cell stream (section 1b contract) to halcyond; halcyond spawns the
  kaua-term per tile, ingests via the Loom ring, rasterizes + composites (MAIN's
  half). A fresh build coordination call covers the three pieces: the H-2a sync,
  the ring seam, and the aux producer side (below).
- **KT-2 -- grow the shared VT parser to full-xterm** (main's `usr/lib/vt` crate,
  H-2a; still `aurora/vt.rs` in aux-2 -- coordinate the crate): DECSTBM + top/bottom
  margin fields, SU/SD, origin mode, wide-char advance via unicode-width, SGR
  residue, ?1 app-cursor-keys) + the `KeyEvent -> xterm` re-encoder (net-new;
  honors DECCKM/keypad). This is what makes `vim` render correctly (it corrupts
  today). Topology-independent (the parser is shared either way).
- **KT-3 -- C2-k1c** (the termios+winsize ioctl reach) -> Linux binaries
  (`vim`/`less`/interactive `sh`) work in a tile. Audit-bearing.
- **KT-4 -- C2-k3** (the job-control ioctl reach) -> a phenotype `bash`'s fg/bg/^Z.

Each sub-chunk lands independently with tests + a status row. KT-3/KT-4 spawn a
focused audit (the vivarium phenotype + pts surface).

---

## 7. Invariants + audit-trigger surface

- **I-43 (a phenotype confers ABI SHAPE, never AUTHORITY):** C2-k1c/C2-k3 add pts
  ioctls to the phenotype branch; they must grant zero authority beyond the fd's
  pts. Prosecute at KT-3/KT-4.
- **I-27 (trusted path):** unchanged. Tiles are uniformly untrusted ptys (like rio
  windows); the trusted console is the kernel `/dev/cons` (serial SAK on QEMU;
  framebuffer SAK with the console-mode kaua-term suspended on simplefb boards).
- **I-20 (PTY master<->slave atomicity):** the kaua-term is a new pts-master
  consumer; it inherits ptyfs's existing I-20 guarantees (it holds the master and
  pumps, exactly as ptyhost does).
- **Audit-trigger rows to add at build:** the C2-k1c/C2-k3 vivarium ioctl reach
  (append to `docs/AUDIT-TRIGGERS.md` + the `CLAUDE.md` index in the same PR that
  builds it); the kaua-term itself is userspace (a tapestryd client) but the grown
  vt.rs is a parser on untrusted input -- prosecute for OOB/overflow on CSI params
  + OSC + UTF-8 assembly.

---

## 8. Naming (thematic; held per the CLAUDE.md discipline)

"kaua-term" is the working + ratified identifier. A thematic name for the terminal
emulator (the thing that hosts + renders a program in a tile) is a HELD PROPOSAL,
not a unilateral rename -- surfaced to the operator before any load-bearing rename.
Candidates in the marsupial/Plan-9 register are welcome; the bar is added clarity
or color without obscuring intent. `ptyhost`'s own held candidate is `den`
(PTY-DESIGN section 10).
