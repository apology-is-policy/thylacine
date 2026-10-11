# Pouch spawn descriptor ordering — October 11

Owner and reviewer: Astra, single-agent self-review. This is a bounded
correction to the existing static file-action interpreter, not an independent
audit or a qualification of POSIX spawn in general.

## Defect and contract

A child action sequence `close(8), dup2(8,1)` previously recovered parent fd8
through the interpreter's unmodelled-source fallback. The child descriptor
had already been closed. The same defect affected descriptors above the
64-slot destination model and self-duplication of a closed standard stream.

Parent-side `FDOP_OPEN` also allocated real descriptors before spawning.
An original source absent from the parent could accidentally acquire such a
temporary, directly or through an earlier alias. This violated descriptor
provenance even though the kernel correctly transferred the supplied list.

Patch0052 preserves the existing contract: only standard streams are seeded,
explicit actions establish additional child slots, and a contiguous final
list is required. It adds no kernel ABI, rights, automatic inheritance,
shared state, persistent allocation or retained descriptor.

Before opening temporaries, each DUP2 inspects its nearest earlier action
on the source. A CLOSE rejects with EBADF; a later OPEN/DUP2 can establish
that source again. History follows musl's reverse-linked action list.

After each successful parent-side OPEN, the new descriptor is entered into
the existing cleanup array. If any action relies on that descriptor as an
original-parent source (no earlier child assignment), reject with EBADF and
close the temporary. The check includes aliases later closed or overwritten.
A freshly allocated parent fd proves that slot was originally absent, under
the existing requirement that callers serialize parent fd mutation during
spawn. Legitimate sources established by earlier child OPEN/DUP2 still work.

No new descriptor-existence probe is introduced: a dup probe cannot distinguish
invalid descriptors from table exhaustion or rights restrictions. Final
original source validation remains the kernel's, including its existing
ENOENT error collapse. A fully discarded invalid original source that never
collides with a temporary remains an open error-validation gap (NT2d).

## Self-review

- CLOSE checking precedes temporary allocation; temporary collision checks
  follow registration in `opened[]`, so every error cleans up.
- Parent handles and their rights are untouched. A closed child slot never
  closes the original parent handle. Existing handles are not reprobed.
- Scanning the nearest earlier action handles close/reopen and chained dup;
  inspecting all original-source uses prevents a discarded alias from hiding
  a collision with a later temporary.
- Existing 64-open bound and 16-final-descriptor bound remain. Worst-case
  history scanning is quadratic in action count with a factor bounded by
  the 64 opens; no new unbounded storage is allocated.
- The initial candidate reused the std-fd R/W existence probe. Review rejected
  that because it would narrow accepted source rights and conflate resource
  failure with absence. A second candidate checked only unmapped sources at
  DUP2; review found earlier/discarded aliases could evade it. The final
  open-time whole-history guard addresses those cases. Earlier evidence stays
  preserved and does not qualify the final source.
- Attr/envp, default inheritance, concurrent parent descriptor reuse,
  nonblocking descriptors, group signals and atomic mask/wait semantics remain
  separate work. No success stubs or native debug-hold workaround were added.

## Verification and limits

`tools/test-pouch-spawn-order.py` executes the actual series-produced C with
host syscall/open/close mocks and minimal public-type declarations. The final
19 cases cover invalid closes, high sources, self-dup, temporary collisions,
chained/discarded aliases, close/reopen, open failure, cleanup, unchanged output
pid on failure, and preserved original handles. The final suite on baseline
has eight failures. Corrected code passes at O0/O2 with LLVM22 ASan+UBSan.
Removing either close-history or temporary-origin guard causes four intended
assertion failures at each optimization level.

All49 patches apply to fresh pristine musl with fuzz0. Hunk counts and spawn
ABI mirrors pass. The actual translation unit cross-compiles for AArch64
Pouch with target headers and the existing generated ABI headers. A stricter
first compile rejected a nested-comment warning in the existing generated
signal header and an unused syscall helper parameter; its output is retained.
The normal libc warning policy compile succeeds (nested-comment warning remains).
A first host setup attempt used LLVM's missing default MacOSX26.sdk; the runner
now obtains the installed SDK through xcrun. These setup failures are recorded,
not test passes.

Evidence: `work/oct11-native-tools/spawn-order/`, including final inputs,
compiler commands, raw logs, baseline and mutant outputs.

The official full `tools/build.sh sysroot` rebuild now includes all49 patches
in both libc.a and libc.so. Its actual spawn implementation matches the final
host-test implementation after the harness-only include substitution. Earlier
sysroot and Pouch program artifacts were preserved as APFS clones before the
build (which replaces both build directories); the existing Ninja executable
remains linked to its earlier libc.

`usr/pouch-hello/pouch-hello-spawn-actions.c` is an explicit native witness,
not a production boot probe. Build it with the rebuilt target sysroot:

```sh
tools/pouch-clang -O2 -Wall -Wextra -static \
  usr/pouch-hello/pouch-hello-spawn-actions.c -o /path/to/spawn-probe
```

Expose that binary read-only through Haul, then run its absolute guest path
from a writable ordinary-user directory as uid1000. It creates/removes
`spawn-target`; use an otherwise empty directory. The runtime7 fixture passes
all14 cases: ordered closes, reopen/reestablishment, chained sources,
three temporary-collision shapes, high parent sources and open failure.
Successful cases verify child uid, exact redirected bytes and wait status;
errors preserve the output pid. The lowest-free-slot check is bounded evidence
against low-slot leaks, not a complete handle-table leak census. Host mocks
separately check all temporary cleanup. Final probe, shell and sync exit0;
raw result assertions pass and the whole harness exits0 after QMP quit/serial
EOF. Host servers are reaped and the temporary host token removed.

The first native witness (runtime6) failed three cases because it predicted
an O_CREAT result from a /dev/null open. Pouch holds a parent-directory handle
during O_CREAT: the measured returned file slot is4, while /dev/null gets3.
That first run therefore missed the intended collision; the discarded-source
case instead exposed the already recorded NT2d limitation. Preserve its raw
failure, original source and disposition. The corrected witness measures and
closes an identical path/flags open before constructing actions; production
code was unchanged between runtime6 and runtime7. This is single-agent
self-review, not an independent audit.

Runtime7 retains the prior kernel and source-matched Stratum7dd103d daemon;
its source/binary/libc/initrd/image receipts and logs live under
`work/oct11-native-tools/`. The probe links static libc; rebuilding libc.so is
not a dynamic-loader qualification. No native Ninja self-build or production
bake is claimed. Fifteen obsolete October1 scratch disks were reclaimed with
SHA256 receipts, preserving their source/initrds/raw logs and baseline images.
Free space rose8.36 to11.32GiB before the fresh guest runs; the8448MiB runner
floor stayed in force.
