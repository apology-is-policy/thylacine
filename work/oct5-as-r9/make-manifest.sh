#!/bin/sh
# Regenerate work/oct5-as-r9/INTEGRATION-MANIFEST.md.
#
# WHY A SCRIPT RATHER THAN A HAND-WRITTEN DOCUMENT: the manifest's whole value is
# that its figures are DERIVED -- the tip SHA, the commit count, the excluded
# commit's file list, the test.c delta and the per-category counts all move with
# the branch. A hand-maintained manifest goes stale silently at the next commit
# and then misdescribes the delivery, which is worse than having none. The first
# version of the manifest was generated inline and told its reader to run this
# script, which did not exist; that is the pointer-to-nothing this file closes.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
BASE=${BASE:-5ff62b788}
TIP=$(git rev-parse HEAD)
EXCL=${EXCL:-55cfdb54c}
OUT=work/oct5-as-r9/INTEGRATION-MANIFEST.md
OBLIG=$(dirname "$0")/MERGE-OBLIGATIONS.md
# A loud absence beats a silently obligation-free manifest: an integrator who
# reads "no obligations" and merges is the failure this refusal prevents.
[ -f "$OBLIG" ] || { echo "REFUSING: $OBLIG is missing -- the manifest's merge obligations live there, and a manifest without them reads as 'nothing owed at merge'"; exit 3; }

# The exclusion is load-bearing, so prove the commit exists and still touches
# only what we claim before writing a document that says so.
git cat-file -e "$EXCL" 2>/dev/null || { echo "REFUSING: $EXCL is not a commit here"; exit 2; }
n_excl_files=$(git show --name-only --format= "$EXCL" | grep -c . || true)
[ "$n_excl_files" = 1 ] || { echo "REFUSING: $EXCL touches $n_excl_files files, not 1 -- the manifest's exclusion claim would be wrong"; exit 2; }
git show --name-only --format= "$EXCL" | grep -q '^\.claude/' || { echo "REFUSING: $EXCL does not touch .claude/ -- re-check EXCL"; exit 2; }

