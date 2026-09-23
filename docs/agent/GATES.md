# Build + test commands (the full gate catalogue)

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). CLAUDE.md keeps the image trap and the core commands.
> Section headings keep their original levels.

## Build + test commands

Per `TOOLING.md`. Top-level wrappers.

**A BARE `tools/build.sh` IS NOT THE GATE IMAGE (since 2026-09-09).** With no
flags it applies `configs/default.config`, and that profile now sets
`HALCYON_SESSION=y` -- **Halcyon is the default UI**, so after login the image
runs the tiled environment instead of `ut` on `/dev/cons`. **47 interactive
scenarios log in and 35 of those then drive a shell** (measured, not
estimated); those 35 want `ut` and will not find it. So:

- **Product / demo image, or anything a person will use** -> bare
  `tools/build.sh` (or `--config <your profile>`). This is the Halcyon image.
- **The gate fleet -- `tools/test-interactive.sh`, and anything asserting on a
  post-login shell** -> `tools/build.sh --config ci`, which pins
  `HALCYON_SESSION=n` EXPLICITLY for exactly this reason. A caller-set
  `THYLACINE_HALCYON_SESSION=0` also wins, since `bc__export_env` does not
  clobber a pre-set env var.

The two Halcyon levers are separate and only the session one is defaulted:
`HALCYON_CONSOLE` (halcyond as the PRE-login console renderer) stays off,
because it is not a pure renderer swap -- it also bakes a `#wedge` test rule
into `/lib/beacon/verbs`, the #880 strip-for-production class. Full schema +
the theme picker: `docs/BUILD-CONFIG-DESIGN.md` section 4.2.

