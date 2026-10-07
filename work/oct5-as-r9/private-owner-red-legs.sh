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
# THREE SEPARATE FACTS, never collapsed into one (astra, yip 0161 t34-C):
#   SRC_RESTORED    -- tracked source is byte-identical to its pre-run state
#   IMAGE_REBUILT   -- build/ was rebuilt from that restored source
#   IMAGE_QUALIFIED -- the suite on that rebuild passed every green check
# Only the third removes the UNQUALIFIED marker. The first two can hold while
# the image is still unfit to found any claim on.
#
# A kernel test FAIL extincts the boot, but NOT where I thought. Measured on a
# real mutant boot (red-legs/20261007T095600Z): the suite runs every test, prints
# `tests: 1835/1836 FAIL`, and boot_main extincts on the SUMMARY. So a red leg
# DOES carry a full tally, and that tally is an independent attribution signal --
# the kernel's own count of failures against this script's parse of them. An
# earlier version of this header claimed the opposite; it was never measured.
#
# THE VERDICT FORMAT IS NOT ONE LINE, which cost this script its first real run.
# test.c prints `    [test] NAME ... ` BEFORE running the test, so anything the
# test itself prints lands between that and its verdict -- and on failure
# test_fail() calls sched_dump_runnable(), which prints a bracket and a per-CPU
# line, so `FAIL: <msg>` arrives two lines later. 87 of 1836 green verdicts are
# also split that way by ordinary kernel output. On top of that the serial log is
# CRLF, so every `$`-anchored pattern silently fails. A single-line regex read
# 1749 of 1836 verdicts and reported the reddened test as ABSENT.
#
# REQUIRES THE MAC LEASE: it builds and boots. Hold it before running. It kills
# nothing it does not own: the only processes it ever signals are the child it
# launched and a QEMU whose command line names THIS tree's build/ directory.
#
# Everything it touches is relative to ROOT, so the wrapper's own control flow
# is exercised off-lease by pointing ROOT at a scratch repo whose tools/ holds
# stubs. There is deliberately NO command-override seam: a seam that can swap
# the build or the suite is a seam that can fake a gate.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 3

BOOTLOG=build/test-boot.log              # tools/test.sh:16,45 -- $BUILD_DIR/test-boot.log
RUN=work/oct5-as-r9/private-owner-logs/red-legs/$(date -u +%Y%m%dT%H%M%SZ)
PRISTINE=$RUN/pristine
MUTANT_STAMP=build/MUTANT-UNQUALIFIED
MUTATED_FILES="kernel/burrow.c kernel/loom.c"

SRC_RESTORED=0
IMAGE_REBUILT=0
IMAGE_QUALIFIED=0
CHILD_PID=
SUITE_RC=
MUTANT_HASH=                             # hash of the file currently mutated, if any
MUTANT_FILE=

say()  { printf '\n== %s\n' "$*"; }
die()  { printf '\nREFUSING: %s\n' "$*"; exit 3; }

[ -e "$RUN" ] && die "$RUN exists -- refusing to write over a prior run's evidence"
mkdir -p "$PRISTINE" || die "cannot create $PRISTINE"

[ -z "$(git status --porcelain -- kernel tools)" ] \
  || die "kernel/ or tools/ is dirty -- commit first; this script mutates tracked source"

HEAD_AT_START=$(git rev-parse HEAD) || die "cannot read HEAD"
printf '%s\n' "$HEAD_AT_START" > "$RUN/head.txt"

# EXACT ORIGINALS as bytes, plus their hashes, so restoration never depends on
# git state a later step might move, and so an unexpected concurrent edit is
# REFUSED rather than silently overwritten.
for f in $MUTATED_FILES; do
  cp "$f" "$PRISTINE/$(basename "$f")" || die "cannot preserve $f"
  git hash-object "$f" > "$PRISTINE/$(basename "$f").hash" || die "cannot hash $f"
done

pristine_hash() { cat "$PRISTINE/$(basename "$1").hash"; }

# ---------------------------------------------------------------------------
# Owned work. Each build and each suite runs as a tracked child so that a signal
# arriving mid-build cannot have the trap copy pristine source back underneath a
# compiler that is still reading it, or leave an orphaned VM holding the lease's
# cores after this script is gone.
# ---------------------------------------------------------------------------
OWNED_RC=
run_owned() { # run_owned <logfile> <cmd> [args...]
  log=$1; shift
  "$@" > "$log" 2>&1 &
  CHILD_PID=$!
  wait "$CHILD_PID"
  OWNED_RC=$?
  CHILD_PID=
}

