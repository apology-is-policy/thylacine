# pouch-libsodium — the first cross-compiled C library against pouch

Phase 6 sub-chunk 14 (`pouch-libsodium`). Cross-compiles **libsodium**
(Frank Denis's modern crypto library — ISC, ~520 KiB of static archive)
for `aarch64-thylacine` against the pouch sysroot, installs it into the
sysroot, and runs a known-answer-test (KAT) round-trip in Thylacine via
`/pouch-hello-sodium`. The chunk's purpose is **not** to ship libsodium
to userspace consumers (Stratum is the only one); it is to **prove the
pouch cross-toolchain** by building a real-world non-trivial C library
against it. The chunk is not audit-bearing per `POUCH-DESIGN.md §14` —
libsodium is portable C, no patch series, no new kernel surface.

## What landed

| Component | Path | Size |
|---|---|---|
| Vendored upstream | `third_party/libsodium/` (583 files) | 12.5 MiB on disk |
| Built archive | `build/sysroot/lib/libsodium.a` | 520780 bytes |
| Installed umbrella header | `build/sysroot/include/sodium.h` | 2774 bytes |
| Installed primitive headers | `build/sysroot/include/sodium/*.h` | 71 files |
| Generated header | `build/sysroot/include/sodium/version.h` | substituted at build time |
| Build step | `tools/build.sh::build_libsodium` | called by `build_sysroot` after `build_compiler_rt` |
| Proving binary | `/pouch-hello-sodium` (`usr/pouch-hello/pouch-hello-sodium.c`) | 276848 bytes (ET_EXEC, static) |

## Why no patch series

libsodium is **portable C** — every crypto primitive is pure computation
(field arithmetic, bit twiddling, table lookups) and the only OS contact
is the CSPRNG seed and the optional `mlock`/`madvise`/`mprotect`
hardening hints. The CSPRNG path goes through musl's `getentropy(3)` →
`getrandom(2)` → `SYS_GETRANDOM` (sub-chunks 4 + 11). The hardening
hints reach pouch's `0xFFFF` sentinel for `mlock`/`madvise`/`mprotect`
and return `ENOSYS`; libsodium's `sodium_mlock` and `sodium_mprotect`
gracefully treat `-1` as advisory. There is no OS-boundary code in
libsodium that needs retargeting, so the byte-pristine vendoring at
`third_party/libsodium/` has **no patch series** — same model as
compiler-rt.

## Why no `./configure`

`./configure --host=aarch64-thylacine` is fragile on a macOS host (the
triple is unknown to `config.sub` 's gnu-platform table; autoconf's link
probes try to build + run a test program and fail without a runtime).
The pragmatic alternative is to **supply the `HAVE_*` configuration
macros that `./configure` would normally emit via `-D` flags on the
compile command line**, and to generate `sodium/version.h` from
`version.h.in` by hand. That is `build_libsodium`'s job.

The macros that matter on `aarch64-thylacine`:

| Macro | Defined? | Rationale |
|---|---|---|
| `CONFIGURED` | 1 | Suppresses `private/common.h` "compiled by an undocumented method" warning. |
| `NATIVE_LITTLE_ENDIAN` | 1 | aarch64 is little-endian — selects the fast `memcpy` byte-load. |
| `HAVE_TI_MODE` | 1 | clang has `__int128` — selects the faster `fe_51` field-element representation for curve25519. |
| `HAVE_C_VARARRAYS` | 1 | C99 VLAs (clang supports). |
| `HAVE_INTTYPES_H` / `HAVE_STDINT_H` | 1 | musl provides. |
| `HAVE_SYS_MMAN_H` / `HAVE_SYS_PARAM_H` / `HAVE_SYS_RANDOM_H` / `HAVE_SYS_AUXV_H` | 1 | musl provides. |
| `HAVE_PTHREAD` | 1 | sub-chunk 9b wires it. |
| `HAVE_C11_MEMORY_FENCES` / `HAVE_GCC_MEMORY_FENCES` / `HAVE_ATOMIC_OPS` | 1 | clang 22 builtins. |
| `HAVE_INLINE_ASM` | 1 | clang supports `__asm__`. |
| `HAVE_WEAK_SYMBOLS` | 1 | ELF static-link weak symbols. |
| `HAVE_MMAP` | 1 | sub-chunk 7b. |
| `HAVE_RAISE` | 1 | sub-chunk 13b. |
| `HAVE_SYSCONF` | 1 | musl provides — reads from auxv. |
| `HAVE_GETRANDOM` / `HAVE_GETENTROPY` / `HAVE_LINUX_COMPATIBLE_GETRANDOM` | 1 | sub-chunks 4 + 11. The Linux-compatible flag bypasses the `__linux__` conditional in `randombytes_sysrandom.c` so libsodium uses musl's `getrandom(2)` directly (a `/dev/urandom` fallback isn't reachable from pouch). |
| `HAVE_GETAUXVAL` | 1 | musl provides — returns 0 for `AT_HWCAP` (pouch's auxv carries only `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/`AT_PAGESZ`/`AT_RANDOM`/`AT_NULL`; the 0 return makes libsodium fall back to the portable refs, which is what we compile anyway). |
| `HAVE_GETPID` | 1 | musl provides — returns `-1` (`ENOSYS`) at runtime; harmless for libsodium's fork-detection (the `-1` value never matches a "new" pid). |
| `HAVE_POSIX_MEMALIGN` | 1 | mallocng (sub-chunk 7b). |
| `HAVE_EXPLICIT_BZERO` | 1 | musl 1.2.5 provides. |
| `SODIUM_STATIC` | 1 | Expands `SODIUM_EXPORT` to nothing (no visibility attribute needed for a static-archive build). |
| `HAVE_AMD64_ASM`, `HAVE_AVX_*`, `HAVE_*INTRIN_H`, `HAVE_RDRAND`, `HAVE_CPUID`, `HAVE_CET_H`, `HAVE_ARMCRYPTO`, `HAVE_ANDROID_GETCPUFEATURES`, `HAVE_INTRIN_H`, `HAVE_ARC4RANDOM`, `HAVE_ELF_AUX_INFO`, `HAVE_MEMSET_S`/`HAVE_MEMSET_EXPLICIT`/`HAVE_EXPLICIT_MEMSET`, `HAVE_CATCHABLE_ABRT`/`HAVE_CATCHABLE_SEGV`, `HAVE_NANOSLEEP`, `HAVE_CLOCK_GETTIME`, `HAVE_MLOCK`, `HAVE_MADVISE`, `HAVE_MPROTECT`, `HAVE_ALIGNED_MALLOC` | NOT defined | x86-only / non-pouch-OS / unsupported on pouch (`sigaction` for `SIGABRT`/`SIGSEGV` is `EINVAL` per sub-chunk 13b; `mlock`/`madvise`/`mprotect`/`nanosleep`/`clock_gettime` all return `ENOSYS` via pouch's `0xFFFF` sentinel — leaving them undefined is more honest than defining them and silently returning `-1`). |

## The source list

Curated from `src/libsodium/Makefile.am`, the `libsodium_la_SOURCES` set
plus the `!HAVE_AMD64_ASM` (ref salsa20), `!EMSCRIPTEN` (randombytes), and
`!MINIMAL` (full API surface) arms. 97 `.c` files compiled. Excluded:

- **x86 ASM** — AESNI, SSE2/3/4.1, AVX, AVX2, AVX512F, sandy2x. None
  reachable on aarch64.
- **ARMv8 crypto extension sources** — `libarmcrypto_la` (`aegis128l_armcrypto.c`,
  `aegis256_armcrypto.c`, `aead_aes256gcm_armcrypto.c`). These need
  `-march=...+crypto`; pouch's baseline is `-march=armv8-a+lse+pauth+bti`
  (no `+crypto`). The portable refs implement every primitive, so AES-GCM
  and AEGIS still work — they just don't get the hardware acceleration.
  Future work: enable `+crypto` opportunistically when the platform
  reports `AT_HWCAP` AES, then conditionally link the `armcrypto` arms.

The list is hard-coded in `build_libsodium`, with a `[[ ${#sources[@]}
-lt 90 ]]` sanity gate to catch a re-vendor that drops sources.

## The build

`build_libsodium` is called by `build_sysroot` after `build_compiler_rt`
(libsodium needs the compiler runtime for `__int128` arithmetic helpers).
The flow:

1. Generate `$obj/gen/sodium/version.h` from `version.h.in` via `sed`
   (substituting `@VERSION@` → `1.0.20`, `@SODIUM_LIBRARY_VERSION_MAJOR@` →
   `26`, `@SODIUM_LIBRARY_VERSION_MINOR@` → `2`, `@SODIUM_LIBRARY_MINIMAL_DEF@`
   → empty for a non-minimal build).
2. Compile each `.c` file with `clang --target=aarch64-thylacine
   -march=armv8-a+lse+pauth+bti -O2 -fno-pic -fno-stack-protector
   -nostdinc -isystem $sysroot/include -I$obj/gen/sodium
   -I$src/include -I$src/include/sodium <HAVE_* defines>` into
   `$obj/<flat-name>.o`. 97 invocations, parallelizable (the current build
   runs them serially in a for-loop — under 5 seconds even so).
3. Archive into `$sysroot/lib/libsodium.a` via `llvm-ar rcs`.
4. Install headers: `cp $src/include/sodium.h $sysroot/include/sodium.h`
   plus all `$src/include/sodium/*.h` and the generated `version.h` into
   `$sysroot/include/sodium/`.
5. Verify symbol presence: `llvm-nm --defined-only` must show `sodium_init`,
   `randombytes_buf`, the AEAD encrypt/decrypt pair, `crypto_hash_sha256`,
   `crypto_generichash`, the ed25519 trio, and `sodium_version_string`.
   A missing symbol surfaces vendor / source-list drift before
   `/pouch-hello-sodium` even links.

## The link line

`build_pouch_progs` handles `pouch-hello-sodium` specially: it appends
`-L$sysroot/lib -lsodium` to the per-binary linker args before
`pouch-ld`'s standard libset. The resulting link order is:

```
crt1.o crti.o pouch-hello-sodium.o -L$sysroot/lib -lsodium
    -L$sysroot/lib --start-group -lc libclang_rt.builtins.a --end-group
    crtn.o
```

libsodium references are resolved against `libc.a` (memset, memcpy,
write, getentropy, getrandom, pthread_mutex_lock, ...) and
`libclang_rt.builtins.a` (`__int128` arithmetic helpers); the group
between `libc` and `libclang_rt` handles the mutual references between
the two. `-static` + `-z noexecstack` + `--build-id=none` come from
`pouch-ld`'s standard policy.

## The proving binary

`/pouch-hello-sodium` (276848 bytes static ET_EXEC) exercises five
primitives in order. Each prints a status line; exit 0 only if all pass.

| Probe | What it proves |
|---|---|
| 1. `sodium_init()` returns `0` | Library initialization works: critical-section pthread mutex (sub-chunk 9b), `_sodium_runtime_get_cpu_features` (auxv via `getauxval`), `randombytes_stir` (CSPRNG seeding), and the various `pick_best_implementation` calls all complete without aborting. |
| 2. SHA-256 of `"abc"` matches the FIPS 180-4 KAT | The portable upper half (musl's string ops + libsodium's sha256 ref) is computing the correct hash. |
| 3. BLAKE2b-256 round-trip | Two consecutive `crypto_generichash` calls on the same input must produce the same 32 bytes — exercises BLAKE2b's compression function. |
| 4. xchacha20-poly1305-IETF AEAD round-trip | The CSPRNG (`randombytes_buf` → `getentropy` → `SYS_GETRANDOM`), the AEAD encrypt path, the AEAD decrypt path with Poly1305 tag verification. Decrypted plaintext must equal original. |
| 5. ed25519 sign + verify + reject-tampered | `crypto_sign_keypair` (curve25519 / GF(2^255 - 19) arithmetic, `HAVE_TI_MODE` 128-bit codegen), `crypto_sign_detached` (Schnorr-style signing over SHA-512), `crypto_sign_verify_detached` (must accept valid + reject tampered). |

The binary is spawned by joey via `pouch_smoke_one_caps` with
`T_CAP_CSPRNG_READ` — same path that `/pouch-hello-getrandom` proved at
sub-chunk 11. The cap is required because libsodium's CSPRNG init calls
`getentropy(3)` which routes through `SYS_GETRANDOM`, gated on
`CAP_CSPRNG_READ`.

## Why the proving binary is not the full `make check`

POUCH-DESIGN.md §13's exit criterion reads "libsodium cross-compiles
against pouch and its self-test passes." A natural literal reading is
"run libsodium's `test/default/*.c` test suite in QEMU" — but that means
~100 separate test binaries, each ~280 KiB, with their own ramfs slots
+ joey orchestration. At v1.0 the goal is to **prove the cross-toolchain
+ the primitive correctness**, and `/pouch-hello-sodium` does that
end-to-end on the primitives Stratum actually consumes — chacha20-poly1305
AEAD (Stratum's at-rest encryption), SHA-256 (Stratum's Merkle integrity),
BLAKE2b (HKDF + key derivation), ed25519 (signatures). A full self-test
suite is a v1.x deliverable once the ramfs file table and joey orchestration
can host it (`RAMFS_FILE_MAX` was bumped to 64 in this chunk; bigger is
fine).

## v1.0 limitations

- **No ARM crypto extension acceleration.** The `+crypto` `-march` slice
  isn't enabled, so AES-GCM and AEGIS use the portable refs. Functionally
  correct; ~10x slower than hardware-accelerated. Re-enable: build
  `libarmcrypto.a` with `-march=armv8-a+crypto`, link conditionally based
  on `getauxval(AT_HWCAP) & HWCAP_AES`.
- **No `mlock` / `madvise` / `mprotect`.** libsodium's `sodium_mlock` and
  `sodium_mprotect` return `-1` (ENOSYS via pouch's sentinel); libsodium
  treats these as hints and proceeds without them. The result: secret
  material may swap (no swap in Thylacine v1.0 anyway) and guarded
  allocator regions don't get PROT_NONE guard pages (same limitation as
  sub-chunk 9b's pthread stacks per F2). Real fix: a PROT_NONE-capable
  kernel syscall (deferred to v1.x).
- **`AT_HWCAP` always 0.** `getauxval(AT_HWCAP)` returns 0 because pouch's
  auxv carries only the six entries the System V process-startup frame
  needs (`AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/`AT_PAGESZ`/`AT_RANDOM`/`AT_NULL`).
  libsodium falls back to "no SIMD, no crypto extension" — exactly what
  we want for the portable-refs build.
- **`getpid()` returns -1.** Pouch's syscall seam has `SYS_getpid` at the
  `0xFFFF` sentinel (no `getpid` syscall retargeted at v1.0). libsodium
  uses it only for fork-detection in `randombytes_internal_random`'s
  reseed path, not the default `sysrandom` path used here; the `-1`
  value harmlessly never matches a "new" pid even if the user opted into
  internal_random.
- **`gettimeofday()` returns -1.** Same as above. Only used by
  `randombytes_internal_random`'s seed-mixing on opt-in; default
  `sysrandom` doesn't touch it.

## Audit posture

Not audit-bearing per POUCH-DESIGN.md §14 row 14. The chunk adds:
- No kernel code beyond bumping `RAMFS_FILE_MAX` (32 → 64) and
  `SYS_SPAWN_BLOB_MAX` (256 KiB → 1 MiB), both straightforward sizing
  changes; no new invariants.
- A vendored library (byte-pristine, no patches), a build step that
  compiles + archives it, a per-binary `-lsodium` in the link line, and
  a proving binary that runs KATs.
- All audit-trigger surfaces (exception entry, page fault, allocator,
  scheduler, territory, handle table, VMO, 9P client, pipe wait/wake,
  poll, notes/signals, capability checks, KASLR/ASLR, ELF loader, burrow,
  torpor, thread, pouch pthread) are unchanged.

The 586/586 PASS × default + UBSan runtime regression confirms no
new kernel-side bug.

## Build + verify

```bash
# Build the libsodium archive (part of sysroot build).
tools/build.sh sysroot

# Build the proving binary + ship in ramfs.
tools/build.sh kernel

# Boot + run KATs.
BOOT_TIMEOUT=420 tools/test.sh

# Expected joey-side output:
#   joey: pouch-hello-sodium smoke ok (libsodium KATs + AEAD + ed25519)
# Expected /pouch-hello-sodium output:
#   pouch-hello-sodium: sodium_init -> 0 (ok, version 1.0.20)
#   pouch-hello-sodium: sha256("abc") KAT ok
#   pouch-hello-sodium: blake2b-256 round-trip ok
#   pouch-hello-sodium: xchacha20-poly1305 round-trip ok
#   pouch-hello-sodium: ed25519 sign + verify + reject-tampered ok
#   pouch-hello-sodium: exit 0
```

## Cross-references

- `docs/POUCH-DESIGN.md §13` — exit criteria (libsodium row).
- `docs/POUCH-DESIGN.md §14` — sub-chunk decomposition (row 14).
- `docs/reference/78-pouch.md` — pouch toolchain (the `aarch64-thylacine`
  cross-compiler that built libsodium).
- `third_party/README.md` — vendoring record (sha256, license, source URL).
- `usr/pouch-hello/pouch-hello-sodium.c` — the proving binary source.
- `tools/build.sh::build_libsodium` — the build step.

## Owed / deferred

- **Full libsodium `make check` suite** — v1.x; needs bigger ramfs +
  joey orchestration changes. Not blocking sub-chunks 15 + 16.
- **ARMv8 crypto extension support** — opportunistic `+crypto`-built
  libarmcrypto.a, linked conditionally on `getauxval(AT_HWCAP) & HWCAP_AES`.
  v1.x.
- **Real `mlock` / `mprotect`** — needs PROT_NONE-capable kernel syscall;
  v1.x (also needed by sub-chunk 9b for pthread stack guards).
- **`AT_HWCAP` in auxv** — Phase 7 (Utopia) would want this for runtime
  CPU dispatch; needs kernel-side `cpu_features` introspection.
- **`SYS_getpid` / `SYS_gettimeofday`** — needed if pouch ever wants
  `randombytes_internal_random` to work; v1.x.
