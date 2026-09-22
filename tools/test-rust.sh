#!/usr/bin/env bash
# tools/test-rust.sh -- run the userspace Rust crates' HOST unit tests.
#
# WHY THIS EXISTS. Until 2026-09-22 no gate in the tree ran `cargo test` at all:
# `grep -rn "cargo test" Makefile tools/` returned one hit, and it was a comment.
# The Makefile carried 25+ targets -- test, test-fault, verify-kaslr, check-floor,
# test-a72, test-haul-kat, smp-gate, idle-gate -- and not one of them ran a Rust
# unit test. So the largest body of tests in userspace (`manual`, `kaua`, `vt`,
# `libhalcyon`, `cartoon`, `beacon`, `libtapestry`, `lantern`, ...) ran only when
# a human transcribed a command out of a Cargo.toml comment. That is CLAUDE.md's
# own #245 class -- "a checker reachable only by hand rots" -- at the worst
# possible scale, and a green `make test` (the kernel suite) is structurally blind
# to every one of those regressions.
#
# WHAT IT REPORTS, and why five buckets rather than two. A crate lands in
# exactly one, and the distinctions are the whole value -- a script that sorted
# these into "ok" and "not ok" would be red on its first run and ignored by its
# second:
#
#   PASS      -- its lib tests ran and passed.
#   FAIL      -- its lib tests ran and FAILED. The only bucket that fails this
#                script.
#   NO-LIB    -- it is `[[bin]]`-only, so there is no `--lib` to test. Every
#                probe, smoke and bench in the tree is one. Measured on the first
#                full run: fifty-odd crates, and counting them as FAIL (which the
#                first draft did) would have made this target permanently red and
#                therefore useless.
#   NO-HOST   -- it CANNOT be host-tested: it depends on `libthyla-rs`
#                unconditionally, whose `_start` inline asm cannot assemble for
#                the host (`.type _start, %function` is ELF, not Mach-O).
#   NO-TESTS  -- it host-builds fine and simply has no test to run.
#
# NO-HOST is the bucket that matters most, and collapsing it into "skipped" is
# how the real problem stays invisible. A crate in it has `#[cfg(test)]` tests in
# its source that CANNOT RUN ANYWHERE -- they read as coverage while being
# unrunnable, which is worse than a test that is merely unrun. `libutopia` is the
# standing example: its `console::tests::is_raw_command_*` tests look like they
# guard the raw-mode allowlist and cannot execute on this host. Fixing that needs
# a feature split (make the libthyla-rs dependency optional, as `manual`, `kaua`
# and `lantern` do), which is a per-crate change, not something this script can
# paper over. So it NAMES them, every run.
#
# The convention this relies on is the tree's existing one: a lib+bin crate makes
# `libthyla-rs` an OPTIONAL dependency behind a default `backend` feature, so
# `--no-default-features` leaves the pure half host-buildable.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USR_DIR="$REPO_ROOT/usr"

# The host triple, from the toolchain itself rather than guessed from uname --
# an aarch64 mac and an x86 mac need different triples and neither is a default.
HOST_TRIPLE="$(rustc -vV | awk '/^host:/ {print $2}')"
[[ -n "$HOST_TRIPLE" ]] || { echo "test-rust: could not read the host triple from rustc -vV" >&2; exit 2; }