# QEMUs belonging to THIS tree only, identified by this ROOT's build/ path in
# the command line. A peer's guest boots a different tree and is never matched,
# never signalled, never waited on.
my_qemu_pids() {
  ps -Ao pid=,comm=,command= 2>/dev/null \
    | awk -v r="$ROOT/build" '$2 ~ /qemu-system-aarch64$/ && index($0, r) { print $1 }'
}

reap_owned() {
  if [ -n "$CHILD_PID" ] && kill -0 "$CHILD_PID" 2>/dev/null; then
    echo "stopping the owned child $CHILD_PID and waiting for it to exit"
    kill -TERM "$CHILD_PID" 2>/dev/null
    i=0
    while kill -0 "$CHILD_PID" 2>/dev/null && [ "$i" -lt 60 ]; do sleep 1; i=$((i + 1)); done
    if kill -0 "$CHILD_PID" 2>/dev/null; then
      echo "child $CHILD_PID ignored TERM; sending KILL"
      kill -KILL "$CHILD_PID" 2>/dev/null
      sleep 1
    fi
  fi
  pids=$(my_qemu_pids)
  if [ -n "$pids" ]; then
    echo "this tree's QEMU still up ($(printf '%s' "$pids" | tr '\n' ' ')); stopping it before restoring source"
    for q in $pids; do kill -TERM "$q" 2>/dev/null; done
    i=0
    while [ -n "$(my_qemu_pids)" ] && [ "$i" -lt 60 ]; do sleep 1; i=$((i + 1)); done
    rest=$(my_qemu_pids)
    if [ -n "$rest" ]; then
      for q in $rest; do kill -KILL "$q" 2>/dev/null; done
      sleep 1
    fi
  fi
  [ -z "$(my_qemu_pids)" ] || echo "WARNING: this tree still has a QEMU up after KILL"
}

# ---------------------------------------------------------------------------
# Cleanup. Runs once, from the EXIT trap or from a signal, with traps disabled
# on entry so `exit` here cannot re-enter it. A failure anywhere inside FORCES a
# nonzero status: a run that could not put the tree back must never report 0.
# ---------------------------------------------------------------------------
cleanup() { # cleanup <rc> <why>
  trap - EXIT INT TERM HUP
  rc=$1
  printf '\n== cleanup (%s, status %s)\n' "$2" "$rc"

  reap_owned

  bad=0
  for f in $MUTATED_FILES; do
    want=$(pristine_hash "$f")
    got=$(git hash-object "$f" 2>/dev/null || echo missing)
    if [ "$got" = "$want" ]; then
      continue
    fi
    # Overwrite ONLY what this script is known to have mutated. Anything else is
    # a concurrent edit by someone or something that is not this run, and losing
    # it would be worse than leaving the tree dirty.
    if [ "$f" = "$MUTANT_FILE" ] && [ "$got" = "$MUTANT_HASH" ]; then
      cp "$PRISTINE/$(basename "$f")" "$f" 2>/dev/null \
        || { echo "RESTORE FAILED: $f"; bad=1; continue; }
      back=$(git hash-object "$f")
      [ "$back" = "$want" ] || { echo "RESTORE MISMATCH: $f ($back != $want)"; bad=1; }
    else
      echo "*** $f IS NOT WHAT THIS RUN LEFT ($got; this run's mutant was ${MUTANT_HASH:-none})"
      echo "    REFUSING to overwrite an edit this run did not make. Original: $PRISTINE/$(basename "$f")"
      bad=1
    fi
  done

  now_head=$(git rev-parse HEAD 2>/dev/null || echo unknown)
  if [ "$now_head" != "$HEAD_AT_START" ]; then
    echo "*** HEAD MOVED during the run ($HEAD_AT_START -> $now_head) -- evidence provenance is suspect"
    bad=1
  fi

  if [ "$bad" -eq 0 ]; then
    SRC_RESTORED=1
    echo "source restored and verified byte-for-byte: $MUTATED_FILES"
  else
    echo "*** SOURCES MAY STILL BE MUTATED -- originals are in $PRISTINE ***"
  fi

  if [ "$IMAGE_QUALIFIED" -eq 1 ]; then
    rm -f "$MUTANT_STAMP"
    echo "build/ is QUALIFIED: rebuilt from restored source and green; marker removed"
  else
    { echo "build/ IS NOT QUALIFIED."
      echo "run      : $0"
      echo "at       : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      echo "HEAD     : $HEAD_AT_START"
      echo "evidence : $RUN"
      echo "src restored  : $SRC_RESTORED"
      echo "image rebuilt : $IMAGE_REBUILT"
      echo "image green   : $IMAGE_QUALIFIED"
      if [ "$IMAGE_REBUILT" -eq 0 ]; then
        echo "This kernel was built from MUTATED source. Rebuild before any gate."
      else
        echo "Rebuilt from restored source, but its suite did not pass every check."
        echo "Do not found a claim on it; read the evidence directory."
      fi
    } > "$MUTANT_STAMP" 2>/dev/null \
      || echo "WARNING: could not write $MUTANT_STAMP"
    echo "*** build/ IS UNQUALIFIED: stamped $MUTANT_STAMP ***"
  fi

  if [ "$bad" -ne 0 ] && [ "$rc" -eq 0 ]; then rc=4; fi
  exit "$rc"
}
trap 'cleanup $? "exit" ' EXIT
trap 'cleanup 130 SIGINT'  INT
trap 'cleanup 143 SIGTERM' TERM
trap 'cleanup 129 SIGHUP'  HUP

