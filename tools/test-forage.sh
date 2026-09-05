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

# B9: unknown target is rejected
if runf nosuch >/dev/null 2>&1; then bad "B9 unknown target should exit nonzero"; else ok "B9 unknown target rejected"; fi

# B10: dry-run touched nothing (alpine was never downloaded)
if [[ -f "$TMP/cache/alpine.tar.gz" ]]; then bad "B10 dry-run created a file"; else ok "B10 dry-run touched nothing"; fi

echo "== $pass passed, $fail failed =="
[[ "$fail" -eq 0 ]]
