# 38 — Userspace tree + libt runtime (P4-Ia1)

## Purpose

Thylacine native userspace lives in `usr/` (top-level). It is a **separate
CMake project** from the kernel — sibling, not subordinate — because the
two have incompatible compiler-flag sets. The kernel is freestanding with
no FP and tiny code-model; userspace is freestanding with FP available
and standard code-model. They share the same `clang` + `ld.lld`
toolchain (Homebrew LLVM) but each through its own toolchain file.

`libt` ("libt" — Thylacine userspace runtime) is the minimum scaffolding
every Thylacine native program needs: a `_start` entry stub and a
header-only SVC wrapper layer for the kernel syscall surface. At P4-Ia1
the wrapper surface is two operations (`t_exits`, `t_puts`) mirroring
the kernel's `SYS_EXITS` + `SYS_PUTS`. Phase 5+ expands the surface as
new syscalls land.

**This is distinct from `tools/build.sh sysroot`** (deferred to Phase 6).
`sysroot` is the *Linux-compatibility* musl tree — the place where
unmodified Linux binaries (`musl-static`, `musl-dynamic`, `glibc-dynamic`
tiers per the user manual) get built or installed. The `usr/` tree
contains *native* Thylacine programs that talk directly to the Thylacine
syscall ABI (Plan 9 / 9P / namespace-aware, not Linux).

---

## Layout

```
usr/
├── CMakeLists.txt                 # C-side toplevel: project(thylacine_userspace)
├── Cargo.toml                     # Rust-side workspace (members: hello-rs)
├── .cargo/config.toml             # Rust target + linker flag pinning
├── scripts/
│   └── aarch64-userspace.ld       # linker script: two PT_LOAD (RX, RW); shared C+Rust
├── lib/
│   └── libt/                      # C runtime (libt.a)
│       ├── CMakeLists.txt
│       ├── include/thyla/
│       │   └── syscall.h          # SVC wrappers (header-only)
│       └── src/
│           └── start.S            # _start + stack-canary stubs
├── hello/                         # C binary
│   ├── CMakeLists.txt
│   └── hello.c
└── hello-rs/                      # Rust binary (no_std, no_main)
    ├── Cargo.toml
    └── src/
        └── main.rs                # SVC wrappers (inline) + _start (global_asm) + panic_handler
```

Build outputs:
- `build/usr/lib/libt.a` — static archive (only object: `start.S`)
- `build/usr/hello/hello` — C-side ELF, ~74 KB unstripped
- `build/usr-rs/aarch64-unknown-none/release/hello-rs` — Rust-side ELF, ~65 KB

---

## Public API (`<thyla/syscall.h>`)

```c
// Terminate the calling process with status. Never returns.
// status==0 ⇒ kernel exits("ok"); non-zero ⇒ exits("fail").
__attribute__((noreturn, always_inline))
static inline void t_exits(long status);

// Write `len` bytes from `buf` to the kernel diagnostic UART.
// Returns `len` on success, -1 on validation failure (NULL buf,
// oversized len, fault on user-VA copy).
__attribute__((always_inline))
static inline long t_puts(const char *buf, size_t len);

// Convenience: write a NUL-terminated string via t_puts. Computes
// strlen inline (no libc dependency).
__attribute__((always_inline))
static inline long t_putstr(const char *s);
```

Syscall ABI (`x8` = syscall number, `x0..x5` = args, `x0` = return) is
identical to Linux AArch64 for tooling familiarity. The kernel's
exception-context save/restore preserves all GPRs across a syscall
except `x0` (return value), so the inline-asm clobber list is just
`"memory" + "cc"`.

---

## Implementation

### Rust-side `_start` (`usr/lib/libthyla-rs/src/lib.rs` — global_asm!)

At P4-Ic4 the Rust-side `_start` lives in the `libthyla-rs` runtime
crate (`usr/lib/libthyla-rs/`). Each Rust binary depends on the crate
and only provides `rs_main` as its program body.

