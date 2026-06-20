#!/usr/bin/env bash
# tools/np3-bench.sh -- NET-PERF NP-3: the NIC-path (M6) bench + host baselines.
#
# Measures the guest's NIC path (M6: rtt / floor / connect / throughput) against
# a HOST server reached through a slirp `guestfwd`, AND the apples-to-apples HOST
# baselines (a socket ping-pong / bulk / connect; openssl speed + s_time). The
# guest M6 run is the in-guest `netperf nic` boot probe; this script supplies the
# host server (so the probe produces numbers instead of a fast SKIP) + the
# guestfwd (THYLACINE_GUESTFWD).
#
# Re-runnable + pure measurement; nothing in the stack changes. The guest M6
# numbers also serve as #258 (the deterministic outbound-TCP-over-NIC proof: the
# guest reaches a HOST server over the virtio-net NIC + slirp, deterministically).
#
# Usage:
#   tools/np3-bench.sh                 # build + host baselines + M6 under TCG and HVF
#   NP3_ACCELS="tcg" tools/np3-bench.sh        # one accel
#   NP3_NO_BUILD=1 tools/np3-bench.sh          # measure the current build (no rebuild)
#   NP3_SKIP_BASELINES=1 tools/np3-bench.sh    # guest M6 only
#
# Env: NP3_HOSTPORT (default 28099; +1/+2 = floor/sink), NP3_FLOOR_MS (default 5;
# MUST match netperf M6_FLOOR_DELAY_MS), NP3_BULK_MIB (host bulk size, default 64).

set -uo pipefail   # NOT -e: the HVF test.sh can exit non-zero on the #34
                   # virtio-input QMP flake; we ground-truth the guest banner
                   # instead of trusting the exit code.

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

HOSTPORT="${NP3_HOSTPORT:-28099}"
FLOOR_MS="${NP3_FLOOR_MS:-5}"
BULK_MIB="${NP3_BULK_MIB:-64}"
ACCELS="${NP3_ACCELS:-tcg hvf}"
LOG="$REPO_ROOT/build/test-boot.log"
CERT="$REPO_ROOT/usr/tlsperf/testdata/loopback-cert.pem"
KEY="$REPO_ROOT/usr/tlsperf/testdata/loopback-key.pem"

SRV_PY="$(mktemp -t np3srv.XXXXXX).py"
BASE_PY="$(mktemp -t np3base.XXXXXX).py"
SRV_PID=""
SSRV_PID=""
cleanup() {
    [[ -n "$SRV_PID" ]] && kill "$SRV_PID" 2>/dev/null
    [[ -n "$SSRV_PID" ]] && kill "$SSRV_PID" 2>/dev/null
    rm -f "$SRV_PY" "$BASE_PY"
}
trap cleanup EXIT

# ============================================================================
# The 3-port host server (port = behavior; no in-band mode byte -- a same-port
# reconnect coalesces onto one slirp guestfwd connection, so one port per metric).
#   HOSTPORT   = echo            (M6 rtt + connect)
#   HOSTPORT+1 = echo, +FLOOR_MS delay  (M6 floor: forces the reply past netd's park)
#   HOSTPORT+2 = sink            (M6 throughput)
# ============================================================================
cat > "$SRV_PY" <<PY
import socket, threading, time
DELAY = ${FLOOR_MS} / 1000.0
def echo(c, delay):
    while True:
        d = c.recv(4096)
        if not d: break
        if delay: time.sleep(delay)
        c.sendall(d)
    c.close()
def sink(c):
    while True:
        d = c.recv(65536)
        if not d: break
    c.close()
def serve(port, fn):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', port)); s.listen(64)
    while True:
        c, _ = s.accept()
        threading.Thread(target=fn, args=(c,), daemon=True).start()
threading.Thread(target=serve, args=(${HOSTPORT},   lambda c: echo(c, 0)),     daemon=True).start()
threading.Thread(target=serve, args=(${HOSTPORT}+1, lambda c: echo(c, DELAY)), daemon=True).start()
threading.Thread(target=serve, args=(${HOSTPORT}+2, sink),                     daemon=True).start()
while True: time.sleep(3600)
PY

# ============================================================================
# Host baselines (the apples-to-apples reference; B1/B2/B3 mirror M1/M2/M3).
# ============================================================================
cat > "$BASE_PY" <<PY
import socket, threading, time
H = '127.0.0.1'
def serve_echo(port, ready):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((H, port)); s.listen(64); ready.set()
    while True:
        c, _ = s.accept()
        threading.Thread(target=lambda c=c: _echo(c), daemon=True).start()
def _echo(c):
    while True:
        d = c.recv(65536)
        if not d: break
        c.sendall(d)
    c.close()
def serve_sink(port, ready, got):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((H, port)); s.listen(64); ready.set()
    c, _ = s.accept()
    n = 0
    while True:
        d = c.recv(1 << 20)
        if not d: break
        n += len(d)
    got[0] = n; c.close()

def stats(xs):
    xs = sorted(xs); n = len(xs)
    return sum(xs)/n, xs[0], xs[-1]

# B1: 1-byte ping-pong RTT (us), 100 iters.
r = threading.Event(); threading.Thread(target=serve_echo, args=(38101, r), daemon=True).start(); r.wait()
c = socket.socket(); c.connect((H, 38101)); c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
rt = []
for _ in range(100):
    t = time.perf_counter(); c.sendall(b'\x5a'); c.recv(1); rt.append((time.perf_counter()-t)*1e6)
