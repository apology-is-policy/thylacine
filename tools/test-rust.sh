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
# how the real problem stays invisible. A crate in it may have `#[cfg(test)]`
# tests that CANNOT RUN ANYWHERE -- they read as coverage while being
# unrunnable, which is worse than a test that is merely unrun. So the bucket
# reports the STRANDED COUNT per crate, because "cannot be host-tested" and
# "has tests that run nowhere" are different facts and only the second is a
# debt: measured 2026-09-22, four of the five NO-HOST crates declare no tests
# at all, and the blanket note this script used to print about "the NO-HOST
# crates carry tests that cannot run" was false for every one of them.
#
# `libutopia` was the real case and is now FIXED (the `backend` feature split):
# 399 stranded -> 296 running. Asking those tests to compile for the first time
# produced 20 build errors and then 49 failures, of which 41 were one stale test
# helper and 8 are genuine findings, now quarantined with `#[ignore]` reasons.
# That is what a test nobody has ever run is worth knowing about.
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

# name -> source directory, for the stranded-test count below. Read from the
# same `cargo metadata` the enumeration uses, and read UNCONDITIONALLY so an
# explicit-crate run reports the same figures a full run does.
declare -A cratesrc=()
while IFS=$'\t' read -r name manifest; do
    [[ -n "$name" ]] && cratesrc["$name"]="$(dirname "$manifest")/src"
done < <(cd "$USR_DIR" && cargo metadata --no-deps --format-version 1 2>/dev/null \
            | python3 -c 'import json,sys
for p in json.load(sys.stdin)["packages"]:
    print(p["name"] + "\t" + p["manifest_path"])')

# How many `#[test]` blocks a crate's sources declare. For a NO-HOST crate this
# is the number that CANNOT RUN ANYWHERE -- the only figure that says whether
# un-host-testability costs anything here, as opposed to merely being true.
stranded_tests() {
    local dir="${cratesrc[$1]:-}"
    [[ -n "$dir" && -d "$dir" ]] || { echo 0; return; }
    # `|| true` is load-bearing under `set -euo pipefail`: grep exits 1 when it
    # matches NOTHING, which is the ordinary answer here (a crate with no
    # tests), and pipefail would turn that answer into a dead script. Caught by
    # running this before committing it -- the summary printed its header and
    # then nothing at all, which is what a `set -e` death looks like from
    # outside.
    { grep -rho '#\[test\]' "$dir" 2>/dev/null || true; } | wc -l | tr -d ' '
}

echo "==> test-rust: $HOST_TRIPLE, ${#crates[@]} crate(s)"

pass=(); fail=(); nohost=(); notests=(); nolib=()
declare -A counts=()
declare -A ignored=()

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
        # `#[ignore]`d tests are a PASS to cargo and a debt to us. Counted and
        # reported separately so a quarantined failure cannot sit in a green
        # column: an ignore is an IOU, and an IOU nobody prints is forgotten.
        ig="$(awk '/^test result: ok\./ {for(i=1;i<=NF;i++) if($i=="ignored;") print $(i-1)}' "$log" | paste -sd+ - | bc 2>/dev/null || echo 0)"
        ignored["$crate"]="${ig:-0}"
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
for c in "${pass[@]:-}"; do
    [[ -n "$c" ]] || continue
    ig="${ignored[$c]:-0}"
    if (( ig > 0 )); then
        printf '  PASS      %-24s %s test(s), %s IGNORED (quarantined -- grep the reason)\n' "$c" "${counts[$c]}" "$ig"
    else
        printf '  PASS      %-24s %s test(s)\n' "$c" "${counts[$c]}"
    fi
done
for c in "${notests[@]:-}"; do [[ -n "$c" ]] && printf '  NO-TESTS  %s\n' "$c"; done
if (( ${#nolib[@]} > 0 )); then printf '  NO-LIB    %d crate(s) are bin-only (probes, smokes, benches): no --lib to test\n' "${#nolib[@]}"; fi
stranded=0
for c in "${nohost[@]:-}"; do
    [[ -n "$c" ]] || continue
    n="$(stranded_tests "$c")"
    stranded=$(( stranded + n ))
    if (( n > 0 )); then
        printf '  NO-HOST   %-24s cannot host-test -- %s test(s) STRANDED\n' "$c" "$n"
    else
        printf '  NO-HOST   %-24s cannot host-test (declares no tests; nothing stranded)\n' "$c"
    fi
done
for c in "${fail[@]:-}";    do [[ -n "$c" ]] && printf '  FAIL      %s\n' "$c"; done
echo "==================================================="

total=0
ig_total=0
for c in "${pass[@]:-}"; do
    [[ -n "$c" ]] || continue
    total=$(( total + ${counts[$c]} ))
    ig_total=$(( ig_total + ${ignored[$c]:-0} ))
done
echo "test-rust: ${#pass[@]} crate(s) passing, $total test(s); ${#nolib[@]} bin-only; ${#notests[@]} with no tests; ${#nohost[@]} un-host-testable; ${#fail[@]} FAILING"

if (( ig_total > 0 )); then
    echo "test-rust: $ig_total test(s) QUARANTINED with #[ignore]. cargo calls that a pass;"
    echo "           this does not. Each carries a reason naming its finding --"
    echo "           \`grep -rn '#\\[ignore' usr --include='*.rs'\` lists them."
fi

if (( stranded > 0 )); then
    echo "test-rust: NOTE -- $stranded test(s) are STRANDED: declared in a NO-HOST crate,"
    echo "           so they run on no machine at all and their assertions have never"
    echo "           executed. The fix is per crate: make libthyla-rs OPTIONAL behind a"
    echo "           default 'backend' feature (the manual / kaua / lantern pattern)."
    echo "           They are NOT counted as passing."
elif (( ${#nohost[@]} > 0 )); then
    echo "test-rust: the NO-HOST crates declare no tests, so nothing is stranded --"
    echo "           un-host-testable here costs coverage only if tests are added."
fi

(( ${#fail[@]} == 0 )) || exit 1
exit 0
