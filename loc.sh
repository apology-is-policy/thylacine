#!/bin/sh
# What Thylacine is made of. One cloc pass, bucketed, no cache.
#
# WHY THIS REPORTS BUCKETS AND NOT ONE NUMBER: the previous version printed a
# single figure with no label, counting only C/headers/Rust/asm/TLA+/CMake --
# so it silently omitted ~102k lines of Markdown (which this project treats as
# BINDING SCRIPTURE, not commentary), the fork patch series (the durable form of
# our LLVM/Mesa forks -- see #117), and the whole tools/ gate harness. An
# unlabelled 184k next to a remembered 240k reads as "we lost 56k lines." It
# was two different questions, but the number could not say so. Now it does.
#
# The caching is GONE (it was a 10-minute per-$PWD file in /tmp). It bought a
# sub-second saving on a command run by hand a few times a day, and in exchange
# a stale entry could report a tree that no longer existed. A measurement tool
# that can hand back a number it did not just measure is worse than a slow one.
#
# Usage:
#   ./loc.sh          bucketed report
#   ./loc.sh -n       the authored total alone (for scripting)

set -u

# Vendored upstream (third_party) is EXCLUDED and reported separately -- it is
# not ours and counting it flatters the total. Build outputs are excluded
# because their contents vary with build state: `target` is cargo's, and a
# build script emitting .rs into OUT_DIR would otherwise be counted as source.
EXCLUDE_DIRS='build,third_party,target,cmake-build-debug,cmake-build-release,CMakeFiles,.cache,node_modules'

command -v cloc >/dev/null 2>&1 || { echo "loc.sh: cloc not installed" >&2; exit 1; }

# Count the REPO, not the current directory. The old script counted `.`, so
# running it from anywhere but the root silently measured a subtree -- or, from
# outside, something else entirely (it will happily report /tmp). A tool that
# answers "what is this project made of" must not give a different answer
# depending on where you were standing when you asked.
root=$(git rev-parse --show-toplevel 2>/dev/null) || root=""
[ -n "$root" ] && cd "$root" || root=$(pwd)

raw=$(cloc . \
    --exclude-dir="$EXCLUDE_DIRS" \
    --not-match-d='(^|/)\.(git|idea|vscode)$' \
    --csv --quiet 2>/dev/null)

[ -n "$raw" ] || { echo "loc.sh: cloc produced no output" >&2; exit 1; }

vendored=$(cloc third_party \
    --not-match-d='(^|/)\.git$' --csv --quiet 2>/dev/null \
    | awk -F',' '$2 == "SUM" { print $5 }')

printf '%s\n' "$raw" | awk -F',' -v mode="${1:-}" -v vendored="${vendored:-0}" '
# cloc --csv columns: files,language,blank,comment,code
function commas(n,   s, out, len, i) {
    s = sprintf("%d", n); out = ""; len = length(s)
    for (i = 1; i <= len; i++) {
        out = out substr(s, i, 1)
        if ((len - i) % 3 == 0 && i < len) out = out ","
    }
    return out
}
$2 == "" || $2 == "language" || $2 == "SUM" { next }
{
    lang = $2; code = $5 + 0
    # The buckets. Anything unlisted lands in "other" rather than vanishing --
    # a silent drop is how the old script lost the docs.
    if (lang == "C" || lang == "C/C++ Header" || lang == "C++" || \
        lang == "Rust" || lang == "Assembly" || lang == "Linker Script")
        { code_t += code; code_d[lang] = code }
    else if (lang == "TLA+")        { spec_t  += code }
    else if (lang == "Markdown")    { docs_t  += code }
    else if (lang == "diff")        { patch_t += code }
    else if (lang == "Bourne Shell" || lang == "Bourne Again Shell" || \
             lang == "Python" || lang == "Expect" || lang == "CMake" || \
             lang == "make")        { tool_t  += code }
    else                            { other_t += code; other_n[lang] = code }
}
END {
    total = code_t + spec_t + docs_t + patch_t + tool_t + other_t
    if (mode == "-n") { print total; exit }

    printf "  authored  %10s lines\n\n", commas(total)
    printf "    code    %10s   C, headers, C++, Rust, asm, linker scripts\n", commas(code_t)
    printf "    docs    %10s   Markdown (binding scripture)\n", commas(docs_t)
    printf "    patches %10s   fork series (the durable form of our forks)\n", commas(patch_t)
    printf "    tooling %10s   shell, Python, Expect, CMake, make\n", commas(tool_t)
    printf "    specs   %10s   TLA+\n", commas(spec_t)
    if (other_t > 0) {
        line = ""
        for (l in other_n) line = line (line == "" ? "" : ", ") l
        printf "    other   %10s   %s\n", commas(other_t), line
    }
    printf "\n    vendored%10s   third_party/ (NOT ours; excluded above)\n", commas(vendored)
}
'
