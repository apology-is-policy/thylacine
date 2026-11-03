#!/usr/bin/env bash
# tools/test-forage.sh -- discrimination tests for the build-input collector
# (tools/forage.sh) + the manifest reader. Every case is a CONTROL that fails
# without the behavior it checks. No network, no git, no GCP: the TOML-subset
# parser + `present` probes are tested against the REAL manifest and a temp
# FIXTURE manifest, and every gather runs under FORAGE_DRY=1 (touch nothing) or
# resolves to an instruction. ~1 s.
#
# FORAGE and BUILDSH are overridable so each control can be PROVEN by pointing it
# at a sabotaged copy (the fail-without-fix check).
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FORAGE="${FORAGE:-$REPO_ROOT/tools/forage.sh}"
BUILDSH="${BUILDSH:-$REPO_ROOT/tools/build.sh}"
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

# A11: every pin feeding the library tests (CHUNK_LIBTESTS) is the same shape:
# build.sh refuses a tarball by its own literal file name and sha256. Derived
# from the manifest, so a new pin is covered without editing this list; the
# enum asserts below prove the derivation found the sections. (Sabotage: edit
# one hex digit of a CHUNK_LIBTESTS sha256 in build.sh -> FAIL.)
lt_secs=""
for csec in $(manifest_sections cache.); do
    case "$(manifest_get "$csec" feeds)" in
        CHUNK_LIBTESTS*) lt_secs="$lt_secs $csec" ;;
    esac
done
assert_grep "$lt_secs" "cache.harfbuzz"   "A11 enum: CHUNK_LIBTESTS includes harfbuzz"
assert_grep "$lt_secs" "cache.fontconfig" "A11 enum: CHUNK_LIBTESTS includes fontconfig"
assert_grep "$lt_secs" "cache.dejavu"     "A11 enum: CHUNK_LIBTESTS includes dejavu"
for csec in $lt_secs; do
    for ckey in file sha256; do
        cval="$(manifest_get "$csec" "$ckey")"
        if [[ -n "$cval" ]] && grep -qF -- "\"$cval\"" "$BUILDSH"; then
            ok "A11 pin: $csec.$ckey matches build.sh"
        else
            bad "A11 pin: $csec.$ckey NOT in build.sh (${cval:-empty}) -- manifest/build.sh drift"
        fi
    done
