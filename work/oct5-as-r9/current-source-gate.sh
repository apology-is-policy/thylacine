#!/bin/sh
# Astra's current-source full Mac gate. No kernel mutations or blind retries.
# Call after acquiring the Mac as Astra; this runner releases it on every exit.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
YIP="$ROOT/work/oct5-as-r9/astra-yip.sh"
OUT="$ROOT/work/oct5-as-r9/current-gate-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir "$OUT"
"$YIP" resources > "$OUT/lease.txt"
grep -qE '^mac +HELD by you' "$OUT/lease.txt" || exit 3
finish() {
    rc=$?
    trap - EXIT
    git diff --exit-code HEAD > "$OUT/end-source.diff" || rc=3
    git rev-parse HEAD > "$OUT/end-source.txt"
    cmp "$OUT/source.txt" "$OUT/end-source.txt" || rc=3
    if [ -f "$OUT/inputs/HASHES.txt" ]; then
        (cd "$OUT/inputs" && shasum -a 256 -c HASHES.txt) > "$OUT/input-verification.txt" 2>&1 || rc=3
        for pair in \
            kernel/thylacine.elf:thylacine.elf \
            kernel/thylacine.bin:thylacine.bin \
            kernel-undefined/thylacine.elf:thylacine-undefined.elf \
            kernel-undefined/thylacine.bin:thylacine-undefined.bin \
            ramfs.cpio:ramfs.cpio \
            fixtures/pool.img.baked-snapshot:pool.img.baked-snapshot \
            fixtures/system.key:system.key \
            fixtures/system.key.baked-snapshot:system.key.baked-snapshot; do
            cmp "build/${pair%:*}" "$OUT/inputs/${pair#*:}" || rc=3
        done
    fi
    printf '%s\n' "$rc" > "$OUT/runner.rc"
    "$YIP" release mac > "$OUT/release.txt" 2>&1 || rc=3
    printf '%s\n' "$rc" > "$OUT/runner.rc"
    echo "current-source gate finished: rc=$rc evidence=$OUT"
    exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
git diff --quiet HEAD || { echo 'REFUSING: tracked tree dirty'; exit 3; }
git rev-parse HEAD > "$OUT/source.txt"
"$YIP" api lease_update "{\"resource\":\"mac\",\"phase\":\"current-source full Mac gate\",\"pids\":[$$]}" > "$OUT/lease-update.txt"

export STRATUM_SRC=/Users/northkillpd/projects/stratum-astra/v2
pin=61dde3727921e70e2c72fbd3c9e2044a192f4a54
[ "$(git -C "$STRATUM_SRC" rev-parse HEAD)" = "$pin" ] || exit 4
[ -z "$(git -C "$STRATUM_SRC" status --porcelain)" ] || exit 4
printf '%s %s\n' "$pin" "$STRATUM_SRC" > "$OUT/stratum.txt"
export AMBUSHFORK=/Users/northkillpd/projects/ambush-pin-073faaa
grep -q '^const launchHeld = true$' "$AMBUSHFORK/pkg/proc/native/held_on_thylacine.go" || exit 4
git -C "$AMBUSHFORK" rev-parse HEAD > "$OUT/ambush.txt"

# Force the complete standard matrix and retain every serial/harness log.
unset SMP_GATE_CONFIGS
export SMP_GATE_N=10 SMP_KEEP_LOGS=1 SMP_GATE_MIN_FREE_MIB=8192
export SMP_GATE_CAPTURE_DIR="$OUT/inputs"
date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/start.txt"
echo "current-source full gate: $OUT"
set +e
tools/ci-smp-gate.sh > "$OUT/gate.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > "$OUT/gate.rc"
[ "$(git -C "$STRATUM_SRC" rev-parse HEAD)" = "$pin" ] || exit 4
[ -z "$(git -C "$STRATUM_SRC" status --porcelain)" ] || exit 4
exit "$rc"
