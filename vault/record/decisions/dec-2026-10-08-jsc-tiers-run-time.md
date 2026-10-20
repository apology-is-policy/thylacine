---
id: dec-2026-10-08-jsc-tiers-run-time
type: dec
title: "B-2c: every JavaScriptCore tier is built, and the ones not yet opened are held off at run time"
date: 2026-10-08
status: standing
decided-by: user-vote
affects: [sub-webkit]
created: 2026-10-08
---
## Fork

The operator's vote of 2026-09-28 set the order in which JavaScriptCore's JIT
tiers open: Baseline and the regexp (YARR) JIT in B-2c, the DFG in B-2d, then
FTL and Wasm's BBQ and OMG in B-2e, each with its own audit. B-2c's first
build, with the JIT on and DFG and FTL off, failed: upstream WebKit no longer
builds that configuration on 64-bit ARM. The question was how B-2c honours the
order when the build cannot express it.

## Research

- **The bit-rot, measured.** B3 exists only with FTL (`PlatformEnable.h:839`),
  and the Wasm code assumes that JIT implies B3 on every port except
  PlayStation. `JITInlineCacheGenerator` visits a DFG inline-cache variant
  that does not exist without the DFG. Some files see the jump-table types
  only as declarations. Seven objects failed, and ninja stopped each at its
  first errors, so the true depth was unknown.
- **Upstream's precedent.** RISC-V 64 builds every JIT level "to simplify
  building JSC, avoiding otherwise rare combinations of build-time
  configuration", and turns FTL off at run time in
  `Options::notifyOptionsChanged` (webkit.org/b/239707). The all-tiers build
  is the configuration upstream maintains.
- **The options freeze.** After `notifyOptionsChanged` the options are
  finalised and the config page is frozen, so a value forced there cannot be
  changed by a later flag (Fable r1 verified every write to the four options
  and every tier entry predicate).

## Options

1. **Gate tiers at run time.** Build every tier; under `OS(THYLACINE)` the
   Options patch forces `useDFGJIT`, `useFTLJIT`, `useBBQJIT` and
   `useOMGJIT` off, where no flag can turn them back on. B-2d and B-2e each
   lift one gate. Costs a bigger binary and a longer build.
2. **Patch the bit-rot.** Compile only Baseline and YARR, carrying upstream
   guard and include fixes until it builds: an unknown number of hunks, dead
   weight once B-2d turns the DFG on, and a Baseline-only 64-bit JSC that
   upstream no longer exercises at run time.

## The call

Option 1 (operator, 2026-10-08, AskUserQuestion, "Gate tiers at run time").
The 2026-09-28 tier order stands, enforced at run time; the B-2c audit covers
the gate, and `ls-jsc` pins it twice: the option values with every tier asked
for by flag (`tiers`), and the behaviour, a hot function that reaches
Baseline code and never DFG code (`dfg`).

## Rationale

The built configuration is the one upstream maintains, so the engine's code
paths are the tested ones; what B-2c opens is decided by four lines that one
audit can read, not by a patch series whose size nobody knew. Each later
chunk's change is one line removed from that block, which keeps every tier's
opening its own audited step.
