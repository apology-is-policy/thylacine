#!/bin/sh
# Exercises private-owner-red-legs.sh's CONTROL FLOW -- cleanup, freshness,
# attribution, the three-facts marker -- without a lease, a build or a boot.
#
# WHY THIS EXISTS. A parser replica tested against a log proves the parsers and
# nothing else (astra, yip 0161 t34): it never runs the trap, never fails a
# build, never meets a stale serial log, never reaches the clean-rebuild
# bookkeeping. Those are the paths that decide whether a run can quietly report
# success while leaving mutated source or an unqualified kernel behind, and they
# had never executed.
#
# HOW, without a seam in the script under test. Everything the runner touches is
# relative to ROOT, so each scenario gets a throwaway git repo whose tools/ holds
# stub build/test scripts and whose kernel/ holds REAL copies of the files it
# mutates -- so the mutation anchors must genuinely bite. The runner is invoked
# unmodified. There is deliberately no command-override switch in the runner: a
# switch that can swap the build or the suite is a switch that can fake a gate.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# Overridable so the harness can be run against an EARLIER runner: a test that
# has only ever seen the fixed script does not show that it discriminates.
RUNNER=${RUNNER:-$ROOT/work/oct5-as-r9/private-owner-red-legs.sh}
# A real recorded serial log, so the verdict/tally/attribution patterns are
# exercised against genuine kernel output at full scale rather than only against
# the handful of lines a stub writes.
REAL_LOG=${REAL_LOG:-$ROOT/work/oct5-as-r9/boot-logs/boot-confirm-232503Z.log}
[ -f "$REAL_LOG" ] || { printf 'no real serial log at %s -- refusing to run with stubs alone\n' "$REAL_LOG"; exit 2; }
WORK=${WORK:-${TMPDIR:-/tmp}/red-legs-wrapper-test.$$}
PASS=0
FAIL=0

note() { printf '  %s\n' "$*"; }
ok()   { PASS=$((PASS + 1)); printf 'OK    %s\n' "$*"; }
bad()  { FAIL=$((FAIL + 1)); printf 'WRONG %s\n' "$*"; }

check() { # check <label> <got> <want>
  if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1: got='$2' want='$3'"; fi
}
check_has() { # check_has <label> <file> <substring>
  if grep -qF "$3" "$2"; then ok "$1"; else bad "$1: '$3' not in $2"; fi
}
check_lacks() { # check_lacks <label> <file> <substring>
  if grep -qF "$3" "$2"; then bad "$1: '$3' unexpectedly in $2"; else ok "$1"; fi
}

