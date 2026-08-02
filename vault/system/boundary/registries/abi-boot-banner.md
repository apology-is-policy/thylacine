---
id: abi-boot-banner
type: abi
kind: contract
stability: frozen
title: "The boot-banner contract — `Thylacine boot OK` and `EXTINCTION:`"
pinned-by:
  - "kernel/main.c (boot_mark_complete)"
  - "kernel/extinction.c"
  - "docs/TOOLING.md §10"
mirrors:
  - "tools/test.sh"
  - "tools/smp-multiboot.sh"
  - "tools/interactive/lib.exp"
created: 2026-08-01
updated: 2026-08-01
---
## The surface

Exactly two strings on the UART are kernel ABI with the development tooling:

- **`Thylacine boot OK`** — boot success. Must appear on a line by itself.
- **`EXTINCTION:`** at start-of-line — catastrophic kernel failure (an
  Extinction Level Event; the thylacine's fate transposed onto a kernel that
  has lost the will to continue).

Everything else the banner prints — `arch:`, `cpus:`, `mem:`, `dtb:`,
`hardening:`, `features:`, `kernel base:` — is **informational** and free to
evolve. Nothing matches on it.

## Why it is frozen

It is the whole agentic loop's success signal. `tools/run-vm.sh`,
`tools/test.sh`, `tools/agent-protocol.md`, and `CLAUDE.md` all key on these
two strings; TOOLING.md §10 states plainly that they "do not change without
updating [all of them] in the same commit."

## The emission rule, and what changed at A-5a

The banner is **not** printed by `boot_main` at the end of bring-up, and it
is no longer tied to init's exit. joey signals `SYS_BOOT_COMPLETE` after its
boot-test asserts pass and just before it becomes the persistent session
supervisor (it getty-loops `/sbin/login`), and `boot_mark_complete()` prints
the line. joey is long-running init and does not exit on success, so there
is no exit to ride.

Three properties the emission must keep:

1. It appears **only after** init's boot-test asserts have passed — a
   pre-completion failure exits joey non-zero, which extincts in `joey_run`,
   so the banner never prints.
2. It does not appear if the kernel extincted, or if init failed, before
   signalling.
3. `SYS_BOOT_COMPLETE` is **one-shot and gated on the caller being
   console-attached** (joey, the boot console-trust anchor) — so a spawned
   child cannot emit a premature banner and manufacture a false PASS.

## Consumer obligations

- **Extinction outranks the banner.** A crash is a FAIL whether or not the
  banner also printed. Every consumer checks `^EXTINCTION:` first on every
  poll.
- **The banner is not the end of the boot.** Because joey persists, a getty
  or login fault can crash *after* a green banner. Consumers watch a grace
  window (`BANNER_GRACE`, default 3 s) before declaring PASS.
- **Match with `grep -a`.** Boot logs carry binary spill; without it grep
  declares the file binary and reports "binary file matches" — which a `-q`
  test reads as a match and a negated test reads as its opposite.
- **Anchor `EXTINCTION:` at start-of-line** (`^`). It appears mid-line in
  quoted context (log slices, this note) and an unanchored match reports a
  crash that did not happen.

## Prosecution

- Any change to either string is an ABI break requiring the four-file
  lockstep update.
- Any new path that can print `Thylacine boot OK` outside
  `boot_mark_complete` breaks the one-shot console-attached gate, which is
  the only thing preventing a forged PASS.
- A consumer that matches the banner without an extinction pre-check, or
  without the grace window, will report a crashed boot as green.

## Referenced by

[[sub-substrate-gates]] · [[sub-substrate-machine]] ·
[[sub-substrate-interactive]] · [[moc-substrate]].
