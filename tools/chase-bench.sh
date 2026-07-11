#!/usr/bin/env bash
# CHASE bench driver (docs/CHASE.md sections 3 + 5) -- the ONLY sanctioned way
# to produce bar-relevant numbers. Wires the measurement law in:
#   1. quiet-host sentinel BEFORE and AFTER every run set (a dirty sentinel
#      voids the run);
#   2. matched cache states (device: the joey go4c probe's gofmt-warm [S1] +
#      gofmt-s3cold [S3] lines; host: two consecutive warm builds [S1] + a
#      fresh-empty-GOCACHE build [S3]);
#   3. pool snapshot-restore per device boot; host measured same-session;
#   4. no background burners -- and any this script starts would be a bug: it
#      starts none, the sentinel enforces none linger;
#   5. instrument lines (DIAG23/STMD26) extracted alongside every wall-clock.
#
# Usage:
#   tools/chase-bench.sh sentinel            # standalone quiet-host check
#   tools/chase-bench.sh device [N]          # N pool-restored boots @ smp=8
#   tools/chase-bench.sh host [N]            # host W1 (gofmt) S1+S3, N reps
#   tools/chase-bench.sh host-w2 [N]         # host W2 (cmd/compile) cold+warm
#
# Device runs need fixtures built with the CHASE joey probe (gofmt-s3cold);
# W2 device runs additionally need a THYLACINE_CHASE_W2=1 build (the
# /chase-w2 pool marker). Logs land under build/chase/<UTC-stamp>/.

set -u
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GOFORK="${CHASE_GOFORK:-$HOME/projects/go-thylacine}"
CPUS="${CHASE_CPUS:-8}"          # docs/CHASE.md section 3: bar runs are smp=8
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUTDIR="$REPO_ROOT/build/chase/$STAMP"

now_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time()*1000'; }

# --- The quiet-host sentinel (measurement law #1) ------------------------
# Records load average + any process burning >50% CPU PERSISTENTLY: a stray
# must appear in BOTH samples (SENTINEL_GAP s apart, matched by pid) to
# void a run. The orphaned-burner class this defends against is persistent
# by nature; single-sample spikes (a finishing pipeline, the agent's own
# turn processing) are transients that decay before the measured window.
# Nothing is excluded by name -- QEMU included: during a sentinel window
# nothing should be persistently hot.
sentinel() {
    local tag="$1" dirty=0
    local load s1 s2 persist
    load="$(sysctl -n vm.loadavg 2>/dev/null || uptime)"
    # tr flattens to one line: macOS awk REJECTS newlines in -v strings
    # (and the error would fail the check OPEN -- verified empirically).
    s1="$(ps -Ao %cpu=,pid=,comm= | awk '$1 > 50.0 {print $2}' | tr '\n' ' ' || true)"
    if [[ -n "$s1" ]]; then
        sleep "${SENTINEL_GAP:-5}"
        s2="$(ps -Ao %cpu=,pid=,comm= | awk '$1 > 50.0' || true)"
        persist="$(echo "$s2" | awk -v keep="$s1" 'BEGIN{split(keep,k," "); for(i in k) want[k[i]]=1} want[$2]' || true)"
    else
        persist=""
    fi
    echo "== sentinel[$tag] load: $load"
    if [[ -n "$persist" ]]; then
        dirty=1
        echo "== sentinel[$tag] DIRTY -- persistent >50% CPU strays (2 samples, ${SENTINEL_GAP:-5}s apart):"
        echo "$persist"
    else
        echo "== sentinel[$tag] clean (no persistent >50% CPU strays)"
    fi
    return $dirty
}

# --- Device: N pool-restored boots at smp=8 ------------------------------
POOL_IMG="$REPO_ROOT/build/fixtures/pool.img"
POOL_SNAP="$POOL_IMG.baked-snapshot"
KEY_IMG="$REPO_ROOT/build/fixtures/system.key"
KEY_SNAP="$KEY_IMG.baked-snapshot"
LOG_FILE="${THYLACINE_TEST_LOG:-$REPO_ROOT/build/test-boot.log}"

pool_restore() {
    [[ -f "$POOL_SNAP" && -f "$POOL_IMG" ]] || {
        echo "ERROR: pool fixtures missing ($POOL_SNAP)"; return 1; }
    cmp -s "$KEY_IMG" "$KEY_SNAP" 2>/dev/null || {
        echo "ERROR: system.key does not match its snapshot (stale twins -- rebuild; see the fixture trap in docs/CHASE.md)"; return 1; }
    cp -c "$POOL_SNAP" "$POOL_IMG" 2>/dev/null || cp "$POOL_SNAP" "$POOL_IMG"
}

extract_device() {
    # Serial logs carry CR/control bytes -> grep -a. The bar lines:
    #   go4c TIMING gofmt-warm=NNNms      (S1)
    #   go4c TIMING gofmt-s3cold=NNNms    (S3)
    # plus every instrument line for the ledger.
    grep -aE "go4c TIMING|DIAG23|STMD26|CHASE|GOFMT374|w2compile|boot OK|EXTINCTION" "$1" || true
}

