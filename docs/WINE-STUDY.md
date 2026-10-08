# Wine on Thylacine -- a feasibility study (graphics, games, 3dfx Voodoo)

> **Not scripture.** A study written 2026-10-04 against `main`@`8746a8a2`, in
> answer to the operator's question: *what would it take, at this point, to port
> Wine to Thylacine -- with a focus on graphics / games and Voodoo emulation?*
> Nothing here binds. A route chosen from it gets its own design doc (scripture
> before code), its votes and its audits.
>
> Evidence: in-tree claims cite `path:line` at `8746a8a2`. Outside claims cite a
> source (Appendix B) and are tagged **[V]** verified at the source, **[S]** seen
> only in a search-engine extract (the egress proxy blocked the page), or **[R]**
> recalled and unconfirmed. Inferences are marked *(inference)*.
>
> **Follow-up (2026-10-04):** the operator chose translator-first. The x86
> translation layer this study's sections 6-7 depend on is designed in
> `docs/X86-TRANSLATION-DESIGN.md` (ratified 2026-10-08; live state in
> `docs/xt-status.md`).

---

## 0. The answer

**Short version.**

1. **No part of Wine can run on Thylacine today, and the blockers sit below
   Wine.** Wine needs five things the kernel does not provide:
   - catchable, resumable faults with an editable context -- every EL0 fault
     terminates the Proc (`arch/arm64/exception.c:481-631`, `kernel/proc.c:3910-3989`);
   - notes aimed at one thread -- `SYS_POSTNOTE` takes a pid
     (`kernel/include/thylacine/syscall.h:886-914`);
   - mappings below 4 GiB -- including `0x7ffe0000`, which lies inside the main
     stack (`kernel/syscall.c:6447`, `kernel/include/thylacine/exec.h:87-88`);
   - a way to hand an open file to another Proc -- I-4's 9P handle attachment is
     designed, not built (`docs/ARCHITECTURE.md:3741-3751`);
   - user-tier shared memory between Procs -- Weft sharing is driver-tier
     (`kernel/syscall.c:7352`), and `MAP_SHARED` files are refused permanently
     (`docs/ARCHITECTURE.md:573-575`).

   Each is an audit-bearing kernel change (section 7.1).
2. **Every classic game is x86, so Wine on Thylacine always carries an x86
   translator inside it.**
   - Upstream Wine (11.x) runs natively on ARM64 and hands x86 code to an
     emulator DLL: FEX for x86-64 and i386, or Box64's `wowbox64` for i386
     ([V] Wine 10.0/11.0 ANNOUNCE; Hangover README).
   - That translator is the "FEX-class" consumer `docs/JIT-ON-WX-DESIGN.md:148-160`
     parked. CAP_JIT's dual map (as built) fits its code cache. Its
     self-modifying-code detection needs the resumable faults of point 1.
3. **Graphics is the most-built part of the problem, and the part least suited
   to Direct3D.**
   - Thylacine has Venus Vulkan with a zero-copy present path, plus llvmpipe and
     virgl OpenGL. All are built and gate-proven; none is in the default images.
   - DXVK (D3D8-11) rejects V3D outright: no full BC textures, no cull distance,
     no robustness2, no multi-draw-indirect ([V] DXVK device checks; Mesa v3dv
     source).
   - So on today's fleet, Direct3D goes through Wine's own wined3d at D3D9-class
     at best. That suits the Voodoo era and nothing newer.
4. **Thylacine already runs Voodoo games, without Wine.**
   - DOSBox-X's software Voodoo 1 renders the 3dfx build of Tomb Raider
     (`docs/reference/152-dosbox.md:169-191`).
   - The cheapest next Voodoo wins stay in that lane: OpenGL for DOSBox-X (its GL
     Voodoo and its Glide passthrough to OpenGlide), then Win98 inside it (DX-6).
   - Under Wine, Glide is a wrapper-DLL question, and it comes last. nGlide's
     Vulkan backend is the only wrapper that plausibly runs on V3D.
5. **Unreal does not need Wine.**
   - OldUnreal's Unreal 227k and UT v469e ship native ARM64 Linux builds, and
     Epic sanctioned free downloads of the game data from archive.org (section 8).
   - What Unreal needs is a Vivarium that runs glibc binaries and reaches the
     display. That is a strict subset of what any Wine route needs.

**Lanes, cheapest first.**

| Lane | Runs | Needs | State |
|---|---|---|---|
| **1. Cryptid** (whole-PC emulation, `docs/DOSBOX.md` section 11) | DOS and Win9x guests running real 3dfx drivers on an emulated Voodoo | GL baked for DOSBox-X; DX-6 | DOS Voodoo AS-BUILT (DX-7, software); DX-6 and GL Voodoo unbuilt |
| **2. Per-title native** | Prebuilt ARM64 Linux games (OldUnreal) via Vivarium; source ports via Pouch | Vivarium glibc, display and GL (the W4 sketch); per-port work | not started |
| **3a. Boxedwine** (Wine inside one emulator process) | 16/32-bit Windows programs, Wine 11 inside an emulated x86 Linux | A DOSBox-X-sized Pouch port plus CAP_JIT plumbing; probably no kernel work (to confirm) | not started |
| **3b. Native Wine** | 32/64-bit Windows programs; x86 through FEX/Box64 inside Wine | Kernel K1-K7, Pouch P1-P5, the Wine port W0-W10 | not started; `README.md:40` "Planned" |

**Recommendation.**

- Take lane 1 and lane 2 first for the late-90s catalogue.
- Run lane 3a as a one-arc probe. It is the cheapest way to learn how far
  "classic Windows games on Thylacine" gets with no kernel work.
- Build lane 3b's kernel prerequisites when they are wanted for their own sake.
  Each pays outside Wine: Vivarium's honest `SIGSEGV`, GC and JIT runtimes, the
  browser's IPC question.
- Port Wine natively through Pouch (Route B, section 3.2) on top of them.

The decisions this hands to the operator are listed in section 10.

---

## 1. What Wine asks of a host (2026)

### 1.1 Wine's shape

Wine 11.19 (2026-10-02) is the current development release; 11.0 (2026-01-13) is
stable [V]. Its parts:

- **PE side.** Windows DLLs (`ntdll`, `kernel32`, `user32`, `d3d9`, `wined3d`, ...)
  compiled as real PE files by a mingw cross-compiler.
- **Unix side.** `ntdll.so`, `win32u.so` and per-DLL "unixlibs" (`winevulkan.so`,
  ...). The loader `dlopen()`s `ntdll.so`, which `dlopen()`s each unixlib with
  `RTLD_NOW` [V `loader/main.c`, `dlls/ntdll/unix/loader.c`]. `configure.ac` has
  no static-build option [V].
- **wineserver.** One per prefix: a separate process that owns the NT kernel
  objects. Clients talk to it over a Unix socket in
  `/tmp/.wine-<uid>/server-<dev>-<ino>/`; children inherit it through
  `WINESERVERSOCKET` [V].
- **Drivers.**
  - User drivers (`winex11`, `winewayland`, `winemac`, `wineandroid`): windows,
    input, GL and Vulkan surfaces.
  - Audio drivers (`winealsa`, `winepulse`, ...) behind `mmdevapi`.
  - `winebus.sys` for HID and gamepads.
- **ARM64 hosts.**
  - Wine runs natively on aarch64. ARM64EC and ARM64X have been fully supported
    since 10.0 [V].
  - The new WoW64 is "considered fully supported" in 11.0, so 32-bit
    applications need no 32-bit Unix libraries [V].
  - x86 code goes to an emulator DLL that Wine loads by registry name:
    - `xtajit` for i386 and `xtajit64` for x86-64, exporting the `BTCpu*`
      interface [V `dlls/wow64/syscall.c`];
    - FEX ships `libwow64fex.dll` and `libarm64ecfex.dll` [V];
    - Box64 ships `wowbox64.dll` (i386 only, since v0.3.6) [V].
  - **Hangover** packages exactly this, with Box64 as the default for i386. At
    11.0 it was "down to 10 patches on top of Wine" [V].

### 1.2 The host contract

