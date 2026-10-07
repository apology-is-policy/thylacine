#!/bin/sh
# The two RED legs for the private-owner chunk, mechanised.
#
# WHY A SCRIPT RATHER THAN HANDS. A test that has never turned red is not a
# witness, so each new fixture must be shown failing against a mutation of the
# thing it guards. Doing that by hand under a contended lease is where the known
# mistake happens: A SABOTAGE RUN LEAVES ITS KERNEL IN build/, AND test.sh BOOTS
# THAT -- so a green taken after a sabotage, without an intervening rebuild from
# clean source, is a true statement about the wrong binary.
#
# THREE DEFECTS ASTRA FOUND IN THE FIRST VERSION OF THIS FILE (yip 0161 t31),
# fixed here, each worth naming because each is a class this tree keeps meeting:
#
#  1. IT READ test.sh's STDOUT WHILE ITS COMMENT CLAIMED THE BOOT LOG. The
#     per-test verdicts live in the SERIAL log ($BUILD_DIR/test-boot.log), which
#     it never touched. That is the stdout-versus-serial defect that previously
#     hid the suite witnesses, and the comment asserting the correct behaviour
#     made it worse: a comment true about the wrong thing, in a script whose
#     whole job is catching that. Now each leg's serial log is PRESERVED the
#     moment test.sh exits, fail-closed on the copy AND on its content, into a
#     unique per-run directory a retry cannot overwrite, and every verdict is
#     read from the preserved copy.
#  2. NO RESTORATION ON FAILURE. build() called die, so a failed mutant compile
#     or an interrupt left TRACKED SOURCE MUTATED while the header above claimed
#     it always reverted -- and a later unrelated build would then compile
#     sabotaged source. Restoration now runs from an EXIT/INT/TERM/HUP trap,
#     from exact byte copies taken before any mutation, verified by hash before
#     it reports success. If a failure prevents the clean rebuild, build/ is
#     STAMPED as holding a mutant kernel rather than left to be booted as clean.
#  3. THE GREEN CONTROL COULD NOT SEE AN UNRELATED FAILURE. run_suite threw
#     test.sh's status away (it returned printf's) and report() checked two named
#     rows, so a green control with some other test failing still printed ALL
#     ROWS AS PREDICTED. Status is data now; the clean control additionally
#     requires the full tally with zero skips, no extinction and no other FAIL;
#     and each RED is attributed by requiring the failing set to be EXACTLY its
#     intended test, so an unrelated build or runtime failure cannot be banked
#     as a successful red leg.
#
# A kernel test FAIL EXTINCTS the boot, so a red leg's serial log stops at the
# failure and carries no tally. That is expected, and it is why a red leg is
# judged on its named verdict plus attribution, never on a tally.
#
# REQUIRES THE MAC LEASE: it builds and boots. Hold it before running. It kills
# nothing it does not own: test.sh manages its own VM, and no peer's process or
# lease is touched.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 3

BOOTLOG=build/test-boot.log              # tools/test.sh:16,45 -- $BUILD_DIR/test-boot.log
RUN=work/oct5-as-r9/private-owner-logs/red-legs/$(date -u +%Y%m%dT%H%M%SZ)
PRISTINE=$RUN/pristine
MUTANT_STAMP=build/MUTANT-UNQUALIFIED

say()  { printf '\n== %s\n' "$*"; }
die()  { printf '\nREFUSING: %s\n' "$*"; exit 3; }

[ -e "$RUN" ] && die "$RUN exists -- refusing to write over a prior run's evidence"
mkdir -p "$PRISTINE" || die "cannot create $PRISTINE"

[ -z "$(git status --porcelain -- kernel tools)" ] \
  || die "kernel/ or tools/ is dirty -- commit first; this script mutates tracked source"
[ -f "$BOOTLOG" ] || say "note: $BOOTLOG absent; it is created by the first suite run"

HEAD_AT_START=$(git rev-parse HEAD)
printf '%s\n' "$HEAD_AT_START" > "$RUN/head.txt"

# EXACT ORIGINALS, kept as bytes rather than trusted to the index, so restoration
# cannot depend on git state that a later step might change.
MUTATED_FILES="kernel/burrow.c kernel/loom.c"
for f in $MUTATED_FILES; do
  cp "$f" "$PRISTINE/$(basename "$f")" || die "cannot preserve $f"
done

