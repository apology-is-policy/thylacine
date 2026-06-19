# JIT on a strict-W^X capability OS: JIT-as-a-capability

**Status: adopted (2026-06-19) as a NOVEL.md post-v1.0 candidate via the
aux-merge intermezzo; this file is the detailed reference, the lead-position
framing is the new NOVEL.md entry. NO v1.0 consumer (Go + Rust are AOT +
W^X-clean); the motivating runtimes (V8 / LuaJIT / HotSpot / wasm-JIT) are
post-v1.0. The mechanism (a dual-mapped code Burrow + `SYS_ICACHE_SYNC` +
`CAP_JIT`) STRENGTHENS I-12 (W^X-clean at PTE granularity), it does not weaken
it; cross-ref ARCH §6.6 (the post-v1.0 pkey-shaped-syscall note).**

Design note / **NOVEL candidate**. Aux-authored from a design conversation
(2026-06-17). The kernel pieces are main-track (aux can't implement them); this
note exists so the idea + reasoning survive, and so the main agent can fold the
load-bearing parts into `docs/NOVEL.md` + `docs/ARCHITECTURE.md` when relevant.

## The problem

Thylacine enforces **strict W^X (invariant I-12)**: every page is writable XOR
executable, and -- crucially -- there is **no prot-mutation syscall** (no
`mprotect(RW->RX)` flip). This is deliberate and load-bearing.

Runtime **JIT** compilers want to *write* machine code and then *execute* it --
the classic `mmap(RWX)` or `mprotect(RW then RX)` dance. That fights I-12 head
on. We hit this twice while scoping ports: V8 (for Node.js / Claude Code) and
the JS engine inside a future Ladybird-based browser.

## First, the scope-shrinker: AOT languages are already clean

A misconception worth killing up front: **Go does not JIT.** It compiles
ahead-of-time (AOT) to a normal native binary; its runtime has a GC + goroutine
scheduler but does **no runtime code generation** (`reflect.MakeFunc` uses a
pre-built trampoline; `plugin` uses `dlopen` = file-backed executable maps,
which are W^X-clean). Empirical proof: **Go runs on OpenBSD**, the strictest
mainstream W^X system.

So a Rust (AOT) + Go (AOT) default toolchain is **entirely W^X-clean**, GC and
all -- no JIT wrinkle for either. The JIT problem is *only* the
interpreted/managed runtimes: **V8** (Node), **HotSpot JVM**, classic **.NET**,
**LuaJIT**, **PyPy**, and **wasm engines in JIT mode**. (CPython is an
interpreter -- clean; its 3.13+ copy-and-patch JIT is optional/off. wasm can
also be AOT-compiled to native -- a sidestep.)

## The mechanism -- and Thylacine already implements it internally

The canonical W^X-compatible JIT technique is **dual-mapping**: map the *same
physical pages* at **two virtual addresses** -- one **RW** (the compiler writes
through it), one **RX** (execution jumps through it). No single PTE is ever W
AND X, so **I-12 holds at page granularity**. You just hold two views of one
code buffer.

The key fact: **the Thylacine kernel already does exactly this.** The Lazarus
W1.5 LSE alternatives-patcher self-modifies `.text` by writing through *"a
TRANSIENT RW-not-X alias (a scratch VA mapped PTE_KERN_RW = RW+PXN+UXN) ... the
canonical .text stays RO+X ... no page is ever writable AND executable at PTE
granularity, not even momentarily"* (`arch/arm64/alternatives.c` +
`mmu.c::mmu_patch_text`). That **is** the JIT primitive -- including the ARM64
icache-coherence sequence it requires:

    dc cvau   <writer alias>     // clean D-cache to PoU
    ic ivau   <exec VA>          // invalidate I-cache
    dsb ish
    isb                          // CTR_EL0 line stride; per-page loop

So this is not a hypothetical mechanism to invent -- it is an existing,
**already-trusted** one to *expose*.

## The design: a small kernel primitive + a thin userspace layer

Both halves of "kernel module or userspace layer?" -- it's naturally both.

### Kernel primitive (small)