```rust
// usr/lib/libthyla-rs/src/lib.rs
global_asm!(
    ".section .text._start, \"ax\"",
    ".globl _start",
    "_start:",
    "    bti     c",
    "    bl      rs_main",     // x0 := rs_main() (extern "C" return)
    "    mov     x8, #0",      // T_SYS_EXITS
    "    svc     #0",
    "1:  wfe; b 1b",
);
```

The linker script `usr/scripts/aarch64-userspace.ld` has `ENTRY(_start)`,
which keeps `_start` alive across the rlib boundary — the linker pulls
the symbol from `libthyla_rs.rlib` even though no Rust code in the
binary references it directly. The binary still needs `#![no_std]
#![no_main]` (Rust requires the binary itself to opt out of `main`),
but the actual entry sequence lives in the runtime crate.

Same contract as the C side: kernel delivers control with sp set,
x0..x30 unspecified; `_start` calls Rust-side `rs_main()` and SYS_EXITS
with its return value. The `rs_main` symbol must be declared `#[no_mangle]
pub extern "C"` so the AArch64 PCS applies (x0 = return).

### libthyla-rs runtime crate (P4-Ic4)

Sibling of the C-side `libt`. Provides:

| Surface | Mirror of |
|---|---|
| `T_SYS_*` constants  | `kernel/include/thylacine/syscall.h` |
| `T_RIGHT_*` constants | `kernel/include/thylacine/handle.h` |
| `T_PROT_*` constants  | `kernel/include/thylacine/vma.h` |
| `t_exits(status)` → ! | `<thyla/syscall.h>::t_exits` |
| `t_puts(buf, len) → i64` | `<thyla/syscall.h>::t_puts` |
| `t_putstr(&str) → i64` | `<thyla/syscall.h>::t_putstr` |
| `t_mmio_create(pa, size, rights)` | `<thyla/syscall.h>::t_mmio_create` |
| `t_mmio_map(handle, vaddr, prot)` | `<thyla/syscall.h>::t_mmio_map` |
| `t_irq_create(intid, rights)` | `<thyla/syscall.h>::t_irq_create` |
| `t_irq_wait(handle)` | `<thyla/syscall.h>::t_irq_wait` |
| `_start` (global_asm!) | `libt/src/start.S::_start` |
| `#[panic_handler]` | C side has no equivalent (panic = abort) |

`#[panic_handler]` is required by `no_std` and must appear exactly once
across the binary's full dependency tree. libthyla-rs provides one that
tail-calls `t_exits(1)`; binaries that want a richer handler (e.g.,
printing the panic message via `t_puts`) would depend on libthyla-rs
with `default-features = false` and provide their own — a Phase 5+
extension when the runtime matures.

The Cargo dependency from a binary is straightforward:

```toml
# usr/hello-rs/Cargo.toml
[dependencies]
libthyla-rs = { path = "../lib/libthyla-rs" }
```

And the binary body collapses to just the entry function:

```rust
// usr/hello-rs/src/main.rs
#![no_std]
#![no_main]

use libthyla_rs::t_putstr;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("hello from /hello-rs\n");
    0
}
```

### C-side `_start` (`usr/lib/libt/src/start.S`)

```asm
_start:
    bti     c           // BTI landing pad
    bl      main        // x0 := main()
    mov     x8, #0      // T_SYS_EXITS
    svc     #0          // never returns
1:  wfe; b 1b           // defensive
```

The kernel's `userland_enter` (`arch/arm64/userland.S`) delivers
control via `eret` with:
- `sp = EXEC_USER_STACK_TOP` (page-aligned ⇒ ABI-required 16-byte aligned)
- `x0..x30` unspecified
- `PSTATE.M = EL0t`

`_start`'s contract: call `main()`, exit with `main()`'s return value.
At v1.0 P4-Ia1 `main()` takes no arguments (no argc/argv/envp); when
the exec syscall surface lands in Phase 5+ the kernel will populate
the auxv on the user stack before transferring control, and `_start`
will become an actual argc/argv loader.