CLEAN_REBUILT=0
restore() {
  rc_in=$?
  bad=0
  for f in $MUTATED_FILES; do
    cp "$PRISTINE/$(basename "$f")" "$f" 2>/dev/null || { echo "RESTORE FAILED: $f"; bad=1; continue; }
    want=$(git hash-object "$PRISTINE/$(basename "$f")")
    got=$(git hash-object "$f")
    [ "$want" = "$got" ] || { echo "RESTORE MISMATCH: $f ($got != $want)"; bad=1; }
  done
  if [ $bad -eq 0 ]; then
    echo "sources restored and verified byte-for-byte: $MUTATED_FILES"
  else
    echo "*** SOURCES MAY STILL BE MUTATED -- originals are in $PRISTINE ***"
  fi
  if [ "$CLEAN_REBUILT" -eq 0 ]; then
    { echo "build/ holds a kernel built from MUTATED source by $0"
      echo "run at $(date -u +%Y-%m-%dT%H:%M:%SZ) against HEAD $HEAD_AT_START"
      echo "DO NOT treat any artifact here as qualified; rebuild before any gate."
    } > "$MUTANT_STAMP" 2>/dev/null
    echo "*** build/ IS UNQUALIFIED: stamped $MUTANT_STAMP -- rebuild before booting it as clean ***"
  fi
  exit $rc_in
}
trap restore EXIT INT TERM HUP

build() { # build <leg>
  say "build ($1)"
  if ! tools/build.sh kernel --config ci > "$RUN/$1-build.txt" 2>&1; then
    tail -30 "$RUN/$1-build.txt"
    die "build failed for $1 -- see $RUN/$1-build.txt"
  fi
}

# Runs the suite and PRESERVES the serial log immediately, fail-closed on both
# the copy and its content: an empty or missing serial log means the run produced
# no evidence, which is a refusal and never an absent verdict to interpret.
run_suite() { # run_suite <leg> -> sets SUITE_RC, writes $RUN/<leg>-serial.log
  say "suite ($1)"
  tools/test.sh > "$RUN/$1-stdout.txt" 2>&1
  SUITE_RC=$?
  printf '%s\n' "$SUITE_RC" > "$RUN/$1-test-sh-rc.txt"
  [ -f "$BOOTLOG" ] || die "$BOOTLOG absent after test.sh ($1) -- no serial evidence"
  cp "$BOOTLOG" "$RUN/$1-serial.log" || die "cannot preserve the serial log for $1"
  grep -q '[^[:space:]]' "$RUN/$1-serial.log" \
    || die "$RUN/$1-serial.log has no content -- the boot produced no serial output"
  printf 'test.sh rc=%s, serial log %s bytes\n' "$SUITE_RC" "$(wc -c < "$RUN/$1-serial.log" | tr -d ' ')"
}

