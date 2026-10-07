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

# ---- the three legs ----
for leg in cross owner-lock both-locked self-drain; do
  "$OUT/mag-double" "$leg" > "$OUT/$leg.out" 2>&1 || true
  races=$(/usr/bin/grep -c 'WARNING: ThreadSanitizer: data race' "$OUT/$leg.out" || true)
  printf '%-13s races=%-3s %s\n' "$leg" "${races:-0}" "$(/usr/bin/grep -h '^leg=' "$OUT/$leg.out" || echo '(no summary line)')"
done
echo "-- evidence: $OUT"
