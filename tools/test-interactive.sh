#!/usr/bin/env bash
# tools/test-interactive.sh -- the LS-CI interactive E2E regression net.
#
# Boots Thylacine under QEMU and drives a REAL PTY into the `-serial mon:stdio`
# console via `expect`, logging in as a seeded user and asserting the rendered
# output. This is the ONLY way to inject console keystrokes (a piped stdin EOFs
# the chardev), and therefore the only harness that can catch the interactive
# regression class -- LS-1 (UART RX disabled) and LS-2 (external output dropped)
# both shipped silently because CI could not type. See docs/LIFE-SUPPORT.md
# ("LS-CI"). Every later LS chunk lands a scenario here.
#
# OPTIONAL gate: `expect` is an optional dependency. Absent it, this SKIPs
# (exit 0) so a pipeline that wires it stays green on hosts without expect.
#
# Usage:
#   tools/test-interactive.sh                 # run every tools/interactive/*.exp
#   tools/test-interactive.sh ls-ci           # run one scenario by name
#   tools/test-interactive.sh path/to/x.exp   # run one scenario by path
#
# Env:
#   THYLACINE_ACCEL=hvf|tcg     accel (default tcg -- deterministic compat run)
#   LS_CI_BOOT_TIMEOUT=N        seconds to reach the shell (default 180)
#   LS_CI_CMD_TIMEOUT=N         seconds per command's output (default 30)

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SCEN_DIR="$REPO_ROOT/tools/interactive"

# --- optional-dependency degrade ---
if ! command -v expect >/dev/null 2>&1; then
    echo "==> SKIP: 'expect' not found -- LS-CI is an optional interactive gate."
    echo "    Install it to run (macOS ships /usr/bin/expect; Debian: apt-get install expect)."
    exit 0
fi

export THYLACINE_ACCEL="${THYLACINE_ACCEL:-tcg}"
# Stage 6: with the Go GOROOT baked by default, joey's go4c on-device
# compile+link probe rides every boot -- a TCG slow-mode boot + the probe can
# exceed 180 s. Same goroot-staged auto-bump as tools/test.sh; an explicit
# LS_CI_BOOT_TIMEOUT always wins.
if [[ -d "$BUILD_DIR/go/goroot" ]]; then
    export LS_CI_BOOT_TIMEOUT="${LS_CI_BOOT_TIMEOUT:-300}"
else
    export LS_CI_BOOT_TIMEOUT="${LS_CI_BOOT_TIMEOUT:-180}"
fi
export LS_CI_CMD_TIMEOUT="${LS_CI_CMD_TIMEOUT:-30}"
# #102: decline the CL-5 build storm. It runs pre-login on any clade-baked pool
# and costs 266-301 s per boot under TCG -- against the 300 s budget above, so
# a pool minted for the GL/clade gate made EVERY scenario fail by timeout with a
# perfectly healthy guest (its log ending mid-compile). LS-CI does not test the
# storm; tools/test.sh runs it unconditionally and that is where the CL-5
# charter proof lives. Declining here removes the pool mismatch instead of
# detecting it, so one pool serves both gates. Set THYLACINE_NOSTORM=0 to run it.
export THYLACINE_NOSTORM="${THYLACINE_NOSTORM:-1}"

# Reap only THIS repo's qemu -- match THIS tree's build dir in the cmdline
# (-kernel $BUILD_DIR/kernel/thylacine.bin), so a co-resident qemu from a
# SIBLING WORKTREE survives. The old pattern ("qemu-system-aarch64.*thylacine")
# matched every thylacine tree: two sessions gating concurrently (main +
# thylacine-aux, 2026-07-21) shot each other's live VMs -- "qemu GONE, guest
# healthy" mid-scenario failures on both sides (task #59).
reap_qemu() { pkill -9 -f "qemu-system-aarch64.*$BUILD_DIR/" 2>/dev/null || true; }

