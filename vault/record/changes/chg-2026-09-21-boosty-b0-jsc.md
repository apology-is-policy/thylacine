---
id: chg-2026-09-21-boosty-b0-jsc
type: chg
title: "Boosty B-0: JavaScriptCore runs on Thylacine (JIT off) -- the port wiring"
date: 2026-09-21
arc: arc-boosty
commits: ["b70e1bfd"]
touched: [sub-webkit, sub-substrate-build, moc-userspace]
established: [sub-webkit]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-21
---
The WebKit port's first rung: `usr/ports/webkit` (one patch, the JSCOnly
port with the JIT, the sampling profiler and FTL off), the CMake platform
and C++ toolchain files, the `CHUNK_WEBKIT` build lever with its forage
entry and ICU, and `tools/interactive/ls-jsc.exp`. `jsc` evaluates
scripts on the device. What it could not do on the unpatched libc -- print
a stack overflow, size its heap, read a number with `fscanf` -- is
[[chg-2026-09-21-pouch-b0-libc]]. `docs/browser-status.md` carries the
findings F1-F9 that size B-1.