{
printf '# AS-R9 integration manifest\n\n'
printf 'Written for astra'"'"'s review item R1 (yip 0161). Every figure below is DERIVED by\n'
printf 'the script that generated this file, not typed from memory: regenerate with\n'
printf '`sh work/oct5-as-r9/make-manifest.sh` after any commit.\n\n'
printf -- '- branch: corona/async-memory\n'
printf -- '- base:   %s  (equal to astra'"'"'s HEAD at review time -- asserted by the\n' "$BASE"
printf -- '  runbook'"'"'s stage 1, which refuses when her HEAD moves off this base)\n'
printf -- '- tip:    %s  (the commit this manifest was GENERATED AGAINST; the\n' "$TIP"
printf -- '  manifest'"'"'s own commit sits above it, so regenerate rather than reading\n'
printf -- '  this line as HEAD)\n'
printf -- '- commits in range: %s\n' "$(git rev-list --count $BASE..$TIP)"
printf -- '- nothing pushed; nothing landed on main\n\n'
printf '## EXCLUDED FROM DELIVERY -- local configuration, not implementation\n\n'
printf 'Commit `%s` is EXCLUDED. It touches exactly one path:\n\n' "$(git rev-parse --short $EXCL)"
git show --name-only --format= "$EXCL" | sed 's/^/    /'
cat <<'EOF'

It is operational hook configuration for this checkout (stale yip hook entries
naming a path the installer moved). Per astra's R1 it is NOT reverted locally --
reverting it would break this checkout's hooks to satisfy a packaging concern --
it is excluded from the integration set instead.

CONSEQUENCE AN INTEGRATOR MUST NOT MISS: the delivery is therefore NOT a
contiguous range. It is "every commit in base..tip EXCEPT that one", so a plain
`git merge` or a range cherry-pick would carry the config change in.

Verification that an assembled integration excludes it -- this must print nothing:

    git diff <base>..<integrated> --name-only -- .claude/

And on this branch, exactly one commit touches that path (so there is nothing
else of this class hiding in the range):

EOF
printf '    $ git log --oneline %s..%s -- .claude/\n' "$BASE" "$TIP"
git log --format='    %h %s' $BASE..$TIP -- .claude/
cat <<'EOF'

## ASTRA'S FOUR PROTECTED WORKING DRAFTS

The four authority/settings drafts live only in astra's working tree and are
UNTOUCHED: this checkout has never contained them, which is checkable rather
than merely stated -- my base IS her HEAD, so anything of hers that is
uncommitted cannot appear in my range by construction.

R1 singles out test.c's added registrations, because BOTH of us append there.
My entire delta to kernel/test/test.c across the whole branch is additive and
consists of nothing but my own four tests -- four forward declarations and four
table rows:

EOF
git diff $BASE..$TIP -- kernel/test/test.c | grep -E '^\+' | grep -v '^+++' | sed 's/^+/    /'
cat <<'EOF'

So her draft registrations and mine are append-only into the same two regions
(the declaration block and the registration table) and do not overlap. The
integration of test.c is additive; the ORDER of the two sets inside those
regions is hers to resolve in her tree, and I have not pre-empted it.

## WHAT IS DELIVERED, BY CATEGORY

EOF
git diff --name-only $BASE..$TIP | grep -v '^\.claude/' | awk -F/ '
  /^kernel\/test\//     {t++; next}
  /^kernel\//           {k++; next}
  /^tools\//            {o++; next}
  /^vault\//            {v++; next}
  /^docs\//             {d++; next}
  /^work\//             {w++; next}
  /^specs\//            {s++; next}
                        {x++}
  END {
    printf "    kernel source (the repair)      : %d file(s)\n", k;
    printf "    kernel tests                   : %d file(s)\n", t;
    printf "    tools/ (SHARED SURFACE)        : %d file(s)\n", o;
    printf "    vault dossiers                 : %d file(s)\n", v;
    printf "    docs                           : %d file(s)\n", d;
    printf "    specs                          : %d file(s)\n", s;
    printf "    work/ evidence + runbooks      : %d file(s)\n", w;
    if (x) printf "    other                          : %d file(s)\n", x;
  }'
cat <<'EOF'

The tools/ files are a shared surface main and aux also bake from. The one
behavioural change there is smp-multiboot.sh's SMP_KEEP_LOGS retention, which is
DEFAULT OFF, so no peer's gate changes unless they opt in.

## NOT DELIVERED, DELIBERATELY

- The green artifact set at work/oct5-as-r9/cpu1-green-pair/ is binary and stays
  UNTRACKED; only its MANIFEST.txt is committed. Nothing in the delivery asks an
  integrator to trust a binary I produced.
- Nothing generated from astra's build/ cache qualifies my source: the clone
  approval was spent and the qualifying run rebuilt from my own tree (provenance
  in work/oct5-as-r9/provenance.log).

## GATING STATE AT DELIVERY -- unchanged by this manifest

- ONE AXIS. The 50/50 clean boots and the D7 50/50 witnesses are Apple M2 + HVF
  only; the thyla-pi A72/KVM leg never ran (operator: mac gate alone, residual
  recorded). This is not two axes.
- The 128 MiB protection is retained; private async, replacement memory
  accounting and clipboard remain NON-DEFAULT and ungated by this work.
- The paused private-owner draft (base c822021a2ea56a452b4cdbe7709e6fa117a7678b)
  stays shut pending astra's review close.
EOF
# The obligations are INCLUDED, never templated: hand-written merge instructions
# inside a generated file are deleted by the next regeneration, and this script
# did exactly that to two of them once.
cat "$OBLIG"
} > "$OUT"
echo "wrote $OUT ($(wc -l < "$OUT" | tr -d ' ') lines) for tip $TIP"
