---
id: dec-2026-10-14-b3-port-policy
type: dec
title: "B-3 port policy: fetched sources, current stable releases, static archives, OpenSSL by getauxval, names through /net/dns"
date: 2026-10-14
status: standing
decided-by: research-collapsed
affects: [sub-substrate-build, sub-pouch-net, sub-webkit]
created: 2026-10-14
---
## Fork

The B-3 survey (main's memory note design-b3-survey) listed ten possible
votes. Research collapsed five of them to one answer each; the other five
were voted ([[dec-2026-10-14-b3-raster-skia]],
[[dec-2026-10-14-b3-fontconfig-port]], [[dec-2026-10-14-b3-test-bar]],
[[dec-2026-10-14-b3-sqlite-9p-locks]], [[dec-2026-10-14-native-fd-calls]]).

## Research

- **Source**: large upstream tarballs are fetched through
  `tools/build-manifest.toml` with a sha256 and gathered by `tools/forage.sh`
  (ICU 78.3, the browser arc's own precedent); small ones are vendored in
  `third_party/` (zlib, libsodium).
- **Versions**: WebKit's floors (`OptionsPlayStation.cmake`): ICU 70.1, curl
  7.85.0, JPEG 1.5.2, LibPSL 0.20.2, LibXml2 2.9.7, SQLite 3.23.1, zlib
  1.2.11; fontconfig 2.13 and FreeType 2.9 for Skia. OpenSSL 3.5 is the
  current LTS; BoringSSL has no releases.
- **Static vs shared**: `cmake/Platform/Thylacine.cmake` is static-only,
  while `libc.so` and dlopen exist since B-1d. How WebKit's processes share
  code is B-5's process-layout question.
- **OpenSSL CPU features**: OpenSSL 3.5's `crypto/armcap.c:69-80` uses
  `getauxval(AT_HWCAP)` only under glibc, Android, FreeBSD or OpenBSD;
  everything else runs a SIGILL probe from a load-time constructor, and Pouch
  delivers no catchable SIGILL ([[dec-2026-10-14-self-death-snare]]). musl
  provides `getauxval`, and the kernel publishes `AT_HWCAP`
  (`kernel/exec.c`).
- **ICU file data**: `umapfile.cpp` maps `MAP_SHARED` and does not fall back;
  `-DU_HAVE_MMAP=0` selects its `fopen`/`fread` path. ICU's own data is linked
  statically.
- **Name resolution**: Plan 9 programs never speak DNS themselves -- APE's
  `gethostbyname` asks `/net/cs`; Thylacine's netd serves `/net/dns` and
  `/net/cs`, and the namespace is the firewall.

## Options

Each item had one option the research left standing; the alternatives
(vendoring multi-megabyte tarballs, tracking WebKit's minimums, shared
libraries before B-5 designs the process layout, SIGILL delivery, a
`MAP_SHARED` file map for ICU, a libc DNS client beside netd's) each
contradict a fact above.

## The call

- Sources fetched through the build manifest with a sha256, each with a
  `third_party/README.md` provenance row.
- The current stable release of each library at port time, at or above
  WebKit's floor; OpenSSL 3.5 LTS.
- Static archives, built position-independent, so B-5 can link either way.
- OpenSSL reads CPU features through `getauxval`, by a patch in its series.
- ICU built with `U_HAVE_MMAP=0`.
- `getaddrinfo` resolves through netd's `/net/dns` by a Pouch patch.

## Rationale

Each call follows a precedent already in the tree or the heritage, and none
adds kernel surface. They are recorded so a later chunk does not reopen them.
