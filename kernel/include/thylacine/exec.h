// kernel-internal exec — load an ELF blob into a Proc's address space.
//
// At v1.0 P3-Eb this is a kernel-internal primitive (not yet a syscall).
// It's the bridge between the parsed-ELF representation (P2-Ga elf_load)
// and the address-space machinery (P3-Da/Db/Dc VMA + demand paging):
//
//   exec_setup(p, blob, size) →
//     elf_load(blob, size, &img)
//       │
//       ├── for each PT_LOAD segment:
//       │       burrow_create_anon(size_rounded)
//       │       copy bytes from blob to burrow->pages via direct map
//       │       burrow_map(p, burrow, vaddr, size_rounded, prot)
//       │       burrow_unref(burrow)        // mapping_count keeps alive
//       │
//       ├── allocate user-stack BURROW + burrow_map at top of user-VA
//       │
//       └── return entry + sp_top
//
// At v1.0 P3-Eb exec_setup loads only — it does NOT transition the
// caller to EL0. The asm trampoline (P3-Eb integration with Thread)
// performs the eret to EL0 once the caller is ready. Tests at P3-Eb
// validate the VMA installation; end-to-end runs of ELF in EL0 land
// at P3-Ed.
//
// Phase 5+: exec(2) syscall replaces in-place. v1.0 path: rfork + then
// the child thread calls exec_setup + transitions to EL0.

#ifndef THYLACINE_EXEC_H
#define THYLACINE_EXEC_H

#include <thylacine/types.h>

struct Proc;
struct Spoor;   // REVENANT R-4: exec_setup_from_spoor's pinned executable

// User-VA layout for v1.0 exec'd Procs:
//
//   0x0000_0000_0001_0000          User code/data (per ELF e_entry +
//   ...                             per-segment vaddr).
//   ...
//   0x0000_0000_7FFB_F000          User stack GUARD page (4 KiB —
//                                   reserved, unmapped, prot==0).
//   0x0000_0000_7FFC_0000          User stack base (EXEC_USER_STACK_BASE).
//   0x0000_0000_8000_0000          User stack TOP (initial SP_EL0).
//   0x0000_0001_0000_0000          Burrow-attach window base — the range
//   ...                             SYS_BURROW_ATTACH places anonymous
//   0x0000_4000_0000_0000          regions into (first-fit upward).
//
// The user-stack region is well below the TTBR1 split (0x0001_0000_*)
// and well above typical ELF segment vaddrs + BSS heaps. Sized 256 KiB
// at v1.0: corvus runs ML-KEM-768 (FIPS 203) keygen/decapsulate, whose
// FO-transform working set is tens of KiB of stack — the prior 16 KiB
// overflowed. 256 KiB is generous headroom for every userspace Proc;
// Phase 5+ replaces the fixed size with demand-grow on stack faults.
//
// P5-secondary-stack-guard: a 4 KiB guard page sits directly below
// EXEC_USER_STACK_BASE, installed by exec_map_user_stack as a prot==0
// / no-BURROW guard VMA (vma_alloc_guard). An overflow past the 256 KiB
// stack crosses into it and faults — userland_demand_page rejects the
// prot==0 VMA — and vma_insert's overlap rejection reserves the page so
// a future mapping allocator (Phase 5+ mmap / heap) cannot place
// anything flush against the stack. Closes corvus-bringup-d audit F7.
#define EXEC_USER_STACK_SIZE         (256ull * 1024)
#define EXEC_USER_STACK_TOP          0x0000000080000000ull
#define EXEC_USER_STACK_BASE         (EXEC_USER_STACK_TOP - EXEC_USER_STACK_SIZE)
#define EXEC_USER_STACK_GUARD_SIZE   0x1000ull
#define EXEC_USER_STACK_GUARD_BASE   (EXEC_USER_STACK_BASE - EXEC_USER_STACK_GUARD_SIZE)

