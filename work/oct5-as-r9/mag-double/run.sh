#!/bin/sh
# Extract the REAL magazine bodies, build the double under ThreadSanitizer, and
# run its three legs. Off-lease by construction: one single-file clang compile
# and three short two-thread runs, no QEMU, no CMake, no parallel build.
set -e
ROOT=${ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}
cd "$ROOT"
D=work/oct5-as-r9/mag-double
SRC=mm/magazines.c
OUT=$D/out-$(date -u '+%Y%m%dT%H%M%SZ')
mkdir -p "$OUT"
CC=${CC:-/opt/homebrew/opt/llvm/bin/clang}
INC=$OUT/extracted.inc

# ---- extraction, with a denominator control on every function ----
# sed ranges from the signature to the first closing brace at column 0. Each
# extracted body is then asserted to be a VERBATIM substring of the source, so
# the double cannot launder its own premise by paraphrase.
: > "$INC"
for fn in 'static int order_to_mag_idx(unsigned order) {' \
          'static unsigned mag_idx_to_order(int idx) {' \
          'static inline int my_cpu(void) {' \
          'static void mag_refill(struct magazine \*m, unsigned order) {' \
          'static void mag_drain(struct magazine \*m, unsigned order) {' \
          'struct page \*mag_alloc(unsigned order) {' \
          'bool mag_free(struct page \*p, unsigned order) {' \
          'void magazines_drain_all(void) {' ; do
  sed -n "/^$fn\$/,/^}\$/p" "$SRC" >> "$INC"
  printf '\n' >> "$INC"
done
nfn=$(/usr/bin/grep -c '^}' "$INC" || true)
if [ "${nfn:-0}" -ne 8 ]; then
  echo "REFUSING: extracted $nfn function(s), expected 8 -- a signature moved in $SRC."
  echo "          The double would otherwise test whatever happened to come out."
  exit 3
fi
python3 - "$SRC" "$INC" <<'PY' || exit 3
import sys
src = open(sys.argv[1]).read()
inc = open(sys.argv[2]).read()
bad = 0
for body in [b for b in inc.split('\n}\n') if b.strip()]:
    if (body + '\n}') not in src:
        print("NOT VERBATIM in the source:", body.strip().split('\n')[0]); bad += 1
print("-- verbatim check: %d extracted body(ies), %d not found in %s" %
      (len([b for b in inc.split('\n}\n') if b.strip()]), bad, sys.argv[1]))
sys.exit(1 if bad else 0)
PY

# ---- build under TSan ----
"$CC" -fsanitize=thread -g -O1 -std=c11 -isysroot "$(xcrun --show-sdk-path)" \
      -I "$OUT" "$D/mag-double.c" -o "$OUT/mag-double" 2> "$OUT/build.log" || {
  echo "BUILD FAILED -- see $OUT/build.log"; tail -15 "$OUT/build.log"; exit 4; }
echo "-- built $OUT/mag-double under ThreadSanitizer"
# THE INSTRUMENT MUST BE LIVE: a TSan binary that reports nothing and an
# uninstrumented binary are indistinguishable from their output alone.
if ! nm "$OUT/mag-double" 2>/dev/null | /usr/bin/grep -q '__tsan_'; then
  echo "REFUSING: no __tsan_ symbols -- the binary is NOT instrumented, so a"
  echo "          clean run would prove nothing at all."; exit 4
fi
echo "-- instrument verified live (__tsan_ symbols present)"

# ---- the legs ----
# EVERY LEG'S STATUS IS RETAINED AND JUDGED (astra, 0161 t59). The first version
# wrote `|| true` after each run and only warned when the summary line was
# missing, so a leg that CRASHED early would contribute zero races and zero
# aliases and read as clean -- fail-open evidence, the same class as an
# unverified pattern check. A leg now counts only if it ran to completion AND
# terminated acceptably: exit 0, or exit 66 which is TSan's configured
# exitcode after it has reported (the report IS the expected outcome on a
# faithful leg, so a nonzero status there must not be read as a broken run).
TSAN_EXITCODE=66
# abort_on_error=0 is what makes the rule crisp: on Darwin TSan ABORTS after
# reporting (SIGABRT, status 134), which is indistinguishable from a genuine
# crash. With it off, a reporting leg exits with exitcode deterministically,
# so "reported a race" and "died" are different statuses.
export TSAN_OPTIONS="abort_on_error=0 exitcode=$TSAN_EXITCODE"
legs_bad=0
for leg in cross owner-lock both-locked self-drain; do
  set +e
  "$OUT/mag-double" "$leg" > "$OUT/$leg.out" 2>&1
  rc=$?
  set -e
  echo "$rc" > "$OUT/$leg.rc"
  races=$(/usr/bin/grep -c 'WARNING: ThreadSanitizer: data race' "$OUT/$leg.out" || true)
  summary=$(/usr/bin/grep -h '^leg=' "$OUT/$leg.out" || true)
  verdict=ok
  if [ -z "$summary" ]; then
    verdict="REJECTED: no completed summary line -- the leg did not finish, so its"
    verdict="$verdict zero counts are not evidence"
    legs_bad=$((legs_bad + 1))
  elif [ "$rc" -ne 0 ] && [ "$rc" -ne "$TSAN_EXITCODE" ]; then
    verdict="REJECTED: exit $rc is neither 0 nor TSan's reporting exit $TSAN_EXITCODE"
    legs_bad=$((legs_bad + 1))
  fi
  printf '%-13s rc=%-4s races=%-3s %s\n' "$leg" "$rc" "${races:-0}" "${summary:-(none)}"
  [ "$verdict" = ok ] || printf '              %s\n' "$verdict"
done
echo "-- evidence: $OUT"
if [ "$legs_bad" -ne 0 ]; then
  echo "REFUSING TO REPORT: $legs_bad leg(s) did not produce usable evidence."
  exit 5
fi
