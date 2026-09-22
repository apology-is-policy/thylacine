#!/usr/bin/env bash
# tools/test-forage.sh -- discrimination tests for the build-input collector
# (tools/forage.sh) + the manifest reader. Every case is a CONTROL that fails
# without the behavior it checks. No network, no git, no GCP: the TOML-subset
# parser + `present` probes are tested against the REAL manifest and a temp
# FIXTURE manifest, and every gather runs under FORAGE_DRY=1 (touch nothing) or
# resolves to an instruction. ~1 s.
#
# FORAGE is overridable so each control can be PROVEN by pointing it at a
# sabotaged copy (the fail-without-fix check).
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FORAGE="${FORAGE:-$REPO_ROOT/tools/forage.sh}"
REAL_MANIFEST="$REPO_ROOT/tools/build-manifest.toml"

pass=0; fail=0
ok()  { printf '  PASS  %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL  %s\n' "$1"; fail=$((fail+1)); }
assert_eq()    { if [[ "$1" == "$2" ]]; then ok "$3"; else bad "$3 (got '$1' want '$2')"; fi; }
assert_grep()  { if printf '%s' "$1" | grep -qE "$2"; then ok "$3"; else bad "$3 (missing /$2/)"; fi; }
assert_ngrep() { if printf '%s' "$1" | grep -qE "$2"; then bad "$3 (unexpected /$2/)"; else ok "$3"; fi; }

# source forage.sh for direct parser/probe access (its dispatch is source-guarded)
MANIFEST="$REAL_MANIFEST"
# shellcheck disable=SC1090
. "$FORAGE"
MANIFEST="$REAL_MANIFEST"          # source re-derived it from env; pin to the real one

echo "== test-forage =="

# --- A. the TOML-subset reader (real manifest) -------------------------------
assert_eq "$(manifest_get fork.go commit)"    "4bb69d2" "A1 parser: quoted value, quotes stripped"
assert_eq "$(manifest_get meta schema)"        "1"      "A2 parser: bareword value"
assert_eq "$(manifest_get fork.go nosuchkey)"  ""       "A3 parser: absent key -> empty"
assert_eq "$(manifest_get cache.alpine sha256)" "f31202c4070c4ef7de9e157e1bd01cb4da3a2150035d74ea5372c5e86f1efac1" "A4 parser: full 64-char hash"
# a key must be read from its OWN section, not a namesake in another
assert_eq "$(manifest_get fork.ambush commit)" "563bae9" "A5 parser: section-scoped (ambush != go)"

secs="$(manifest_sections fork.)"
assert_grep  "$secs" "fork.go"      "A6 enum: fork. includes fork.go"
assert_grep  "$secs" "fork.stratum" "A6 enum: fork. includes fork.stratum"
assert_ngrep "$secs" "cache"        "A7 enum: fork. excludes cache.*"
assert_ngrep "$secs" "meta"         "A7 enum: fork. excludes meta"

nsecs="$(manifest_sections network.)"
assert_grep "$nsecs" "network.duke3d"     "A8 enum: network. includes duke3d (DX-8)"
assert_grep "$nsecs" "network.tombraider" "A8 enum: network. includes tombraider (DX-8)"

# A9: the network pins are TWO COPIES OF ONE TRUTH -- build.sh fetches with its
# own literal sha256/url and the manifest records them for forage + the
# installer. A pin bumped in one place and not the other is exactly the drift
# this catches: every hash-shaped value + url under network.* must appear
# verbatim in build.sh. (Sabotage: edit one hex digit in the manifest -> FAIL.)
for nsec in $nsecs; do
    for nkey in sha256 pak_sha256 grp_sha256 exe_sha256 url; do
        nval="$(manifest_get "$nsec" "$nkey")"
        [[ -n "$nval" ]] || continue
        if grep -qF -- "$nval" "$REPO_ROOT/tools/build.sh"; then
            ok "A9 pin: $nsec.$nkey matches build.sh"
        else
            bad "A9 pin: $nsec.$nkey NOT in build.sh ($nval) -- manifest/build.sh drift"
        fi
    done
done

# A10: the Boosty pins are the same two-copies-of-one-truth shape. build.sh
# refuses a checkout that is not WEBKIT_PIN + the series and a tarball that is
# not ICU_SHA256; forage gathers by the manifest's values. (Sabotage: edit one
# hex digit of either manifest value -> FAIL.)
for pin in "source.webkit commit" "cache.icu4c sha256" "cache.icu4c file"; do
    set -- $pin
    pval="$(manifest_get "$1" "$2")"
    if [[ -n "$pval" ]] && grep -qF -- "\"$pval\"" "$REPO_ROOT/tools/build.sh"; then
        ok "A10 pin: $1.$2 matches build.sh"
    else
        bad "A10 pin: $1.$2 NOT in build.sh (${pval:-empty}) -- manifest/build.sh drift"
    fi
