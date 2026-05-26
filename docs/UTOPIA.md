# UTOPIA — the textual milestone

The Utopia milestone (per `VISION.md §13`) is the moment Thylacine "feels real, not broken." A developer connecting via UART or SSH lands at a Pale Fire prompt; pipelines compose; namespaces compose; the editor works; common tasks succeed without grinding the user's gears. The system is not yet graphical — that is Phase 8 (Halcyon) — but it is genuinely usable.

This document is the umbrella scripture for the Utopia surface. It points at the binding designs and the implementation arc.

**STATUS**: COMMITTED — scripture commit, U-1 chunk. Realised by execution Phase 7 (per `ROADMAP.md §2.1`; corresponds to `ROADMAP.md §8`).

---

## 1. What Utopia is

Three things, treated as one whole:

1. **The shell** — `ut`. A Plan 9-rc-shaped, native-Rust, capability-aware shell with refined error handling and a poll-driven main loop. The user's daily interface. Binding design: `docs/UTOPIA-SHELL-DESIGN.md`.
2. **The coreutils** — `cat`, `ls`, `echo`, `grep`, `sed`, `awk`, and the rest. Native Rust. Built on `libthyla-rs` + `libutopia`. Plan 9-9base-shaped feature scope; uutils-coreutils-shaped Linux flag compatibility where it helps without bloating. Sub-chunked under the U-* arc.
3. **The editor** — `hx`. Helix, ported via Pouch. The default `$EDITOR`. Tree-sitter syntax, multi-cursor, modal Kakoune-style. Lands in U-Helix.

And the visual identity that ties them together: **Pale Fire** (`docs/UTOPIA-VISUAL.md`). Four colours; one glyph; one typeface recommendation. Coloured output is disciplined to the palette in Utopia's own programs; user programs colour freely.

---

## 2. The experience

A developer connects to a running Thylacine instance via UART or SSH. The greeting:

```
Welcome to Thylacine.
~/home/user ⊢ 
```

The prompt is path (`#8898b4`) + glyph `⊢` (`#e07840`) + space + the cursor. The user types `ls`:

```
~/home/user ⊢ ls
Documents/  src/  .config/  utopia.rc
~/home/user ⊢ 
```

They run a pipeline:

```
~/home/user ⊢ cat src/main.rs | grep fn | wc -l
   42
```

They bind a Stratum admin tree into their namespace:

```
~/home/user ⊢ bind /srv/stratum-ctl /n/stratum
~/home/user ⊢ ls /n/stratum/pools
pool0/  pool1/
```

They start a long-running task:

```
~/home/user ⊢ cargo build &
[1] 4231
~/home/user ⊢ 
```

They edit a file:

```
~/home/user ⊢ hx ~/.config/utopia/utopia.rc
[Helix opens in the terminal; user edits; saves; quits]
~/home/user ⊢ 
```

The background job finishes; they see it at the next prompt:

```
~/home/user ⊢ 
[1]+ Done    cargo build
~/home/user ⊢ 
```

They Ctrl-C an interactive program; the shell reads the note and forwards it to the foreground job; the job exits cleanly; the prompt returns. They Ctrl-D when they're done; the shell exits.

Nothing here is novel from a user's perspective — it is the modern Unix experience, executed with intention. What's novel is what runs underneath: Plan 9 namespaces as a shell builtin; the notes substrate as the signal mechanism; capability handles as the privilege model; native Rust on Thylacine-shaped syscalls instead of POSIX emulation. The user does not see the novelty until they look for it.

---

## 3. The Plan 9 split

A scripture decision under U-1 (binding):

> Every Thylacine userspace program is either **native** (built against `libthyla-rs`, speaking Thylacine syscalls directly) or **ported** (built via Pouch, speaking musl + the boundary-line patches that translate to Thylacine).

Native: ut, libutopia, coreutils, corvus, the userspace drivers, future Thylacine-shaped daemons.

Ported: Helix, stratumd, libsodium, future POSIX-ported tools (ssh, git, python).

The boundary determines the runtime substrate. The full scripture is in `docs/UTOPIA-SHELL-DESIGN.md §3` and `docs/ARCHITECTURE.md §3.5` (extended in U-1).

---

## 4. Phase mapping

The Utopia milestone is achieved by execution Phase 7. The numbering is messy across the canonical docs because Pouch (Phase 6) was inserted between the original Phase 5 and Phase 6 of `ROADMAP.md`. The project-execution numbering is:

