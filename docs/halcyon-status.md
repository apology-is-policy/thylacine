# Halcyon phase status (ROADMAP §11; the H-arc)

The authoritative pickup guide for the Halcyon phase (ROADMAP §11.1's H-0..H-7
table is the plan; this doc tracks what has LANDED). Named `halcyon-status.md`
rather than `phase8-status.md` because that filename is already the network-era
tracker (the ROADMAP §9 phase) — the H-arc is arc-named the way its chunks are.

## TL;DR

**H-0 landed** (kickoff + same-day concretization: `HALCYON.md` + `BEACON.md`
born implementation-grade; ARCH §17/§23.5.4 + ROADMAP §11 rewritten). **H-1 is
LANDED in four sub-chunks** — the kernel fd-class syscall + consctl tier verb,
the beacon crate (wire/sink + the cells relocation), the tier plumbing (aurora
advertisement → ut export → transcript zones), and the emitters
(ls/grep/ps/stat + the `--color=auto` unification). The H-1 close (focused
audit round + the first push since the kickoff) is in flight. Next: **H-2**,
the transcript MVP on the CPU floor (HALCYON.md §13).

## Landed chunks

| Commit | What | Tests |
|---|---|---|
| `9a4db65b` | **H-0a kickoff**: the design conversation → scripture. `docs/HALCYON.md` (the environment: 3 sources / 3 layers, vk rendering, paper-light theme, two pane classes, presentations + menus, layouts) + `docs/BEACON.md` (the markup: OSC 1936 wire, semantic v1 vocabulary, none\|cells\|rich tiers, the emission gate, the security clause) + reconciliation (ROADMAP §11 EVOLVED, VISION §3.3/§9/§14, NOVEL Angle #4, TAPESTRY §14/§17, COREUTILS-design pointer) | n/a (scripture) |
| `fecff135` | **H-0b concretization** (operator: "as much detail and rigor as you can"): BEACON.md §12 (the H-1 build card: wire grammar, op registry, tier mechanism, crate/relocation plan, ut hook sites, P1–P3 test plan) + HALCYON.md §13 (the BOUND architecture: native halcyond + display list + CPU-floor executor + pouch vk executor post-compose; VT-core extraction; transcript model; layout format) + ARCH §23.5.4 + ROADMAP §11.1 (the H-table) + the resequencing (H-2 = CPU floor; H-5 = compose; H-6 = vk executor; guest-lavapipe off the critical path) | n/a (scripture) |
| `7cd1ab94` | **H-1a**: `SYS_FD_DEVCLASS` = 80 (rights=0 fd introspection; the `/dev/cons` leaf normalizes to `'c'` via `devdev_fd_devclass` keyed on `DEV_KIND_CONS`) + the consctl `beacon none\|cells\|rich` verb (winsize discipline verbatim: staged parse, atomic reject, render append ` beacon <tier>`, resets at `cons_drain_close` + `cons_test_reset`; allowed on the renderer-minted CCONSWINSZONLY consctl) + libt/libthyla-rs wrappers + the joey `probe H1` five-arm E2E. Trap banked: the 67-byte render floor's reader set includes the kernel's OWN consctl staging buffer (`devdev.c` `tmp[96]`, was 64 — every consctl read EOF'd; caught by the suite) | 1434/1434 (+`cons.beacon_roundtrip`, +`devdev.fd_devclass`, exact-line renders updated) + boot OK + `joey: probe H1 … OK` |
| `5d638fec` | **H-1b**: the `usr/lib/beacon` crate — `wire` (OSC 1936 emit/parse/strip; caps FRAME_MAX 2048 / VALUE_MAX 1024 / ARGS_MAX 8 / depth 8; foreign-OSC passthrough; the P1 strip identity) + `sink` (Sink zones/em/obj/hdr/rule/mark + Table with padding-outside-frames) + `boxd`/`color`/`palette` relocated VERBATIM from the coreutils crate (git 100%-rename; the 15-test baseline reproduced). As-built deviations recorded in BEACON.md §12.5 | 27 host tests (`cargo test -p beacon`); coreutils lib baseline 4/4 preserved |
| `04186229` | **H-1c-1**: the tier plumbing — aurora writes `beacon cells` at consctl bring-up; ut reads the render line → exports `/env/BEACON` (children inherit via `env_clone_into`) → arms Repl zones iff `rich` AND stdout dc=='c'; the Repl transcript zones (prompt/output brackets + the exit mark; redraws stay in-zone); the u-repl-test beacon leg (rich vs plain discrimination pair; `strip(rich) == plain` asserted in-guest) | 1434/1434 + `u-repl-test: beacon zones OK` in-guest every boot |
| *(H-1c-2 pending — the emitters commit)* | **H-1c-2**: the four emitters — ls (rich short: `obj type=path` per name; rich `-l`: a beacon table, obj name cells; the `--color=auto` flip + `--beacon=WHEN`), grep (`obj path` prefix + `em strong` spans, byte-span wire emission), stat (`obj path` on the subject; the `table` op deferred — recorded), **ps built new** (one atomic `/ctl/procs` read; passthrough/boxed/rich table + `obj pid`); the unification sweep (18 stubs → the real probe; 17 defaults → Auto per COREUTILS-design's ordained end-state); `coreutils::path` + `coreutils::beacon_gate`; the coreutil-smoke Beacon E2E legs (the pipe-budget rule: file-operand subjects + the ps-rich loud-skip guard); BEACON.md §12.5 H-1c-2 deviations + the AUDIT-TRIGGERS.md `SYS_FD_DEVCLASS` row + LS-8 BEACON TIER addendum + the CLAUDE.md index line | *(suite re-run in flight; strip identities + flip proofs asserted in-guest on real spawns)* |

## Remaining work

Per ROADMAP §11.1: the H-1 close (the focused audit round batched over
H-1a..c-2 → fix findings → push both mirrors), then H-2..H-7. The parked
display-wall work (B/A wall mechanisms + the GL-parity ledger WSI-DESIGN §8.3 +
the blit default flip) queues at H-6.

## Exit criteria status

ROADMAP §11.2 is the v1.0-final bar; per-chunk gates are in the §11.1 table.
H-1's gate: P1 strip property ✓ (host + in-guest) + the kernel fd-devclass
tests ✓ + `ls | cat` byte-clean ✓ (the coreutil-smoke piped legs) + aurora
zero-diff (its parser swallows OSC 1936; the LS-CI family is the standing
witness) + the focused audit round (in flight at the close).

## Build + verify

```bash
THYLACINE_BAKE_CLADE=1 tools/build.sh all && tools/test.sh   # the suite
cd usr && cargo test -p beacon --target aarch64-apple-darwin  # 27 host tests
cd usr && cargo test -p coreutils --lib --no-default-features --target aarch64-apple-darwin
```

## Trip hazards

- **The consctl render line floor is 67 bytes; every reader ≥ 96** — including
  the kernel's own staging buffers (the H-1a lesson; sweep readers by
  enclosing function).
- **The boot ramfs is FLAT**: pre-pivot, only root + the empty synth mounts
  are listable directories. A smoke leg wanting a bounded `ls` subject uses
  explicit file operands, not a directory.
- **coreutil-smoke's REAP-BEFORE-READ deadlocks the BOOT on child output >
  4096 bytes** (PIPE_BUF) — every new leg budgets its output; the ps-rich leg
  self-guards (measure raw, skip loudly).
- The tier resets at `cons_drain_close` + `cons_test_reset` (NOT "where
  winsize unsets" — winsize never reset on detach).
- SGR never appears inside rich-structured output; the emitting bin forces
  `on = !rich && …`.
- An obj ref is cleaned-absolute-or-no-frame (`coreutils::path::abs`).

## References

`docs/HALCYON.md` (§13 = the bound architecture) · `docs/BEACON.md` (§12 = the
H-1 build card; §12.5 = as-built deviations) · `docs/SYS-FD-DEVCLASS-SPEC.md`
(AS-BUILT) · ARCH §23.5.4 · ROADMAP §11 · `docs/AUDIT-TRIGGERS.md` (the
`SYS_FD_DEVCLASS` row + the LS-8 BEACON TIER addendum) · `docs/JOURNAL.md`
runs 12–15.
