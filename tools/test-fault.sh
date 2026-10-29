#!/usr/bin/env bash
# tools/test-fault.sh — deliberate-fault verification (P1-I).
#
# Per ROADMAP §4.2 exit criteria: each v1.0 hardening protection should
# fire under deliberate attack. This script builds N kernels, each with
# exactly one fault provoker enabled (THYLACINE_FAULT_TEST=<variant>),
# runs each, and PASSes iff the kernel EXTINCTIONs with the expected
# diagnostic message.
#
# Variants at v1.0:
#   canary_smash    — stack canary check fires →
#                     "EXTINCTION: stack canary mismatch (smashed stack)"
#   wxe_violation   — kernel-image permission fault →
#                     "EXTINCTION: PTE violates W^X (kernel image)"
#   wxe_install_kernel, wxe_install_block, wxe_install_gib, wxe_install_user —
#                     each leaf encoder handed a writable + executable
#                     attribute (I-12, vote 22) →
#                     "EXTINCTION: PTE violates W^X at install (kernel page)",
#                     "(kernel L2 block)", "(kernel L1 block)", "(user page)"
#   bti_fault       — Branch Target Exception (FEAT_BTI required) →
#                     "EXTINCTION: BTI fault (...)" with the ELR named
#                     "  _bti_unguarded_target+0x0": the exception is taken
#                     at the target's first instruction, which a dump naming
#                     a live ELR at a call would print as the symbol before
#                     it
#   kstack_overflow — per-thread kstack guard page fires (P2-Dc) →
#                     "EXTINCTION: kernel stack overflow"
#   secondary_stack_guard — secondary-CPU boot-stack guard page fires
#                     (P5-secondary-stack-guard) →
#                     "EXTINCTION: kernel stack overflow"
#   bootcpu_idle_guard — boot-CPU idle-stack guard page fires →
#                     "EXTINCTION: kernel stack overflow"
#   recursive_kernel_fault — the #806 handler re-entrancy guard fires →
#                     "EXTINCTION: recursive kernel fault"
#   el1_sync_runaway — a fault taken from INSIDE the extinction path drives
#                     the EL1-sync depth to EL1_SYNC_DEPTH_MAX (#246) →
#                     "EXTINCTION: el1-sync recursion"
#   extinction_peer_chatter — every peer CPU writes the console directly while
#                     cpu0 extincts; the banner must reach the wire WHOLE
#                     (the per-byte console gate) →
#                     "EXTINCTION: peer chatter: this banner must reach the wire whole"
#                     matched as the ENTIRE line, not a substring
#   extinction_mmu_off — a W+X leaf encoded inside build_page_tables, with
#                     stage 1 still off: the banner must come from extinction's
#                     MMU-off path, which takes no exclusive →
#                     "EXTINCTION: PTE violates W^X at install (kernel page)"
#                     with "console-claim: skipped with the MMU off", a
#                     symbolized "  extinction+0x" frame and a
#                     "  mmu_enable+0x" frame present and no "console-ring:"
#                     line. mmu_enable's last instruction is the call to
#                     extinction, so its return address is the next
#                     function's first byte: only a dump that names a return
#                     address at its call names that frame mmu_enable (measured
#                     on the 2026-10-28 image; a reshaped mmu_enable whose call
#                     is not last no longer tells the two apart). An exclusive
#                     left on that path is seen only where one to Device
#                     memory never completes: measured under HVF on Apple
#                     silicon and KVM on thyla-pi's A72 (zero bytes); TCG
#                     completes it and passes.
#   vbar_pre_reloc, vbar_pre_mmu — an unaligned load with stage 1 off, so
#                     an Alignment fault on Device memory: in kaslr_init before
#                     the image is relocated, and at the top of
#                     build_page_tables after. Only the vectors start.S
#                     installs at their PA before the first C call can print it
#                     (without them: zero bytes) →
#                     "EXTINCTION: unhandled kernel alignment fault" with
#                     "console-claim: skipped with the MMU off" and the ELR
#                     symbolized as "fault_test_unaligned_load+0x" (a live
#                     frame's PA ELR rebased by the dump).
#   vbar_boot_main  — a write to kernel text at the top of boot_main, before
#                     the banner and before exception_vectors_verify →
#                     "EXTINCTION: PTE violates W^X" with no banner line (the
#                     fault came before it); the banner's text is read from
#                     kernel/main.c, so a reworded banner cannot leave the
#                     check looking for a line that no longer exists.
#   no_entropy      — no provoker in the build: the LAUNCH leaves the kernel
#                     no strong entropy source (TCG -cpu cortex-a72 has no
#                     RNDR; THYLACINE_NO_RNG=1 attaches no virtio-rng), and the
#                     boot must refuse userspace before the suite (vote 18) →
#                     "EXTINCTION: CSPRNG unseeded: ..." with "  tests:" absent
#
# This list is the one place a reader learns what exists, so keep it equal to
# ALL_VARIANTS below. It was short by two (bootcpu_idle_guard,
# recursive_kernel_fault) until 2026-08-18 -- the same rot #245 fixed one
# level up, where a thing exists and its only description does not say so.
#
# Deferred at v1.0 (post-v1.0 hardening pass):
#   pac_mismatch — needs forged-LR inline asm; the resulting fault
#   depends on FEAT_FPAC and implementation-specific poison-bit
#   patterns. Verified instead via hardening.detect_smoke +
#   manual code review.
#
# Usage:
#   tools/test-fault.sh              — run all variants
#   tools/test-fault.sh canary_smash — run a single variant
#   tools/test-fault.sh -v           — verbose log dumps
#
# Compatible with bash 3.2 (macOS default — no associative arrays).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR_BASE="$REPO_ROOT/build"
# fault_test_run() fires AFTER the full in-kernel suite (main.c), and the
# suite's irq-bench long-pole alone is ~10s+ -- so the provoker is not even
# reached inside a 10s budget on a normal/loaded host, producing a false
# "timeout" FAIL. Default to 60s (env-overridable; bump further under heavy
# host load, e.g. taskpolicy -b).
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"

