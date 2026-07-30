# usr/ports/mesa — the Clade Mesa/llvmpipe port (the durable Thylacine delta)

CL-7 (`docs/LLVM-DESIGN.md` §16) puts a real OpenGL implementation on Thylacine
by cross-building **Mesa's llvmpipe** software rasteriser against the
`aarch64-thylacine` LLVM from CL-7k, so that GL draw calls are compiled to
AArch64 machine code at runtime through the I-42 dual-mapped JIT. This directory
is that port's durable, version-controlled home: the patches here are the *only*
Thylacine-authored Mesa source.

## The pin

Upstream base: **`mesa-26.1.6`**. The patches apply cleanly on that tag.

## The fork vs. these patches

Iteration happens in a clone on the GCP builder (`thyla-keep:/build/src/mesa`,
branch `osmesa-resurrect-spike`). That clone is **never pushed** — its `origin`
is the read-only upstream `mesa/mesa`, and it lives on a disk that exists to be
thrown away. **These patches are the durable form**; the fork is reconstructable
from them at any time, and they are not reconstructable from a lost fork. This is
the same policy as `usr/ports/llvm`, and it matters more here, not less: a GCP
disk is less permanent than a laptop.

## Reconstruct the fork from these patches

```bash
git clone --depth 1 --branch mesa-26.1.6 \
    https://gitlab.freedesktop.org/mesa/mesa.git /build/src/mesa
cd /build/src/mesa
git config user.email you@example.com && git config user.name you
git am <thylacine-repo>/usr/ports/mesa/patches/*.patch
```

This is **verified, not asserted**: applying the series with `git am` to a
pristine `mesa-26.1.6` worktree reproduces the fork tip's tree hash exactly
(`bb4a37cca7488ca96813eff091e07bd190bbdaf0`). Re-check it after any refresh —
a patch series that no longer round-trips is a fork you have already lost.
(`git am` reports four trailing-whitespace warnings from the grafted 25.0.7
OSMesa source; they are cosmetic and it exits 0.)

## Cross-configure and build

Mesa's build system finds LLVM by *running* `llvm-config`, and a cross build has
none it can execute — the cross tree's own `bin/llvm-config` is an
`aarch64-thylacine` binary. Two repo tools bridge that (CL-7a-1):

- `tools/clade-llvm-config-cross.sh` — a cross `llvm-config` shim. It splits
  questions by authority: the component *graph* is delegated to the host
  `llvm-config` (the real implementation, which cannot drift from LLVM's
  dependency tables), while every *path* and target fact is read from the cross
  tree's generated headers and `CMakeCache.txt`.
- `tools/clade-mesa-cross.sh` — emits the meson cross file, deriving every path
  from that same `CMakeCache.txt`. Not a convenience: Mesa's objects link against
  that specific LLVM, so a cross file naming a different toolchain would describe
  something the library was never built with.

```bash
tools/clade-mesa-cross.sh emit /build/cl7a-cross.ini
meson setup /build/mesa-x /build/src/mesa --cross-file /build/cl7a-cross.ini \
    -Dgallium-drivers=llvmpipe -Dvulkan-drivers= -Dplatforms= \
    -Dllvm=enabled -Dshared-llvm=disabled -Dllvm-orcjit=true \
    -Dglx=disabled -Degl=disabled -Dgbm=disabled \
    -Dopengl=true -Dgles1=disabled -Dgles2=disabled -Dosmesa=true
ninja -C /build/mesa-x src/gallium/targets/osmesa/osmesa-prove
```

Three configure requirements are non-obvious and each one *builds cleanly when
set wrong* (CL-7 entry / CL-7a-1):

- `-Dllvm-orcjit=true` is **mandatory**. AArch64 is in Mesa's `llvm_has_mcjit`
  list, so the default silently selects MCJIT — which bypasses CL-7k's dual-map
  memory mapper entirely. Check for `-DGALLIVM_USE_ORCJIT=1` in the compile line.
- The cross LLVM must be built `-DLLVM_ENABLE_RTTI=ON`. The ORC backend uses
  `dynamic_cast`; without RTTI it cannot build. (Now set in `tools/build.sh`.)
- `llvm_modules` never contains `orcjit`, so `libLLVMOrcJIT.a` is absent from the
  link line that meson computes; the OSMesa target adds it explicitly. Upstream
  never notices because distros link a *shared* libLLVM, where every symbol is
  present regardless of the component list.

## The patches

