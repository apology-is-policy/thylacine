---
id: dec-2026-09-21-browser-engine-order
type: dec
title: "The browser arc: WebKit first, Rust std in parallel (aux), Servo second, no stage 0"
date: 2026-09-21
status: standing
decided-by: user-vote
affects: [inv-i12, sub-pouch-seam, sub-kernel-weft]
created: 2026-09-21
---
## Fork

The operator opened a web-browser arc on 2026-09-21 with Ladybird and
Gecko/SpiderMonkey as their candidates, a third option invited, and one
exclusion: nothing Chrome, nothing tied to Google or Microsoft -- narrowed the
same day to exactly Blink, V8 and Chromium. Which engine is brought up first,
whether a small browser ships early as a stage 0, and when the Rust `std` port
starts were the three choices; a fourth question was the effort level for the
load-bearing kernel work the arc reaches.

## Research

`docs/BROWSER-DESIGN.md` (committed PROPOSED at `c09141da`) carries it in full:
six parallel research lanes over primary sources, spot-checked, plus the tree's
own starting line measured by grep. The findings that decided it: every full
engine needs the same platform tranche (an anonymous-memory surface Pouch lacks,
a channel that can carry memory, C libraries, a GL entry point); Ladybird is
now one third Rust and so needs both the Rust `std` port and the IPC work, on
an upstream closed to outside code since 2026-06-05; WebKit has a GLib-free
upstream port that paints into a caller-owned buffer, runs JIT-less with
WebAssembly as a supported AArch64 configuration, and carries a Darwin-only
dual-mapped JIT that matches I-42; Servo is single-process without a sandbox on
aarch64 and loses WebAssembly without a JIT; Gecko has no embedding off Android.

## Options

1. WebKit first, then Servo (recommended).
2. Servo first.
3. Ladybird first.
4. Platform tranche first, engine chosen later.

Stage 0: `webfs` only (recommended) / `webfs` + NetSurf / neither.
Rust `std`: in parallel now (recommended) / after WebKit renders / undecided.
Effort: raise to max / xhigh for B-0 only / xhigh throughout.

## The call

The operator, by blocking question, 2026-09-21:

- **Engine: WebKit, then Servo.**
- **Stage 0: neither.** No NetSurf and no `webfs` now; every hour goes to the
  main engine's path. (This overrides the recommendation to build `webfs`
  regardless.)
- **Rust `std`: in parallel, now** -- and, in a follow-up message the same
  hour: "I will launch Aux to deliver the Rust STD." The aux track owns it.
- **Effort: stay at xhigh throughout** the arc, noted in each audit-bearing
  commit.
- **Name: Boosty**, after the operator's cat (a message the same hour). The
  thematic candidates the design had held are withdrawn.

## Rationale

The operator took the recommendation on the engine and on the `std` timing and
cut scope where the recommendation had hedged: a side browser and a heritage
fetch service are both deferrable, and neither is on the path to WebKit, whose
own network process is the confined fetch service of the design. Two kernel
designs stay open by intent and return for a signature in their own scripture
commits: the shape of guard regions against I-12's "no permission-mutation
syscall" sentence, and shared memory for unprivileged Procs (a generalised
Weft gate, or Mycelium).
