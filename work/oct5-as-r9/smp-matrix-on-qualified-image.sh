#!/bin/sh
# The ci-smp-gate MATRIX, run against the already-qualified image.
#
# WHY NOT tools/ci-smp-gate.sh ITSELF. That script's first act is an
# unconditional `tools/build.sh kernel` (ci-smp-gate.sh:140), which in THIS tree
# refuses: build/ carries a stratumd CMake cache generated from a peer's source
# tree (D7's residue, inherited through an APFS clone of a peer's build/), so
# every full bake dies at the stratumd step. That is enqueued as its own bug and
# is NOT worked around here.
#
# WHAT IS AND IS NOT CLAIMED. This runs the gate's matrix STAGE -- the same five
# rows, the same N, the same per-row boot timeouts, the same classifier and the
# same pass condition -- on the image the suite and both red legs already
# qualified. It is NOT `ci-smp-gate.sh` and must never be reported as that
# script having passed. What it omits is the gate's own rebuild of userspace,
# which is exactly the part that cannot run here.
#
# It is arguably the better experiment for a KERNEL change: the image under test
# is bit-for-bit the one the green suite and the two credited red legs ran on,
# so the kernel is the single variable across all three bodies of evidence.
#
# THE ROWS ARE DERIVED, NEVER RETYPED. They are read out of ci-smp-gate.sh's own
# DEFAULT_MATRIX, so this cannot drift from the gate it stands in for -- and it
# refuses rather than running a matrix it failed to parse.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 3

GATE=tools/ci-smp-gate.sh
N=${SMP_GATE_N:-$(sed -n 's/^N="\${SMP_GATE_N:-\([0-9]*\)}"/\1/p' "$GATE" | head -1)}
[ -n "$N" ] || { echo "REFUSING: cannot read the gate's default N out of $GATE"; exit 3; }

ROWS=$(sed -n '/^DEFAULT_MATRIX=(/,/^)/p' "$GATE" | sed -n 's/^[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p')
nrows=$(printf '%s\n' "$ROWS" | grep -c '[^[:space:]]')
[ "$nrows" -ge 3 ] \
  || { echo "REFUSING: parsed only $nrows matrix rows from $GATE -- the extractor is broken, not the gate"; exit 3; }

echo "== matrix stage on the qualified image, N=$N, $nrows rows derived from $GATE"
echo "== default kernel  $(shasum -a 256 build/kernel/thylacine.bin | cut -c1-16)"
echo "== ubsan kernel    $(shasum -a 256 build/kernel-undefined/thylacine.bin 2>/dev/null | cut -c1-16)"
echo "== ramfs           $(shasum -a 256 build/ramfs.cpio | cut -c1-16)"
echo "== pool            $(shasum -a 256 build/fixtures/pool.img | cut -c1-16)"
echo "== HEAD            $(git rev-parse HEAD)"

# Keep every boot's serial log, so the per-boot D7 witnesses can be counted
# rather than inferred from a row being green (astra's close condition, 0161 n17).
export SMP_KEEP_LOGS=1

RESULTS=work/oct5-as-r9/private-owner-logs/smp-matrix/$(date -u +%Y%m%dT%H%M%SZ)
[ -e "$RESULTS" ] && { echo "REFUSING: $RESULTS exists"; exit 3; }
mkdir -p "$RESULTS" || { echo "REFUSING: cannot create $RESULTS"; exit 3; }

# NO PIPELINE INTO THE LOOP. A `printf | while read` loop runs in a SUBSHELL,
# so every status it computes is lost and the script exits on the pipeline's
# status instead -- which is exactly the defect that let an earlier version of
# the red-legs runner print ALL ROWS AS PREDICTED while a row had failed. The
# rows go through a file and the verdict is counted from RECORDED results.
ROWFILE=$RESULTS/rows.txt
printf '%s\n' "$ROWS" > "$ROWFILE"
VERDICTS=$RESULTS/row-results.txt
: > "$VERDICTS"

while IFS= read -r row; do
  [ -n "$row" ] || continue
  # shellcheck disable=SC2086
  set -- $row
  label=$1; cpus=$2; san=$3; to=$4
  sanarg=""
  [ "$san" = "-" ] || sanarg=$san
  echo
  echo "-- $label: cpus=$cpus sanitizer=${sanarg:-none} boot_timeout=${to}s N=$N --"
  if BOOT_TIMEOUT="$to" tools/smp-multiboot.sh "$label" "$cpus" "$N" "$sanarg"; then
    printf '%s PASS\n' "$label" >> "$VERDICTS"
    echo "ROW-RESULT $label PASS"
  else
    printf '%s FAIL\n' "$label" >> "$VERDICTS"
    echo "ROW-RESULT $label FAIL"
  fi
done < "$ROWFILE"

echo
echo "================ matrix summary (N=$N) ================"
while IFS= read -r line; do echo "  $line"; done < "$VERDICTS"
echo "======================================================="

recorded=$(grep -c '[^[:space:]]' "$VERDICTS")
failed=$(grep -c ' FAIL$' "$VERDICTS" || true)

# A SHORT RESULT SET IS A FAILURE, not a pass. If a row never recorded a verdict
# -- killed, crashed, skipped -- the matrix did not run, and an absent row must
# never read as a green one.
if [ "$recorded" -ne "$nrows" ]; then
  echo "MATRIX INCOMPLETE: $recorded of $nrows rows recorded a verdict"
  echo "evidence: $RESULTS"
  exit 1
fi
if [ "$failed" -ne 0 ]; then
  echo "MATRIX FAIL: $failed of $nrows rows failed -- see build/multiboot-fails/ and $RESULTS"
  exit 1
fi
echo "MATRIX PASS: $nrows/$nrows rows, 0 corruption, 0 external-kill, 0 unclassified"
echo "evidence: $RESULTS"
echo "NOT a ci-smp-gate.sh run: its userspace rebuild is blocked by the stratumd"
echo "cache bug. This is the gate's matrix stage on the already-qualified image."
exit 0