| Wine needs | For | Source |
|---|---|---|
| Synchronous `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGTRAP`/`SIGFPE` on the faulting thread, with the full context (GPRs, FP/SIMD, fault address), **editable before return** | SEH, guard pages, stack probes, write watches; FEX/Box64 self-modifying-code detection. Handlers rewrite the context, e.g. `x18 = TEB` | [V `signal_arm64.c`] |
| A per-thread alternate signal stack | Stack-overflow handling | [V] |
| Thread-directed async signals across processes: `SIGUSR1` (suspend + report the context), `SIGUSR2` (set the context), `SIGQUIT` (kill the thread) | `NtSuspendThread`, `Get/SetThreadContext`, `TerminateThread`. The server uses `tgkill` | [V `server/thread.c`] |
| fd passing (`SCM_RIGHTS`) both ways over the server socket | Every fd-backed handle (files, sockets, pipes, devices, sections) via `server_get_unix_fd`; each thread's reply/wait pipes; process init; per-object ntsync fds | [V `dlls/ntdll/unix/server.c`] |
| Shared memory between server and clients | Anonymous sections via `memfd_create("wine-mapping", MFD_EXEC)` [V]; the user32 state the server publishes in shared objects (`session_shm_t`, `desktop_shm_t`, `queue_shm_t`, `input_shm_t`, `window_shm_t`) | [V `server/protocol.def`] |
| Fixed low mappings | `KUSER_SHARED_DATA` is hard-coded at `0x7ffe0000` in ntdll, kernelbase and win32u. PE images sit at preferred bases. WoW64 processes need everything below 2 GiB (4 GiB if large-address-aware). The Linux preloader reserves 0x10000-0x68000000 and 0x7f000000-0x7fff0000 before ld.so runs | [V `loader/preloader.c`, `virtual.c`] |
| 4 KiB pages | Windows' page granularity. 11.0 can simulate it on 16K/64K kernels, but "a 4K-page kernel is strongly recommended" | [V 11.0 ANNOUNCE] |
| `dlopen` | `ntdll.so` and every unixlib | [V] |
| Executable memory | Native PE code and the emulator's JIT. Wine maps `PAGE_EXECUTE_READWRITE` straight to host RWX (`get_unix_prot`); on noexec mounts it falls back to reading images into anonymous memory | [V] |
| Cross-process memory access and ptrace | `ReadProcessMemory`/`WriteProcessMemory`, debuggers, debug registers (`process_vm_readv`, `/proc/pid/mem`, ptrace) | [V `server/ptrace.c`] |
| A fast NT synchronisation primitive (optional) | NTSync (`/dev/ntsync`, Linux 6.14), used since 10.16; otherwise every wait is a server round trip | [V] |
| POSIX file APIs | Case-insensitive lookup done in userspace; `dosdevices/` symlinks (`c:` -> `../drive_c`) read with `readlink`; dirfd `*at()` calls; `fcntl` locks; xattrs for DOS attributes (optional) | [R] |
| x18 | The Windows ARM64 ABI keeps the TEB in x18. Wine no longer needs `-ffixed-x18`: it reloads x18 on every return to PE code, so the host only has to preserve x18 across exceptions | [V] |

---

## 2. Thylacine against that contract

### 2.1 Kernel

| Need | Thylacine as built | Gap |
|---|---|---|
| Catchable faults | Every unresolved EL0 fault goes to `proc_fault_terminate` and the Proc dies with `snare:*` (`arch/arm64/exception.c:481-631`, `kernel/proc.c:3910-3989`). `snare:*` is not in `g_known_notes` (`kernel/notes.c:78-109`). `docs/VIVARIUM.md:2459` calls these notes catchable; the code disagrees | **K1** |
| Fault context in the handler | A native handler gets the note name plus a u32 (`kernel/notes.c:1855-1915`). The Linux phenotype gets GPRs, sp, pc and pstate, but no FPSIMD/ESR records and no `si_addr` (`kernel/notes.c:1421-1564`) | **K1** |
| Editable resume | `NCONT` and `rt_sigreturn` restore the kernel's saved copy and ignore edits (`kernel/notes.c:1963-1975`). Plan 9's own `notify` hands the handler a modifiable `Ureg*` | **K1** |
| Alternate stack | `sigaltstack` is ENOSYS (`kernel/vivarium.c:375-386`). While a handler runs, only `kill` is delivered, and a fault inside the handler kills the Proc (`kernel/include/thylacine/notes.h:46-48`) | **K1** |
| Thread-directed notes | `SYS_POSTNOTE` takes a pid, and only the parent or the Proc itself may post (`kernel/include/thylacine/syscall.h:886-914`). `tkill`/`tgkill` are not served (`kernel/vivarium.c:361-370`). There are no `SIGUSR`/`SIGALRM`-class note names | **K2** |
| Suspend / get / set another thread's context | `/proc/<pid>/ctl` stops the whole Proc and needs the single debugger slot. `regs` covers only the focus thread, and pstate is read-only (`kernel/devproc.c:1732-1900, 2264-2290`) | **K2** (Wine's own scheme needs only K1 + K2) |
| Low fixed mappings | Every burrow/mmap/JIT placement lives in `[0x1_0000_0000, 0x4000_0000_0000)` (`kernel/vma.c:529-556`), and a FIXED map below it is refused (`kernel/syscall.c:6447`). The main stack is `[0x7F80_0000, 0x8000_0000)` (`kernel/include/thylacine/exec.h:87-88`), so it contains `0x7ffe0000` | **K3** |
| Reserve without commit; partial protect/unmap | AS-BUILT: `RESERVE` (none/R/RW, alignment 2^12..2^30), lazy commit, partial protect and unmap that split mappings atomically (`kernel/include/thylacine/syscall.h:2356-2417`, `kernel/vma.c:610-666`) | fits |
| Protection changes | AS-BUILT among none/R/RW under a mint-time ceiling. Anonymous memory is capped at RW; a file map's RX only goes down (`kernel/include/thylacine/vma.h:65-85`) | fits, except that X is never added |
| Executable memory | Only exec, a vouched file map (devramfs or dev9p on a non-`MNOEXEC` mount: `kernel/devramfs.c:732`, `kernel/dev9p.c:2351`), or `SYS_JIT_CREATE`. Never anonymous memory turned X | shapes W2 and W7 (sections 6.2, 7.3) |
| fd / handle transfer | I-4: handles cross Procs only via 9P (`docs/ARCHITECTURE.md:5041`). The out-of-band handle attachment is DESIGNED (`docs/ARCHITECTURE.md:3741-3751`; `kernel/include/thylacine/handle.h:20-21` says "not yet"). Mycelium is parked (`docs/NOVEL.md:287-318`) | **K4** |
| Shared memory | Weft share is gated to `CAP_HW_CREATE` (`kernel/syscall.c:7352`). Writable/`MAP_SHARED` file maps are refused permanently (`docs/ARCHITECTURE.md:573-575`). A Proc may take in at most 128 MiB of shared memory (`kernel/include/thylacine/proc.h:164`) | **K5** |
| Thread churn | `PROC_THREAD_MAX` is 256 (`kernel/include/thylacine/proc.h:143`), and exited threads still count until the Proc dies (`kernel/proc.c:1193, 3732, 5777-5793`). That makes it a lifetime cap (Appendix A, F3) | **K6** |
| Feature probing | A trapped `mrs` of an ID register, or of `CTR_EL0`, is not emulated: the Proc dies with `snare:ill` (`arch/arm64/exception.c:627-631`; OpenSSL died this way, `docs/JOURNAL.md:26380-26388`). Only `AT_HWCAP`, no `AT_HWCAP2` (`arch/arm64/hwfeat.c:108-166`) | **K7** |
| Cache maintenance from EL0 | `dc`/`ic` trap (`SCTLR_EL1.UCI=0`). `SYS_ICACHE_SYNC` is the door (`kernel/include/thylacine/syscall.h:1987-2031`) | Wine, FEX and Box64 call sites route through it (as DX-4 did) |
| x18, TLS, FP | x0-x30 are saved on every EL0 entry (`arch/arm64/vectors.S:95-154`); `TPIDR_EL0` is per thread; FP/SIMD is switched eagerly; SVE is off (`arch/arm64/context.S:39-54, 91-92`) | fits |
| Page size, VA | 4 KiB granule; 48-bit TTBR0 with a 47-bit policy cap (`arch/arm64/mmu.c:671-689`, `arch/arm64/mmu.h:168-175`). That is exactly Windows' user range | fits |
| BTI | User PTEs never set the GP bit (`arch/arm64/mmu.c:1532-1589`), so PE code without landing pads runs | fits (but see Appendix A, F4) |
| Futex, time | `SYS_TORPOR_WAIT/WAKE`, process-private (`kernel/include/thylacine/syscall.h:688-736`); the vDSO clock page (`kernel/vdso.c`); counters readable at EL0 (`arch/arm64/timer.c:304-321`) | fits (server-mediated cross-process sync) |

### 2.2 Pouch (musl plus the boundary-line patches)

