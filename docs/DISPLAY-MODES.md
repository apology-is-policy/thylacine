# DISPLAY-MODES.md — the per-deployment display authority

**Binding scripture (aux, 2026-08-25; operator-ratified).** This document
defines how Thylacine decides *which display is primary* per deployment, and the
consequences that flow from that choice: which console owns the terminal
geometry, which console is silenced, and where a program reads its terminal
width. It is the design that resolves the ut line-wrap/winsize question
(`docs/reference/92-utopia-line-editor.md`; the render mechanism landed dormant
at `0a7e4c18`) and it governs any future console/display/Halcyon work.

Cross-refs: `ARCHITECTURE.md §23.5.3` (the #55 console winsize contract),
`TRUSTED-PATH.md §7` (the medium-aware trusted sink — "the medium is a DTB boot
fact"), `AURORA.md` (the framebuffer console renderer),
`memory/project_deployment_modes.md`.

---

## 1. Thesis: one primary display per deployment

Thylacine's `/dev/cons` can drive two output media at once — the serial UART and
aurora's framebuffer (aurora is a *renderer* for the same kernel console, not a
separate console). Historically this created an ambiguity: a single global
`/dev/winsize` held aurora's framebuffer grid (128x36) even under headless
`-nographic`, so a serial ut read the *framebuffer's* virtual size, not the
serial terminal's — and both aurora and the serial terminal answer a CPR
(`ESC[6n`) width query, so "ask the terminal" raced.

The resolving insight (operator, 2026-08-25): **the serial console and the
graphical display are not two simultaneous views fighting over geometry — they
are mutually-exclusive PRIMARY displays selected at launch.** Each deployment
picks exactly one primary; the others are silent or emergency-only. With one
primary, there is exactly one terminal that answers a CPR, so a program can just
ASK and get the right width every time.

## 2. The deployment modes

| # | Mode | Primary display | Others |
|---|---|---|---|
| 1 | **Pure virtual console** — ships with QEMU; `thylacine` boots HVF straight into the host PTY | host PTY (serial) | no GPU stack |
| 1a | **Virtual + virtual GPU** — opt into the virtual GPU for graphical apps + Halcyon | aurora framebuffer | serial SILENT (1b) |
| 2 | **Bare-metal desktop (Pi)** — fully graphical Aurora/Halcyon | aurora / Halcyon | UART = emergency IO; SSH |
| 3 | **Headless server (Pi, no display)** — SSH up | the SSH pts | local console silent |

Full statements: `memory/project_deployment_modes.md`.

## 3. The mechanism

Four moving parts, each riding an existing seam (file:line anchors from the
boot/console/display architecture map, aux 2026-08-25):

### 3.1 The display-mode signal — a boot arg

The kernel has **no command-line parser**; boot flags travel as DTB
`/chosen/bootargs` and are read by userspace through the `/hw` FDT mount via
`bootarg_has(needle, nlen)` (`usr/joey/joey.c:3625`; the Rust twin
`usr/debug-probe/src/main.rs:127`). `run-vm.sh` composes `-append` tokens
(`tools/run-vm.sh:481-494`; existing tokens `thylacine.nowatchpoint`,
`thylacine.nostorm`). The new token is **`thylacine.display=<console|gpu>`**,
read the same way. Absence of the token means the **default** (testing /
back-compat) posture.

### 3.2 Console mode (1) — drop the GPU, serial is primary

Console mode is realized by **not attaching the virtio-gpu device** — the
existing `THYLACINE_NO_GPU` path. With no GPU function, the warden finds no
`virtio-pci:16` (`usr/warden/src/main.rs:108-120`) → no tapestryd → no
`/srv/tapestry` → **joey already skips aurora** (`usr/joey/joey.c:10367-10437`,
the `if (tap_root >= 0)` gate), and aurora would `exit(1)` anyway at
`Surface::fullscreen()` with no display. So no code change is needed to make the
console lean — only the launcher wiring:

- `run-vm.sh`: `THYLACINE_DISPLAY=console` drops the GPU device **and** appends
  `thylacine.display=console`.
- The shipping `thylacine` launcher (mode 1) sets console mode explicitly.

Result: `/dev/winsize` stays `winsize 0 0`, the serial UART is the sole console,
and ut's CPR (§3.4) is answered by the host terminal alone.

### 3.3 GPU mode (1a/2) — aurora primary, serial silenced (1b)

GPU present + `thylacine.display=gpu` → tapestryd + aurora run (the existing
path), aurora owns `/dev/winsize` + answers CPR, and **1b silences the EL0
serial output** so the framebuffer is the sole view and the sole CPR answerer.

**1b uses the seam the kernel already names** (`kernel/cons.c:207-210`): "the
selector will gate `uart_putc`, not the tap." The write path
`cons_emit_bulk`/`cons_emit_bulk_wait` (`kernel/cons.c:853-897`) taps aurora's
`cons_drain` mirror FIRST, then pushes to the UART TX ring second — the two
sinks are cleanly separable there. 1b gates the **UART-sink half**
(`cons_tx_push_bulk` / `uart_putc`) on a `serial_silent` flag in the cons state,
leaving `cons_drain_tap_bulk` untouched so aurora keeps rendering. Emergency IO
(mode 2's UART) is the un-silenced kernel path (extinction dumps, the trusted
sink) — 1b silences **EL0 program output only**, never the kernel's own
diagnostics.

The flag is set by **aurora**: on startup, when aurora reads
`thylacine.display=gpu` from `/hw/chosen/bootargs`, it issues a new consctl verb
(aurora already holds `/dev/consctl`, `usr/aurora/src/main.rs:367`) that sets
`serial_silent`. Under the **default** posture (no `thylacine.display=gpu`
token) aurora does NOT silence — so the test harness, which runs GPU + aurora
under `-nographic` but drives the serial for the non-gfx scenarios, keeps its
serial output.

### 3.4 ut width — the §23.5.3 client rule, made pts-aware

ARCH §23.5.3 already states the deterministic client rule: **"read
`/dev/winsize`; if `0 0`, CPR."** The deployment-mode model is exactly what
makes that rule correct, because it makes `/dev/winsize` reflect the *primary*:

- **On the direct console.** Console mode has no aurora, so `/dev/winsize` is
  `0 0` → ut falls to the CPR probe (`ESC[s ESC[9999;9999H ESC[6n ESC[u`), which
  the host PTY (the sole answerer, no aurora competing) answers. GPU mode has
  aurora, so `/dev/winsize` is the framebuffer grid → ut uses it directly, and
  ut IS on that framebuffer, so the grid is its real width — **no CPR emitted**,
  so no probe cost and no two-answerer race.
- **On a pts (SSH/tmux, mode 3).** `/dev/winsize` is the *console* leaf, not the
  pts's geometry — so a pts ut instead reads its **own** pts winsize from the
  `/dev/pts/<n>ctl` it already opens (`repl.rs:184`; ptyfs renders
  `... winsize <cols> <rows>` in the ctl). No CPR round-trip; tmux/ssh set the
  pts winsize via TIOCSWINSZ→ptyfs.

The reply consumption is already built: the line editor's CSI parser recognizes
`ESC[<rows>;<cols>R` → `set_cols` (landed dormant at `0a7e4c18`). A terminal that
never answers (a dumb pipe) leaves cols `None` → the byte-identical no-wrap
fallback. CPR-*always* was considered and rejected: it emits a needless probe in
GPU mode where `/dev/winsize` is authoritative, and it revives the two-answerer
race in the testing-hybrid posture (aurora's in-guest reply vs the serial
round-trip); reading `/dev/winsize` first sidesteps both.

## 4. Per-mode summary

| Mode | GPU dev | aurora | `/dev/winsize` | serial EL0 out | ut width from |
|---|---|---|---|---|---|
| 1 console | absent | no | `0 0` | ON | CPR → host PTY |
| 1a/2 gpu | present | yes | aurora grid | **SILENT (1b)** | `/dev/winsize` (aurora grid) |
| 3 SSH | (either) | (either) | n/a (pts) | n/a | the `/dev/pts/<n>ctl` winsize |
| default (test) | present | yes | aurora grid | ON | `/dev/winsize` (aurora grid; no CPR, no race) |

## 5. Implementation plan + anchors

1. **`run-vm.sh`**: `THYLACINE_DISPLAY=console` → drop the GPU device (reuse the
   `THYLACINE_NO_GPU` device-omission) + append `thylacine.display=console`;
   `THYLACINE_DISPLAY=gpu`/`cocoa`/`vnc` → append `thylacine.display=gpu`.
   (`tools/run-vm.sh:278-323` GPU device block, `:481-494` append block.)
2. **ut width source** (`usr/utopia/libutopia/src/repl.rs` +
   `usr/utopia/shell/src/main.rs`): the §23.5.3 client rule, pts-aware — on a
   pts read the `/dev/pts/<n>ctl` winsize (ut already holds that fd); else read
   `/dev/winsize` and, iff `0 0`, emit the CPR probe. Session-only (the
   bare-spawn boot check does not probe). The render mechanism already consumes
   the reply.
3. **Kernel 1b** (`kernel/cons.c`): a `serial_silent` flag on the cons state; a
   consctl verb to set it; the UART-sink gate in `cons_emit_bulk`/
   `cons_emit_bulk_wait` (leave the drain tap). Audit-trigger surface — cons is
   on the table; run the focused round.
4. **aurora** (`usr/aurora/src/main.rs`): read `/hw/chosen/bootargs` for
   `thylacine.display=gpu`; if present, issue the consctl silence verb after the
   surface is up.

## 6. Verification

- **Console-mode boot** (`THYLACINE_DISPLAY=console`): assert no aurora
  (`joey: /srv/tapestry absent ... skipping`), `/dev/winsize` == `winsize 0 0`,
  and a clean interactive serial session. (The expect harness's pty does not
  answer CPR, so ut's width stays `None` → the no-wrap fallback there; the CPR
  round-trip itself is proven in-guest by u-repl-test + on a real terminal.)
- **GPU-mode boot** (`THYLACINE_DISPLAY=gpu`): assert 1b — EL0 program output
  does NOT reach the serial (the harness sees the boot banner + kernel
  diagnostics but not an EL0 `echo`), while aurora renders it (screendump), and
  `/dev/winsize` == the aurora grid.
- **Default boot** (no flag): unchanged — test.sh G-4 gate + SMP gate + the 9
  `ls-gfx*` scenarios + the serial-driven ls-ci all pass as today.

## 7. Owed / v1.x

- winch-driven LIVE reflow (a resize mid-session) — the note is currently
  discarded (`stmt.rs` `dispatch_note`, "the v1.0 editor has no resize
  consumer"); a `tty:winch` re-probe is v1.x.
- The `thylacine` production launcher (the QEMU-bundling `thylacine` command of
  mode 1) is a packaging deliverable, not this chunk.
- The framebuffer SAK / trusted-sink enforcement (TRUSTED-PATH.md §7 "reserved
  then enforced") composes with 1b — both gate the same UART sink — but is its
  own arc.