# The boot banner's fixed tail, read from the source that prints it: the
# vbar_boot_main forbid. Read, not restated, because a negative check on a stale
# literal passes forever.
BANNER_TAIL="$(sed -n 's/^ *uart_puts("Thylacine v" THYLACINE_VERSION_STRING "\(.*\)\\n");$/\1/p' "$REPO_ROOT/kernel/main.c")"
if [[ -z "$BANNER_TAIL" || "$BANNER_TAIL" == *$'\n'* ]]; then
    echo "test-fault.sh: cannot read the boot banner's line from kernel/main.c" >&2
    exit 2
fi

# variant → expected extinction substring (case-sensitive prefix match
# against the EXTINCTION: line). Keep the case below in sync with this.
ALL_VARIANTS="canary_smash wxe_violation wxe_install_kernel wxe_install_block wxe_install_gib wxe_install_user bti_fault kstack_overflow secondary_stack_guard bootcpu_idle_guard recursive_kernel_fault el1_sync_runaway extinction_peer_chatter extinction_mmu_off vbar_pre_reloc vbar_pre_mmu vbar_boot_main no_entropy"

# Launcher settings a variant boots under, word-split into run-vm.sh's
# environment. Empty = the caller's own.
launch_env_for() {
    case "$1" in
        no_entropy) echo "THYLACINE_ACCEL=tcg THYLACINE_CPU=cortex-a72 THYLACINE_NO_RNG=1" ;;
        *)          echo "" ;;
    esac
}

# A substring that must NOT appear for a variant to PASS. Empty = no such
# requirement. Paired with expected_for: "the right line appeared" and "the
# wrong line did not" are SEPARATE claims and need separate checks.
# Variants whose expected line must be the WHOLE line: a peer byte before the
# prefix or inside the message is the failure the variant exists to catch, and a
# substring match would pass a banner torn after its last expected byte.
whole_line_for() {
    case "$1" in
        extinction_peer_chatter) echo 1 ;;
        *)                       echo "" ;;
    esac
}