done
assert_grep "$(manifest_sections source.)" "source.webkit" "A10 enum: source. includes webkit"

# --- B. status + gather via a fixture manifest (isolated to a temp root) ------
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
FIX="$TMP/manifest.toml"
cat > "$FIX" <<EOF
[meta]
schema = "1"

[fork.go]
path = "$TMP/nogo"
probe = "bin/go"
repo = "https://example.invalid/go.git"
commit = "deadbeef"
branch = "master"
feeds = "go feed"
forageable = "clone"

[fork.gopls]
path = "$TMP/nogopls"
probe = ".git"
commit = "cafef00d"
feeds = "gopls feed"
forageable = "manual"

[fork.llvm]
path = "$TMP/nollvm"
feeds = "llvm feed"
forageable = "remote-source"

[cache.alpine]
file = "alpine.tar.gz"
dir = "cache"
url = "http://127.0.0.1:9/alpine.tar.gz"
sha256 = "aaaa"
feeds = "alpine feed"
forageable = "download"

[cache.busybox]
file = "busybox.apk"
dir = "cache"
url = "http://127.0.0.1:9/busybox.apk"
sha256 = "bbbb"
feeds = "busybox feed"
forageable = "download"

[network.game]
file = "game.zip"
dir = "game"
url = "http://127.0.0.1:9/game.zip"
sha256 = "cccc"
feeds = "game feed"
forageable = "auto-at-build"

[remote.clade_llvm]
path = "clade/llvm-build"
probe = "clade/stage/bin"
pull = "echo PULLING-CLADE"
rebuild = "tools/clade-gcp-build.sh"
feeds = "clade feed"
forageable = "remote-pull"
EOF

runf() { FORAGE_ROOT="$TMP" MANIFEST="$FIX" "$FORAGE" "$@"; }

# B1/B2: present() reads the filesystem -- the SAME input flips on file presence
out="$(runf status 2>&1)"
assert_grep "$out" "cache.busybox[[:space:]]+ABSENT"  "B1 status: absent cache -> ABSENT"
mkdir -p "$TMP/cache"; : > "$TMP/cache/busybox.apk"
out="$(runf status 2>&1)"
assert_grep "$out" "cache.busybox[[:space:]]+present" "B2 status: present cache -> present (flips on the file)"

# B3/B4: dry-run download names the url; the `alpine` alias gathers BOTH inputs
out="$(FORAGE_DRY=1 runf alpine 2>&1)"
assert_grep "$out" "\[dry-run\] cache.alpine: curl http://127.0.0.1:9/alpine.tar.gz" "B3 dry-run download names the url + sha"
assert_grep "$out" "cache.busybox" "B4 alias: 'alpine' target also gathers busybox"

# B5: dry-run remote-pull (absent probe) names the delegated pull command
out="$(FORAGE_DRY=1 runf clade 2>&1)"
assert_grep "$out" "\[dry-run\] remote.clade_llvm: echo PULLING-CLADE" "B5 dry-run remote-pull names the pull cmd"

# B6: dry-run clone (absent fork) names the repo
out="$(FORAGE_DRY=1 runf go 2>&1)"
assert_grep "$out" "\[dry-run\] fork.go: git clone https://example.invalid/go.git" "B6 dry-run clone names the repo"

# B7/B8: non-automatable inputs INSTRUCT (do not silently no-op)
assert_grep "$(runf gopls 2>&1)" "no public source"                "B7 instruct: manual names the remedy"
assert_grep "$(runf llvm 2>&1)"  "source pin for a remotely-built" "B8 instruct: remote-source explains"

# B8b: an auto-at-build input is always "present" to forage (build.sh owns the
# fetch) and its gather INSTRUCTS rather than downloading (never a silent no-op,
# never a fetch forage does not own)
out="$(runf status 2>&1)"
assert_grep "$out" "network.game[[:space:]]+present" "B8b status: auto-at-build reports present (build.sh fetches it)"
out="$(FORAGE_DRY=1 runf network.game 2>&1 || true)"
assert_grep "$out" "fetched automatically at build time" "B8b gather: auto-at-build instructs"
if [[ -f "$TMP/game/game.zip" ]]; then bad "B8b auto-at-build was downloaded by forage"; else ok "B8b auto-at-build not downloaded by forage"; fi

# B9: unknown target is rejected
if runf nosuch >/dev/null 2>&1; then bad "B9 unknown target should exit nonzero"; else ok "B9 unknown target rejected"; fi

