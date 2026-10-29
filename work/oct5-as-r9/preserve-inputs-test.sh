#!/bin/sh
# Tests the preserve_boot_inputs step of lease-runbook.sh -- the step, not a
# copy of it: the function is EXTRACTED from the live runbook so this harness
# cannot pass against a stale duplicate. Needs no lease, no build, no guest.
#
# Why it exists: on 2026-10-07 four boot inputs that every private-owner verdict
# attached to lived only in build/, and a bake destroyed them. The kernel half
# was recoverable (a flat binary is a pure function of a retained ELF); the
# ramfs was not. The cure is a script step, and a step nothing exercises is a
# step that will be wrong when it matters.
#
# AND THE LESSON THAT ADDED S6-S8: the first 22 checks were all stub-driven and
# passed while the step had two defects a real build/ exposed on first contact --
# the stubs encoded MY BELIEF ABOUT THE LAYOUT, so they could only confirm it.
# S8 drives the premise from the REAL build/ tree and carries its own positive
# control, so a wrong path cannot be green here again.
#
#   sh work/oct5-as-r9/preserve-inputs-test.sh [runbook]
RUNBOOK=${1:-work/oct5-as-r9/lease-runbook.sh}
[ -f "$RUNBOOK" ] || { echo "no runbook at $RUNBOOK"; exit 2; }
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
FN=$T/preserve-fn.sh
awk '/^PRESERVE_DIR=work\/oct5-as-r9\/boot-inputs$/,/^}$/' "$RUNBOOK" > "$FN"
# DENOMINATOR CONTROL: an extraction that silently yields nothing would make
# every scenario below vacuously green.
lines=$(wc -l < "$FN" | tr -d ' ')
[ "$lines" -ge 20 ] || { echo "REFUSING: extracted only $lines lines of the function from $RUNBOOK"; exit 2; }
/usr/bin/grep -q 'preserve_boot_inputs()' "$FN" || { echo "REFUSING: extraction has no function definition"; exit 2; }
sh -n "$FN" || { echo "REFUSING: extracted function does not parse"; exit 2; }
echo "-- exercising the function as extracted from $RUNBOOK ($lines lines)"
pass=0; fail=0; skip=0
ck() { # ck <desc> <expected> <actual>
  if [ "$2" = "$3" ]; then pass=$((pass+1)); else
    fail=$((fail+1)); echo "  WRONG: $1 -- expected [$2] got [$3]"; fi
}
mktree() { # mktree <root> [omit...]
  rm -rf "$1"; mkdir -p "$1/build/kernel" "$1/build/kernel-undefined" "$1/build/fixtures"
  echo DEFAULT-ELF > "$1/build/kernel/thylacine.elf"
  echo DEFAULT-BIN > "$1/build/kernel/thylacine.bin"
  echo UBSAN-ELF   > "$1/build/kernel-undefined/thylacine.elf"
  echo UBSAN-BIN   > "$1/build/kernel-undefined/thylacine.bin"
  echo RAMFS       > "$1/build/ramfs.cpio"
  echo POOLSNAP    > "$1/build/fixtures/pool.img.baked-snapshot"
  echo KEY         > "$1/build/fixtures/system.key"
  echo KEY         > "$1/build/fixtures/system.key.baked-snapshot"
  # The real file lives at build/.config, NOT build/kernel/.config, and it names
  # its own flavour on a SANITIZE line. Both facts are what S6 discriminates on.
  printf 'BUILD_TYPE       = debug    # Kernel build type\nSANITIZE         = %s    # Kernel sanitizer\n' "${2:-none}" > "$1/build/.config"
  for o in $3 $4; do [ -n "$o" ] && rm -f "$1/$o"; done
  return 0
}

