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
(`150990a` at 0017, verified on the vkQuake-arc W-1 close by `git am
0001..0017` onto a fresh `mesa-26.1.6` worktree -- the reconstructed tree
`9dd555ae986afc0afc9e94a135b3d0cc71a254f9` equalled the W-1 commit's tree
exactly; `5f9d0ce226057e7ef17aa29e963fada67c9ed72f` at 0016, verified on the
multi-queue audit close by `git am 0001..0016` onto a fresh `mesa-26.1.6`
worktree -- the reconstructed tree `85031d88de67` equalled the audit-close
commit's tree exactly; `deed314a45fa093b53e6f2008e11a70456ec8831` at 0015,
verified on the multi-queue chunk by `git am 0001..0015` --
the reconstructed tree `0575a6189666` equalled the multi-queue commit's tree
exactly; `d7f4ef1071fe74705b168f72ced4c00ca7d8bd3a` at 0014, verified on the V-3b-3c-2b
re-audit close by `git am 0014` onto the 2c742991 tip -- the reconstructed tree
`a403ed08ceba33756149e27729788238406d5246` equalled the re-audit commit's tree
exactly; `2c742991466c06129a014171faaa7b7a9f121e3a` at 0013, verified on the
V-3b-3c-2b close by `git am 0013` onto the 0012 tip -- the reconstructed tree
`41ee62527e878369f1bf1637c1e482b13977d3af` equalled the V-3b-3c-2b commit's tree
exactly; `c317dd6346ea09220a14862610f0d6589af348b3` at 0012, verified on the
V-3b-3c-1 close by `git am 0001..0012` of the emitted files onto a fresh
`mesa-26.1.6` worktree -- the reconstructed tree equalled the V-3b-3c-1 commit's
tree exactly; `b1bc565f29f2908762fc0e91f13eaf14443e1752` at 0011;
`21d8eef749eea16177403c48fd32310b564a04ef` at 0010;
`b117c8e52774ba3b85082e7a9004d8dc0387c4f8` at 0009; 0008 was
`88ade8b2af3d48b0ca3873e5fa955ef179895b44`, 0007
`cd00196c85cea3d92fe87351563b2c60d14d76cd`, 0006
`d302f50eb931bef25c8deab034093292adcc39ae`, 0005
`414b19f24384ae66d2107cbbab46cb7c963641e6`). Re-check it after any refresh —
a patch series that no longer round-trips is a fork you have already lost.
(The builder's fork matches the durable series exactly: the #204 cycle
committed 0008 on the fork, emitted the patch file, and re-`git am`'d the
EMITTED file onto the 0007 tip in a scratch worktree to confirm it lands on
`88ade8b2…` — a patch only known to match the working tree it came from has
not been checked at all.)
(`git am` reports four trailing-whitespace warnings from the grafted 25.0.7
OSMesa source; they are cosmetic and it exits 0.)

At CL-7b-2 that recipe stopped being a claim and became the *only* way the work
survived, so it was exercised end to end on a **different machine** (macOS,
different git) from the one the series was generated on. A fresh clone of the
tag plus `git am` of `0001..0004` reproduced
`b32a1ca2847e19d8aefb156313cfb7084597d253` — the hash recorded here before this
refresh — which is what makes the reconstruction a fact about the *patches*
rather than a fact about one disk. `0005` was generated from that
reconstruction and then re-applied *from the emitted file* onto the `0004` tip
to confirm it lands on `414b19f2…`; a patch that is only known to match the
working tree it came from has not been checked at all.

```bash
tools/clade-stage1.sh --reconcile-only \
    --patches usr/ports/mesa/patches --tag mesa-26.1.6 --src /build/src/mesa
```

reconciles a builder fork against this series. Since #117 it handles *growth*
correctly: it compares `git patch-id --stable` per commit and `git am`s only the
patches the fork does not already carry. It refuses if the fork holds Thylacine
changes that are **not** in the series, which is the correct behaviour and
exactly the state this directory exists to prevent. A drifted fork is not worth
reconciling by hand: reset to the tag and re-run the recipe above.