# A scratch tree that is a faithful miniature: real mutated files, real git, a
# tools/test.sh that declares BOOT_MARKER exactly as the real one does (the
# runner derives the banner from it rather than hard-coding it).
new_tree() { # new_tree <name> -> echoes the path
  t=$WORK/$1
  mkdir -p "$t/kernel/test" "$t/tools" "$t/build" "$t/work/oct5-as-r9"
  cp "$ROOT/kernel/burrow.c" "$ROOT/kernel/loom.c" "$t/kernel/"
  cp "$ROOT/kernel/test/test.c" "$t/kernel/test/"
  cp "$REAL_LOG" "$t/fixture-serial.log"
  cat > "$t/work/oct5-as-r9/rebuild-kernel.sh" <<'STUB'
#!/bin/sh
set -u
n=$(cat build/.builds 2>/dev/null || echo 0); n=$((n + 1)); printf '%s\n' "$n" > build/.builds
printf 'stub build #%s args: %s\n' "$n" "$*"
[ -f ./stub-plan ] && . ./stub-plan
if [ -n "${BUILD_SLEEP:-}" ]; then printf 'building\n' > build/.build-started; sleep "$BUILD_SLEEP"; fi
if [ "${FOREIGN_EDIT_ON:-x}" = "$n" ]; then
  printf '\n// a third party was here\n' >> kernel/loom.c
  printf 'stub: a third party edited kernel/loom.c during build #%s\n' "$n"
fi
if [ "${BUILD_FAIL_ON:-x}" = "$n" ]; then printf 'stub: deliberate build failure\n'; exit 2; fi
exit 0
STUB
  # Declared the way tools/test.sh:121 declares it, because the runner reads it
  # from here; a stub that spelled it differently would test the wrong banner.
  cat > "$t/tools/test.sh" <<'STUB'
#!/bin/sh
set -u
BOOT_MARKER="Thylacine boot OK"
n=$(cat build/.suites 2>/dev/null || echo 0); n=$((n + 1)); printf '%s\n' "$n" > build/.suites
[ -f ./stub-plan ] && . ./stub-plan
eval "mode=\${SUITE_${n}_MODE:-green}"
eval "rc=\${SUITE_${n}_RC:-0}"
L=build/test-boot.log
emit_head() { printf '==> qemu: accel=hvf\n' > "$L"; }
emit_tail() { printf '  tests: %s/%s PASS\n' "$1" "$2" >> "$L"; printf '%s\n' "$BOOT_MARKER" >> "$L"; }
case "$mode" in
  nolog)   printf 'stub: exiting before the boot; no new serial log\n'; exit "$rc" ;;
  empty)   : > "$L"; exit "$rc" ;;
  red1)    emit_head
           printf '    [test] burrow.unmap_interior_start_refused ... FAIL: an interior start must be refused -- v1.0 has no partial unmap\n' >> "$L"
           printf 'EXTINCTION: test failure\n' >> "$L" ;;
  red1wrong) emit_head
           printf '    [test] burrow.unmap_interior_start_refused ... FAIL: proc_alloc failed\n' >> "$L"
           printf 'EXTINCTION: test failure\n' >> "$L" ;;
  red1plus) emit_head
           printf '    [test] burrow.unmap_interior_start_refused ... FAIL: an interior start must be refused -- v1.0 has no partial unmap\n' >> "$L"
           printf '    [test] weft.ring_teardown ... FAIL: something else broke\n' >> "$L"
           printf 'EXTINCTION: test failure\n' >> "$L" ;;
  red2)    emit_head
           printf '    [test] burrow.unmap_interior_start_refused ... PASS\n' >> "$L"
           printf '    [test] loom.private_owner_lifecycle ... FAIL: a nonfinal ring drop refunds the metadata only\n' >> "$L"
           printf 'EXTINCTION: test failure\n' >> "$L" ;;
  green)   emit_head
           printf '    [test] burrow.unmap_interior_start_refused ... PASS\n' >> "$L"
           printf '    [test] loom.private_owner_lifecycle ... PASS\n' >> "$L"
           emit_tail "${SUITE_TALLY:-1836}" "${SUITE_TALLY:-1836}" ;;
  reallog) cp fixture-serial.log "$L" ;;
  greenshort) emit_head
           printf '    [test] burrow.unmap_interior_start_refused ... PASS\n' >> "$L"
           printf '    [test] loom.private_owner_lifecycle ... PASS\n' >> "$L"
           emit_tail 1835 1835 ;;
  *) printf 'stub: unknown mode %s\n' "$mode" >&2; exit 9 ;;
esac
exit "$rc"
STUB
  chmod 755 "$t/work/oct5-as-r9/rebuild-kernel.sh" "$t/tools/test.sh"
  ( cd "$t" && git init -q . && git add -A kernel tools && git -c user.email=h@x -c user.name=h commit -qm base ) >/dev/null
  echo "$t"
}

run_runner() { # run_runner <tree> -> status in RC, output in $tree/run.out
  ( cd "$1" && ROOT="$1" sh "$RUNNER" ) > "$1/run.out" 2>&1
  RC=$?
}

src_intact() { # src_intact <tree> -> yes|no
  a=$(cd "$1" && git status --porcelain -- kernel | wc -l | tr -d ' ')
  [ "$a" = 0 ] && echo yes || echo no
}

