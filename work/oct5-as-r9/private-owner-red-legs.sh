#!/bin/sh
# The two RED legs for the private-owner chunk, mechanised.
#
# WHY A SCRIPT RATHER THAN HANDS. A test that has never turned red is not a
# witness, so each new fixture has to be shown failing against a mutation of the
# thing it guards. Doing that by hand under a contended lease is where the known
# mistake happens: A SABOTAGE RUN LEAVES ITS KERNEL IN build/, AND test.sh BOOTS
# THAT -- so a green result taken after a sabotage, without an intervening
# rebuild from clean source, is a lie about the wrong binary. This script always
# reverts and always rebuilds, and verifies the revert by content.
#
# It also refuses to start on a dirty tree, because it mutates tracked source
# and a revert would otherwise destroy uncommitted work.
#
# WHAT EACH LEG PROVES:
#   interior-unmap  deleting burrow_unmap_reporting's `vma->vaddr_start != vaddr`
#                   guard must FAIL burrow.unmap_interior_start_refused. Without
#                   that guard the call uninstalls a tail page and then removes
#                   the WHOLE two-page VMA -- a partial unmap accepted as a full
#                   teardown.
#   uncond-refund   making loom_private_destroy refund the ring's recorded charge
#                   regardless of whether its drop ended the occupancy must FAIL
#                   loom.private_owner_lifecycle. This is the implementation the
#                   draft's all-final fixture could not catch, and the host double
#                   predicts the shape (rc 11, over-refund by the ring's pages).
#
# REQUIRES THE MAC LEASE: it builds and boots. Hold it before running.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 3
LOGS=${LOGS:-work/oct5-as-r9/private-owner-logs}
mkdir -p "$LOGS" || exit 3

say() { printf '\n== %s\n' "$*"; }
die() { printf '\nREFUSING: %s\n' "$*"; exit 3; }

[ -z "$(git status --porcelain -- kernel tools)" ] \
  || die "kernel/ or tools/ is dirty -- commit first; this script reverts source by checkout"

BASE_LOOM=$(git hash-object kernel/loom.c)
BASE_BURROW=$(git hash-object kernel/burrow.c)

build() {
  say "build ($1)"
  tools/build.sh kernel --config ci > "$LOGS/red-$1-build.txt" 2>&1 \
    || { tail -30 "$LOGS/red-$1-build.txt"; die "build failed for $1"; }
}

# Reads the BOOT LOG, not test.sh's stdout: an ELF name is not an execution
# witness, and a kernel FAIL extincts the boot, so the per-test lines are the
# only place a verdict for one named test actually appears.
run_suite() {
  say "suite ($1)"
  tools/test.sh > "$LOGS/red-$1-suite.txt" 2>&1
  printf 'test.sh rc=%s\n' "$?" >> "$LOGS/red-$1-suite.txt"
}

# verdict <leg> <test name> -> prints PASS/FAIL/ABSENT for that one test
verdict() {
  line=$(grep -E "\[test\] $2 \.\.\. (PASS|FAIL)" "$LOGS/red-$1-suite.txt" | tail -1)
  if [ -z "$line" ]; then echo ABSENT; return; fi
  case $line in *PASS*) echo PASS;; *FAIL*) echo FAIL;; *) echo ABSENT;; esac
}

mutate() { # mutate <file> <old> <new>
  python3 -I - "$1" "$2" "$3" <<'PY'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
n = s.count(old)
if n != 1:
    sys.exit("ABORT: anchor appears %d times (want 1) in %s" % (n, p))
open(p, "w").write(s.replace(old, new))
PY
}

revert() { # revert <file> <expected hash>
  git checkout -- "$1" || die "could not revert $1"
  got=$(git hash-object "$1")
  [ "$got" = "$2" ] || die "revert of $1 did not restore the original content ($got != $2)"
}

fail=0
report() { # report <leg> <test> <want>
  got=$(verdict "$1" "$2")
  if [ "$got" = "$3" ]; then
    printf 'PASS  leg=%-14s %s -> %s (want %s)\n' "$1" "$2" "$got" "$3"
  else
    printf 'FAIL  leg=%-14s %s -> %s (want %s)\n' "$1" "$2" "$got" "$3"
    fail=1
  fi
}

# ---------------------------------------------------------------- leg 1
say "LEG 1: delete the vaddr_start guard, expect the interior fixture RED"
mutate kernel/burrow.c \
  '    if (vma->vaddr_start != vaddr) return -1;
' '' || die "leg 1 mutation refused"
build interior-unmap
run_suite interior-unmap
revert kernel/burrow.c "$BASE_BURROW"
report interior-unmap burrow.unmap_interior_start_refused FAIL

# ---------------------------------------------------------------- leg 2
say "LEG 2: refund the ring unconditionally, expect the lifecycle fixture RED"
mutate kernel/loom.c \
  '    u32 refund = 0;
    (void)burrow_unref_settled_in(l->ring, as, &refund);
' '    u32 refund = 0;
    u32 paid_unconditionally = burrow_charge_claim_in(l->ring, as);
    (void)burrow_unref_settled_in(l->ring, as, &refund);
    refund = paid_unconditionally;
' || die "leg 2 mutation refused"
build uncond-refund
run_suite uncond-refund
revert kernel/loom.c "$BASE_LOOM"
report uncond-refund loom.private_owner_lifecycle FAIL

# ------------------------------------------------- the green control, LAST
# Both reverts are verified by hash above, but the binary in build/ is still the
# last MUTANT's. This rebuild is the one that makes the green meaningful.
say "GREEN CONTROL: rebuilt from restored source, both fixtures must PASS"
build green
run_suite green
report green burrow.unmap_interior_start_refused PASS
report green loom.private_owner_lifecycle PASS

say "SUMMARY"
printf 'loom.c   %s -> %s\n' "$BASE_LOOM" "$(git hash-object kernel/loom.c)"
printf 'burrow.c %s -> %s\n' "$BASE_BURROW" "$(git hash-object kernel/burrow.c)"
if [ $fail -eq 0 ]; then
  echo "ALL 4 ROWS AS PREDICTED -- both fixtures are witnesses, not intentions."
else
  echo "SOME ROWS WRONG -- read the per-leg suite logs in $LOGS before claiming anything."
fi
exit $fail
