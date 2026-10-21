---
id: dec-2026-10-13-jit-region-128
type: dec
title: "B-2e: a JIT code region may be 128 MiB (JIT_REGION_MAX 64 -> 128 MiB); JavaScriptCore's pool runs at upstream's ARM64 no-islands size"
date: 2026-10-13
status: standing
decided-by: user-vote
affects: [sub-webkit, sub-kernel-syscall-dispatch]
created: 2026-10-13
---
## Fork

The FTL and Wasm's BBQ and OMG join Baseline, the DFG and YARR in one code
region. How big may it be?

## Research

- Upstream's ARM64 pool without jump islands is 128 MiB
  (`ExecutableAllocator.cpp:101-106`); Thylacine ran it at 64 MiB,
  `JIT_REGION_MAX` (`syscall.h:2544`).
- A compile whose allocation may fail is refused above 75% of the pool
  (`ExecutableAllocator.cpp:135-137, 1251-1259`) and fails gracefully (the
  function is not optimized again soon); the allocations that must succeed --
  OSR-exit and lazy-slow-path stubs, compiled at run time, and FTL thunks --
  `CRASH()` on true exhaustion (`:1263-1268`).
- A region is a reservation charged per touched page (B-2a), so its size costs
  nothing until code is written.
- The near-branch static assert (`pool <= nearJumpRange`) holds at exactly
  128 MiB: the farthest branch inside the region is 128 MiB - 4 bytes.

## Options

- **A. 128 MiB**: `JIT_REGION_MAX` rises to 128 MiB, the pool follows.
- **B. Keep 64 MiB**: optimizing compiles stop at 48 MiB; the must-succeed stubs
  crash the process if the last 16 MiB fills.

## The call

A (operator vote, 2026-10-13). The kernel limit rises at B-2e2; the pool size
at B-2e3.
