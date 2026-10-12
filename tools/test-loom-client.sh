#!/usr/bin/env bash
# tools/test-loom-client.sh -- the native C Loom client cores (libt's
# thyla/loom_client/): their HOST tests, and the proof that they build and link
# freestanding the way libt builds them (docs/ASYNC-SERVICE-ABI.md, "Native C
# client storage").
#
# Three stages; any failure fails the script.
#   sanitized     every test, cores and test at -O0 under ASan + UBSan.
#   optimized     the cores at -O2 with libt's codegen flags (-ffreestanding
#                 -fno-builtin -std=c99), the tests hosted at -O2.
#   freestanding  the cores and libt's fatal.c compiled for aarch64-none-elf with
#                 libt's flags, then linked with start.S under libt's linker
#                 script and --no-undefined into a program that takes the
#                 address of every public function. No object may import a
#                 mem* routine: libt has none, and -O2 inlines every aggregate
#                 copy. The control builds the same objects at -O0, where clang
#                 calls memcpy, and requires that link to fail on it.
#
# A test passes only by exiting 0 AND printing its own PASS line, so a test that
# returns early, or is never built, cannot read as green.
#
# Usage: tools/test-loom-client.sh [--out DIR]   (default: a fresh temp dir)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LLVM=${LLVM_PREFIX:-/opt/homebrew/opt/llvm}
LLD=${LLD_PREFIX:-/opt/homebrew/opt/lld}
CC="$LLVM/bin/clang"
NM="$LLVM/bin/llvm-nm"
OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT=$2; shift 2 ;;
        *) echo "usage: $0 [--out DIR]" >&2; exit 2 ;;
    esac
done
[ -n "$OUT" ] || OUT=$(mktemp -d -t loom-client)
mkdir -p "$OUT"
for tool in "$CC" "$NM" "$LLD/bin/ld.lld"; do
    [ -x "$tool" ] || { echo "test-loom-client: missing $tool" >&2; exit 2; }
done

SRC="$ROOT/usr/lib/libt/src/loom_client"
TESTS="$ROOT/usr/lib/libt/tests/loom_client"
INC="$ROOT/usr/lib/libt/include"
CORES="fixed_requests lease_ledger owned_mapping paired_cq request_book retirement_executor ring_payload"
# test name -> the PASS line it prints
PASS_LINES=(
    "test_lease_ledger:C lease ledger ownership and malformed-vector checks PASS"
    "test_paired_cq:C paired CQ geometry/wrap/concurrent100000 PASS"
    "test_owned_mapping:C six owned mapping scenarios PASS"
    "test_request_book:C seven request correlation scenarios PASS"
    "test_ring_payload:C full-ring seven shared ownership scenarios PASS"
    "test_composed_ring:C two composed CQ request payload scenarios PASS"
    "test_fixed_requests:C eleven typed fixed request scenarios PASS"
    "test_deadlines:C eight deadline cancellation scenarios PASS"
    "test_retirement_executor:C eight retirement owner scenarios PASS"
    "test_retirement_timers:C six retirement timer scenarios PASS"
    "test_fatal:C six fatal invariant scenarios PASS"
)
SDK=()
if command -v xcrun >/dev/null 2>&1; then SDK=(-isysroot "$(xcrun --show-sdk-path)"); fi
WARN=(-Wall -Wextra -Werror)

