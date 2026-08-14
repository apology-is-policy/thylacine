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
#   THYLACINE_ACCEL=hvf|tcg     default accel (tcg -- the deterministic compat
#                               run). 14 gfx scenarios override this to hvf in
#                               the .exp itself; the timings table reports what
#                               actually BOOTED, not this.
#   LS_CI_JOBS=N                scenarios to run at once (default 1). RAM-bound,
#                               not core-bound -- each VM takes
#                               THYLACINE_MEM_MIB. Boot/cmd budgets scale by N.
#   LS_CI_BOOT_TIMEOUT=N        seconds to reach the shell (300 with a staged
#                               goroot, else 180). Pinning it disables scaling.
#   LS_CI_CMD_TIMEOUT=N         seconds per command's output (default 30)
#   LS_CI_ATTEMPTS=N            attempts per scenario (default 3)
#
# Timings land in build/ls-ci-timings.tsv and in a sorted summary (G-1).

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
# Record whether the CALLER pinned the budgets, before the defaults below make
# that unknowable. The JOBS scaling further down must not silently override an
# explicit value -- if you pinned it, you meant it.
BOOT_TIMEOUT_PINNED="${LS_CI_BOOT_TIMEOUT:+1}"
CMD_TIMEOUT_PINNED="${LS_CI_CMD_TIMEOUT:+1}"
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
#
# #217 (the intra-tree half of #59, surfaced by aux): the EXIT trap below is
# still TREE-wide, so a VM this script never started (an SMP gate, a test.sh
# boot, a manual run-vm) dies uncatchably on our way out -- its log just
# stops, the misread-as-flake shape. Refuse up front instead of narrowing:
# in-tree concurrency is unsafe for a second reason anyway (both gates
# restore the same pool.img, and a restore under a live VM manufactures
# exactly the corruption the gates exist to detect), so a named operator
# error beats a silent mutual-corruption race. This check MUST precede the
# trap install -- install-then-refuse fires the reaper on the refusal's own
# exit, killing the VM it just declined to disturb (aux's 849d85fc lesson).
# (pgrep never matches itself, procps + BSD both; the pattern must never
# appear in a wrapper's cmdline.)
if pgrep -f "qemu-system-aarch64.*$BUILD_DIR/" >/dev/null 2>&1; then
    echo "test-interactive: a qemu from THIS tree is already running" >&2
    echo "  (matches: qemu-system-aarch64.*$BUILD_DIR/). Refusing to start:" >&2
    echo "  this script's EXIT reaper is tree-wide and would SIGKILL it," >&2
    echo "  and concurrent gates corrupt the shared pool fixture anyway." >&2
    echo "  Finish or kill that run first (by explicit PID)." >&2
    exit 2
fi
reap_qemu() { pkill -9 -f "qemu-system-aarch64.*$BUILD_DIR/" 2>/dev/null || true; }
trap reap_qemu EXIT

