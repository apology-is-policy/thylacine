---
id: dec-2026-11-10-mac-runner
type: dec
title: "Thylacine runs on a Mac as an app: a Rust shell over a hidden, patched QEMU, with the Mac served to the guest as a 9P tree"
date: 2026-11-10
status: standing
decided-by: user-vote
affects: []
created: 2026-11-10
---
## Fork

The operator wants Thylacine useful on macOS the way Parallels makes Windows useful: a DMG, a
Dock icon, a window that goes fullscreen, and interop with the Mac. `docs/MAC-RUNNER.md` holds
the research and fifteen forks. Eight went to a vote: how QEMU lives in the app, the 3D route,
which Macs, what closing the window does, the interop transport, key capture, updates, and the
fork's base version.

## Decision

The operator, 2026-11-10. M1: QEMU is a hidden helper, an XPC service inside the app, shipped as
a separate GPLv2 program with a source offer; the app is GPLv3 and links none of it. M2: virgl
over ANGLE to Metal first, then Venus, in one build. M3: Apple silicon on macOS 15 or later, no
Intel build. M4: closing the window suspends to disk, Quit shuts down. M5: 9P over a
virtio-serial port, served by the app, mounted at `/mnt/mac`, shared folders inside it. M6: keys
taken while focused and process switching blocked in fullscreen with no permission, a full grab
optional. M7: Sparkle 2 with EdDSA. M8: the fork starts from QEMU 11.1.x stable. Before the
research the operator had fixed: the app owns its window and no user sees QEMU; Rust, no Swift
required; an unsigned DMG opened through Privacy & Security; QEMU patched for 3D. The same day
the operator approved the I-45 sentence naming the larger trusted host set on macOS, and ROADMAP
10.8.

## Why

Linking QEMU in would make one program of a GPLv2-only whole and a GPLv3 app, and a QEMU assert
would take the window with it; a helper is two programs and a restartable VM. The guest composes
on virgl today, so virgl accelerates it now and Venus waits for a guest client. macOS 15 brings
the in-kernel GICv3, measured working on an M2, which retires the GICv2-only limit and gives the
interrupt controller a saved state, the thing suspend needs. One 9P tree is the Plan 9 shape,
drawterm turned around: one transport and one place where the Mac decides what it exports.

## What it costs

A QEMU fork to rebase. A trusted host half that grows by ANGLE, a Vulkan-on-Metal driver and
our patches, with GL kept from guest code its owner does not trust. Intel Macs get nothing. A
two-step first launch, and whatever Apple does next to unsigned apps. A new guest driver, and
HVF-only bugs that become daily ones.