| Need | Pouch as built | Gap |
|---|---|---|
| `dlopen` | **AS-BUILT since B-1d (2026-09-25).** `-pie` gives a dynamic PIE whose interpreter is `/lib/libc.so`; `-shared` gives `.so`s; text only from vouched mounts; `LD_LIBRARY_PATH`/`LD_PRELOAD` ignored (`docs/ARCHITECTURE.md:577-579`, `docs/browser-status.md:294`) | none for unixlibs. Wine's `dlopen` design works as is |
| Threads, TLS | `pthread_*` over `SYS_THREAD_SPAWN` + torpor (`vault/system/boundary/pouch-seam/sub-pouch-thread.md:22-44`) | fits (K6 aside) |
| Signals | `sigaction` is stubbed to the tty/pipe/child family; no `SA_SIGINFO`, no `ucontext`, no `sigaltstack`, no `pthread_kill` (`vault/system/boundary/pouch-seam/sub-pouch-signal.md:25-39, 106-113, 172-175`) | **P1** (over K1/K2) |
| `mmap` | The kernel picks the address; `MAP_FIXED` only re-protects your own memory; `PROT_EXEC` is EACCES; `MAP_SHARED` is ENOSYS (`vault/system/boundary/pouch-seam/sub-pouch-mem.md:58-91, 300-318`) | **P2** (over K3/K5) |
| Unix sockets | `AF_UNIX` is stream-only on `/srv/<name>`. `bind` needs a TCB flag or the elevation-only `CAP_POST_SERVICE`. Limits: 8 sockets per Proc, 16 `/srv` slots system-wide (`usr/lib/pouch/patches/0006-pouch-sockets.patch:557-575`, `kernel/devsrv.c:510`, `kernel/include/thylacine/devsrv.h:70`). `socketpair`/`sendmsg`/`recvmsg`/`SCM_RIGHTS` are ABSENT (`vault/seams/seam-pouch-sendmsg.md`) | **P3** (over K4): a wineserver cannot even bind its socket as a normal user today |
| Event loop | `poll` handles at most 64 fds per call (`kernel/include/thylacine/poll.h:232`); no epoll or eventfd (`docs/POUCH-DESIGN.md:237`). The wineserver polls one fd per client thread | **P4** (wide poll, or Loom) |
| Files | `*at()` with a real dirfd is ENOTSUP; `symlinkat` is ENOSYS; `readlink` is EINVAL outside `/proc/self`; `fcntl` is ENOSYS for every fd (no `O_NONBLOCK`, no locks); no xattrs (`vault/system/boundary/pouch-seam/sub-pouch-fs.md:39-49`, `vault/system/boundary/pouch-seam/sub-pouch-net.md:350-358`) | **P5**: `dosdevices` alone needs `symlink` + `readlink` |
| Processes | No `fork`/`execve` by design; `posix_spawn` is AS-BUILT (`docs/POUCH-DESIGN.md:241-251`, `vault/system/boundary/pouch-seam/sub-pouch-process.md:78-110`) | Wine's process spawn is expressed with `posix_spawn` (W2) |
| Toolchain | LLVM 22.1.8 fork, AArch64 only, no mingw, `llvm-dlltool` or `llvm-rc` (`usr/ports/llvm/README.md:12, 36`) | The PE side (aarch64, arm64ec, i686) cross-builds on the host with llvm-mingw, as Hangover does [V] |

### 2.3 What already fits

The list of fits is not small:

- 4 KiB pages and a 47-bit user range.
- x18 preserved by the kernel.
- No user BTI to trip PE code.
- `dlopen` (B-1d).
- A dual-mapped JIT with an icache-sync syscall, of exactly the shape FEX's own
  source names as its W^X fix [V] and Madeira implemented (section 6).
- A zero-copy Vulkan present path.
- llvmpipe GL 4.6.
- An audio server (Nocturne).
- An SDL2 port that already carries a CAP_JIT dynarec emulator to the screen.

DOSBox-X proved the port idiom end to end (`docs/DOSBOX.md` section 5).

---

## 3. Where Wine would live: the routes

### 3.1 Route A -- Linux Wine under Vivarium

Run a Linux aarch64 Wine (and a Linux FEX/Box64) under the Linux phenotype.

What Vivarium is missing for that, by its own docs:
- **glibc.** Distro Wine is a glibc build; glibc-dynamic is OUT (`docs/VIVARIUM.md:3462`).
- **The signal tier.** No fault delivery, no `ucontext` edits, no `sigaltstack`,
  no `tgkill`, no `USR1`/`USR2`/`ALRM` (`kernel/vivarium.c:361-386, 1689-1786`).
- **Memory.** `MAP_FIXED` below 4 GiB (`kernel/vivarium.c:1397-1413`), `MAP_SHARED`
  and `memfd` (`kernel/vivarium.c:1352-1377`).
- **IPC and events.** `AF_UNIX` with `SCM_RIGHTS` (`kernel/vivarium.c:2266-2318`);
  `epoll` is OUT (`docs/VIVARIUM.md:3461`).
- **Display and GPU.** The W4 graphics sketch is unbuilt (`docs/AUX-ROADMAP.md:167-203`).
  The Wayland bridge is post-v1.0 (`docs/ROADMAP.md:1392`). X11 and `/dev/dri`
  are rejected (`docs/AUX-ROADMAP.md:191-195`).
- **JIT.** No Linux syscall row reaches `SYS_JIT_CREATE`, so a Linux FEX/Box64
  can never get CAP_JIT.

The kernel prerequisites are the same as Route B's. On top of them, Route A must
finish Vivarium's hardest tiers and build its graphics arc, and some of that
contradicts settled policy (`MAP_SHARED`, `/dev/dri`). **Not recommended for
Wine.** It is the right route for *prebuilt Linux games*, which need a much
smaller subset (section 8).

### 3.2 Route B -- a native Pouch port (recommended)

The DOSBox-X idiom at roughly ten times the scale, and the route
`docs/agent/NATIVE-VS-PORTED.md` already prescribes: foreign POSIX code goes
through Pouch.

- **Wine's Unix side is portable C over POSIX.** The port is `__thylacine__` arms
  in about six files:
  - `signal_arm64.c` (notes);
  - `virtual.c` (burrows, KUSER, the JIT pool);
  - `server.c` and the server's `request.c`/`fd.c` (handle passing);
  - `process.c` (`posix_spawn`);
  - `file.c` gaps;
  - instruction-cache flushes.
- **Three new drivers:** a user driver over Tapestry (section 4.4), `pVulkanInit`
  over Warp, and an `mmdevapi` driver over Nocturne.
- **It talks to Tapestry, Warp and Nocturne directly**, as SDL_thylacine does, so
  no bridge is needed.

**Haiku is the instructive precedent.** HaikuPorts ships Wine 11.8. The
patchset needed one kernel call (`THREAD_SET_GS_BASE`), signal-context
accessors and some stubs, because Haiku already had a POSIX signal tier and
`SCM_RIGHTS` [V]. Thylacine's patchset will be larger because those two pieces
are the K-items.

### 3.3 Route B-lite -- the Madeira shape (single-Proc Wine)

Madeira (Sept-Oct 2026) runs Wine 11.4 as ARM64EC plus FEX on stock iOS 26 [V].
iOS is a host with strict W^X, no fd passing and no mappings below 4 GiB. To get
there, Madeira:
- links the unixlibs statically;
- runs the wineserver as a **thread**, with every Windows process a
  pseudo-process inside one task;
