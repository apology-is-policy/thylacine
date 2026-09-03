# KAUA-TERM.md -- the per-tile terminal emulator (the Halcyon terminal substrate)

The `kaua-term` is the shared component that hosts a program (native `ut` OR an
unmodified Linux binary) on a pseudoterminal, parses its full-xterm output into a
`kaua` cell `Buffer`, rasterizes that into a `tapestryd` surface, and re-encodes
input back to the pts. It is the per-tile terminal emulator for Halcyon's
"the terminal is the desktop" model, and it SUBSUMES `aurora` (today's single
console renderer) as one instantiation mode.

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
- **AUX (this doc):** the per-tile kaua-term -- full-xterm parse + pts master host
  + kaua `Buffer` render + input re-encode.
- **KERNEL / viv (aux):** C2-k1c -- the pts termios+winsize ioctl reach for a
  Linux program (section 5). C2-k3 (Linux job-control ioctls) is a follow-on.

---

## 2. Architecture -- the pipeline

```
  tile:  app (ut OR Linux binary)
           |  fd 0/1/2 = the pts SLAVE
           v
         pts slave  --(ptyfs userspace line discipline: cook/echo/isig)-->  pts master
                                                                               ^  |
                                                          held by the tile's kaua-term
           kaua-term:                                                          |  v
             master bytes --> FULL-xterm VT PARSER --> kaua cell Buffer
             kaua Buffer   --> rasterize (Cornucopia | fontdue) --> tapestryd weave surface --> present
             compositor KeyEvent --> xterm re-encode (honors DECCKM/keypad) --> master
           halcyond:
             composites the kaua-term's surface into the tile rect --> framebuffer
             routes input: raw kbd --> KeyEvent (chrome chords filtered) --> the focused tile's kaua-term
```

The kaua-term is a `tapestryd` CLIENT (it owns a surface, exactly as aurora does
today -- `Surface::fullscreen` / `present`). D7 stays pristine: the kaua-term owns
glyphs (rasterization); the compositor owns placement (pixels + input routing),
never text.

**Two instantiation modes (R2: one codebase).**

| axis        | tile mode                     | console mode (subsumes aurora)          |
|-------------|-------------------------------|-----------------------------------------|
| input       | pts master                    | `/dev/consdrain` + `/dev/consfeed`      |
| output      | composited weave (a tile rect)| whole-screen `Surface`                  |
| trust       | untrusted (like a rio window) | the trusted console (SAK sink)          |
| tier        | compositor-set flag           | the console beacon posture              |
| parser      | the ONE grown vt.rs (shared)  | the ONE grown vt.rs (shared)            |

Console mode IS today's aurora, generalized. During a framebuffer SAK episode the
console-mode kaua-term is suspended exactly as aurora is today (the kernel is sole
painter; `TAPESTRY.md` 18.7) -- I-27 unchanged. On QEMU/virtio-gpu the trusted
path stays on serial and the renderer is not suspended (18.7).

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
  `present`). Its `vt.rs` (1170 lines) is the bytes->cells parser: CUU/CUD/CUF/CUB
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
  the pump's two ends re-pointed: `master -> fd1` becomes `master -> parse ->
  Buffer -> present`; `console-fd0 -> master` becomes `KeyEvent -> xterm-encode ->
  master`. The transcode must sit where the master bytes are; a separate host +
  transcoder would be two processes + an extra pipe hop for no gain. `ptyhost`
  stays as-is for the non-tile console-hosted `ptyhost` command.
- **R3 -- C2-k1c scope:** section 5.
- **R1 -- winsize + beacon relocate per-tile TOGETHER.** WINSIZE: the per-pts
  winsize model already exists (ptyfs carries each pts's winsize on its ctl);
  the compositor is the geometry authority (it owns the tile rect), sets the tile
  pts's winsize, and a resize raises `TTY_SIG_WINCH` -> SIGWINCH to the fg pgrp.
  BEACON (the render/advertise TIER): the RENDER side (AUX) is a compositor-set
  rasterizer flag the kaua-term honors (Cornucopia MVP / fontdue later); the
  ADVERTISE side (MAIN, `BEACON` on the tile's pts, read by the program to decide
  what to emit) must MATCH -- a CELLS tile whose program reads a stale
  `BEACON=rich` would emit TTF-assuming output the tile cannot honor. Retiring
  the single-renderer `/dev/winsize` + `CCONSWINSZONLY` console special-case for
  tiles moves winsize AND beacon onto the per-tile pts ctl together. (The console
  special-case stays for the non-tile console/serial fallback.)
- **R2 -- SUBSUME aurora** into the kaua-term (one VT codebase; the two modes in
  section 2). Growing aurora's `vt.rs` to full-xterm is the one real net-new parser
  piece; a "beside" model would keep kaua's weaker emitter-subset AND aurora's
  subset-parser as two VT codebases -- the duplication the convergence deleted.
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

- **KT-1 -- kaua-term native-ut mode + the halcyond per-tile seam -> unblocks H-4d.**
  The welcome's two console tiles are native `ut`, which already has full pts job
  control; ZERO kernel work. Grow the parser enough for `ut`'s own output (it emits
  the kaua subset), host ut on a pts, render into a composited surface. (halcyond's
  per-tile spawn/composite is MAIN's half; the seam is a build-time coordination
  call.)
- **KT-2 -- grow `aurora/vt.rs` to full-xterm** (DECSTBM + top/bottom margin
  fields, SU/SD, origin mode, wide-char advance via unicode-width, SGR residue,
  ?1 app-cursor-keys) + the `KeyEvent -> xterm` re-encoder (net-new; honors DECCKM
  /keypad). This is what makes `vim` render correctly (it corrupts today).
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
