#!/bin/sh
# AS-R9 pre-lease syntax check. NOT a build: -fsyntax-only, no linking, no
# artifacts, no mutation of build/. Flags mirror THYLACINE_KERNEL_C_FLAGS in
# cmake/Toolchain-aarch64-thylacine.cmake (minus the codegen-only ones).
#
# Its job is to keep the contended Mac lease for real gates instead of spending
# it discovering a typo. It proves the files PARSE; it proves nothing about
# behaviour -- tools/test.sh does that.
#
# It also attributes: weft.c and syscall.c carry pre-existing warnings, so the
# check compares the warning COUNT against the base commit rather than requiring
# zero. A count that merely matches is the discriminating result; "no warnings"
# would be satisfied by an invocation that never looked.
set -e
CLANG=${CLANG:-/opt/homebrew/opt/llvm@22/bin/clang}
BASE=${BASE:-5ff62b78809846af4780ec41f82d1676e7584e80}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
COMMON="--target=aarch64-none-elf -march=armv8-a -ffreestanding -fno-builtin
        -fno-common -mgeneral-regs-only -std=c99 -Wall -Wextra
        -Wno-unused-parameter -I kernel/include -I arch/arm64 -I ."
rc=0
echo "-- kernel (-Wstrict-prototypes -Wmissing-prototypes, as the toolchain does)"
for f in kernel/burrow.c kernel/vma.c kernel/loom.c kernel/weft.c kernel/syscall.c; do
  mine=$($CLANG $COMMON -Wstrict-prototypes -Wmissing-prototypes -fsyntax-only "$f" 2>&1 || true)
  if echo "$mine" | grep -q "error:"; then echo "  ERROR  $f"; echo "$mine" | head -5; rc=1; continue; fi
  m=$(echo "$mine" | grep -c "warning:" || true)
  # The base must be compiled against the BASE's OWN headers. Extracting just
  # the .c and compiling it with -I kernel/include mixes base source with the
  # CURRENT tree's headers, so any header edit of mine silently moves the
  # "base" number -- which is exactly what happened when burrow_is_shared_out's
  # declaration was deleted: base burrow.c then had no visible prototype and
  # -Wmissing-prototypes took the base from 0 to 1. A baseline that moves when
  # the thing under test changes is not a baseline.
  bdir=$(mktemp -d /tmp/asr9base.XXXXXX)
  git archive "$BASE" | tar -x -C "$bdir"
  b=$(cd "$bdir" && $CLANG --target=aarch64-none-elf -march=armv8-a -ffreestanding \
        -fno-builtin -fno-common -mgeneral-regs-only -std=c99 -Wall -Wextra \
        -Wno-unused-parameter -I kernel/include -I arch/arm64 -I . \
        -Wstrict-prototypes -Wmissing-prototypes -fsyntax-only "$f" 2>&1 \
        | grep -c "warning:" || true)
  rm -rf "$bdir"
  if [ "$m" = "$b" ]; then echo "  OK     $f  (warnings $m, base $b -- none introduced)"
  elif [ "$m" -lt "$b" ]; then echo "  IMPROVED $f  (warnings $m, base $b -- $((b-m)) removed)"
  else echo "  REGRESSED $f  (warnings $m, base $b)"; echo "$mine" | head -8; rc=1; fi
done
echo "-- tests (KERNEL_TESTS guards the for_test hooks in burrow.h)"
for f in kernel/test/test_burrow.c kernel/test/test.c kernel/test/test_addrspace.c; do
  out=$($CLANG $COMMON -DKERNEL_TESTS=1 -I kernel/test -fsyntax-only "$f" 2>&1 || true)
  if [ -z "$out" ]; then echo "  CLEAN  $f"; else echo "  ISSUES $f"; echo "$out" | head -8; rc=1; fi
done
[ $rc = 0 ] && echo "RESULT: all 8 edited files parse; no warning introduced" || echo "RESULT: FAILED"
exit $rc