# G-2: the SLOT-scoped reaper. reap_qemu above kills every VM in this tree,
# which is correct for a serial run and catastrophic for a parallel one -- a
# scenario finishing its first boot would shoot down all its neighbours. Each
# slot's qemu carries its own fixture + QMP paths on the cmdline, so the slot
# directory is a sufficient discriminator.
#
# MEASURED, not assumed: bash resets *signal* traps in a subshell but the EXIT
# trap still fires when that subshell exits, so a forked scenario inherits
# `trap reap_qemu EXIT` and runs the TREE-WIDE reaper on the way out. That is
# #59's cross-tree shootout reproduced inside one tree, and it would present as
# "qemu GONE, guest healthy" mid-scenario -- indistinguishable from a guest
# fault and exactly the shape this project keeps mistaking for load. Every
# forked scenario therefore re-traps to its own slot before doing anything.
reap_slot() { pkill -9 -f "qemu-system-aarch64.*$1/" 2>/dev/null || true; }

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
#
# G-2 made the destination a PARAMETER. Restoring the pristine twin into a
# per-scenario slot instead of over the one shared path is the whole of what
# unblocks concurrency: the isolation was always per-attempt, but every attempt
# restored into the SAME `$POOL`, so two scenarios could never be in flight at
# once. Semantics are unchanged -- each scenario still begins from the same
# pristine snapshot, and a scenario's own multiple boots still share its pool
# (ls-gfx-mode/-font/-osd-persist persist state across boots by design).
pool_restored=0
pool_restore() {
    local dest="${1:-$POOL}"
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
    if ! { cp -c "$POOL_SNAP" "$dest" 2>/dev/null || cp "$POOL_SNAP" "$dest"; }; then
        echo "==> FATAL: pool restore failed -- $dest may be partial/truncated." >&2
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
    local dest="${1:-$DISK}"
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
    if ! { cp -c "$DISK_SNAP" "$dest" 2>/dev/null || cp "$DISK_SNAP" "$dest"; }; then
        echo "==> FATAL: disk restore failed -- $dest may be partial/truncated." >&2
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
# --- G-3: the TCG anchor set -------------------------------------------------
# Accel is not only a speed knob. tools/run-vm.sh derives the CPU model AND the
# GIC version from it -- hvf gives `-cpu host` + GICv2, tcg gives `-cpu max` +
# GICv3 (HVF cannot do v3 here at all: its emulated GICv3 distributor trips an
# `isv` data-abort assert). So every scenario moved to HVF stops covering
# GICv3 and `-cpu max`, and #166 is the standing proof that a scenario can go
# INERT under HVF while still reporting PASS -- the worst outcome available, a
# green test that quietly stopped testing.
#
# These scenarios therefore stay on TCG, and the list is MECHANICAL rather than
# "whatever nobody got round to flipping":
#   freeze-172 / flood-174  TCG's serialized vCPU + main loop IS their trigger
#                           ("HVF wedges on a mere held key"); they also spawn
#                           run-vm.sh themselves with accel pinned.
#   split173                pins tcg for a deterministic split.
#   nora-demo               breaks under HVF (bug_nora_hvf_cpr_handshake).
#   idle-probe              the tickless-idle guard is timer/interrupt-shaped;
#                           `make idle-gate` covers the HVF side separately.
#   ls-7                    #70 was a TCG-ONLY watchpoint livelock -- moving it
#                           would retire that regression's only coverage.
#   pty-4                   #162 reproduces only under TCG gate load; this is
#                           the sole place that open bug can still be seen.
# Together they keep GICv3 and `-cpu max` live in every run.
#
# Enforced, not documented: a directive on an anchor is REFUSED, never silently
# honoured. A coverage rule that depends on nobody editing the wrong file is not
# a rule.
LS_CI_TCG_ANCHORS="freeze-172 flood-174 split173 nora-demo idle-probe ls-7 pty-4"
for a in $LS_CI_TCG_ANCHORS; do
    f="$SCEN_DIR/$a.exp"
    [[ -f "$f" ]] || continue
    if grep -q 'set ::env(THYLACINE_ACCEL) hvf' "$f"; then
        echo "==> REFUSING TO RUN: $a is a TCG anchor but its .exp forces hvf." >&2
        echo "    Anchors keep GICv3 + '-cpu max' covered, and each has a reason" >&2
        echo "    recorded in tools/test-interactive.sh. Remove the directive, or" >&2
        echo "    remove $a from LS_CI_TCG_ANCHORS *and say why in the same commit*." >&2
        exit 2
    fi
done

# Default 3, tuned to this 8-core / 8 GB host and proven by a full green 34/34
# run (4908s serial -> 2925s wall). Concurrency is RAM-bound, not core-bound --
# each VM takes THYLACINE_MEM_MIB -- so lower it on a smaller machine. The
# budgets scale with the job count, so an over-subscribed host degrades into
# slowness rather than into false failures; it will still swap if you ask for
# more VMs than the RAM holds.
JOBS="${LS_CI_JOBS:-3}"
case "$JOBS" in ''|*[!0-9]*) echo "==> LS_CI_JOBS must be a positive integer (got '$JOBS')" >&2; exit 2 ;; esac
[[ "$JOBS" -lt 1 ]] && JOBS=1

