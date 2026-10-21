---
id: dec-2026-10-13-wasm-plan-isb-writers
type: dec
title: "B-2e: the Wasm plan ISB stands, at the instance; the stubs' writers are the validation plan and the group's creator"
date: 2026-10-13
status: standing
decided-by: research-collapsed
affects: [sub-webkit]
created: 2026-10-13
supersedes: dec-2026-10-13-wasm-plan-isb
---
## Fork

[[dec-2026-10-13-wasm-plan-isb]] put an ISB on the thread that takes a finished
Wasm plan, and described the gap as stubs written by "whichever thread
finishes the module's plan" and reached through a synchronous compile's wait.
B-2e1's Fable round (F1) showed that map is wrong. Which thread writes the
stubs, which wait skips the sleep, and which fence covers every consumer?

## Research

Verified tree facts (fork `5e8cf8cd83`, JavaScriptCore):

- A module's wasm-to-JS exit stubs come from the *validation* plan.
  `Module::Module` takes them from the `IPIntPlan` that `Module::validateSync`
  or `Module::validateAsync` ran, and the first worklist thread to claim them
  generates them in `EntryPlan::compileFunctions`. `validateSync`'s
  `waitForCompletion` is the wait that can return without sleeping.
- A CalleeGroup's wasm-to-wasm exit stubs are written by the thread that
  completes the group's plan. The IPInt group completes on the thread that
  creates it (`completeSyncIfPossible` in the `CalleeGroup` constructor), and
  another thread may take that group later through `Module::compileSync`.
- Both instance-creation sites (`WebAssemblyInstanceConstructor`,
  `JSWebAssembly::instantiate` for bytes, a Module, streaming and the module
  loader) call `JSWebAssemblyInstance::finalizeCreation`, and its fence
  precedes every read of a stub address and the start function
  (`WebAssemblyModuleRecord::evaluate`).
- IPInt calls a JS import through the stub, so the gap is live with IPInt alone.

## Options

- **A. Keep both fences and correct the record** (the call below).
- **B. Keep only `finalizeCreation`'s**, the one every instance passes.
  `compileSync` would then return a group whose wasm-to-wasm stubs another CPU
  wrote, safe only while every caller instantiates before running anything.

## The call

A. `JSWebAssemblyInstance::finalizeCreation`'s fence is the load-bearing one:
every instance passes it before any stub runs. `Wasm::Module::compileSync`'s
stays, for a group another thread created. The build checks both functions for
an `isb`, and checks that `finalizeCreation`'s comes before its first call into
`WebAssemblyModuleRecord`.

## Rationale

The call of [[dec-2026-10-13-wasm-plan-isb]] stands: a consumer-side ISB, the
DFG finalizer's rule. Only the description of the writer and the wait changes.
B saves one instruction on a path that compiles a module, and it would leave a
returned group's safety depending on what its caller does next.
