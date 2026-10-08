# x86 on Thylacine -- the translation layer (design)

> **Status: RATIFIED 2026-10-08.** The operator voted F1-F9 as recommended and
> F10 as G2 with the proposed defaults (`dec-2026-10-08-xt-design`,
> `dec-2026-10-08-xt-guest-code`). The scripture this binds:
> - ARCH section 28: I-48, RESERVED, plus the XT paragraph's amendments;
> - `docs/ERRORS.md`: the exact-fault contract;
> - `specs/SPEC-TO-CODE.md`: spec-first re-enabled for `fault_note` and
>   `hosted_decode`.
>
> Live state is in `docs/xt-status.md`. Arc prefix **XT**. Name: held (section 14).
> Drafted 2026-10-04.
>
> **The 2026-10-08 revision** re-reads the draft against `main`@`04df02c9`, which
> carries B-2: the code Burrow became a lazy reservation at random addresses, a
> sealed execute-only region exists, and debug authority over an image holding a
> code alias now needs `CAP_JIT` cover. It closes two gaps the first draft left
> open:
> - how the entitlement meets debug authority (section 5.2);
> - guest code provenance: a translated program may execute only guest code a
>   native program could (section 5.4 item 4, I-48(e)).
>
> It adds one fork, F10 (guest code generation).
>
> Motivated by `docs/WINE-STUDY.md`, which found every route to x86 software runs
> through an x86 translator and the same handful of missing kernel primitives.
> Operator brief (2026-10-04): x86's fruit is wanted in full, translation should be
> first class the way Rosetta is on macOS, cost does not matter, no bolt-ons --
> invasive redesign is on the table where the proper answer needs it -- and the
> result must stay true to Thylacine's tenets while embracing novel angles aligned
> with its heritage.
>
> Evidence conventions follow the study: `path:line` at `main`@`04df02c9`; outside
> claims tagged **[V]** verified at the source, **[S]** search extract only, **[R]**
> recalled, unconfirmed.

---

## 0. Thesis

x86 software -- Linux amd64 and 386 programs first, Windows programs through Wine
on the same machinery -- runs on Thylacine as **ordinary processes**:
- launched by a plain `exec`;
- granted what translation needs by the system rather than by a prompt;
- sharing translated code the way Plan 9 shares text.

Two things never happen: the kernel never learns x86, and no page is ever writable
and executable.

Seven principles carry the design. Each later section cites the one it serves.

| # | Principle | Tenet it serves |
|---|---|---|
| **P1** | **The kernel never learns x86.** It gains ISA-neutral primitives only: exact faults, shapes, a declared second decode, entitlement, memory-model control. No x86 decoder, syscall table or register file enters the kernel | VISION 3.4: compatibility comes from translation layers, "not by designing the kernel around a foreign ABI" |
| **P2** | **An objtype is a namespace property.** A tree is *declared* to hold `amd64` or `386` programs, Plan 9's `$objtype` made executable. The ELF header corroborates; it never decides | VISION 3.2 (territories); I-43's rule "an ELF byte may corroborate but never decide" (`docs/ARCHITECTURE.md:5194`) |
| **P3** | **The translator is a native citizen.** It is a vouched Thylacine program, entitled by the system, using native facilities directly: code Burrows, notes, 9P, native libraries | VISION 3.6: rigor over expedience |
| **P4** | **Linux semantics live once, in Vivarium.** The translator converts x86 *shape* into Vivarium's shape and never re-implements Linux | single source of truth; I-43 by construction |
| **P5** | **Translated code is text.** It is stored as files, demand-paged, shared through the Image cache and integrity-checked by Stratum. JIT is the fallback, not the path | VISION 3.1 ("everything is a file", 9P composition); REVENANT |
| **P6** | **The translator is not a sandbox; the territory is.** Translation adds no authority and removes none. A guest reaches exactly what its namespace and capabilities allow | I-22, I-23, I-43 |
| **P7** | **Faults are notes, and notes are exact.** Plan 9's `notify`/`noted` contract, in which the handler sees and may edit the interrupted state, is restored and made per-thread | Plan 9 heritage; I-19 |

---

## 1. Goal, scope, non-goals

**Goal.** Run x86-64 and i386 programs on Thylacine as first-class processes. That
means:
- launched by `exec` from any shell or `posix_spawn`;
- visible in `/proc`;
- their translations persisted and shared;
- performance bounded by the hardware, not by plumbing.

**In scope.**
- The kernel primitives (section 5).
- The native translator runtime for Linux guests (section 6).
- The translation store (section 7).
- The library bridges that give translated programs native graphics and audio
  (section 6.4).
- The hooks Wine's in-process translator uses (section 6.5).

**Out of scope (separate arcs, listed as dependencies).**
- Wine itself (`docs/WINE-STUDY.md`, Route B).
- Vivarium completeness work that is not specific to translation: AF_UNIX, epoll,
  `memfd`/shared maps, glibc breadth.
- DOSBox-X, which emulates a whole machine and keeps its own dynrec.

**Non-goals.**
- A Thylacine-native x86 ABI: nobody will build "amd64 Thylacine" programs, so the
  only x86 personalities are Linux and Windows.
- x86 anywhere in the kernel.
- Translation as a security boundary (P6).

---

## 2. Heritage and the state of the art

### 2.1 Plan 9 (the heritage)

- **`$objtype` and per-architecture trees.** A Plan 9 file server carries `/386/bin`,
  `/amd64/bin`, `/arm/bin` side by side, and a namespace `bind`s the right one over
  `/bin` [R]. Architecture is a property of *where you look*, not of a global
  registry. This is the model P2 adopts unchanged.
- **`cpu(1)`.** To run a program built for another architecture, Plan 9 moves the
  *process* to a CPU server of that architecture and exports the caller's namespace
  to it over 9P [R]. The translator is the local form of the same idea: a virtual
  CPU of another objtype, running inside your own namespace.
- **`#!` interpreters.** `exec` already loads a different image from the one named
  when the file says so [R]. Thylacine's DISTRO D-4 is the modern form: "the kernel
  loads exactly ONE image per exec ... What changes is WHICH image"
  (`kernel/exec.c:1128-1130`).
- **`notify`/`noted` with a Ureg.**
  - The handler receives the interrupted registers. 9front's `notify(2)`: the Ureg
    "is provided to help recover from traps such as floating point exceptions" [V].
  - `notejmp` "modif[ies] the saved state" and then calls `noted(NCONT)` [V]. So a
    handler that edits the state it resumes into is the heritage contract.
  - Thylacine's `NCONT` currently restores a kernel-side copy and ignores edits
    (`kernel/notes.c:2011-2024`). P7 restores the heritage.
- **Shared text.** Plan 9 shares text segments among processes running the same
  image. Thylacine's Image cache is that idea, qid-keyed
  (`docs/EXEC-LOAD-DESIGN.md` section 4.4). P5 extends it to translated text.

### 2.2 Translation, as others ship it