# Fail CLOSED on the one incoherent combination. With restores disabled the slot
# never receives a pool, and run-vm.sh silently boots without one -- every
# scenario would then fail on a missing /srv/stratum-fs and the gate would
# report 34 guest regressions that are really one bad flag. Refusing is the #74
# lesson: a harness must not manufacture failures it then attributes to Thylacine.
# #137: scale the budgets with the job count, or concurrency manufactures guest
# regressions. The boot timeout exists to catch a WEDGED guest, not to enforce a
# performance SLA -- and under LS_CI_JOBS>1 the same healthy guest is
# legitimately slower: run-vm.sh gives each VM 4 vCPUs, so N VMs oversubscribe
# this 8-core host and TCG emulation is CPU-bound. MEASURED at JOBS=3: three TCG
# scenarios that take ~190s serially take ~400s each, blowing the fixed 300s
# budget while their logs show the guest still counting up through the boot
# ladder. That is a harness fault reported as a guest regression -- the #74
# fail-open lesson pointed the other way.
#
# Erring generous is the safe direction: an over-large budget only delays
# declaring a genuinely wedged guest dead (it is still declared), whereas an
# under-sized one reports failures that do not exist. An explicit
# LS_CI_BOOT_TIMEOUT in the environment still wins outright -- this only scales
# the default.
if [[ "$JOBS" -gt 1 ]]; then
    [[ -z "$BOOT_TIMEOUT_PINNED" ]] && export LS_CI_BOOT_TIMEOUT=$((LS_CI_BOOT_TIMEOUT * JOBS))
    [[ -z "$CMD_TIMEOUT_PINNED" ]]  && export LS_CI_CMD_TIMEOUT=$((LS_CI_CMD_TIMEOUT * JOBS))
    echo "==> LS_CI_JOBS=$JOBS: budgets boot<=${LS_CI_BOOT_TIMEOUT}s cmd<=${LS_CI_CMD_TIMEOUT}s" \
         "(scaled by job count unless pinned; a healthy guest is slower under contention," \
         "and the budget catches a WEDGE, not slowness)"
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
#
# --- G-1: per-scenario / per-attempt timings ----------------------------------
# Until this landed, NOTHING in the gate recorded how long anything took, so
# every statement about which scenarios are expensive was arithmetic over a
# single wall-clock total, not a measurement. That is the wrong footing for the
# two changes queued behind it: G-2 (parallelism) needs a per-scenario cost to
# pack slots sensibly and a before/after to prove it gained anything, and G-3
# (per-scenario accel) allocates a scarce, riskier resource BY cost -- deciding
# that from a guess is how you spend the risk budget on a cheap scenario.
#
# Each attempt is timed separately, not just each scenario: a scenario that
# passes on attempt 3 costs three boots, and folding that into one number hides
# both the retry and the true per-boot cost. `SECONDS` is a bash builtin (no
# subprocess, no locale, works on the stock macOS bash 3.2 this runs under).
#
# The TSV is the artifact G-2/G-3 are measured against; the on-screen summary is
# for the human reading the run. Both carry the accel, because a timing without
# its accel is unusable for G-3 -- comparing a tcg number against an hvf one and
# calling the difference a regression is exactly the trap.
TIMINGS="$BUILD_DIR/ls-ci-timings.tsv"
printf 'scenario\tattempt\trc\tverdict\tseconds\taccel\n' > "$TIMINGS"
gate_t0=$SECONDS

attempts="${LS_CI_ATTEMPTS:-3}"

