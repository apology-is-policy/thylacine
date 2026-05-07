# Reference: /init bringup (P3-F)

## Purpose

`/init` is the first userspace process at v1.0. It runs once during boot, validating the full kernel→exec→userspace→syscall→kernel chain in the production boot path (no test-harness scaffolding). At v1.0 P3-F it's a 9-instruction embedded blob that prints `hello\n` via `SYS_PUTS` and exits via `SYS_EXITS(0)`.

ARCH §16 defines exec as the boundary between kernel-internal Proc/Thread model and userspace. ROADMAP §6 names "/init starts and prints 'hello' via syscall" as a Phase 3 exit criterion. P3-F closes that line.

P3-F lives at the seam between two design points: (a) the v1.0 minimum viable demonstration that userspace runs (the embedded blob), and (b) the eventual real /init at Phase 4+ (loaded from Stratum / ramfs, runs as a long-lived supervisor). The boot-path machinery — rfork → exec_setup → userland_enter → wait_pid → assert exit — is identical for both; only the blob's source changes.

## Public API

### `<thylacine/joey.h>`

```c
void joey_run(void);
```

#### `joey_run()`

Single entry. Called from `boot_main()` after kernel bring-up + tests + fault_test_run + before the `Thylacine boot OK` banner. Builds the embedded ELF, rforks a child Proc, exec_setup's the blob in the child, userland_enter's to EL0, blocks the boot CPU on `wait_pid`, verifies the child exited with `status==0`. Extincts on any failure (rfork OOM / exec_setup error / wait_pid mismatch / non-zero exit_status).

Returns normally on success. Caller (boot_main) prints the boot OK banner immediately after.

## Implementation

### `kernel/joey.c`

The implementation is intentionally compact (~165 LOC including comments). Three sections:

1. **The user program**: a 9-instruction `static const u32 g_joey_program[]` array, hand-encoded AArch64 little-endian:

   | Offset | Hex | Asm | Purpose |
   |---|---|---|---|
   | 0x00 | `d2800800` | `movz x0, #0x40` | x0[15:0] = 0x40 |
   | 0x04 | `f2a00020` | `movk x0, #1, lsl #16` | x0 = 0x10040 (msg addr) |
   | 0x08 | `d28000c1` | `movz x1, #6` | x1 = 6 (msg length) |
   | 0x0C | `d2800028` | `movz x8, #1` | x8 = SYS_PUTS |
   | 0x10 | `d4000001` | `svc #0` | trap → SYS_PUTS |
   | 0x14 | `d2800000` | `movz x0, #0` | status 0 ("ok") |
   | 0x18 | `d2800008` | `movz x8, #0` | x8 = SYS_EXITS |
   | 0x1C | `d4000001` | `svc #0` | trap → SYS_EXITS (noreturn) |
   | 0x20 | `14000000` | `b .` | defensive (unreachable) |

   Encodings verified against `clang --target=aarch64-none-elf -c` disassembly during P3-F authoring.

2. **The synthetic ELF wrapper** (`build_init_elf()`): writes a minimal `Elf64_Ehdr` + one `Elf64_Phdr` into a static 8 KiB BSS buffer. Headers occupy the first page (offset 0..end-of-headers); segment data starts at file_offset = `PAGE_SIZE`. The PT_LOAD segment maps to `vaddr 0x10000`, R+X, `filesz = 0x46` (code through msg), `memsz = PAGE_SIZE`. `e_entry = 0x10000` (start of program).

3. **The boot orchestration** (`joey_run()`): builds the ELF, calls `rfork(RFPROC, joey_thunk, &args)`, the child runs `joey_thunk` which calls `exec_setup` + `userland_enter`. Parent calls `wait_pid(&status)`, asserts the reaped pid + exit_status, prints diagnostic.

### Boot-path placement

```
boot_main()
   │
   ├── ... DTB, MMU, GIC, timer, proc/thread/sched, smp ...
   │
   ├── test_run_all()                # in-kernel tests (kproc context)
   ├── fault_test_run()               # hardening proof (production no-op)
   │
   ├── joey_run()                    ← P3-F
   │      ├── build_init_elf()
   │      ├── rfork(RFPROC, joey_thunk, &args)
   │      │     │ child kthread runs joey_thunk:
   │      │     │   exec_setup(p, blob, size)
   │      │     │   userland_enter(entry, sp)  ← eret to EL0
   │      │     │ child user code:
   │      │     │   svc #0 (SYS_PUTS "hello\n")
   │      │     │   svc #0 (SYS_EXITS 0)       ← never returns
   │      └── wait_pid(&status)      ← parent reaps; verifies status==0
   │
   └── uart_puts("Thylacine boot OK\n")   # TOOLING.md §10 ABI signal
```