- gives FEX a pre-made dual-mapped JIT pool ("every JIT write goes to address +
  WriteOffset");
- runs 32-bit guests in 4 GiB-aligned "guest windows" (FEX pins x19 to the
  window base and forms base + zext32(address)).

On Thylacine this shape would erase **K4 and K5** (no cross-Proc handles or
shared memory) and could erase **K3** for i386 (guest windows). The costs:
- a deep, unupstreamable Wine fork;
- no isolation between Windows processes;
- Madeira's own report that `SuspendThread` does not stop the target [V].

Hold it as a fallback if K4/K5 stall, not as the plan.

### 3.4 Route E -- Boxedwine (Wine inside one emulator process)

Boxedwine emulates the Linux kernel **and** the x86 CPU inside one SDL process,
and runs unmodified 32-bit Linux Wine in it [V]:
- "16/32-bit Windows programs";
- Wine "3.1 to 11.0";
- OpenGL, Direct3D and Vulkan via host passthrough;
- an `armv8` CPU backend in-tree;
- GPL v2.

It is DOSBox-X's shape exactly: a C++ SDL app whose guest world, including the
wineserver, its sockets and its signals, lives inside the emulator. *(Inference:)*
it should need only what DOSBox-X needed (Pouch, SDL2, CAP_JIT plumbing, plus
SDL GL), and no K-item.

Two unknowns decide that:
1. Does its JIT detect self-modifying code in software (as DOSBox-X does) or
   through host page faults (as FEX/Box64 do)? The latter would need K1.
2. Is it GPL-2.0-only? That would conflict with linking against Pouch's GPLv3
   patches (the `docs/DOSBOX.md` section 2 rule).

Its own README warns "games after the year 2010 have limited success", which is
no handicap for the Voodoo era. It costs one probe arc and gives an early,
honest answer about demand.

### 3.5 Route D -- emulate the whole PC

That is Cryptid: DOSBox-X now, with 86Box as a later candidate (section 5). No
Wine is involved. It is the authentic vehicle for Win9x and real 3dfx drivers,
and the slowest per frame.

### 3.6 "Wine as a Vivarium phenotype", reconciled

`README.md:40` muses about running Wine as a Vivarium phenotype.
- **As an ABI personality in the kernel, it is the wrong layer.** Wine *is* the
  Windows personality, in userspace ("personality is a loader + a library set",
  `docs/VIVARIUM.md:171`).
- **As a layout *shape*, the phenotype machinery fits K3 well.** A Proc whose exec
  annotation asks for a "Windows-shaped" address space (stack and PIE base
  moved above 4 GiB, `[64 KiB, 4 GiB)` opened for FIXED reservations) gets ABI
  shape and no authority. I-43 then holds by construction.

So "Wine as a phenotype" means Route B plus a layout-shape phenotype.

---

## 4. Graphics and games

### 4.1 The path a frame takes (Route B, an i386 game)

```
 game.exe (i386)  -- emulated by FEX/Box64 under WoW64 --
   | ddraw / d3d8 / d3d9 / opengl32 / glide2x       (i386 PE, also emulated)
   v
 wined3d (i386 PE)         nGlide (i386 PE)          DXVK (i386 PE)
   | GL calls                | Vulkan calls            | Vulkan calls
   v                         v                         v
 opengl32 / winevulkan: WoW64 thunk ==> native aarch64 win32u.so / winevulkan.so
   |                                              |
   v                                              v
 EGL + Mesa llvmpipe (CPU) or virgl         Venus (static) -> virtio-gpu -> host GPU
   |                                              |
   +-------> user driver: window surface / presentable -> Tapestry pane
```

**Only the API crossing runs native; everything above the thunk is emulated
x86.** That favours thin wrappers (nGlide over Vulkan) over thick ones (DXVK's
translation logic runs under emulation). *(Inference.)*

### 4.2 Thylacine's GPU stack, as built

| Path | Status | What it gives | Where |
|---|---|---|---|
| virtio-gpu 2D + Tapestry weaves | AS-BUILT, baked | CPU-drawn surfaces, zero-copy present | `tools/run-vm.sh:359-365` |
| llvmpipe GL via OSMesa | built, not baked (`libOSMesa.a` is fetched, about 205 MB) | GL 4.6 compatibility profile, CPU-rendered, needs CAP_JIT | `docs/LLVM-DESIGN.md:1465`; `usr/ports/mesa/README.md:190-202` |
| virgl GL | built, not baked; thyla-pi only | Hardware GL (capped at 4.3 by design) | `docs/phase7-status.md:990`; `docs/GPU-DESIGN.md:2881-2891` |
| Venus Vulkan | built, not baked; thyla-pi only (Linux host, `venus=on,blob=on`) | Vulkan, instance 1.4; device = the host's v3dv on V3D 4.2 | `docs/WARP-V3-DESIGN.md:48-68, 813-816`; `docs/agent/GATES.md:91-96` |
| EGL / GLX / GBM / GLES | ABSENT (disabled in the Mesa port) | -- | `usr/ports/mesa/README.md:152-157` |
| Vulkan loader / ICDs | ABSENT; Venus is linked statically into each app | -- | `tools/build.sh:5989` |
| lavapipe, zink, a native v3d driver | ABSENT (guest lavapipe deferred) | -- | `docs/HALCYON.md:249-251`; `vault/invariants/inv-i45.md:46` |

Present constraints that shape Wine:
- **Vulkan presents fullscreen-DIRECT only.** The windowed "composed" arm is
  unbuilt (`usr/ports/sdl2/thylacine/SDL_thylacinevulkan.c:184-191`; Halcyon H-5).
- **Swapchain:** FIFO only (`docs/WARP-WSI-DESIGN.md:867`), B8G8R8A8 formats, at
  most 4096x4096.
- **Per Warp context:** 16 presentable images, 256 memory objects, 64 MiB of
  guest buffer memory and 192 MiB of host-visible memory.
- **System-wide:** 8 contexts and 4 Warp connections
  (`usr/tapestryd/src/server.rs:138, 278, 303, 356, 382`; `usr/lictor/src/limits.rs:3`).

One GPU game at a time, fullscreen, with a 1999-sized VRAM budget: fine for
this catalogue.

### 4.3 Direct3D and friends against that stack

| API (era) | Translation | Requirement | On Thylacine |
|---|---|---|---|
| GDI, DirectDraw 2D | Wine's DIB engine (CPU) | none | Works on the 2D path |
| D3D 1-7 (Voodoo era) | wined3d; D7VK (needs Vulkan 1.4, in maintenance mode [V]); dgVoodoo2 (to D3D11) | wined3d-GL: GL 2.1-class plus FBOs and GLSL 1.20 [V] | **wined3d-GL over llvmpipe or virgl** -- once EGL exists (below) |
| D3D 8/9 | wined3d; DXVK | DXVK's D3D9 baseline still needs `geometryShader`, `textureCompressionBC`, `shaderCullDistance` and robustness2 [V] | wined3d (GL, or Vulkan up to FL 9_3 [V]). DXVK: no capable device in the fleet |
| D3D 10/11 | DXVK; wined3d (FL10 needs GL 3.2 + SM4) | DXVK 3.1.1 rejects a device lacking BC, cull distance, int64, multiViewport, multiDrawIndirect, robustness2, maintenance5/6 ... [V] | v3dv lacks all of those [V Mesa source]. Only Venus over a desktop-class host GPU would qualify |
| D3D 12 | vkd3d-proton (Vulkan 1.3 + a million-descriptor heap) [V] | -- | out of reach |
| OpenGL games | `opengl32` -> win32u's EGL path, `EGL_OPENGL_API` only [V] | Desktop GL through EGL | **Needs Mesa EGL** (surfaceless or a Thylacine platform) over OSMesa's drivers |
| Vulkan games | `winevulkan` -> the host driver | Venus | Works within section 4.2's limits |

**Two tree-level facts follow.**

1. **Wine's GL wants EGL, and the Mesa port has none.** Turning on Mesa's EGL
   (surfaceless first; it presents by copy into the window surface) is a
   prerequisite for wined3d-GL and for OpenGL games. It is a Mesa-port chunk, not
   a Wine one.
2. **Wine 11's Vulkan renderer for wined3d is "not yet at parity" and not the
   default [V].** So the mature D3D1-9 path is wined3d over GL, and GL on
   Thylacine means llvmpipe (CPU) anywhere, or virgl on thyla-pi.

### 4.4 Windowing: the virtual desktop is Thylacine's native shape for Windows

Halcyon's thesis rules out "overlapping / floating / z-ordered windows" and
app-owned overlays (`docs/HALCYON.md:254-256`). Tapestry gives a client four
surfaces (`usr/tapestryd/src/server.rs:157`) and tiles them. Windows programs are
the opposite of that.

Wine already has the reconciliation: **virtual-desktop mode** (`explorer
/desktop=...`). Wine draws one desktop and manages its own windows, z-order and
menus inside it, so the host sees one surface.

The recommended user driver is therefore *virtual-desktop-only*:
- **One pane per Wine prefix**, the "Windows world in a tile", like Cryptid's
  DOSBox-X pane.
- **Composition:** Wine's window surfaces composited into one weave.
- **Fullscreen games:** the desktop resized to the game's mode. Mode changes are
  emulated by scaling, because apps cannot change the display mode
  (`docs/gfx-status.md:74`). The pane is zoomed (Super+F) for DIRECT Vulkan
  present.

It is a smaller driver than `winewayland`. It needs:
- no host window management;
- no clipboard or IME at first;
- only `pVulkanInit` and `pOpenGLInit` beyond surfaces and input.

`pVulkanInit` returns four hooks: surface creation, presentation support, and
instance and device extension mapping [V]. Behind them sits Venus's headless
surface bound to a Tapestry presentable, the way `SDL_thylacinevulkan.c` already
binds it. win32u `dlopen`s `libvulkan`. Since there is no loader, the port either
binds Venus statically or builds Venus once as a `.so` (B-1d makes either
possible).

### 4.5 Input, audio, timing

**Input.**
- Keyboard: evdev keycodes, US QWERTY only (`usr/lictor/src/keymap.rs:1-5`).
- Mouse: absolute and relative (virtio-tablet and virtio-mouse), five buttons
  and a vertical wheel.
- Missing:
  - **pointer warp and confinement** (`usr/ports/sdl2/thylacine/SDL_thylacineevents.c:229-264`),
    which Windows mouselook does through `SetCursorPos`/`ClipCursor`. The
    driver can fake it with relative events in raw-input mode, but a Tapestry
    pointer-lock verb is cleaner;
  - **a cursor**: the guest draws none, and QEMU's `show-cursor` stands in
    (`tools/run-vm.sh:427-434`);
  - **gamepads and joysticks**: ABSENT, with no HID input stack
    (`usr/lictor/src/backend/input.rs:313-322`) and xHCI only DESIGNED
    (`docs/MENAGERIE.md:330`), so XInput/DirectInput pads are their own arc;
  - **IME and clipboard**.

**Audio.**
- Nocturne playback is AS-BUILT over virtio-sound (`docs/NOCTURNE.md:1247-1265`).
- SDL's driver is fixed S16LE / stereo / 48 kHz with about 340 ms of buffering
  (`usr/ports/sdl2/thylacine/SDL_thylacineaudio.c:13-28`), too laggy for games.
  A Wine `mmdevapi` driver should use Nocturne's shared-ring voices.
- DirectSound, XAudio2 and winmm all sit above `mmdevapi`.

**Timing.**
- There is no guest vblank. The compositor ticks at 60 Hz and throttles to
  15 Hz unless a visible surface declares `intent dynamic`
  (`docs/TAPESTRY.md:815-831`), so the Wine driver must declare it the way SDL
  does.
- Vulkan frames pay about 10-11 ms of display flush; MAILBOX is designed but
  unsigned (`docs/WARP-WSI-DESIGN.md:860-945`).

### 4.6 The GPU fleet question

The fleet has no host that would run DXVK-class Direct3D:
- **The dev loop** is QEMU on macOS. It has no `virtio-gpu-gl` and so no
  hardware 3D (`docs/GPU-DESIGN.md:164-190`), and upstream QEMU on macOS still
  cannot do Venus [V] (UTM 5.x can, through MoltenVK [V]).
- **thyla-pi** is a Raspberry Pi 400: V3D 4.2 and Cortex-A72
  (`docs/agent/THYLA-PI.md:6-10`).
- **thyla-gl** uses the host's software lavapipe (`docs/GPU-DESIGN.md:2816`).

A Linux host with a desktop GPU behind Venus would be needed.

That decides the realistic ceiling: **wined3d at D3D9-class on GL**, which covers
roughly 1996-2004. It is a hardware decision, not a software one (D6).

---

## 5. 3dfx Voodoo

### 5.1 Three layers

| Layer | What it is | Examples | Thylacine |
|---|---|---|---|
| **H** -- hardware emulation | The guest runs real 3dfx drivers against an emulated Voodoo PCI device | DOSBox-X (V1); DOSBox Staging (V1, multithreaded, "scale[s] well up to 8-16 threads" [V]); 86Box (V1/V2/Banshee/V3, with an ARM64 Voodoo JIT since v6.0, 2026-05-31 [V]); PCem (no ARM64 Voodoo codegen [V]); MAME | **DOSBox-X V1 software: AS-BUILT** (DX-7) |
| **G** -- API wrapper in the game's process | A `glide2x`/`glide3x` DLL translates Glide to D3D/GL/Vulkan | nGlide, dgVoodoo2, OpenGlide, psVoodoo | none (needs Wine for Windows games) |
| **P** -- passthrough | The emulator intercepts the guest's Glide calls and runs them on a host-side Glide (layer G on the host) | DOSBox-X `glide=` (Glide 2.x); qemu-3dfx | Code present in DOSBox-X; no host provider |

### 5.2 What Thylacine has today

All in `third_party/dosbox-x/src/hardware/`:
- **`voodoo_emu.cpp`** (3,772 lines): the software rasteriser that renders the
  Tomb Raider demo (`docs/reference/152-dosbox.md:169-191`). It is
  single-threaded: it allocates one statistics block (`voodoo_emu.cpp:2944`).
- **`voodoo_opengl.cpp`** (2,082 lines): compiled out, because `C_OPENGL` is
  undefined (`usr/ports/dosbox-x/config.h:69`).
- **`glide.cpp`** (1,953 lines): Glide 2.x passthrough. It speaks an I/O-port
  protocol to a guest-side OVL or DLL, and `dlopen`s `libglide2x.so` on the
  host (`glide.cpp:353-363`). The emulator is a static `ET_EXEC`, and no host
  Glide exists in the tree.

### 5.3 Voodoo steps that need no Wine (cheap to dear)

- **V1 -- GL Voodoo.**
  - Bake GL (OSMesa/llvmpipe; virgl on thyla-pi) and build DOSBox-X with
    `C_OPENGL`, which gives `voodoo_card=opengl`.
  - Higher resolutions, and rasterisation moves off the emulator thread.
    llvmpipe is CPU too, but multithreaded with JIT'd shaders.
  - It inherits llvmpipe's CAP_JIT need, which DOSBox-X already holds
    *(inference)*.
- **V2 -- Glide passthrough.**
  - Build OpenGlide natively (LGPL-2.1; Glide 2.x only; fixed-function GL 1.x;
    last commit 2023-12-10 [V]).
  - Link it statically into DOSBox-X, or rebuild DOSBox-X as a dynamic PIE so
    upstream's `dlopen` stands (B-1d).
  - DOS Glide games then rasterise outside the emulated CPU. DOSBox-X's
    passthrough also serves Win9x guests [V].
- **V3 -- DX-6.** A Win98 guest gives Windows Glide titles through V1/V2 inside
  DOSBox-X, with real 3dfx drivers. This is the authentic Win9x/Voodoo showcase
  `docs/DOSBOX.md` section 8 already scopes.
- **V4 -- a faster H layer**, one of two ways:
  - **Port DOSBox Staging's multithreaded Voodoo** into DOSBox-X's software path.
  - **Port 86Box.** It is GPL, has an SDL-only frontend with `QT=OFF` [V], and
    emulates Voodoo 2/Banshee/3. On non-Linux hosts its JIT flips pages between
    RW and RX with `mprotect` [V], which I-12 forbids, so it needs the same
    dual-map rework DX-4 did.
- **V5 -- a native "Thylacine Glide".**
  - Glide 2.x and 3.x over Vulkan or GL: one engine serving as DOSBox-X's
    passthrough provider and as Wine's `glide2x`/`glide3x` (an i386 PE thunk
    plus an aarch64 unixlib).
  - This is the README's "(Planned) 3DFx Glide emulation" (`README.md:51`).
  - Prior art is thin: GlideGL is skeletal [V]. 3dfx's 1999 Glide source licence
    [V] is reportedly non-OSI and GPL-incompatible [R], so build on OpenGlide's
    design, not 3dfx's code.