# G-2: one scenario, start to verdict, in its own slot.
#
# Always invoked inside a SUBSHELL -- in serial mode too, so the two modes run
# identical code and the trap re-arming below cannot be a parallel-only path
# that rots untested. Nothing is returned in variables: the verdict is a file,
# because in parallel mode there is no shared memory to increment a counter in,
# and having one accounting path rather than two is what keeps the serial and
# parallel tallies from drifting apart.
run_one_scenario() {
    local scen="$1" slot="$2"
    local name transcript steps passed skipped attempt rc att_t0 att_dur scen_t0
    local reason n_att n_cut s
    name="$(basename "$scen" .exp)"
    scen_t0=$SECONDS

    # Re-arm the EXIT trap to THIS slot before anything can boot. The inherited
    # trap is the tree-wide reaper, and bash runs an inherited EXIT trap when a
    # subshell exits (measured -- signal traps are reset in a subshell, EXIT is
    # not), so without this line the first scenario to finish would kill every
    # other scenario's live VM. That is #59's cross-tree shootout reproduced
    # inside one tree, and it would surface as "qemu GONE, guest healthy"
    # mid-scenario -- indistinguishable at the expect layer from a guest fault.
    # Release rides the trap rather than the return paths: there are five ways
    # out of this function and a crash is a sixth, and a fixture clone leaked on
    # the one uncovered path is how disks fill.
    trap "reap_slot '$slot'; slot_release '$slot'" EXIT

    # Every one of these was a FIXED path shared by all 34 scenarios, and #127's
    # lesson is that a fixed host resource is a DETERMINISTIC collision at N>1 --
    # not a flake, and not something a retry budget can help with. The pool and
    # disk are the fixtures; qmp.sock is the control socket 14 gfx scenarios
    # drive. (The serial socket and vm log are already per-pid in lib.exp, and
    # the VNC display is already probe-allocated -- see LS_CI_VNC_BASE below for
    # why the probe alone is not enough once we are the ones racing.)
    mkdir -p "$slot"
    export THYLACINE_POOL_IMG="$slot/pool.img"
    export THYLACINE_DISK_IMG="$slot/disk.img"
    export THYLACINE_QMP_SOCK="$slot/qmp.sock"
    # Spread the VNC probe's starting point per slot. lc_pick_vnc_display derives
    # its base from the REPO PATH, which separates trees but gives every scenario
    # in THIS tree the same base -- fine when one runs at a time, a race between
    # the bind-test and qemu's own bind when several do. The probe stays as the
    # backstop; this makes it stop being the primary mechanism.
    export LS_CI_VNC_BASE="${3:-0}"

    # Per-slot timings, merged by the parent after the run. Appending to one
    # shared TSV from N concurrent writers would interleave partial lines.
    local TIMINGS="$slot/timings.tsv"
    : > "$TIMINGS"
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
        reap_slot "$slot"
        sleep 0.5
        # #85: pristine fixtures for this attempt. STRICTLY after the reap +
        # the settle -- overwriting an image out from under a live VM is how you
        # manufacture the corruption this gate exists to detect.
        pool_restore "$slot/pool.img"
        disk_restore "$slot/disk.img"   # the virtio-blk scratch device (#87 masking)
        # Run expect UNDER `script` so its stdio is a real PTY. macOS expect 5.45
        # corrupts its own std channels inside `spawn` when its stdout is NOT a tty
        # (a `>file` redirect OR a pipe) -- it aborts with "Tcl_RegisterChannel:
        # duplicate channel names" (SIGABRT) or breaks `puts` with "bad file number".
        # `script -q <file> <cmd>` gives <cmd> a controlling PTY, captures the full
        # session to <file> (our transcript), AND propagates <cmd>'s exit code. The
        # steps file is the flush-immune live view. `< /dev/null` is a clean stdin;
        # `script` still waits for the wrapped command to exit (verified).
        att_t0=$SECONDS
        LS_CI_STEPS="$steps" script -q "$transcript" expect -f "$scen" < /dev/null >/dev/null 2>&1
        rc=$?
        att_dur=$((SECONDS - att_t0))
        # The accel that ACTUALLY booted, read out of the artifact rather than
        # taken from our own environment. 14 scenarios set THYLACINE_ACCEL=hvf
        # inside the .exp itself, so the wrapper's value is simply wrong for
        # them -- and a timings table whose accel column is wrong is worse than
        # one with no accel column, because it invites exactly the tcg-vs-hvf
        # comparison the column exists to prevent. lib.exp records the resolved
        # value in the steps file at boot ("BOOT vm accel=<x> ...").
        att_accel="$(grep -ha -o 'accel=[a-z]*' "$steps" 2>/dev/null | head -1 | cut -d= -f2)"
        [[ -z "$att_accel" ]] && att_accel="$THYLACINE_ACCEL"
        reap_slot "$slot"
        # 77 is the conventional SKIP code: the SCENARIO decided it cannot run
        # (a missing optional host artifact, e.g. ls-gfx-mp without
        # build/quake/host/tyr-quake). That is not a guest result. Retrying
        # cannot change it, and counting it as a failure reports a regression
        # that does not exist -- the #74 fail-open lesson pointed the other way.
        # ls-gfx-mp was reported as one of #82's six "gfx regressions" purely
        # because of this.
        if [[ $rc -eq 77 ]]; then
            reason="$(grep -ha 'LS-CI SKIP:' "$transcript" 2>/dev/null | head -1 | sed 's/.*LS-CI SKIP: //' | tr -d '\r')"
            printf '%s\t%s\t%s\tSKIP\t%s\t%s\n' "$name" "$attempt" "$rc" "$att_dur" "$att_accel" >> "$TIMINGS"
            echo "    SKIP: $name [${att_dur}s]${reason:+ -- $reason}"
            skipped=1
            break
        fi
        if [[ $rc -eq 0 ]]; then
            printf '%s\t%s\t%s\tPASS\t%s\t%s\n' "$name" "$attempt" "$rc" "$att_dur" "$att_accel" >> "$TIMINGS"
            if [[ $attempt -gt 1 ]]; then
                echo "    PASS: $name [${att_dur}s] (attempt $attempt/$attempts; earlier failed attempt(s) preserved: $BUILD_DIR/ls-ci-$name.attempt*.{log,steps})"
            else
                echo "    PASS: $name [${att_dur}s]"
            fi
            passed=1
            break
        fi
        printf '%s\t%s\t%s\tFAIL\t%s\t%s\n' "$name" "$attempt" "$rc" "$att_dur" "$att_accel" >> "$TIMINGS"
        # Preserve this attempt's evidence BEFORE the next attempt truncates.
        cp "$transcript" "$BUILD_DIR/ls-ci-$name.attempt$attempt.log" 2>/dev/null || true
        cp "$steps" "$BUILD_DIR/ls-ci-$name.attempt$attempt.steps" 2>/dev/null || true
        echo "    attempt $attempt/$attempts FAILED [${att_dur}s] (rc=$rc; evidence: ls-ci-$name.attempt$attempt.{log,steps})" >&2
        [[ $attempt -lt $attempts ]] && echo "    retrying (an unexplained early exit -- see the preserved evidence; a retry is NOT a diagnosis)..." >&2
    done
    # The TOTAL row is not the sum of this scenario's attempt rows: it also
    # carries the per-scenario overhead the attempt timer cannot see (the reap,
    # the 0.5 s settle, and both fixture restores). That overhead is precisely
    # what G-2 has to pay PER SLOT rather than once, so it has to be visible
    # here or the parallel projection is built on the wrong per-scenario cost.
    printf '%s\tTOTAL\t-\t%s\t%s\t%s\n' \
        "$name" "$([[ $skipped -eq 1 ]] && echo SKIP || { [[ $passed -eq 1 ]] && echo PASS || echo FAIL; })" \
        "$((SECONDS - scen_t0))" "${att_accel:-$THYLACINE_ACCEL}" >> "$TIMINGS"
    # The verdict is a FILE, not a counter -- see run_one_scenario's header.
    # Written coarse here and refined to INFRA/HARNESS below, so there is
    # exactly one place that can decide a scenario passed.
    if [[ $skipped -eq 1 ]]; then echo SKIP > "$slot/verdict"; return 0; fi
    if [[ $passed -eq 1 ]]; then echo PASS > "$slot/verdict"; return 0; fi
    echo FAIL > "$slot/verdict"
    if [[ $passed -ne 1 ]]; then
        # An attempt whose VM never STARTED says nothing about the guest, and
        # calling it "a real regression" is failing open (#74). lib.exp records
        # QEMU's own refusal under an INFRA: marker; surface that verdict
        # instead, and count it separately.
        if grep -qa "^INFRA:" "$BUILD_DIR/ls-ci-$name.attempt"*.steps "$steps" 2>/dev/null; then
            echo "    INFRA-FAIL: $name -- the VM never started; this is a HARNESS/environment fault, NOT a guest regression:" >&2
            grep -ha "^INFRA:" "$BUILD_DIR/ls-ci-$name.attempt"*.steps "$steps" 2>/dev/null | sort -u | sed 's/^/        /' >&2
            echo INFRA > "$slot/verdict"
            return 0
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
            echo HARNESS > "$slot/verdict"
            return 0
        fi
        echo "    FAIL: $name -- all $attempts attempts failed (deterministic = a real regression, not a flake)" >&2
        echo "    --- steps ($steps; last attempt) ---" >&2
        cat "$steps" >&2 2>/dev/null || true
        echo "    --- transcript tail ($transcript; last attempt) ---" >&2
        tr -d '\r' < "$transcript" 2>/dev/null | tail -40 >&2 || true
        echo "    every attempt's evidence: $BUILD_DIR/ls-ci-$name.attempt*.{log,steps}" >&2
        echo "    --------------------------------------" >&2
    fi
    return 0
}

