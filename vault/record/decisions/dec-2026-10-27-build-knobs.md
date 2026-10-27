---
id: dec-2026-10-27-build-knobs
type: dec
title: "KASLR and the kernel hardening flags are not build options: every image has both"
date: 2026-10-27
status: standing
decided-by: user-vote
affects: [sub-substrate-build, inv-i16]
created: 2026-10-27
---
## Fork

The build configurator offered two kernel symbols, `HARDENING_FULL` ("Enables
the full hardening flag set") and `KASLR` ("off makes crash addresses stable
for debugging"). `CMakeLists.txt` declared `THYLACINE_HARDENING_FULL` and
`THYLACINE_KASLR` (with a third, `THYLACINE_DEBUG_BUILD`), `build.sh` passed
the first two to CMake and printed them, and nothing read any of the three: no
CMake use, no `#if` in the kernel, no boot argument. The kernel toolchain file
adds `-fstack-protector-strong`, `-fstack-clash-protection` and
`-mbranch-protection=pac-ret+bti` to every build, and `kaslr.c` randomizes the
base at every boot. So `KASLR=n` still gave random addresses and
`HARDENING_FULL=n` still gave a hardened kernel, while `test-build-config.sh`
asserted the values and not their effect. Wire the knobs or retire them?

## Research

- Linux: `CONFIG_RANDOMIZE_BASE` builds KASLR in, and the `nokaslr` boot
  argument turns it off for one boot, a documented debug escape. The hardening
  flags are separate Kconfig options (`CONFIG_STACKPROTECTOR_STRONG`, the
  arm64 PAC and BTI options), each changing the compile.
- Plan 9 has no KASLR and no compiler hardening flags to configure.
- Here, a crash dump already prints `HALLS: KASLR offset` and symbolizes each
  frame through the halls symbol table, so a fixed base buys debugging little.

## Options

- **A. Retire both.** Delete the CMake options, the schema entries, the
  `--kaslr` and `--hardening-full` flags, the preset lines and the two
  fragments. KASLR (I-16) and the hardening flags stay unconditional.
- **B. Wire `KASLR=n` as a fixed slide** for debug builds: a documented I-16
  exception, like Linux's `nokaslr`.
- **C. Wire both.** `HARDENING_FULL=n` would then build a kernel without
  canaries, PAC or BTI, which a release could ship by mistake.

## The call

A (operator vote, 2026-10-27). `THYLACINE_DEBUG_BUILD` went with them in the
same change: nothing read it either.

## Rationale

An option that changes nothing misleads the operator who sets it. Options B
and C each add a way to build a kernel weaker than the one that ships, and the
debugging case B serves is already covered by the halls symbolization. A
profile written before the retirement still loads: the configurator names the
unknown symbol and the file holding it, skips that line, and applies the rest.