mkdir -p "$WORK"
printf 'wrapper test, scratch in %s\n' "$WORK"

# ---------------------------------------------------------------- S1 happy path
printf '\n-- S1 happy path: both legs red for their own reason, green clean\n'
T=$(new_tree s1)
cat > "$T/stub-plan" <<'P'
SUITE_1_MODE=red1; SUITE_1_RC=1
SUITE_2_MODE=red2; SUITE_2_RC=1
SUITE_3_MODE=green; SUITE_3_RC=0
P
run_runner "$T"
check "S1 exit status" "$RC" 0
check_has "S1 credits leg 1" "$T/run.out" "PASS  leg=interior-unmap"
check_has "S1 credits leg 2" "$T/run.out" "PASS  leg=uncond-refund"
check_has "S1 derives the expected suite size" "$T/run.out" "derived from kernel/test/test.c: 1836"
check_has "S1 pins the tally to the registrations" "$T/run.out" "matching the 1836 registrations"
check_has "S1 reports both as witnesses" "$T/run.out" "BOTH FIXTURES ARE WITNESSES"
check "S1 source restored" "$(src_intact "$T")" yes
check "S1 marker removed on qualification" "$([ -f "$T/build/MUTANT-UNQUALIFIED" ] && echo present || echo absent)" absent
check "S1 built three times" "$(cat "$T/build/.builds")" 3

# ------------------------------------------------------- S2 mutant build failure
printf '\n-- S2 the first mutant build fails\n'
T=$(new_tree s2)
printf 'BUILD_FAIL_ON=1\n' > "$T/stub-plan"
run_runner "$T"
check "S2 refuses" "$RC" 3
check_has "S2 names the failing build" "$T/run.out" "build failed for interior-unmap"
check "S2 source restored anyway" "$(src_intact "$T")" yes
check_has "S2 marker present" "$T/build/MUTANT-UNQUALIFIED" "built from MUTATED source"
check_has "S2 marker separates the three facts" "$T/build/MUTANT-UNQUALIFIED" "image rebuilt : 0"

# --------------------------------------------------------- S3 stale serial log
printf '\n-- S3 test.sh exits before producing a new serial log, with a stale FAIL log present\n'
T=$(new_tree s3)
cat > "$T/stub-plan" <<'P'
SUITE_1_MODE=nolog; SUITE_1_RC=1
P
# A prior run's log that would credit the leg if it were ever read.
printf '    [test] burrow.unmap_interior_start_refused ... FAIL: an interior start must be refused -- v1.0 has no partial unmap\n' > "$T/build/test-boot.log"
run_runner "$T"
check "S3 refuses" "$RC" 3
check_has "S3 refuses for absent NEW evidence" "$T/run.out" "no NEW serial log after test.sh"
check_lacks "S3 credits nothing" "$T/run.out" "PASS  leg="
check "S3 displaced the stale log" "$([ -f "$T"/work/oct5-as-r9/private-owner-logs/red-legs/*/interior-unmap-displaced-prior-serial.log ] && echo kept || echo lost)" kept
check "S3 source restored" "$(src_intact "$T")" yes

# ------------------------------------------------------------- S4 wrong reason
printf '\n-- S4 the intended test reddens, but for a different assertion\n'
T=$(new_tree s4)
cat > "$T/stub-plan" <<'P'
SUITE_1_MODE=red1wrong; SUITE_1_RC=1
P
run_runner "$T"
check "S4 refuses" "$RC" 3
check_has "S4 names the wrong reason" "$T/run.out" "reddened for the WRONG reason"
check_lacks "S4 credits nothing" "$T/run.out" "PASS  leg="

