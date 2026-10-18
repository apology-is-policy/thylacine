#!/usr/bin/env bash
# tools/test-loom-client.sh -- the native Loom client: the C cores and the C
# adapter in libt's thyla/loom_client/, and their Rust twins in
# usr/lib/loom-client. Their HOST tests (the adapter's against a simulated
# kernel), and the proof that the C cores and adapter build and link
# freestanding the way libt builds them (docs/ASYNC-SERVICE-ABI.md, "Native C
# client storage", "Native Rust client cores" and "Native adapter").
#
# Four stages; any failure fails the script.
#   sanitized     every test, cores and test at -O0 under ASan + UBSan, with
#                 no recovery: any report fails the test. A control proves a
#                 signed overflow fails this build.
#   optimized     the cores at -O2 with libt's codegen flags (-ffreestanding
#                 -fno-builtin -std=c99) and -DNDEBUG, the tests hosted at -O2
#                 with their asserts on.
#   freestanding  the cores and libt's fatal.c compiled for aarch64-none-elf with
#                 libt's flags, then linked with start.S under libt's linker
#                 script and --no-undefined into a program that takes the
#                 address of every public function. No object may import a
#                 mem* routine: libt has none, and -O2 inlines every aggregate
#                 copy. The control builds the same objects at -O0, where clang
#                 calls memcpy, and requires that link to fail on it. libt's
#                 kernel call table (native_kernel.c) makes system calls, so it
#                 is built only here.
#   rust          the Rust twins' tests with cargo, in the dev profile and in
#                 the release profile the guest builds (opt-level "z", overflow
#                 checks on). A run passes only when every #[test] the crate
#                 declares ran and passed, and the build printed no warning.
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
CORES="adapter fixed_requests lease_ledger owned_mapping paired_cq request_book retirement_executor ring_payload"
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
    "test_fatal:C seven fatal invariant scenarios PASS"
    "test_adapter:C seventy-seven native adapter scenarios PASS"
)
# test name -> the test sources it links beside its own
extra_sources() {
    case $1 in
        test_adapter) extra=("$TESTS/sim_kernel.c") ;;
        *) extra=() ;;
    esac
}
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
    local entry t want extra
    for entry in "${PASS_LINES[@]}"; do
        t=${entry%%:*}; want=${entry#*:}
        extra_sources "$t"
        "$CC" "${SDK[@]}" "${WARN[@]}" -std=c11 "${test_flags[@]}" -DT_REQUEST_TEST -I"$INC" \
            "$TESTS/$t.c" ${extra[@]+"${extra[@]}"} "${objs[@]}" "$dir/host_fatal.o" -lpthread -o "$dir/$t"
        if ! "$dir/$t" > "$dir/$t.log" 2>&1; then
            cat "$dir/$t.log"; echo "test-loom-client: $name $t FAILED"; exit 1
        fi
        /usr/bin/grep -qxF "$want" "$dir/$t.log" || {
            cat "$dir/$t.log"; echo "test-loom-client: $name $t exited 0 without its PASS line"; exit 1; }
        if /usr/bin/grep -qE 'runtime error:|ERROR: AddressSanitizer' "$dir/$t.log"; then
            cat "$dir/$t.log"; echo "test-loom-client: $name $t logged a sanitizer report"; exit 1
        fi
        echo "  $name: $t PASS"
    done
}

