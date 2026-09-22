#!/usr/bin/env bash
# tools/forage.sh -- the build-input collector (an animal foraging for scattered
# provisions). Reads tools/build-manifest.toml and gathers what it can: clones
# the sibling forks at their pinned commits, downloads + sha256-verifies the
# manual-drop cache inputs, and pulls the remotely-built Clade artifacts. What it
# cannot do automatically it prints as an instruction. Design: BUILD-CONFIG-DESIGN.md
# section 5.
#
#   forage.sh                 report the status of every input
#   forage.sh <target>        gather one: go|ambush|stratum|gopls|llvm|mesa|webkit|icu|
#                             alpine|busybox|static-curl|static-git|quake|duke3d|tombraider|
#                             clade|clade-gl
#   forage.sh all             gather everything that can be gathered automatically
#   FORAGE_DRY=1 forage.sh …   print what it WOULD do; touch nothing (git/net/gcp)
#
# bash 3.2 safe. MANIFEST is overridable (fixture isolation for tools/test-forage.sh).
set -uo pipefail

# FORAGE_ROOT overrides the repo root the REPO_ROOT-relative inputs (build/cache,
# build/clade) resolve under -- so tools/test-forage.sh can isolate probes +
# downloads to a temp dir. Its default is the real repo. MANIFEST is likewise
# overridable (a fixture manifest).
REPO_ROOT="${FORAGE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
MANIFEST="${MANIFEST:-$REPO_ROOT/tools/build-manifest.toml}"
DRY="${FORAGE_DRY:-0}"

usage() { sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; }

# --- the TOML-subset reader (controlled subset; see the manifest header) ------
# manifest_get "section.name" "key" -> the value, quotes stripped; empty if absent.
manifest_get() {
    awk -v sec="$1" -v key="$2" '
        /^[[:space:]]*#/ { next }
        /^[[:space:]]*\[/ {
            s=$0; sub(/^[[:space:]]*\[/,"",s); sub(/\][[:space:]]*$/,"",s); gsub(/[[:space:]]/,"",s); cur=s; next
        }
        cur==sec {
            if (match($0, "^[[:space:]]*" key "[[:space:]]*=")) {
                v=$0; sub("^[[:space:]]*" key "[[:space:]]*=[[:space:]]*","",v)
                sub(/[[:space:]]+$/,"",v)
                if (v ~ /^".*"$/) { sub(/^"/,"",v); sub(/"$/,"",v) }
                print v; exit
            }
        }
    ' "$MANIFEST"
}
# manifest_sections "prefix." -> each full section name under that prefix, in order.
manifest_sections() {
    awk -v pre="$1" '
        /^[[:space:]]*\[/ { s=$0; sub(/^[[:space:]]*\[/,"",s); sub(/\][[:space:]]*$/,"",s); gsub(/[[:space:]]/,"",s); if (index(s,pre)==1) print s }
    ' "$MANIFEST"
}
all_sections() { manifest_sections "fork."; manifest_sections "source."; manifest_sections "cache."; manifest_sections "network."; manifest_sections "remote."; }

expand()    { case "$1" in "~/"*) printf '%s' "$HOME/${1#\~/}" ;; *) printf '%s' "$1" ;; esac; }
sha_of()    { if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'; else sha256sum "$1" | awk '{print $1}'; fi; }
verify_sha(){ [[ "$(sha_of "$1")" == "$2" ]]; }

# --- present? (per forageable class) -----------------------------------------
present() {
    local sec="$1" kind path probe dir file
    kind="$(manifest_get "$sec" forageable)"
    case "$kind" in
        clone|clone-sparse|manual|remote-source)
            path="$(expand "$(manifest_get "$sec" path)")"; probe="$(manifest_get "$sec" probe)"
            if [[ -n "$probe" ]]; then [[ -e "$path/$probe" ]]; else [[ -d "$path" ]]; fi ;;
        download)
            dir="$(manifest_get "$sec" dir)"; file="$(manifest_get "$sec" file)"
            [[ -f "$REPO_ROOT/$dir/$file" ]] ;;
        remote-pull)
            probe="$(manifest_get "$sec" probe)"; [[ -n "$probe" && -e "$REPO_ROOT/$probe" ]] ;;
        auto-at-build) return 0 ;;
        *) return 1 ;;
    esac
}

