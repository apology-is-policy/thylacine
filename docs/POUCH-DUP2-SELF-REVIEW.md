# Pouch self-dup descriptor validation

Astra, October 11. Single-agent self-review, not an independent audit.

## Contract and correction

`dup2(fd,fd)` must reject an invalid descriptor and otherwise leave it intact.
The previous Pouch implementation accepted every nonnegative value. Its claim
that self-dup clears close-on-exec was also wrong: this case preserves flags.
See the [POSIX specification](https://pubs.opengroup.org/onlinepubs/9799919799/functions/dup.html).

Patch0054 (codex/astra 0051) validates native descriptors with the approved128 metadata query.
It rejects negatives before examining the socket tag. Tagged descriptors use
`pouch_sock_resolve`, whose existing lock covers the live-slot read; the result
is compared with NULL without dereferencing the returned static slot. A fresh
socket is valid even without a backing kernel handle. Neither branch allocates,
closes, changes flags, acquires rights, or takes an object reference. Validity
is observed at an instant; concurrent close/reuse is not pinned. Native legacy
unknown-call -1 becomes ENOSYS only at this query, whose errors exclude EPERM.

Distinct-target replacement remains ENOSYS. Native `handle_dup_to` exists
internally for Vivarium but is not exposed through a native syscall; lowest-free
dup plus close cannot safely emulate it. No Linux-number forwarding or new ABI
was added. The self-dup fix does not implement inheritance, fcntl setters,
spawn attributes, or cancellation.

## Evidence

The maintained host runner compiles actual series-produced `dup2.c` with
metadata and socket-owner mocks. Its12 cases pass at O0/O2 with ASan+UBSan.
Baseline fails10 assertions: seven wrong results and three missing lookup
observations. Removing native query, socket lookup, or legacy translation gives
7/3/1 failures respectively at both levels. Host errno values are host-specific;
they are not claimed as target numeric encodings. The flag-set case is a host
mock witness; the native guest checks preservation of its actual zero flag.

The official51-patch sysroot rebuild produces libc.a and libc.so. The native
probe links twice byte-identically. Runtime17 on the qualified128 kernel as
uid1000 passes17 checks, including transfer-only rights,1024live native slots,
1019 temporary duplicates and complete duplicate cleanup, fresh/closed sockets,
malformed tags, invalid handles and unchanged distinct-target refusal.
Runtime18 with the same libc on the pre-query kernel passes4 checks, including
ENOSYS without mutating the valid source. Both complete harnesses exit0 with
probe/shell/sync success, QMP quit and serial EOF; host servers are reaped and
temporary credentials removed. Kernel sources were unchanged; the earlier
source-matched50-boot evidence still belongs to those exact kernel artifacts.

Receipts, raw logs and verifier: `work/oct11-native-tools/dup2-self/`, with
runtime17/18 beside it. Prior50-patch artifacts are preserved as APFS clones
under handle-query/qualified-{sysroot,pouch}; previous-artifacts.json pins them.
The patch checker must be given a directory: initial file/--help invocations
checked zero hunks and are not validation. The proper Pouch-directory run
checks266 hunks with zero errors or warnings. All ten protected files remain
unchanged, Stratum stays clean7dd103d, and the8448MiB runner floor remains.
This is a static-probe qualification, not a dynamic-loader or production bake.
