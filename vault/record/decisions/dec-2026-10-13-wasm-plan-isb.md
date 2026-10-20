---
id: dec-2026-10-13-wasm-plan-isb
type: dec
title: "B-2e: a thread that takes a finished Wasm plan executes an ISB before running its stubs (the DFG finalizer's rule)"
date: 2026-10-13
status: superseded
decided-by: research-collapsed
affects: [sub-webkit]
created: 2026-10-13
superseded-by: dec-2026-10-13-wasm-plan-isb-writers
---
## Fork

A Wasm module's wasm-to-wasm and wasm-to-JS exit stubs are JIT-compiled by
whichever thread finishes the module's plan (`WasmEntryPlan.cpp:238-265,
292-307, 340-351`), usually a Wasm worklist thread. The JS thread then runs
them. Where is its context-synchronization event?

## Research

- A synchronous compile waits in `CalleeGroup::waitUntilFinished` ->
  `Worklist::completePlanSynchronously` -> `Plan::waitForCompletion`
  (`WasmPlan.cpp:77-83`), which returns WITHOUT sleeping when the plan is
  already complete; its lock is a compare-and-swap. So the JS thread can reach
  the stubs with no syscall and no ISB: the B-2c obligation 3 class, live today
  with IPInt and the Baseline JIT on. Upstream has the same gap.
- The DFG's rule (`DFGJITFinalizer.cpp:60`): the thread that installs a
  compiler thread's code runs `crossModifyingCodeFence` first.

## Options

- **A. An ISB on the consuming thread** where it takes a finished plan.
- **B. The address-space sync** (`dec-2026-10-13-wasm-space-sync`) on the
  producing thread after the stubs are written.

## The call

A. The consumer is the thread that takes the finished plan, at the
synchronous wait or where the asynchronous completion is delivered, so the
cheaper, local rule covers it; B is for publications with no consumer point (BBQ/OMG installs).
It lands first, at B-2e1, because the gap is live.