- `0001` — CL-7: graft the 25.0.7 gallium OSMesa frontend onto 26.1.6. Upstream
  deleted the gallium OSMesa frontend after 25.0.x; OSMesa is the right target
  for Thylacine because it renders to a caller-supplied memory buffer and needs
  no window system, no DRM, and no dynamic loader. Resolved by measurement over
  EGL-surfaceless, which is *structurally* impossible here: `loader.c` `dlopen`s
  the gallium driver, and Thylacine has no dynamic loader at all.
- `0002` — CL-7a: build OSMesa as a **static archive** plus an `osmesa-prove`
  executable. Thylacine has no shared libraries, so the version script, the
  `.def` custom_target, the soversion triple and the symbols-check test all go.

  The executable is the point, and it earned its keep immediately. A
  `static_library` that builds proves almost nothing — an archive is a bag of
  objects and no symbol resolution happens while making one. The 210 MB
  `libOSMesa.a` built perfectly well while *missing every GL entry point*, and
  built again, just as happily, with only half of them. Only linking the
  executable ever said so.

  What it found: glapi is a **pair** at 26.1.6, and neither half is in libmesa
  (which the first cut of this file assumed). `libglapi`
  (`glapi/shared-glapi/core.c`) carries the `_mesa_glapi_*` dispatch and the
  noop table; `libglapi_bridge` (`glapi/glapi/libgl_public.c`) carries the 1300
  public `gl*` entry points, and its *only* undefined symbol is
  `_mesa_glapi_tls_Dispatch` from its partner. On Linux the split is invisible
  because `libGL.so` links the bridge and resolves the dispatch dynamically from
  `libglapi.so`; a static target has to name both halves.

  (Aside worth knowing if you touch this: aarch64 takes glapi's *generic C*
  entry path. `_GLAPI_ENTRY_ARCH_TLS_H` is defined only for x86, x86-64 and
  ppc64le, so the hand-written TLS assembly stubs — and the `#error
  "Unsupported architecture"` next to them — are simply not in play here.)
- `0003` — CL-7a-2: the OS-port layer. Registers Thylacine with Mesa's
  `detect_os.h` at the **`DETECT_OS_POSIX_LITE`** tier — the tier Fuchsia
  introduced, and the honest one: Thylacine has pthreads, mmap, poll,
  clock_gettime, nanosleep, sched_yield and BSD sockets, but no fork, no dynamic
  loader and no `/proc/self/exe`. Then four arms:
  - `os_time.c` — both `clock_nanosleep` arms. pouch's `__clock_nanosleep`
    (`usr/lib/pouch/patches/0022`) is torpor-backed and supports
    `CLOCK_MONOTONIC` *and* `TIMER_ABSTIME`, so Thylacine takes Mesa's
    *preferred* sleep path, not a degraded fallback.
  - `os_misc.c` — the `<unistd.h>` arm, exactly as Managarm was added to it.
    `os_get_total_physical_memory` / `os_get_page_size` need no arm: they are
    gated on `HAVE_SYSCONF`, which the cross configure detects from musl.
  - `log.c` — makes `u_process.h` visible. `log.c` calls
    `util_get_process_name()` under `#if !DETECT_OS_WINDOWS` but includes its
    header under `#if DETECT_OS_POSIX`, so *any* POSIX_LITE-only platform
    compiles the call with no declaration in scope. This is latent for Fuchsia
    too; fixing it there would mean widening the gate to POSIX_LITE, which would
    also newly compile `<syslog.h>` on Fuchsia — untestable from here, so
    Thylacine joins the arm rather than shipping an unverifiable change to
    someone else's platform.
  - `include/drm-uapi/drm.h` — takes the `__GNU__` (Hurd) escape to
    `<sys/ioctl.h>` instead of BSD's `<sys/ioccom.h>`. llvmpipe's `lp_texture.c`
    includes `drm_fourcc.h` under a bare `#ifndef _WIN32` and uses
    `DRM_FORMAT_MOD_LINEAR` under that same gate, so the include cannot just be
    skipped. `drm.h`'s `__linux__` arm wants `<linux/types.h>` and
    `<asm/ioctl.h>` — *neither* is in the pouch sysroot, so claiming `__linux__`
    would fail too (measured). Its `#else` arm is the portable one: it typedefs
    `__u8..__u64` from `<stdint.h>` itself and needs only the ioctl-encoding
    macros, which musl has in `bits/ioctl.h`. Thylacine is exactly the Hurd shape
    here.

## Refresh (when the fork changes)

```bash
git -C /build/src/mesa format-patch --quiet mesa-26.1.6..HEAD \
    -o <thylacine-repo>/usr/ports/mesa/patches
```
