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
`mprotect(RW->RX)` flip). This is deliberate and load-bearing. [Amended
2026-09-23: there is now `burrow_protect` (ARCH 6.5, the permission ceiling), and
the load-bearing part is intact by construction -- it never accepts X as a target
and never raises a mapping past a mint-time ceiling that, for anonymous memory, is
RW. The three doors to X are unchanged: exec, a provenance-gated file map, and this
document's `SYS_JIT_CREATE`.]

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

*(5 and 6 added 2026-08-13, after CL-7k landed the primitive. Both are gaps this
document originally missed; the full write-up with verified file:line evidence
is task #235, which is DORMANT until a real consumer exists.)*

5. **The permanent writer alias weakens W^X *within* the JIT process — and that
   matters only for UNTRUSTED JIT.** An arbitrary-write primitive can write
   through `writer_va` and jump to `exec_va`. I-12 still holds literally (no VA
   is both W and X), but the property people *want* from W^X is softened; this
   is why V8 moved toward pkey-based protection where the hardware offers it.
   Irrelevant for a JVM or a trusted compiler, central for a browser.
   **Thylacine has an answer Linux and macOS do not**: `burrow_share_into` takes
   a *per-Proc* prot, so the two views can live in **different address spaces** —
   a codegen Proc holding RW, an executing Proc holding only RX, which cannot
   reach the writable alias at all because it is not mapped there. That is
   caveat 1's confinement made concrete on the Weft (I-37) substrate. Open cost
   question: an IPC per patch is likely too slow for inline caches, so it
   probably wants tiering — a measurement, not an argument.

6. **Dynamic binary translators (FEX-class) are blocked on something else
   entirely: resumable faults.** SMC detection means write-protecting *guest*
   code pages, trapping the write, invalidating translated blocks, and
   **resuming** — the same primitive GC write barriers and CRIU need, and
   nothing to do with emitting code. We are one wire from it: `SYS_NOTED(NCONT)`
   already restores a saved user context, and `snare:segv`/`snare:bus` are
   already synthesized on EL0 faults — but `snare:*` is **not in
   `g_known_notes`** at v1.0 and any unhandled one terminates. Connecting them
   is a connection, not a new mechanism, and the Linux phenotype needs it
   regardless to be honest about a guest that installs a `SIGSEGV` handler and
   expects to continue. The delivery model (in-thread Plan 9 `notify`/`noted`
   vs. an out-of-thread exception channel à la Fuchsia/seL4/Mach) is a real
   fork and audit-bearing on I-19 — scripture before code.

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

## B-2: what a browser engine's JIT needs (2026-10-07)

The browser arc (`docs/BROWSER-DESIGN.md` section 7) brings the first
*untrusted* JIT: JavaScriptCore running a web page's script. The operator voted
the shape on 2026-09-28; three things change, and Wasm brought a fourth
(2026-10-13).

1. **The region is a reservation** (B-2a). JavaScriptCore reserves one
   executable pool at startup (its ARM64 default is 128 MiB without jump
   islands; Thylacine ran it at 64 MiB, `JIT_REGION_MAX`, inside the +-128 MiB
   branch range, until B-2e2 raised the limit to 128 MiB, upstream's size) and fills it as it compiles. An eager region charged and
   zeroed the whole pool at startup and needed one physically contiguous buddy
   block. The code Burrow is now demand-zero, like `SYS_BURROW_ATTACH_LAZY`'s
   memory: a page is committed, zeroed, I-cache-invalidated and charged once,
   by the first touch through either alias. Nothing surveyed (V8,
   SpiderMonkey, .NET, JSC itself) needs physical contiguity.
2. **The I-cache sync is exact on aliasing I-caches** (B-2a). The sync
   invalidates by the kernel's direct-map address. On a VIPT I-cache
   (Cortex-A53 class) that can miss lines fetched through the exec alias, so
   when any CPU reports a non-PIPT I-cache the sync invalidates the whole
   I-cache, as Linux does. The syscall stays a syscall: handing EL0 the cache
   maintenance instructions (`SCTLR_EL1.UCI`) would give every Proc a flush
   primitive; Plan 9's `segflush(2)` is a system call too.
3. **The writer alias is hardened before the JIT first runs** (B-2b). Caveat 5
   above, answered for the browser: the aliases go to independent random
   addresses; the engine's write thunk, with the writer base burned in as
   immediates (the design Apple ships), lives in a **sealed** region, so no
   readable memory names the writer. A sealed region is born that way
   (`SYS_JIT_CREATE_SEALED`, voted 2026-10-07): the kernel copies the thunk's
   bytes in, invalidates the I-cache and maps only an execute-only alias, so
   no writer alias ever exists for it. The kernel's user-copy routines use
   `LDTR`/`STTR`, which check EL0's permissions, so no syscall reads an
   execute-only page for its caller, and the debugger's memory reader refuses
   one too.
   Linux's execute-only user mappings were reverted once because the kernel
   could still read them; OpenBSD ships the same scheme with unprivileged
   user copies.
   The hardening answers an attacker holding a read/write primitive inside
   the process's memory. A Proc's mapping listing (`/proc/<pid>/maps`) names
   every code alias's address, as Linux's `/proc/self/maps` and macOS's
   `vmmap` do, so a content process that can open files finds the writer
   there: B-2c must leave `/proc` out of the web content process's namespace,
   as a renderer sandbox leaves the file system out on other systems. That
   listing is also ambient here -- every Proc may read an unsealed Proc's
   `maps`, Plan 9's posture, sound only while no user address was random --
   so the addresses of its code rows go only to a reader with debug authority
   over the target (the I-39 predicate) and to the target itself, which I-39
   refuses when the target is NOTRACE but whose own layout tells no one else
   anything. Any other reader still sees each code row's
   permissions and type, with its addresses and offset printed as zero,
   listed after the other rows and grouped by permission, so the listing's
   order does not place them in the address space either (voted 2026-10-07,
   B-2b audit F2). Linux gates all of `maps` on `PTRACE_MODE_READ` and zeroes
   the code addresses in `/proc/<pid>/stat` for a reader without it; the rest
   of Thylacine's `maps` stays ambient, because nothing else in it is random
   and the diorama reads a Linux guest's map as itself.
   Debug authority over an image holding a code alias needs `CAP_JIT` even
   when no mapper holds it -- an `RFMEM` child keeps the aliases after their
   creator is reaped, and the address space records the creator's caps when it
   leaves (DEBUG-FS-DESIGN 3.3; `dec-2026-10-08-image-holder-record`, which
   replaced B-2b audit r2's count of code aliases) -- so a reader
   holding no elevation-only cap is never shown a code address. That is what
   keeps the diorama, which reads every pid's `maps` as itself for clients of
   any principal, from becoming a deputy: it refuses to serve while it holds
   one.
4. **The address space's other CPUs take a context-synchronization event on
   request** (B-2e2; `dec-2026-10-13-icache-sync-space`, which supersedes
   `dec-2026-10-13-wasm-space-sync`). `SYS_ICACHE_SYNC` makes new code
   fetchable by every CPU (`IC IVAU` broadcast, then `DSB ISH`), but only the
   calling CPU takes a context-synchronization event (CSE). A peer thread
   already running in the address space may go on executing instructions it
   fetched before the invalidation completed. Wasm's BBQ and OMG publish code
   to such peers with no point at which they pass through C++ (BROWSER-DESIGN
   7, B-2e), so the kernel provides the event. `SYS_ICACHE_SYNC_SPACE` takes no
   arguments and returns 0 once every other CPU that was running a thread of
   the caller's address space has run the barrier interrupt's handler after
   observing a request this call made, and returned from it.

   *Which CPUs.* At every context switch a CPU records the address space whose
   tables it installs (`cpu_space[cpu]`; NULL for a kernel thread, the idle
   thread included), then executes `DSB ISH`. The record is written where the
   switch resolves the ASID (`sched_install_asid_ttbr0`, IRQs masked under
   the run-queue lock) and where `execve` installs a new space
   (`sched_activate_addrspace`). It is compared by value and never
   dereferenced, so a space torn down concurrently is no lifetime hazard; a
   freed space's address reused by a new one can only add an interrupt, never
   lose one. The ASID allocator's per-CPU active slot cannot serve: a rollover
   zeroes it while the CPU keeps running on its reserved ASID. Thylacine's
   switch had an `ISB` after the `TTBR0` write and no `DSB`; Linux's arm64
   `__switch_to` has one, and `membarrier` relies on it.

   *The acknowledgement is a ticket.* Each CPU has a request count and a done
   count. A sender adds one to the target's request count (the new value is
   its ticket), executes `DSB SY` and sends the barrier IPI; the target's
   handler reads its request count and stores that value as done; the sender
   waits until done reaches its ticket. A ticket is therefore served only by
   a handler that observed the request, which the call made after its
   invalidation completed, and the handler's `eret` is a CSE that follows
   that read: a `DSB` sits between them, because a CSE does not by itself
   wait for an earlier load to complete. This is the ARM ARM's idiom for
   code modified while another PE may run it: invalidate, `DSB`, publish a
   flag; observe the flag, then a CSE. The interrupt that served a ticket
   need not have been acknowledged after its send: a handler already running
   for an earlier sender can read the newer request, and that is enough,
   because what the guarantee needs is the observation followed by the CSE,
   not the acknowledgement. The GICv3 acknowledge executes `DSB SY` after
   reading `ICC_IAR1_EL1`, as Linux's `gic_read_iar_common` does, so a
   handler's read cannot be satisfied before its interrupt is acknowledged;
   the GICv2 read already drains. That keeps a ticket from being stranded:
   an SGI merged into one already pending is acknowledged after both sends,
   and its handler's read must see both requests. The kernel's previous
   witness, a per-CPU count of barrier interrupts that `gic_synchronize_cpu`
   compared with the value it read before sending, could be satisfied by an
   earlier sender's handler whose increment landed late, before any handler
   had observed the new request; `gic_synchronize_cpu` moves to tickets with
   this change (it serves the ITS's interrupt drain, which had the same gap).

   *The call.* (1) `DSB ISH`. (2) With IRQs masked: note the calling CPU, read
   `cpu_space[x]` for every other online CPU, and for each that names the
   caller's space take a ticket and send the IPI. (3) With IRQs unmasked, wait
   for every ticket. The calling CPU needs nothing: its thread returns through
   `eret`, and any thread of the space that switches in on it afterwards does
   so after the call's `DSB`, in program order. Masking IRQs across (2) keeps
   "the calling CPU" true while the slots are read.

   *Why it is enough* (`membarrier`'s store-buffering argument). The switch
   stores `cpu_space[x]`, executes `DSB ISH`, and later `eret`s into the
   thread. The call completes the invalidation with its `DSB`, then reads the
   slot. Take any thread of the space that runs on CPU x after the call
   returns, and its last switch-in on x:
   - if the call's read saw that store, x was interrupted, and the
     ticket's handler `eret`ed after observing a request made after the
     invalidation completed;
   - if the read did not see it, the store was not yet visible to every
     observer, so the switcher's `DSB` completed after the read, which came
     after the call's `DSB`: the invalidation completed before the switcher's
     `eret`, and every instruction x fetches for the thread is fetched after
     it.
   A slot naming another space, or NULL, means x switched away; the thread's
   next run on x is a later switch-in, covered the same way.

   *No timeout.* An IPI is taken as soon as its target unmasks IRQs, and the
   kernel's IRQ-masked spans are bounded (I-8 rests on the same property), so
   the wait ends. Plan 9's `procflushseg` and Linux's `smp_call_function`
   wait without a bound too. A timeout returning an error would let a
   descheduled vCPU on a loaded host crash JavaScriptCore, which treats a
   failed sync as fatal: contention would change the outcome, not only the
   duration. The kernel is not preemptible inside a syscall (ARCH 8.12), so
   the wait offers the CPU to other runnable threads between checks
   (`sched_yield_hint`), as `procflushseg` calls `sched()`.

   *No capability.* The call interrupts only CPUs running the caller's own
   threads, so its cost lands on the caller, as with any syscall those threads
   make; it reads no memory and changes no mapping. Linux's `membarrier` is
   unprivileged for the same reason. W^X and the publish (I-42) are
   untouched: the sync publishes nothing a code region's writer had not
   already published through `SYS_ICACHE_SYNC`.

   *The number.* The operator chose a new syscall over a mode of
   `SYS_ICACHE_SYNC` (vote 2026-10-13): that call's callers set only `x0`,
   `x1` and `x8`, so a flags word in `x2` would read garbage, and a zero
   length as the selector was declined. `SYS_ICACHE_SYNC_SPACE` is 128, the
   next native number. Linux's 128 is `restart_syscall`, which the vivarium
   answers with a mandatory `ENOSYS` row before native dispatch, so a
   Linux-phenotype Proc never reaches the sync and a native program
   mis-declared as Linux only loses it; the native ceiling moves to 128, and
   `restart_syscall`'s collision check moves from the ceiling argument to a
   per-number one.

   *Heritage.* Plan 9's `procflushseg` (`port/proc.c`) marks each processor
   whose current process maps the segment and waits until each has taken its
   next clock interrupt; this is that rule with an IPI in place of the tick.
   Linux: `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE)`, which
   interrupts the CPUs running the caller's mm.

   *Witnesses.* Kernel tests: a thread of the space on another CPU is
   interrupted (its ticket is served) and a CPU running another space is not
   (no ticket is taken); the call does not return until a CPU of the space
   holding IRQs masked has unmasked and taken the interrupt; a ticket is not
   served by a handler that read the request count before the ticket was
   taken; the GIC barrier still reaches every live CPU. A thread that switches in during the call needs no IPI by
   the argument above; no test can schedule that window, so the audit checks
   the switch-side `DSB` by reading the code. 128 MiB: `SYS_JIT_CREATE`
   accepts 128 MiB and refuses 128 MiB plus one page.

## Status / handoff

- **NOVEL candidate** -- main agent folds the lead-position framing into
  `docs/NOVEL.md` and the kernel primitive into `docs/ARCHITECTURE.md` when the
  managed-runtime era is in scope.
- Kernel work (the dual-map code-Burrow kind, `SYS_ICACHE_SYNC`, `CAP_JIT`) is
  main-track; aux cannot implement it.
- Motivating use-cases on record: V8 for Node.js / Claude Code (see the
  conversation), and the JS engine inside a Ladybird-based Halcyon browser.
- **The browser use-case largely de-risks itself, so it is NOT the consumer
  that would wake this** (noted 2026-08-13): V8 has a supported jitless mode
  (Ignition), and Ladybird's LibJS is a bytecode interpreter — its JIT was
  abandoned rather than shipped. *(That last is a recollection; verify before
  relying on it.)* So a Halcyon browser is a **performance** decision here, not
  a bring-up blocker. Interpreters run today with zero kernel change.
- Post-v1.0. Not a roadmap item; a design idea banked for when it matters.
  **Dormant queue entry: task #235**, which carries the verified evidence, the
  two gaps above, and the delivery-model fork with a recommendation. Wake it
  when a real consumer lands, not before.