# The marker goes down BEFORE the first mutation, not only from the trap: a
# SIGKILL or a power loss cannot run a trap, and the one state that must never
# be silently inherited is a mutant kernel in build/ that looks clean.
stamp_pre_mutation() {
  { echo "build/ IS NOT QUALIFIED -- a red-legs run is in progress or died."
    echo "started : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "HEAD    : $HEAD_AT_START"
    echo "evidence: $RUN"
    echo "If this file is still here, treat build/ as holding MUTATED-source output."
  } > "$MUTANT_STAMP" 2>/dev/null || die "cannot write $MUTANT_STAMP"
}

# Kernel-only, by the script that explains why: the mutations are kernel-only,
# so userspace is held FIXED and the kernel is the single changed variable.
build() { # build <leg>
  say "build ($1)"
  run_owned "$RUN/$1-build.txt" work/oct5-as-r9/rebuild-kernel.sh
  if [ "$OWNED_RC" -ne 0 ]; then
    tail -30 "$RUN/$1-build.txt"
    die "build failed for $1 (rc $OWNED_RC) -- see $RUN/$1-build.txt"
  fi
}

# Runs the suite and preserves NEWLY PRODUCED serial evidence. The prior shared
# log is DISPLACED first, so what is preserved cannot be a previous leg's boot:
# test.sh truncates the log only once it launches QEMU (tools/test.sh:190), so an
# earlier exit leaves no log at all -- which is a refusal, never a verdict.
run_suite() { # run_suite <leg> -> sets SUITE_RC, writes $RUN/<leg>-serial.log
  say "suite ($1)"
  if [ -e "$BOOTLOG" ]; then
    mv -f "$BOOTLOG" "$RUN/$1-displaced-prior-serial.log" \
      || die "cannot displace the prior $BOOTLOG before leg $1"
  fi
  [ -e "$BOOTLOG" ] && die "$BOOTLOG still present after displacement -- refusing to read stale evidence"

  run_owned "$RUN/$1-stdout.txt" tools/test.sh
  SUITE_RC=$OWNED_RC

  [ -f "$BOOTLOG" ] \
    || die "no NEW serial log after test.sh ($1), rc=$SUITE_RC -- the boot never started, so there is no evidence"
  cp "$BOOTLOG" "$RUN/$1-serial.log" || die "cannot preserve the serial log for $1"
  grep -q '[^[:space:]]' "$RUN/$1-serial.log" \
    || die "$RUN/$1-serial.log has no content -- the boot produced no serial output"

  { echo "leg      : $1"
    echo "at       : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "HEAD     : $HEAD_AT_START"
    echo "test.sh  : rc=$SUITE_RC"
    echo "serial   : $(wc -c < "$RUN/$1-serial.log" | tr -d ' ') bytes, sha256 $(shasum -a 256 < "$RUN/$1-serial.log" | cut -d' ' -f1)"
    for f in $MUTATED_FILES; do echo "$(basename "$f"): $(git hash-object "$f")"; done
  } > "$RUN/$1-provenance.txt"
  printf 'test.sh rc=%s, %s bytes of NEW serial evidence\n' \
    "$SUITE_RC" "$(wc -c < "$RUN/$1-serial.log" | tr -d ' ')"
  normalize_verdicts "$1"
}