# #224: the 2026-07-21 fix above narrowed this reaper from cross-tree to
# TREE-WIDE, which closed the cross-tree half of the bug and left the intra-tree
# half live. It still SIGKILLs every VM in this tree, including boots it did not
# start -- so an SMP gate running concurrently here loses a boot to a signal it
# cannot see the source of, presenting as exactly the symptom the comment above
# already names: "qemu GONE, guest healthy".
#
# Narrowing the pattern further is the wrong fix, because concurrency in one
# tree is unsafe for a SECOND, independent reason: this gate and the SMP gate
# both restore the same build/fixtures/pool.img, and pool_restore overwriting an
# image under a live VM manufactures the corruption this gate exists to detect.
# A narrower reaper would leave that collision in place and merely stop
# announcing it. So REFUSE instead of killing: turn a silent mutual-corruption
# race into an operator error with a name.
#
# This check MUST precede `trap reap_qemu EXIT` -- installing the trap first
# would make the refusal itself kill the VM it is refusing to disturb.
foreign_vms="$(pgrep -f "qemu-system-aarch64.*$BUILD_DIR/" 2>/dev/null | tr '\n' ' ')"
if [[ -n "${foreign_vms// /}" ]]; then
    echo "test-interactive: REFUSING to start -- a VM from this tree is already running (pid(s): ${foreign_vms% })." >&2
    echo "  This gate's reaper is tree-wide, so starting would SIGKILL a boot it does not own," >&2
    echo "  and both gates restore the same $BUILD_DIR/fixtures/pool.img." >&2
    echo "  Wait for the other run to finish, or use a separate worktree." >&2
    exit 2
fi

trap reap_qemu EXIT

# --- ensure boot artifacts exist (match test.sh: build-if-missing) ---
KERNEL_BIN="$BUILD_DIR/kernel/thylacine.bin"
RAMFS="$BUILD_DIR/ramfs.cpio"
POOL="$BUILD_DIR/fixtures/pool.img"
if [[ ! -f "$KERNEL_BIN" || ! -f "$RAMFS" ]]; then
    echo "==> kernel/ramfs missing; building (tools/build.sh kernel)..."
    "$REPO_ROOT/tools/build.sh" kernel
fi
if [[ ! -f "$POOL" ]]; then
    echo "==> pool fixture missing; building (tools/build.sh pool)..."
    "$REPO_ROOT/tools/build.sh" pool
fi

# --- #85: per-attempt pool isolation ------------------------------------------
# Every scenario boots against the SAME pool.img and MUTATES it, and nothing ever
# reset it -- so the fixture carried whatever the previous 31 scenarios left
# behind. That is the worst property a test fixture can have, because it fails in
# BOTH directions:
#   #82: a FAILED ls-gfx-mode leaves `mode 1600 900` in /lib/aurora/config, and
#        every later boot inherits a 1600x900 display -- breaking geometry
#        asserts in scenarios that do not heal (ls-gfx-panes does not). A false
#        RED, blamed on a merge for a day.
#   #83: a config.cfg written by some earlier run satisfied ls-gfx-play's
#        assertion trivially, so the leg kept passing for five days after the
#        path it was asserting stopped being written. A false GREEN -- it MASKED
#        a real failure, which is strictly worse.
# Measured contamination at the time of writing: the live pool differed from
# pristine in 73,911,951 bytes.
#
# The machinery already existed and LS-CI was simply the one gate not using it:
# build.sh maintains `.baked-snapshot` twins, and both smp-multiboot.sh and
# chase-bench.sh already restore from them before every boot. cp -c is an APFS
# clonefile; falls back to a plain copy off APFS.
#
# COST, measured on the real shape (2.5 GB image, ~70 MiB divergence, restoring
# over an EXISTING destination): ~30 ms, flat across repetitions. Note that a
# clone to a FRESH path is ~2 ms -- the extra cost is unlinking the old file, so
# do not quote the 2 ms figure for this path. A full 32-scenario gate does <=96
# restores = ~3 s against a run measured in tens of minutes. Space is near-free
# (CoW, and only ONE divergence is live at a time -- each restore frees the last).
#
# Scope is the ATTEMPT, not the scenario. An attempt is a full re-run from the
# top, so a failed attempt's mutations must not poison its own retry (exactly
# #82's shape). A scenario's own multiple boots live INSIDE one attempt --
# ls-gfx-mode/-font/-osd-persist deliberately persist state in boot 1 and read it
# back in boot 2 -- so they are untouched by construction.
#
# The key twin is validated coherent before restoring: the ramfs bakes the key,
# so ONLY the pool matching the live key may be restored. LS_CI_POOL_RESTORE=0
# opts out (e.g. to reproduce a contamination bug deliberately).
POOL_SNAP="$POOL.baked-snapshot"
KEYFILE="$BUILD_DIR/fixtures/system.key"
KEY_SNAP="$KEYFILE.baked-snapshot"
pool_restored=0
pool_restore() {
    [[ "${LS_CI_POOL_RESTORE:-1}" == "0" ]] && return 0
    [[ -f "$POOL_SNAP" && -f "$POOL" ]] || return 0
    if ! cmp -s "$KEYFILE" "$KEY_SNAP" 2>/dev/null; then
        [[ $pool_restored -eq 0 ]] && echo "    (pool restore SKIPPED: system.key does not match its snapshot -- stale twins? re-run tools/build.sh pool)" >&2
        pool_restored=-1
        return 0
    fi
    # A restore that fails part-way leaves a TRUNCATED pool, and booting on it
    # would surface as guest corruption -- the #74/#60 fail-open shape, where
    # the harness's own fault gets read as a Thylacine defect. Refuse to boot on
    # a fixture whose state we do not know.
    if ! { cp -c "$POOL_SNAP" "$POOL" 2>/dev/null || cp "$POOL_SNAP" "$POOL"; }; then
        echo "==> FATAL: pool restore failed -- $POOL may be partial/truncated." >&2
        echo "    Refusing to boot on an unknown fixture (it would read as guest corruption)." >&2
        echo "    Check free space, or re-run 'tools/build.sh pool'." >&2
        exit 1
    fi
    pool_restored=1
}
if [[ ! -f "$POOL_SNAP" ]]; then
    echo "==> NOTE: no pool snapshot ($POOL_SNAP) -- scenarios will share a mutable pool (#85)." >&2
    echo "    Re-run 'tools/build.sh pool' to mint the pristine twin." >&2