# ------------------------------------------------------------ S5 unattributed
printf '\n-- S5 the intended test reddens and so does another\n'
T=$(new_tree s5)
cat > "$T/stub-plan" <<'P'
SUITE_1_MODE=red1plus; SUITE_1_RC=1
P
run_runner "$T"
check "S5 refuses" "$RC" 3
check_has "S5 names the other failure" "$T/run.out" "weft.ring_teardown"
check_has "S5 calls it unattributed" "$T/run.out" "unattributed"

# -------------------------------------------------------- S6 red leg with rc 0
printf '\n-- S6 the named test shows FAIL but test.sh exited 0\n'
T=$(new_tree s6)
cat > "$T/stub-plan" <<'P'
SUITE_1_MODE=red1; SUITE_1_RC=0
P
run_runner "$T"
check "S6 refuses" "$RC" 3
check_has "S6 refuses on the suite status" "$T/run.out" "test.sh exited 0"

# ------------------------------------------------------- S7 short green tally
printf '\n-- S7 green control passes every test but runs fewer than are registered\n'
T=$(new_tree s7)
cat > "$T/stub-plan" <<'P'
SUITE_1_MODE=red1; SUITE_1_RC=1
SUITE_2_MODE=red2; SUITE_2_RC=1
SUITE_3_MODE=greenshort; SUITE_3_RC=0
P
run_runner "$T"
check "S7 fails the run" "$RC" 1
check_has "S7 names the missing tests" "$T/run.out" "tests went missing from the run"
check_has "S7 withholds the claim" "$T/run.out" "GREEN CONTROL IMPERFECT"
check "S7 marker KEPT though source is clean and image rebuilt" \
  "$([ -f "$T/build/MUTANT-UNQUALIFIED" ] && echo present || echo absent)" present
check_has "S7 marker says rebuilt but not green" "$T/build/MUTANT-UNQUALIFIED" "image rebuilt : 1"
check "S7 source restored" "$(src_intact "$T")" yes

# ------------------------------- S8 a concurrent edit the run must NOT overwrite
printf '\n-- S8 a third party edits a mutated file mid-run\n'
T=$(new_tree s8)
cat > "$T/stub-plan" <<'P'
SUITE_1_MODE=red1; SUITE_1_RC=1
P
# Leg 1's build edits loom.c -- a file this run preserved but has not mutated --
# standing in for any concurrent writer. It must stop the run at leg 2's
# begin_mutation guard AND survive cleanup unoverwritten.
printf 'FOREIGN_EDIT_ON=1\n' >> "$T/stub-plan"
run_runner "$T"
check_has "S8 refuses to mutate the changed file" "$T/run.out" "refusing to mutate a file something else changed"
check_has "S8 refuses to overwrite the foreign edit" "$T/run.out" "REFUSING to overwrite an edit this run did not make"
check_has "S8 still credits leg 1, which did complete" "$T/run.out" "PASS  leg=interior-unmap"
check "S8 forces a nonzero status" "$([ "$RC" -ne 0 ] && echo nonzero || echo zero)" nonzero
check "S8 leaves the foreign edit in place" \
  "$(grep -c 'a third party was here' "$T/kernel/loom.c")" 1
check_has "S8 marker present" "$T/build/MUTANT-UNQUALIFIED" "IS NOT QUALIFIED"

