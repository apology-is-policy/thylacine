---
id: sub-dosbox
type: sub
title: "DOSBox-X (Cryptid) — native SDL surfaces and capability-bound dynamic code"
parent: moc-userspace
code:
  - tools/dosbox-x-sources.py
  - tools/dx2c-dosprog.py
  - tools/dx3-keyprog.py
  - usr/lib/thylajit/thyla_jit.h
  - usr/ports/dosbox-x/config.h
  - usr/ports/dosbox-x/config_package.h
  - usr/ports/dosbox-x/duke3d/DUKE3D.CFG
  - usr/ports/dosbox-x/duke3d/dosbox-x.conf
  - usr/ports/dosbox-x/glue/thylacine-audio-stubs.c
  - usr/ports/dosbox-x/glue/thylacine-serial-stub.cpp
  - usr/ports/dosbox-x/patches/0001-thylacine-byteorder.patch
  - usr/ports/dosbox-x/patches/0002-thylacine-bios-logo-libpng-gate.patch
  - usr/ports/dosbox-x/patches/0003-thylacine-whereami-platform.patch
  - usr/ports/dosbox-x/patches/0005-thylacine-non-resizable-window.patch
  - usr/ports/dosbox-x/patches/0006-thylacine-dynrec-capjit.patch
  - usr/ports/dosbox-x/patches/0007-thylacine-cycle-telemetry.patch
  - usr/ports/dosbox-x/patches/0008-thylacine-system-config.patch
  - usr/ports/dosbox-x/tombraider/dosbox-x.conf
audit: hard
guarded-by: [inv-i12]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: [docs/DOSBOX.md]
created: 2026-09-17
updated: 2026-09-17
---
## Purpose

Run the vendored DOSBox-X emulator as a native Pouch/SDL application on ARM64.
The port owns its build configuration, boundary patches, JIT adapter and game
configuration fixtures. It does not edit the pristine vendor tree in place.

## Contract

`dosbox-x` uses system defaults under `/lib/dosbox-x`, followed by user/local
configuration and command-line overrides. Repeated `-c` arguments run DOS
commands. A DOS drive maps an explicitly mounted native directory; writable
game state belongs in the operator's home, not the read-only bundled master.
`docs/manual/40-dosbox.md` describes the supported operator workflow.

## Mechanism

`build_dosbox_x` copies the vendored source, applies the ordered patches and
links a static ET_EXEC using Pouch, libc++, SDL2 and zlib. The source extractor
uses upstream Makefile source declarations with explicit disabled-feature
exclusions. The port configuration selects the AArch64 dynrec backend.

Patch 0006 acquires the process's own JIT clearance, then uses
`thyla_jit_create` to obtain distinct writable and executable aliases. Upstream
`DYNCOREM_DUAL_RW_X` already distinguishes emission and branch addresses.
`thyla_jit_icache_sync` publishes emitted blocks and the link/trampoline page,
including the cache-reset path; ordinary EL0 cache maintenance cannot replace
that syscall. Failure is fatal to the emulator rather than permission to use
an executable ordinary heap.

[[sub-sdl-port]] owns the display and input integration. Video-mode recreation
restores the native title and dynamic-frame intent. Halcyon places and zooms
the surface; DOSBox's own resolution remains an application choice. Audio goes
through Nocturne. The serial/CD-codec stubs describe unsupported host APIs;
they do not implement physical serial hardware or the omitted codecs.

## Data structures

The dynrec's cache metadata carries writer and execution pointers and their
fixed alias delta. The kernel owns the backing JIT object and mapping lifetime;
DOSBox owns generated blocks and links. Game configuration files set mounts,
CPU cycles, input behavior and startup commands independently of the binary.

## Concurrency

The emulator uses SDL's event pump and audio callbacks. Surface teardown joins
the event thread through the retire/EOF protocol in [[sub-sdl-port]]. Generated
instructions are published before execution through the executable alias.
This dossier does not introduce a second compositor or interrupt owner.

## Invariants enforced

![[inv-i12#Statement]]

The JIT patch requests separate RW/RX aliases and does not make an ordinary
writable mapping executable. JIT authority remains the kernel's I-42 contract;
clearance is requested by the running process, not inherited from its parent.

## Error paths

A failed patch application or compile/link fails the build. Missing optional
build toolchains are reported by the build dispatcher. JIT acquisition or
cache-publication failure stops the dynamic core. Unsupported host serial and
CD-codec APIs return their explicit stub behavior.

## Performance

The shipped configuration uses bounded fixed cycle presets. The dynamic core
avoids interpretation overhead; its actual throughput depends on the DOS
workload and compositor/audio cadence. A native surface declares continuous
frame intent only while visible under the compositor's visibility rules.

## Prosecution

Attack missing cache publication, alias confusion, JIT failure fallback,
configuration precedence and writes into bundled read-only data. Require DOS
file output and keyboard input witnesses, not only a successful launch log.
The four serial DOSBox gates cover display, input plus foreground return,
configuration and dynamic code. The Halcyon session gate verifies titled pane,
zoom, keyboard exit and shell restoration with reviewed captures.

## Seams

[[sub-sdl-port]], [[sub-nocturned]] and [[sub-substrate-build]] own the runtime,
audio and build boundaries. No unresolved integration seam was found by the
recorded session check.

## Caveats

The basic integration gates do not establish compatibility with every DOS or
Windows program. The separately available Duke3D/Tomb Raider workload gates
are not claimed as newly run by this integration. Software Voodoo rendering
is not a claim of hardware 3dfx acceleration.

## Provenance

(generated from incoming change records)