# --- actions ------------------------------------------------------------------
do_clone() {
    local sec="$1" path dest repo commit branch
    path="$(expand "$(manifest_get "$sec" path)")"
    dest="$(manifest_get "$sec" clone_dest)"; [[ -n "$dest" ]] && dest="$(expand "$dest")" || dest="$path"
    repo="$(manifest_get "$sec" repo)"; commit="$(manifest_get "$sec" commit)"; branch="$(manifest_get "$sec" branch)"
    if [[ -z "$repo" ]]; then echo "forage: $sec has no repo -- see 'manual'." >&2; return 1; fi
    if [[ "$DRY" == 1 ]]; then
        if [[ -d "$dest/.git" ]]; then echo "[dry-run] $sec: git -C $dest fetch; checkout ${commit:-$branch}"
        else echo "[dry-run] $sec: git clone $repo $dest; checkout ${commit:-$branch}"; fi
        return 0
    fi
    if [[ -d "$dest/.git" ]]; then
        echo "==> $sec: present at $dest -- fetching"; git -C "$dest" fetch --all --tags || true
    else
        echo "==> $sec: cloning $repo -> $dest"; git clone "$repo" "$dest" || { echo "forage: clone failed" >&2; return 1; }
    fi
    if [[ -n "$commit" ]]; then git -C "$dest" checkout "$commit" 2>/dev/null || git -C "$dest" checkout "${branch:-HEAD}" || true
    elif [[ -n "$branch" ]]; then git -C "$dest" checkout "$branch" || true; fi
    echo "    $sec now at $(git -C "$dest" rev-parse --short HEAD 2>/dev/null || echo '?')"
}

