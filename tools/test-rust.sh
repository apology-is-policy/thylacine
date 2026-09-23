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
#   FAIL      -- its lib tests ran and FAILED, in any of its passes (see
#                FEATURE_PASSES). The only bucket that fails this script.
#   NO-LIB    -- it is `[[bin]]`-only, so there is no `--lib` to test. Every
#                probe, smoke and bench in the tree is one. Measured on the first
#                full run: fifty-odd crates, and counting them as FAIL (which the
#                first draft did) would have made this target permanently red and
#                therefore useless.
#   NO-HOST   -- it CANNOT be host-tested: it depends on `libthyla-rs`
#                unconditionally, whose `_start` inline asm cannot assemble for
#                the host (`.type _start, %function` is ELF, not Mach-O).
#   NO-TESTS  -- it host-builds fine and runs no test.
#
# STRANDED cuts across all five, and it is the figure that matters most. It is
# measured per crate, from the crate: the `#[test]`s its sources declare, minus
# the tests that compiled into a pass here (passed or ignored). A NO-HOST crate
# strands everything it declares. So does a NO-LIB crate, whose tests live in a
# `no_main` binary that cannot link a harness. And a PASS crate strands whatever
# its `--no-default-features` build compiles out -- the syscall half of a
# `backend` split. Those tests run on NO machine: they read as coverage while
# never having executed, which is worse than a test that is merely unrun, so each
# crate prints its count and none of them is counted as passing.
#
# It is derived from the crate, never from its bucket, and that is a correction.
# The first version counted NO-HOST crates only, so the fix that made `libutopia`
# host-testable moved it into PASS and took its remaining 91 stranded tests out of
# the report with it: the summary said "nothing is stranded" while 101 tests in
# three crates ran nowhere (measured 2026-09-23). A debt reported by CATEGORY
# disappears the moment a fix changes the category.
#
# The convention the PASS bucket relies on is the tree's existing one: a lib+bin
# crate puts `libthyla-rs` behind a DEFAULT feature -- `backend` in most crates;
# `guest`, `driver` or `bin` in some -- so `--no-default-features` leaves the pure
# half host-buildable.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USR_DIR="$REPO_ROOT/usr"

# The host triple, from the toolchain itself rather than guessed from uname --
# an aarch64 mac and an x86 mac need different triples and neither is a default.
HOST_TRIPLE="$(rustc -vV | awk '/^host:/ {print $2}')"
[[ -n "$HOST_TRIPLE" ]] || { echo "test-rust: could not read the host triple from rustc -vV" >&2; exit 2; }

# Extra `--features` passes, for a crate whose tests sit on both sides of a
# feature: `cornucopia` tests its `scale` bakes only WITH the feature and their
# absence only WITHOUT it, so any single pass strands one of the pair. Each entry
# is `crate:features`. A test counts as run if it compiled in ANY of its crate's
# passes, matched by name. This table decides only what RUNS -- the stranded
# count is derived regardless -- so a crate missing from it shows up as
# stranded, never as quietly passing.
FEATURE_PASSES=(
    "cornucopia:scale"
)

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

# How many `#[test]` attributes a crate's sources declare. Anchored at the start
# of a line, because a comment that mentions `#[test]` is not a test --
# libhalcyon's theme.rs has one, and an unanchored count called it stranded.
declared_tests() {
    local dir="${cratesrc[$1]:-}"
    [[ -n "$dir" && -d "$dir" ]] || { echo 0; return; }
    # `|| true` is load-bearing under `set -euo pipefail`: grep exits 1 when it
    # matches NOTHING, which is the ordinary answer here (a crate with no
    # tests), and pipefail would turn that answer into a dead script. Caught by
    # running this before committing it -- the summary printed its header and
    # then nothing at all, which is what a `set -e` death looks like from
    # outside.
    { grep -rhE --include='*.rs' '^[[:space:]]*#\[test\]' "$dir" 2>/dev/null || true; } | wc -l | tr -d ' '
}

# The sum of one field ("passed" or "ignored") over a log's `test result: ok.`
# lines -- cargo's own count.
result_sum() {
    local n
    n="$(awk -v f="$2;" '/^test result: ok\./ {for(i=1;i<=NF;i++) if($i==f) print $(i-1)}' "$1" \
            | paste -sd+ - | bc 2>/dev/null || true)"
    echo "${n:-0}"
}

# The tests a log shows compiled into its pass, as `<outcome> <name>` lines.
# libtest prints `test <path>[ - should panic] ... ok|ignored[, <reason>]|FAILED`,
# and an ignored test compiled: it is quarantined, not stranded.
compiled_names() {
    sed -nE 's/^test ([^ ]+)( - should panic)? \.\.\. (ok|ignored)(, .*)?$/\3 \1/p' "$1"
}

