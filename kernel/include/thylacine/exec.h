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

#include <thylacine/elf.h>
#include <thylacine/types.h>

struct Proc;
struct Spoor;   // REVENANT R-4: exec_setup_from_spoor's pinned executable

// User-VA layout for v1.0 exec'd Procs:
//
//   0x0000_0000_0001_0000          User code/data (per ELF e_entry +
//   ...                             per-segment vaddr).
//   ...
//   0x0000_0000_7FEF_F000          User stack GUARD page (4 KiB —
//                                   reserved, unmapped, prot==0).
//   0x0000_0000_7FF0_0000          User stack base (EXEC_USER_STACK_BASE).
//   0x0000_0000_8000_0000          User stack TOP (initial SP_EL0).
//   0x0000_0001_0000_0000          Burrow-attach window base — the range
//   ...                             SYS_BURROW_ATTACH places anonymous
//   0x0000_4000_0000_0000          regions into (first-fit upward).
//
// The user-stack region is well below the TTBR1 split (0x0001_0000_*)
// and well above typical ELF segment vaddrs + BSS heaps. Sized 1 MiB
// (G-7b; was 256 KiB): a real ported program's call graph (TyrQuake's
// model loader) overflowed 256 KiB into the guard page. 1 MiB is eager
// headroom for every userspace Proc; the 0x8000_0000..0xC000_0000 gap
// above TOP leaves room to grow. The Linux-model lazy demand-grown stack
// (commits only touched pages — no eager per-Proc cost) is the tracked
// v-next lift.
//
// P5-secondary-stack-guard: a 4 KiB guard page sits directly below
// EXEC_USER_STACK_BASE, installed by exec_map_user_stack as a prot==0
// / no-BURROW guard VMA (vma_alloc_guard). An overflow past the stack
// crosses into it and faults — userland_demand_page rejects the
// prot==0 VMA — and vma_insert's overlap rejection reserves the page so
// a future mapping allocator (Phase 5+ mmap / heap) cannot place
// anything flush against the stack. Closes corvus-bringup-d audit F7.
//
// G-7b: 1 MiB (was 256 KiB). The original 256 KiB was too small for a
// real ported program's call graph — TyrQuake's model loader
// (Mod_ForName -> Mod_LoadAliasModel, large on-stack temp buffers)
// overflowed it into the guard page during the first map load. 1 MiB is
// eager-anon (whole thing committed at exec, like the ELF data), so it
// stays modest: ~1 MiB * boot-Proc-count, trivial against RAM, and the
// 0x80000000..0xC0000000 gap above STACK_TOP leaves 1 GiB of headroom to
// grow down further. The proper Linux-model answer — a large lazy
// (demand-grown) reservation that commits only touched pages — is the
// tracked v-next lift (it needs the exec frame-fill to pre-commit just
// the top page; the overcommit BURROW_TYPE_ANON_LAZY infra already
// exists). This bump unblocks real ports now at a bounded eager cost.
#define EXEC_USER_STACK_SIZE         (1024ull * 1024)
#define EXEC_USER_STACK_TOP          0x0000000080000000ull
#define EXEC_USER_STACK_BASE         (EXEC_USER_STACK_TOP - EXEC_USER_STACK_SIZE)
#define EXEC_USER_STACK_GUARD_SIZE   0x1000ull
#define EXEC_USER_STACK_GUARD_BASE   (EXEC_USER_STACK_BASE - EXEC_USER_STACK_GUARD_SIZE)

// DISTRO D-2: the PIE window (elf.h) is part of THIS plan, so the two
// constants are pinned against each other here rather than trusted to stay
// consistent. elf_load refuses any ET_DYN segment whose biased top passes
// ELF_PIE_LOAD_LIMIT, so this inequality is what makes "a PIE can never be
// placed into the stack guard, the stack, or anything above it" a build-time
// fact instead of an arithmetic argument about particular binaries.
_Static_assert(ELF_PIE_LOAD_LIMIT <= EXEC_USER_STACK_GUARD_BASE,
               "the PIE window must end below the user stack guard");

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