# The crates to try. With no arguments: every workspace member, read from the
# manifest rather than from a hand-kept list here -- a list transcribed into this
# file would go stale the first time someone added a crate, which is the exact
# failure mode this script exists to correct.
crates=()
if (( $# > 0 )); then
    crates=("$@")
else
    # `cargo metadata` names the workspace members authoritatively, including the
    # path-only ones, so a new `usr/<thing>` is picked up by existing.
    while IFS= read -r name; do
        [[ -n "$name" ]] && crates+=("$name")
    done < <(cd "$USR_DIR" && cargo metadata --no-deps --format-version 1 2>/dev/null \
                | python3 -c 'import json,sys; [print(p["name"]) for p in json.load(sys.stdin)["packages"]]' \
                | sort)
fi

(( ${#crates[@]} > 0 )) || { echo "test-rust: no crates to test" >&2; exit 2; }

echo "==> test-rust: $HOST_TRIPLE, ${#crates[@]} crate(s)"

pass=(); fail=(); nohost=(); notests=(); nolib=()
declare -A counts=()

logdir="$(mktemp -d)"
trap 'rm -rf "$logdir"' EXIT

for crate in "${crates[@]}"; do
    log="$logdir/$crate.log"
    # `--lib` only: a bin target is `no_main` + `no_std` and cannot link a test
    # harness. `|| true` so a failure is CLASSIFIED below rather than killing the
    # run under errexit -- the whole point is to report every crate, not the first.
    rc=0
    ( cd "$USR_DIR" && cargo test -p "$crate" --lib --no-default-features \
        --target "$HOST_TRIPLE" 2>&1 ) > "$log" || rc=$?

    if (( rc == 0 )); then
        # A crate with no #[test] reports "running 0 tests". Distinguish it, so a
        # crate whose tests silently vanished is not counted as a pass.
        n="$(awk '/^test result: ok\./ {for(i=1;i<=NF;i++) if($i=="passed;") print $(i-1)}' "$log" | paste -sd+ - | bc 2>/dev/null || echo 0)"
        n="${n:-0}"
        if (( n > 0 )); then
            pass+=("$crate"); counts["$crate"]="$n"
        else
            notests+=("$crate")
        fi
        continue
    fi

    # A crate with no lib target at all -- every probe, smoke and bench in the
    # tree is `[[bin]]`-only. `cargo test --lib` cannot run there and says so,
    # which is not a failure and must not be counted as one. Measured on the
    # first full run: this mis-bucketed FIFTY-ODD crates as FAIL and would have
    # made the target permanently red, i.e. useless, on its first day.
    if grep -q 'no library targets found in package' "$log"; then
        nolib+=("$crate")
        continue
    fi

    # The host-impossible signature: libthyla-rs's ELF-only `_start` asm, or a
    # cargo refusal to build it for this target at all.
    if grep -qE '\.type _start, %function|could not compile `libthyla-rs`' "$log"; then
        nohost+=("$crate")
    else
        fail+=("$crate")
        echo "--- FAIL $crate ---"
        tail -25 "$log"
    fi
done

echo
echo "================ test-rust summary ================"
for c in "${pass[@]:-}";    do [[ -n "$c" ]] && printf '  PASS      %-24s %s test(s)\n' "$c" "${counts[$c]}"; done
for c in "${notests[@]:-}"; do [[ -n "$c" ]] && printf '  NO-TESTS  %s\n' "$c"; done
if (( ${#nolib[@]} > 0 )); then printf '  NO-LIB    %d crate(s) are bin-only (probes, smokes, benches): no --lib to test\n' "${#nolib[@]}"; fi
for c in "${nohost[@]:-}";  do [[ -n "$c" ]] && printf '  NO-HOST   %-24s cannot host-test (unconditional libthyla-rs)\n' "$c"; done
for c in "${fail[@]:-}";    do [[ -n "$c" ]] && printf '  FAIL      %s\n' "$c"; done
echo "==================================================="

total=0
for c in "${pass[@]:-}"; do [[ -n "$c" ]] && total=$(( total + ${counts[$c]} )); done
echo "test-rust: ${#pass[@]} crate(s) passing, $total test(s); ${#nolib[@]} bin-only; ${#notests[@]} with no tests; ${#nohost[@]} un-host-testable; ${#fail[@]} FAILING"

if (( ${#nohost[@]} > 0 )); then
    echo "test-rust: NOTE -- the NO-HOST crates carry #[cfg(test)] tests that cannot run"
    echo "           anywhere. Each needs libthyla-rs made OPTIONAL behind a default"
    echo "           'backend' feature (the manual / kaua / lantern pattern) before its"
    echo "           tests mean anything. They are NOT counted as passing."
fi

(( ${#fail[@]} == 0 )) || exit 1
exit 0
