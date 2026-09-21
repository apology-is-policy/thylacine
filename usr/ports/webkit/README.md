# usr/ports/webkit -- WebKit for Thylacine (the Boosty arc, `docs/BROWSER-DESIGN.md`)

**B-0.** The source is NOT vendored: WebKit is about 13 GB. It lives in a sparse,
partial clone beside the other forks, pinned to a release tag; this directory carries the
patch series. Gather and build:

```
tools/forage.sh webkit                 # the sparse clone at the pin + this series; the ICU tarball
tools/build.sh jsc                     # ICU (host tools, then cross) + JavaScriptCore
tools/build.sh kernel --config ci --set CHUNK_WEBKIT=y   # ...and bake /webkit/jsc
tools/test-interactive.sh ls-jsc       # the device gate
```

`CHUNK_WEBKIT` defaults OFF (~40 min cold on an 8-core / 8 GiB host). The pins live in
`tools/build-manifest.toml` (`[source.webkit]`, `[cache.icu4c]`) and are mirrored by
`WEBKIT_PIN` / `ICU_SHA256` in `tools/build.sh`; `tools/test-forage.sh` fails when they drift.

| | |
|---|---|
| Upstream | https://github.com/WebKit/WebKit.git |
| Pin | tag `webkitgtk-2.54.0` = `5220e80b97a253c60ed899361654142ab5021998` (2026-09-16) |
| Checkout | `~/projects/webkit-thylacine` (branch `thylacine`), sparse cone: `Source/JavaScriptCore Source/WTF Source/bmalloc Source/cmake Tools/Scripts` |
| ICU | 78.3, `icu4c-78.3-sources.tgz`, sha256 `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |

Recreate the checkout by hand (what `tools/forage.sh webkit` does):

```
git clone --filter=blob:none --no-checkout --depth 1 --branch webkitgtk-2.54.0 \
    https://github.com/WebKit/WebKit.git ~/projects/webkit-thylacine
cd ~/projects/webkit-thylacine && git sparse-checkout init --cone
git sparse-checkout set Source/JavaScriptCore Source/WTF Source/bmalloc Source/cmake Tools/Scripts
git checkout webkitgtk-2.54.0 && git checkout -b thylacine
git am /path/to/thylacine/usr/ports/webkit/patches/*.patch
```

What `build_icu` + `build_jsc` do, as first measured by hand:

1. ICU host build (`runConfigureICU MacOSX --disable-shared --enable-static ...`), then the
   cross build with `--host=aarch64-unknown-linux-musl --with-cross-build=<host dir>
   --with-data-packaging=static --disable-dyload`, CC/CXX = the fork clang, AR/RANLIB from
   the host LLVM. Put `--target=aarch64-thylacine` in CC itself: with it only in CFLAGS,
   ICU's dependency-generation steps compile for macOS.
2. `cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-aarch64-pouch-cxx.cmake
   -DTHYLACINE_EXTRA_ROOTS=<icu stage> -DPORT=JSCOnly -DENABLE_STATIC_JSC=ON -DENABLE_JIT=OFF
   -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF -DENABLE_C_LOOP=OFF -DUSE_SYSTEM_MALLOC=ON
   -DENABLE_SAMPLING_PROFILER=OFF -DENABLE_REMOTE_INSPECTOR=OFF -DDEVELOPER_MODE=OFF
   -DUSE_LIBBACKTRACE=OFF -DENABLE_WEBASSEMBLY=ON -DUSE_SYSTEM_UNIFDEF=ON -DICU_ROOT=<icu stage>`
3. `ninja -j5 jsc` (the host has 8 GiB of RAM). Result: a 60 MB static `ET_EXEC`, no
   `PT_DYNAMIC`, no writable+executable segment.

What the one patch changes, and why, is recorded in `docs/browser-status.md`.