# Every verdict below is read from the PRESERVED SERIAL LOG, never from stdout.
verdict() { # verdict <leg> <test name>
  line=$(grep -E "\[test\] $2 \.\.\. (PASS|FAIL)" "$RUN/$1-serial.log" | tail -1)
  case "${line:-}" in
    *PASS*) echo PASS ;;
    *FAIL*) echo FAIL ;;
    *)      echo ABSENT ;;
  esac
}
failing_set() { # every test that FAILED, one per line
  grep -oE '\[test\] [a-z0-9_.]+ \.\.\. FAIL' "$RUN/$1-serial.log" \
    | sed 's/\[test\] //; s/ \.\.\. FAIL//' | sort -u
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

unmutate() { # restore one file from its pristine copy, mid-run
  cp "$PRISTINE/$(basename "$1")" "$1" || die "cannot restore $1"
  want=$(git hash-object "$PRISTINE/$(basename "$1")"); got=$(git hash-object "$1")
  [ "$want" = "$got" ] || die "restore of $1 did not match ($got != $want)"
}

# A red leg is credited ONLY if its intended test failed and nothing else did.
# Otherwise the leg is unattributed: logs are kept and the run aborts rather
# than continuing as though the mutation had been the cause.
expect_red() { # expect_red <leg> <test>
  got=$(verdict "$1" "$2")
  others=$(failing_set "$1" | grep -v "^$2$" || true)
  if [ "$got" != FAIL ]; then
    printf 'FAIL  leg=%-14s %s -> %s (want FAIL)\n' "$1" "$2" "$got"
    die "leg $1 did not redden its intended test; evidence kept in $RUN"
  fi
  if [ -n "$others" ]; then
    printf 'FAIL  leg=%-14s %s FAILED, but so did:\n%s\n' "$1" "$2" "$others"
    die "leg $1 is unattributed -- another test failed too; evidence kept in $RUN"
  fi
  printf 'PASS  leg=%-14s %s -> FAIL, and it is the ONLY failure\n' "$1" "$2"
}

say "evidence dir $RUN (HEAD $HEAD_AT_START)"

# ---------------------------------------------------------------- leg 1
say "LEG 1: delete the vaddr_start guard, expect the interior fixture RED"
mutate kernel/burrow.c '    if (vma->vaddr_start != vaddr) return -1;
' '' || die "leg 1 mutation refused"
build interior-unmap
run_suite interior-unmap
unmutate kernel/burrow.c
expect_red interior-unmap burrow.unmap_interior_start_refused

# ---------------------------------------------------------------- leg 2
say "LEG 2: refund the ring unconditionally, expect the lifecycle fixture RED"
mutate kernel/loom.c '    u32 refund = 0;
    (void)burrow_unref_settled_in(l->ring, as, &refund);
' '    u32 refund = 0;
    u32 paid_unconditionally = burrow_charge_claim_in(l->ring, as);
    (void)burrow_unref_settled_in(l->ring, as, &refund);
    refund = paid_unconditionally;
' || die "leg 2 mutation refused"
build uncond-refund
run_suite uncond-refund
unmutate kernel/loom.c
expect_red uncond-refund loom.private_owner_lifecycle

# ------------------------------------------------- the green control, LAST
# Both sources are restored and hash-verified above, but build/ still holds the
# last MUTANT's kernel. This rebuild is what makes the green mean anything.
say "GREEN CONTROL: rebuilt from restored source"
build green
run_suite green
CLEAN_REBUILT=1
rm -f "$MUTANT_STAMP"

fail=0
[ "$SUITE_RC" -eq 0 ] || { echo "FAIL  green: test.sh exited $SUITE_RC"; fail=1; }

tally=$(grep -E '^ *tests: [0-9]+/[0-9]+ PASS' "$RUN/green-serial.log" | tail -1)
if [ -z "$tally" ]; then
  echo "FAIL  green: no suite tally in the serial log -- the suite did not finish"
  fail=1
else
  ran=$(printf '%s\n' "$tally" | sed -E 's#.*tests: ([0-9]+)/([0-9]+) PASS.*#\1#')
  tot=$(printf '%s\n' "$tally" | sed -E 's#.*tests: ([0-9]+)/([0-9]+) PASS.*#\2#')
  if [ "$ran" = "$tot" ]; then echo "PASS  green: tally $ran/$tot"
  else echo "FAIL  green: tally $ran/$tot -- not every test passed"; fail=1; fi
fi

skips=$(grep -c '\[skip\]' "$RUN/green-serial.log")
[ "$skips" -eq 0 ] && echo "PASS  green: 0 skips" || { echo "FAIL  green: $skips skip(s)"; fail=1; }

ext=$(grep -c 'EXTINCTION:' "$RUN/green-serial.log")
[ "$ext" -eq 0 ] && echo "PASS  green: no extinction" || { echo "FAIL  green: $ext extinction line(s)"; fail=1; }

others=$(failing_set green)
[ -z "$others" ] && echo "PASS  green: no failing test" || { printf 'FAIL  green: failing tests:\n%s\n' "$others"; fail=1; }

for t in burrow.unmap_interior_start_refused loom.private_owner_lifecycle; do
  g=$(verdict green "$t")
  [ "$g" = PASS ] && printf 'PASS  green: %s -> PASS\n' "$t" \
                  || { printf 'FAIL  green: %s -> %s (want PASS)\n' "$t" "$g"; fail=1; }
done

say "SUMMARY"
printf 'HEAD           %s\n' "$HEAD_AT_START"
printf 'evidence       %s\n' "$RUN"
for f in $MUTATED_FILES; do
  printf '%-14s %s\n' "$(basename "$f")" "$(git hash-object "$f")"
done
if [ $fail -eq 0 ]; then
  echo "BOTH FIXTURES ARE WITNESSES: each reddened on its own mutant as the ONLY"
  echo "failure, and both pass on a kernel rebuilt from restored source."
else
  echo "GREEN CONTROL IMPERFECT -- read $RUN before claiming anything."
fi
exit $fail