do_clone_sparse() {
    # A partial sparse clone of a huge upstream, at an exact commit, plus our
    # in-repo patch series. Idempotent: a checkout already at pin + series is
    # left alone; one at the bare pin gets the series; anything else is REPORTED,
    # never reset -- it may hold somebody's work.
    local sec="$1" path repo tag commit branch sparse patches pp
    path="$(expand "$(manifest_get "$sec" path)")"
    repo="$(manifest_get "$sec" repo)"; tag="$(manifest_get "$sec" tag)"; commit="$(manifest_get "$sec" commit)"
    branch="$(manifest_get "$sec" branch)"; sparse="$(manifest_get "$sec" sparse)"
    patches="$REPO_ROOT/$(manifest_get "$sec" patches)"
    if [[ -z "$repo" || -z "$tag" || -z "$commit" || -z "$branch" || -z "$sparse" ]]; then
        echo "forage: $sec needs repo + tag + commit + branch + sparse" >&2; return 1; fi
    if [[ "$DRY" == 1 ]]; then
        echo "[dry-run] $sec: partial sparse clone of $repo @ $tag ($commit) -> $path [$sparse]; git am $patches/*.patch on '$branch'"
        return 0
    fi
    if [[ ! -d "$path/.git" ]]; then
        echo "==> $sec: partial sparse clone $repo @ $tag -> $path"
        git clone --filter=blob:none --no-checkout --depth 1 --branch "$tag" "$repo" "$path" \
            || { echo "forage: clone failed" >&2; return 1; }
        git -C "$path" sparse-checkout init --cone || return 1
        # shellcheck disable=SC2086  # the cone list is space-separated on purpose
        git -C "$path" sparse-checkout set $sparse || return 1
    fi
    local at; at="$(git -C "$path" rev-parse "$tag^{commit}" 2>/dev/null || true)"
    if [[ "$at" != "$commit" ]]; then
        echo "forage: $sec: tag $tag resolves to '${at:-nothing}', the manifest pins $commit -- refusing" >&2
        return 1
    fi
    if git -C "$path" rev-parse -q --verify "refs/heads/$branch" >/dev/null; then
        git -C "$path" checkout -q "$branch" || return 1
    else
        git -C "$path" checkout -q -b "$branch" "$commit" || return 1
    fi
    for pp in "$patches"/*.patch; do
        [[ -e "$pp" ]] || continue
        if git -C "$path" apply --check --reverse "$pp" 2>/dev/null; then
            echo "    $(basename "$pp"): already applied"
        elif git -C "$path" -c user.name=forage -c user.email=forage@localhost am -q "$pp"; then
            echo "    $(basename "$pp"): applied"
        else
            git -C "$path" am --abort 2>/dev/null || true
            echo "forage: $sec: $(basename "$pp") neither applies nor is applied -- the checkout has drifted; inspect $path" >&2
            return 1
        fi
    done
    echo "    $sec now at $(git -C "$path" rev-parse --short HEAD) (pin $tag + $(ls "$patches"/*.patch 2>/dev/null | wc -l | tr -d ' ') patch(es))"
}

do_download() {
    local sec="$1" url dir file sha dest
    url="$(manifest_get "$sec" url)"; dir="$(manifest_get "$sec" dir)"; file="$(manifest_get "$sec" file)"; sha="$(manifest_get "$sec" sha256)"
    dest="$REPO_ROOT/$dir/$file"
    if [[ -f "$dest" ]] && verify_sha "$dest" "$sha"; then echo "==> $sec: present + verified ($file)"; return 0; fi
    if [[ "$DRY" == 1 ]]; then echo "[dry-run] $sec: curl $url -> $dir/$file; verify sha256 $sha"; return 0; fi
    echo "==> $sec: downloading $url"
    mkdir -p "$REPO_ROOT/$dir"
    curl -fSL --max-time 300 -o "$dest" "$url" || { echo "forage: download failed ($url)" >&2; return 1; }
    if verify_sha "$dest" "$sha"; then echo "    sha256 OK ($file)"; else
        echo "forage: sha256 MISMATCH for $file -- got $(sha_of "$dest"), want $sha" >&2
        echo "    (the mirror may serve a newer -rN; fetch the pinned file from the Alpine archive by sha)" >&2
        return 1
    fi
}

do_remote_pull() {
    local sec="$1" pull probe rebuild
    probe="$(manifest_get "$sec" probe)"; pull="$(manifest_get "$sec" pull)"; rebuild="$(manifest_get "$sec" rebuild)"
    if [[ -n "$probe" && -e "$REPO_ROOT/$probe" ]]; then echo "==> $sec: present at $probe"; return 0; fi
    if [[ "$DRY" == 1 ]]; then echo "[dry-run] $sec: $pull  (needs the builder reachable)"; return 0; fi
    echo "==> $sec: pulling via '$pull' (needs the builder reachable)"
    ( cd "$REPO_ROOT" && eval "$pull" ) || { echo "forage: pull failed${rebuild:+; rebuild with $rebuild}" >&2; return 1; }
    # A pulled FILE with a pin is verified like a download; a pulled tree has none.
    local sha; sha="$(manifest_get "$sec" sha256)"
    if [[ -n "$sha" && -f "$REPO_ROOT/$probe" ]]; then
        if verify_sha "$REPO_ROOT/$probe" "$sha"; then echo "    sha256 OK ($probe)"; else
            echo "forage: sha256 MISMATCH for $probe -- got $(sha_of "$REPO_ROOT/$probe"), want $sha" >&2
            return 1
        fi
    fi
}

do_instruct() {   # SECTION -- print the manual remedy for a non-automatable input
    local sec="$1" kind path
    kind="$(manifest_get "$sec" forageable)"; path="$(expand "$(manifest_get "$sec" path)")"
    case "$kind" in
        manual)        echo "==> $sec: no public source -- obtain it from the operator and place it at $path (commit $(manifest_get "$sec" commit))" ;;
        remote-source) echo "==> $sec: a source pin for a remotely-built artifact ($(manifest_get "$sec" feeds)); not fetched locally. Pull the built artifact instead (forage clade / clade-gl)." ;;
        auto-at-build) echo "==> $sec: fetched automatically at build time ($(manifest_get "$sec" file)); no action needed." ;;
        *)             echo "==> $sec: no action" ;;
    esac
}

forage_section() {
    local sec="$1" kind
    kind="$(manifest_get "$sec" forageable)"
    case "$kind" in
        clone)        do_clone "$sec" ;;
        clone-sparse) do_clone_sparse "$sec" ;;
        download)     do_download "$sec" ;;
        remote-pull)  do_remote_pull "$sec" ;;
        manual|remote-source|auto-at-build) do_instruct "$sec" ;;
        "")           echo "forage: unknown section '$sec'" >&2; return 1 ;;
        *)            echo "forage: $sec has an unknown forageable '$kind'" >&2; return 1 ;;
    esac
}

# target name -> the manifest section(s) it gathers
target_sections() {
    case "$1" in
        go)       echo "fork.go" ;;
        ambush)   echo "fork.ambush" ;;
        stratum)  echo "fork.stratum" ;;
        gopls)    echo "fork.gopls" ;;
        llvm)     echo "fork.llvm" ;;
        mesa)     echo "fork.mesa" ;;
        webkit)   echo "source.webkit cache.icu4c" ;;
        icu)      echo "cache.icu4c" ;;
        alpine)   echo "cache.alpine cache.busybox" ;;
        busybox)  echo "cache.busybox" ;;
        static-curl) echo "cache.static-curl" ;;
        static-git) echo "remote.static_git" ;;
        quake)    echo "network.quake" ;;
        duke3d)   echo "network.duke3d" ;;
        tombraider) echo "network.tombraider" ;;
        clade)    echo "remote.clade_llvm" ;;
        clade-gl) echo "remote.clade_gl" ;;
        *.*)      # a literal manifest section (fork.x / cache.x / network.x / remote.x)
                  [[ -n "$(manifest_get "$1" forageable)" ]] && echo "$1" || return 1 ;;
        *)        return 1 ;;
    esac
}

forage_status() {
    local sec st
    printf '%-18s %-8s %-13s %s\n' "INPUT" "STATUS" "ACTION" "FEEDS"
    printf '%-18s %-8s %-13s %s\n' "-----" "------" "------" "-----"
    for sec in $(all_sections); do
        if present "$sec"; then st="present"; else st="ABSENT"; fi
        printf '%-18s %-8s %-13s %s\n' "$sec" "$st" "$(manifest_get "$sec" forageable)" "$(manifest_get "$sec" feeds)"
    done
    echo
    echo "Gather one:  tools/forage.sh <go|ambush|stratum|gopls|alpine|static-curl|static-git|clade|clade-gl|quake|duke3d|tombraider>"
    echo "Gather all:  tools/forage.sh all   (FORAGE_DRY=1 to preview)"
}

forage_all() {
    local sec
    for sec in $(all_sections); do forage_section "$sec"; done
}

# --- dispatch (skipped when sourced, so tools/test-forage.sh can call the
#     parser/probe functions directly) ------------------------------------------
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    case "${1:-status}" in
        -h|--help)  usage; exit 0 ;;
        status)     [[ -f "$MANIFEST" ]] || { echo "forage: no manifest at $MANIFEST" >&2; exit 2; }; forage_status ;;
        all)        [[ -f "$MANIFEST" ]] || { echo "forage: no manifest at $MANIFEST" >&2; exit 2; }; forage_all ;;
        *)
            [[ -f "$MANIFEST" ]] || { echo "forage: no manifest at $MANIFEST" >&2; exit 2; }
            secs="$(target_sections "$1")" || { echo "forage: unknown target '$1' (try: tools/forage.sh status)" >&2; exit 2; }
            rc=0
            for s in $secs; do forage_section "$s" || rc=1; done
            exit "$rc" ;;
    esac
fi