echo "== S1: full set -- all nine preserved, flavours distinct =="
R=$T/t1; mktree "$R"
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs preserve_boot_inputs run-1 pb default ) > "$R/out" 2>&1
ck "S1 exit" 0 $?
ck "S1 kept 9" 1 "$(/usr/bin/grep -c 'preserved 9 boot input' "$R/out")"
ck "S1 no ABSENT line" 0 "$(/usr/bin/grep -c 'ABSENT' "$R/out")"
ck "S1 phase in the directory name" 1 "$([ -d "$R/inputs/run-1-pb" ] && echo 1 || echo 0)"
ck "S1 files on disk" 9 "$(ls -1A "$R/inputs/run-1-pb" | /usr/bin/grep -vcE 'HASHES.txt|PROVENANCE.txt')"
ck "S1 hash lines" 9 "$(wc -l < "$R/inputs/run-1-pb/HASHES.txt" | tr -d ' ')"
ck "S1 .config hashed (a dotfile the plain glob misses)" 1 "$(/usr/bin/grep -c '\.config-default' "$R/inputs/run-1-pb/HASHES.txt")"
# THE DISCRIMINATING CHECK: a flat copy would have one flavour overwrite the other.
ck "S1 default elf content"  DEFAULT-ELF "$(cat "$R/inputs/run-1-pb/thylacine.elf")"
ck "S1 ubsan elf content"    UBSAN-ELF   "$(cat "$R/inputs/run-1-pb/thylacine-undefined.elf")"
ck "S1 default bin content"  DEFAULT-BIN "$(cat "$R/inputs/run-1-pb/thylacine.bin")"
ck "S1 ubsan bin content"    UBSAN-BIN   "$(cat "$R/inputs/run-1-pb/thylacine-undefined.bin")"
ck "S1 provenance names the phase" 1 "$(/usr/bin/grep -c '^phase    : pb' "$R/inputs/run-1-pb/PROVENANCE.txt")"
ck "S1 provenance stamp is a Z time" 1 "$(/usr/bin/grep -cE '^taken    : 20[0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z$' "$R/inputs/run-1-pb/PROVENANCE.txt")"

echo "== S2: two inputs absent -- recorded, never substituted =="
R=$T/t2; mktree "$R" none build/kernel-undefined/thylacine.bin build/ramfs.cpio
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs preserve_boot_inputs run-1 pb default ) > "$R/out" 2>&1
ck "S2 exit" 0 $?
ck "S2 kept 7" 1 "$(/usr/bin/grep -c 'preserved 7 boot input' "$R/out")"
ck "S2 names ubsan bin" 1 "$(/usr/bin/grep -c 'kernel-undefined/thylacine.bin' "$R/out")"
ck "S2 names ramfs" 1 "$(/usr/bin/grep -c 'build/ramfs.cpio' "$R/out")"
ck "S2 no ramfs substituted" 0 "$(ls -1A "$R/inputs/run-1-pb" | /usr/bin/grep -c '^ramfs.cpio$')"

echo "== S3: found but uncopyable -- must REFUSE =="
R=$T/t3; mktree "$R"; chmod 000 "$R/build/ramfs.cpio"
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs preserve_boot_inputs run-1 pb default ) > "$R/out" 2>&1
rc=$?; chmod 644 "$R/build/ramfs.cpio"
ck "S3 refuses nonzero" 1 "$([ $rc -ne 0 ] && echo 1 || echo 0)"
ck "S3 says REFUSING" 1 "$(/usr/bin/grep -c 'REFUSING: found' "$R/out")"

echo "== S4: pruning keeps KEEP_INPUT_GENS, and a run's own pair survives it =="
R=$T/t4; mktree "$R"
for g in 1 2 3; do
  ( cd "$R" && . "$FN" && PRESERVE_DIR=inputs KEEP_INPUT_GENS=2 \
      preserve_boot_inputs run-$g pb default ) > "$R/out-$g" 2>&1
  sleep 1