### 5.4 Glide under Wine

Glide games are 32-bit x86, so the wrapper must be an i386 PE inside the WoW64
process. Wine ships no Glide [V].

| Wrapper | Licence | Glide | Output | Arch | Fit |
|---|---|---|---|---|---|
| nGlide 2.10 (2019) | closed freeware | 2.11 / 2.60 / 3.10 | D3D9 or **Vulkan 1.0** | i386 only | Its Vulkan backend calls `winevulkan` directly with no DXVK. The only plausible wrapper on V3D (untested) [S/inference] |
| dgVoodoo2 2.87.5 (2026-09-15) | closed freeware, "freely ship" | Glide 1-3, Napalm (plus DirectDraw and D3D3-9) | D3D11/12 | x86 / x64 / ARM64 / ARM64EC | Needs D3D11, i.e. DXVK, impossible on V3D. The author says Wine is "not a target", and recent versions fail under DXVK [V] |
| OpenGlide | LGPL-2.1 | 2.x | GL 1.x | source | As an i386 PE it runs emulated; better as DOSBox-X's host provider (V2) |
| psVoodoo | LGPL-2.0 | 2.x | D3D9 | i386 | dormant [S] |
| native "Thylacine Glide" (V5) | ours | 2.x + 3.x | Vulkan or GL | i386 thunk + aarch64 | The only option whose per-call work runs natively |

### 5.5 Voodoo recommendation

1. **Do V1 and V2 next.** Both ride assets already in the tree; the gate is baking GL.
2. **Then V3** (DX-6) for Windows-era Glide titles with real drivers.
3. **Under Wine, start with nGlide's Vulkan backend** once W6 and W7 exist.
4. **Build V5 only if lane 3b is taken.** It is then the shared Glide engine for
   both emulators and Wine.