fi

# build/disk.img is the SECOND shared mutable fixture (the virtio-blk scratch
# device), and it carries a masking hazard of its own -- #87. usr/virtio-blk-rw
# runs every boot as pass A read-verify / pass B write / pass C read-back, and
# pass C is the only proof the write landed. On a FRESH disk the write region
# holds pattern A, so a silently-dropped pass B makes pass C read A != B and
# FAIL. On a REUSED disk the region already holds pattern B from the last boot,
# so the same dropped write reads a stale B and PASSES. Reusing the fixture
# therefore disarms the write proof -- #83's stale-artifact class again.
#
# Restoring per attempt makes every LS-CI boot a "boot 1", so the leg can fail
# again here. It is a MITIGATION, not the fix: test.sh and smp-multiboot.sh
# share the same fixture and stay exposed until #87's guest-side fresh-pattern
# fix lands. Unlike the pool there is no build.sh-maintained twin, so mint one
# with mkdisk.py (deterministic, ~0.45 s, once) and clone from it thereafter.
DISK="$BUILD_DIR/disk.img"
DISK_SNAP="$DISK.pristine"
disk_restore() {
    [[ "${LS_CI_POOL_RESTORE:-1}" == "0" ]] && return 0
    [[ -f "$DISK" ]] || return 0
    local want have
    want="$(wc -c < "$DISK" | tr -d ' ')"
    have="$([[ -f "$DISK_SNAP" ]] && wc -c < "$DISK_SNAP" | tr -d ' ' || echo 0)"
    # Mint (or re-mint on a size change -- THYLACINE_DISK_SIZE varies for the
    # 1 GiB stress config, and a wrong-sized twin would silently resize the
    # device under the guest).
    if [[ "$want" != "$have" ]]; then
        command -v python3 >/dev/null 2>&1 || return 0
        python3 "$REPO_ROOT/tools/mkdisk.py" "$DISK_SNAP" "$want" >/dev/null 2>&1 || return 0
    fi
    # Same fail-closed rule as the pool: a partial disk.img would fail the
    # boot's pattern-A verify and read as a guest defect. A MISSING python3 is
    # different -- that degrades silently to the shared fixture above, which is
    # merely the old behaviour, not an unknown one.
    if ! { cp -c "$DISK_SNAP" "$DISK" 2>/dev/null || cp "$DISK_SNAP" "$DISK"; }; then
        echo "==> FATAL: disk restore failed -- $DISK may be partial/truncated." >&2
        echo "    Refusing to boot on an unknown fixture (it would read as guest corruption)." >&2
        exit 1
    fi
}