run_device() {
    local n="${1:-3}" i rc pass=0
    mkdir -p "$OUTDIR"
    sentinel "device-pre" | tee "$OUTDIR/sentinel-pre.txt"
    grep -q DIRTY "$OUTDIR/sentinel-pre.txt" && {
        echo "VOID: dirty pre-sentinel -- kill the strays, re-run"; return 1; }
    for i in $(seq 1 "$n"); do
        echo "== device boot $i/$n (smp=$CPUS, pool-restored)"
        pool_restore || return 1
        BOOT_TIMEOUT="${BOOT_TIMEOUT:-300}" THYLACINE_TEST_CPUS="$CPUS" \
            THYLACINE_INPUT_INJECT=0 \
            "$REPO_ROOT/tools/test.sh" >/dev/null 2>&1
        rc=$?
        cp "$LOG_FILE" "$OUTDIR/device-$i.log" 2>/dev/null || true
        extract_device "$OUTDIR/device-$i.log" > "$OUTDIR/device-$i.extract.txt"
        if [[ $rc -eq 0 ]]; then
            pass=$((pass+1))
            echo "-- boot $i PASS; bar lines:"
            grep -aE "gofmt-warm=|gofmt-s3cold=|w2compile" "$OUTDIR/device-$i.extract.txt" || echo "   (no bar lines -- probe not in this build?)"
        else
            echo "-- boot $i FAIL (rc=$rc); see $OUTDIR/device-$i.log"
        fi
    done
    sentinel "device-post" | tee "$OUTDIR/sentinel-post.txt"
    grep -q DIRTY "$OUTDIR/sentinel-post.txt" && {
        echo "VOID: dirty post-sentinel -- numbers above are suspect"; return 1; }
    echo "== device: $pass/$n PASS; logs in $OUTDIR"
    [[ $pass -eq $n ]]
}

# --- Host: W1 (gofmt) S1 + S3 --------------------------------------------
host_build() {  # $1 = output, env GOCACHE optional
    ( cd "$GOFORK" && ./bin/go build -o "$1" cmd/gofmt )
}

run_host() {
    local n="${1:-3}" i t0 t1 s1a s1b s3 tmpc
    mkdir -p "$OUTDIR"
    [[ -x "$GOFORK/bin/go" ]] || { echo "ERROR: $GOFORK/bin/go missing"; return 1; }
    sentinel "host-pre" | tee -a "$OUTDIR/host.txt"
    grep -q DIRTY "$OUTDIR/host.txt" && { echo "VOID: dirty pre-sentinel"; return 1; }
    for i in $(seq 1 "$n"); do
        # S1: two consecutive warm builds; the SECOND is the S1 number (the
        # device analog is gofmt-warm, also the second consecutive build).
        t0=$(now_ms); host_build /tmp/chase-gofmt-a; t1=$(now_ms); s1a=$((t1-t0))
        t0=$(now_ms); host_build /tmp/chase-gofmt-b; t1=$(now_ms); s1b=$((t1-t0))
        # S3: fresh empty GOCACHE on the real FS (the go clean -cache state
        # without the deletion storm; device analog /gocold). rm AFTER timing.
        tmpc="$(mktemp -d /tmp/chase-gocold.XXXXXX)"
        t0=$(now_ms); GOCACHE="$tmpc" host_build /tmp/chase-gofmt-c; t1=$(now_ms); s3=$((t1-t0))
        rm -rf "$tmpc"
        echo "host rep $i: S1a=${s1a}ms S1=${s1b}ms S3=${s3}ms" | tee -a "$OUTDIR/host.txt"
    done
    sentinel "host-post" | tee -a "$OUTDIR/host.txt"
    grep -q "host-post.*DIRTY" "$OUTDIR/host.txt" && { echo "VOID: dirty post-sentinel"; return 1; }
    echo "== host W1 done; log: $OUTDIR/host.txt"
}

run_host_w2() {
    local n="${1:-2}" i t0 t1 cold warm tmpc
    mkdir -p "$OUTDIR"
    sentinel "hostw2-pre" | tee -a "$OUTDIR/host-w2.txt"
    grep -q DIRTY "$OUTDIR/host-w2.txt" && { echo "VOID: dirty pre-sentinel"; return 1; }
    for i in $(seq 1 "$n"); do
        tmpc="$(mktemp -d /tmp/chase-gocold2.XXXXXX)"
        t0=$(now_ms); ( cd "$GOFORK" && GOCACHE="$tmpc" ./bin/go build -o /tmp/chase-compile-a cmd/compile ); t1=$(now_ms); cold=$((t1-t0))
        t0=$(now_ms); ( cd "$GOFORK" && GOCACHE="$tmpc" ./bin/go build -o /tmp/chase-compile-b cmd/compile ); t1=$(now_ms); warm=$((t1-t0))
        rm -rf "$tmpc"
        echo "host W2 rep $i: cold=${cold}ms warm=${warm}ms" | tee -a "$OUTDIR/host-w2.txt"
    done
    sentinel "hostw2-post" | tee -a "$OUTDIR/host-w2.txt"
    echo "== host W2 done; log: $OUTDIR/host-w2.txt"
}

case "${1:-}" in
    sentinel) sentinel "standalone" ;;
    device)   run_device "${2:-3}" ;;
    host)     run_host "${2:-3}" ;;
    host-w2)  run_host_w2 "${2:-2}" ;;
    *) echo "usage: $0 {sentinel|device [N]|host [N]|host-w2 [N]}"; exit 2 ;;
esac