// Initial process stack — the System V process-startup frame every exec
// entry builds at the very top of the user stack (POUCH-DESIGN.md §12.1).
// A C runtime (pouch, musl, the Go runtime) reads argc / argv / envp and
// the auxiliary vector from here.
//
//   sp + 0                        argc                  u64
//   sp + 8 + 8*i                  argv[i]               i = 0..argc-1; user VAs
//                                                        into the strings region
//   sp + 8 + 8*argc               argv[argc]            u64 = 0 (terminator)
//   sp + 16 + 8*argc + 8*j        envp[j]               j = 0..envc-1
//   sp + 16 + 8*argc + 8*envc     envp[envc]            u64 = 0 (terminator)
//   sp + 24 + 8*argc + 8*envc     auxv[]                EXEC_INIT_AUXV_COUNT
//                                                        × Elf64_auxv_t (AT_PHDR,
//                                                        AT_PHENT, AT_PHNUM,
//                                                        AT_PAGESZ, AT_HWCAP,
//                                                        AT_RANDOM, AT_ENTRY,
//                                                        [AT_VDSO_CLOCK], AT_NULL)
//   sp + ...                      (pad to the next 16-align)
//   sp + R                        AT_RANDOM entropy     16 kernel-CSPRNG bytes
//                                                        (R is that 16-aligned
//                                                        offset; the AT_RANDOM
//                                                        auxv value is sp + R)
//   sp + R + 16                   argv strings          argv_data_len bytes,
//                                                        concatenated NUL-
//                                                        terminated
//   sp + R + 16 + argv_data_len   envp strings          env_data_len bytes, same
//                                                        packing ("NAME=VALUE\0"
//                                                        records)
//   sp + frame_size               EXEC_USER_STACK_TOP   (frame_size rounded up
//                                                        to 16-byte alignment)
//
// Initial sp = EXEC_USER_STACK_TOP - frame_size, 16-byte aligned because the
// frame size is rounded up to a 16-byte multiple and the TOP is aligned.
// Frame size is variable, bounded above by EXEC_INIT_STACK_MAX_SIZE, and
// exactly EXEC_INIT_STACK_SIZE when argc and envc are both zero.
//
// ONE SHAPE, WHICH IS A #140 CORRECTION AND NOT A TIDY-UP. Until L-6c this
// was two: a fixed 176-byte "no argv" Shape A and a variable "argv-bearing"
// Shape B. Each wrote a lone NULL for envp — `w[2] = 0` in one, `w[2+argc] =
// 0` in the other — so the bug this layout exists to fix had TWO homes and
// fixing one would have left the other. The unified builder produces a
// byte-identical frame for the old Shape A case (the assertion below pins
// that arithmetic), so nothing that used to take Shape A can observe the
// change; what it buys is that envp now has exactly one place to be wrong.
//
// 9 = AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_HWCAP, AT_RANDOM,
// AT_ENTRY, AT_VDSO_CLOCK, AT_NULL. The frame ALWAYS reserves room for all 9
// (so the AT_RANDOM block + strings region offsets are stable); when the vDSO
// page is absent the builder writes the AT_NULL terminator after 7 entries and
// the AT_VDSO_CLOCK slot stays unused (zeroed) padding before the random
// block. AT_HWCAP is unconditional (the hwcap word exists on every boot).
//
// AT_ENTRY joined at DISTRO D-2, placed after AT_RANDOM and BEFORE the
// optional AT_VDSO_CLOCK. Every reader scans for its tag, so position is free;
// what the placement buys is that the seven MANDATORY entries keep the frame
// indices they had, and only the optional tail shifts by one 16-byte slot.
#define EXEC_INIT_AUXV_COUNT   9

// Environment bounds (#140). The env block is packed exactly like argv: a
// run of NUL-terminated strings, here "NAME=VALUE" records, with envc equal
// to the NUL count.
//
// THESE ARE A DELIBERATE CHOICE, NOT THE PRODUCT OF THE TWO /env MAXIMA.
// ENV_MAX_ENTRIES (64) × (ENV_NAME_MAX + ENV_VALUE_MAX) is 260 KiB, which
// beside the 64 KiB argv bound would put ~330 KiB of FRAME under a 1 MiB
// stack. That still passes the _Static_assert below — the assert only proves
// the frame FITS — while leaving ~690 KiB of actual program stack against a
// call graph G-7b measured overflowing 256 KiB (TyrQuake's model loader).
// The failure it would buy is a squeezed stack, which the build cannot see.
//
// So the env block is bounded independently, on Linux's model (argv+envp
// bounded as a fraction of the stack: 1/4 of RLIMIT_STACK, floor 32 pages).
// A quarter of our 1 MiB is 256 KiB; 64 KiB argv + 32 KiB env is 96 KiB,
// comfortably inside it, and leaves ~920 KiB of program stack. Against
// ENV_MAX_ENTRIES that is 512 bytes per variable on average — far above a
// realistic NAME=VALUE, so no plausible environment reaches it, and one that
// does is refused with T_E_2BIG rather than silently truncated.
//
// EXEC_ENV_MAX bounds the POINTER VECTOR independently, because the data
// bound alone does not: 32 KiB of minimal "A=\0" records would be ~10900
// entries and 87 KiB of envp[] pointers. 512 mirrors SYS_SPAWN_ARGV_MAX. The
// /env projection can never approach it (ENV_MAX_ENTRIES is 64); it is the
// vivarium's user-supplied envp walk that needs the ceiling.
#define EXEC_ENV_MAX        512u
#define EXEC_ENV_DATA_MAX   32768u