# B10: dry-run touched nothing (alpine was never downloaded)
if [[ -f "$TMP/cache/alpine.tar.gz" ]]; then bad "B10 dry-run created a file"; else ok "B10 dry-run touched nothing"; fi

# --- C. clone-sparse: a pinned UPSTREAM + an in-repo patch series --------------
# A local upstream stands in for the network: two dirs (one inside the cone, one
# outside), a tag, and a one-patch series made with format-patch.
UP="$TMP/upstream"; SER="$TMP/series"; G="git -c user.name=t -c user.email=t@t -c init.defaultBranch=main"
mkdir -p "$UP/keep" "$UP/skip" "$SER/p"
$G -C "$UP" init -q && echo one > "$UP/keep/a.txt" && echo x > "$UP/skip/b.txt"
$G -C "$UP" add -A && $G -C "$UP" commit -qm base && $G -C "$UP" tag v1
PINSHA="$($G -C "$UP" rev-parse HEAD)"
$G clone -q "$UP" "$TMP/mk" && echo two > "$TMP/mk/keep/a.txt"
$G -C "$TMP/mk" commit -qam port && $G -C "$TMP/mk" format-patch -q -1 -o "$SER/p"
sparse_fix() {   # COMMIT -> a manifest with one clone-sparse section
    cat > "$TMP/sparse.toml" <<EOF2
[meta]
schema = "1"

[source.thing]
path = "$TMP/co"
probe = "keep/a.txt"
repo = "file://$UP"
tag = "v1"
commit = "$1"
branch = "port"
sparse = "keep"
patches = "series/p"
feeds = "thing feed"
forageable = "clone-sparse"
EOF2
}
runs() { FORAGE_ROOT="$TMP" MANIFEST="$TMP/sparse.toml" "$FORAGE" "$@"; }

# C1: a wrong pin is REFUSED (the tag must resolve to the manifest's commit)
sparse_fix "0000000000000000000000000000000000000000"
out="$(runs source.thing 2>&1)"; rc=$?
if [[ $rc -ne 0 ]]; then ok "C1 wrong pin exits nonzero"; else bad "C1 wrong pin was accepted"; fi
assert_grep "$out" "refusing" "C1 wrong pin names the refusal"
if [[ "$(cat "$TMP/co/keep/a.txt" 2>/dev/null)" == "two" ]]; then bad "C1 patched a tree it refused"; else ok "C1 refused tree left unpatched"; fi
rm -rf "$TMP/co"

# C2: the right pin gathers -- cone honoured, series applied on the named branch
sparse_fix "$PINSHA"
out="$(runs source.thing 2>&1)"; rc=$?
if [[ $rc -eq 0 ]]; then ok "C2 gather exits zero"; else bad "C2 gather failed: $out"; fi
if [[ "$(cat "$TMP/co/keep/a.txt" 2>/dev/null)" == "two" ]]; then ok "C2 series applied (content changed)"; else bad "C2 series not applied"; fi
if [[ -e "$TMP/co/skip/b.txt" ]]; then bad "C2 sparse cone ignored (skip/ is checked out)"; else ok "C2 sparse cone honoured"; fi
if [[ "$(git -C "$TMP/co" branch --show-current)" == "port" ]]; then ok "C2 on the named branch"; else bad "C2 wrong branch"; fi
assert_grep "$(runs status 2>&1)" "source.thing[[:space:]]+present" "C2 status: clone-sparse reports present"

# C3: idempotent -- a second gather applies nothing and moves nothing
h1="$(git -C "$TMP/co" rev-parse HEAD)"
out="$(runs source.thing 2>&1)"
assert_grep "$out" "already applied" "C3 second gather: already applied"
if [[ "$(git -C "$TMP/co" rev-parse HEAD)" == "$h1" ]]; then ok "C3 HEAD unchanged"; else bad "C3 HEAD moved on a no-op gather"; fi

# C4: a DRIFTED checkout is reported, never reset (it may hold somebody's work)
echo three > "$TMP/co/keep/a.txt"; $G -C "$TMP/co" commit -qam drift
h2="$(git -C "$TMP/co" rev-parse HEAD)"
out="$(runs source.thing 2>&1)"; rc=$?
if [[ $rc -ne 0 ]]; then ok "C4 drift exits nonzero"; else bad "C4 drift was accepted"; fi
assert_grep "$out" "drifted" "C4 drift is named"
if [[ "$(git -C "$TMP/co" rev-parse HEAD)" == "$h2" && "$(cat "$TMP/co/keep/a.txt")" == "three" ]]; then
    ok "C4 drifted work left intact"; else bad "C4 forage modified a drifted checkout"; fi

echo "== $pass passed, $fail failed =="
[[ "$fail" -eq 0 ]]