// P6-pouch-mem: the burrow-attach window — the user-VA range SYS_BURROW_
// ATTACH places anonymous Burrows into (ARCHITECTURE.md §6.5, Tier 1).
// The base sits at 4 GiB, well above the user stack TOP (0x8000_0000),
// so an attached region can never collide with the ELF image (low VAs),
// the stack, or the stack guard. The top (64 TiB) is well under
// USER_VA_TOP (2^47 = 128 TiB), the hard ceiling burrow_map enforces;
// the headroom above is reserved for future Tier-2 (handle-backed
// Burrow) placement. The kernel first-fit-scans the window via
// vma_find_gap — userspace never chooses an address (no MAP_FIXED).
#define EXEC_USER_BURROW_BASE   0x0000000100000000ull
#define EXEC_USER_BURROW_TOP    0x0000400000000000ull

// The burrow window must not overlap the user stack / guard / ELF
// region — the isolation guarantee that SYS_BURROW_ATTACH never lands
// on, and SYS_BURROW_DETACH never dismantles, the stack rests on this
// inequality. And it must stay under USER_VA_TOP (2^47), the hard
// ceiling burrow_map enforces. Pinned at build time (F3, P6-pouch-mem-a
// audit) so a future edit to either constant cannot silently break it.
_Static_assert(EXEC_USER_BURROW_BASE >= EXEC_USER_STACK_TOP,
               "burrow-attach window must sit above the user stack");
_Static_assert(EXEC_USER_BURROW_BASE < EXEC_USER_BURROW_TOP,
               "burrow-attach window must be non-empty");
_Static_assert(EXEC_USER_BURROW_TOP <= (1ull << 47),
               "burrow-attach window must stay under USER_VA_TOP (2^47)");

// vDSO clock page (docs/VDSO-DESIGN.md): the single READ-ONLY kernel
// timekeeping page mapped into every exec'd Proc so native userspace reads
// CLOCK_MONOTONIC/_REALTIME from CNTVCT_EL0 + this page without a syscall. It
// sits at a fixed VA in the free 2-4 GiB gap between the user-stack TOP
// (0x8000_0000) and the burrow-attach window base (0x1_0000_0000) — so it can
// never collide with the ELF image (low VAs), the stack, the guard, or an
// attached anon region. The kernel maps it (no MAP_FIXED) and delivers its VA
// in the AT_VDSO_CLOCK auxv entry. One page; the same physical page shared
// across every Proc.
#define EXEC_USER_VDSO_BASE   0x00000000C0000000ull   // 3 GiB
#define EXEC_USER_VDSO_SIZE   0x1000ull
_Static_assert(EXEC_USER_VDSO_BASE >= EXEC_USER_STACK_TOP,
               "vDSO page must sit above the user stack");
_Static_assert(EXEC_USER_VDSO_BASE + EXEC_USER_VDSO_SIZE <= EXEC_USER_BURROW_BASE,
               "vDSO page must sit below the burrow-attach window");
_Static_assert((EXEC_USER_VDSO_BASE & 0xFFFull) == 0,
               "vDSO page VA must be page-aligned");