# Drop this slot's heavy fixtures the moment its verdict is in. Each is a CoW
# clone of a 2.5 GB pool that diverges by tens of MiB as it runs; holding 34 of
# them to the end of the gate would be gigabytes of divergence for nothing, and
# this project has already had a disk fill up under it. The small metadata
# (verdict, timings) stays until the parent has merged it, and every artifact a
# failure needs -- transcripts, steps, per-attempt archives -- lives in build/
# and is not touched here.
slot_release() { rm -f "$1/pool.img" "$1/disk.img" "$1/qmp.sock" 2>/dev/null || true; }

# --- G-2: run the scenarios, up to $JOBS at a time ----------------------------
#
# The serial gate took 75 minutes because it could not do otherwise: isolation
# was already per-attempt, but every attempt restored the pristine pool into the
# SAME path, so two scenarios could never be in flight at once. The fixtures and
# the QMP socket are now per-slot, which is the whole of what was blocking this.
#
# Concurrency here is RAM-bound, not core-bound: this host has 8 cores but 8 GB,
# and each VM takes THYLACINE_MEM_MIB (2048 default), so ~3 is the honest
# ceiling. Overcommitting would swap, and a swapping host makes every timeout in
# the suite marginal -- which would then get read as guest flakiness. The
# default stays 1 until a parallel run has been proven green; LS_CI_JOBS is how
# you ask for more.
if [[ "$JOBS" -gt 1 && "${LS_CI_POOL_RESTORE:-1}" == "0" ]]; then
    echo "==> LS_CI_JOBS>1 requires the fixture restore (LS_CI_POOL_RESTORE=0 given)." >&2
    echo "    Parallel slots ARE the restore -- without it a slot has no pool to boot." >&2
    exit 2
