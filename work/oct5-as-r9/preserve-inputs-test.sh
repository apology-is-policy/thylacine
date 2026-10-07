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
pass=0; fail=0
ck() { # ck <desc> <expected> <actual>
  if [ "$2" = "$3" ]; then pass=$((pass+1)); else
    fail=$((fail+1)); echo "  WRONG: $1 -- expected [$2] got [$3]"; fi
}
mktree() { # mktree <root> [omit...]
  rm -rf "$1"; mkdir -p "$1/build/kernel" "$1/build/kernel-undefined" "$1/build/fixtures"
  echo DEFAULT-ELF > "$1/build/kernel/thylacine.elf"
  echo DEFAULT-BIN > "$1/build/kernel/thylacine.bin"
  echo CONFIG      > "$1/build/kernel/.config"
  echo UBSAN-ELF   > "$1/build/kernel-undefined/thylacine.elf"
  echo UBSAN-BIN   > "$1/build/kernel-undefined/thylacine.bin"
  echo RAMFS       > "$1/build/ramfs.cpio"
  echo POOLSNAP    > "$1/build/fixtures/pool.img.baked-snapshot"
  echo KEY         > "$1/build/fixtures/system.key"
  for o in $2 $3; do [ -n "$o" ] && rm -f "$1/$o"; done
  return 0
}

echo "== S1: full set -- all eight preserved, flavours distinct =="
R=$T/t1; mktree "$R"
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs preserve_boot_inputs gen1 ) > "$R/out" 2>&1
ck "S1 exit" 0 $?
ck "S1 kept 8" 1 "$(/usr/bin/grep -c 'preserved 8 boot input' "$R/out")"
ck "S1 no ABSENT line" 0 "$(/usr/bin/grep -c 'ABSENT' "$R/out")"
ck "S1 files on disk" 8 "$(ls -1A "$R/inputs/gen1" | /usr/bin/grep -vc HASHES.txt)"
ck "S1 hash lines" 8 "$(wc -l < "$R/inputs/gen1/HASHES.txt" | tr -d ' ')"
# THE DISCRIMINATING CHECK: a flat copy would have one flavour overwrite the other.
ck "S1 default elf content"  DEFAULT-ELF "$(cat "$R/inputs/gen1/thylacine.elf")"
ck "S1 ubsan elf content"    UBSAN-ELF   "$(cat "$R/inputs/gen1/thylacine-undefined.elf")"
ck "S1 default bin content"  DEFAULT-BIN "$(cat "$R/inputs/gen1/thylacine.bin")"
ck "S1 ubsan bin content"    UBSAN-BIN   "$(cat "$R/inputs/gen1/thylacine-undefined.bin")"

echo "== S2: two inputs absent -- recorded, never substituted =="
R=$T/t2; mktree "$R" build/kernel-undefined/thylacine.bin build/ramfs.cpio
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs preserve_boot_inputs gen1 ) > "$R/out" 2>&1
ck "S2 exit" 0 $?
ck "S2 kept 6" 1 "$(/usr/bin/grep -c 'preserved 6 boot input' "$R/out")"
ck "S2 names ubsan bin" 1 "$(/usr/bin/grep -c 'kernel-undefined/thylacine.bin' "$R/out")"
ck "S2 names ramfs" 1 "$(/usr/bin/grep -c 'build/ramfs.cpio' "$R/out")"
ck "S2 no ramfs substituted" 0 "$(ls -1A "$R/inputs/gen1" | /usr/bin/grep -c '^ramfs.cpio$')"

echo "== S3: found but uncopyable -- must REFUSE =="
R=$T/t3; mktree "$R"; chmod 000 "$R/build/ramfs.cpio"
( cd "$R" && . "$FN" && PRESERVE_DIR=inputs preserve_boot_inputs gen1 ) > "$R/out" 2>&1
rc=$?; chmod 644 "$R/build/ramfs.cpio"
ck "S3 refuses nonzero" 1 "$([ $rc -ne 0 ] && echo 1 || echo 0)"
ck "S3 says REFUSING" 1 "$(/usr/bin/grep -c 'REFUSING: found' "$R/out")"

echo "== S4: pruning keeps KEEP_INPUT_GENS=2 =="
R=$T/t4; mktree "$R"
for g in gen1 gen2 gen3; do
  ( cd "$R" && . "$FN" && PRESERVE_DIR=inputs KEEP_INPUT_GENS=2 \
      preserve_boot_inputs $g ) > "$R/out-$g" 2>&1
  sleep 1
done
ck "S4 two generations remain" 2 "$(ls -1d "$R"/inputs/*/ 2>/dev/null | wc -l | tr -d ' ')"
ck "S4 oldest gone" 0 "$([ -d "$R/inputs/gen1" ] && echo 1 || echo 0)"
ck "S4 newest kept" 1 "$([ -d "$R/inputs/gen3" ] && echo 1 || echo 0)"
ck "S4 announced prune" 1 "$(/usr/bin/grep -c 'pruned old generation' "$R/out-gen3")"

echo "== S5: positive control -- the harness can SEE a flat-copy collision =="
R=$T/t5; mktree "$R"
( cd "$R" && mkdir -p inputs/flat && for f in build/kernel/thylacine.elf \
    build/kernel-undefined/thylacine.elf; do cp "$f" inputs/flat/; done )
ck "S5 flat copy collides to 1 file" 1 "$(ls -1A "$R/inputs/flat" | wc -l | tr -d ' ')"
ck "S5 collision loses the default" UBSAN-ELF "$(cat "$R/inputs/flat/thylacine.elf")"

echo
echo "RESULT: $pass pass, $fail wrong"
[ "$fail" = 0 ] || exit 1
