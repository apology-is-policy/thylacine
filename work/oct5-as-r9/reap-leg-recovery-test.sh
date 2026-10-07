#!/bin/sh
# Drive reap-leg-run.sh's RECOVERY half, off-lease, before it is trusted.
#
# WHY A HARNESS AT ALL. astra's 0161 t55 finding was that recovery ran only on
# the happy path. A fix to that is a control-flow claim about paths that, by
# construction, only happen when something has already gone wrong -- so the one
# thing I must not do is ship it unexercised and find out during the lease.
#
# WHAT IT IS AND IS NOT. The functions are EXTRACTED from the live script, never
# retyped, so this cannot drift from what runs. What they are exercised against
# is a STUB tree, which is honest for CONTROL FLOW (does a mismatch fail closed?
# does a pre-mutation exit skip the quarantine?) and worthless for LAYOUT (is
# the kernel image really at build/kernel/thylacine.bin?) -- my stubs encode my
# belief about the layout, which is exactly how the preserve step passed 22
# checks and then missed the file it existed for. So the layout half is asked of
# the REAL tree in L1 below, not of the stub.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
RUN=${RUN:-$ROOT/work/oct5-as-r9/reap-leg-run.sh}   # overridable so MUTANTS of it can be driven
WORK=${TMPDIR:-/tmp}/reap-recovery-$$
pass=0; fail=0
ok()   { pass=$((pass+1)); echo "  ok   $1"; }
bad()  { fail=$((fail+1)); echo "  WRONG: $1"; }
want() { # want <label> <expected> <got>
  if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1 -- expected [$2] got [$3]"; fi
}

# ---- the extraction, with its own denominator control ----
mkdir -p "$WORK"
awk '/^recover\(\) \{/,/^\}$/'  "$RUN" >  "$WORK/fns.sh"
awk '/^on_exit\(\) \{/,/^\}$/'  "$RUN" >> "$WORK/fns.sh"
awk '/^free_gb\(\) \{/,/^\}$/'  "$RUN" >> "$WORK/fns.sh"
for fn in recover on_exit free_gb; do
  if /usr/bin/grep -q "^$fn() {" "$WORK/fns.sh"; then ok "extracted $fn() from the live script"
  else bad "extraction found no $fn() -- THE HARNESS IS BROKEN, not the script"; fi
done

# ---- a scratch tree with the shape recover() touches ----
mktree() { # mktree <dir>
  rm -rf "$1"; mkdir -p "$1/build/kernel" "$1/kernel" "$1/tools" "$1/out"
  printf 'pristine source\n' > "$1/kernel/addrspace.c"
  printf 'MUTANT ELF\n' > "$1/build/kernel/thylacine.elf"
  printf 'MUTANT BIN\n' > "$1/build/kernel/thylacine.bin"
  printf '#!/bin/sh\nprintf "CONTROL BIN\\n" > build/kernel/thylacine.bin\nprintf "CONTROL ELF\\n" > build/kernel/thylacine.elf\nexit ${STUB_BUILD_RC:-0}\n' > "$1/tools/build.sh"
  chmod +x "$1/tools/build.sh"
  printf '#!/bin/sh\ncat "$STUB_LEASE"\n' > "$1/yip"; chmod +x "$1/yip"
  printf 'mac   HELD by you for 0.1h, 1.9h left\n' > "$1/lease-mine.txt"
  printf 'mac   HELD by aux for 1.2h, 3.8h left\n' > "$1/lease-theirs.txt"
}

# The hash the stub build produces, measured rather than assumed.
mktree "$WORK/probe"
CONTROL_HASH=$(printf 'CONTROL BIN\n' | shasum -a 256 | cut -d' ' -f1)

drive() { # drive <scenario> <mutated> <exit-status> <control-hash> <lease> <free-gb> [build-rc] [bad-pristine-hash]
  _s=$1; _mut=$2; _rc=$3; _ch=$4; _lease=$5; _free=$6; _brc=${7:-0}; _bad=${8:-}
  mktree "$WORK/$_s"
  ( cd "$WORK/$_s"
    OUT=out; MUTATED=$_mut; RECOVERED=0; RECOVERY_FAILED=0
    CONTROL_BIN=$_ch; FLOOR_GB=8; YIP=./yip
    STUB_LEASE=$_lease; STUB_BUILD_RC=$_brc
    export STUB_LEASE STUB_BUILD_RC
    cp kernel/addrspace.c out/addrspace.c.pristine
    PRISTINE=out/addrspace.c.pristine
    PRISTINE_HASH=${_bad:-$(shasum -a 256 "$PRISTINE" | cut -d' ' -f1)}
    . "$WORK/fns.sh"
    free_gb() { echo "$_free"; }   # defined AFTER the source, so it wins
    trap on_exit EXIT
    exit "$_rc"
  ) > "$WORK/$_s.log" 2>&1
  echo $?
}

echo
echo "R1 a PRE-MUTATION exit: no quarantine, source restored, status preserved"
got=$(drive r1 0 3 "$CONTROL_HASH" lease-mine.txt 20)
want "R1 exit status is the run's own, not the recovery's" 3 "$got"
[ -d "$WORK/r1/out/mutant-artifacts-DO-NOT-BOOT" ] \
  && bad "R1 quarantined with MUTATED=0" || ok "R1 no quarantine before a mutation"