// The frame size when argc == 0 AND envc == 0 — one word of argc, one NULL
// each for argv[] and envp[], the auxv block, and the 16-byte AT_RANDOM
// block, rounded to 16. This is the frame a Proc with no arguments and an
// empty environment gets, and the value the builder's general arithmetic
// must reduce to (see the assertion below).
#define EXEC_INIT_STACK_SIZE \
    (((8 + 8 + 8 + EXEC_INIT_AUXV_COUNT * 16 + 16) + 15) & ~15ull)
// Offset (from the frame base / from sp) of the 16-byte AT_RANDOM block in
// that same empty-frame case. The builder computes the offset dynamically;
// this is what it arrives at when both counts are zero.
#define EXEC_INIT_RANDOM_OFFSET   (EXEC_INIT_STACK_SIZE - 16)

// The general structured-region size (everything above the AT_RANDOM block):
// argc, argv[] + its terminator, envp[] + its terminator, and the auxv block.
#define EXEC_INIT_STRUCTURED(argc, envc) \
    (8ull + ((u64)(argc) + 1ull) * 8ull + ((u64)(envc) + 1ull) * 8ull \
     + (u64)EXEC_INIT_AUXV_COUNT * 16ull)
// The whole frame: structured region rounded to 16, the AT_RANDOM block,
// then both strings regions, rounded to 16.
#define EXEC_INIT_FRAME_SIZE(argc, envc, argv_len, env_len) \
    (((((EXEC_INIT_STRUCTURED(argc, envc) + 15ull) & ~15ull) + 16ull \
       + (u64)(argv_len) + (u64)(env_len)) + 15ull) & ~15ull)

// Maximum frame size. Bounds the kernel's transient staging buffer + the
// EXEC_USER_STACK_SIZE budget at the corner of every bound at once: argc =
// SYS_SPAWN_ARGV_MAX (512) with argv_data_len = SYS_SPAWN_ARGV_DATA_MAX
// (64 KiB, both raised for the on-device Go toolchain's command lines), and
// envc = EXEC_ENV_MAX with env_data_len = EXEC_ENV_DATA_MAX. ~104 KiB (was
// ~68 KiB before the environment) — still far under the 1 MiB user-stack
// budget; the _Static_assert below proves it.
#define EXEC_INIT_STACK_MAX_SIZE \
    EXEC_INIT_FRAME_SIZE(512u, EXEC_ENV_MAX, 65536u, EXEC_ENV_DATA_MAX)
_Static_assert(EXEC_INIT_STACK_SIZE % 16 == 0,
               "initial sp must be 16-byte aligned (AArch64 SysV ABI)");
_Static_assert(EXEC_INIT_STACK_MAX_SIZE % 16 == 0,
               "max frame size must be 16-byte aligned");
_Static_assert(EXEC_INIT_STACK_MAX_SIZE < EXEC_USER_STACK_SIZE,
               "max frame must fit under the user stack budget");
// The unification claim, pinned rather than asserted in prose: the general
// arithmetic at argc == 0, envc == 0 must reproduce the empty-frame constants
// EXACTLY, or a caller that passes no argv and has no environment would
// silently get a different frame than the one documented above (and than the
// one exec.h has promised since 16b-alpha). A future edit to the auxv count
// or the padding rule cannot drift the two apart without failing here.
_Static_assert(EXEC_INIT_FRAME_SIZE(0, 0, 0, 0) == EXEC_INIT_STACK_SIZE,
               "the general frame arithmetic must reduce to the empty frame");
_Static_assert(((EXEC_INIT_STRUCTURED(0, 0) + 15ull) & ~15ull)
                   == EXEC_INIT_RANDOM_OFFSET,
               "the general AT_RANDOM offset must reduce to the empty one");

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
// `prog_name` is the DISTRO D-4 parameter: the name the CALLER used for this
// program, carried down so the PT_INTERP rewrite can put it on the
// interpreter's command line. It is NOT `exe->path` deliberately -- I-33 says
// the Spoor's Path is cosmetic and no syscall result may turn on it, and an
// exec that failed because a path-alloc OOM'd would be exactly that. NULL/0 is
// legal and means "this entry cannot produce a dynamic image", which is the
// honest answer for the two native-only spawn thunks.
int exec_setup_from_spoor(struct Proc *p, struct Spoor *exe, size_t exe_size,
                          const char *prog_name, u32 prog_name_len,
                          const char *argv_data, u32 argv_data_len, u32 argc,
                          u64 *entry_out, u64 *sp_out);