**`--reconcile-only` is not optional here, and the flag exists because this
paragraph was wrong.** It previously claimed the script was "generic, nothing in
it is LLVM-specific". The reconcile *is* generic; the script around it was not —
`--src` pointing at a Mesa fork died on an LLVM-tree assert hundreds of lines
before reaching it, and everything after the reconcile is a cmake+ninja of LLVM.
Nothing catches that: no build parses this file, so the recipe sat here reading
as verified for as long as nobody ran it. The flag was added (#120) to make the
claim true rather than to withdraw it.

### The fork was checked against this series, and the check had to be a different one

#120 asked whether the builder's fork carried anything beyond what the
CL-7b-2 script applied — reconstruction captures only what it *knows* about, so
an interactive fix made directly on the builder would be invisible from here.
The reconcile above could not answer it, for a second reason beyond the flag:
**the 0005 changes were never committed on the builder.** They sat as
modifications to two tracked files, so `git log mesa-26.1.6..HEAD` showed four
commits, and `git am` refuses onto a dirty tree anyway.

What answers it for an uncommitted delta is `patch-id` on the *working-tree
diff*, which is what a patch file's identity is computed from too:

```bash
git -C /build/src/mesa diff | git patch-id --stable          # the fork
git patch-id --stable < usr/ports/mesa/patches/0005-*.patch  # the durable form
```

Both returned `32e073482afff8c60cc237e15aedbe510fa18ef5`, with `git status
--porcelain` showing exactly those two files and no untracked ones. So the
reconstruction is complete: **the fork holds nothing this directory does not.**
(`clade-stage1.sh` now refuses a dirty tree by name rather than letting `git am`
fail with a message that mentions neither the series nor the script.)

**The builder's fork is still in that state, deliberately.** A reconcile there
will refuse until someone commits the 0005 delta or discards it — which is the
guard working, not a surprise. It was left alone because the durable form is
already safe: these patches are pushed, and the fork is reconstructable from
them, so nothing is at risk in the way CL-7b-2's `/private/tmp`-only script was.
Restarting a builder to tidy a scratch tree buys nothing the repo does not
already hold.

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

That is the recipe for a *fresh* tree. The one that exists on the builder is
**`/build/mesa-xOS5`** — the survivor of twenty `mesa-*` iteration dirs, and the
tree that produced every shipped `osmesa-prove`. Rebuild there unless you mean
to pay a full configure: it is ~900 objects warm, so picking up a new LLVM
archive is a relink of seconds rather than a build of minutes. `build.ninja`
does carry `libLLVMOrcJIT.a` as a declared dependency, so an LLVM-only change
is seen — the relink does not need forcing (checked, #120).

The fetched binary is copied to `build/clade/gl/osmesa-prove` on the dev host.
**`tools/build.sh stage-clade` must be run before any pool bake that should
carry it**: the `all` path re-stages `/storm` but *not* `/clade` (its comment
says so — the multi-hundred-MiB copy is deliberately manual), so a plain
`THYLACINE_BAKE_CLADE=1 tools/build.sh all` will happily re-mint the pool
around the *previously* staged binary and report success. The #101 bake-verify
does not catch it either: it gates on `/clade/bin/clang++`, so it proves the
tree is present, never that this binary is current.

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

## Fetching the GL link artifacts (#138)

`osmesa-prove` is a finished executable, so shipping it needs nothing but the
binary. Linking *another* GL program — `gl-sdl-prove`, and later GLQuake —
needs the link inputs on the dev host. They are not built here; they come off
the builder once and stay in `build/`:

| what | from (builder) | to (dev host) | size |
|---|---|---|---|
| `libOSMesa.a` + `libz.a` | `/build/mesa-xOS5/…` | `build/clade/gl/lib/` | 205 MB |
| 73 `libLLVM*.a` | `/build/src/thylacine/build/clade/llvm-build/lib` | `build/clade/llvm-build/lib/` | 160 MB |
| `GL/` + `KHR/` headers | `/build/src/mesa/include` | `build/sysroot/include/` | 1.4 MB |
| the archive list | derived (below) | `build/clade/gl/llvm-libs.list` | — |

**None of this is required to build the tree (#239).** It used to be, by
accident: `SDL_thylacineopengl.c` includes `<GL/osmesa.h>`, so a checkout
without the headers could not build `libSDL2.a` — and SDL2 is built before the
ramfs bake, so a missing *optional* GPU stage took out the kernel image with
it. `build_sdl2` now compiles `usr/ports/sdl2/thylacine-nogl/` instead when
`build/sysroot/include/GL/osmesa.h` is absent, announces it, and records the
mode in `build/sysroot/lib/.libSDL2.gl-mode` so that fetching the headers later
invalidates the archive (no timestamp would). See `docs/reference/142-sdl-port.md`,
"The headers are optional too".

Note the trap in the destination column while you are here: the headers land
**inside** `build/sysroot/include/`, which `build_sysroot` recreates. Fetching
them once is not permanent — any pouch-patch edit rebuilds the sysroot and
takes them with it, and the next SDL build silently drops to `nogl` (loudly, as
of #239, but it still drops). Re-fetch after a sysroot rebuild, or expect a
GL-less `libSDL2.a`.

**Derive the archive list, never type it.** It is exactly what meson computed
for `osmesa-prove`'s own link, so a list written by hand can only drift from
the one that is known to close:

```bash
cd /build/mesa-xOS5
ninja -t commands src/gallium/targets/osmesa/osmesa-prove > /tmp/cmds.txt
tail -1 /tmp/cmds.txt > /tmp/link.txt          # the link line
tr ' ' '\n' < /tmp/link.txt | grep '^-lLLVM' | sort -u > /tmp/llvmlibs.txt
```

Three things about that set are not guessable and cost a round each:

- **Five of the seven Mesa archives are meson THIN archives** (`!<thin>` —
  a few KB of pathnames into the builder's object tree). They are meaningless
  on another machine, and ld.lld says so obliquely: "could not get the buffer
  for a child of the archive". They are also *redundant* — `libOSMesa.a` is a
  fat 214 MB archive of 899 members that `link_whole` already merged them
  into. Fetch `libOSMesa.a` and `libz.a` (a subproject, hence fat) and drop
  the rest. Verify rather than assume: `llvm-ar t libOSMesa.a | grep lp_bld`.
- **The GL headers have a dependency closure**, and it is not the directory
  you copied. `GL/gl.h` includes `GL/glext.h` includes `<KHR/khrplatform.h>`,
  which lives in `include/KHR/`, not `include/GL/`. Compute it on the builder
  (`grep -rh '^#include <' GL/ KHR/`) rather than discovering it one failed
  compile at a time — and take it from *this* tree rather than the Khronos
  registry, because the types it defines (`khronos_intptr_t` and friends) are
  what `GLsizeiptr`/`GLint64` are built from, i.e. ABI.
- **The LLVM archives must NOT go in `build/sysroot/lib/`.** That directory is
  staged to `/clade/sysroot` and baked into the pool, so 160 MB of link-only
  archives would ride along into every image. `build/clade/llvm-build/lib` is
  the right home: `stage_clade` copies `bin/` and `lib/clang/*/include` from
  that tree and nothing else. The headers are small enough that the sysroot is
  the right place for them, and putting them there also lets the on-device
  clang++ compile GL sources.

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

- `0004` — CL-7b-1: JIT through the I-42 dual-mapped code Burrow. Three edits,
  and the first was found only by checking a symbol after a build that reported
  success.

  **`USE_JITLINK` had to gain `__thylacine__`, and this is the load-bearing
  one.** That macro selects the object *linking layer* and is a **different
  axis** from `GALLIVM_USE_ORCJIT`, which selects ORC vs MCJIT — conflating them
  cost a build here. aarch64 is not in the `USE_JITLINK` list (RISCV,
  LoongArch and Win32 only), so the ORC path still ran on
  `RTDyldObjectLinkingLayer`, whose `SectionMemoryManager` allocates RW and then
  calls `mprotect` to add `PROT_EXEC`. Thylacine has no mprotect at all, and
  `MemoryMapper` is a **JITLink-only** seam — so on RTDyld the dual-map mapper
  is never even consulted. Third wrong-default-that-builds-clean in this arc
  after `llvm_has_mcjit` and `LLVM_ENABLE_RTTI`, and the quietest of the three:
  the build succeeds, the binary links, and the fault is at runtime.

  With JITLink selected, the `ObjectLinkingLayer` takes
  `MapperJITLinkMemoryManager` over `DualMapMemoryMapper` (llvm-thylacine)
  instead of `InProcessMemoryManager`. Reservation granularity is 1 MiB: each
  reservation is one `SYS_JIT_CREATE`, charged whole against the per-Proc page
  budget (I-32), and a larger allocation rounds up to a multiple of it, so
  nothing is capped by the choice.

  `osmesa_prove.c` also calls `SYS_JIT_CREATE` directly before any of Mesa runs.
  llvmpipe reaches the same syscall eventually, but behind enough machinery that
  a missing capability, a broken mapper and an unrelated gallivm fault all
  present identically as "OSMesaCreateContextExt returned NULL".

- `0005` — CL-7b-2: the process-symbols JITDylib, and `CAP_JIT` as a clearance
  the program walks for itself. Two independent things stood between an
  `osmesa-prove` that *links* and a JIT that *runs*, and both present as that
  same NULL context.

  LLJIT links process symbols by default, and its default setup reaches them
  through `dlopen(NULL)` — which a statically linked musl binary cannot do, so
  `LLJITBuilder::create()` itself fails with "Dynamic loading not supported" and
  the JIT never comes up. Turning the flag *off* does not help: the generic LLVM
  IR platform then refuses with "Native platforms require a process symbols
  JITDylib". Neither state is reachable by configuration, so the JITDylib is
  supplied explicitly, populated from a table fixed at link time
  (llvm-thylacine's `ThylacineProcessSymbols.cpp`).

  `CAP_JIT` is **elevation-only**: stripped at every fork, so no parent can hand
  it to a child at spawn and no amount of privilege in joey helps. The only way
  any Proc ever holds it is to walk the corvus clearance path itself and redeem
  the grant, which `osmesa_prove.c` now does before any GL runs. That is the
  concrete sense in which a GL program on Thylacine is a *capability client*
  rather than an ordinary binary. Reaching corvus needs `SYS_OPEN` (65), **not**
  `SYS_WALK_OPEN` (34): 34 walks exactly one path component, so it cannot
  resolve `/srv/corvus`. Measured, not reasoned — with 34 the open failed every
  retry and the prover reported "cannot reach /srv/corvus" while `jit-prover`
  connected fine in the same boot.

  The probe also becomes a **verdict**. It returned `void` and only printed, so
  a boot with no `CAP_JIT` ran on to `OSMesaCreateContextExt` anyway and reported
  the generic NULL-context failure — indistinguishable from a broken mapper or
  an unrelated gallivm fault, which is precisely the ambiguity `0004` had already
  been bitten by. It now returns non-zero at every station, and that is what lets
  joey's `gl_gate` treat `rc=0` as a strong assertion and parse no output at all.

- `0006` — Warp-3: the virgl winsys over the /srv/warp GPU seam. The
  unmodified virgl gallium driver plus a new winsys
  (`src/gallium/winsys/virgl/thylacine/`) whose transport is
  `warp_client.c` — blocking file ops on tapestryd's warp tree via raw
  Thylacine syscalls (`<thyla/syscall.h>`, carried into the build by the
  generated cross file's c_args). vtest's 18 load-bearing slots minus
  the displaytarget arms; fences counted client-side against the seam's
  monotonic per-ctx `fence-signaled`; submits split at CCMD boundaries
  under the 32 KiB msize. Meson gating keys on
  `with_thylacine = cc.get_define('__thylacine__')` because the cross
  file deliberately claims `system = 'linux'`. Configure adds virgl
  (and, since Warp-4/#194, kills the driconf/expat pair):

  ```
  -Dgallium-drivers=llvmpipe,virgl -Dxmlconfig=disabled -Dexpat=disabled
  ```

  (a meson reconfigure on the existing tree, not a fresh setup; with
  the 26.1.6 meson floor that is `/build/venv-meson/bin/meson setup
  --reconfigure`, and it must run AGAIN after any missing-LLVM-archive
  repair -- meson caches a failed `--libs` answer and reports "LLVM
  found: YES" over empty link args). The osmesa target then carries
  both drivers; `GALLIUM_DRIVER=virpipe` (or `virgl`) selects the warp
  screen at runtime, with a loud fallback to llvmpipe. `virgl-prove`
  is the new gate binary beside `osmesa-prove`: it never walks the
  CAP_JIT clearance (so a silent llvmpipe fallback cannot pass it) and
  asserts GL_RENDERER names virgl.

  Six port findings from the builder cycles, all folded into the patch
  or the configure, all of the builds-clean-when-wrong class:

  - the osmesa target needed `inc_virtio` (`virgl_winsys.h` includes
    `virtio-gpu/virgl_hw.h`);
  - `<libsync.h>` resolves upstream from LIBDRM's installed copy, which
    dropping `dep_libdrm` orphaned -- Thylacine points the include path
    at Mesa's own vendored, self-contained `src/util/libsync.h`
    instead. NOTE: this slipped GPU-DESIGN 2.3's portability census
    because `libsync` matches none of the grepped patterns
    (`__linux__` / `<sys/*>` / `DETECT_OS`) -- that census was
    pattern-bounded, not complete;
  - `inline_sw_helper.h`'s own `virpipe` arm hardwires
    `virgl_vtest_winsys_wrap` under `GALLIUM_VIRGL`; on Thylacine the
    arm is dead (the osmesa target forks to the warp winsys first) and
    is guarded out so it cannot drag the vtest symbol into the link.
    The target also links `libgalliumvl_stub` -- virgl's screen
    references the gallium video layer, which llvmpipe never did;
  - (#191, found by the tri gate, not the builder) virgl's disk-cache
    keying `assert(note)`s a build-id note into existence, but the
    static `ld.lld` ET_EXEC never passes `--build-id` and carries none,
    so screen creation aborted in-guest before any triangle. The fork
    makes the keying contribution conditional: no note -> return with
    `disk_cache` NULL (the configuration `MESA_SHADER_CACHE_DISABLE`
    ships, NULL-handled by every consumer) rather than link-flagging
    `--build-id`, which would have *activated* the cache onto the 9P
    home and its untested flock/mmap surface;
  - (#194, found by the FIRST guest-side relink after Warp-3, not the
    builder) virgl pulled `util/xmlconfig` (driconf), and meson's
    `expat` feature auto-resolved through the `subprojects/expat.wrap`
    FALLBACK -- it silently built a cross libexpat.a, linked the
    builder-side provers against it, and the #138 fetch set never
    carried it. Every builder artifact was green; every GUEST hand-link
    of the archive (`gl-sdl-prove`, the GLQuake relink) was broken with
    undefined `XML_*`, and build caching hid it until the Warp-4 shim
    edit forced the first relink. Configure now disables the pair
    (driconf is dead weight on Thylacine -- no /etc/drirc, defaults
    only): `-Dxmlconfig=disabled -Dexpat=disabled` (both: expat cannot
    be disabled while xmlconfig is enabled). Verified in the artifact:
    `nm -u libOSMesa.a | grep -c XML_` is 0;
  - (Warp-4) `osmesa_target.c`'s present bridge includes
    `virgl_screen.h`/`virgl_resource.h`, whose chain reaches NIR's
    GENERATED headers (`glsl_types.h -> builtin_types.h`);
    `driver_virgl` links the driver but does not propagate that include
    path, so the target's meson gains `idep_nir_headers`.

## Refresh — per arc, not "when the fork stops changing"

```bash
git -C /build/src/mesa format-patch --quiet mesa-26.1.6..HEAD \
    -o <thylacine-repo>/usr/ports/mesa/patches
```

**Refresh at the close of every arc**, even mid-iteration. The tempting policy —
"regenerate once the fork settles" — is backwards, because it leaves the durable
form maximally stale exactly while the work is newest, least reproducible, and
most expensive to re-derive. CL-7b-2 is the worked example: its six Mesa edits
lived only in a throwaway script in `/private/tmp` while the arc was declared
complete, so the most valuable state in the port had no copy in any repo, on
either machine, at the moment it mattered most. The same lag had grown to eight
commits on `usr/ports/llvm` in the same arc.

The cost of refreshing early is a patch file that gets superseded. The cost of
refreshing late is measured in builder rounds, and once, nearly in the work
itself.