# Every verdict below is read from the PRESERVED SERIAL LOG, never from stdout,
# and through ONE normalisation pass rather than three regexes: a verdict is a
# STATE in the log, not a line in it. The machine tracks the pending test name
# from `[test] NAME ... ` and resolves it on the first following `FAIL: <msg>`
# or line-final `PASS`, which is exactly how test.c:4604-4612 emits them.
# Validated against two REAL logs before being trusted: the 1836/1836 green boot
# (1836 rows, 1836 PASS, 0 unresolved) and a real mutant boot (1836 rows,
# exactly 1 FAIL, correctly named and reasoned).
normalize_verdicts() { # normalize_verdicts <leg> -> $RUN/<leg>-verdicts.tsv
  awk '
    { sub(/\r$/, "") }
    {
      line = $0
      if (match(line, /\[test\] /)) {
        tail = substr(line, RSTART + 7)
        if (match(tail, / \.\.\. /)) {
          cur  = substr(tail, 1, RSTART - 1)
          line = substr(tail, RSTART + 5)
        }
      }
      if (cur != "") {
        if (match(line, /FAIL: /)) { print cur "\tFAIL\t" substr(line, RSTART + 6); cur = "" }
        else if (line ~ /(^|[^A-Za-z-])PASS$/) { print cur "\tPASS\t"; cur = "" }
      }
    }
    END { if (cur != "") print cur "\tPENDING\t" }
  ' "$RUN/$1-serial.log" > "$RUN/$1-verdicts.tsv"
  n=$(wc -l < "$RUN/$1-verdicts.tsv" | tr -d ' ')
  # The denominator control. A boot that reached the suite at all produces
  # thousands of verdicts; a handful means the PARSER broke, not the kernel, and
  # that must refuse rather than report ABSENT for every test in the suite.
  [ "$n" -ge 1000 ] \
    || die "only $n verdicts parsed out of $RUN/$1-serial.log -- the parser or the boot is broken, and an ABSENT verdict here would be a lie"
  p=$(cut -f2 "$RUN/$1-verdicts.tsv" | grep -c PENDING)
  [ "$p" -eq 0 ] \
    || die "$p verdict(s) in $1 never resolved to PASS or FAIL -- refusing to read a log the parser does not fully understand"
  printf '%s verdicts parsed, 0 unresolved\n' "$n"
}

verdict() { # verdict <leg> <test name>
  v=$(awk -F'\t' -v n="$2" '$1 == n { print $2 }' "$RUN/$1-verdicts.tsv" | tail -1)
  printf '%s\n' "${v:-ABSENT}"
}

fail_reason() { # fail_reason <leg> <test name>
  awk -F'\t' -v n="$2" '$1 == n && $2 == "FAIL" { print $3 }' "$RUN/$1-verdicts.tsv" | tail -1
}

failing_set() { # every test that FAILED, one per line
  awk -F'\t' '$2 == "FAIL" { print $1 }' "$RUN/$1-verdicts.tsv" | sort -u
}