# Append a passing log's tests to <names>, but only after proving the parse
# agrees with cargo's own count. Every figure below is derived from these names:
# a parse that silently matched nothing would report every test STRANDED, and
# one that matched only some would report a plausible number that is wrong.
collect_names() {
    local log="$1" names="$2" parsed want
    compiled_names "$log" > "$names.pass"
    parsed="$(wc -l < "$names.pass" | tr -d ' ')"
    want=$(( $(result_sum "$log" passed) + $(result_sum "$log" ignored) ))
    if (( parsed != want )); then
        echo "--- FAIL: parsed $parsed test name(s) from $(basename "$log"), but cargo counted $want ---"
        return 1
    fi
    # A name twice in ONE pass is a test registered twice -- a doubled `#[test]`,
    # which makes the declared count and the compiled count disagree by one and
    # runs the test twice. rustc says so (`duplicate_macro_attributes`), and one
    # sat in libutopia for a day with that warning printed on every build.
    local dups
    dups="$(awk '{print $2}' "$names.pass" | sort | uniq -d)"
    if [[ -n "$dups" ]]; then
        echo "--- FAIL: $(basename "$log") registers a test more than once (a doubled #[test]?):"
        printf '      %s\n' $dups
        return 1
    fi
    cat "$names.pass" >> "$names"
}