A **dual-mappable "code Burrow":** allow one BURROW (VMO) to be attached
**RW at VA_w** and **RX at VA_x** in the same Proc simultaneously. This is
W^X-clean because the two mappings are *different VAs / different PTEs* of one
physical region -- the per-PTE W^X check (`arch/arm64/mmu.c`) is never
violated. (Today the VMA/burrow path presumably refuses an executable mapping
that aliases a writable one; the lift is to permit it *specifically* for a
code-Burrow kind.) Plus a tiny **`SYS_ICACHE_SYNC(range)`** -- the cache dance
the W1.5 patcher already has, lifted to a syscall. Gate the whole thing behind a
new **`CAP_JIT`** capability (elevation-only / non-rfork-grantable, like the
other sensitive caps).

### Userspace layer

A `libthyla_rs::jit` module: create the code Burrow -> get
`(writer_ptr, exec_ptr)` -> the engine compiles into `writer_ptr` -> call
`icache_sync` -> `transmute` `exec_ptr` to a fn pointer and call. V8 /
wasmtime / a JVM plug their codegen backend's "emit + commit" step into that
shim instead of `mprotect`.

## Why this is a NOVEL angle

**JIT-as-an-explicit-capability** on a strict-W^X capability OS. Instead of
ambient `mprotect(RWX)` -- which any process can call and which exploit chains
love -- making code executable becomes a **deliberate, capability-gated,
region-confined, auditable** act:

- only a Proc holding `CAP_JIT` can create executable-able code regions;
- the executable region is a *specific* Burrow, not arbitrary process memory;
- the act is logged / attributable.

That is strictly stronger than the Linux/macOS posture, and it is the natural
extension of the W1.5 self-patcher's discipline turned outward. It belongs in
`docs/NOVEL.md` as a lead position when the graphical / managed-runtime era
(Halcyon, V8, JVM) comes into view.

## Honest caveats

1. **It controls who-may-emit-code and where -- NOT what the code does.** JIT'd
   native code runs with the Proc's full capability set. For *untrusted* JIT (a
   browser's JS), pair `CAP_JIT` with per-Proc namespace/capability confinement
   (the renderer-sandbox story): the JIT cap says "you may emit code," the
   namespace says "into a box with no authority."
2. **The JIT backend must be Thylacine-hardening-aware:** emit **BTI** landing
   pads (`bti c` at indirect-branch targets) and respect **PAC**, since the
   kernel runs BTI/PAC-hardened. Real but bounded porting cost on the codegen
   backend.
3. **ARM64 has no fast per-thread W^X toggle** (Apple Silicon's APRR -- what
   JavaScriptCore uses -- is Apple-proprietary). So **dual-mapping is the
   portable answer** anyway; we are not giving up a faster option.
4. **SMP / icache coherence** is the W1.5 dance (already coded); a userspace JIT
   that relocates/moves code re-runs `icache_sync` over the moved range.

## The spectrum, ranked

1. **Interpreter / jitless** -- works today, zero kernel change, slower. Fine
   for I/O-bound runtimes (e.g. an agent CLI on jitless V8).
2. **AOT** -- sidestep entirely where the language allows: **Rust, Go** (both
   clean); wasm -> native; *not* JS.
3. **Dual-mapped code Burrow + `CAP_JIT`** -- the exotic, capability-gated path
   for V8 / JVM / wasm-JIT. The kernel mechanism already exists (W1.5); the lift
   is a modest primitive + a userspace shim.
4. *(Hardware W^X toggle -- not portable; Apple-only -- noted for completeness.)*

## Punchline

The two languages actually chosen for the default toolchain (Rust + Go) need
**none** of this -- both are AOT and W^X-clean. But if/when V8 (Node, a browser)
or a JVM arrives, the "exotic way" isn't exotic on Thylacine: it's the **W1.5
LSE patcher, turned outward and gated as a capability.**

## Status / handoff

- **NOVEL candidate** -- main agent folds the lead-position framing into
  `docs/NOVEL.md` and the kernel primitive into `docs/ARCHITECTURE.md` when the
  managed-runtime era is in scope.
- Kernel work (the dual-map code-Burrow kind, `SYS_ICACHE_SYNC`, `CAP_JIT`) is
  main-track; aux cannot implement it.
- Motivating use-cases on record: V8 for Node.js / Claude Code (see the
  conversation), and the JS engine inside a Ladybird-based Halcyon browser.
- Post-v1.0. Not a roadmap item; a design idea banked for when it matters.