# ------------------------------------------------------------------- S9 SIGTERM
printf '\n-- S9 SIGTERM arrives during the first mutant build\n'
T=$(new_tree s9)
printf 'BUILD_SLEEP=20\n' > "$T/stub-plan"
( cd "$T" && ROOT="$T" exec sh "$RUNNER" ) > "$T/run.out" 2>&1 &
RUNNER_PID=$!
i=0
while [ ! -f "$T/build/.build-started" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
if [ -f "$T/build/.build-started" ]; then
  kill -TERM "$RUNNER_PID" 2>/dev/null
  wait "$RUNNER_PID" 2>/dev/null; RC=$?
  check "S9 explicit SIGTERM status" "$RC" 143
  check_has "S9 names the signal" "$T/run.out" "cleanup (SIGTERM"
  check_has "S9 stops its own child first" "$T/run.out" "stopping the owned child"
  check "S9 source restored" "$(src_intact "$T")" yes
  check_has "S9 marker present" "$T/build/MUTANT-UNQUALIFIED" "IS NOT QUALIFIED"
else
  bad "S9 the stub build never started; scenario did not run"
  kill -TERM "$RUNNER_PID" 2>/dev/null; wait "$RUNNER_PID" 2>/dev/null
fi

# ------------------------------- S11 the parsers against a REAL serial log
printf '\n-- S11 green control reads a REAL recorded boot log (1835 [test] lines, pre-dating both new tests)\n'
T=$(new_tree s11)
cat > "$T/stub-plan" <<'P'
SUITE_1_MODE=red1; SUITE_1_RC=1
SUITE_2_MODE=red2; SUITE_2_RC=1
SUITE_3_MODE=reallog; SUITE_3_RC=0
P
run_runner "$T"
note "real log: $REAL_LOG ($(/usr/bin/grep -c '\[test\]' "$REAL_LOG") [test] lines)"
check "S11 fails the run" "$RC" 1
check_has "S11 reads the real tally" "$T/run.out" "tally 1834/1834"
check_has "S11 catches the stale image by count" "$T/run.out" "tests went missing from the run"
check_has "S11 finds the real boot banner" "$T/run.out" "PASS  green: boot completed"
check_has "S11 finds no failing test in 1835 real verdict lines" "$T/run.out" "PASS  green: no failing test"
check_has "S11 reports the absent new test as ABSENT, not as a pass" "$T/run.out" "loom.private_owner_lifecycle -> ABSENT"
check "S11 marker kept" "$([ -f "$T/build/MUTANT-UNQUALIFIED" ] && echo present || echo absent)" present

# --------------- the QEMU predicate: discrimination on recorded ps output
printf '\n-- S10 the owned-QEMU filter discriminates (recorded input; signals nothing)\n'
# The program is EXTRACTED from the runner rather than retyped, so this cannot
# drift into testing a copy that the runner no longer uses.
PROG=$(sed -n "s/.*awk -v r=\"\$ROOT\/build\" '\(.*\)'.*/\1/p" "$RUNNER" | head -1)
if [ -z "$PROG" ]; then
  bad "S10 could not extract the filter from the runner -- nothing was tested"
else
  note "filter under test: $PROG"
  # Real recorded lines: a peer's running guest, and a same-named binary booting
  # this tree. Format matches ps -Ao pid=,comm=,command=.
  peer=/Users/northkillpd/projects/thylacine
  cat > "$WORK/ps.fixture" <<FIX
11406 /opt/homebrew/bin/qemu-system-aarch64 qemu-system-aarch64 -machine virt,gic-version=2,accel=hvf -kernel $peer/build/kernel-undefined/thylacine.bin -initrd $peer/build/ramfs.cpio -nographic
22222 /opt/homebrew/bin/qemu-system-aarch64 qemu-system-aarch64 -machine virt -kernel $ROOT/build/kernel/thylacine.bin -nographic
33333 /bin/sh sh -c echo qemu-system-aarch64 $ROOT/build/decoy
FIX
  hits_mine=$(awk -v r="$ROOT/build" "$PROG" "$WORK/ps.fixture" | tr '\n' ' ' | sed 's/ $//')
  hits_peer=$(awk -v r="$peer/build" "$PROG" "$WORK/ps.fixture" | tr '\n' ' ' | sed 's/ $//')
  check "S10 matches this tree's guest only" "$hits_mine" "22222"
  check "S10 matches the peer's guest only when given the peer's root" "$hits_peer" "11406"
  note "the sh decoy naming qemu and this build path is excluded by the comm test"
fi

printf '\n== %s passed, %s wrong (scratch kept at %s)\n' "$PASS" "$FAIL" "$WORK"
[ "$FAIL" -eq 0 ] || exit 1
