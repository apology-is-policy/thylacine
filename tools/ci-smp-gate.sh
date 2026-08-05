#!/usr/bin/env bash
# tools/ci-smp-gate.sh -- the SMP soundness CI gate (#865).
#
# Single boots lie. The #788/#806/#860 SMP context-corruption races are
# layout-/timing-sensitive and pass a single boot most of the time, so a
# one-shot tools/test.sh is NOT a soundness gate -- it is the thing that
# masked #860 for weeks (SMP-REVIEW-FINDINGS section 7d: "the process fix
# that masked everything"). This driver is the honest gate: it multi-boots
# (N>=10) the kernel under the configurations that actually exercise the
# race -- the two CPU counts AND the UBSan build (UBSan-smp4 is the #860
# amplifier: on the broken bringup it crashed ~33-43% of boots, 0% on a
# single lucky boot) -- and FAILS if ANY boot shows a ctx/stack-corruption
# signature.
#
# It composes tools/smp-multiboot.sh (which re-runs tools/test.sh N times
# against ONE built kernel and classifies each failure as CORRUPTION vs
# EXTERNAL-KILL vs inject-miss vs benign host-TIMING vs OTHER). This driver
# builds each needed kernel ONCE up front (so the N boots reuse the ELF
# instead of rebuilding per boot), runs every requested config, and
# aggregates.
#
# Usage:
#   tools/ci-smp-gate.sh                 # full matrix, N=10
#   SMP_GATE_N=15 tools/ci-smp-gate.sh   # full matrix, N=15
#   SMP_GATE_CONFIGS="default-smp4 ubsan-smp4" tools/ci-smp-gate.sh
#                                        # subset (e.g. a fast pre-push check)
#
# Configs (label / cpus / sanitizer / per-boot BOOT_TIMEOUT seconds):
#   default-smp4   4   --         300    the canonical CI default
#   default-smp8   8   --         300    max-CPU concurrency
#   ubsan-smp4     4   undefined  420    the #860 amplifier (most sensitive)
#   ubsan-smp8     8   undefined  420    amplifier + max concurrency
#
# Timeouts are sized for the go4c-ENFORCING boot (#362): every boot runs two
# real on-device go builds (~65-70 s of the boot) plus the suite + fsbench, so
# a default boot is ~95-110 s -- the pre-go4c 90/120 s budgets timed out
# HEALTHY boots (a 10/10 false-OTHER band). A timeout is a ceiling, not a
# sleep: fast boots exit early, only a genuine wedge waits it out.
#
# UBSan boots are ~150-300 s each (vs ~95-110 s default), so a full N=10
# matrix is intentionally heavy (~tens of minutes to ~hours of wall clock).
# That cost IS the gate -- per "complexity is permitted only where it is
# verified," the SMP soundness claim is only as good as the multi-boot
# evidence behind it. For a quick pre-push check use SMP_GATE_CONFIGS to run
# the amplifier subset.
#
# Exit 0 iff every requested config reports 0 CORRUPTION, 0 EXTERNAL-KILL,
# and 0 OTHER across all N boots. Benign host-TIMING failures are reported
# but do not fail the gate (DEBUGGING-PLAYBOOK section 6: do not conflate
# host-fragility with scheduler corruption). An EXTERNAL-KILL (#88) is not a
# guest defect either, but it FAILS loudly under its honest label so outside
# interference with the VM is seen, never absorbed.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
N="${SMP_GATE_N:-10}"

# label  cpus  sanitizer  boot_timeout
DEFAULT_MATRIX=(
    "default-smp4 4 -       300"
    "default-smp8 8 -       300"
    "ubsan-smp4   4 undefined 420"
    "ubsan-smp8   8 undefined 420"
)

# Allow selecting a subset by label. SMP_GATE_CONFIGS is a space-separated
# list of labels; if set, only matching rows run.
declare -a MATRIX=()
if [[ -n "${SMP_GATE_CONFIGS:-}" ]]; then
    for want in $SMP_GATE_CONFIGS; do
        found=0
        for row in "${DEFAULT_MATRIX[@]}"; do
            if [[ "${row%% *}" == "$want" ]]; then MATRIX+=("$row"); found=1; fi
        done
        [[ $found -eq 0 ]] && { echo "ci-smp-gate: unknown config '$want'" >&2; exit 2; }
    done