/usr/bin/grep -q 'RESTORED and hash-verified' "$WORK/r1.log" \
  && ok "R1 restores and verifies the source anyway" || bad "R1 did not verify the source"
/usr/bin/grep -q 'STILL HELD BY YOU' "$WORK/r1.log" \
  && ok "R1 prints the lease-release line on a refusal path" || bad "R1 lost the lease line"

echo
echo "R2 the HAPPY path: quarantine, restore, byte-identical rebuild, exit 0"
got=$(drive r2 1 0 "$CONTROL_HASH" lease-mine.txt 20)
want "R2 exits 0" 0 "$got"
[ -f "$WORK/r2/out/mutant-artifacts-DO-NOT-BOOT/thylacine.bin" ] \
  && ok "R2 the mutant image is quarantined, not deleted (it is evidence)" \
  || bad "R2 the mutant image was not quarantined"
/usr/bin/grep -q 'BYTE-IDENTICAL' "$WORK/r2.log" \
  && ok "R2 byte identity is asserted, not assumed" || bad "R2 no byte-identity line"
[ -f "$WORK/r2/build/kernel/thylacine.bin" ] \
  && ok "R2 build/ holds a kernel image again" || bad "R2 left build/ imageless on the happy path"

echo
echo "R3 the REBUILD DIFFERS -- astra's named case: it must FAIL CLOSED"
got=$(drive r3 1 0 "0000000000000000000000000000000000000000000000000000000000000000" lease-mine.txt 20)
want "R3 exits 9 although the run itself exited 0" 9 "$got"
/usr/bin/grep -q 'FAILS CLOSED' "$WORK/r3.log" \
  && ok "R3 says so in words, not only in the status" || bad "R3 is silent about failing closed"

echo
echo "R4 the DISK FLOOR: no rebuild, quarantine stands, status preserved"
got=$(drive r4 1 2 "$CONTROL_HASH" lease-mine.txt 3)
want "R4 keeps the finding's own status (2), since quarantine IS safe" 2 "$got"
/usr/bin/grep -q 'NO REBUILD: 3 GiB free' "$WORK/r4.log" \
  && ok "R4 refuses the rebuild on the re-measured floor" || bad "R4 rebuilt under the floor"
[ -f "$WORK/r4/build/kernel/thylacine.bin" ] \
  && bad "R4 left an image in build/ without rebuilding" \
  || ok "R4 leaves build/ imageless, so nothing can boot the mutant"

echo
echo "R5 the LEASE IS GONE: no rebuild, and the cores are not taken back"
got=$(drive r5 1 2 "$CONTROL_HASH" lease-theirs.txt 20)
want "R5 keeps the run's status" 2 "$got"
/usr/bin/grep -q 'lease is no longer mine' "$WORK/r5.log" \
  && ok "R5 refuses the rebuild without a lease" || bad "R5 built without the lease"

echo
echo "R6 the RECOVERY BUILD FAILS: fail closed, and the log is named"
got=$(drive r6 1 0 "$CONTROL_HASH" lease-mine.txt 20 7)
want "R6 exits 9" 9 "$got"
/usr/bin/grep -q 'RECOVERY BUILD FAILED' "$WORK/r6.log" \
  && ok "R6 names the failed build and its log" || bad "R6 did not report the build failure"

echo
echo "R7 a FAILED SOURCE RESTORE must be nonzero, not a printed remark"
# The hash is one the restored file CANNOT match, so the defect under test is a
# restore that reports success without having restored anything.
got=$(drive r7 0 0 "$CONTROL_HASH" lease-mine.txt 20 0 deadbeef)
want "R7 exits 9 on a restore whose hash does not verify" 9 "$got"
/usr/bin/grep -q 'restore it by hand' "$WORK/r7.log" \
  && ok "R7 tells the reader where the pristine copy is" || bad "R7 does not point at the pristine copy"

echo
echo "L1 THE LAYOUT, asked of the REAL tree and not of a stub"
layout_paths=$(/usr/bin/grep -oE 'build/kernel/thylacine\.(elf|bin)' "$RUN" | sort -u | wc -l | tr -d ' ')
if [ "$layout_paths" -lt 2 ]; then
  bad "L1 extracted only $layout_paths image path(s) from the script -- BROKEN ARM"
elif [ ! -d "$ROOT/build/kernel" ]; then
  echo "  SKIPPED (loudly): $ROOT/build/kernel does not exist, so the layout"
  echo "          premise is UNCHECKED -- re-run this after a build."
else
  for f in build/kernel/thylacine.elf build/kernel/thylacine.bin; do
    [ -f "$ROOT/$f" ] && ok "L1 $f exists in the real build tree" \
                      || bad "L1 $f is NOT where the script quarantines from"
  done
fi

echo
echo "RESULT: $pass pass, $fail wrong"
rm -rf "$WORK"
[ "$fail" = 0 ] || exit 1