# The kernel's own count, independent of this script's parse of the same log.
tally() { # tally <leg> -> "<ran>/<total> <PASS|FAIL>" or empty
  sed -n -E 's/^ *tests: ([0-9]+\/[0-9]+) (PASS|FAIL).*$/\1 \2/p' "$RUN/$1-serial.log" | tail -1
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

begin_mutation() { # begin_mutation <file>; refuses unless the file is pristine
  got=$(git hash-object "$1")
  [ "$got" = "$(pristine_hash "$1")" ] \
    || die "$1 is not what this run preserved ($got) -- refusing to mutate a file something else changed"
  [ "$(git rev-parse HEAD)" = "$HEAD_AT_START" ] \
    || die "HEAD moved since this run started -- refusing to mutate"
  MUTANT_FILE=$1
}

end_mutation() { # record the mutant's hash so cleanup knows what it may overwrite
  MUTANT_HASH=$(git hash-object "$MUTANT_FILE")
  [ "$MUTANT_HASH" != "$(pristine_hash "$MUTANT_FILE")" ] \
    || die "the mutation left $MUTANT_FILE unchanged -- the anchor did not bite"
  printf '%s\n' "$MUTANT_HASH" > "$RUN/$(basename "$MUTANT_FILE").mutant-hash"
}

unmutate() { # restore one file mid-run, refusing if it is not this run's mutant
  got=$(git hash-object "$1")
  [ "$got" = "$MUTANT_HASH" ] \
    || die "$1 changed under this run ($got != $MUTANT_HASH) -- refusing to overwrite it"
  cp "$PRISTINE/$(basename "$1")" "$1" || die "cannot restore $1"
  back=$(git hash-object "$1")
  [ "$back" = "$(pristine_hash "$1")" ] || die "restore of $1 did not match ($back)"
  MUTANT_FILE=
  MUTANT_HASH=
}

# A red leg is credited ONLY if the suite failed, its intended test failed FOR
# THE INTENDED REASON, and nothing else failed. Any other shape is unattributed:
# the logs are kept and the run aborts rather than banking a red it cannot
# ascribe to the mutation.
expect_red() { # expect_red <leg> <test> <expected fail-reason substring>
  [ "$SUITE_RC" -ne 0 ] \
    || die "leg $1: test.sh exited 0 -- a reddened fixture extincts the boot, so rc 0 means the mutation did not bite"
  got=$(verdict "$1" "$2")
  if [ "$got" != FAIL ]; then
    printf 'FAIL  leg=%-14s %s -> %s (want FAIL)\n' "$1" "$2" "$got"
    die "leg $1 did not redden its intended test; evidence kept in $RUN"
  fi
  reason=$(fail_reason "$1" "$2")
  case "$reason" in
    *"$3"*) ;;
    *) printf 'FAIL  leg=%-14s %s reddened for the WRONG reason\n  got : %s\n  want: *%s*\n' \
         "$1" "$2" "${reason:-(none captured)}" "$3"
       die "leg $1 is unattributed -- the mutation must break the assertion it targets, not another one" ;;
  esac
  others=$(failing_set "$1" | grep -v "^$2\$" || true)
  if [ -n "$others" ]; then
    printf 'FAIL  leg=%-14s %s FAILED, but so did:\n%s\n' "$1" "$2" "$others"
    die "leg $1 is unattributed -- another test failed too; evidence kept in $RUN"
  fi
  # The kernel counted the failures itself. Requiring its count to agree with
  # this script's parse means a verdict the parser cannot see can no longer be
  # mistaken for a verdict that is not there.
  want_tally="$((EXPECT_TESTS - 1))/$EXPECT_TESTS FAIL"
  got_tally=$(tally "$1")
  [ "$got_tally" = "$want_tally" ] \
    || die "leg $1: the kernel's own tally is '$got_tally', not '$want_tally' -- exactly one test must have failed"
  printf '      and the kernel agrees: tests %s\n' "$got_tally"
  printf 'PASS  leg=%-14s %s -> FAIL (%s), the ONLY failure\n' "$1" "$2" "$reason"
}