| System | Mechanism | Lesson for us |
|---|---|---|
| **Rosetta 2** (macOS) [R] | The kernel recognises x86 Mach-O and loads the runtime. An install-time AOT service (`oahd`) caches translations; a runtime JIT handles the rest. Translated threads get hardware TSO and 4K pages. Every system library ships an x86 slice, so the process is homogeneous | "First class" is **OS integration** (exec, AOT cache, hardware memory model), not a clever JIT. The homogeneous-process trick is unavailable to us: no x86 build of Thylacine's native interface exists |
| **Windows on Arm** [R; the interface is V] | The emulator is a DLL behind the `BTCpu*` interface (`xtajit`, `xtajit64`; Wine's `dlls/wow64/syscall.c` implements the same interface [V]). A system service (XtaCache) persists translations. ARM64EC lets native and emulated code share a process | The translator lives *inside the personality*. Persistent translation is a *system service* |
| **FEX** [V] | A JIT plus a Linux frontend translating x86 Linux syscalls into the host's aarch64 Linux syscalls, and Windows frontends (ARM64EC, WoW64). Host "thunks" forward GL, Vulkan and other libraries to native implementations. Since FEX-2609 (2026-09) it has a disk code cache [S] | One engine core, several frontends. Thunks make graphics native. Its own source names a dual-mapped RW/RX mirror as the W^X answer [V], and Madeira's fork implements one [V] |
| **Box64** [V] | Dynarec plus *wrapping* of native host libraries by name; `wowbox64` for Wine; tuned for ARMv8.0 (a Pi 4 target); RWX code cache, no W^X mode | A viable second engine for v8.0 hardware. Library wrapping is the thunk idea taken to the limit |
| **qemu-user + binfmt_misc** (Linux) [R] | The kernel matches a file's magic and runs a registered interpreter | Interpreter-by-magic is the classic shape. A global table keyed by sniffing is the part to reject (P2) |
| **FreeBSD Linuxulator** [R] | One in-kernel Linux personality with a second table for 32-bit Linux (`linux32`) | Per-ISA tables in the kernel are the alternative P1 rejects (section 10, F1) |

### 2.3 Capability-OS state of the art

- **Fuchsia: code authority is a resource.** Making memory executable needs
  `zx_vmo_replace_as_executable` with the VMEX resource, handed to specific
  components. ARCH cites it as the precedent for `CAP_JIT` (`docs/ARCHITECTURE.md:558`).
- **Fuchsia Starnix** runs Linux binaries in userspace but does no ISA translation
  [R].
- **macOS hardened runtime: entitlements are image-bound authority.** A binary may
  JIT only if its code signature carries `com.apple.security.cs.allow-jit` [R].
  Authority attaches to *what is running*, not to who runs it. Section 5.2 takes
  this shape without code signing: Stratum's content addressing already gives
  images an identity.
- **Microsoft Edge (2017): an out-of-process JIT** writes code into the content
  process, so that process can enable Arbitrary Code Guard [R]. This is the
  strongest W^X posture, weighed and rejected in section 10, F3.

### 2.4 What the research settles

1. **Exec integration plus a persistent, shared translation cache plus a
   hardware-aware memory model** is what every first-class translator has. All
   three are OS work.
2. **The translator belongs inside the personality** (Rosetta's runtime, Windows'
   `xtajit`, FEX's frontends), never in the kernel.
3. **Code-emission authority is image-bound** in the systems that grant it
   implicitly (macOS entitlements, Fuchsia's VMEX).
4. **Plan 9 already has the vocabulary:** objtype trees, `#!`, shared text, a Ureg
   handed to the handler. The design is mostly heritage with one genuinely new organ
   (section 5.4).

---

## 3. Ground truth (as built, cited)

The full survey is `docs/WINE-STUDY.md` section 2. The facts this design moves:

- **ELF.** The loader accepts only `EM_AARCH64` (`kernel/elf.c:89`).
- **Phenotype.** Decided at *every* image load from the namespace: an `MPHENO_LINUX`
  mount crossing or the Territory's `root_pheno`. ELF bytes never decide
  (`docs/VIVARIUM.md` sections 12.1 and 13.10).
- **Layout.**
  - Main stack `[0x7F80_0000, 0x8000_0000)` (`kernel/include/thylacine/exec.h:87-88`).
  - PIE base `0x2000_0000` (`kernel/include/thylacine/elf.h:143`).
  - vDSO clock page `0xC000_0000`.
  - Every runtime mapping lives in `[4 GiB, 64 TiB)`, and FIXED below it is refused
    (`kernel/include/thylacine/exec.h:110-111`, `kernel/syscall.c:6465`).
- **The three doors to X:** exec, a provenance-vouched file map, and
  `SYS_JIT_CREATE`. Only `devramfs` and `dev9p` may back executable bytes
  (`docs/ARCHITECTURE.md:546-558`).
- **JIT** (as reshaped by B-2, `docs/ARCHITECTURE.md:595`).
  - A code Burrow is a lazy reservation of at most 64 MiB, dual-mapped RW/RX. A
    page is committed, zeroed, I-cache-invalidated and charged on its first touch
    through either alias (`kernel/include/thylacine/syscall.h:2542`).
  - The two aliases sit at independent random addresses. A *sealed* region
    (`SYS_JIT_CREATE_SEALED`) is born with only an execute-only alias.
  - User copies use `LDTR`/`STTR`, so no syscall reads an execute-only page for
    its caller. EL0 cache maintenance stays off: `SYS_ICACHE_SYNC` is the only
    publish path (`docs/JIT-ON-WX-DESIGN.md`, "B-2").
  - `/proc/<pid>/maps` shows a code alias's addresses only to the target and to a
    reader with debug authority over it (`docs/ARCHITECTURE.md:5072`).
  - **Debug authority over an image holding a code alias needs `CAP_JIT` cover**,
    because a debugger may write a stopped target's data memory, which includes
    the writer alias. I-39's image join ORs `CAP_JIT` into an address space's caps
    while it holds any code alias (`docs/DEBUG-FS-DESIGN.md:595-607`).
  - `CAP_JIT` is elevation-only and comes from the corvus `jit` clearance, which
    needs re-authentication and does not propagate
    (`usr/corvus/src/main.rs:1285-1301`).
- **Notes.**
  - fd-shaped first; the async handler path receives only a name and an argument
    (`kernel/include/thylacine/notes.h:1-40`).
  - Every EL0 fault terminates the Proc (`arch/arm64/exception.c:429-634`).
  - `NCONT` ignores handler edits (`kernel/notes.c:2011-2024`).
  - No alternate stack; no thread-directed posting
    (`kernel/include/thylacine/syscall.h:886-914`).
- **Threads.** 256 per Proc, counted over the Proc's lifetime: an exited thread
  is freed only when the whole Proc is reaped (`kernel/proc.c:1200`, `:6075-6082`;
  `docs/WINE-STUDY.md` Appendix A, F3).
- **Feature discovery.** A trapped `mrs` of an ID register kills the Proc
  (`arch/arm64/exception.c:630-634`); only `AT_HWCAP`, no `AT_HWCAP2`.
- **Hardware.** The userspace floor is ARMv8.0 (`tools/check-v80-floor.py`). The
  permanent hardware box is a Pi 400 (Cortex-A72: no LSE, no RCpc;
  `docs/agent/THYLA-PI.md:6-10`). The dev loop is QEMU/HVF on an M2.
- **Already right for this design.**
  - 4 KiB pages; a 47-bit user range.
  - x18 preserved across exceptions.
  - B-1d `dlopen` (`docs/ARCHITECTURE.md:578`).
  - The Image cache.
  - Pouch's C++20 runtime (`docs/LLVM-DESIGN.md:202-206`).

---

## 4. Architecture

### 4.1 Layers

```
  ┌───────────────────────────── a translated Proc ─────────────────────────────┐
  │  guest x86 code + guest libs (from the amd64 tree)  ── never host-executable │
  │        │  x86 syscall                    │ call into a bridged library       │
  │        v                                 v                                   │
  │  translator runtime (native, entitled) ── bridges: native SDL2/Tapestry,     │
  │   engine (FEX core) + Thylacine host layer    Venus Vulkan, OSMesa GL, audio  │
  │        │ x86->aarch64-Linux shape            │ native calls                  │
  │        v                                     v                               │
  │  svc #GUEST (hosted decode)              svc #0 (native decode)              │
  └────────┼─────────────────────────────────────┼───────────────────────────────┘
           v                                     v
  ┌──────────────────────────── kernel (ISA-neutral) ───────────────────────────┐
  │ Vivarium Linux semantics      native syscalls       exact faults (notes)     │
  │ (guest partition only)        (runtime partition)   shapes · entitlement     │
  │                                                     memory-model control     │
  └──────────────────────────────────────────────────────────────────────────────┘
           ^ translated text (RX, shared, Merkle-verified) from the store (Stratum files)
```

### 4.2 The life of an x86 process

1. **Exec.** The user runs `/amd64/bin/ls`. The path crosses a mount declared
   `objtype amd64` + `pheno linux` (P2). `exec` opens the file and checks it
   exactly as a native exec would (the X bit, the vouching rule). The ELF header
   says `EM_X86_64`, which corroborates the declaration.
2. **The objtype table.** The kernel consults the system objtype table, which maps
   `(linux, amd64)` to: translator image `/lib/xt/amd64` (vouched), shape `foreign`,
   entitlement `code-emit`.
3. **Load.** The kernel loads the translator as *the* image, as D-4 does for
   `PT_INTERP`. It hands the translator an **open handle** to the guest file it
   resolved (auxv), so the translator does not re-walk the path (no TOCTOU). The
   guest's bytes are never mapped executable.
4. **Map the guest.** The translator maps the guest's segments R or RW in the guest
   arena, loads the guest's own interpreter (if any) from the guest's view of the
   namespace, and starts executing.
5. **Run.** Code found in the store maps RX from Stratum and is shared with every
   other process running that image. Code not in the store is JIT-compiled into the
   Proc's code Burrows and may be appended to the user's store.
6. **Syscalls.** A guest `syscall` is converted from x86-64 Linux shape to
   aarch64-Linux shape and issued with `svc #GUEST`. Vivarium serves it against
   the guest partition. The runtime's own work uses native `svc #0`.
7. **Faults.** Self-modifying code, guard pages and guest `SIGSEGV`s become exact
   notes delivered to the faulting thread with a full Ureg. The runtime edits the
   Ureg and resumes, or synthesises an x86 signal frame for the guest.
8. **fork / exec / exit.**
   - **fork** keeps the image, so the child keeps the entitlement.
   - **execve** re-decides objtype and phenotype from the namespace, as Design D
     does. An x86 target reloads the translator; an aarch64 Linux target becomes a
     plain Linux-phenotype Proc; a native target becomes native.
   - **exit** is ordinary group termination (I-24).

### 4.3 Where each concern lives

| Concern | Home | Why |
|---|---|---|
| x86 decoding, translation, flags, x87/SSE/AVX state, CPUID, RDTSC, FS/GS bases, segments | translator runtime | P1 |
| x86-64 and i386 Linux ABI *shape* (syscall numbers, struct layouts, signal frames, `arch_prctl`) | translator runtime (FEX's Linux layer) | P1, P4 |
| Linux *semantics* (files, sockets, futex, processes, `/proc` contents) | Vivarium, reached through the hosted decode | P4: one implementation, shared with aarch64 Linux binaries |
| Guest signal dispositions and frame synthesis | translator runtime | Vivarium's frames are aarch64-shaped; only the runtime knows x86 frames |
| Fault capture, delivery, resume | kernel (exact notes, section 5.5) | P7, ISA-neutral |
| Objtype declaration, image choice, shape, entitlement | kernel exec + namespace | P2, P3 |
| Persisted translations | Stratum files written by the runtime (per user) or a TCB producer (system trees) | P5 |
| Graphics, audio, input for guests | native Thylacine libraries reached through bridges | P3: no Linux display stack needed |
| Isolation | territory and capabilities | P6 |

---

## 5. Kernel primitives (the redesigns)

Nine changes. Each is ISA-neutral, so each also benefits non-x86 consumers
(named per item).

### 5.1 XT-K1 -- objtype declaration and foreign exec

- **Declaration channels.** These mirror Vivarium's two phenotype channels exactly
  (Design D, `docs/VIVARIUM.md` section 13.10).
  - **By location:** a mount marked with an objtype, `MOBJ_AMD64` or `MOBJ_386`.
    The natural place is the Plan 9 tree `/amd64` and `/386`. An x86 tree mount
    carries the personality flag too (`MPHENO_LINUX`).
  - **By container:** `Territory.root_objtype`, set from a manifest annotation
    (`org.thylacine.objtype: amd64`), next to `root_pheno`.
  - **Fail-safe:** with no declaration the objtype is native, and an x86 ELF fails
    with today's `ELF_LOAD_BAD_MACHINE`.
- **Corroboration.** `e_machine` must equal the declared objtype (`EM_X86_64` for
  amd64, `EM_386` for 386); a mismatch refuses the exec. The header confirms the
  declaration and never chooses.
- **The guest file passes exec's own checks.** Before substituting the translator,
  foreign exec applies to the guest file every check a native exec applies to its
  image: the X bit, the `may_back_exec` floor, and a mount not marked `MNOEXEC`
  (the vouching rule, `docs/ARCHITECTURE.md:546-552`). A file a native exec would
  refuse is refused here too: translation is never a way around `noexec`
  (I-48(e), section 5.4 item 4).
- **The objtype table.** A small kernel table:
  `(personality, objtype) -> {translator image, shape, entitlements}`.
  - It is loaded at boot from a system file (Plan 9 `/lib` idiom, e.g.
    `/lib/objtype`) by a TCB process through a ctl file.
  - Changing it needs host-owner authority.
  - The translator image must resolve on a vouched mount (`docs/ARCHITECTURE.md:546-552`).
    Its identity is pinned by qid version, so a swapped file is not the registered
    image.
- **Foreign exec.** A D-4-shaped image substitution:
  - the kernel loads the translator as the single image;
  - argv is the guest's, untouched;
  - auxv gains `AT_GUEST_IMAGE` (a read-only handle to the resolved guest file),
    `AT_GUEST_OBJTYPE` and `AT_GUEST_PHDR`-class hints parsed from the header the
    kernel already read.
  - The kernel never maps guest segments and never processes a guest `PT_INTERP`;
    both are the translator's job.
- **Also unblocks.** Wine as the registered interpreter for PE objtypes
  (`./game.exe` from the shell). DOSBox-X for DOS programs, optionally.

### 5.2 XT-K2 -- image entitlement for code emission (amends I-42)

**Rule.** `SYS_JIT_CREATE` succeeds iff the Proc holds `CAP_JIT` **or** its *loaded
image* carries the `code-emit` entitlement from the objtype table.
- The entitlement is stamped at every image load and cleared when another image
  loads. That is Design D's rule, applied to entitlement.
- `rfork` preserves it, because the child runs the same image.
- It is **not a capability**: it is never in `caps`, never delegated, never
  inherited across exec. I-2 is untouched.

**Why it is sound.**
- `CAP_JIT`'s only power is emitting code into the caller's *own* Proc. Code
  Burrows are refused by `burrow_share_into` (`usr/corvus/src/main.rs:1397-1415`;
  I-42). A guest that steers its translator into emitting arbitrary host code gains
  nothing its own syscalls did not already give it (P6).
- I-42's purpose is that code emission be "an explicit, bounded, auditable act"
  (`usr/corvus/src/main.rs:1294`). Here the operator performs that act once per
  translator image, in a host-owned table. It is bounded per image, auditable in
  one file, and needs no prompt per process.

**Precedent.** macOS's image-bound `allow-jit` entitlement; Fuchsia's VMEX handed to
named components. Thylacine needs no signature machinery, because a pinned qid on a
vouched mount *is* the identity.

**Debug authority (I-39, after B-2).** A host debugger may write a stopped target's
data memory, and a translator's writer alias is data memory. Three rules follow.
- **The entitlement counts as `CAP_JIT` in I-39's image join**, from the moment it
  is stamped and before any alias exists. B-2b already counts each code alias that
  way (`docs/DEBUG-FS-DESIGN.md:595-607`). Attaching a host debugger to a
  translated Proc therefore needs `CAP_JIT` cover, or the `CAP_DEBUG`/
  `CAP_HOSTOWNER` axis. A birth-held translator (`SPAWN_DEBUG_HELD`) offers no
  window before its first alias.
- **No entitlement is stamped on a load by a debug-tainted Proc**, by I-39's rule
  that a debugged image never elevates. That load gets the translator without code
  emission: it runs what the store holds (and an interpreter, where the engine has
  one), or `SYS_JIT_CREATE` fails it cleanly with `EACCES`.
- **Guest-level debugging is the runtime's own** and needs none of this: x86
  registers, guest memory, breakpoints placed by retranslation. It is a designed
  extension, not built in XT. The runtime would serve each guest as a per-objtype
  debug view, the shape of acid's `$objtype`-selected machine tables [R].

B-2b's writer hardening (random aliases, a sealed write thunk) is open to the
runtime, but buys little here. Under P6 a guest can reach the runtime's memory
through translated stores anyway.

**Also unblocks.** DOSBox-X's dynrec (today it activates the corvus clearance), and
Wine's loader when it hosts FEX.

### 5.3 XT-K3 -- address-space shapes

**The refactor.** Exec's layout constants become a **shape** object selected at image
load. The objtype table names it; native Procs get `native`.
- **Native shape: today's layout, unchanged,** plus a hook where user ASLR lands
  later (`docs/ARCHITECTURE.md` notes it "lands with the loader-randomization work").
- **Foreign shape:**
  - the runtime's image, stacks, vDSO page and runtime arena all sit above 4 GiB;
  - the **guest arena** is `[64 KiB, 2^47)` minus the runtime arena;
  - `[64 KiB, 4 GiB)` is open to FIXED reservations;
  - `[0, 64 KiB)` is permanently unmapped.
- **What it carries.** That one shape serves:
  - non-PIE x86-64 binaries at `0x400000`;
  - every 386 guest (all below 4 GiB);
  - and Wine (`KUSER_SHARED_DATA` at `0x7ffe0000`, PE preferred bases, WoW64).
- **The FIXED rule moves into the shape.** `kernel/syscall.c:6465`'s window check
  becomes "inside the Proc's shape's FIXED-able range".

**Why a shape and not a global change.** The native layout keeps its guard
properties and its future ASLR, and the foreign layout is ABI *shape*. That makes it
the I-43 kind of thing: declared by the namespace, conferring no authority.

**Also unblocks.** Vivarium's `MAP_FIXED` below 4 GiB, and Wine (study K3).

### 5.4 XT-K4 -- hosted Procs (the new organ)

A *hosted* Proc runs a native runtime that drives a declared guest personality. It
needs three things no Thylacine Proc has had.

1. **A declared second decode.**
   - The AArch64 `svc #imm16` immediate reaches the kernel in `ESR_EL1.ISS`.
   - `svc #0` keeps today's per-Proc decode: native for the runtime.
   - `svc #GUEST` (one fixed immediate) selects the guest personality's decode.
     For x86 Linux guests that is Vivarium's aarch64-Linux table.
   - A Proc with no declared guest personality gets `snare:ill` for `svc #GUEST`.
   - Precedent: Windows on ARM64 carries the service number in the SVC immediate
     [R]; 32-bit ARM Linux's old ABI did the same [R].
   - I-43 holds by construction: both decodes land on the same native cores and gates.
2. **Partitions.**
   - **Handles.** Every handle-table entry is tagged *runtime* or *guest*. The guest
     decode allocates and resolves only guest entries, so Linux fd numbers index
     the guest partition. A guest that closes fds 3-1023, as daemons do, cannot
     close the runtime's store connection or note fd.
   - **Memory.** Guest-decode memory calls (`mmap`, `munmap`, `mprotect`,
     `madvise`) act only inside the guest arena.
   - **What they are not.** The partitions police guest *syscalls*, not translated
     loads and stores: that is P6, and the docs say so rather than imply otherwise.
   - FEX on Linux protects its own fds and maps by convention [R]. Here the kernel
     does it.
3. **Note routing.**
   - A hosted Proc's notes all reach the runtime's native handler (section 5.5):
     synchronous faults, and asynchronous notes including Linux `kill`/`tgkill`
     mapped by Vivarium. Each note carries the Linux signal number when it came
     from one.
   - Vivarium's aarch64 signal-frame delivery is not used for hosted Procs. The
     runtime owns the guest's dispositions and synthesises x86 frames.
   - The runtime mirrors each guest thread's mask into that thread's native note
     mask, so Vivarium's EINTR rules (VIV-EINTR) still decide which guest blocking
     calls a note interrupts.
4. **Guest code provenance (proposed I-48(e)).** The runtime translates only
   *guest-executable* memory. Guest memory becomes guest-executable only on the
   terms native code would:
   - **The guest image**, which foreign exec has already vouched (section 5.1).
   - **A guest file map with `PROT_EXEC`**: the guest's `ld.so` loading a
     library, or Wine mapping a DLL. The guest decode applies the vouching rule as
     Vivarium's native arm does, and records *guest-X* on the mapping. Host X is
     never set.
   - **Anonymous or written guest memory made executable**: `mprotect(PROT_EXEC)`,
     `VirtualProtect`, an x86 JIT, a packer, self-modifying code. That is code
     generation, so it needs code authority: `CAP_JIT`, or what fork F10 decides.

   A jump to memory that is not guest-executable raises the guest's own fault
   (`SIGSEGV`, `STATUS_ACCESS_VIOLATION`). That is how the runtime emulates NX.

   **The split.** The kernel owns the checks and the guest-X marking. The runtime
   is the only thing that fetches guest code, so it owns the honouring, and it is in
   the TCB for that as the loader is for native code. Without this rule, `MNOEXEC`
   and I-42 would hold for every program except translated ones.

**Threads.** A guest `clone(CLONE_THREAD)` goes out through the guest decode with a
*runtime* entry point and host stack, which Vivarium's N-3 row already accepts.
The new Thread joins the guest's thread group and runs runtime code first. `fork`
and `execve` go through the guest decode too, and re-decide objtype per section 5.1.

**A fork carries text, not scratch.**
- Today a fork of an address space holding a code region fails whole. The clone
  classifier sends `BURROW_TYPE_CODE` to its refusing arm (`kernel/addrspace.c:204-208`),
  because a pair of aliases over one region has no copy-on-write.
- XT adds a creation flag, *don't-fork*, set at `SYS_JIT_CREATE`: the shape of
  Linux's `MADV_DONTFORK` [R]. A fork omits such a region. The child is born
  holding none, and its runtime rebuilds its JIT state.
- Store text is file-backed, so the fork shares it as it shares all text
  (`kernel/addrspace.c:177-183`). A fork child therefore mostly maps what its
  parent translated, instead of compiling it again.
- A region created without the flag keeps today's refusal.
- The child keeps the entitlement, because it runs the same image.

**Why this is the proper answer.** The alternative is a translator that is itself a
Linux-phenotype program (section 10, F1, option A1). That translator could reach
Thylacine only through Linux-shaped side doors, could load only Linux-ABI libraries
(so no native display bridge), and would depend on Vivarium's fidelity for its *own*
correctness. A hosted Proc lets the translator be native (P3) while Linux stays
single-homed (P4).

### 5.5 XT-K5 -- exact faults (the study's K1, done as Plan 9 did it)

- **Thread-directed.** A synchronous fault note (`snare:segv/bus/align/ill/brk`, and
  `snare:fpe` once emitted) goes to the faulting thread and never enters the
  per-Proc queue's fd path. The thread retires no further EL0 instruction until the
  note is handled or its default runs.
- **The Ureg.** The handler gets `x0` = name, `x1` = argument, and `x2` = a pointer
  to a **Ureg** on the note stack. The Ureg holds:
  - x0-x30, sp, pc, pstate;
  - FPSR, FPCR and v0-v31;
  - the fault address (FAR) and syndrome (ESR), plus a decoded fault kind
    (read, write, exec, permission, translation, alignment).
- **Editable resume (the `notejmp` contract).**
  - `noted(NCONT)` copies the Ureg back in, validates it, and resumes there.
  - Validation: pstate must be EL0t AArch64 with only user-writable flag bits; pc
    must be canonical, aligned and in user space. A Ureg that fails validation, or
    cannot be read back, takes the note's default action (fail closed).
  - An unedited Ureg reproduces today's behaviour exactly.
- **Alternate note stack.** `SYS_NOTE_STACK` sets one per thread (the
  `sigaltstack` analogue), so guard-page and stack-overflow faults are handleable.
- **Nesting.** The heritage rule: a synchronous fault *inside* a handler terminates
  the Proc. Both FEX and Wine finish their real work after returning from the
  handler, so neither needs nesting.
- **Linux frames.** For plain Linux-phenotype Procs (not hosted), Vivarium's
  `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGTRAP`/`SIGFPE` delivery carries:
  - `si_addr` and `si_code`;
  - a `ucontext` with `fpsimd_context` and `esr_context` records;
  - and `rt_sigreturn` that honours (validated) `uc_mcontext` edits.

  That is the honest Linux contract `docs/VIVARIUM.md` section 5.4 wants.
- **Designed extension.** An fd-shaped, out-of-thread fault channel for debuggers
  and supervisors. It is the Fuchsia exception-channel shape in Thylacine's
  "notes are fd-shaped first" dress (`kernel/include/thylacine/notes.h:4-13`), and
  is not built in XT.
- **Invariant work.** A new I-19 sub-invariant **N-6**: a synchronous fault note is
  delivered once, to the faulting thread only, before that thread retires another
  EL0 instruction, and execution resumes exactly at the validated returned context
  or the default runs. It interacts with `kill` (N-4), `in_handler` (N-3) and group
  termination (I-24).
- **Also unblocks.** OpenSSL's `SIGILL` probe, GC write barriers, Wine's SEH and
  guard pages, Box64, DOSBox-X's guest-facing `SIGSEGV`.

### 5.6 XT-K6 -- thread-directed notes

- **`SYS_POSTNOTE_THREAD(pid, tid, name, arg)`.**
  - Within one's own Proc, it is unrestricted.
  - Across Procs it uses I-26's two-axis rule (owner, or
    `CAP_HOSTOWNER`/`CAP_KILL`) for every note, which also lifts the current
    "parent or self" limit on cross-Proc posting.
- **User note names.** A user-definable family (`usr:*`), so Linux's `USR1`,
  `USR2`, `ALRM` and `TERM` have something to ride.
- **Also unblocks.** Wine's thread suspension, Vivarium's `tgkill`, Pouch's
  `pthread_kill`.

### 5.7 XT-K7 -- thread memory model

- **The primitive.** `SYS_THREAD_MEMMODEL(set | get, TSO | DEFAULT)`.
  - Where the CPU has a hardware TSO mode (Apple silicon's per-thread
    `ACTLR_EL1` bit [R]), the kernel tracks a per-thread flag and switches it on
    context switch.
  - Elsewhere it returns `ENOTSUP` and never pretends (proposed I-48(d), section 9).
- **The fallback.** The translator falls back to software TSO, as FEX does by
  default [V].
- **To measure first.** Whether QEMU's HVF backend lets a guest kernel set the bit.
  Apple documents per-thread TSO for Linux guests of its own Virtualization
  framework [S], but QEMU uses the lower-level Hypervisor framework.
- **Why it matters.** Hardware TSO is the largest single reason Rosetta is fast.
  On the M2 dev host it may be available to Thylacine; on the Pi 400 it is not.

### 5.8 XT-K8 -- feature discovery, and XT-K9 -- thread reaping

- **XT-K8.**
  - Emulate EL0 `mrs` of the ID registers (`ID_AA64*`, sanitised `MIDR_EL1`),
    following Linux's documented `HWCAP_CPUID` ABI [R].
  - Let EL0 read `CTR_EL0` directly (`SCTLR_EL1.UCT`). That is a read of cache
    geometry only; the maintenance instructions (`SCTLR_EL1.UCI`) stay off, per B-2a.
  - Add `AT_HWCAP2`.
  - Fix the EL2-entry `nTWE`/`nTWI` defect while in the same register
    (`docs/WINE-STUDY.md` Appendix A, F2). The EL2 path writes `0x30D00800`
    (`arch/arm64/start.S:130-134`), and the MMU enable only ORs in M, C and I
    (`arch/arm64/mmu.c:767`). So `SCTLR_EL1`'s EL0 controls are whatever the entry
    path left, and the two boot paths differ. The fix composes one explicit
    value, as Linux's `INIT_SCTLR_EL1_MMU_ON` does [R].
- **XT-K9.**
  - Reap exited threads individually, so `PROC_THREAD_MAX` counts live threads
    (study F3).
  - A translated program inherits its guest's thread churn, and Windows thread
    pools churn constantly.

---

## 6. The translator runtime

### 6.1 Engine

**FEX is the primary engine.**
- One core serves both the Linux and the Windows frontends, so Linux x86 and Wine
  share the work.
- It emulates TSO by default, which is correct for multithreaded games.
- Valve ships it in Arm Proton, with the disk code cache enabled since 2026-09 [S].

**Box64 is the evaluated alternative for ARMv8.0 hardware:**
- FEX's proposed ARMv8.4 floor (issue #4120, open) would drop every Pi model [V].
- Box64 keeps an A72 target [V].

The kernel is engine-agnostic: the objtype table names an image. Whether an engine
choice is per objtype, per host or per program is decided in XT-12.

**The upstream constraint, stated.** FEX does not accept AI-generated contributions,
which is why Madeira's patches stay downstream [V]. Thylacine's changes will be a
maintained fork. Mitigation: confine them to a **Thylacine host layer**, a frontend
like FEX's own Windows ones, and keep core patches minimal and enumerated.

### 6.2 FEX on Thylacine

- **Build.** Native Pouch C++20, as a dynamic PIE (B-1d) so it can load bridge
  libraries. Installed at `/lib/xt/` on a vouched mount.
- **Code buffers.**
  - Each FEX code buffer is a code Burrow. Emission goes through the writer alias;
    execution, dispatch and block linking use the exec alias; patches are published
    with `SYS_ICACHE_SYNC`.
  - This is the pattern FEX's own source names and Madeira proved [V], and the one
    DOSBox-X's DX-4 port already uses here.
  - `JIT_REGION_MAX` (64 MiB) against FEX's 128 MB buffers means two regions or a
    revised cap (XT-12 measures). B-2a's lazy regions suit FEX: an untouched
    reservation costs nothing.
  - FEX's own cache maintenance (its clear-cache loop) becomes `SYS_ICACHE_SYNC`,
    since EL0 cache maintenance stays off (B-2a).
  - Its code regions are created *don't-fork* (section 5.4, "A fork carries text,
    not scratch"). Its post-fork child path discards its block cache and rebuilds
    the dispatcher before it returns to the guest.
- **Memory.** The runtime's own allocations are native burrows in the runtime
  arena. Guest allocations go through the guest decode into the guest arena.
- **Faults.** FEX's signal delegator is rebased onto exact notes (section 5.5).
  Self-modifying-code tracking write-protects guest code pages, catches the fault,
  invalidates and resumes, the same `mtrack` shape FEX uses on Linux [V].
- **Linux layer.** FEX's x86-64/i386-to-aarch64 syscall conversion is kept and its
  output retargeted to `svc #GUEST`.
  - Guest `rt_sigaction` and frame synthesis stay in FEX.
  - Guest `clone` uses the hosted thread path (section 5.4).
  - x86 glibc guests are therefore *easier* than aarch64 glibc binaries under plain
    Vivarium. FEX owns their signal frames and thread creation, so Vivarium's
    `SA_RESTORER` and clone flag-word limits (`kernel/vivarium.c:3230-3245`,
    `kernel/include/thylacine/vivarium.h:2402-2435`)
    never reach the guest. What remains is the aarch64-Linux calls FEX forwards,
    `FUTEX_WAIT_BITSET` among them.
- **FEXServer** (rootfs mounting, shared state) is not ported. The territory *is*
  the rootfs (section 6.3), and the store (section 7) is the shared state.

### 6.3 The amd64 territory

- **Layout.** An x86 environment is a territory, Plan 9 style:
  - the amd64 tree (an x86-64 Alpine or Debian rootfs, staged like the existing
    Alpine one) is mounted with `MOBJ_AMD64 | MPHENO_LINUX`;
  - `/proc` and `/sys` come from the diorama;
  - the runtime needs nothing from the guest's `/lib`, because it is a native
    program with its own libraries on a vouched mount.
- **Entry points.**
  - A shell command `amd64 [cmd]` builds the territory and runs `cmd` in it, the
    `viv` runner's shape.
  - Or the container manifest declares `org.thylacine.objtype: amd64`.
  - Or a user binds the tree's `/amd64/bin` after `/bin` and runs x86 tools
    side by side with native ones. Exec decides per binary, from where it was found.

### 6.4 Bridges (native libraries for guests)

- **The mechanism.** FEX's thunks: guest-side x86 libraries whose functions marshal
  into host-side libraries [R].
- **On Thylacine, the host side is native.** It is Pouch `.so`s loaded by B-1d
  `dlopen`, calling:
  - **SDL2** with the Tapestry backend (`usr/ports/sdl2/thylacine/`);
  - **Vulkan** through Venus, statically linked as today;
  - **GL** through OSMesa;
  - **audio** through SDL to Nocturne.
- **What it buys.** An x86 Linux SDL game reaches the screen through *native* SDL,
  with no Linux display stack and no Wayland bridge. That was the decisive argument
  for a native runtime (section 10, F1).
- **Callbacks.** Guest functions called by host libraries (audio callbacks, debug
  callbacks) use FEX's host-to-guest trampolines.
- **Coverage.** Bridge libraries are per API and generated from headers (FEX's
  thunk generator). Coverage is a list, grown per title.

### 6.5 Windows (Wine's in-process translator)

FEX's ARM64EC and WoW64 DLLs (`libarm64ecfex.dll`, `libwow64fex.dll`) run inside
native Wine (`docs/WINE-STUDY.md` Route B). They reach the same primitives:
- **Entitlement** goes to the Wine loader image, registered for PE objtypes.
- **Shape:** the foreign shape (KUSER, WoW64).
- **Faults:** exact notes, through Wine's `signal_arm64` port.
- **FEX's unixlib** (eight calls since FEX-2609 [V]) is a native `.so`, so its
  "hardware TSO" and "map file" calls map straight onto XT-K7 and code Burrows.

The Wine arc owns the details. This design guarantees that nothing in it is Linux-only.

---

## 7. The translation store

### 7.1 Shape: translations are files

The store is a directory tree on Stratum, not a daemon protocol:

```
<store>/<engine-id>/<guest-image-hash>/
    text      aarch64 code for the image's translated regions (position-independent)
    index     guest-address -> text-offset table, plus the guest base it assumes
    meta      engine version, options, coverage
```

- **Mapping.**
  - The runtime maps `text` RX through the existing vouched file-map door, since
    `dev9p` may back executable bytes (`docs/ARCHITECTURE.md:546-558`). It maps
    `index` R.
  - No new door to X exists, and none is needed.
- **Sharing.** The Image cache shares the pages across every Proc that maps the
  same file. Plan 9's shared text, for translations (P5).
- **Integrity.** Stratum's content addressing and Merkle verification give I-36's
  conditions (immutable snapshot, verified pages) for free.

### 7.2 Producers, and who must be trusted

- **Per-user store (`~/.cache/xt/`, the default).**
  - The user's own runtime writes it: blocks it JIT-compiled become entries for the
    next run.
  - It needs **no new trust**. A user can already author executable bytes in their
    own executable tree (`docs/ARCHITECTURE.md:550`, "crosses no boundary").
  - A bad entry harms only that user.
- **System store (`/lib/xt/store/`).**
  - Covers system-installed trees only, such as an amd64 tree shipped in the image.
  - Written by a TCB producer, the same engine in AOT mode, at install or build
    time. Read by everyone.
  - Clients only *request* translation and never supply code. A user can never place
    code that another user runs.
- **Privacy.** Per-user stores are kept apart deliberately. A shared cache keyed by
  content hash would reveal, through hit timing, that *someone* ran a given binary:
  the deduplication side channel VISION cites when it rejects KSM (CVE-2015-2877).
  Only system trees, whose contents are public, share.

### 7.3 Position independence (the engine research item)

- **The constraint.** Shared text must not depend on where it is mapped or on
  per-process state.
  - Host addressing is PC-relative.
  - Runtime state is reached through the engine's state register.
  - Guest addresses must be fixed for a given store entry.
- **How guest addresses are fixed.**
  - Non-PIE guests: their absolute addresses already are. That includes the classic
    Windows EXE at `0x400000` and most old Linux binaries.
  - PIE guests and DLLs: the runtime loads each guest image at a **deterministic
    base derived from its identity**. Guest-level ASLR is given up in exchange for
    sharing; Thylacine has no user ASLR today either.
- **The gap.** FEX's disk cache (FEX-2609) persists code for reload into its own
  buffer [S]. Mappable, shareable text is a larger engine change, and the single
  largest research risk in XT (section 12).
- **Interim, until the engine supports it.** Per-user caching through FEX's own disk
  cache. Entries load as data into code Burrows and are published, so nothing is
  shared or mapped from files.

### 7.4 Invalidation

- **Keys.** The key is the guest image's content hash plus the engine identity, so
  a changed binary or engine misses cleanly.
- **Self-modifying code** invalidates in-process and falls back to the JIT for the
  affected range. Store entries are never edited in place.
- **Eviction** is per-user and size-bounded (the knob XT-11 sets). FEX-2609's cache
  has no eviction [S]; ours must.

---

## 8. Personalities on top

- **Linux amd64 and 386.** Vivarium through the hosted decode. Breadth depends on
  Vivarium serving what translated glibc and musl programs forward:
  - futex `WAIT_BITSET`;
  - AF_UNIX (desktop programs);
  - `memfd` and shared maps;
  - `epoll`.

  Static and musl guests come first; glibc guests follow as those land. Those items
  belong to Vivarium's own roadmap and are listed in section 11 as dependencies,
  not built here.
- **Windows.** Wine (study, Route B) with FEX's Windows frontends (section 6.5). The
  Wine-specific kernel items (study K2, K4, K5) stay in that arc. K2 is XT-K6 here,
  so it lands once for both.
- **DOS and Win9x.** DOSBox-X stays a whole-machine emulator with its own dynrec. It
  gains the entitlement (no clearance prompt) and exact faults, and optionally an
  objtype entry for `.COM`/`.EXE`.

---

## 9. Invariants and verification

### 9.1 Proposed I-48 -- foreign code is never host code

> **I-48 (proposed).** In a Proc running a foreign objtype:
> **(a)** no byte of guest-ISA code or guest memory is ever host-executable. Host
> execution comes only through the three doors of ARCH 6.5: the vouched translator
> image, vouched file maps (including store text), and the Proc's own code Burrows.
> **(b)** A foreign objtype confers ABI shape, never authority (I-43 extended to
> objtypes and the hosted decode). The `code-emit` entitlement reaches only the
> entitled Proc's own address space (I-42).
> **(c)** The hosted decode is partitioned: a guest-decode call names only
> guest-partition handles and acts only on guest-arena memory.
> **(d)** A requested memory model is honoured or refused, never silently weakened.
> **(e)** Guest code is held to host code's rules. Guest memory becomes
> guest-executable only through exec's vouching (the guest image, guest file maps)
> or through code authority (`CAP_JIT`, or the objtype entry's guest-code policy,
> section 10 F10). The runtime translates nothing else.

### 9.2 Amendments

- **I-42:** `code-emit` entitlement as a second, image-bound route to code Burrows.
- **I-43:** objtype declaration (same channels, same fail-safe), and the hosted
  decode as shape.
- **I-19:** sub-invariant N-6 (section 5.5).
- **I-39:** a `code-emit`-entitled image counts as `CAP_JIT` in the image join from
  the moment the entitlement is stamped, and no entitlement is stamped on a
  debug-tainted load (section 5.2).
- **I-12:** unchanged. I-48 restates it for translated Procs: the page half in (a),
  the provenance half in (e).

### 9.3 Specs (spec-first is suspended; re-enabling is the operator's call)

**Recommend re-enabling spec-first for exactly two surfaces:**
- **`fault_note.tla`:** synchronous delivery, Ureg edit and validated resume, racing
  `kill`, death-wake (I-24) and `in_handler`. Buggy configs: delivery to the wrong
  thread; resume at a stale context; a fault lost when `kill` races the handler.
- **`hosted_decode.tla`:** partition tagging under concurrent guest and runtime
  calls and `fork`. Buggy config: a guest close reaching a runtime handle.

Everything else is prose plus audit, under the standing policy
(`docs/agent/SPEC-POLICY.md`).

### 9.4 Audit surfaces (rows to add when each lands)

- The foreign-exec path (I-12, I-36, I-43).
- The entitlement (I-42).
- The hosted decode and partitions (I-48(b), (c)).
- The guest-X marking at the guest decode, and the runtime's honouring of it
  (I-48(e)).
- Don't-fork code regions at the clone classifier (I-44, I-42).
- Exact faults (I-19 N-6, I-24).
- Memory-model switching (context switch).
- The store producer (I-36, I-48(a)).

### 9.5 Gates

- **XT first light.** A static x86-64 binary prints on serial and exits 0.
- **Correctness.** FEX's instruction test suites (ASM tests, instruction-count CI)
  run *on Thylacine*, and are diffed against the same suites on Linux.
- **Torture.** Self-modifying-code stress; signal torture (nested masks,
  `sigaltstack`, `SIGSEGV` resume); thread churn (more than 1,000 spawns, proving
  XT-K9); fork/exec chains crossing objtypes; a translated shell's fork loop.
- **Provenance.** A guest jump into data faults as the guest's own `SIGSEGV`; an x86
  binary on an `MNOEXEC` mount is refused; a guest `mprotect(PROT_EXEC)` on
  anonymous memory is refused without code authority (I-48(e)).
- **Bridges.** An x86 SDL program paints a Tapestry pane (screendump gate, the
  DOSBox-X idiom).
- **Sharing.** Two processes running one image share store text (`/proc` shows one
  Image).

---

## 10. Forks for the operator

The research (section 2) collapses most of these; each carries a recommendation.
**Voted 2026-10-08: every recommendation below was ratified as written**, F1-F9
together and F10 separately, with its proposed defaults.

| # | Fork | Options | Recommendation and why |
|---|---|---|---|
| **F1** | Where the guest's Linux semantics live, and what the translator is | **A1** the translator is a Linux-phenotype program (FEX's Linux frontend as is); **A2** native translator plus a hosted decode into Vivarium; **A3** the kernel learns x86 Linux (a second table, FreeBSD `linux32`-style) | **A2.** It is the only option where the translator is native (P3) *and* Linux stays single-homed (P4). It alone allows native bridges (section 6.4), without which x86 games need a Linux display stack. A1 forces Thylacine through Linux-shaped side doors and makes the translator's own correctness depend on Vivarium. A3 violates VISION 3.4 and doubles Vivarium. Cost of A2: section 5.4, a new kernel organ |
| **F2** | Fault delivery model | **In-thread** `notify`/`noted` with an editable Ureg; or an **out-of-thread** exception channel (Fuchsia, Mach) | **In-thread.** The heritage contract (`notejmp`), zero context switches on the self-modifying-code hot path, and what FEX and Wine are written for. The fd-shaped channel is the designed extension for debuggers, not the translator's path. (This is the fork `docs/JIT-ON-WX-DESIGN.md:148-160` left open) |
| **F3** | Code-emission authority for translators | **Image entitlement** (section 5.2); the per-process corvus clearance (status quo, a prompt per process); an **out-of-process writer** (no writable code in guest Procs at all) | **Image entitlement.** The out-of-process writer is the strongest W^X posture and genuinely novel. But under P6 its marginal security is small (a guest already holds its Proc's full authority through syscalls), and its cost is an IPC per cache miss plus a split engine. Most code arrives as store text anyway (F4), so the writable surface is small. Revisit if translated code ever runs with *less* authority than its Proc. Consequence, stated: the entitlement counts as `CAP_JIT` for debug authority (section 5.2) |
| **F4** | Translation store | In-process cache only; **Stratum files, per user plus a system store for system trees**; one shared system store | **Per-user plus system-trees store.** Sharing via the Image cache, integrity via Stratum, no new door to X, no new trust for per-user entries, and no deduplication side channel. FEX's disk cache is the interim |
| **F5** | Low-address space | **Per-Proc shapes at exec**; a global layout change; Madeira-style guest windows (no kernel change, a deep Wine/FEX fork) | **Shapes.** Layout is ABI shape (I-43), so the native layout keeps its properties and future ASLR |
| **F6** | Engine | **FEX**, with Box64 evaluated for ARMv8.0; Box64 primary; write our own | **FEX** (one core for Linux and Windows, TSO by default), kept behind an engine-agnostic kernel. Writing our own would spend years re-earning FEX's x86 corner cases; first class comes from integration, not from the JIT |
| **F7** | Second-decode selector | **SVC immediate**; an explicit `SYS_GUEST_CALL(nr, args)` | **SVC immediate.** Zero cost, keeps the argument registers unshuffled, with precedent (Windows ARM64). The explicit call is the fallback if hardware or debuggers make the immediate awkward |
| **F8** | Memory model | Software TSO only; **plus a kernel per-thread primitive where hardware has it** | **Plus the primitive.** It is cheap, ISA-neutral and honest (I-48(d)), and is the Rosetta advantage where it exists |
| **F9** | Spec posture | Prose plus audit only; **re-enable spec-first for `fault_note` and `hosted_decode`** | **Re-enable for those two** (section 9.3). They are the two genuinely concurrent new mechanisms |
| **F10** | Guest code generation: who may make anonymous or written guest memory executable. Examples: x86 JITs (Unity's Mono, Java), packers (UPX), DRM, self-modifying 386 code, and 32-bit Windows programs built without `/NXCOMPAT`, which Windows' default OptIn policy runs with DEP off [R] (section 5.4 item 4) | **G1** strict: `CAP_JIT` only, exactly as for a native JIT (the corvus `jit` clearance, a prompt per process). **G2** a guest-code policy per objtype entry in the host-owned table, `strict` or `permissive`: the shape of Windows' system-wide DEP policy. **G3** a per-session grant: the launcher takes the `jit` clearance once and the territory's Procs inherit it | **G2.** Proposed defaults: `strict` for `(linux, amd64)`; `permissive` for `(linux, 386)` and both Windows entries, whose corpora need it. The research does not settle this; it is a value call between W^X posture and compatibility. For G2: guest code runs with exactly its Proc's authority (P6), so `permissive` costs exploit mitigation, not authority, the posture Linux and Windows give these programs natively. And the decision sits where the entitlement does, in one auditable host-owned file. Against it: under `permissive`, an x86 JIT needs no clearance that a native one would. Under G1, most 32-bit Windows games and every Mono-built Unity title prompt (IL2CPP builds are compiled ahead of time and do not). G3 makes the `jit` clearance propagate, which it deliberately does not (`usr/corvus/src/main.rs:1285-1301`). Contrast DOSBox-X, whose guests self-modify freely because the emulator *is* a sandbox: a DOS guest makes no Thylacine syscalls |

---

## 11. Arc

Kernel chunks are main-track and audit-bearing; runtime chunks may be aux-track.
Each closes with its gate.

| Chunk | Content | Exit (gate) |
|---|---|---|
| **XT-0** | This design, voted (F1-F10), landed as a scripture commit; ARCH section 28 rows (I-48, the amendments); NOVEL candidates | ratified |
| **XT-1** | Exact faults (5.5), plus `fault_note.tla` | A native C test catches `SIGSEGV`, edits pc, resumes; Linux-phenotype `SIGSEGV` with an edited `ucontext`; OpenSSL's probe without `OPENSSL_armcap` |
| **XT-2** | Thread-directed notes and user note names (5.6) | Cross-thread suspend/resume test; Vivarium `tgkill` |
| **XT-3** | Thread reaping and feature discovery (5.8; study F2, F3) | More than 1,000 thread spawns in one Proc; `mrs ID_AA64ISAR0_EL1` at EL0 returns sanitised values |
| **XT-4** | Address-space shapes (5.3) | A foreign-shape Proc maps FIXED at `0x400000` and `0x7ffe0000`; native layout byte-identical |
| **XT-5** | Objtype declaration, the table, foreign exec, entitlement (5.1, 5.2) | `exec` of an x86 ELF under an amd64 mount loads the registered stub translator with `AT_GUEST_IMAGE`; refused when undeclared, mismatched, or when a native exec would refuse the guest file (`MNOEXEC`, no X bit) |
| **XT-6** | Hosted Procs (5.4), plus `hosted_decode.tla` | `svc #GUEST` reaches Vivarium; partition tests (guest `close` cannot touch runtime handles; guest `munmap` cannot touch the runtime arena); guest-X marking (a vouched `PROT_EXEC` file map is marked, an anonymous one is refused without code authority); don't-fork code regions (a Proc holding one forks, and the child holds none) |
| **XT-7** | Memory-model primitive (5.7), plus the HVF measurement | Recorded either way; `ENOTSUP` honest on the Pi 400 |
| **XT-8** | FEX port: the Thylacine host layer, code Burrows, faults, threads (6.2) | **XT first light:** a static x86-64 hello on serial |
| **XT-9** | The amd64 territory: an Alpine x86_64 rootfs, the `amd64` command, dynamic musl guests (6.3) | busybox sh plus coreutils gate suite under translation; FEX's instruction tests diffed against Linux |
| **XT-10** | i386 guests | A 386 static and a 386 musl binary |
| **XT-11** | The store: per-user files and the system store producer (7) | The second run maps store text; two Procs share one Image |
| **XT-12** | Bridges: SDL2, Vulkan, GL, audio (6.4); measure engine and region limits | An x86 SDL program paints a pane with sound |
| **XT-13** | Engine evaluation on the Pi 400 (Box64 versus FEX) | A recorded decision |
| **XT-14** | Hand-off to Wine (study Route B): FEX's Windows frontends on these primitives | (the Wine arc's gates) |

**Dependencies, not built here.** Vivarium's futex `WAIT_BITSET`, AF_UNIX, `memfd`
and shared maps, `epoll`, and the glibc-breadth seam (for XT-9 onward). The study's
K4 and K5 (for Wine).

**Sizing (rough; no comparable arc has a measured spend in-tree).**
- Kernel XT-1 to XT-7: about 14-20 chunks and 8-14 K LOC, including tests and the
  two specs.
- Runtime and store XT-8 to XT-13: about 12-18 chunks and 10-20 K LOC of
  Thylacine-side code and FEX patches, plus a generated bridge corpus.
- **Together:** roughly Vivarium-scale.

---

## 12. Risks

- **Shareable translated text** (7.3) is research-grade engine work. If it proves
  impractical, the store degrades to per-process loading: correct, unshared.
- **FEX fork maintenance**, permanent under its contribution policy (6.1).
- **FEX's ARMv8.4 floor** (#4120) would drop the Pi 400. Mitigation: Box64 (XT-13).
- **ARMv8.0 software TSO is slow.** Without hardware TSO (XT-7), the Pi 400 runs
  multithreaded x86 slowly however good the integration is.
- **The hosted decode is new kernel ground.** Its partitions are exactly the kind of
  filter whose one missed path is a bug class. Hence the spec, and audit before merge.
- **Vivarium breadth gates guest breadth.** Translation cannot outrun the Linux
  semantics it forwards to.
- **The store producer is TCB.** A compromised system producer is equivalent to a
  compromised `/bin`. It must be audited like the loader.
- **The runtime is TCB for guest NX (I-48(e)).** A runtime bug that translates
  memory the guest never made executable reopens code injection for that guest.
  Hence the provenance gates (section 9.5), and the runtime in the audit set.
- **A fork child recompiles what the store lacks** (section 5.4). Fork-heavy guests
  (preforking servers, build systems) pay for it until the store holds their code.

---

## 13. NOVEL candidates (for NOVEL.md on ratification)

1. **Translation as text.** Translated code served by the filesystem, mapped through
   the ordinary exec-provenance door, shared through the Image cache and
   integrity-checked by the filesystem's Merkle tree. Rosetta and XtaCache keep
   private caches [R]. None shares translations through the page cache of an
   integrity-verified, content-addressed filesystem.
2. **Objtype as a namespace declaration.** Plan 9's `$objtype` made executable by
   translation: an x86 environment is a `bind`, and a binary's ISA is decided by
   where it was found.
3. **Hosted Procs.** A declared second decode, with kernel-partitioned handles and
   memory, lets a native runtime drive the system's single Linux personality for a
   foreign ISA. The translator is a citizen, not a guest.
4. **Image entitlements without code signing.** Code-emission authority bound to a
   content-pinned image on a vouched mount, registered in a host-owned table.
5. **Translated code under native code's provenance rules** (I-48(e)). NX, `noexec`
   mounts and JIT-as-a-capability hold for an x86 program exactly as for a native
   one, because the translator refuses to translate what the native loader would
   refuse to map. To verify before NOVEL.md: whether Rosetta or FEX applies the
   host's `noexec` and exec-memory policy to guest code [R].

---

## 14. Naming (held for the operator; nothing renamed)

- **Wine, FEX and Box64 keep their names**, as DOSBox-X kept its own.
- **For the layer as a whole:**
  - **Convergence.** The thylacine is the textbook case of convergent evolution: a
    marsupial whose skull grew a wolf's shape. Translation is how an ARM lineage
    grows an x86 shape. The study's D8 held the word for the Windows capability;
    this draft proposes it move to the foundation.
  - **Alternate: Tiger**, for the "Tasmanian tiger", a marsupial wearing another
    animal's name, and a nod to Mac OS X Tiger, the release that shipped the
    first Rosetta.
- **Component names stay descriptive until then:** `/lib/xt/<objtype>`,
  `~/.cache/xt/`, `XT-n` chunks.

---

## 15. Exit criteria ("done")

- An x86-64 or i386 Linux program runs by plain `exec` from a declared tree or
  container, translated by a native runtime, with no prompt.
- Its faults, signals and threads behave as on Linux; its Linux semantics are
  Vivarium's.
- A second run uses stored, shared translated text.
- An x86 SDL program draws and sounds through native Thylacine libraries.
- I-48 and the amendments are in ARCH section 28; `fault_note` and `hosted_decode`
  pass their clean and buggy configs; every audit row is closed.
- No page is ever writable and executable.
- A translated program executes only guest code a native program could (I-48(e)).
- The kernel contains no x86.