else
    MATRIX=("${DEFAULT_MATRIX[@]}")
fi

# Build each sanitizer flavor needed by the selected matrix exactly once.
need_default=0; need_ubsan=0
for row in "${MATRIX[@]}"; do
    read -r _label _cpus san _to <<<"$row"
    if [[ "$san" == "-" ]]; then need_default=1; else need_ubsan=1; fi
done

# #101: build.sh RE-BAKES the pool from the ambient environment, and
# THYLACINE_BAKE_CLADE defaults to 0. A bare `build.sh kernel` here therefore
# produced a pool with NO /clade even when the tree had a staged device
# toolchain -- so a CL-6 gate ran 40 boots in which clangd was simply absent
# (lsp-probe skips an absent server by design) and still reported 40/40 PASS.
# A gate that cannot see the feature reports success identically to one that
# verified it, which is the #72 / #74 failure class.
#
# So: default the bake ON when the tree is configured for it, and ALWAYS print
# what was chosen. The caller can still force either way by exporting the var.
if [[ -z "${THYLACINE_BAKE_CLADE:-}" && -d "$REPO_ROOT/build/clade/stage/bin" ]]; then
    export THYLACINE_BAKE_CLADE=1
fi
echo "== ci-smp-gate: bake config -- CLADE=${THYLACINE_BAKE_CLADE:-0} GOROOT=${THYLACINE_BAKE_GOROOT:-1} =="

echo "== ci-smp-gate: building kernels (default=$need_default ubsan=$need_ubsan) =="
if [[ $need_default -eq 1 ]]; then
    "$REPO_ROOT/tools/build.sh" kernel || { echo "ci-smp-gate: default kernel build FAILED" >&2; exit 1; }
fi
if [[ $need_ubsan -eq 1 ]]; then
    "$REPO_ROOT/tools/build.sh" kernel --sanitize=undefined || { echo "ci-smp-gate: ubsan kernel build FAILED" >&2; exit 1; }
fi

# Prove the bake actually happened rather than trusting the flag. A staged
# toolchain that did not reach the pool is exactly the silent-miss above, and
# the pool is what the boots read -- so verify the artifact, not the intent.
if [[ "${THYLACINE_BAKE_CLADE:-0}" == "1" ]]; then
    if [[ ! -f "$REPO_ROOT/build/fixtures/pool.img" ]]; then
        echo "ci-smp-gate: BAKE_CLADE=1 but no pool.img was produced" >&2; exit 1
    fi
    # 5120M (clade+goroot) / 3072M (clade alone); a goroot-only pool is 2560M.
    pool_bytes=$(wc -c < "$REPO_ROOT/build/fixtures/pool.img" | tr -d ' ')
    if [[ "$pool_bytes" -lt $((3072 * 1024 * 1024)) ]]; then
        echo "ci-smp-gate: BAKE_CLADE=1 but pool.img is only $pool_bytes bytes --" >&2
        echo "  too small to hold /clade, so the boots would run WITHOUT clangd." >&2
        exit 1
    fi
    echo "== ci-smp-gate: clade bake verified (pool.img $pool_bytes bytes) =="
fi

echo "== ci-smp-gate: multi-boot matrix (N=$N per config) =="
fail=0
declare -a SUMMARY=()
for row in "${MATRIX[@]}"; do
    read -r label cpus san to <<<"$row"
    sanarg=""; [[ "$san" != "-" ]] && sanarg="$san"
    echo
    echo "-- $label: cpus=$cpus sanitizer=${sanarg:-none} boot_timeout=${to}s N=$N --"
    if BOOT_TIMEOUT="$to" "$REPO_ROOT/tools/smp-multiboot.sh" "$label" "$cpus" "$N" "$sanarg"; then
        SUMMARY+=("PASS  $label")
    else
        SUMMARY+=("FAIL  $label")
        fail=1
    fi
done

echo
echo "================ ci-smp-gate summary (N=$N) ================"
for line in "${SUMMARY[@]}"; do echo "  $line"; done
echo "==========================================================="
if [[ $fail -eq 0 ]]; then
    echo "ci-smp-gate: PASS -- 0 corruption across all configs"
else
    echo "ci-smp-gate: FAIL -- see build/multiboot-fails/ for the captured logs" >&2
fi
exit $fail