# Substrings, one per line, that must ALSO appear for a variant to PASS.
# Empty = no such requirement. The expected line names the extinction, not the
# path that printed it: where the variant exists to prove the path, these do.
require_for() {
    case "$1" in
        # The MMU-off claim's own report, and a pre-MMU frame symbolized: before
        # the MMU every code address is a PA, which only the dump's PA rebase
        # turns into a name. mmu_enable ends in its call to extinction, so its
        # frame is named only when a return address is named at its call.
        extinction_mmu_off) printf '%s\n' "console-claim: skipped with the MMU off" \
                                           "  extinction+0x" "  mmu_enable+0x" ;;
        # The same report, and the faulting PC named: a live frame's ELR taken
        # before the MMU is a PA, which only the dump's rebase turns into one.
        vbar_pre_reloc|vbar_pre_mmu)
                            printf '%s\n' "console-claim: skipped with the MMU off" \
                                           "  fault_test_unaligned_load+0x" ;;
        # A live ELR names the instruction it took, here a function's first:
        # named at a call instead, it would read as the symbol before it.
        bti_fault)          echo "  _bti_unguarded_target+0x0" ;;
        *)                  echo "" ;;
    esac
}

forbid_for() {
    case "$1" in
        el1_sync_runaway) echo "console-ring: NOT held" ;;
        extinction_peer_chatter) echo "console-claim: taken with irqs on" ;;
        # The ring is not claimed before the MMU, so no ring report is true.
        extinction_mmu_off) echo "console-ring:" ;;
        # The banner: a fault after it could have come after the vectors'
        # verify too, which would prove nothing about start.S's install.
        vbar_boot_main)   echo "$BANNER_TAIL" ;;
        # The suite's first line: a refusal printed after it came too late.
        no_entropy)       echo "  tests:" ;;
        # The provoker's own line: the encode returned.
        wxe_install_kernel|wxe_install_block|wxe_install_gib|wxe_install_user)
                          echo "FAIL: a W+X leaf was encoded" ;;
        *)                echo "" ;;
    esac
}

expected_for() {
    case "$1" in
        canary_smash)    echo "EXTINCTION: stack canary mismatch" ;;
        wxe_violation)   echo "EXTINCTION: PTE violates W^X" ;;
        wxe_install_kernel) echo "EXTINCTION: PTE violates W^X at install (kernel page)" ;;
        wxe_install_block)  echo "EXTINCTION: PTE violates W^X at install (kernel L2 block)" ;;
        wxe_install_gib)    echo "EXTINCTION: PTE violates W^X at install (kernel L1 block)" ;;
        wxe_install_user)   echo "EXTINCTION: PTE violates W^X at install (user page)" ;;
        bti_fault)       echo "EXTINCTION: BTI fault" ;;
        kstack_overflow) echo "EXTINCTION: kernel stack overflow" ;;
        secondary_stack_guard) echo "EXTINCTION: kernel stack overflow" ;;
        bootcpu_idle_guard)    echo "EXTINCTION: kernel stack overflow" ;;
        recursive_kernel_fault) echo "EXTINCTION: recursive kernel fault" ;;
        el1_sync_runaway)      echo "EXTINCTION: el1-sync recursion" ;;
        extinction_peer_chatter) echo "EXTINCTION: peer chatter: this banner must reach the wire whole" ;;
        extinction_mmu_off) echo "EXTINCTION: PTE violates W^X at install (kernel page)" ;;
        vbar_pre_reloc|vbar_pre_mmu) echo "EXTINCTION: unhandled kernel alignment fault" ;;
        vbar_boot_main)  echo "EXTINCTION: PTE violates W^X" ;;
        no_entropy)      echo "EXTINCTION: CSPRNG unseeded: neither RNDR nor a virtio-mmio virtio-rng gave the boot reseed entropy; refusing to start userspace" ;;
        # ROUND F6: the variant REACHES cons_tx_claim_for_dump's
        # already-ours arm but could not FAIL if it were wrong -- delete that
        # arm and the re-entrant claim burns its bound, returns false, and the
        # banner still prints ("torn beats silent"), so the expected string is
        # present and the variant passes ~20 ms slower. A control must prove
        # discrimination, not detection. `forbid_for` closes it: on this
        # variant the miss line must be ABSENT.
        *)               echo "" ;;
    esac
}