// Initial process stack — the System V process-startup frame exec_setup
// (and exec_setup_with_argv) builds at the very top of the user stack
// (POUCH-DESIGN.md §12.1). A C runtime (pouch — the Thylacine POSIX libc)
// reads argc/argv/envp and the auxiliary vector from here.
//
// Two shapes:
//
// Shape A — "no argv" (legacy exec_setup; backward-compat for v1.0
// callers that have not yet adopted the argv-bearing entry):
//   sp + 0      argc                  u64 — 0
//   sp + 8      argv[] terminator     one NULL pointer
//   sp + 16     envp[] terminator     one NULL pointer
//   sp + 24     auxv[]                EXEC_INIT_AUXV_COUNT × Elf64_auxv_t
//                                      (AT_PHDR, AT_PHENT, AT_PHNUM,
//                                       AT_PAGESZ, AT_HWCAP, AT_RANDOM,
//                                       [AT_VDSO_CLOCK], AT_NULL)
//   sp + ...    (8 bytes padding)
//   sp + ...    AT_RANDOM entropy     16 kernel-CSPRNG bytes
//   sp + 176    EXEC_USER_STACK_TOP
// Frame size is the fixed EXEC_INIT_STACK_SIZE = 176 bytes (room for the max 8
// auxv entries reserved; AT_VDSO_CLOCK is present only when the vDSO page
// mapped, else its slot stays unused before the AT_NULL terminator).
//
// Shape B — "argv-bearing" (exec_setup_with_argv, P6-pouch-stratumd-boot
// sub-chunk 16b-alpha):
//   sp + 0                        argc                  u64 = argc
//   sp + 8                        argv[0]               u64 = user-VA pointing
//                                                        into the strings region
//   sp + 8 + 8*i                  argv[i]               (i = 1..argc-1)
//   sp + 8 + 8*argc               argv[argc]            u64 = 0 (terminator)
//   sp + 16 + 8*argc              envp[0]               u64 = 0 (no envp at v1.0)
//   sp + 24 + 8*argc              auxv[]                up to 8 × 16 bytes (same
//                                                        entries as Shape A;
//                                                        AT_RANDOM points to the
//                                                        AT_RANDOM block below)
//   sp + 152 + 8*argc             (pad to next 16-align)
//   sp + R                        AT_RANDOM entropy     16 kernel-CSPRNG bytes
//                                                        (R is the 16-aligned
//                                                        offset; the AT_RANDOM
//                                                        auxv entry's value is
//                                                        sp + R)
//   sp + R + 16                   argv strings region   argv_data_len bytes,
//                                                        concatenated NUL-
//                                                        terminated; argv[i]
//                                                        pointers above point
//                                                        into here
//   sp + frame_size               EXEC_USER_STACK_TOP   (frame_size rounded up
//                                                        to 16-byte alignment)
// Frame size is variable, bounded above by EXEC_INIT_STACK_MAX_SIZE.
//
// In both shapes, initial sp = EXEC_USER_STACK_TOP - frame_size; sp is
// 16-byte aligned because the frame size is rounded up to a 16-byte
// multiple and EXEC_USER_STACK_TOP is aligned.
// 8 = AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_HWCAP, AT_RANDOM,
// AT_VDSO_CLOCK, AT_NULL. The frame ALWAYS reserves room for all 8 (so the
// AT_RANDOM block + strings region offsets are stable); when the vDSO page
// is absent the builder writes the AT_NULL terminator after 6 entries and
// the AT_VDSO_CLOCK slot stays unused (zeroed) padding before the random
// block. AT_HWCAP is unconditional (the hwcap word exists on every boot).
#define EXEC_INIT_AUXV_COUNT   8
#define EXEC_INIT_STACK_SIZE \
    (((8 + 8 + 8 + EXEC_INIT_AUXV_COUNT * 16 + 16) + 15) & ~15ull)
// Offset (from the frame base / from sp) of the 16-byte AT_RANDOM block
// under Shape A (no argv). Shape B computes the offset dynamically.
#define EXEC_INIT_RANDOM_OFFSET   (EXEC_INIT_STACK_SIZE - 16)
// Maximum frame size under Shape B (argv-bearing). Bounds the kernel
// validation + the EXEC_USER_STACK_SIZE budget: with argc = 512 (=
// SYS_SPAWN_ARGV_MAX) and argv_data_len = 64 KiB (= SYS_SPAWN_ARGV_DATA_MAX,
// both raised for the on-device Go toolchain's compile/link command lines),
// the structured top is (8 + 8*513 + 8 + AUXV*16 + 16-align-pad + 16) bytes,
// followed by 64 KiB strings bytes, rounded up to 16. ~68 KiB — still well
// under the 256 KiB user-stack budget; the _Static_assert below proves it.
#define EXEC_INIT_STACK_MAX_SIZE \
    (((8 + (512u + 1u) * 8 + 8 + EXEC_INIT_AUXV_COUNT * 16 + 16 + 16 + 65536) + 15) & ~15ull)
