#!/usr/bin/env bash
# HX-2: regenerate the per-build-dir in-kernel symbol table from a linked kernel
# ELF, then re-link if it changed. Shared by tools/build.sh (the main build) and
# tools/test-fault.sh (the deliberate-fault harness) so EVERY dump path gets live
# `func+0xN` symbolization, not just the main build.
#
# Two-pass: the table lives in <build>/generated/halls_symtab.c (compiled into
# the kernel) and in .rodata (after .text per kernel.ld), so re-linking with the
# real table does NOT move the symbolized .text addresses -> it converges after
# one re-link; pass 2 re-derives a byte-identical table (the stability assert).
# A no-op incremental build converges in pass 1 with no extra link.
#
# BEST-EFFORT: a missing llvm-nm / python3, or a generation failure, keeps the
# existing (stub or prior) table and exits 0 -- symbolization is ergonomics,
# never a build gate (HX-2 design, docs/HALLS-OF-EXTINCTION.md section 3).
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_PREFIX="${LLVM_PREFIX:-/opt/homebrew/opt/llvm}"

kbuild="${1:?usage: regen-halls-symtab.sh <kernel-build-dir> [cmake-verbose-flag]}"
verbose="${2:-}"

elf="$kbuild/thylacine.elf"
gen="$kbuild/generated/halls_symtab.c"
nm="$LLVM_PREFIX/bin/llvm-nm"

[[ -f "$elf" ]] || exit 0
[[ -x "$nm" ]]  || { echo "==> HX-2: $nm not found; keeping current symtab"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "==> HX-2: python3 not found; keeping current symtab"; exit 0; }

mkdir -p "$kbuild/generated"
for pass in 1 2 3; do
    tmp="$gen.new"
    if ! "$nm" --defined-only "$elf" | python3 "$REPO_ROOT/tools/gen-halls-symtab.py" > "$tmp"; then
        echo "==> HX-2: symtab generation failed; keeping current table" >&2
        rm -f "$tmp"
        exit 0
    fi
    if [[ -f "$gen" ]] && cmp -s "$tmp" "$gen"; then
        rm -f "$tmp"
        [[ $pass -gt 1 ]] && echo "==> HX-2: symbol table stable (converged after $((pass - 1)) re-link)"
        exit 0
    fi
    mv "$tmp" "$gen"
    echo "==> HX-2: regenerated in-kernel symbol table (pass $pass) -- re-linking"
    cmake --build "$kbuild" $verbose
done
echo "==> HX-2: WARNING symbol table did not converge in 3 passes (using last)" >&2
exit 0
