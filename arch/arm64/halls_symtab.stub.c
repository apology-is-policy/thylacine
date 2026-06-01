// Stub in-kernel symbol table (HX-2). tools/build.sh's two-pass build copies
// this into <build>/generated/halls_symtab.c at first configure, then
// tools/gen-halls-symtab.py overwrites that build-dir copy with the REAL table
// extracted from the linked ELF and re-links. This stub is never edited by the
// generator and is the fallback for a bare `cmake` / IDE build: count==0 makes
// halls_symbolize() return NULL, so the Tier-1 backtrace prints raw+link addrs
// only (no live symbolization) -- correct, just less ergonomic.
//
// The generated build-dir copy is per-build-dir on purpose: the default and
// sanitizer builds have different .text layouts, so the table is build-specific
// and must never live in the source tree.

#include "halls_symtab.h"

const u64               halls_symtab_link_base = 0;
const u32               halls_symtab_count     = 0;
const struct halls_sym  halls_symtab[1]        = { { 0, 0 } };
const char              halls_symtab_names[1]  = { 0 };