// exec_load_into -- the same file-backed load, into an EXPLICIT address space
// (LINEAGE L-2). exec_setup_from_spoor is the wrapper: it passes p->as, p's I-32
// exemption, and additionally applies the two Proc-side stamps (process name,
// exe_path).
//
// Those stamps are the reason this split exists rather than a plain parameter
// add. execve must be able to fail with the caller COMPLETELY untouched, and a
// name stamped before a load that then fails would leave a live Proc advertising
// a binary it is not running. The spawn path cannot observe the distinction (its
// Proc is a fresh child, discarded on failure), so the stamps stay there.
//
// `as` must be a clean address space (no VMAs) that no other thread can reach --
// either a freshly rforked child's, or one addrspace_alloc'd for this load.
// `exempt` is the I-32 policy verdict for the Proc this is being built for.
//
// On failure the target is left PARTIALLY POPULATED and the caller owns the
// teardown: vma_drain_in + addrspace_unref for a detached target, or the
// existing dispose-the-Proc rollback for a spawn.
//
// The environment arrives as DATA (#140), packed exactly like argv, and NOT as
// a Proc to read it from -- which is forced by the same thing that forced this
// entry to exist. Reading p->env here would read the OLD environment, because
// at this moment the Proc still has it: L-2a builds the new image detached and
// commits only after everything failable has succeeded. `exec_stage_env`
// projects a Proc's own /env into this shape for the callers that want it.
// `nsp` is the DISTRO D-4 parameter and is READ-ONLY here, in the strict sense:
// this function never writes a field of it. It supplies exactly two things --
// the phenotype that gates the PT_INTERP rewrite, and the namespace the
// interpreter path resolves through. NULL disables the rewrite entirely (the
// kernel test entries pass it), which restores the pre-D-4 behaviour exactly:
// a PT_INTERP binary is refused. Passing the Proc rather than a
// `bool pheno_linux` + a Territory keeps the two halves of one decision from
// being supplied by two arguments that a caller could disagree about.
//
// It is deliberately NOT the "Proc being modified" -- that is `as`'s owner, and
// on the execve path the two are the same Proc while `as` is still detached.
// Nothing below may start writing through `nsp`; the whole reason this entry
// takes an AddrSpace is that the Proc-side mutations are the caller's.
struct AddrSpace;
struct Proc;
// Design D (VIVARIUM 13.10.4): `pheno` is the phenotype the image being loaded
// was DECIDED to have (phenotype_decide at the resolve). The loader consults
// the parameter -- never nsp->phenotype -- wherever the phenotype shapes the
// load (the PT_INTERP dispatch), because for execve the field is not updated
// until the commit AFTER this returns: a native caller execve'ing a dynamic
// /viv/bin binary decides Linux at the resolve while its field still says
// native (review F1 Leg C). The loader never writes nsp->phenotype (Leg B).
int exec_load_into(struct AddrSpace *as, bool exempt, struct Proc *nsp,
                   u32 pheno,
                   struct Spoor *exe, size_t exe_size,
                   const char *prog_name, u32 prog_name_len,
                   const char *argv_data, u32 argv_data_len, u32 argc,
                   const char *env_data, u32 env_data_len, u32 envc,
                   u64 *entry_out, u64 *sp_out);

// The argv bounds, MIRRORED from syscall.h (SYS_SPAWN_ARGV_MAX /
// SYS_SPAWN_ARGV_DATA_MAX). exec.c deliberately does not include syscall.h --
// it includes back into this surface -- and until D-4 the mirror was two bare
// literals inside exec_build_init_stack with a comment saying what they
// mirrored. #178 tracks the absence of a tool that cross-checks the ABI
// mirrors; naming them here does one better than another comment, because
// syscall.c sees BOTH spellings and carries a _Static_assert that they agree.
#define EXEC_ARGV_MAX       512u
#define EXEC_ARGV_DATA_MAX  65536u