echo "test-loom-client: $OUT"
# UBSan recovers by default, so a core with undefined behaviour would still print
# its PASS line; -fno-sanitize-recover makes the first report fatal.
SAN=(-O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
run_profile sanitized -std=c99 "${SAN[@]}" -- "${SAN[@]}"
# Control: the same flags must turn a signed overflow into a failing exit.
printf '%s\n' '#include <limits.h>' '#include <stdio.h>' \
    'int main(int argc, char **argv) { int x = INT_MAX - 1 + argc; (void)argv; x += 1; printf("%d\n", x); return 0; }' \
    > "$OUT/sanitized/ubsan_control.c"
"$CC" "${SDK[@]}" "${SAN[@]}" "$OUT/sanitized/ubsan_control.c" -o "$OUT/sanitized/ubsan_control"
if "$OUT/sanitized/ubsan_control" > "$OUT/sanitized/ubsan_control.log" 2>&1; then
    echo "test-loom-client: control: a signed overflow exited 0 under the sanitized flags"; exit 1
fi
/usr/bin/grep -q 'runtime error: signed integer overflow' "$OUT/sanitized/ubsan_control.log" || {
    cat "$OUT/sanitized/ubsan_control.log"; echo "test-loom-client: control: no UBSan report"; exit 1; }
echo "  sanitized: control -- a signed overflow fails the run"
# The cores' checks are T_LOOM_CHECK, not assert: NDEBUG must not remove them,
# which test_fatal shows here against NDEBUG-built cores.
run_profile optimized -std=c99 -O2 -ffreestanding -fno-builtin -DNDEBUG -- -O2

# Freestanding: libt's own flags (cmake/Toolchain-aarch64-userspace.cmake),
# plus -Werror.
FS="$OUT/freestanding"; mkdir -p "$FS"
TARGET=(--target=aarch64-none-elf -march=armv8-a -ffreestanding -fno-builtin -fno-common
        -fno-omit-frame-pointer -fstack-protector-strong -fstack-clash-protection
        -mbranch-protection=pac-ret+bti -Wall -Wextra -Wstrict-prototypes
        -Wmissing-prototypes -Wno-unused-parameter -Wno-unused-command-line-argument
        -Werror -std=c99 -O2 -g)
fs_objs=()
for c in $CORES native_kernel fatal; do
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
[ "$nfn" -ge 120 ] || { echo "test-loom-client: found only $nfn public functions"; exit 1; }
{
    echo '#include <thyla/loom_client/adapter.h>'
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
for c in $CORES native_kernel fatal; do
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

# Rust. The declared count comes from the crate, so a test compiled out or
# filtered away cannot leave a green summary.
HOST_TRIPLE=$(rustc -vV | awk '/^host:/ {print $2}')
RUST_SRC="$ROOT/usr/lib/loom-client/src"
declared=$(/usr/bin/grep -rhcE '^[[:space:]]*#\[test\]' "$RUST_SRC" | awk '{s+=$1} END {print s+0}')
[ "$declared" -gt 0 ] || { echo "test-loom-client: rust: $RUST_SRC declares no #[test]"; exit 1; }
for profile in dev release; do
    log="$OUT/rust-$profile.log"
    case $profile in release) release=--release ;; *) release= ;; esac
    if ! (cd "$ROOT/usr" && cargo test -p loom-client --lib $release --target "$HOST_TRIPLE") > "$log" 2>&1; then
        tail -60 "$log"; echo "test-loom-client: rust $profile FAILED"; exit 1
    fi
    if /usr/bin/grep -qE '^warning' "$log"; then
        /usr/bin/grep -E -A6 '^warning' "$log" | head -60; echo "test-loom-client: rust $profile built with warnings"; exit 1
    fi
    results=$(/usr/bin/grep -E '^test result: ' "$log" || true)
    [ "$(printf '%s\n' "$results" | /usr/bin/grep -c .)" -eq 1 ] || {
        tail -20 "$log"; echo "test-loom-client: rust $profile: expected one test harness summary"; exit 1; }
    printf '%s\n' "$results" | /usr/bin/grep -qE "^test result: ok\. $declared passed; 0 failed; 0 ignored; 0 measured; 0 filtered out" || {
        echo "$results"; echo "test-loom-client: rust $profile did not pass all $declared declared tests"; exit 1; }
    echo "  rust: $profile -- $declared of $declared declared tests passed"
done

# The Rust ring's own ownership witnesses: raw registration is unsafe, and a
# range borrow can neither alias a mutable one nor outlive its buffer.
if ! python3 "$ROOT/tools/test-loom-rust-ownership.py" --logs "$OUT/ownership" > "$OUT/ownership.log" 2>&1; then
    cat "$OUT/ownership.log"; echo "test-loom-client: ownership FAILED"; exit 1
fi
[ "$(/usr/bin/grep -c '^PASS ' "$OUT/ownership.log")" -eq 4 ] || {
    cat "$OUT/ownership.log"; echo "test-loom-client: ownership: expected 4 PASS lines"; exit 1; }
echo "  ownership: raw registration is unsafe; range borrows cannot alias or dangle"
echo "test-loom-client: PASS"
