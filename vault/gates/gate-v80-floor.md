---
id: gate-v80-floor
type: gate
title: "The ARMv8.0-A userspace floor guard"
proves: "That nothing shipped in userspace requires above the ARMv8.0-A baseline: (a) no tracked build input asks for a higher -march, and (b) no shipped aarch64 binary carries an LSE instruction a runtime check cannot skip. PORTABILITY.md section 3 binds the system to that floor -- it is the only baseline that runs on every target row, including the Cortex-A72."
blind-to: "The KERNEL (deliberately out of scope: the W1.5 boot patcher rewrites LL/SC into LSE in place, so kernel LSE lives in .altinstr_replacement with no branch before it and this checker would correctly call it ungated -- do NOT point --binaries at the kernel ELF); non-LSE baseline violations (a v8.1+ instruction that is not an atomic); anything not present in the scanned artifact set; and the SOURCE half is blind to a vendored .S, a prebuilt archive, or a dependency shipping its own flags."
invocation: "tools/check-v80-floor.py (source + the ramfs binaries, ~7 s -- runs automatically at the tail of every ramfs bake); --all adds /clade + /goroot (make check-floor, ~6 min); make test-a72 boots on -cpu cortex-a72 and is the only gate that can OBSERVE a regression, since the default test.sh runs HVF -cpu host where LSE is present."
created: 2026-08-01
updated: 2026-08-01
---
## Method

Two independent checks, because they fail differently.

**source** — no tracked build input asks above the floor. Cheap, exact,
names the offending file. It greps every tracked file rather than a
maintained list, which is what let it catch the three `-march` sites that
appeared after W1u.

**binaries** — no shipped binary carries an ungated LSE. This is the one
that matters, and #71's postmortem says exactly why: `tools/pouch-clang` was
not in the first enumeration (cmake + build.sh + cargo); it was found only
because measuring the OUTPUT left two ungated instructions in a shipped
binary. **"Enumerating the files you expect is not the same as measuring
what shipped."**

## Classification rules

An LSE instruction is GATED iff a nearby preceding feature-byte load pairs
with a conditional branch whose target lies past it. One structural rule
covers both producers in the tree:

```
outline-atomics:   ldrb w16,[x16,#0x588]   ; __aarch64_have_lse_atomics
                   cbz  w16, .Lllsc
                   casalb ...
Go runtime:        ldrb w2,[x27,#0x357]    ; runtime.arm64HasATOMICS
                   tbz  w2,#0x0,.Lllsc
                   swpalb ...
```

Deliberately NOT symbol-based: the shipped clade toolchain (clang, clang++,
lld, clangd) is fully STRIPPED, so there are no symbol names to allowlist.
The LSE regex is anchored so the LL/SC forms a v8.0 core actually runs
(`ldaxr` / `stlxr` / `ldar` / `stlr`) do not match.

## History

Exists because #71 shipped: Lazarus W1 moved the kernel to the floor, the
userspace toolchains kept `+lse` for months, and on an A72 the kernel booted
1209/1209 and then every allocating userspace binary died with `snare:ill`
on a `casalb`. **Nothing checked, so nothing noticed.**

Its own build (#91) then reproduced the family failure one level up: the
positive control passed VACUOUSLY — the "clean" leg contained zero LSE
instructions, so "not flagged" proved nothing. A control must prove
DISCRIMINATION, not detection ([[haz-harness-fail-open]]).