### Stack-canary stubs

`-fstack-protector-strong` emits canary checks at function epilogues
of any function with address-taken locals or arrays. The compiler
references two external symbols: `__stack_chk_guard` (the canary
sentinel) and `__stack_chk_fail` (called on mismatch).

At v1.0 P4-Ia1 the guard is a fixed sentinel (`0xdeadbeefcafef00d`)
defined in `start.S`. The userspace CSPRNG-seeded variant (matching
the kernel's per-boot guard) lands when the seed-via-syscall surface
arrives in Phase 5+.

`__stack_chk_fail` calls `T_SYS_EXITS(1)` — "fail" status. The kernel
records the non-zero exit; the parent's `wait_pid` observes it.

### Linker script (`usr/scripts/aarch64-userspace.ld`)

Layout:
```
PHDRS {
    text PT_LOAD FLAGS(0x5);   // R + X
    data PT_LOAD FLAGS(0x6);   // R + W
}

SECTIONS {
    . = 0x400000;
    .text :    { *(.text._start) *(.text .text.*) } :text
    .rodata :  { *(.rodata .rodata.*) }            :text
    . = ALIGN(4096);
    .data :    { *(.data .data.*) }                :data
    .bss :     { *(.bss .bss.*) *(COMMON) }        :data
    /DISCARD/ : { unused metadata }
}
```

**W^X (ARCH §28 I-12) is enforced two ways**:
1. The linker script emits exactly two PT_LOAD segments — text+rodata
   RX, data+bss RW. No segment carries both W and X.
2. The kernel ELF loader (`kernel/elf.c`) rejects any PT_LOAD with
   both PF_W and PF_X set.

Base address `0x400000` (4 MiB) matches the Linux static-executable
convention. Userspace ASLR (Phase 5+) randomizes via `R_AARCH64_RELATIVE`
relocations; at v1.0 the base is fixed.

---

## Build pipeline

### C-side toolchain (`cmake/Toolchain-aarch64-userspace.cmake`)

Sibling of `cmake/Toolchain-aarch64-thylacine.cmake`. Same `clang` +
`ld.lld`, same triple `aarch64-none-elf`, but with differences:

| Flag | Kernel | Userspace | Why |
|---|---|---|---|
| `-mgeneral-regs-only` | YES | NO | Userspace gets FP/SIMD; kernel saves it on context switch (ARCH §3.6) |
| `-mcmodel=tiny` | YES | NO | Tiny restricts to ±4 GiB; cramps userspace shared libs |
| `-fpie` | YES | NO | Userspace v1.0 is static-fixed-base; PIE arrives with ASLR Phase 5+ |
| Hardening (`-fstack-protector-strong`, PAC, BTI, canaries) | YES | YES | Same posture both sides |

### Rust-side toolchain (`usr/.cargo/config.toml`)

Target: `aarch64-unknown-none` (built-in Rust target for bare-metal
aarch64; no_std required, no libc, no host OS assumptions). Custom
target specs are an option for true Thylacine-native syscall ABI
integration in Phase 5+, but at v1.0 the syscall surface is small
enough that inline `asm!` suffices.

Linker: `rust-lld` (Rust-bundled lld). Same `usr/scripts/aarch64-userspace.ld`
linker script as the C side — both sides produce ELFs with identical
PT_LOAD layout (and the same kernel ELF loader path handles both).

Posture matches the C side: static, no PIE, code-model=small,
target-feature `+lse,+paca,+pacg,+bti`. `panic = "abort"` in the
release profile so panics don't unwind (no_std has no unwinder anyway).
`opt-level = "z"`, `lto = "fat"`, `codegen-units = 1` — size-first
posture appropriate for embedded-style binaries.

`target-dir` is set to `../build/usr-rs` to keep `usr/` git-clean and
to mirror the kernel's `build/kernel/` convention.

### Wrapper (`tools/build.sh`)

```bash
tools/build.sh userspace    # build usr/ → build/usr/... + build/usr-rs/...
tools/build.sh kernel       # also runs build_userspace + build_ramfs
tools/build.sh all          # alias for kernel (which chains the rest)
```

`build_userspace` invokes both:
1. CMake on `usr/` (C side) → `build/usr/`
2. `cargo build --release` from `usr/` (Rust side) → `build/usr-rs/aarch64-unknown-none/release/`

Rust is skipped with a notice (not an error) if `rustup target add
aarch64-unknown-none` hasn't been run, so the C-only build path stays
usable on fresh checkouts.

`build_ramfs` curates two lists of userspace binaries to ship in the
cpio (`usr_bins` for C; `usr_rs_bins` for Rust); each is copied into
`build/ramfs-src/` before `mkcpio.py` assembles `build/ramfs.cpio`.
Curated rather than glob to prevent accidental shipment of CMake /
cargo byproducts. New binaries get added to the appropriate array in
`tools/build.sh::build_ramfs`.

---

## Data structures

### `struct ramfs_file` (private to `kernel/devramfs.c`)

```c
struct ramfs_file {
    const char *name;        // NUL-terminated; into cpio blob
    const u8   *data;        // file content; into cpio blob
    size_t      size;
    u32         mode;        // cpio newc mode (0o100644 from mkcpio.py)
};
```

### `devramfs_lookup` (public; `<thylacine/devramfs.h>`)

```c
int devramfs_lookup(const char *name,
                    const void **out_data, size_t *out_size);
```

P4-Ia1 addition. Returns 0 on success; populates `*out_data` /
`*out_size` with pointers into the initrd blob (kernel-owned for boot
lifetime; caller MUST NOT modify or free). Returns -1 on missing file,
uninitialized devramfs, or NULL args. Used by `test_userspace_ramfs.c`
to load `/hello` for the regression test; the future Phase 5+ exec
syscall implementation will route through the same helper (or the
full Dev/Spoor walk path, which uses the same backing data).

---

## State machines

None at P4-Ia1. `_start` is straight-line code; canary failure → SVC.

The future exec syscall (Phase 5+) will replace `kernel/joey.c`'s
embedded-blob path with a load-from-disk path, removing the need to
hand-encode userspace bytes into the kernel image.

---

## Spec cross-reference

None at P4-Ia1 — toolchain scaffolding doesn't touch a load-bearing
invariant. The downstream paths (`exec_setup` → `userland_enter` →
syscall return) are pinned by:
- `specs/scheduler.tla` — wait/wake atomicity (I-9), already proved.
- ARCH §28 I-12 — W^X enforced at linker script + ELF loader.
- `arch/arm64/userland.S` SPSel discipline — P4-Fix157 closure; not a
  spec invariant but documented in `docs/reference/08-exception.md`.

---

## Tests

| Test | File | What |
|---|---|---|
| `userspace.ramfs_hello` | `kernel/test/test_userspace_ramfs.c` | Loads `/hello` (C-side libt) via `devramfs_lookup`, copies to 8-aligned buffer, rforks child, `exec_setup` + `userland_enter`, `wait_pid` verifies clean exit. Gracefully skips (PASS) if `/hello` not in ramfs. |
| `userspace.ramfs_hello_rs` | `kernel/test/test_userspace_ramfs.c` | Same as above for `/hello-rs` (Rust no_std + cargo). Same kernel-side machinery; different binary. Validates the Rust toolchain path end-to-end. Skips PASS if `/hello-rs` not in ramfs. |

The test memcpy's the cpio data into an 8-byte-aligned static buffer
before calling `exec_setup` because cpio newc only mandates 4-byte
alignment, which UBSan trips on the `Elf64_Ehdr` cast (R5-G F61). Future
binary-loading paths (Phase 5+ exec syscall, network loaders) will do
the same alignment hop at their entry point.

The kernel's existing `test_userspace_first_iteration` +
`test_userspace_second_iteration` continue to use a hand-encoded
in-test blob for the P4-Fix157 regression — they test the kernel
*dispatch path*, not the userspace toolchain.

---

## Error paths

| Caller | Failure | Result |
|---|---|---|
| `devramfs_lookup("hello", ...)` | `/hello` absent | -1; test prints `[skip]` + returns PASS |
| `rfork(RFPROC, ramfs_exec_thunk, ...)` | OOM | TEST_ASSERT failure |
| `exec_setup` | ELF rejected (bad alignment, RWX, etc.) | thunk calls `exits("fail-exec")` → `wait_pid` sees non-zero status → TEST_EXPECT_EQ fails |
| `main()` returns non-zero | `_start` calls `T_SYS_EXITS(status)` | non-zero `wait_pid` status → TEST_EXPECT_EQ fails |
| Canary mismatch | `__stack_chk_fail` calls `T_SYS_EXITS(1)` | same as above |

---

## Performance characteristics

| Metric | Value | Note |
|---|---|---|
| `/hello` build (cold cache) | ~0.9 s | clang + lld, one .c + one .S + link |
| `/hello` ELF size | 73,776 B | unstripped (debug info dominates) |
| `/hello` text-segment size | 0x88 (136 B) | actual runtime footprint after load |
| `/hello-rs` build (cold cache) | ~2.7 s | cargo + rustc + rust-lld first build; warm cache <100 ms |
| `/hello-rs` ELF size | 66,336 B | unstripped (smaller than C because rust-lld strips harder) |
| `/hello-rs` text-segment size | 0x74 (116 B) | smaller than C because no canary stubs |
| Boot pipeline overhead | ~0 ms | binaries load from initrd-resident cpio; no I/O |

Stripped binary would be ~2 KB; debug retained for development. Strip
step lands when release builds appear (Phase 7+).

---

## Status

- **P4-Ia1 — landed**. `usr/` tree, `libt` runtime, `/hello` binary,
  C toolchain file, build wrapper, ramfs integration, kernel test.
- **P4-Ia2 — landed**. Rust nostd userspace: cargo workspace at
  `usr/`, `aarch64-unknown-none` target, `/hello-rs` binary,
  `usr/.cargo/config.toml` toolchain pinning, build wrapper extended,
  `userspace.ramfs_hello_rs` kernel test. Inline SVC wrappers in
  `hello-rs/src/main.rs` (extracted at P4-Ic4).
- **P4-Ib — landed**. `KObj_MMIO` + `KObj_IRQ` handle-table integration
  with syscall surface; spec-first.
- **P4-Ic1 — landed**. Burrow MMIO type extension.
- **P4-Ic2 — landed**. `SYS_MMIO_MAP` syscall + device-memory PTE attrs
  + demand-page MMIO dispatch.
- **P4-Ic3 — landed**. Kernel-internal `rfork_with_caps` capability
  grant primitive.
- **P4-Ic4 — landed**. `libthyla-rs` runtime crate extraction:
  `_start`, SVC wrappers (`t_exits`, `t_puts`, `t_putstr`,
  `t_mmio_create`, `t_mmio_map`, `t_irq_create`, `t_irq_wait`),
  syscall/right/prot constants, and `#[panic_handler]` all moved from
  `usr/hello-rs/src/main.rs` into the new `usr/lib/libthyla-rs/` crate.
  hello-rs body collapses to ~10 lines: `use libthyla_rs::t_putstr;
  #[no_mangle] pub extern "C" fn rs_main() -> i64 { ...; 0 }`. ELF
  layout pinned identical to pre-extraction (same 66,336-byte binary;
  `_start` at 0x400000; `rs_main` at 0x400018).
- **P4-Ic5 — next**. Rust no_std virtio-blk driver crate; closes
  deferred R10 F159 (SVC-path test coverage).
- **P4-Id — after Ic**. Driver exposed as 9P server.

210/210 tests PASS × default + UBSan; 4/4 fault; 10/10 KASLR (P4-Ic4 tip).

`kernel/joey.c` remains as the boot-time embedded "hello" blob until
the disk-loaded `/joey` refactor (post-Phase 5+ when stratum is
mounted; the embedded blob disappears at the same time the initrd is
freed).

---

## Naming rationale

`libt` is the minimum-effort prefix for "Thylacine" — short, easy to
type, doesn't collide with anything in the C99 reserved namespace. The
header path `<thyla/...>` uses "thyla" as the deeper-namespace prefix
because "libt/include/t/syscall.h" would suggest a per-letter scheme
("t" for what?) whereas "thyla" is unambiguously the project.

Held proposals for future thematic renames:
- `_start` is universal (every loader expects `_start`); don't rename.
- The C-side runtime could be `marsupial` instead of `libt`; rejected
  for verbosity — userspace developers will type the include + link
  flags frequently. `libt` stays.

---

## Known caveats / footguns

1. **cpio data alignment**. The cpio newc format aligns data to 4 bytes;
   `Elf64_Ehdr` requires 8 (R5-G F61 / UBSan -fsanitize=alignment).
   Any loader reading binaries from the cpio MUST copy to an aligned
   buffer before casting to `Elf64_Ehdr *`. The test does this; the
   future exec-from-disk path (Phase 5+) will need the same hop, or a
   redesign that aligns at ramfs-init time.

2. **`tools/build.sh kernel` chains `build_userspace`**. If the
   userspace toolchain breaks, the kernel build breaks too. Workaround:
   `tools/build.sh kernel` doesn't currently have a `--no-userspace`
   flag (add when first needed).

3. **`tools/build.sh sysroot` is RESERVED for the Linux-compat musl
   tree** (Phase 6). Don't repurpose for native userspace; the naming
   will get confusing once the musl tier lands.

4. **`_Alignas(16)` on the test's static buffer is overkill** —
   `_Alignas(8)` would satisfy ELF alignment. 16 is chosen for SIMD
   future-proofing (if a future loader needs `Vector` types in the
   header). Not load-bearing.

5. **The hello binary is 73 KB unstripped** — most of it is DWARF
   debug. Strip yields ~2 KB. Production builds (post-v1.0) will
   strip; v1.0 debug keeps stack traces useful when something goes
   wrong.

6. **Cargo runs in `usr/`**. The `usr/.cargo/config.toml`'s
   `link-arg=-Tscripts/aarch64-userspace.ld` resolves the linker
   script path relative to cargo's CWD, which is the workspace root
   (`usr/`). `tools/build.sh::build_userspace` does
   `( cd "$REPO_ROOT/usr" && cargo build --release )` to ensure this
   stays correct.

7. **Cargo build dir is `build/usr-rs/`** (not `build/usr/rs/`) — a
   sibling of `build/usr/` rather than nested under it. Keeps cargo's
   target structure (which has its own `aarch64-unknown-none/release/`
   sub-tree) cleanly separated from CMake's flat output structure.

8. **Rust target `aarch64-unknown-none` requires `rustup target add`
   on a fresh dev machine**. `tools/build.sh::build_userspace` skips
   the Rust path gracefully (with a notice) if the target isn't
   installed — the C-only build still works. CI environments + the
   intended dev posture both install the target during initial setup.

9. **No userspace ASan / TSan / MSan at v1.0**. Rust's debug profile
   has overflow checks; release has them too (we set
   `overflow-checks = true` in `usr/Cargo.toml`'s `[profile.release]`).
   ASan-style memory safety for Rust userspace gets infrastructure
   in Phase 5+ when the std-like crate ecosystem solidifies.

10. **`rs_main` not `main` in the Rust binary** — chosen to avoid
    confusion with Rust's `fn main()` (which `#![no_main]` disables).
    `_start` calls `rs_main` via `bl` and the global_asm! string
    references the symbol by that name. Rename to `main` is possible
    but would shadow the disabled-yet-special name; the explicit
    rename keeps the intent visible.