run_profile() { # run_profile <name> <core flags...> -- <test flags...>
    local name=$1; shift
    local core_flags=() test_flags=()
    while [ "$1" != "--" ]; do core_flags+=("$1"); shift; done; shift
    test_flags=("$@")
    local dir="$OUT/$name"; mkdir -p "$dir"
    local objs=()
    for c in $CORES; do
        "$CC" "${SDK[@]}" "${WARN[@]}" "${core_flags[@]}" -DT_REQUEST_TEST -I"$INC" \
            -c "$SRC/$c.c" -o "$dir/$c.o"
        objs+=("$dir/$c.o")
    done
    "$CC" "${SDK[@]}" "${WARN[@]}" -std=c11 "${test_flags[@]}" -I"$INC" \
        -c "$TESTS/host_fatal.c" -o "$dir/host_fatal.o"
    local entry t want
    for entry in "${PASS_LINES[@]}"; do
        t=${entry%%:*}; want=${entry#*:}
        "$CC" "${SDK[@]}" "${WARN[@]}" -std=c11 "${test_flags[@]}" -DT_REQUEST_TEST -I"$INC" \
            "$TESTS/$t.c" "${objs[@]}" "$dir/host_fatal.o" -lpthread -o "$dir/$t"
        if ! "$dir/$t" > "$dir/$t.log" 2>&1; then
            cat "$dir/$t.log"; echo "test-loom-client: $name $t FAILED"; exit 1
        fi
        /usr/bin/grep -qxF "$want" "$dir/$t.log" || {
            cat "$dir/$t.log"; echo "test-loom-client: $name $t exited 0 without its PASS line"; exit 1; }
        echo "  $name: $t PASS"
    done
}

echo "test-loom-client: $OUT"
run_profile sanitized -std=c99 -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -- -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer
run_profile optimized -std=c99 -O2 -ffreestanding -fno-builtin -- -O2

# Freestanding: libt's own flags (cmake/Toolchain-aarch64-userspace.cmake),
# plus -Werror.
FS="$OUT/freestanding"; mkdir -p "$FS"
TARGET=(--target=aarch64-none-elf -march=armv8-a -ffreestanding -fno-builtin -fno-common
        -fno-omit-frame-pointer -fstack-protector-strong -fstack-clash-protection
        -mbranch-protection=pac-ret+bti -Wall -Wextra -Wstrict-prototypes
        -Wmissing-prototypes -Wno-unused-parameter -Wno-unused-command-line-argument
        -Werror -std=c99 -O2 -g)
fs_objs=()
for c in $CORES fatal; do
    "$CC" "${TARGET[@]}" -I"$INC" -c "$SRC/$c.c" -o "$FS/$c.o"
    fs_objs+=("$FS/$c.o")
done
"$CC" "${TARGET[@]}" -c "$ROOT/usr/lib/libt/src/start.S" -o "$FS/start.o"
imports=$("$NM" -u "${fs_objs[@]}" | awk 'NF==2 {print $2}' | sort -u)
mem=$(printf '%s\n' "$imports" | /usr/bin/grep -E '^(memcpy|memset|memmove|memcmp|bcmp|bzero)$' || true)
[ -z "$mem" ] || { echo "test-loom-client: freestanding objects import: $mem"; exit 1; }
echo "  freestanding: $(printf '%s\n' "$imports" | /usr/bin/grep -c .) imported symbols, none a mem* routine"
# Every public function, by its declaration in the installed headers.
publics=$(cat "$INC"/thyla/loom_client/*.h | tr -d '\n' | tr ';' '\n' \
    | /usr/bin/grep -oE '(^|[ *])t_[a-z_]+\(' | /usr/bin/grep -oE 't_[a-z_]+' \
    | /usr/bin/grep -vxE 't_loom_client_fatal|t_request_exhausted_test' | sort -u)
nfn=$(printf '%s\n' "$publics" | /usr/bin/grep -c .)
[ "$nfn" -ge 60 ] || { echo "test-loom-client: found only $nfn public functions"; exit 1; }
{
    echo '#include <thyla/loom_client/owned_mapping.h>'
    echo '#include <thyla/loom_client/retirement_executor.h>'
    echo 'int main(void);'
    echo 'int main(void) { static void (*const volatile all[])(void) = {'
    printf '%s\n' "$publics" | sed 's/.*/    (void (*)(void))&,/'
    echo '}; return all[0] == 0; }'
} > "$FS/link_all.c"
"$CC" "${TARGET[@]}" -Wno-cast-function-type -I"$INC" -c "$FS/link_all.c" -o "$FS/link_all.o"
"$CC" --target=aarch64-none-elf -fuse-ld=lld --ld-path="$LLD/bin/ld.lld" -nostdlib -nostartfiles \
    -static -Wl,-z,text -Wl,-z,noexecstack -Wl,-z,max-page-size=4096 -Wl,--no-dynamic-linker \
    -Wl,--build-id=none -Wl,--no-undefined -T"$ROOT/usr/scripts/aarch64-userspace.ld" \
    "$FS/start.o" "$FS/link_all.o" "${fs_objs[@]}" -o "$FS/link_all"
echo "  freestanding: $nfn public functions link under --no-undefined"
# Control: the same build at -O0 must import memcpy and fail the same link, or
# neither check above could have failed.
O0="$OUT/freestanding-O0"; mkdir -p "$O0"; o0_objs=()
for c in $CORES fatal; do
    "$CC" "${TARGET[@]}" -O0 -I"$INC" -c "$SRC/$c.c" -o "$O0/$c.o"; o0_objs+=("$O0/$c.o")
done
"$NM" -u "${o0_objs[@]}" | /usr/bin/grep -qE ' memcpy$' || {
    echo "test-loom-client: control: -O0 objects import no memcpy"; exit 1; }
if "$CC" --target=aarch64-none-elf -fuse-ld=lld --ld-path="$LLD/bin/ld.lld" -nostdlib -nostartfiles \
    -static -Wl,--no-undefined -T"$ROOT/usr/scripts/aarch64-userspace.ld" \
    "$FS/start.o" "$FS/link_all.o" "${o0_objs[@]}" -o "$O0/link_all" > "$O0/link.log" 2>&1; then
    echo "test-loom-client: control: the -O0 link succeeded"; exit 1
fi
/usr/bin/grep -q 'undefined symbol: memcpy' "$O0/link.log" || {
    cat "$O0/link.log"; echo "test-loom-client: control: the -O0 link failed, but not on memcpy"; exit 1; }
echo "  freestanding: control -- at -O0 the same link fails on memcpy"
echo "test-loom-client: PASS"