_Static_assert(EXEC_INIT_STACK_SIZE % 16 == 0,
               "initial sp must be 16-byte aligned (AArch64 SysV ABI)");
_Static_assert(EXEC_INIT_STACK_MAX_SIZE % 16 == 0,
               "Shape-B frame max-size must be 16-byte aligned");
_Static_assert(EXEC_INIT_STACK_MAX_SIZE < EXEC_USER_STACK_SIZE,
               "Shape-B max frame must fit under the user stack budget");

// exec_setup — parse the ELF blob, populate `p`'s VMA tree, set up the
// user stack mapping.
//
// Constraints (v1.0):
//   - `p` must be a non-kproc Proc (pgtable_root != 0; kproc has 0).
//   - `p` must currently have NO VMAs (clean address space; otherwise
//     overlap rejection or undefined behavior on overlapping segment).
//     A future "exec replaces" semantics (Phase 5+) will tear down
//     existing VMAs first.
//   - Each PT_LOAD segment's `vaddr` and `file_offset` must be
//     page-aligned (low 12 bits zero). The ELF spec permits non-zero
//     low bits as long as `vaddr ≡ offset (mod p_align)`; v1.0 P3-Eb
//     rejects non-zero alignment for simplicity. Real toolchain output
//     (clang, gcc) page-aligns segments by default.
//
// Side effects on success:
//   - Each PT_LOAD segment installed as a VMA backed by a fresh
//     anonymous BURROW. The BURROW's pages are populated with bytes from the
//     blob (filesz bytes from blob[file_offset..]); tail (memsz -
//     filesz) is zero (KP_ZERO from alloc).
//   - User stack VMA installed at `[EXEC_USER_STACK_BASE,
//     EXEC_USER_STACK_TOP)` backed by a fresh anonymous BURROW (zeroed).
//   - Caller-held BURROW handles dropped via burrow_unref; mapping_count
//     (held by the VMA) keeps each BURROW alive until proc_free's
//     vma_drain.
//
// Side effects on failure:
//   - Whatever VMAs were installed before the failing step remain
//     installed. The Proc is in a partial state. v1.0 callers should
//     dispose of the Proc (proc_free with state=ZOMBIE) on any
//     non-zero return.
//
// Returns 0 on success; -1 on any failure (ELF parse error, alignment
// violation, VMA SLUB OOM, page allocation OOM).
//
// `*entry_out`: ELF e_entry (the EL0 entry PC). Caller writes to
//               ELR_EL1 before the eret to EL0.
// `*sp_out`:    the initial SP_EL0 — `EXEC_USER_STACK_TOP -
//               EXEC_INIT_STACK_SIZE`, pointing at the `argc` word of
//               the System V startup frame (see the layout above).
//               Caller writes it to SP_EL0 before the eret to EL0.
int exec_setup(struct Proc *p, const void *blob, size_t blob_size,
               u64 *entry_out, u64 *sp_out);

// exec_setup_with_argv — extended entry that threads argv through to the
// initial-stack layout (P6-pouch-stratumd-boot sub-chunk 16b-alpha). Same
// contract as exec_setup but the System V frame carries argc + argv[]
// pointing into a strings region populated from argv_data.
//
// argv_data: kernel-resident buffer of `argv_data_len` bytes containing
//            `argc` concatenated NUL-terminated strings. May be NULL iff
//            argv_data_len == 0 AND argc == 0 (degrades to exec_setup's
//            "no argv" behavior — frame is the legacy fixed-size shape).
//
// argv_data_len: total bytes including NULs. Bounded by
//                SYS_SPAWN_ARGV_DATA_MAX (4096). Last byte MUST be NUL
//                if argv_data_len > 0.
//
// argc: number of NULs in argv_data (i.e., number of strings). Bounded
//       by SYS_SPAWN_ARGV_MAX (512). Caller's invariant: argv_data has
//       exactly `argc` NUL terminators.
//
// Returns 0 on success; -1 on any failure (ELF parse error, alignment
// violation, VMA SLUB OOM, page allocation OOM, argv invariant
// violation). The argv_data buffer is read-only from this function's
// perspective; the caller retains ownership and is responsible for
// freeing it after this returns.
int exec_setup_with_argv(struct Proc *p, const void *blob, size_t blob_size,
                         const char *argv_data, u32 argv_data_len, u32 argc,
                         u64 *entry_out, u64 *sp_out);

