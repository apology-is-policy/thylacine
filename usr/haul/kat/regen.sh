#!/usr/bin/env bash
# Re-derive haul's npxf known-answer vectors from npxf's own source and check
# them against the committed fixture.
#
# Without this, kat/vectors.txt is a RECORDING: a file whose only claim to being
# npxf's output is that someone once said so. With it, the claim is re-checked
# on demand, and a change to npxf's key schedule or record framing breaks here
# -- loudly, and in the tree that would otherwise silently keep speaking the old
# protocol.
#
#   regen.sh            check the committed vectors (default)
#   regen.sh --write    overwrite kat/vectors.txt with what npxf produces now
#
# NPXF_ROOT locates npxf's tree. Absent, this SKIPs (exit 77) rather than
# failing: npxf is a separate repository and not every checkout has it.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NPXF_ROOT="${NPXF_ROOT:-$HOME/projects/npxf}"
CXX="${CXX:-c++}"
vectors="$here/vectors.txt"

skip() {
    echo "LS-CI SKIP: $*" >&2
    exit 77
}

[[ -d $NPXF_ROOT ]] || skip "npxf tree not found at $NPXF_ROOT (set NPXF_ROOT)"
[[ -f $NPXF_ROOT/src/channel.cpp ]] || skip "$NPXF_ROOT is not an npxf tree (no src/channel.cpp)"

write=0
case "${1:-}" in
    --write) write=1 ;;
    "") ;;
    *) echo "regen.sh: unknown argument '$1'" >&2; exit 2 ;;
esac

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# npxf's primitives are OpenSSL 3's (npxf af68838, 2026-09-18), so crypto.cpp
# needs libcrypto's headers and library. pkg-config supplies them; set
# OPENSSL_CFLAGS and OPENSSL_LIBS to override. npxf cannot build without
# OpenSSL either, so a present npxf with no libcrypto FAILS rather than SKIPs:
# a skip here would leave the vectors unchecked, as they were before this check.
if [[ -z ${OPENSSL_CFLAGS+set} || -z ${OPENSSL_LIBS+set} ]]; then
    if ! command -v pkg-config >/dev/null 2>&1 ||
       ! pkg-config --exists libcrypto; then
        echo "regen.sh: FAIL -- libcrypto not found by pkg-config; npxf needs OpenSSL 3 (set OPENSSL_CFLAGS and OPENSSL_LIBS)" >&2
        exit 1
    fi
    OPENSSL_CFLAGS="$(pkg-config --cflags libcrypto)"
    OPENSSL_LIBS="$(pkg-config --libs libcrypto)"
fi

# channel.cpp is #included, so its dependencies must be LINKED: crypto.cpp for
# the primitives, net.cpp for the read/write/timeout helpers Channel::send and
# the two handshakes call.
# shellcheck disable=SC2086 # the flag strings are word lists
"$CXX" -std=c++20 -O1 -pthread \
    -Wall -Wextra -Wno-unused-parameter \
    -I "$NPXF_ROOT" -I "$NPXF_ROOT/src" $OPENSSL_CFLAGS \
    -o "$tmp/npxf_kat" \
    "$here/npxf_kat.cpp" "$NPXF_ROOT/src/crypto.cpp" "$NPXF_ROOT/src/net.cpp" \
    $OPENSSL_LIBS

"$tmp/npxf_kat" > "$tmp/vectors.txt"

if (( write )); then
    cp "$tmp/vectors.txt" "$vectors"
    echo "regen.sh: wrote $vectors"
    exit 0
fi

if diff -u "$vectors" "$tmp/vectors.txt"; then
    echo "regen.sh: PASS -- $(grep -c ' = ' "$vectors") vectors re-derived from $NPXF_ROOT"
else
    cat >&2 <<EOF

regen.sh: FAIL -- the committed vectors are not what npxf produces now.

Either npxf's protocol changed (then usr/haul/src/npxf.rs must change WITH it,
and --write is the last step, not the first), or this tree's npxf is a different
version than the one the fixture came from.
EOF
    exit 1
fi