# --- relay preflight (host-only, ~4s, no QEMU) ---
# serial-bridge.py is the carrier EVERY scenario below depends on, and its two
# load-bearing properties (never back-pressure the guest; emit a DISCRIMINATING
# exit record) are provable without booting anything. Left unrun they rot: the
# exit-record check exists precisely because `stdout-broken` was read as a
# diagnosis for three sessions of #78. A harness that fails OPEN is the #74
# lesson, so this runs first and hard-fails -- a broken relay would otherwise
# surface as a mysterious guest failure in every scenario.
BRIDGE_TEST="$SCEN_DIR/test-serial-bridge.py"
if [[ -f "$BRIDGE_TEST" ]] && command -v python3 >/dev/null 2>&1; then
    echo "==> preflight: serial-bridge relay properties"
    if ! python3 "$BRIDGE_TEST"; then
        echo "==> FAIL: serial-bridge relay preflight -- not booting anything." >&2
        exit 1
    fi
fi

# --- scenario selection ---
scenarios=()
if [[ $# -gt 0 ]]; then
    for a in "$@"; do
        if   [[ -f "$a" ]];               then scenarios+=("$a")
        elif [[ -f "$SCEN_DIR/$a" ]];     then scenarios+=("$SCEN_DIR/$a")
        elif [[ -f "$SCEN_DIR/$a.exp" ]]; then scenarios+=("$SCEN_DIR/$a.exp")
        else echo "==> scenario not found: $a" >&2; exit 2
        fi
    done
else
    for f in "$SCEN_DIR"/*.exp; do
        [[ "$(basename "$f")" == "lib.exp" ]] && continue   # library, not a scenario
        scenarios+=("$f")
    done
fi
if [[ ${#scenarios[@]} -eq 0 ]]; then
    echo "==> no scenarios under $SCEN_DIR" >&2
    exit 2
fi

mkdir -p "$BUILD_DIR"
if [[ "${LS_CI_POOL_RESTORE:-1}" == "0" ]]; then
    pool_iso="pool=SHARED (LS_CI_POOL_RESTORE=0 -- contamination possible, #85)"
elif [[ -f "$POOL_SNAP" ]]; then
    pool_iso="pool=per-attempt"
else
    pool_iso="pool=SHARED (no snapshot, #85)"
fi
echo "==> LS-CI: ${#scenarios[@]} scenario(s); accel=$THYLACINE_ACCEL boot<=${LS_CI_BOOT_TIMEOUT}s cmd<=${LS_CI_CMD_TIMEOUT}s $pool_iso"

# Bounded retry per scenario. It does NOT mask a real regression: a genuine
# break (e.g. LS-2 reverted) fails EVERY attempt deterministically (the output is
# missing each time), so a scenario fails only if ALL attempts fail.
#
# #72 CORRECTION -- this block used to justify the retry by asserting that "an
# unexpected qemu exit before a terminal PASS/FAIL is a host-timing artifact
# (TCG-under-oversubscription), never a kernel fault". That was WRONG, and it
# was never measured. Ground truth (N=10, instrumented): 5 of 10 boots were
# lost, and in ALL FIVE the VM was still ALIVE (stat R+/S+) while the `nc -U`
# serial relay had died of SIGPIPE (`bridge exit=141`). It was never a qemu
# exit at all -- it was the HARNESS's own relay dying, mislabelled by lib.exp's
# `eof` arm and then rationalized here as host timing. The relay is now
# serial-bridge.py (SIGPIPE-immune); the retry stays as belt-and-braces, but a
# retry is a TOLERANCE, never a diagnosis. If attempts start failing again,
# read the preserved evidence -- do not reach for "host timing".
fails=0
infra_fails=0
harness_fails=0
skips=0
attempts="${LS_CI_ATTEMPTS:-3}"
for scen in "${scenarios[@]}"; do
    name="$(basename "$scen" .exp)"
    transcript="$BUILD_DIR/ls-ci-$name.log"
    steps="$BUILD_DIR/ls-ci-$name.steps"
    # #72: failed-attempt evidence must survive the retry. The per-attempt
    # truncation below used to destroy the very transcript a retry was
    # retrying over, so a claim like "attempt 1 was a host-timing artifact"
    # could never be checked against its own evidence -- the no-host-load
    # discipline needs the artifact to LOOK at. Each failed attempt is
    # archived as ls-ci-<name>.attempt<N>.{log,steps}; retention is bounded
    # to the LAST run (cleared here, per scenario, not per attempt).
    rm -f "$BUILD_DIR/ls-ci-$name.attempt"*.log \
          "$BUILD_DIR/ls-ci-$name.attempt"*.steps 2>/dev/null || true
    echo "==> scenario: $name (up to $attempts attempt(s))"
    passed=0
    skipped=0
    for attempt in $(seq 1 "$attempts"); do
        : > "$transcript"
        : > "$steps"
        reap_qemu
        sleep 0.5
        # #85: pristine fixtures for this attempt. STRICTLY after reap_qemu +
        # the settle -- overwriting an image out from under a live VM is how you
        # manufacture the corruption this gate exists to detect.
        pool_restore
        disk_restore   # the virtio-blk scratch device (#87 masking)
        # Run expect UNDER `script` so its stdio is a real PTY. macOS expect 5.45
        # corrupts its own std channels inside `spawn` when its stdout is NOT a tty
        # (a `>file` redirect OR a pipe) -- it aborts with "Tcl_RegisterChannel:
        # duplicate channel names" (SIGABRT) or breaks `puts` with "bad file number".
        # `script -q <file> <cmd>` gives <cmd> a controlling PTY, captures the full
        # session to <file> (our transcript), AND propagates <cmd>'s exit code. The
        # steps file is the flush-immune live view. `< /dev/null` is a clean stdin;
        # `script` still waits for the wrapped command to exit (verified).
        LS_CI_STEPS="$steps" script -q "$transcript" expect -f "$scen" < /dev/null >/dev/null 2>&1
        rc=$?
        reap_qemu
        # 77 is the conventional SKIP code: the SCENARIO decided it cannot run
        # (a missing optional host artifact, e.g. ls-gfx-mp without
        # build/quake/host/tyr-quake). That is not a guest result. Retrying
        # cannot change it, and counting it as a failure reports a regression
        # that does not exist -- the #74 fail-open lesson pointed the other way.
        # ls-gfx-mp was reported as one of #82's six "gfx regressions" purely
        # because of this.
        if [[ $rc -eq 77 ]]; then
            reason="$(grep -ha 'LS-CI SKIP:' "$transcript" 2>/dev/null | head -1 | sed 's/.*LS-CI SKIP: //' | tr -d '\r')"
            echo "    SKIP: $name${reason:+ -- $reason}"
            skipped=1
            break
        fi
        if [[ $rc -eq 0 ]]; then
            if [[ $attempt -gt 1 ]]; then
                echo "    PASS: $name (attempt $attempt/$attempts; earlier failed attempt(s) preserved: $BUILD_DIR/ls-ci-$name.attempt*.{log,steps})"
            else
                echo "    PASS: $name"
            fi
            passed=1
            break
        fi
        # Preserve this attempt's evidence BEFORE the next attempt truncates.
        cp "$transcript" "$BUILD_DIR/ls-ci-$name.attempt$attempt.log" 2>/dev/null || true
        cp "$steps" "$BUILD_DIR/ls-ci-$name.attempt$attempt.steps" 2>/dev/null || true
        echo "    attempt $attempt/$attempts FAILED (rc=$rc; evidence: ls-ci-$name.attempt$attempt.{log,steps})" >&2
        [[ $attempt -lt $attempts ]] && echo "    retrying (an unexplained early exit -- see the preserved evidence; a retry is NOT a diagnosis)..." >&2
    done
    if [[ $skipped -eq 1 ]]; then
        skips=$((skips + 1))
        continue
    fi
    if [[ $passed -ne 1 ]]; then
        # An attempt whose VM never STARTED says nothing about the guest, and
        # calling it "a real regression" is failing open (#74). lib.exp records
        # QEMU's own refusal under an INFRA: marker; surface that verdict
        # instead, and count it separately.
        if grep -qa "^INFRA:" "$BUILD_DIR/ls-ci-$name.attempt"*.steps "$steps" 2>/dev/null; then
            echo "    INFRA-FAIL: $name -- the VM never started; this is a HARNESS/environment fault, NOT a guest regression:" >&2
            grep -ha "^INFRA:" "$BUILD_DIR/ls-ci-$name.attempt"*.steps "$steps" 2>/dev/null | sort -u | sed 's/^/        /' >&2
            infra_fails=$((infra_fails + 1))
            fails=$((fails + 1))
            continue
        fi
        # A cut where the bridge reports the READER went away while the VM was
        # still ALIVE is #60: the harness's own expect side closed the pipe.
        # The guest booted, passed earlier legs, and was healthy at the cut --
        # so "deterministic = a real regression" is the #74 fail-open pointed
        # the other way, blaming the guest for a harness fault. Ground truth
        # (2026-07-28, ls-gfx): three attempts, three different legs, all
        # `reason=stdout-broken`, VM stat=S+/R+, cut bytes within 1 KB of each
        # other -- #60's fingerprint, not a regression.
        #
        # This is NOT a pass: the scenario's remaining legs never ran, so
        # coverage was LOST and the gate stays RED. It is only attributed
        # honestly. Requiring EVERY attempt to carry the fingerprint is what
        # keeps it from failing open -- one timeout, EXTINCTION, or genuine
        # qemu exit among the attempts drops it to the real-regression branch.
        n_att=0
        n_cut=0
        for s in "$BUILD_DIR/ls-ci-$name.attempt"*.steps; do
            [[ -f "$s" ]] || continue
            n_att=$((n_att + 1))
            if grep -qa "bridge-at-fail:.*reason=stdout-broken" "$s" 2>/dev/null \
               && grep -qa "^vm-at-fail:.*stat=" "$s" 2>/dev/null; then
                n_cut=$((n_cut + 1))
            fi
        done
        if [[ $n_att -gt 0 && $n_cut -eq $n_att ]]; then
            echo "    HARNESS-FAIL: $name -- all $n_att attempt(s) cut by the serial relay losing its READER while the VM was ALIVE (#60). Coverage LOST; this says NOTHING about the guest:" >&2
            grep -ha "bridge-at-fail:" "$BUILD_DIR/ls-ci-$name.attempt"*.steps 2>/dev/null | sed 's/^/        /' >&2
            echo "        evidence: $BUILD_DIR/ls-ci-$name.attempt*.{log,steps}" >&2
            harness_fails=$((harness_fails + 1))
            fails=$((fails + 1))
            continue
        fi
        echo "    FAIL: $name -- all $attempts attempts failed (deterministic = a real regression, not a flake)" >&2
        echo "    --- steps ($steps; last attempt) ---" >&2
        cat "$steps" >&2 2>/dev/null || true
        echo "    --- transcript tail ($transcript; last attempt) ---" >&2
        tr -d '\r' < "$transcript" 2>/dev/null | tail -40 >&2 || true
        echo "    every attempt's evidence: $BUILD_DIR/ls-ci-$name.attempt*.{log,steps}" >&2
        echo "    --------------------------------------" >&2
        fails=$((fails + 1))
    fi
done

if [[ $fails -gt 0 ]]; then
    guest_fails=$((fails - infra_fails - harness_fails))
    echo "==> LS-CI: FAIL -- $fails/${#scenarios[@]} scenario(s) failed" \
         "($guest_fails guest, $infra_fails INFRA [VM never started], $harness_fails HARNESS [#60 relay cut, VM alive])." >&2
    if [[ $infra_fails -gt 0 || $harness_fails -gt 0 ]]; then
        echo "    Only the $guest_fails guest failure(s) say anything about Thylacine;" \
             "the rest are environment/harness faults that LOST coverage -- re-run or fix the harness." >&2
    fi
    [[ $skips -gt 0 ]] && echo "    ($skips scenario(s) SKIPPED -- a missing optional host artifact, not a guest result)" >&2
    exit 1
fi
if [[ $skips -gt 0 ]]; then
    echo "==> LS-CI: PASS -- $(( ${#scenarios[@]} - skips ))/${#scenarios[@]} scenario(s); $skips SKIPPED (missing optional host artifact -- NOT a guest result, and NOT coverage)."
else
    echo "==> LS-CI: PASS -- all ${#scenarios[@]} scenario(s)."
fi
exit 0