If joey_run extincts (rfork OOM / non-zero exit_status), tools/test.sh observes the `EXTINCTION:` prefix and reports failure.

### Memory layout of the embedded ELF

Static storage:
- `g_joey_elf_blob[8192]` in `.bss` (zero-initialized by the kernel image's BSS clear).
- `g_joey_program[36]` and `g_joey_msg[7]` in `.rodata` (kernel image read-only data).

At `joey_run()` time:
- `build_init_elf()` zeros `g_joey_elf_blob` then writes Elf64_Ehdr at offset 0, Elf64_Phdr at offset `sizeof(Elf64_Ehdr)`, code at offset `PAGE_SIZE`, message at offset `PAGE_SIZE + 0x40`.
- `exec_setup` allocates a fresh anonymous BURROW for the segment, copies the 0x46 bytes from `blob[PAGE_SIZE..]` into the BURROW via the kernel direct map.
- The user thread, on first instruction fetch at vaddr 0x10000, takes an instruction abort, gets demand-paged via `userland_demand_page` (P3-Dc) → PTE installed → retry → user code runs.

### Why hand-encoded + synthetic ELF, not a cross-compiled C binary

At v1.0 P3-F the kernel build (CMake) doesn't build any userspace artifacts. The /init blob has to come from somewhere; the lightest path is a hand-encoded program embedded in the kernel image. Phase 4 (Stratum 9P client + ramfs) and Phase 5 (sysroot + cross-toolchain) are the eventual real solution: real userspace binaries built separately, embedded in a ramfs (Phase 5) or pulled from Stratum (Phase 4+).

Tradeoff accepted: the v1.0 /init is a 9-instruction toy. It doesn't exercise libc, dynamic linking, multi-module, dynamic stack, or anything resembling a real init system. It's a milestone demonstration that the chain works, not a step toward a real /init.

## Spec cross-reference

No new TLA+ at P3-F. /init is impl-orchestration over already-spec'd primitives:
- `rfork` semantics — covered structurally by Proc lifecycle.
- `exec_setup` — covered by `burrow.tla` (mapping lifecycle) + `handles.tla` (handle transfer-via-9P invariant doesn't apply; handles dropped immediately).
- `userland_enter` — single-instruction EL transition; spec'd by ARM ARM, no concurrency.
- `wait_pid` — covered structurally by Proc lifecycle (P2-D wakeup-inside-lock pattern).

Phase 5+ /init-as-supervisor (long-running parent that respawns dead children, supervises drivers, handles SIGCHLD-like notes) will warrant a spec extension covering the supervisor/child reaping protocol with concurrency.

## Tests

P3-F retires the predecessor `userspace.exec_exits_ok` test in favor of the production `joey_run()` path. Rationale: trip-hazard #157 (second-userspace-test-iteration hang) means running TWO userspace exec'd threads sequentially within the kernel test harness reproducibly hangs the second. The prior test ran ONE userspace exec; adding /init AFTER it would have hit the bug. /init alone runs once per boot — the bug doesn't manifest, and the production path itself is the regression guard.

Trade made:
- **Loss**: a unit-tested invocation of exec_setup+userland_enter inside a try/wait_pid harness (could test specific failure modes via test_userspace's `_fail` sibling — never landed).
- **Gain**: the production boot path itself is the regression guard. If exec_setup or userland_enter regresses, /init fails to print "hello" or wait_pid sees a non-zero status, and the kernel extincts (visible to tools/test.sh).

The following tests exercise /init's component primitives in isolation (unaffected by P3-F):
- `kernel/test/test_exec.c` — 5 tests on `exec_setup` (ELF parse, segment data copy, constraints, multi-segment, lifecycle).
- `kernel/test/test_syscall.c` — 5 tests on `syscall_dispatch` (unknown nr, SYS_PUTS, SYS_EXITS ok/fail, args layout).
- `kernel/test/test_demand_page.c` — 7 tests on the demand-paging path.

The end-to-end "userspace runs in EL0" assertion is now the boot-path /init itself.

## Performance characteristics

`joey_run()` cost (measured at v1.0 P3-F, QEMU virt 4-core, KASLR enabled):

| Step | Approx cost |
|---|---|
| `build_init_elf` | ~10 µs (zero 8 KiB + write headers + copy ~50 bytes) |
| `rfork(RFPROC, joey_thunk, ...)` | ~50 µs (proc_alloc + thread_alloc + asid_alloc + pgtable_create) |
| Child `exec_setup` | ~80 µs (burrow_create_anon + 1 segment + stack BURROW; eager page allocation) |
| `userland_enter` (eret) | <1 µs |
| User code (4 SVCs equivalent) | <10 µs |
| `wait_pid` block + reap | ~20 µs |
| **Total /init phase** | **~200 µs** |

Total boot time at P3-F: ~336 ms. Under the 500 ms VISION §4 budget. The /init phase is ~0.06% of total boot — well below noise.

## Status

- **Implemented at P3-F**: `joey_run()`, embedded /init blob, boot path wired, prior `userspace.exec_exits_ok` test retired.
- **Stubbed**: real /init binary (Phase 4 ramfs / Phase 5 cross-toolchain).
- **Stubbed**: /init-as-supervisor (Phase 5+ long-running supervisor model).

Commit landing point: `00527db`.

## Known caveats / footguns

1. **The blob is hand-encoded**. Any change to `g_joey_program[]` requires re-running `clang --target=aarch64-none-elf -c` to verify the new encoding. Touch carefully; an off-by-one bit error becomes "kernel extincts on the SVC dispatch" or "user code triple-faults silently".

2. **The blob lives at vaddr 0x10000**. The `movz x0, #0x40 / movk x0, #1, lsl #16` sequence assumes the segment maps at 0x10000. If `JOEY_SEGMENT_VADDR` is changed, the encoding must match. (At v1.0 the address is fixed; Phase 5+ ASLR will load /init at a randomized base, requiring a different addressing pattern — adrp/add or a literal-pool load.)

3. **The "hello\n" message has implicit-NUL trailing**. The C string literal carries `\0` past `\n`; the SYS_PUTS length is hard-coded to `6` so the NUL stays embedded but doesn't reach UART. Changing the message text requires updating both `g_joey_msg[]` and the `movz x1, #6` instruction's immediate.

4. **/init runs as kproc's child, not as PID 1**. ARCH §7.4 + standard Plan 9 convention assigns PID 1 to /init. At v1.0 P3-F /init's pid is whatever `g_next_pid` happens to be when `rfork` runs (typically 1305+ after the test suite consumes pids); the role-of-/init logic doesn't depend on PID 1. Phase 5+ may pin /init to PID 1 by reserving the slot at proc_init.

5. **Trip-hazard #157** (second-userspace-iteration hang). Running ANY second userspace exec'd thread after /init reproducibly hangs. /init runs once per boot; the trip-hazard doesn't manifest in /init's path. If a future change adds a SECOND userspace exec (e.g., spawning a child driver process from /init), trip-hazard #157 must be investigated and resolved first.

6. **No retry / restart on /init failure at v1.0**. If /init extincts, the kernel halts. Phase 5+ will add a restart loop (the kernel can re-exec /init from a known-good blob; alternatively a recovery shell takes over). v1.0's "extinct on /init failure" is the strongest signal for tools/test.sh and the simplest semantics for the milestone.

## Naming rationale

`joey_run` (not `start_init` or `do_init`) — matches `test_run_all`, `fault_test_run` and the "*_run" pattern for boot-path entry points. The function name reads as "run the init phase of bring-up" not "start an init process" — at v1.0 they're the same thing, but the name generalizes to Phase 5+ where init becomes a long-lived supervisor and the boot-path entry is just a launcher.

`joey_thunk` (not `init_entry` or `init_main`) — "thunk" is the established term in this codebase for "the kernel function the new process's initial thread runs after rfork before the kernel-userspace transition" (mirrors `exec_thunk` from the retired test fixture). The thunk performs the kernel→user transition; it's not the user code itself.

`/init` (the path-style name in user-facing diagnostics) — reflects Plan 9 + Unix tradition. v1.0 doesn't have a filesystem so there's no actual `/init` file; the name is aspirational, anticipating Phase 4+ when /init is loaded from a real filesystem path.