fi

SLOTS="$BUILD_DIR/ls-ci-slots"
rm -rf "$SLOTS" 2>/dev/null || true
mkdir -p "$SLOTS"

# Mint the disk twin ONCE, here, before anything forks. disk_restore creates
# $DISK_SNAP on demand when it is missing or the size changed, and that path
# writes a SHARED file -- three workers entering it at once would tear the very
# twin they then clone from, which would surface as guest corruption in
# whichever scenario lost. The pool twin has no such hazard (build.sh mints it),
# but the disk twin is minted lazily by this script, so it is ours to serialize.
# Restoring the canonical disk here is harmless: no VM is running yet.
disk_restore "$DISK"

# Slot dirs hold CoW clones of a 2.5 GB pool. Cheap to make, but each one
# diverges by tens of MiB as its scenario runs, so they are dropped as soon as
# the verdict is in -- 34 retained slots would be gigabytes of divergence for
# nothing. The evidence a failure needs (transcripts, steps, per-attempt
# archives) lives in build/ and is untouched by this.
slot_dir_for() { echo "$SLOTS/$1"; }

pids=""
launched=0
for scen in "${scenarios[@]}"; do
    sname="$(basename "$scen" .exp)"
    slot="$(slot_dir_for "$sname")"
    if [[ "$JOBS" -le 1 ]]; then
        # Still a subshell: identical code path to the parallel case, so the
        # trap re-arming above cannot become a parallel-only branch that rots.
        # No VNC offset -- serial mode keeps byte-identical display selection to
        # the pre-G-2 gate, so a serial run stays a valid baseline to compare a
        # parallel one against.
        ( run_one_scenario "$scen" "$slot" )
    else
        # bash 3.2 (what macOS ships, and what this script runs under) has no
        # `wait -n`, so the slot limiter polls liveness instead of waiting on
        # the first completion. A 1 s poll against scenarios measured in
        # minutes costs nothing.
        while :; do
            live=""
            n=0
            for p in $pids; do
                if kill -0 "$p" 2>/dev/null; then live="$live $p"; n=$((n + 1)); fi
            done
            pids="$live"
            [[ $n -lt "$JOBS" ]] && break
            sleep 1
        done
        echo "==> start: $sname"
        ( run_one_scenario "$scen" "$slot" "$launched" ) > "$slot.console" 2>&1 &
        pids="$pids $!"
    fi
    launched=$((launched + 1))