- P1 — Phase 1: kernel skeleton. CLOSED.
- P2 — Phase 2: SMP + memory + scheduler. CLOSED.
- P3 — Phase 3: device model + userspace drivers. CLOSED.
- P4 — Phase 4: handles + Territory + corvus foundations. CLOSED.
- P5 — Phase 5: 9P client + Stratum integration + corvus completion. CLOSED.
- P6 — Phase 6: Pouch (cross-compilation + musl + stratumd boot). CLOSED.
- **P7 — Phase 7: Utopia (this milestone). U-1..U-Z arc.**
- P8 — Phase 8 (in ROADMAP §9): Linux compat + network.
- P9 — Phase 9 (in ROADMAP §10): Hardening + audit + 8-CPU stress + v1.0-rc.
- P10 — Phase 10 (in ROADMAP §11): Halcyon + v1.0 final.

The execution-phase registry per `ROADMAP.md §2.1` is authoritative; the section-numbering quirks in ROADMAP itself are preserved for historical readability.

---

## 5. The U-* arc

Per `docs/UTOPIA-SHELL-DESIGN.md §19`:

- **U-1** (this commit) — scripture: UTOPIA.md + UTOPIA-SHELL-DESIGN.md + UTOPIA-VISUAL.md + ARCH/ROADMAP/CLAUDE updates + phase7-status.md.
- **U-2** — Scripture amendment: the libthyla-rs uplift framing (§15 of UTOPIA-SHELL-DESIGN.md). NO code.
- **U-2a..U-2-test** — The libthyla-rs uplift: error types, handle RAII, allocator, fs + io, process + pipe, notes + poll, territory + cap, thread/torpor/time/rand/tty, 9P + hardware, cross-module smoke test. ~9-12 sessions of investment in the library Thylacine deserves.
- **U-3** — Utopia workspace skeleton: ut skeleton + libutopia stubs + build wiring.
- **U-4** — line editor in libutopia.
- **U-5** — parser + AST.
- **U-6** — evaluator core + main loop.
- **U-7** — fd-notes job control.
- **U-8** — Thylacine builtins (bind/mount/cap/note).
- **U-9..N** — coreutils (cat, ls, echo, grep, sed, awk, cp, mv, rm, mkdir, find, wc, etc.).
- **U-Helix** — Helix port via Pouch (parallel arc).
- **U-Z** — the integration test.

Tracking: `docs/phase7-status.md`.

---

## 6. The exit gate

Per `docs/UTOPIA-SHELL-DESIGN.md §18`. The 12-point integration test. When it passes, the Utopia milestone ships and Phase 7 exits.

The high-level check: a developer at a Pale Fire prompt can compose a pipeline, edit a file in Helix, bind a namespace, background a job, Ctrl-C cleanly, exit cleanly. No kernel extinctions. No driver crashes. No zombies.

---

## 7. What Utopia is NOT

- **Not graphical.** Halcyon (Phase 10 in execution-phase numbering; ROADMAP §11) is the graphical layer. Utopia is the textual layer that runs underneath Halcyon and stands on its own.
- **Not network-dependent.** The network stack lands at Phase 8 (ROADMAP §9). Utopia is reached and operated via UART or via SSH-over-an-already-up-network; the milestone itself does not require either if the user is happy at a serial console.
- **Not Linux-binary-compatible.** Linux compat lands at Phase 8 (ROADMAP §9). Utopia is rich enough to be productive without it; the compat layer is additive.
- **Not POSIX-compliant.** The shell is rc-shaped with refinements, not POSIX-sh. Scripts written for POSIX `sh` do not run unchanged. Linux-compat compat will let some POSIX scripts run via bash-via-Pouch in the §9 phase; Utopia v1 doesn't ship bash. Users who need POSIX bash before Phase 8 install it themselves via the Pouch path.
- **Not a 100% feature-parity replacement for any existing shell.** Job control, history, tab completion, prompt customization are all present but minimal. v1.x will add polish; v1 ships when "feels real, not broken."

---

## 8. References

- `docs/UTOPIA-SHELL-DESIGN.md` — the shell design scripture (12 axes + impl roadmap).
- `docs/UTOPIA-VISUAL.md` — Pale Fire palette + glyph + prompt format.
- `docs/ARCHITECTURE.md §3` — language/toolchain split (extended for the native vs ported scripture).
- `docs/ARCHITECTURE.md §23` — POSIX surfaces and the Utopia milestone (updated in U-1).
- `docs/ROADMAP.md §8` — the execution-phase definition (rewritten in U-1).
- `docs/POUCH-DESIGN.md` — the ported-code substrate.
- `docs/phase7-status.md` — per-chunk progress tracking.
- `docs/VISION.md §13` — the Utopia milestone framing.
- `CLAUDE.md` — operational notes including the native-vs-ported decision rule.