# How many compiler warnings a pass reports for the crate's OWN test build.
# Summed from cargo's per-crate footer, so a dependency's warnings are not
# charged to it. Printed, never fatal: a gate that fails on warnings gets
# bypassed, and one that swallows them hid the doubled `#[test]` above.
crate_warnings() {
    local n
    n="$(awk -v c="warning: \`$2\` (lib test) generated " \
            'index($0, c) == 1 {for(i=1;i<=NF;i++) if($i=="generated") print $(i+1)}' "$1" \
            | paste -sd+ - | bc 2>/dev/null || true)"
    echo "${n:-0}"
}

# One `cargo test` pass; extra arguments are passed through (a `--features`).
cargo_test() {
    local crate="$1" log="$2"
    shift 2
    # `--lib` only: a bin target is `no_main` + `no_std` and cannot link a test
    # harness. The caller takes the status with `|| rc=$?`, so a failure is
    # CLASSIFIED below rather than killing the run under errexit -- the whole
    # point is to report every crate, not the first.
    ( cd "$USR_DIR" && cargo test -p "$crate" --lib --no-default-features "$@" \
        --target "$HOST_TRIPLE" 2>&1 ) > "$log"
}

echo "==> test-rust: $HOST_TRIPLE, ${#crates[@]} crate(s)"

pass=(); fail=(); nohost=(); notests=(); nolib=()
declare -A counts=()
declare -A ignored=()
declare -A warnings=()

logdir="$(mktemp -d)"
trap 'rm -rf "$logdir"' EXIT

for crate in "${crates[@]}"; do
    log="$logdir/$crate.log"
    names="$logdir/$crate.names"
    : > "$names"
    rc=0
    cargo_test "$crate" "$log" || rc=$?

    if (( rc == 0 )); then
        ok=1
        collect_names "$log" "$names" || ok=0
        for entry in "${FEATURE_PASSES[@]}"; do
            (( ok )) || break
            [[ "${entry%%:*}" == "$crate" ]] || continue
            flog="$logdir/$crate.features.log"
            frc=0
            cargo_test "$crate" "$flog" --features "${entry#*:}" || frc=$?
            if (( frc != 0 )); then
                echo "--- FAIL $crate (--features ${entry#*:}) ---"
                tail -25 "$flog"
                ok=0
            else
                collect_names "$flog" "$names" || ok=0
            fi
        done
        if (( ! ok )); then
            fail+=("$crate")
            continue
        fi
        # By name, so a test that compiled in two passes counts once.
        # `#[ignore]`d tests are a PASS to cargo and a debt to us. Counted and
        # reported separately so a quarantined failure cannot sit in a green
        # column: an ignore is an IOU, and an IOU nobody prints is forgotten.
        n="$(awk '$1 == "ok" {print $2}' "$names" | sort -u | wc -l | tr -d ' ')"
        ig="$(awk '$1 == "ignored" {print $2}' "$names" | sort -u | wc -l | tr -d ' ')"
        counts["$crate"]="$n"
        ignored["$crate"]="$ig"
        warnings["$crate"]="$(crate_warnings "$log" "$crate")"
        # A crate that runs nothing is not counted as a pass, so a crate whose
        # tests silently vanished cannot read as green.
        if (( n + ig > 0 )); then
            pass+=("$crate")
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

stranded=0
stranded_in=()
note_stranded() {
    (( $2 > 0 )) || return 0
    stranded=$(( stranded + $2 ))
    stranded_in+=("$1")
}

echo
echo "================ test-rust summary ================"
for c in "${pass[@]:-}"; do
    [[ -n "$c" ]] || continue
    n="${counts[$c]}"
    ig="${ignored[$c]:-0}"
    gap=$(( $(declared_tests "$c") - n - ig ))
    line="$(printf '  PASS      %-24s %s test(s)' "$c" "$n")"
    if (( ig > 0 )); then
        line+=", $ig IGNORED (quarantined -- grep the reason)"
    fi
    if (( gap > 0 )); then
        line+=", $gap STRANDED (declared, compiled into no pass)"
        note_stranded "$c" "$gap"
    elif (( gap < 0 )); then
        line+=" -- $(( -gap )) more than its sources declare (a macro-generated test?)"
    fi
    if (( ${warnings[$c]:-0} > 0 )); then
        line+=", ${warnings[$c]} compiler WARNING(s)"
    fi
    echo "$line"
done
for c in "${notests[@]:-}"; do
    [[ -n "$c" ]] || continue
    d="$(declared_tests "$c")"
    if (( d > 0 )); then
        printf '  NO-TESTS  %-24s declares %s test(s) and compiled none -- all STRANDED\n' "$c" "$d"
        note_stranded "$c" "$d"
    else
        printf '  NO-TESTS  %s\n' "$c"
    fi
done
quiet_nolib=0
for c in "${nolib[@]:-}"; do
    [[ -n "$c" ]] || continue
    d="$(declared_tests "$c")"
    if (( d > 0 )); then
        printf '  NO-LIB    %-24s bin-only -- %s test(s) STRANDED in a binary that cannot link a harness\n' "$c" "$d"
        note_stranded "$c" "$d"
    else
        quiet_nolib=$(( quiet_nolib + 1 ))
    fi
done
if (( quiet_nolib > 0 )); then
    printf '  NO-LIB    %d crate(s) are bin-only (probes, smokes, benches) and declare no tests\n' "$quiet_nolib"
fi
for c in "${nohost[@]:-}"; do
    [[ -n "$c" ]] || continue
    d="$(declared_tests "$c")"
    if (( d > 0 )); then
        printf '  NO-HOST   %-24s cannot host-test -- %s test(s) STRANDED\n' "$c" "$d"
        note_stranded "$c" "$d"
    else
        printf '  NO-HOST   %-24s cannot host-test (declares no tests; nothing stranded)\n' "$c"
    fi
done
for c in "${fail[@]:-}"; do [[ -n "$c" ]] && printf '  FAIL      %s\n' "$c"; done
echo "==================================================="

total=0
ig_total=0
w_total=0
w_in=()
for c in "${pass[@]:-}" "${notests[@]:-}"; do
    [[ -n "$c" ]] || continue
    total=$(( total + ${counts[$c]:-0} ))
    ig_total=$(( ig_total + ${ignored[$c]:-0} ))
    if (( ${warnings[$c]:-0} > 0 )); then
        w_total=$(( w_total + ${warnings[$c]} ))
        w_in+=("$c")
    fi
done
echo "test-rust: ${#pass[@]} crate(s) passing, $total test(s); ${#nolib[@]} bin-only; ${#notests[@]} with no tests; ${#nohost[@]} un-host-testable; ${#fail[@]} FAILING"

if (( ig_total > 0 )); then
    echo "test-rust: $ig_total test(s) QUARANTINED with #[ignore]. cargo calls that a pass;"
    echo "           this does not. Each carries a reason naming its finding --"
    echo "           \`grep -rn '#\\[ignore' usr --include='*.rs'\` lists them."
fi

if (( w_total > 0 )); then
    echo "test-rust: $w_total compiler warning(s) in the test builds of: ${w_in[*]}."
    echo "           \`cargo test -p <crate> --lib --no-default-features --no-run\` shows them."
fi

if (( stranded > 0 )); then
    echo "test-rust: NOTE -- $stranded test(s) STRANDED in ${#stranded_in[@]} crate(s): ${stranded_in[*]}."
    echo "           Declared in source but compiled into no pass this script runs, so"
    echo "           their assertions have never executed; NOT counted as passing. The"
    echo "           fix is per crate: move the pure part out from behind the default"
    echo "           feature (PASS), give a bin-only crate a lib (NO-LIB), make"
    echo "           libthyla-rs optional (NO-HOST), or add a FEATURE_PASSES entry."
elif (( ${#fail[@]} == 0 )); then
    echo "test-rust: every #[test] the sources declare compiled into a pass -- nothing is stranded."
fi

(( ${#fail[@]} == 0 )) || exit 1
exit 0