done
wait

# In parallel mode each scenario's output was captured to keep the streams from
# interleaving into an unreadable mess. Replay it in ROSTER order (not completion
# order) so two runs of the same suite produce comparable logs.
if [[ "$JOBS" -gt 1 ]]; then
    for scen in "${scenarios[@]}"; do
        sname="$(basename "$scen" .exp)"
        [[ -f "$SLOTS/$sname.console" ]] && cat "$SLOTS/$sname.console"
    done
fi

# Merge the per-slot timings into the one TSV the summary reads.
for scen in "${scenarios[@]}"; do
    sname="$(basename "$scen" .exp)"
    [[ -f "$SLOTS/$sname/timings.tsv" ]] && cat "$SLOTS/$sname/timings.tsv" >> "$TIMINGS"
done

# Tally from the verdict files -- the single accounting path for both modes.
fails=0
infra_fails=0
harness_fails=0
skips=0
missing=""
for scen in "${scenarios[@]}"; do
    sname="$(basename "$scen" .exp)"
    v="$(cat "$SLOTS/$sname/verdict" 2>/dev/null || true)"
    case "$v" in
        PASS)    ;;
        SKIP)    skips=$((skips + 1)) ;;
        INFRA)   infra_fails=$((infra_fails + 1)); fails=$((fails + 1)) ;;
        HARNESS) harness_fails=$((harness_fails + 1)); fails=$((fails + 1)) ;;
        FAIL)    fails=$((fails + 1)) ;;
        # A scenario whose subshell died before writing a verdict (OOM-killed,
        # crashed, killed by hand) has NO result -- and counting "no result" as
        # anything but a failure is the fail-open shape this gate has been bitten
        # by twice (#74, #78). Name it, and fail.
        *)       missing="$missing $sname"; fails=$((fails + 1)) ;;
    esac
