// Halls of Extinction -- Tier-2 in-kernel symbol table (HX-2). See the design
// scripture docs/HALLS-OF-EXTINCTION.md section 3 + docs/reference/101-halls.md.
//
// A minimal sorted (offset, name) table embedded in the kernel image so the
// Tier-1 backtrace prints `func+0xN` live, no offline addr2line step. The real
// table is GENERATED per-build-dir from the linked ELF by
// tools/gen-halls-symtab.py (driven by tools/build.sh's two-pass build); a
// committed stub (halls_symtab.stub.c, count=0) seeds a bare/IDE build so the
// kernel always links + runs (count=0 -> halls_symbolize returns NULL -> the
// backtrace falls back to raw+link addrs, the HX-1 behaviour).

#ifndef THYLACINE_HALLS_SYMTAB_H
#define THYLACINE_HALLS_SYMTAB_H

#include <thylacine/types.h>

// One symbol. `off` is the link-relative offset (sym_link_va -
// halls_symtab_link_base); `name_off` is the byte offset of its
// NUL-terminated name within halls_symtab_names. Stored as u32 OFFSETS, not
// absolute u64 VAs, deliberately: in this PIE kernel an absolute VA in
// initialized data emits an R_AARCH64_RELATIVE reloc the boot stub SLIDES by
// the KASLR offset -- which would both bloat .rela.dyn by one entry per symbol
// AND turn the stored value into a runtime (slid) address, defeating the
// KASLR-independent "link-relative" design. A u32 offset is a plain constant:
// reloc-free + slide-independent. The table is sorted ascending by `off`.
struct halls_sym {
    u32 off;
    u32 name_off;
};

// The link VA the offsets are relative to (the generator emits the minimum
// text symbol address it observed, == _start == KERNEL_LINK_VA). Carried in
// the table rather than hardcoded so a future KERNEL_LINK_VA change flows
// through automatically on regeneration. A plain integer constant -> no reloc.
extern const u64               halls_symtab_link_base;
extern const u32               halls_symtab_count;
extern const struct halls_sym  halls_symtab[];
extern const char              halls_symtab_names[];

// Pure lookup over an EXPLICIT table (unit-testable). `tab` MUST be sorted
// ascending by `.off`, `count` entries; `names` is the NUL-separated blob;
// `base` is the link VA the offsets are relative to. Returns the name of the
// greatest entry with `off <= (link_va - base)` and sets *out_off to the byte
// delta; returns NULL if count==0, link_va < base, the offset exceeds u32, or
// link_va is below the first entry. No allocation, no locks, no faulting reads
// -- safe on the dying-machine dump path.
const char *halls_symbolize_table(const struct halls_sym *tab, u32 count,
                                  const char *names, u64 base,
                                  u64 link_va, u64 *out_off);

// Wrapper over the linked-in global table.
const char *halls_symbolize(u64 link_va, u64 *out_off);

#endif  // THYLACINE_HALLS_SYMTAB_H