c.close()
m, lo, hi = stats(rt)
print(f"B1 host rtt (1B ping-pong, lo): mean {m:.1f} us / min {lo:.1f} / max {hi:.1f}")

# B2: bulk send throughput (MiB/s).
MIB = ${BULK_MIB}; total = MIB << 20
r = threading.Event(); got = [0]
threading.Thread(target=serve_sink, args=(38102, r, got), daemon=True).start(); r.wait()
c = socket.socket(); c.connect((H, 38102))
buf = b'\xa5' * (256 << 10); sent = 0; t = time.perf_counter()
while sent < total:
    k = c.send(buf if total - sent >= len(buf) else buf[:total-sent]); sent += k
c.shutdown(socket.SHUT_WR)
while got[0] < total: time.sleep(0.001)
dt = time.perf_counter() - t; c.close()
print(f"B2 host bulk (lo): {MIB} MiB in {dt*1e3:.1f} ms; {total/dt/(1<<20):.1f} MiB/s")

# B3: connect latency (us), 100 dials.
r = threading.Event(); threading.Thread(target=serve_echo, args=(38103, r), daemon=True).start(); r.wait()
ct = []
for _ in range(100):
    t = time.perf_counter(); s = socket.socket(); s.connect((H, 38103)); ct.append((time.perf_counter()-t)*1e6); s.close()
m, lo, hi = stats(ct)
print(f"B3 host connect (lo): mean {m:.1f} us / min {lo:.1f} / max {hi:.1f}")
PY

echo "============================================================"
echo "NET-PERF NP-3 -- NIC path (M6) + host baselines"
echo "  hostport=$HOSTPORT (echo/+1 delayed ${FLOOR_MS}ms/+2 sink)  accels='$ACCELS'"
echo "============================================================"

# --- build (unless skipped) ---
if [[ "${NP3_NO_BUILD:-0}" != "1" ]]; then
    echo "==> building userspace + ramfs ..."
    tools/build.sh userspace >/dev/null 2>&1 && tools/build.sh ramfs >/dev/null 2>&1 \
        && echo "    build OK" || { echo "    BUILD FAILED" >&2; exit 1; }
fi

# --- host baselines ---
if [[ "${NP3_SKIP_BASELINES:-0}" != "1" ]]; then
    echo
    echo "=== HOST BASELINES (the apples-to-apples reference) ==="
    python3 "$BASE_PY" || echo "  (host socket baselines failed)"
    if command -v openssl >/dev/null 2>&1; then
        echo "--- B4 host crypto (openssl speed, ARMv8 AES/SHA) ---"
        openssl speed -elapsed -seconds 1 -evp aes-128-gcm 2>/dev/null | tail -1
        openssl speed -elapsed -seconds 1 -evp sha256 2>/dev/null | tail -1
        openssl speed -elapsed -seconds 1 ecdsap256 2>/dev/null | grep -iE "256 bit|nistp256" | tail -1
        openssl speed -elapsed -seconds 1 ecdhx25519 2>/dev/null | grep -iE "x25519|253 bit" | tail -1
        echo "--- B5 host TLS 1.3 handshake (openssl s_time vs a local s_server) ---"
        if [[ -f "$CERT" && -f "$KEY" ]]; then
            openssl s_server -quiet -cert "$CERT" -key "$KEY" -accept 38443 -www \
                -tls1_3 >/dev/null 2>&1 & SSRV_PID=$!
            sleep 0.4
            openssl s_time -connect 127.0.0.1:38443 -new -time 2 2>/dev/null \
                | grep -iE "connections|connect" | head -2 || echo "  (s_time failed)"
            kill "$SSRV_PID" 2>/dev/null; SSRV_PID=""
        else
            echo "  (no baked test cert at $CERT -- skipping B5)"
        fi
    else
        echo "  (openssl not found -- skipping B4/B5)"
    fi
fi

# --- start the host M6 server ---
python3 "$SRV_PY" & SRV_PID=$!
sleep 0.4

# --- guest M6 under each accel ---
for accel in $ACCELS; do
    echo
    echo "=== GUEST M6 (accel=$accel) ==="
    THYLACINE_ACCEL="$accel" THYLACINE_GUESTFWD=1 BANNER_GRACE=8 \
        tools/test.sh >/tmp/np3-${accel}.out 2>&1
    rc=$?
    if grep -q "Thylacine boot OK" "$LOG"; then
        banner="boot OK"
    else
        banner="NO BANNER (boot incomplete!)"
    fi
    ext=$(grep -c "^EXTINCTION:" "$LOG" 2>/dev/null); ext="${ext:-0}"  # grep -c prints 0; exit 1 ignored
    grep -E "netperf M6|netperf: NP-3" "$LOG" | sed 's/^/    /'
    echo "    [accel=$accel: test.sh exit=$rc, guest=$banner, EXTINCTION=$ext]"
    if [[ "$banner" != "boot OK" || "$ext" != "0" ]]; then
        echo "    !! guest end-state unhealthy -- inspect $LOG / /tmp/np3-${accel}.out" >&2
    fi
done

echo
echo "============================================================"
echo "NP-3 done. Compare M6 (NIC) to M1/M2/M3 (lo) in the same log,"
echo "and to the host baselines above. See docs/NET-PERF.md section 10."
echo "============================================================"