done
if [[ -n "$missing" ]]; then
    echo "==> NO VERDICT from:$missing" >&2
    echo "    Their runner exited without recording a result (crash / OOM / external kill)." >&2
    echo "    Counted as failures: an absent result is not a pass." >&2
fi
rm -rf "$SLOTS" 2>/dev/null || true

# --- G-1 summary: where the time actually went --------------------------------
# Printed for every run, pass or fail. A failing run's timings are MORE useful
# than a passing one's (a timeout costs the full budget), so gating this on
# success would hide the expensive cases.
gate_dur=$((SECONDS - gate_t0))
if [[ -s "$TIMINGS" ]]; then
    echo "==> LS-CI timings (default accel=$THYLACINE_ACCEL; per-row accel is what BOOTED; full data: $TIMINGS)"
    # Sort key is emitted as a leading field and cut off after sorting -- BSD awk
    # has no asort(), and sorting the rendered lines directly would drag the
    # trailer rows in among the scenarios.
    awk -F'\t' '
        NR == 1 { next }
        $2 == "TOTAL" { tot[$1] = $5; verdict[$1] = $4; next }
        { att[$1]++; attsec[$1] += $5 }
        END {
            for (s in tot)
                printf "%d\t    %5ds  %-4s  %-24s%s\n", tot[s], tot[s], verdict[s], s,
                       (att[s] > 1 ? "(" att[s] " attempts, " attsec[s] "s in boots)" : "")
        }
    ' "$TIMINGS" | sort -rn -k1,1 | cut -f2-
    awk -F'\t' -v gate="$gate_dur" '
        NR == 1 { next }
        $2 == "TOTAL" { sum += $5; n++ }
        END {
            printf "    %5ds  scenario total (%d scenario(s), mean %ds)\n", sum, n, (n ? sum / n : 0)
            printf "    %5ds  gate wall (scenario loop only)\n", gate
            # Reconciliation, and the reason this line exists: serially these two
            # must be close, because the only work between them is loop
            # bookkeeping. A large positive delta means the timer is missing a
            # span it should be charging -- say that out loud rather than leave a
            # silent discrepancy to be rationalized later.
            #
            # Under G-2 they diverge BY DESIGN: the sum stays ~constant while the
            # wall falls, and that ratio IS the speedup. So the line stays honest
            # in both modes, and in parallel mode it becomes the measurement.
            d = gate - sum
            # OVERLAP, not speedup, and the distinction is load-bearing. Under
            # concurrency each scenario gets SLOWER (measured: 3 TCG scenarios
            # at JOBS=3 went ~190s -> ~400s, since 4 vCPUs x 3 VMs oversubscribe
            # an 8-core host and TCG is CPU-bound), so `sum` here is inflated by
            # exactly the contention the parallelism caused. Dividing it by the
            # wall credits that slowdown as if it were work done, and reports a
            # flattering number: the same run that "overlapped 2.95x" saved only
            # 1.40x against its own serial baseline. Real speedup needs a SERIAL
            # reference this run does not have, so it is not claimed here.
            printf "    %5ds  unaccounted (%s)\n", d, \
                   (d < 0 ? "wall < sum: scenarios OVERLAPPED " sprintf("%.2fx", sum / (gate ? gate : 1)) " -- this is OVERLAP, not speedup; per-scenario times are inflated by contention, so compare the WALL against a serial run" : \
                   (d > 60 ? "WARNING: >60s outside the timed spans -- the instrument is missing work" : \
                             "setup/bookkeeping outside the timed spans; expected small"))
        }
    ' "$TIMINGS"
fi

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
