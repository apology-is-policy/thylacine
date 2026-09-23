---
id: dec-2026-09-23-memory-surface-and-loader
type: dec
title: "B-1: the permission ceiling, the memory bar, and the loader model"
date: 2026-09-23
status: standing
decided-by: user-vote
affects: [inv-i12, inv-i32, inv-i36, inv-i28, inv-i44, sub-kernel-burrow, sub-kernel-vma, sub-kernel-fault, sub-kernel-addrspace, sub-kernel-exec, sub-kernel-vivarium, sub-pouch-seam, sub-pouch-process, sub-libthyla-rs]
created: 2026-09-23
---
## Fork

The browser arc's B-0 measured nine platform findings (F1-F9,
`docs/browser-status.md`) and the operator's standing directive was that the
kernel ones -- "mprotect, dlopen etc." -- are a conversation, never designed
alone. The conversation was held 2026-09-23 at max effort on Fable 5.1. Mid-way
the operator asked two things of their own: whether a permission-mutation call
should be gated on a capability "like we do with JIT", and whether "the
growable heap" is finally needed for WebKit -- which, on being asked what it
meant, became a bar: memory management comparable to production operating
systems, on both substrates.

## Research

Heritage: Plan 9 has no `mprotect` -- `segattach(2)` fixes `SG_RONLY` at
attach and nothing changes it later; `segfree` is our decommit; static linking
is deliberate (Minnich's four reasons, 9p.io "why static"). SOTA: Fuchsia's
`zx_vmar_protect` is bounded only by the VMAR and VMO handle rights and
sub-regions may only be reduced, while *execute* needs
`zx_vmo_replace_as_executable` with the VMEX resource -- a capability;
`zx_vmar_map` takes `ZX_VM_ALIGN_*`; libraries arrive as VMO handles from a
loader service and `dlopen_vmo` takes one. Genode's `Region_map::attach` fixes
`writeable`/`executable` and no method mutates an attachment; its ldso IS the
executable and takes ROM dataspaces. seL4's `Page_Map` on a mapped page updates
the attributes, rights bounded by the frame cap. OpenBSD `mimmutable` freezes
protection and mapping; Mach `vm_protect(set_maximum)` lowers only the ceiling.
FreeBSD `fdlopen` loads by descriptor, for race-freedom and Capsicum.

WebKit, read in the sparse clone rather than recalled: B-0 builds with
`USE_SYSTEM_MALLOC=ON`, so WTF's `OSAllocatorPOSIX` is the surface. Guard
pages are `mmap(MAP_FIXED, PROT_NONE)` over a reservation's ends (`:127`);
aligned reservation over-maps and trims (`:221`; patch 0001 already disables
the trim on Thylacine, leaking the slack); commit and decommit are `madvise`;
the config freeze is `mprotect(PROT_READ)` (`WTFConfig.cpp:228`). And release
JavaScriptCore RAISES none -> RW inside its own reservations for resizable
`ArrayBuffer` and shared Wasm memory (`ArrayBuffer.cpp:595/611`,
`WasmMemory.cpp:234/378`, `BufferMemoryHandle.cpp:273`) -- paths B-0 never
exercised. It never asks for X through this call; the JIT is the I-42 dual map.

Tree: `struct Vma.prot` is {0, R, RW, RX}; `vma_alloc_guard` exists;
`vma_replace_range_in` (D-3b) is a byte-identity-preserving split; the fault
path enforces `vma->prot` per fault (`fault.c:402`); COW chose "uninstall the
PTE, re-fault" over in-place edits. Missing: any syscall with a prot,
alignment, partial detach, or a native file map. Pouch's `__mmap` ignores prot;
`__mprotect` is an honest ENOSYS; pthread stacks have no guard
(`POUCH-DESIGN.md:239` had already named "a syscall to flip VMA permissions"
as the v1.x fix). The memory bar, measured: four refusals with free memory
(the fixed 4 MiB native heap `alloc.rs:77`; the 256 MiB default budget
`proc.h:114` against a 2 GiB VM; the 256 MiB / 1 GiB reservation caps anchored
to the flat uncharged `filepages` array `syscall.h:3203/3217`; eager attach's
contiguity, rings only) and three paths that keep relinquished pages
(`mallocng`'s `MADV_FREE` inside a retained group `free.c:124` -> ENOSYS;
WebKit's decommit -> ENOSYS; `linked_list_allocator` never trims). Decommit
frees and uncharges (`burrow.c:1246`); whole-group `munmap` works. dlopen:
D-2, D-3a/b and D-4 all exist and are all gated to `PHENO_LINUX`
(`exec.c:1332`); no native file-map syscall; the fork driver pushes `-static`,
non-PIE `ET_EXEC` (`Thylacine.cpp:25,44`); musl's static `dlopen` is a stub.

## Options

Shape: (1) ceiling-bounded protect (Fuchsia / Mach); (2) monotone reduce only
(I-6's twin); (3) mint-time only (Plan 9 / Genode). Gate: structural / an
elevation-only cap / a default-granted cap. Seal: same chunk / later / never.
Loader: (A) dynamic Pouch, musl's real model; (B) static host + a plugin
loader in the static libc; the handle form now or with its first consumer.
Sysroot: static default with `.so` only for runtime-loaded objects; both
`.a` and `.so`; dynamic default. I-32 default: RAM minus a reserve / RAM /
keep 256 MiB. Native allocator: `dlmalloc-rs` / extend `linked_list_allocator`
/ a new size-class allocator. Capacity: range detach + charged sparse
`filepages` together / detach only / merged into B-1a.

## The call

The operator, by blocking question, 2026-09-23 -- eleven votes, every one on
the recommended option except the fourth:

1. **Ceiling-bounded `burrow_protect`**: a mint-time `prot_max` per VMA
   (anon RW; file maps their provenance-gated mint; guards none; CODE and
   shared-in refuse); X never a target; a range within one mapping.
2. **No capability on the call; the gate is structural.**
3. **`PROTECT_SEAL` in the same chunk**, Mach `set_maximum` shape.
4. **dlopen designed now** (the non-recommended arm). The stack to 8 MiB with
   its extent in auxv rode this vote as the agent's stated reading, not as a
   vote of its own.
5. **Loader model L-A, dynamic Pouch**: `libc.so` is the loader; D-4's
   PT_INTERP rewrite lifted to native execs; `burrow_map_file` exposes D-3.
6. **Static by default; `.so` only for runtime-loaded objects and `libc.so`.**
7. **The memory bar**: never refused while free memory exists; relinquished
   memory returns; both substrates.
8. **The I-32 default becomes physical RAM minus a boot-sized TCB reserve**;
   the cap is the confinement mechanism.
9. **`dlmalloc-rs`** over a Thylacine platform trait replaces the fixed heap.
10. **Range detach + the charged sparse `filepages`, both in B-1a'.**
11. **Sequence**: one scripture commit, then B-1a permissions, B-1a'
    capacity, B-1b Pouch, B-1c dlmalloc + the two-substrate witness, B-1d
    dlopen before B-3.

## Rationale

A capability gates the creation of authority; `CAP_JIT` is right because
bytes becoming code is exactly that. Lowering a permission creates none, and
raising it back within a ceiling the mint conferred creates none either, so a
cap on the call would invert I-6 (a holder never needs permission to hold
less) and would be ambient besides -- every `pthread_create` needs it. The
ceiling rather than monotone reduction is decided by the JSC measurement:
reduce-only would have forced an `mprotect` whose contract differs from its
name, the class of lie A-6 was about. The memory bar is the operator's, and
every kernel item under it is a seam scripture already named (`syscall.h`'s
"charged radix" note; `POUCH-DESIGN.md:239`). dlopen is exec into the current
address space and inherits exec's gates -- no new authority -- which is why
designing it cost a conversation and not an invariant; Plan 9's static default
is kept, and dynamic linking is the opt-in for code chosen at runtime. The
record of the amended scripture: ARCH 6.5 "The permission ceiling", "Range
detach", "Capacity", "Dynamic loading"; 6.6; section 28 I-12 and I-32;
11.2; BROWSER-DESIGN 6 and 9 and O-1; browser-status "The B-1 decisions";
POUCH-DESIGN 2.2 / 8.1 / 8.2; LLVM-DESIGN 5.1 and F3; VIVARIUM 6.21 and the
9 ladder; DISTRO 7.1; JIT-ON-WX-DESIGN; NOVEL 3.7.