// REVENANT R-4: bounds for the file-backed exec path (exec_setup_from_spoor).
//
// EXEC_ELF_HEADER_MAX — the eager read of the ELF header + program-header
// table. A static aarch64 ET_EXEC has ehdr(64) + a handful of phdrs(56 each);
// 16 KiB covers the worst case the loader permits (ELF_MAX_PHNUM=256 phdrs ->
// 64 + 256*56 = 14400 bytes) with margin. The phdr table MUST reside within
// the first EXEC_ELF_HEADER_MAX bytes (every real toolchain emits phoff=64);
// a binary whose phdrs sit beyond it is rejected. This replaces the old whole-
// binary SYS_SPAWN_BLOB_MAX wall: only the header is read eagerly, so a binary
// of any size execs (its text demand-pages, its data eager-copies per-segment).
#define EXEC_ELF_HEADER_MAX  16384u

// EXEC_FILE_MAX — a sanity ceiling on the executable's total size (from stat),
// bounding the loader's arithmetic + rejecting a pathological "file." Generous
// (the #67 toolchain is tens of MiB); per-segment OOM is still graceful.
#define EXEC_FILE_MAX  (256ull * 1024 * 1024)

// exec_setup_from_spoor — the file-backed production exec path (REVENANT R-4).
//
// Unlike exec_setup (which eager-copies a whole in-memory ELF blob — retained
// for the kernel test suite), this reads ONLY the ELF header + phdrs from the
// pinned executable `exe` (a Spoor resolved by exec_resolve_from_namespace),
// then maps each PT_LOAD:
//   - executable text (PF_X, no bss page beyond the file) -> a SHARED file-
//     backed BURROW_TYPE_FILE via the Image cache (image_lookup_or_create),
//     demand-paged by the R-2 fault arm; R+X (W^X-clean by construction).
//   - everything else (data/bss/rodata, or a rare PF_X segment with whole bss
//     pages) -> a PRIVATE anonymous BURROW eager-copied from the file (filesz
//     bytes; the memsz tail zero-filled). No userspace file-backed writable
//     mapping is ever created.
//
// `exe_size` is the executable's total byte size (from the caller's stat in the
// parent context); it bounds the ELF loader's per-segment file-extent checks.
// BORROWS `exe` (does NOT consume it): the caller (the spawn thunk) retains the
// pin and spoor_clunks it after this returns. Internally spoor_refs `exe` per
// text-segment Image lookup (which consumes a ref) and dev->reads the header +
// data segments through the borrowed `exe` (death-interruptible, #811).
//
// Same `*entry_out` / `*sp_out` contract + same partial-state-on-failure
// disposal contract as exec_setup. argv threads through exactly as
// exec_setup_with_argv (argc==0 + argv_data==NULL is the no-argv shape).
int exec_setup_from_spoor(struct Proc *p, struct Spoor *exe, size_t exe_size,
                          const char *argv_data, u32 argv_data_len, u32 argc,
                          u64 *entry_out, u64 *sp_out);

// Kernel→EL0 transition (asm in arch/arm64/userland.S). Sets
// ELR_EL1 = entry_pc, SP_EL0 = user_sp, SPSR_EL1 = 0 (PSTATE = EL0t,
// all DAIF clear), zeros every GPR, then eret. Never returns —
// transitions atomically to EL0.
//
// At v1.0 P3-Ed this is the only path from kernel to EL0. Phase 5+
// syscall-return path uses the existing exception-exit ERET via
// .Lexception_return for the syscall-return case.
__attribute__((noreturn))
extern void userland_enter(u64 entry_pc, u64 user_sp);

#endif // THYLACINE_EXEC_H