done
ck "S4 two generations remain" 2 "$(ls -1d "$R"/inputs/*/ 2>/dev/null | wc -l | tr -d ' ')"
ck "S4 oldest gone" 0 "$([ -d "$R/inputs/run-1-pb" ] && echo 1 || echo 0)"
ck "S4 newest kept" 1 "$([ -d "$R/inputs/run-3-pb" ] && echo 1 || echo 0)"
ck "S4 announced prune" 1 "$(/usr/bin/grep -c 'pruned old generation' "$R/out-3")"
# THE TWO-GENERATIONS-PER-RUN CASE, which is the whole reason the bound needed
# re-reading: the post-gate call must never evict its own post-build sibling.
R=$T/t4b; mktree "$R"
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs KEEP_INPUT_GENS=2 preserve_boot_inputs run-9 postbuild default ) >/dev/null 2>&1
sleep 1
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs KEEP_INPUT_GENS=2 preserve_boot_inputs run-9 postgate undefined ) >/dev/null 2>&1
ck "S4b sibling postbuild survives" 1 "$([ -d "$R/inputs/run-9-postbuild" ] && echo 1 || echo 0)"
ck "S4b postgate written"           1 "$([ -d "$R/inputs/run-9-postgate" ] && echo 1 || echo 0)"

echo "== S5: positive control -- the harness can SEE a flat-copy collision =="
R=$T/t5; mktree "$R"
( cd "$R" && mkdir -p inputs/flat && for f in build/kernel/thylacine.elf \
    build/kernel-undefined/thylacine.elf; do cp "$f" inputs/flat/; done )
ck "S5 flat copy collides to 1 file" 1 "$(ls -1A "$R/inputs/flat" | wc -l | tr -d ' ')"
ck "S5 collision loses the default" UBSAN-ELF "$(cat "$R/inputs/flat/thylacine.elf")"

echo "== S6: the .config flavour is READ FROM THE FILE, not from the caller =="
# build/.config is shared and rewritten by every flavour, so a step that trusts
# its argument mislabels the artifact. Here the file says ubsan while the call
# claims default: the destination must follow the FILE.
R=$T/t6; mktree "$R" ubsan
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs preserve_boot_inputs run-1 pg default ) > "$R/out" 2>&1
ck "S6 exit" 0 $?
ck "S6 named for the OBSERVED flavour" 1 "$([ -f "$R/inputs/run-1-pg/.config-undefined" ] && echo 1 || echo 0)"
ck "S6 did NOT take the caller's word" 0 "$([ -f "$R/inputs/run-1-pg/.config-default" ] && echo 1 || echo 0)"
ck "S6 announced the disagreement" 1 "$(/usr/bin/grep -c 'MISLABEL AVOIDED' "$R/out")"
ck "S6 provenance records both" 1 "$(/usr/bin/grep -c 'SANITIZE=ubsan -> undefined' "$R/inputs/run-1-pg/PROVENANCE.txt")"
# And the agreeing case must stay quiet, or the warning is noise nobody reads.
R=$T/t6b; mktree "$R" ubsan
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs preserve_boot_inputs run-1 pg undefined ) > "$R/out" 2>&1
ck "S6b agreement is silent" 0 "$(/usr/bin/grep -c 'MISLABEL' "$R/out")"
ck "S6b still named undefined" 1 "$([ -f "$R/inputs/run-1-pg/.config-undefined" ] && echo 1 || echo 0)"

echo "== S7: a bound that cannot hold one run's generations is REFUSED =="
R=$T/t7; mktree "$R"
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs KEEP_INPUT_GENS=1 preserve_boot_inputs run-1 pb default ) > "$R/out" 2>&1
ck "S7 refuses nonzero" 1 "$([ $? -ne 0 ] && echo 1 || echo 0)"
ck "S7 says why" 1 "$(/usr/bin/grep -c 'cannot hold one run' "$R/out")"
ck "S7 wrote nothing" 0 "$([ -d "$R/inputs/run-1-pb" ] && echo 1 || echo 0)"
echo "== S7b: a wrong arity is REFUSED, not silently relabelled =="
R=$T/t7b; mktree "$R"
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs preserve_boot_inputs run-1 ) > "$R/out" 2>&1
ck "S7b refuses nonzero" 1 "$([ $? -ne 0 ] && echo 1 || echo 0)"
ck "S7b names the signature" 1 "$(/usr/bin/grep -c 'REFUSING: preserve_boot_inputs <run-stamp>' "$R/out")"