// exec_interp_argv -- the DISTRO D-4 argv block for the interpreter route:
//
//     [interp, "--argv0", orig_argv0, "--", orig_path, orig argv[1..]]
//
// Returns a kmalloc'd block (caller kfree's) packed exactly like every other
// argv here -- concatenated NUL-terminated strings, `*out_argc` of them, the
// last byte a NUL -- or NULL if it does not fit the argv bounds, the input is
// mis-packed, or the allocation fails.
//
// Exported ONLY so the kernel test can assert the block byte-for-byte. That
// assertion is not optional coverage: exec_build_init_stack EXTINCTS when the
// NUL count disagrees with argc, so an off-by-one in the slot arithmetic is a
// dead kernel rather than a failed exec. It is also the only place the
// `--argv0` claim can be discriminated at all -- nothing in a container can
// produce a vector whose argv[0] differs from the path it resolved.
char *exec_interp_argv(const char *interp, u32 interp_len,
                       const char *path, u32 path_len,
                       const char *argv_data, u32 argv_data_len, u32 argc,
                       u32 *out_len, u32 *out_argc);

// exec_resolve_from_namespace (#58 / REVENANT R-4) -- resolve `name` in `p`'s
// namespace and PIN the resulting executable Spoor (the ref transfers to the
// caller, which spoor_clunks it). OEXEC-gated: a per-component X-search on
// every directory hop and PERM_R|PERM_X on the leaf (I-28).
//
// DEFINED IN syscall.c, next to the SYS_SPAWN_* family that is its main caller
// -- it needs stalk + Territory + the LS-4 cwd join, all of which live there.
// Declared here because DISTRO D-4 made exec.c a caller: the interpreter path
// resolves through exactly the same gate the program did, so there is one
// answer to "what is executable from this namespace" and not two.
struct Spoor *exec_resolve_from_namespace(struct Proc *p, const char *name,
                                          size_t name_len, size_t *size_out);

// _ex variant (VIVARIUM section 13): also reports, via *pheno_out (OPTIONAL),
// whether the resolution crossed an MPHENO_LINUX mount -- the /viv/bin subtree
// phenotype declaration channel. Only the SYS_SPAWN_FULL_ARGV path (the sole
// fresh-phenotype declarer) passes non-NULL; the plain wrapper above passes NULL.
struct Spoor *exec_resolve_from_namespace_ex(struct Proc *p, const char *name,
                                             size_t name_len, size_t *size_out,
                                             bool *pheno_out);

// exec_stage_env -- project `p`'s /env into the packed "NAME=VALUE\0" block
// exec_load_into takes (#140). The one place a Proc's environment becomes an
// exec argument, so the bound and the encoding live in one place rather than
// at each caller.
//
// On success returns 0 and sets *data_out / *len_out / *count_out; the CALLER
// owns *data_out and must kfree it (NULL/0/0 for a Proc with no environment,
// which is not an error). On failure returns a NEGATIVE errno and leaves the
// outputs zeroed: -T_E_NOMEM for the staging allocation, -T_E_2BIG when the
// environment exceeds EXEC_ENV_DATA_MAX or EXEC_ENV_MAX.
//
// Callers that HAVE an environment of their own to impose (a Linux execve's
// envp) do not come through here -- they pack their own block. This is for the
// callers whose answer is "whatever this Proc already has": every SYS_SPAWN_*
// thunk (projecting the child's already-cloned copy) and the native SYS_EXECVE,
// whose ABI has no envp argument and therefore means "preserve".
int exec_stage_env(struct Proc *p, char **data_out, u32 *len_out,
                   u32 *count_out);

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

// #107 test observable: the (kernel VA, length) span the eager exec paths last
// asked the arch layer to make instruction-coherent, plus the monotonic COUNT
// of those requests. Emulated targets model a coherent I-cache, so a stale-
// instruction fetch is not observable in-guest at all -- what IS checkable is
// that the maintenance was issued over the whole executable span rather than
// only the copied bytes, and issued once PER executable segment. Same posture
// as the W1.5 patcher's `g_alt_applied == g_alt_total` gate: assert the work
// was done, not that the hardware misbehaved.
//
// The count carries the half of that precedent a last-call snapshot cannot
// (#107-audit F3): with two PF_X PT_LOADs the span scalars hold only the
// second segment's, so span-only assertions are blind to a sabotage that
// syncs just the last one. Nothing in production reads either -- both are
// KERNEL_TESTS-only (F4), so the production shape carries no storage and no
// stores.
#ifdef KERNEL_TESTS
void exec_icache_last_for_test(u64 *addr_out, size_t *len_out);
u64  exec_icache_calls_for_test(void);
#endif

#endif // THYLACINE_EXEC_H