```bash
# Build the kernel ELF -- the DEFAULT (Halcyon) image
tools/build.sh kernel

# Build the GATE image (no Halcyon session; what test-interactive expects)
tools/build.sh kernel --config ci

# Build the musl + sysroot
tools/build.sh sysroot

# Build all Rust userspace components
tools/build.sh userspace

# Assemble the disk image
tools/build.sh disk

# Build everything
tools/build.sh all

# Run tests against a fresh QEMU VM
tools/test.sh

# SMP soundness gate (single boots lie -- multi-boot or it didn't happen).
# Builds default + UBSan kernels, multi-boots smp4/smp8 x default/UBSan N>=10,
# classifies CORRUPTION vs EXTERNAL-KILL vs benign host-TIMING vs OTHER. Fails
# iff any boot corrupts, is externally killed, or is unclassified.
# EXTERNAL-KILL has TWO detectors (#222): QEMU's own 'terminating on signal'
# report (#88) sees only CATCHABLE signals -- SIGKILL is uncatchable, so the
# arm that most needed the bucket could not reach it, and the #200 sightings
# landed in OTHER. The second reads the shell's job notification from the
# HARNESS log, gated on test.sh's qemu_alive_at_teardown=0 so the harness's own
# teardown kill cannot trip it. Captures are ARCHIVED, never deleted (#223) --
# re-running a label to investigate it must not destroy the evidence.
tools/ci-smp-gate.sh                    # full matrix, N=10 (or: make smp-gate)
SMP_GATE_CONFIGS="default-smp4 ubsan-smp4" tools/ci-smp-gate.sh   # amplifier subset

# The gate's classifier, tested without booting (fast; sources the real ladder).
tools/test-smp-classify.sh

# Hardening witnesses (#245). Both of these were invoked by NOTHING until
# 2026-08-18 -- and both were already named in this file, inside the boot-banner
# paragraph below, purely as CONSUMERS of the ABI strings (things that would
# break if you reworded one), never as gates to run. That is the entire
# difference between a mention and a command, and it is why they rotted while
# `test-a72` and `check-v80-floor` -- one screen down, in this block -- did not.
# test-fault builds one kernel per provoker and PASSes iff each EXTINCTIONs with
# its expected message: the ONLY proof that the canary / W^X / BTI / the two
# stack guards / the idle guard / the recursion arm actually FIRE, as opposed to
# merely being compiled in. Its absence from every gate is how #244 --
# recursive_kernel_fault emitting nothing at all -- hid for about a month.
tools/test-fault.sh                 # all 7 variants    (or: make test-fault)
tools/test-fault.sh canary_smash    # one variant       (-v for log dumps)

# verify-kaslr multi-boots and PASSes iff the slide varies across N: ROADMAP
# section 4.2's exit criterion for I-16, and that invariant's ONLY runtime
# witness. `make test` accepts any SINGLE boot, so it is structurally blind to a
# slide that never moves -- the same shape as test.sh being blind to LSE above.
tools/verify-kaslr.sh               # 10 boots          (or: make verify-kaslr)
tools/verify-kaslr.sh -n 25 -v      # more boots, print each offset

# Warp-6 V-0 (the Venus gate). `warp-host.sh venus` boots the remote GL host
# TWICE -- once with `venus=on,blob=on,hostmem=256M` and once WITHOUT -- and
# passes only if capset id=4 (VENUS) is present in the first and ABSENT in the
# second. The control leg is not a bonus: a one-directional check is satisfied
# by a host that advertises the capset unconditionally. venus needs blob AND
# hostmem together, and QEMU refuses the device otherwise rather than degrading.
# BOTH GL hosts pass it: thyla-pi (KVM/V3D, ~220 s per boot) and thyla-gl
# (Parallels/TCG/lavapipe, ~350 s), and they report byte-identical feature
# words. test-venus-verdict drives the SAME verdict verb against crafted logs,
# so the discrimination is testable without paying two boots at all (#245: a
# checker reachable only by hand rots).
WARP_HOST=thyla-pi WARP_ACCEL=kvm tools/warp-host.sh venus   # certify (2 boots)
WARP_HOST=thyla-gl tools/warp-host.sh venus                  # iterate (2 boots)
tools/test-venus-verdict.sh         # its verdict, no boot  (or: make test-venus-verdict)

# haul's npxf known-answer vectors (#245 again). Re-derives all 23 from npxf's
# OWN source and diffs the committed fixture, so kat/vectors.txt is a CHECKED
# recording rather than a file whose only claim to being npxf's output is that
# someone once said so. It also refuses to emit unless BOTH direction labels
# hold -- one leg per handshake, each pinning the other side's ephemeral. One
# leg is not enough and the reason is instructive: a symmetric check cannot see
# a swap applied to both halves, and pinning only the responder stops exercising
# server_handshake entirely. All four sabotage cases are measured in
# HAUL-DESIGN.md 4. SKIPs cleanly (exit 0 + a SKIP line) where npxf is absent --
# it lives outside version control, so on any other machine this is a skip, not
# a failure.
make test-haul-kat                  # or: usr/haul/kat/regen.sh [--write]

# ARMv8.0 floor guard (#91). The SOURCE + BINARY checks run automatically at the
# tail of every ramfs bake; these are the extras. `check-floor` adds the big pool
# payloads (/clade, /goroot, ~6 min); `test-a72` is PORTABILITY.md section 3's
# verification bar -- the ONLY gate that can see an LSE regression, since the
# default test.sh runs HVF -cpu host (M2, LSE present) and is structurally blind.
tools/check-v80-floor.py            # fast: source + the ramfs binaries (~7 s)
tools/check-v80-floor.py --all      # + /clade + /goroot   (or: make check-floor)
make test-a72                       # boot on -cpu cortex-a72 (ARMv8.0-only)

# Interactive E2E regression net (LS-CI): expect/PTY drives a REAL console --
# login + assert rendered command output (the test that would have caught LS-1).
# Optional gate (SKIPs without `expect`). THYLACINE_ACCEL=tcg default; bounded
# retry (LS_CI_ATTEMPTS=3) tolerates host-timing flakes.
# REFUSES to start (exit 2) if a VM from this tree is already running (#224):
# its reaper is tree-wide `pkill -9`, so it would SIGKILL a boot it does not
# own -- presenting to the other gate as "qemu GONE, guest healthy" -- and both
# gates restore the same build/fixtures/pool.img. Do not run it alongside the
# SMP gate in one tree; use a separate worktree.
# NEEDS THE GATE IMAGE: bake with `--config ci` (or THYLACINE_HALCYON_SESSION=0).
# A bare build is the Halcyon-default image since 2026-09-09, where login
# spawns the session compositor and the 35 shell-driving scenarios find no
# `ut` prompt. See "Build + test commands" above.
tools/test-interactive.sh               # full set (or: make test-interactive)
tools/test-interactive.sh ls-ci         # one scenario by name

# Signal witness (#200): NAME the sender of the SIGKILL that makes a QEMU vanish
# with a healthy guest. SIGKILL is uncatchable, so the victim can never report
# it -- smp-multiboot's arm-2 detector proves THAT it happened but prints
# "sender NOT RECOVERABLE". macOS Endpoint Security observes signals from
# OUTSIDE the victim, so it names both ends; needs root, but NOT a SIP change.
# Watch mode REFUSES until --selftest has proven the capture can see a kill --
# an unproven watcher logs nothing and reads exactly like a quiet host.
# Routine teardown kills appear on every boot: the finding is a sender that is
# NOT ours, never the mere presence of records.
sudo tools/sigwatch.sh --selftest       # prove it, then
sudo tools/sigwatch.sh                  # watch -> build/sigwatch.jsonl

# Launch a dev VM
tools/run-vm.sh

# Snapshot management
tools/snapshot.sh save <name>
tools/snapshot.sh restore <name>
```

The `Makefile` at the root provides `make kernel`, `make all`, `make test`, etc. as conventional aliases.

---