verbose=0
selected=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) verbose=1; shift ;;
        -h|--help)
            echo "Usage: $0 [-v] [variant...]"
            echo "Variants: $ALL_VARIANTS"
            exit 0
            ;;
        # DERIVED from ALL_VARIANTS, never a second hand-written list. There
        # were FOUR enumerations of this set (ALL_VARIANTS, expected_for, this
        # arm, and --help); adding a variant updated two of them and the other
        # two silently refused it. A set with four independent spellings has no
        # spelling anything can be checked against.
        *)
            if [[ " $ALL_VARIANTS " == *" $1 "* ]]; then
                selected="$selected $1"; shift
            else
                echo "Unknown arg: $1" >&2
                echo "Variants: $ALL_VARIANTS" >&2
                exit 2
            fi
            ;;
    esac
done

if [[ -z "$selected" ]]; then
    selected="$ALL_VARIANTS"
fi

pass=0
fail=0

for variant in $selected; do
    expect="$(expected_for "$variant")"
    if [[ -z "$expect" ]]; then
        echo "==> [$variant] no expected diagnostic (script bug)" >&2
        exit 2
    fi
    build_dir="$BUILD_DIR_BASE/kernel-fault-$variant"
    log_file="$BUILD_DIR_BASE/test-fault-$variant.log"

    echo "==> [$variant] building..."
    # #101: capture rather than discard. --build-dir redirects only KERNEL_BUILD,
    # NOT $BUILD_DIR, so `fixtures` stays build/fixtures -- every variant here
    # re-mints the SHARED pool.img from the ambient environment, N times per run,
    # for a harness that never reads the pool at all. That is invisible waste at
    # best and, with a clade toolchain staged but the flag unset, a silent
    # destruction of a clade-baked pool (#101). Surfacing build.sh's bake config
    # line makes both visible without restoring the full build spew.
    bake_log="$BUILD_DIR_BASE/test-fault-$variant-build.log"
    # THYLACINE_FAULT_BUILD_ARGS: extra build.sh arguments, word-split (a
    # harness that must reach a provoker past a suite its mutant fails passes
    # "--config default --set TESTS=n").
    read -r -a fault_build_args <<< "${THYLACINE_FAULT_BUILD_ARGS:-}"
    if ! "$REPO_ROOT/tools/build.sh" kernel \
        --build-dir="$build_dir" \
        ${fault_build_args[@]+"${fault_build_args[@]}"} \
        -- -DTHYLACINE_FAULT_TEST="$variant" > "$bake_log" 2>&1; then
        echo "==> [$variant] BUILD FAILED -- tail of $bake_log:" >&2
        tail -20 "$bake_log" >&2
        exit 2
    fi
    grep -E '^==> (populate pool: bake config|WARNING: a clade)' "$bake_log" || true

    echo "==> [$variant] booting (expecting: $expect)..."
    read -r -a launch_env <<< "$(launch_env_for "$variant")"
    env ${launch_env[@]+"${launch_env[@]}"} \
        THYLACINE_BUILD_DIR="$build_dir" "$REPO_ROOT/tools/run-vm.sh" --no-share \
        < /dev/null > "$log_file" 2>&1 &
    pid=$!

    # Wait until either the expected extinction shows up, an
    # unexpected line shows up, or QEMU exits.
    deadline=$(( $(date +%s) + BOOT_TIMEOUT ))
    result="timeout"
    while [[ $(date +%s) -lt $deadline ]]; do
        if [[ -f "$log_file" ]] && grep -qF "$expect" "$log_file"; then
            result="pass"
            # The whole-line and forbid checks below read what the kernel
            # prints AFTER the banner (the dump, then the report lines): let
            # the log stop growing before QEMU is killed, or a late line is
            # judged absent from a log cut short. Bounded at ~5 s.
            prev=-1
            for _ in $(seq 1 25); do
                size=$(wc -c < "$log_file" | tr -d ' ')
                [[ "$size" == "$prev" ]] && break
                prev=$size
                sleep 0.2
            done
            break
        fi
        # If we see "Thylacine boot OK" without the expected fault, the
        # provoker didn't fire — that's a FAIL: the protection should
        # have triggered before reaching the success line.
        if [[ -f "$log_file" ]] && grep -q "^Thylacine boot OK" "$log_file"; then
            result="provoker_silent"
            break
        fi
        if [[ -f "$log_file" ]] && grep -q "^EXTINCTION:" "$log_file"; then
            # Race: UART prints character-by-character; we may be
            # mid-line. Wait briefly for the line to complete, then
            # re-check $expect before declaring wrong_extinction.
            sleep 1
            if grep -qF "$expect" "$log_file"; then
                result="pass"
            else
                result="wrong_extinction"
            fi
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            # QEMU exited; final check.
            if grep -qF "$expect" "$log_file"; then
                result="pass"
            elif grep -q "^EXTINCTION:" "$log_file"; then
                result="wrong_extinction"
            else
                result="qemu_exit"
            fi
            break
        fi
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true

    case "$result" in
        pass)
            # ROUND F6: the expected line appearing and the forbidden line
            # being absent are SEPARATE claims. Check the second here, on the
            # arm that otherwise reports success -- a variant that reached its
            # banner the slow way (a broken re-entrant claim burning its bound
            # and emitting unserialized) prints the expected string too.
            forbid="$(forbid_for "$variant")"
            missing=""
            while IFS= read -r req; do
                [[ -n "$req" ]] && ! grep -qF -- "$req" "$log_file" && missing="$req" && break
            done <<< "$(require_for "$variant")"
            if [[ -n "$(whole_line_for "$variant")" ]] \
               && ! tr -d '\r' < "$log_file" | grep -qxF "$expect"; then
                fail=$((fail + 1))
                echo "==> [$variant] FAIL: '$expect' is not a whole line -- the banner was torn:"
                grep -aF "$expect" "$log_file" | head -2 | sed 's/^/    /'
            elif [[ -n "$missing" ]]; then
                fail=$((fail + 1))
                echo "==> [$variant] FAIL: saw '$expect' but not '$missing'"
                echo "    the banner printed, but not by the path the variant exists to prove"
            elif [[ -n "$forbid" ]] && grep -qF -- "$forbid" "$log_file"; then
                fail=$((fail + 1))
                echo "==> [$variant] FAIL: saw '$expect' but ALSO '$forbid'"
                echo "    the banner printed, but not the way the variant exists to prove:"
                grep -F -- "$forbid" "$log_file" | head -2 | sed 's/^/    /'
            else
                pass=$((pass + 1))
                required="$(require_for "$variant" | sed '/^$/d' | sed "s/.*/'&'/" | paste -sd, -)"
                echo "==> [$variant] PASS (saw '$expect'${required:+ and $required}${forbid:+; '"$forbid"' absent})"
                (( verbose )) && grep "^EXTINCTION:" "$log_file" | head -3
            fi
            ;;
        provoker_silent)
            fail=$((fail + 1))
            echo "==> [$variant] FAIL: provoker silent — kernel reached 'Thylacine boot OK' without firing the protection"
            ;;
        wrong_extinction)
            fail=$((fail + 1))
            echo "==> [$variant] FAIL: wrong extinction message:"
            grep "^EXTINCTION:" "$log_file" | head -3
            ;;
        qemu_exit)
            fail=$((fail + 1))
            echo "==> [$variant] FAIL: QEMU exited unexpectedly. Log tail:"
            tail -8 "$log_file"
            ;;
        timeout)
            fail=$((fail + 1))
            echo "==> [$variant] FAIL: timeout (${BOOT_TIMEOUT}s) — neither expected fault nor unrelated termination."
            tail -8 "$log_file"
            ;;
    esac
done

echo
echo "==> Summary: $pass PASS, $fail FAIL out of $((pass + fail))"
[[ $fail -eq 0 ]] || exit 1
exit 0