5. **Weigh V4 (86Box) when Voodoo 2/3-only titles or accuracy become the goal.**

---

## 6. x86 on ARM64: the translator inside Wine

### 6.1 Options

| Option | Shape | Viable on Thylacine? |
|---|---|---|
| FEX as `libarm64ecfex.dll` (x86-64) + `libwow64fex.dll` (i386) | Hangover/upstream emulator interface | Yes, with the adaptations in section 6.2 |
| Box64 `wowbox64.dll` (i386; Hangover's default) | Same interface | Yes, with the same adaptations. ARM64EC is only "planned" (issue #3648) [V] |
| Whole-process x86 Wine (Winlator: x86_64 Linux Wine under Box64; or FEX standalone with an x86 rootfs) | Linux ABI plus native-lib wrapping | **No**: needs Route A's full Linux surface (glibc, X11, epoll...) |
| Boxedwine | Emulates the CPU and the Linux kernel in one process | Yes, as Route E |

### 6.2 What the translator needs from Thylacine

- **A code cache that is never W+X.**
  - FEX allocates RWX; buffers start at 16 MB and grow to 128 MB.
  - FEX's own source names the fix: "a memory mirror where one half is mapped as
    RW and the other is RX" [V].
  - Madeira's fork implements it with a pool built before Wine starts [V].
  - On Thylacine: `SYS_JIT_CREATE` regions (64 MiB each,
    `kernel/include/thylacine/syscall.h:2494-2498`), so FEX needs two regions or
    a raised cap.
  - Box64 allocates RWX with no W^X mode, behind an OS layer split into
    `os_linux.c` and `os_wine.c` [V], so a Thylacine backend slots in there.
- **Publish through the syscall.** User-mode `dc`/`ic` and `CTR_EL0` reads trap.
  Every `__clear_cache` site in Wine, FEX and Box64 routes to `SYS_ICACHE_SYNC`,
  as DX-4 did.
- **Resumable faults (K1).**
  - Under Wine, FEX write-protects translated guest pages with
    `NtProtectVirtualMemory`, catches the access violation, invalidates the
    translation and restores write access (`InvalidationTracker.cpp`) [V].
  - Box64 does the same with `SIGSEGV` [V].
  - This is the JIT document's caveat 6, and now it has a consumer.
- **Guest RWX is free.**
  - A page a guest marks `PAGE_EXECUTE_READWRITE` never has to be host-executable:
    the translator reads it. So `virtual.c` must stop asking for host X on
    non-native images *(inference; Wine maps RWX straight through today [V])*.
  - The flip side is a limit: *native* ARM64 Windows programs that JIT (RWX at
    one address) cannot run. x86 ones can.
- **Memory ordering.**
  - FEX emulates x86 TSO by default [V]. On ARMv8.0 without LSE/LRCPC (the
    A72 in thyla-pi) every load and store becomes acquire/release [S].
  - FEX issue #4120 proposes an ARMv8.4 floor that would drop "all Raspberry Pi
    models" [V].
  - Box64 defaults to `STRONGMEM=0` and keeps a Pi 4 (A72) build target [V].
  - Thylacine's userspace floor is ARMv8.0 (`tools/check-v80-floor.py`).
  - So: **Box64 for i386 on thyla-pi; FEX where the host has LRCPC2** (M2 [R]).
- **Feature detection without `mrs`.** A translator that reads ID registers dies
  (K7), unless the port feeds it `AT_HWCAP`.
- **The FEX unixlib.** Since FEX-2609 the emulator DLLs require
  `libarm64ecfex.so`/`libwow64fex.so`, exporting eight small calls (TSO control,
  unaligned-atomic control, madvise, VMA naming, SHM stats, map-file, getpid) [V].
  These are ported in W7.
- **A 32-bit address space below 4 GiB (K3)**, or Madeira's guest windows.
- **CAP_JIT for every Wine process that runs x86.**
  - The corvus `jit` clearance is re-authenticated and not propagating
    (`usr/corvus/src/main.rs:1285-1301`). Every human user is eligible by
    default (`usr/corvus/src/main.rs:1397-1415`).
  - A launcher that spawns the game spawns a second activation (D5).

---

## 7. Work breakdown

### 7.1 Kernel prerequisites (main track, all audit-bearing)

| ID | Item | Touches | Also unblocks |
|---|---|---|---|
| **K1** | **Resumable faults.** Deliver `snare:segv/bus/ill/trap` to the faulting thread with a full `Ureg` (GPRs, FP/SIMD, FAR, ESR). Honour edits on `NCONT`, validating pstate. Add a per-thread alternate note stack, and a nested-fault rule. Wakes dormant task #235 with a wider scope. The delivery model is the open fork (D2) | I-19, I-24; the notes paths | Vivarium's honest `SIGSEGV`; FEX/Box64; GC write barriers; Boxedwine if it needs it |
| **K2** | **Thread-directed notes** (at least within a Proc; across Procs for same-owner peers under I-26's two-axis rule), plus user note names of the `USR1/USR2` class. Wine's server can instead post to the Proc and have a helper thread forward intra-Proc, which shrinks this to the intra-Proc half | I-19, I-26 | Vivarium `tgkill`; Pouch `pthread_kill` |
| **K3** | **A low-VA address-space shape.** Chosen at exec (phenotype-style), it moves the stack, PIE base and vDSO above 4 GiB and opens `[64 KiB, 4 GiB)` to FIXED reserve/map, including `0x7ffe0000` (D3) | VM layout; I-32; I-43 | Vivarium's `MAP_FIXED` below 4 GiB |
| **K4** | **Handle passing usable from userspace.** Build ARCH section 18.6's out-of-band handle attachment on 9P, plus user-tier service posting: today `bind` needs `CAP_POST_SERVICE` and `/srv` has 16 slots (D4) | I-4, I-5, I-6; `handles.tla` | Pouch `sendmsg`; the browser's open IPC question (`docs/BROWSER-DESIGN.md:405-421`); Mycelium |
| **K5** | **User-tier shared memory** between same-owner Procs: a Weft share without `CAP_HW_CREATE`, inside I-32's shared-in bound, handed over via K4. Serves `memfd` sections and the `*_shm_t` objects | I-32, I-44 | Pouch `memfd`/`MAP_SHARED`-on-memfd |
| **K6** | **Per-thread reaping**, so the 256-thread cap counts live threads (Appendix A, F3) | I-24, I-32 | WebKit's thread pools; any long-running server |
| **K7** | **EL0 feature probing.** Emulate `mrs` of the ID registers and `CTR_EL0` (Linux's `HWCAP_CPUID` model), and add `AT_HWCAP2` | exception path | OpenSSL's probe, FEX, vixl, LLVM |
| K8 | *(Optional, performance)* An NTSync-shaped kernel device: wait-any/wait-all over semaphores, mutexes and events. Without it every wait is a server round trip | I-9 (spec-bearing) | -- |

### 7.2 Pouch prerequisites

- **P1 -- signals.** `sigaction` with `SA_SIGINFO` and a real `ucontext_t`,
  `sigaltstack`, `pthread_kill` and `SIGUSR1/2`, over K1/K2.
- **P2 -- memory.** `mmap(MAP_FIXED)` into K3's window, and `memfd_create` plus
  shared maps of memfds over K5.
- **P3 -- sockets.** `socketpair`, `sendmsg`/`recvmsg` with `SCM_RIGHTS`
  emulated over K4, and user-tier `AF_UNIX` `bind`.
- **P4 -- events.** `poll` beyond 64 fds, or an epoll-alike over Loom, for the
  wineserver.
- **P5 -- files.** dirfd `*at()`, `symlink`/`readlink`, `fcntl`
  (`O_NONBLOCK`, `F_SETLK`), and optionally xattrs.

### 7.3 The Wine port (Route B)

| ID | Chunk | Exit |
|---|---|---|
| W0 | Scripture: `docs/WINE.md` plus the votes in section 10 | ratified design |
| W1 | Build plumbing: Wine's tools built on the host; the PE side with llvm-mingw (aarch64 + arm64ec + i686, as Hangover does [V]); the Unix side with the Pouch toolchain (dynamic, B-1d); installed onto a vouched mount | `wine --version` on device |
| W2 | `ntdll` Unix port: notes (K1/K2); burrows (K3, KUSER); no host X for non-native images; native PE text file-mapped at page-aligned offsets without code relocation (no anonymous memory turns X); `posix_spawn`; icache through `SYS_ICACHE_SYNC` | `wine cmd /c echo` (aarch64 PE) on serial |
| W3 | wineserver port: main loop on wide poll or Loom (P4); handles via K4; shared objects via K5; suspend via K2; ptrace and debug features stubbed | `wineboot` builds a prefix |
| W4 | `dosdevices`, the registry, fonts, locale (C plus UTF-8) | a GDI app starts headless |
| W5 | The virtual-desktop user driver over Tapestry: weaves, keyboard, relative and absolute mouse, a software cursor, emulated modes, `intent dynamic` | Notepad and Minesweeper in a pane |
| W6 | 3D: `pVulkanInit` over Venus (static or `.so`); `pOpenGLInit` over Mesa EGL. Enabling EGL in the Mesa port is its own chunk | a Vulkan sample; wined3d D3D9 over GL |
| W7 | x86: FEX (or Box64) with the dual-mapped JIT pool, K1 self-modifying-code tracking, `SYS_ICACHE_SYNC`, `AT_HWCAP` probing, the FEX unixlib; then WoW64 i386 over K3 | an x86-64 console app; then an i386 GDI game |
| W8 | Audio: an `mmdevapi` driver over Nocturne shared-ring voices | DirectSound in a game |
| W9 | Games: DirectDraw/D3D7 through wined3d-GL; D3D9; OpenGL titles; **Glide through nGlide-Vulkan**, the Voodoo showcase under Wine | a Glide title renders |
| W10 | Gamepads (needs a HID input stack, its own arc); IME and clipboard | an XInput pad |

### 7.4 Sizing (rough; no comparable arc has a measured spend in-tree)

Units are chunks of the DX-n kind: one coherent, audited and gated change. LOC
is new code plus patch lines, including tests.

| Group | Chunks | LOC | Comparable |
|---|---|---|---|
| K1-K7 | 15-25 (K1 and K4 dominate: a design fork, likely a spec, audits) | 8-15 K | a Vivarium-tier kernel arc |
| P1-P5 | 6-10 | 4-8 K | the Pouch socket and signal work |
| Mesa EGL + Venus as a `.so` | 2-3 | 1-2 K | Warp chunks |
| W0-W9 | 20-30 | 20-35 K (drivers 8-12 K; ntdll/server port 8-15 K; FEX 1-3 K) | about 3x DOSBox-X's arc |
| Route E probe (Boxedwine) | 4-8 | 2-5 K | about DOSBox-X DX-1..DX-4 |
| V1 + V2 (+ V3) | 3-6 | 1-3 K | DX-7 |

Lane 3b totals roughly **45-70 chunks**. That is the largest port the project
would have attempted, bigger than Vivarium to date. Rebasing the Wine patch
series (two-weekly development releases) is a standing cost on top.

### 7.5 Critical path

```
K1 ──> P1 ──> W2 ──┐
K3 ──> P2 ─────────┤
K4 ─┬> P3 ──> W3 ──┼──> W4 ──> W5 ──> W6 ──> W7 ──> W9   (Voodoo under Wine)
K5 ─┘   P4, P5 ────┘                  │
EGL in Mesa ──────────────────────────┘          W8 (audio) and W10 (pads) in parallel
```

---

## 8. Worked example: Unreal (1998)

Unreal is 32-bit x86 Windows, with Glide, D3D and software renderers (OpenGL
arrived with later patches). Five routes:

| Route | What runs | Thylacine needs | Speed | Verdict |
|---|---|---|---|---|
| **OldUnreal 227k / UT 469e under Vivarium** | Native aarch64 Linux binaries [V]. `SystemARM64/unreal-bin-arm64` needs glibc (`/lib/ld-linux-aarch64.so.1`), `libstdc++`, and the **bundled** `libSDL3.so.0` and `libopenal.so.1`. XOpenGLDrv carries GLSL 3.30/4.50/4.60 core and a GLES 3.1 path [V, binary inspected] | (1) glibc in Vivarium (a seam, `docs/DISTRO.md:1049-1053`), or Alpine's gcompat over the musl loader (untested); (2) W4-1 weft `mmap`; (3) a Linux-ABI SDL3 with a Tapestry backend, dropped in place of the bundled one; (4) GL for viv guests: OSMesa needs CAP_JIT, which no Linux syscall row reaches, so a new row, or virgl; (5) OpenAL through SDL3 audio to Nocturne | native | **shortest correct route**; game data legally from archive.org (Epic-approved installers, Nov 2024 [S]) |
| SurrealEngine via Pouch | Open-source UE1 reimplementation | C++ plus Vulkan, like vkQuake | native | immature: only UT v436 is "relatively playable" [S] |
| Boxedwine + Windows Unreal | Wine 11 inside emulated x86 Linux | Route E | everything emulated; M2 plausible, A72 doubtful *(inference)* | **cheapest Wine-shaped probe** |
| Native Wine + FEX/Box64 (WoW64) + Unreal Gold or OldUnreal Win32 | i386 PE under the translator. Renderer: XOpenGL (to EGL), D3D9 (wined3d) or Glide (nGlide-Vulkan) | everything in section 7 | translated, near-native-ish | the general answer, and the last |
| DOSBox-X Win98 + emulated Voodoo | The real 1998 game on an emulated Pentium and Voodoo 1 | DX-6 | Unreal's minimum is a P166 plus a 3D card; DOSBox-X's dynrec plateaus near 187k cycles/ms on M2 (`docs/reference/152-dosbox.md:321`), and its Voodoo is single-threaded on the same host | authentic, likely sluggish: **a measurement, not a promise** |

**Answer to "we'd need Wine/FEX for classic titles such as Unreal".**
- **True for the Windows-only long tail:** Deus Ex, Thief, Need for Speed III/IV,
  Carmageddon II and the like.
- **Not for Unreal or UT99,** where OldUnreal's native ARM64 builds make
  Vivarium the shorter path.

---

## 9. Risks

- **Size and churn.** Lane 3b is 45-70 chunks plus a permanent fork.
  - Upstream absorbs ARM work: Hangover is down to 10 patches.
  - Thylacine's patches (notes, burrows, 9P handles) are not upstreamable, and
    Wine's development releases come every two weeks.
- **Authority.**
  - Every x86-running Wine process holds CAP_JIT, with a permanent writer
    alias (`docs/JIT-ON-WX-DESIGN.md` caveat 5).
  - Windows binaries are untrusted code. The confinement story is the
    namespace (I-22, I-23), and a Wine prefix wants its own territory.
  - A wineserver that posts notes to other Procs needs I-26 authority.
- **Performance.** ARMv8.0 TSO costs on the A72. GL is CPU-only in the dev loop.
  Only one GPU game runs at a time (8 Warp contexts, 4 connections).
- **Fleet.** DXVK-class Direct3D needs a Venus host with a desktop GPU, and none
  exists (D6).
- **Thesis.** Windows' UI only lives inside a virtual-desktop pane; multi-window
  applications stay awkward (accepted in `docs/TAPESTRY.md:405-442`).
- **Licences.**
  - Wine (LGPL-2.1+), FEX and Box64 (MIT) and OpenGlide (LGPL-2.1) are
    compatible with GPLv3.
  - nGlide and dgVoodoo2 are closed freeware; shipping them in an image is an
    operator call (D7).
  - Boxedwine's GPL v2 needs its "or later" status checked.
  - Game data follows the fetch-at-build idiom and is never committed.

---

## 10. Decisions for the operator

| # | Decision | Options (recommended first) |
|---|---|---|
| D1 | Wine's route | **B, native Pouch** / E, a Boxedwine probe first (recommended as a precursor, not an alternative) / B-lite, single-Proc Madeira / A, Vivarium |
| D2 | K1 delivery model | **In-thread Plan 9 `notify`/`noted` with an editable `Ureg`** (the heritage shape) / an out-of-thread exception channel (Fuchsia/Mach) -- the fork `docs/JIT-ON-WX-DESIGN.md:148-160` left open |
| D3 | K3 low-VA shape | **A per-Proc shape at exec (phenotype-style, I-43)** / a global layout change / Madeira guest windows (no kernel change, heavy Wine/FEX patches) |
| D4 | K4 handle passing | **Build ARCH section 18.6 (9P out-of-band attach)** / Mycelium (parked) / a single-Proc Wine that needs neither |
| D5 | CAP_JIT for a process tree | One activation per process / a clearance scoped to a Wine prefix (needs an I-42 reading: elevation-only, never inherited) / a JIT broker |
| D6 | GPU fleet | Accept wined3d's D3D9-class ceiling / add a Linux host with a desktop GPU for Venus |
| D7 | Closed Glide wrappers | User-supplied nGlide / bundled / a native Glide (V5) |
| D8 | A name for the Thylacine-side Windows capability | Held candidate: **Convergence**. The thylacine is the textbook case of convergent evolution -- a marsupial that grew a wolf's shape -- and Wine is how a non-Windows lineage grows a Windows shape. Wine itself keeps its name, as DOSBox-X did |

---

## Appendix A. Defects and drift found along the way

Each item is verified at the cited lines and enqueued (session tasks #5-#8). None
is caused by this study; all are ours.

- **F1 -- PAC keys are shared by the kernel and every EL0 process.**
  - `pac_derive_keys` derives the IA/IB/DA/DB keys once, from `CNTPCT_EL0`, and
    `pac_apply_this_cpu` loads them on every CPU (`arch/arm64/start.S`, around
    lines 425-500). Nothing switches them per Proc or on EL0 entry.
  - The kernel and Pouch both build with `-mbranch-protection=pac-ret+bti`
    (`cmake/Toolchain-aarch64-thylacine.cmake:153`,
    `cmake/Toolchain-aarch64-pouch.cmake:117`).
  - So any process can `pacia`-sign values with the kernel's return-address key,
    and kernel pac-ret buys nothing against a local attacker who holds a
    kernel-stack write.
  - This is distinct from the tracked PAC-*entropy* deferral
    (`docs/handoffs/005-phase1-close.md:62`).
  - The Linux shape: per-process user keys set at exec, with the kernel key
    swapped on EL0 entry and exit.
- **F2 -- EL0 `WFI`/`WFE` kills the Proc on an EL2-entry boot.**
  - The EL2-to-EL1 drop writes `SCTLR_EL1 = 0x30D00800` (`arch/arm64/start.S`,
    around lines 100-134): nTWI (bit 16) and nTWE (bit 18) are clear.
  - MMU enable ORs in only `M|C|I` (`arch/arm64/mmu.c:766`).
  - The EL0 exception switch has no WFx case (`arch/arm64/exception.c`), so a
    trapped `wfe` falls to `snare:ill`.
  - It is latent on QEMU/KVM/HVF direct-EL1 boots, which inherit the
    hypervisor's SCTLR, and live on bare metal (Lazarus/W4) or under
    `virtualization=on`.
- **F3 -- `PROC_THREAD_MAX` (256) is a lifetime cap.**
  - `proc_thread_cap_ok` counts `p->thread_count` (`kernel/proc.c:1193`), which
    includes unreaped exited peers (`kernel/proc.c:3732`). Threads are freed only
    at Proc reap (`kernel/proc.c:5777-5793`).
  - It is documented as v1.0-accepted ("Per-Thread reaping is a v1.x extension",
    `kernel/include/thylacine/syscall.h:800-808`).
  - Any long-running process that retires and respawns workers stops being able
    to create threads after 256 spawns: thread pools, WebKit WorkQueues, Windows
    thread pools under Wine. This is K6.
- **F4 -- documentation drift** (each needs a correction through its owner):
  - `docs/VIVARIUM.md:2459` says the fault notes are catchable; they are not
    delivered (`kernel/vivarium.c:1775-1786`).
  - `docs/EXEC-LOAD-DESIGN.md:55-56, 375-396` still refuses `dlopen` permanently;
    B-1d landed on 2026-09-25.
  - `docs/ARCHITECTURE.md:171` says "anonymous-only mmap, no permission-mutation
    call", but `MAP_FILE` and `burrow_protect` exist.
  - `docs/ARCHITECTURE.md:206` says BTI is "enabled for kernel and userspace";
    user PTEs never set GP.
  - `docs/VDSO-DESIGN.md:7-9` says "no functional code yet"; the data clock page
    is as-built.
  - `docs/reference/152-dosbox.md:340` says DOSBox-X audio is stubbed; it now goes
    through Nocturne (`vault/system/userspace/ports/sub-dosbox.md:67`).
  - `docs/HALCYON.md:241-242` says "SDL-based Linux apps ... are carryable
    today"; there is no viv display path.
  - `README.md:49-50` reads as if lavapipe and the Pi GPU drivers run in the
    guest. As built, both sit on the host behind virgl/Venus (thyla-gl's
    lavapipe, thyla-pi's v3d/v3dv), and no Pi 5 is in the fleet.
  - Smaller items: `docs/RUST-STD-DESIGN.md:107, 139`; `docs/POSIX-FS-COOKBOOK.md:55`
    (`SYS_RW_MAX`); the BT0 comment at `arch/arm64/start.S:280`.

## Appendix B. Sources (external, read 2026-10-04 unless dated)

**Wine**
- github.com/wine-mirror/wine at 11.19 (2026-10-02):
  - `ANNOUNCE.md` for 9.0, 10.0, 11.0 (2026-01-13) and 11.1-11.19;
  - `loader/preloader.c`, `loader/main.c`;
  - `dlls/ntdll/unix/{loader,server,signal_arm64,virtual}.c`, `dlls/wow64/syscall.c`;
  - `server/{protocol.def,thread.c,ptrace.c,mapping.c,inproc_sync.c}`;
  - `dlls/win32u/{vulkan,opengl}.c`, `dlls/winewayland.drv`, `dlls/wined3d/{adapter_gl,adapter_vk}.c` [V].

**ARM64 Wine and the emulators**
- github.com/AndreRH/hangover: README and releases (hangover-11.0, 2026-01-13; 11.16, 2026-08-30) [V].
- github.com/FEX-Emu/FEX:
  - Readme; `Source/Windows/Common/InvalidationTracker.cpp`; `SharedCodeBufferManager.cpp`;
  - releases (FEX-2608, FEX-2609 on 2026-09-08); issue #4120; discussions #3267 and #5367 [V].
- github.com/ptitSeb/box64: `docs/COMPILE.md`, `docs/USAGE.md`, `src/custommem.c`, `src/os/os_wine.c`; v0.3.6 (2025-06-06), v0.4.4 (2026-08-02); issue #3648 [V].
- github.com/willfaust/Madeira and github.com/willfaust/FEX (PRs #1, #6, #11, #12; Sept-Oct 2026) [V].
- github.com/danoon2/Boxedwine: README and `source/emulation/cpu` [V].
- github.com/haikuports/haikuports `app-emulation/wine` [V]; gist X547/4a0a6a2a (2021-12-31) [V].
- github.com/brunodev85/winlator; github.com/GameNative/proton-wine; github.com/olegos2/mobox [V].

**Direct3D translation**
- github.com/doitsujin/dxvk: master d30be2b (2026-10-02), `dxvk_device_info.cpp`; wiki "Driver support" (2026-06-28); releases 3.0 (2026-06-25) and 3.1.1 (2026-09-15) [V].
- github.com/WinterSnowfall/d7vk (2026-09-29) [V].
- github.com/HansKristian-Work/vkd3d-proton (3.0.1, 2026-05-06) [V].
- github.com/dege-diosg/dgVoodoo2 (2.87.5, 2026-09-15) [V]; dxvk issue #5217 [V].

**GPUs**
- Mesa v3dv source (`src/broadcom/vulkan/v3dv_device.c`, `v3dvx_formats.c`, 25.0.7 and 26.1.0-devel mirrors) [V].
- github.com/qemu/qemu `docs/system/devices/virtio/virtio-gpu.rst` [V].
- github.com/utmapp/UTM releases (5.0.3, 2026-05-04; 5.0.6, 2026-09-27) [V].
- collabora.com "the state of gfx virtualization" (2025-01-15) [S].

**Glide and Voodoo**
- github.com/voyageur/openglide (2023-12-10) [V].
- nGlide 2.10 (zeus-software.com; dosbox-x.com wiki) [S].
- github.com/sezero/glide LICENSE [V].
- github.com/kjliew/qemu-3dfx [V].
- github.com/86Box/86Box (v6.0, 2026-05-31) [V].
- github.com/sarah-walker-pcem/pcem [V].
- github.com/joncampbell123/dosbox-x `dosbox-x.reference.full.conf` [V].
- github.com/dosbox-staging/dosbox-staging `src/hardware/video/voodoo.cpp` [V].
- vogons.org/viewtopic.php?t=23633 (Glide-only titles) [S].

**Unreal**
- github.com/OldUnreal/UnrealTournamentPatches:
  - release assets for v469e, including `OldUnreal-UTPatch469e-Linux-arm64.tar.bz2`;
  - v469d notes ("Support for Linux/aarch64");
  - v469f RC5 (2026-09-27) [V].
- github.com/OldUnreal/Unreal-testing v227k_15 (2024-08-16): `OldUnreal-UnrealPatch227k-Linux.tar.bz2`, downloaded and inspected (`SystemARM64/`, ELF headers, `NEEDED`, shader strings) [V].
- unrealsp.org news 2024-11-13 and gamingonlinux.com 2024-11, on Epic-approved archive.org installers [S].
- github.com/dpjudas/SurrealEngine [S].