# The expected suite size is DERIVED from the registration table rather than
# written down, because a number written down is re-pointed by hand and a
# derived one cannot go stale. The denominator control is the point: a pattern
# that matches nothing, or a grep whose -o semantics differ, must refuse rather
# than hand back a small number that looks like a tally.
expected_tests() {
  n=$(grep -ohE '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c | wc -l | tr -d ' ')
  u=$(grep -ohE '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c \
        | sed -E 's/^[[:space:]]*\{[[:space:]]*"//; s/"$//' | sort -u | wc -l | tr -d ' ')
  [ "$n" -ge 1000 ] || die "derived test count $n is implausible -- the extractor, not the suite, is broken"
  [ "$n" -eq "$u" ] || die "the registration table has $((n - u)) duplicate test name(s) -- fix that before trusting a tally"
  echo "$n"
}

# The boot-completion banner is tooling ABI and test.sh is the consumer of
# record (tools/test.sh:121). Read it from there: a string copied into this file
# is re-pointed by hand, and a banner reword would leave this check green on a
# boot that never completed -- which is the gauge-reading-zero failure exactly.
BOOT_MARKER=$(sed -n 's/^BOOT_MARKER="\(.*\)"$/\1/p' tools/test.sh | tail -1)
[ -n "$BOOT_MARKER" ] \
  || die "cannot read BOOT_MARKER out of tools/test.sh -- refusing to guess the boot banner"

say "evidence dir $RUN (HEAD $HEAD_AT_START)"
EXPECT_TESTS=$(expected_tests) || exit 3
say "expected suite size, derived from kernel/test/test.c: $EXPECT_TESTS"
stamp_pre_mutation

# ---------------------------------------------------------------- leg 1
say "LEG 1: delete the vaddr_start guard, expect the interior fixture RED"
begin_mutation kernel/burrow.c
mutate kernel/burrow.c '    if (vma->vaddr_start != vaddr) return -1;
' '' || die "leg 1 mutation refused"
end_mutation
build interior-unmap
run_suite interior-unmap
unmutate kernel/burrow.c
expect_red interior-unmap burrow.unmap_interior_start_refused \
  "an interior start must be refused"

# ---------------------------------------------------------------- leg 2
say "LEG 2: refund the ring unconditionally, expect the lifecycle fixture RED"
begin_mutation kernel/loom.c
mutate kernel/loom.c '    u32 refund = 0;
    (void)burrow_unref_settled_in(l->ring, as, &refund);
' '    u32 refund = 0;
    u32 paid_unconditionally = burrow_charge_claim_in(l->ring, as);
    (void)burrow_unref_settled_in(l->ring, as, &refund);
    refund = paid_unconditionally;
' || die "leg 2 mutation refused"
end_mutation
build uncond-refund
run_suite uncond-refund
unmutate kernel/loom.c
expect_red uncond-refund loom.private_owner_lifecycle \
  "a nonfinal ring drop refunds the metadata only"

# ------------------------------------------------- the green control, LAST
# Both sources are restored and hash-verified above, but build/ still holds the
# last MUTANT's kernel. This rebuild is what makes the green mean anything.
say "GREEN CONTROL: rebuilt from restored source"
build green
IMAGE_REBUILT=1
run_suite green

fail=0
[ "$SUITE_RC" -eq 0 ] || { echo "FAIL  green: test.sh exited $SUITE_RC"; fail=1; }

# tally() returns the kernel's own line WITH its verdict word, so a FAIL tally
# is reported as a failing suite rather than as a missing one.
tally=$(tally green)
if [ -z "$tally" ]; then
  echo "FAIL  green: no suite tally in the serial log -- the suite did not finish"
  fail=1
elif [ "${tally##* }" != PASS ]; then
  echo "FAIL  green: the kernel's own tally says ${tally} -- tests failed on the clean rebuild"
  fail=1
else
  ran=${tally%%/*}
  tot=${tally#*/}; tot=${tot%% *}
  if [ "$ran" != "$tot" ]; then
    echo "FAIL  green: tally $ran/$tot -- not every test passed"; fail=1
  elif [ "$tot" != "$EXPECT_TESTS" ]; then
    echo "FAIL  green: tally $ran/$tot but $EXPECT_TESTS tests are registered -- tests went missing from the run"
    fail=1
  else
    echo "PASS  green: tally $ran/$tot, matching the $EXPECT_TESTS registrations"
  fi
fi

if grep -qF "$BOOT_MARKER" "$RUN/green-serial.log"; then
  echo "PASS  green: boot completed (\"$BOOT_MARKER\")"
else
  echo "FAIL  green: no boot-completion banner -- the boot did not finish"; fail=1
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

# Only now, with every green check passed, is the image fit to found a claim on.
[ "$fail" -eq 0 ] && IMAGE_QUALIFIED=1

say "SUMMARY"
printf 'HEAD            %s\n' "$HEAD_AT_START"
printf 'evidence        %s\n' "$RUN"
printf 'expected tests  %s\n' "$EXPECT_TESTS"
for f in $MUTATED_FILES; do
  printf '%-15s %s\n' "$(basename "$f")" "$(git hash-object "$f")"
done
if [ "$fail" -eq 0 ]; then
  echo "BOTH FIXTURES ARE WITNESSES: each reddened on its own mutant, for the"
  echo "assertion that mutant targets, as the ONLY failure -- and both pass on a"
  echo "kernel rebuilt from restored source with the full $EXPECT_TESTS-test tally."
else
  echo "GREEN CONTROL IMPERFECT -- read $RUN before claiming anything."
fi
exit "$fail"
