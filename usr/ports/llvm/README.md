# usr/ports/llvm — the Clade LLVM port (the durable Thylacine delta)

The Clade arc (`docs/LLVM-DESIGN.md`) builds a real `aarch64-thylacine` LLVM
toolchain (clang + lld) from upstream LLVM plus a **small, enumerable delta** —
the §4 "vendoring policy" shape. This directory is that delta's durable,
version-controlled home: the patches here are the *only* Thylacine-authored LLVM
source, and they live in the (mirrored) Thylacine repo so the work survives the
loss of any single machine.

## The pin

Upstream base: **`llvmorg-22.1.8`** (host brew is 22.1.x — same major; F2 pins to
the host major). The patches apply cleanly on that tag.

## The fork vs. these patches

Day-to-day iteration happens in a full local clone at
`~/projects/llvm-thylacine` (branch `thylacine`) — the `$LLVMFORK` the build
recipes point at, analogous to the Go arc's `$GOFORK`. That clone is **never
pushed**: its `origin` is the read-only upstream `llvm/llvm-project`, and a push
of the whole tree would be a ~291 MB near-duplicate of upstream (a shallow
clone's pack) for the sake of ~40 KB of actual change — GitHub is the wrong home
for it. **These patches are the durable form.** The fork is reconstructable from
them at any time; they are not reconstructable from a lost fork.

## Reconstruct the fork from these patches

```bash
git clone --depth 1 --branch llvmorg-22.1.8 \
    https://github.com/llvm/llvm-project.git ~/projects/llvm-thylacine
cd ~/projects/llvm-thylacine
git config user.email you@example.com && git config user.name you
git am <thylacine-repo>/usr/ports/llvm/patches/*.patch
# then build the host fork clang (the cross-compiler):
cmake -G Ninja -S llvm -B build -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS='clang;lld' -DLLVM_TARGETS_TO_BUILD=AArch64 \
    -DLLVM_ENABLE_{ZLIB,ZSTD,LIBXML2,TERMINFO,LIBEDIT,CURL,HTTPLIB,LIBPFM}=OFF
ninja -C build clang lld clang-resource-headers llvm-tblgen clang-tblgen
```

## Build the device toolchain

With the fork clang built, `tools/build.sh`:

- `build_clade` (`tools/build.sh clade`) — cross-builds the static
  `aarch64-thylacine` clang+lld multicall (`bin/llvm`) against the pouch sysroot,
  linked through the CL-3 Thylacine ToolChain. On a small-RAM host this is a long
  build; the GCP-VM path (`scratchpad`-staged `clade-vm-build.sh`) offloads it.
- `stage_clade` (`tools/build.sh stage-clade`) — assembles `build/clade/stage/`
  (`bin/{clang++,ld.lld}` copies + resource headers + sysroot).
- `THYLACINE_BAKE_CLADE=1 tools/build.sh all` — bakes `/clade` into the pool.

## The patches

- `0001` — CL-3: a real `Triple::Thylacine` + `ThylacineTargetInfo` + a
  Fuchsia-templated `Thylacine` clang ToolChain (ld.lld default, static/non-PIE),
  so `--target=aarch64-thylacine` resolves a real OS, not "unknown".
- `0002` — CL-3b: libc++abi's `__cxa_thread_atexit` recognizes `__thylacine__`
  (retires the CL-2 surgical `-D__linux__`; eliminates the int32/int64 ODR split).
- `0003` — CL-3b: libc++abi's `__cxa_guard` uses `pthread_self()` for the thread
  id (not `syscall(SYS_gettid)`, which is the ENOSYS sentinel on pouch → the
  concurrent-static-init false-abort).
- `0004` — CL-4: the Support-layer port — `getMainExecutable` via argv[0],
  `bit.h`'s `<endian.h>` arm, and `is_local`'s non-BSD-fallback, all for
  `__thylacine__` (the `__linux__`-family branches Thylacine falls through).

## Refresh (when the fork changes)

```bash
git -C ~/projects/llvm-thylacine format-patch --quiet llvmorg-22.1.8..HEAD \
    -o <thylacine-repo>/usr/ports/llvm/patches
```