done

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
# outside), a tag, and a one-patch series made with format-patch (series/p). A
# second, STACKED series (series/s) adds a patch that edits the line the first one
# wrote, as WebKit's does (0003 corrects a comment 0001 added).
UP="$TMP/upstream"; SER="$TMP/series"; G="git -c user.name=t -c user.email=t@t -c init.defaultBranch=main"
mkdir -p "$UP/keep" "$UP/skip" "$UP/Source/JavaScriptCore" "$SER/p" "$SER/s"
$G -C "$UP" init -q && echo one > "$UP/keep/a.txt" && echo x > "$UP/skip/b.txt"
for i in $(seq 1 20); do echo "m$i"; done > "$UP/keep/m.txt"
echo "project(JavaScriptCore)" > "$UP/Source/JavaScriptCore/CMakeLists.txt"
$G -C "$UP" add -A && $G -C "$UP" commit -qm base && $G -C "$UP" tag v1
PINSHA="$($G -C "$UP" rev-parse HEAD)"
$G clone -q "$UP" "$TMP/mk" && echo two > "$TMP/mk/keep/a.txt" && sed -i.bak 's/^m2$/M2/' "$TMP/mk/keep/m.txt" && rm -f "$TMP/mk/keep/m.txt.bak"
$G -C "$TMP/mk" commit -qam port && $G -C "$TMP/mk" format-patch -q -1 -o "$SER/p"
echo two-b > "$TMP/mk/keep/a.txt" && $G -C "$TMP/mk" commit -qam stack && $G -C "$TMP/mk" format-patch -q -2 -o "$SER/s"
# series/o: one patch that edits a file OUTSIDE the cone (skip/)
mkdir -p "$SER/o" && $G clone -q "$UP" "$TMP/mko" && echo y > "$TMP/mko/skip/b.txt"
$G -C "$TMP/mko" commit -qam outside && $G -C "$TMP/mko" format-patch -q -1 -o "$SER/o"
sparse_fix() {   # COMMIT [PATH] -> a manifest with one clone-sparse section
    cat > "$TMP/sparse.toml" <<EOF2
[meta]
schema = "1"

[source.thing]
path = "${2:-$TMP/co}"
probe = "keep/a.txt"
repo = "file://$UP"
tag = "v1"
commit = "$1"
branch = "port"
sparse = "keep"
patches = "${3:-series/p}"
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

# C5: a FULL checkout at the path is reported, never converted or widened (git's
# own refusal names neither the section nor the path). C3 is its control: the
# same gather on a sparse checkout passes through the widen.
$G clone -q "$UP" "$TMP/full"
h5="$(git -C "$TMP/full" rev-parse HEAD)"
sparse_fix "$PINSHA" "$TMP/full"
out="$(runs source.thing 2>&1)"; rc=$?
if [[ $rc -ne 0 ]]; then ok "C5 full checkout exits nonzero"; else bad "C5 full checkout was accepted"; fi
assert_grep "$out" "source.thing: .*full is not a sparse checkout" "C5 names the section and the path"
if [[ "$(git -C "$TMP/full" rev-parse HEAD)" == "$h5" && -e "$TMP/full/skip/b.txt" \
      && "$(git -C "$TMP/full" config --bool core.sparseCheckout)" != true ]]; then
    ok "C5 full checkout left as it was"; else bad "C5 forage modified a full checkout"; fi

# C6/C7: a STACKED series gathers, and a second gather finds both patches applied
# (asked one patch at a time, 0001 no longer reverse-applies and the checkout
# would be called drifted).
sparse_fix "$PINSHA" "$TMP/cs" series/s
out="$(runs source.thing 2>&1)"; rc=$?
if [[ $rc -eq 0 && "$(cat "$TMP/cs/keep/a.txt" 2>/dev/null)" == "two-b" ]]; then ok "C6 stacked series applied in order"; else bad "C6 stacked series: rc=$rc $out"; fi
h6="$(git -C "$TMP/cs" rev-parse HEAD 2>/dev/null)"
out="$(runs source.thing 2>&1)"; rc=$?
if [[ $rc -eq 0 ]]; then ok "C7 second gather of a stacked series exits zero"; else bad "C7 stacked series called drifted: $out"; fi
assert_grep "$out" "0002-stack.patch: already applied" "C7 the stacked patch reads as applied"
if [[ "$(git -C "$TMP/cs" rev-parse HEAD 2>/dev/null)" == "$h6" ]]; then ok "C7 HEAD unchanged"; else bad "C7 HEAD moved"; fi

# C10: a RELATIVE FORAGE_ROOT still finds the series applied (the patch paths
# built from it go to `git -C`, which resolves a relative one in the checkout:
# every prefix failed and a correct checkout was called drifted)
out="$(cd "$(dirname "$TMP")" && FORAGE_ROOT="$(basename "$TMP")" MANIFEST="$TMP/sparse.toml" "$FORAGE" source.thing 2>&1)"; rc=$?
if [[ $rc -eq 0 ]]; then ok "C10 relative FORAGE_ROOT exits zero"; else bad "C10 relative FORAGE_ROOT: rc=$rc $out"; fi
assert_grep "$out" "0002-stack.patch: already applied" "C10 relative FORAGE_ROOT reads the series as applied"

# C11: a patch that touches a path outside the sparse cone is refused by name,
# before any patch is applied (the series check's scratch index holds the whole
# tree, so it would have passed it, and `git am` failed on the skip-worktree path)
sparse_fix "$PINSHA" "$TMP/co11" series/o
out="$(runs source.thing 2>&1)"; rc=$?
if [[ $rc -ne 0 ]]; then ok "C11 a patch outside the cone exits nonzero"; else bad "C11 a patch outside the cone was accepted"; fi
assert_grep "$out" "touches skip/b.txt, outside the sparse cone" "C11 names the path and the cone"
if [[ "$(git -C "$TMP/co11" rev-parse HEAD 2>/dev/null)" == "$PINSHA" ]]; then ok "C11 nothing applied"; else bad "C11 the checkout moved"; fi

# C8: a checkout at the pin plus a PREFIX of the series gets only the rest
sparse_fix "$PINSHA" "$TMP/cp" series/p
runs source.thing > /dev/null 2>&1
sparse_fix "$PINSHA" "$TMP/cp" series/s
out="$(runs source.thing 2>&1)"; rc=$?
assert_grep "$out" "0001-port.patch: already applied" "C8 the applied prefix is recognised"
assert_grep "$out" "0002-stack.patch: applied" "C8 the rest of the series is applied"
if [[ $rc -eq 0 && "$(cat "$TMP/cp/keep/a.txt" 2>/dev/null)" == "two-b" ]]; then ok "C8 checkout reaches the whole series"; else bad "C8 rc=$rc"; fi

# C9: an extra hunk in a file the series NAMES is drift, not "already applied"
# (each patch still reverse-applies, so a per-patch check accepted it)
sparse_fix "$PINSHA" "$TMP/cx" series/p
runs source.thing > /dev/null 2>&1
sed -i.bak 's/^m20$/m20-extra/' "$TMP/cx/keep/m.txt" && rm -f "$TMP/cx/keep/m.txt.bak"
$G -C "$TMP/cx" commit -qam extra
h9="$(git -C "$TMP/cx" rev-parse HEAD)"
out="$(runs source.thing 2>&1)"; rc=$?
if [[ $rc -ne 0 ]]; then ok "C9 extra hunk in a named file exits nonzero"; else bad "C9 extra hunk accepted as applied"; fi
assert_grep "$out" "drifted" "C9 extra hunk is named drift"
if [[ "$(git -C "$TMP/cx" rev-parse HEAD)" == "$h9" ]]; then ok "C9 checkout left intact"; else bad "C9 forage moved the checkout"; fi

# --- D. build.sh's own check of the WebKit checkout ---------------------------
# webkit_series_tree + webkit_checkout_ok, lifted from build.sh, against a full
# clone at the pin plus the stacked series: HEAD's tree must be EXACTLY the pin
# plus the series in order.
eval "$(sed -n '/^webkit_series_tree() {/,/^}/p;/^webkit_checkout_ok() {/,/^}/p' "$BUILDSH")"
mkdir -p "$TMP/port/patches" && cp "$SER/s/"*.patch "$TMP/port/patches/"
$G clone -q "$UP" "$TMP/dco" && $G -C "$TMP/dco" checkout -q -b port "$PINSHA" && $G -C "$TMP/dco" am -q "$TMP/port/patches/"*.patch
chk() { WEBKIT_PIN="$PINSHA" webkit_checkout_ok "$TMP/dco" "$TMP/port" 2>&1; }
out="$(chk)"; rc=$?
if [[ $rc -eq 0 ]]; then ok "D1 pin + stacked series accepted"; else bad "D1 refused: $out"; fi
sed -i.bak 's/^m20$/m20-extra/' "$TMP/dco/keep/m.txt" && rm -f "$TMP/dco/keep/m.txt.bak"
out="$(chk)"; rc=$?
if [[ $rc -ne 0 ]]; then ok "D2 uncommitted change refused"; else bad "D2 uncommitted change accepted"; fi
assert_grep "$out" "uncommitted" "D2 names the uncommitted change"
$G -C "$TMP/dco" commit -qam extra
out="$(chk)"; rc=$?
if [[ $rc -ne 0 ]]; then ok "D3 extra hunk in a named file refused"; else bad "D3 extra hunk in a named file accepted"; fi
assert_grep "$out" "not the pin plus the patch series" "D3 names the mismatch"
$G -C "$TMP/dco" reset -q --hard HEAD~1
printf 'From 0 Mon Sep 17 00:00:00 2001\nSubject: [PATCH] bad\n\n---\n--- a/keep/a.txt\n+++ b/keep/a.txt\n@@ -1 +1 @@\n-nothing-like-this\n+x\n' > "$TMP/port/patches/0003-bad.patch"
out="$(chk)"; rc=$?
if [[ $rc -ne 0 ]]; then ok "D4 a patch that does not apply is refused"; else bad "D4 unappliable patch accepted"; fi
assert_grep "$out" "0003-bad.patch does not apply" "D4 names the patch"
assert_grep "$out" "0003-bad.patch does not apply on the pin plus the patches before it: .+" "D4 keeps git's reason"

# D6: a patch that cannot be opened is named as such, not "does not apply"
out="$(webkit_series_tree "$TMP/dco" "$PINSHA" "$TMP/port/patches/no-such.patch" 2>&1)"; rc=$?
if [[ $rc -ne 0 ]]; then ok "D6 a missing patch is refused"; else bad "D6 a missing patch was accepted"; fi
assert_grep "$out" "no-such.patch is not readable" "D6 names the unreadable patch"

# D7: RELATIVE patch paths build the same tree as absolute ones (git -C resolves
# a relative path in the checkout, where the patch is not)
want="$(git -C "$TMP/dco" rev-parse 'HEAD^{tree}')"
out="$(cd "$SER/s" && webkit_series_tree "$TMP/dco" "$PINSHA" 0001-*.patch 0002-*.patch 2>&1)"; rc=$?
if [[ $rc -eq 0 && "$out" == "$want" ]]; then ok "D7 relative patch paths build HEAD's tree"; else bad "D7 relative paths: rc=$rc $out (want $want)"; fi

# D5: the same extra hunk over the ONE-patch series, which a per-patch check could
# ask about (D3's series is stacked, so a per-patch check refuses it for that).
mkdir -p "$TMP/port1/patches" && cp "$SER/p/"*.patch "$TMP/port1/patches/"
$G clone -q "$UP" "$TMP/dco1" && $G -C "$TMP/dco1" checkout -q -b port "$PINSHA" && $G -C "$TMP/dco1" am -q "$TMP/port1/patches/"*.patch
sed -i.bak 's/^m20$/m20-extra/' "$TMP/dco1/keep/m.txt" && rm -f "$TMP/dco1/keep/m.txt.bak" && $G -C "$TMP/dco1" commit -qam extra
out="$(WEBKIT_PIN="$PINSHA" webkit_checkout_ok "$TMP/dco1" "$TMP/port1" 2>&1)"; rc=$?
if [[ $rc -ne 0 ]]; then ok "D5 extra hunk over a one-patch series refused"; else bad "D5 extra hunk over a one-patch series accepted"; fi
assert_grep "$out" "not the pin plus the patch series" "D5 names the mismatch"

echo "== $pass passed, $fail failed =="
[[ "$fail" -eq 0 ]]