echo "== S9: a PINNED generation is never pruned and never counted =="
R=$T/t9; mktree "$R"
for g in 1 2 3; do
  ( cd "$R" && . "$FN" && PRESERVE_DIR=inputs KEEP_INPUT_GENS=2 \
      preserve_boot_inputs run-$g pb default ) > "$R/out-$g" 2>&1
  [ "$g" = 1 ] && touch "$R/inputs/run-1-pb/PINNED"
  sleep 1
done
ck "S9 pinned oldest survives" 1 "$([ -d "$R/inputs/run-1-pb" ] && echo 1 || echo 0)"
ck "S9 and is not counted, so both unpinned remain" 3 "$(ls -1d "$R"/inputs/*/ | wc -l | tr -d ' ')"
ck "S9 says so" 1 "$(/usr/bin/grep -c 'PINNED, neither counted nor pruned' "$R/out-3")"
# CONTROL one variable away: identical schedule without the pin must prune it.
R=$T/t9b; mktree "$R"
for g in 1 2 3; do
  ( cd "$R" && . "$FN" && PRESERVE_DIR=inputs KEEP_INPUT_GENS=2 \
      preserve_boot_inputs run-$g pb default ) >/dev/null 2>&1
  sleep 1
done
ck "S9b control: unpinned oldest IS pruned" 0 "$([ -d "$R/inputs/run-1-pb" ] && echo 1 || echo 0)"
ck "S9b control: two remain" 2 "$(ls -1d "$R"/inputs/*/ | wc -l | tr -d ' ')"

echo "== S8: THE PREMISE, checked against the REAL build/ tree =="
# Every stub above is built from my own belief about the layout. This arm takes
# the source paths OUT OF THE FUNCTION and asks the real tree whether they
# exist -- the check that would have caught build/kernel/.config on day one.
REAL=${REAL_BUILD:-build}
srcs=$(/usr/bin/grep -oE '"build/[^:]+:' "$FN" | sed 's/^"//; s/:$//')
nsrc=$(printf '%s\n' "$srcs" | /usr/bin/grep -c 'build/')
if [ ! -d "$REAL/kernel" ]; then
  echo "  SKIPPED (no $REAL/kernel here -- run this from a tree that has built)"
  skip=$((skip+4))
elif [ "$nsrc" -lt 8 ]; then
  echo "  WRONG: S8 extracted only $nsrc source paths from the function -- the"
  echo "         EXTRACTION is broken, so this arm proves nothing."
  fail=$((fail+1))
else
  missing=
  for s in $srcs; do
    [ -f "$s" ] && continue
    # kernel-undefined exists only after ci-smp-gate builds that flavour, so it
    # is the one legitimately-absent family in a post-build tree.
    case "$s" in build/kernel-undefined/*) continue ;; esac
    missing="$missing $s"
  done
  ck "S8 every non-flavour-conditional source path exists ($nsrc checked)" "" "$missing"
  # POSITIVE CONTROL for this arm, one variable away: the path the step used to
  # name must be absent, and the one it names now must be present. If both were
  # present the arm could not tell a right premise from a wrong one.
  ck "S8 control: build/kernel/.config does NOT exist" 0 "$([ -f build/kernel/.config ] && echo 1 || echo 0)"
  ck "S8 control: build/.config DOES exist" 1 "$([ -f build/.config ] && echo 1 || echo 0)"
  ck "S8 the function reads build/.config" 1 "$(/usr/bin/grep -c '"build/\.config:' "$FN")"
fi

echo
echo "RESULT: $pass pass, $fail wrong, $skip skipped"
[ "$fail" = 0 ] || exit 1
