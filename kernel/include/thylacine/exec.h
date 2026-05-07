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

// User-VA layout for v1.0 exec'd Procs:
//
//   0x0000_0000_0001_0000          User code/data (per ELF e_entry +
//   ...                             per-segment vaddr).
//   ...
//   0x0000_0000_8000_0000 - 16 KiB User stack base (16 KiB stack).
//   0x0000_0000_8000_0000          User stack TOP (initial SP_EL0).
//
// The user-stack region is well below the TTBR1 split (0x0001_0000_*),
// well above typical ELF segment vaddrs, and small (16 KiB) at v1.0.
// Phase 5+ adds growable stack via demand paging on stack-grow faults.
#define EXEC_USER_STACK_SIZE   (16ull * 1024)
#define EXEC_USER_STACK_TOP    0x0000000080000000ull
#define EXEC_USER_STACK_BASE   (EXEC_USER_STACK_TOP - EXEC_USER_STACK_SIZE)

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
// `*sp_out`:    EXEC_USER_STACK_TOP. Caller writes to SP_EL0 before
//               the eret to EL0.
int exec_setup(struct Proc *p, const void *blob, size_t blob_size,
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
