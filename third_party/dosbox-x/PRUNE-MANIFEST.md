# DOSBox-X vendored tree -- prune manifest

Vendored PRUNED-PRISTINE for the Thylacine **Cryptid** DOS/Win9x-emulation arc
(`docs/DOSBOX.md`, arc prefix DX). The kept files are byte-identical to upstream;
only whole non-source subtrees + one unused blob were dropped. Port glue (the
boundary-line patch series, the hand `config.h`, the curated object build) lives
in `usr/ports/dosbox-x/` + `tools/build.sh::build_dosbox_x`, mirroring the
TyrQuake idiom (`docs/reference/143-tyrquake.md`) -- the vendored tree is NEVER
edited.

## Provenance

- Upstream: DOSBox-X (https://github.com/joncampbell123/dosbox-x)
- Release tag: `dosbox-x-v2026.08.31`
- Commit: `4f19017c5f565dc40d01fede0f1382892e7243d7`
- License: **GPL-2.0-or-later** (`COPYING` is the GPLv2 text; every source file
  header carries "either version 2 ... or (at your option) any later version").
  Compatible with Thylacine's GPLv3 via the "or later" clause. No vendored
  GPL-2.0-only or GPLv3-incompatible component (audited at vendor time).

## Kept

- `src/`    -- all C++/C source (the emulator: cpu, dos, hardware, ints, gui,
              output, shell, builtin, misc, fpu, debug, aviwriter, gamelink,
              and the bundled libs under src/libs). The curated object build
              selects a subset; config.h gates the rest.
- `include/`-- all headers.
- `COPYING`, `AUTHORS`, `README.source-code-description`, `CHANGELOG` -- license
              + provenance.
- `dosbox-x.reference.conf` -- the default runtime config (for DX-2 first light).

## Dropped (whole subtrees; none is a compile input for the SDL2 software build)

- `vs/`             -- Visual Studio project + bundled Windows prebuilt libs
                       (151 MB). `vs/config.h` was the TEMPLATE for our hand
                       `usr/ports/dosbox-x/config.h`; not needed at build time.
                       EXCEPTION: `vs/sdl/src/cdrom/` is KEPT (4 files:
                       `compat_SDL_cdrom.h`, `SDL_cdrom.c`, `SDL_syscdrom.h`,
                       `dummy/SDL_syscdrom.c`) -- `src/dos/cdrom.h` reaches it
                       via "../../vs/sdl/src/cdrom/compat_SDL_cdrom.h" for the
                       SDL1 CD-ROM types + entry points that SDL2 dropped, and
                       `cdrom.cpp` calls SDL_CD*(). The `dummy` syscdrom backend
                       reports 0 host drives -- correct for Thylacine (no host
                       CD passthrough; CD *images* are handled by cdrom_image).
                       EXCEPTION: `vs/zlib/contrib/minizip/` is KEPT (7 files:
                       zip/unzip/ioapi .c+.h + crypt.h) -- `src/misc/savestates.cpp`
                       unity-includes "vs/zlib/contrib/minizip/{zip,unzip,ioapi}.c"
                       for zip-compressed savestates. The Windows (iowin32) + CLI
                       (miniunz/minizip) files are dropped.
- `contrib/`        -- fonts, translations, icons, GL shaders (66 MB). Runtime
                       resource data (res_DATA), not compile inputs; DX-1 links
                       without them. (TTF/PC-98 font data is a later concern; we
                       build with C_FREETYPE off.)
- `build-scripts/`  -- CI build scripts + their bundled deps (47 MB).
- `ref/`, `docs/`, `OLD-REFERENCE/`, `NOTES/`, `NOTES-TESTING-LOG/`,
  `snapshots/`, `optimization-1/`, `experiments/`, `patch-integration/`,
  `pc98-testme-1/`, `chk/`, `BUGS/`, `tests/`, `scripts/` -- docs, notes,
                       reference material, test logs.
- `ROMs/`           -- PC-98 / device ROM images (runtime data; some are
                       redistribution-restricted -- kept out deliberately).
- top-level `build*` wrappers, `*.sh`, `*.bat`, `*.cmd`, `*.pl`, `*.ps1`,
  `*.py` helper scripts, `*.png`/`*.bmp` logos, `Doxyfile`, `Makefile.am`,
  `configure.ac`, `acinclude.m4`, `autogen.sh` and the autotools scaffold --
                       we do a curated object build (no autotools), so the
                       generated build system is not vendored.

## Single-file drop

- `src/dos/exepackv1.bin` (283 bytes) -- the source-of-truth binary for the
  EXEPACK stub. The build consumes `src/dos/exepackv1.h` (the committed,
  generated `EXEPACKv1[]` byte array that `dos_execute.cpp` `#include`s); the
  `.bin` is never a compile input. Dropped because `*.bin` is gitignored
  repo-wide (the vendored-gitignore class): keeping it would make a fresh
  checkout differ from the worktree. The `.h` (text) is kept and is what
  compilation uses.

## Reproduce

```
git clone --depth 1 --branch dosbox-x-v2026.08.31 \
    https://github.com/joncampbell123/dosbox-x.git
# keep: src/ include/ COPYING AUTHORS README.source-code-description \
#       CHANGELOG dosbox-x.reference.conf
# then: rm src/dos/exepackv1.bin
```
