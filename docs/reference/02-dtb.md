# 02 — DTB parser (as-built reference)

The kernel's DTB (Flattened Device Tree) parser. Reads the DTB QEMU loaded for us at boot and exposes simple lookups for memory regions, devices by compatible string, and `/chosen` properties. As-built reference for **P1-B**; extended at later sub-chunks as more of the DTB is consumed.

Scope: `lib/dtb.c`, `kernel/include/thylacine/dtb.h`, plus the Linux ARM64 image header in `arch/arm64/start.S` that triggers QEMU's `load_aarch64_image()` path (which is the prerequisite for the DTB being loaded into RAM at all).

Reference: `ARCHITECTURE.md §22.2` (DTB-driven hardware abstraction; invariant I-15), `ARCHITECTURE.md §5` (boot sequence), spec: devicetree-specification.readthedocs.io v0.4.

---

## Purpose

ARM64 Linux boot protocol passes the DTB physical address in `x0`. The DTB is the kernel's authoritative source for hardware discovery — memory regions, peripheral base addresses, IRQ numbers, CPU topology, KASLR seed, etc. (Invariant I-15: hardware view derives entirely from DTB; no compile-time hardware constants outside `arch/arm64/<platform>/`.)

P1-B's parser does the minimum needed for the rest of Phase 1:

- Validate the DTB and cache its header.
- Find the `/memory@...` node's primary `(base, size)` pair.
- Find any node by its `compatible` string and return its first `reg` pair (used for PL011 base; will be used for GIC, ARM generic timer, VirtIO devices in P1-F+).
- Read `/chosen/kaslr-seed` (consumed by P1-C KASLR).

Property tree walks, string-list matching, and FDT structure-block iteration: all hand-rolled. No libfdt dependency.

---

## Public API

`kernel/include/thylacine/dtb.h`:

```c
// Initialize the parser with the DTB physical address. Validates magic
// (0xd00dfeed BE), version (>= 17, last_comp_version <= 17), caches
// header offsets. Returns false on invalid DTB.
bool dtb_init(paddr_t base);

// Whether dtb_init() previously succeeded.
bool dtb_is_ready(void);

// First /memory@... node's first (base, size) pair. Assumes
// #address-cells = 2 and #size-cells = 2 (QEMU virt + ARM64 Linux
// convention). Returns false if no memory node found or cells differ.
bool dtb_get_memory(u64 *base, u64 *size);

// First node whose "compatible" property contains `compat`; returns
// its first `reg` (base, size) pair. Stack-based per-node accumulator
// so that property order within a node doesn't matter.
bool dtb_get_compat_reg(const char *compat, u64 *base, u64 *size);

// Like dtb_get_compat_reg, but returns the (base, size) pair at index
// `idx` within the matched node's reg property. Used by P1-G's GIC
// driver: GICv3 advertises distributor at reg[0] and redistributor at
// reg[1]. Returns false if the node's reg holds fewer than (idx + 1)
// pairs. dtb_get_compat_reg(compat, ...) is sugar for idx = 0.
bool dtb_get_compat_reg_n(const char *compat, u32 idx, u64 *base, u64 *size);

// True iff some DTB node's "compatible" property contains `compat`.
// Used by P1-G for v2-vs-v3 autodetect (probes "arm,gic-v3" first,
// then v2 candidates). Cheaper than the full reg walk if you only
// care whether a string exists.
bool dtb_has_compat(const char *compat);

// /chosen/kaslr-seed as a 64-bit value (concatenated from two u32 cells).
// Returns 0 if absent — caller should treat that as low-entropy fallback
// per ARCH §5.3 KASLR design.
u64 dtb_get_chosen_kaslr_seed(void);
```

All functions are safe to call before `dtb_init()` (they return false / 0).

---

## Implementation

### Top-level state (`lib/dtb.c:34-44`)

A single `static struct g_dtb` holds the cached header offsets after `dtb_init()` succeeds:

```c
static struct {
    bool ready;
    paddr_t base;
    uint32_t totalsize;
    uint32_t off_struct;
    uint32_t off_strings;
    uint32_t size_struct;
    uint32_t size_strings;
} g_dtb;
```

No mutable state beyond this; queries are pure reads.

### Byte-order helpers (`lib/dtb.c:46-83`) — IMPORTANT

The on-disk FDT format is big-endian; ARM64 is little-endian. We use `__builtin_bswap32` / `__builtin_bswap64` (which clang lowers to ARM `rev` — one cycle).

**Critical alignment constraint while the MMU is off** (P1-A through P1-C entry):

> All kernel data accesses are treated as Device-nGnRnE memory. Device memory mandates natural alignment for the access width. An unaligned 8-byte load takes a synchronous abort.

The DTB structure block guarantees only **4-byte alignment** for property data. So a one-shot 8-byte load of property data is unsafe. We constrain every load to 4 bytes via a `volatile` u32 pointer read:

```c
static inline uint32_t be32_load(const void *p) {
    uint32_t v = *(const volatile uint32_t *)p;
    return be32_to_host(v);
}

static inline uint64_t be64_load(const void *p) {
    uint32_t hi = be32_load(p);
    uint32_t lo = be32_load((const uint8_t *)p + 4);
    return ((uint64_t)hi << 32) | lo;
}
```

The `volatile` qualifier on the u32 pointer is **load-bearing**: without it, clang fuses adjacent `be32_load` calls into a single `ldr x_, [x_]` (8-byte load). On Device memory with property data only 4-aligned, that fused load faults. The `volatile` forbids fusion.

We caught this empirically — the parser worked with debug printfs interleaved (which broke the fusion) and silently faulted without them. Codified as a comment in the source. Once the MMU is on (P1-C+) and Normal-WB cacheable memory is in use, the constraint relaxes; for now, `volatile` is the cheapest way to guarantee correctness.

### FDT header validation (`lib/dtb.c:91-122`)

```c
bool dtb_init(paddr_t base) {
    g_dtb.ready = false;
    if (base == 0) return false;
    const struct fdt_header_be *hdr = (const struct fdt_header_be *)(uintptr_t)base;
    uint32_t magic = be32_to_host(hdr->magic);
    if (magic != FDT_MAGIC) return false;          // 0xd00dfeed expected
    uint32_t version = be32_to_host(hdr->version);
    uint32_t last_comp = be32_to_host(hdr->last_comp_version);
    if (version < 17 || last_comp > 17) return false;
    // Cache offsets
    g_dtb.base         = base;
    g_dtb.totalsize    = be32_to_host(hdr->totalsize);
    g_dtb.off_struct   = be32_to_host(hdr->off_dt_struct);
    g_dtb.off_strings  = be32_to_host(hdr->off_dt_strings);
    g_dtb.size_struct  = be32_to_host(hdr->size_dt_struct);
    g_dtb.size_strings = be32_to_host(hdr->size_dt_strings);
    g_dtb.ready        = true;
    return true;
}
```

We accept FDT v17 (the current spec). The header struct is read field-by-field; each access is a u32 load, which is 4-byte aligned (header starts at the DTB base, which QEMU loads at an 8-aligned address).

### Structure-block walker (`lib/dtb.c:125-218`)

The FDT structure block is a flat sequence of 4-byte big-endian tokens:

| Token | Value | Payload |
|---|---|---|
| `FDT_BEGIN_NODE` | 1 | unit-name `<NUL>` (padded to 4-byte) |
| `FDT_END_NODE` | 2 | (none) |
| `FDT_PROP` | 3 | `u32 len` `u32 nameoff` `bytes[len]` (padded to 4-byte) |
| `FDT_NOP` | 4 | (none; skip) |
| `FDT_END` | 9 | (end of structure block) |

`walker_next()` reads one token, advances the cursor, and returns the token type. For `FDT_BEGIN_NODE` it returns the node's unit-name; for `FDT_PROP` it returns the property's name (looked up in the strings block by `nameoff`), data pointer, and length.

The walker tracks node depth; `BEGIN_NODE` increments it, `END_NODE` decrements. Callers use depth to scope per-node accumulators.

### `dtb_get_memory()` (`lib/dtb.c:230-273`)

Walks the structure block looking for a node named `memory` or with prefix `memory@`. On match, the next `reg` property within that node yields the first `(base, size)` pair via `read_reg_pair()`.

`read_reg_pair()` does four 4-byte loads (high+low cells of base + high+low cells of size) and combines:

```c
static void read_reg_pair(const uint8_t *propdata, uint64_t *base, uint64_t *size) {
    uint32_t b_hi = be32_load(propdata);
    uint32_t b_lo = be32_load(propdata + 4);
    uint32_t s_hi = be32_load(propdata + 8);
    uint32_t s_lo = be32_load(propdata + 12);
    *base = ((uint64_t)b_hi << 32) | b_lo;
    *size = ((uint64_t)s_hi << 32) | s_lo;
}
```

Assumes `#address-cells = 2` and `#size-cells = 2` (root-level inheritance, which matches QEMU virt and the ARM64 Linux convention). Different cell sizes return false; relaxing to read the parent's cells dynamically is a P1-C task if any DTB demands it.

### `dtb_get_compat_reg()` — stack-based per-node accumulator (`lib/dtb.c:284-340`)

The first implementation set a single "match pending" flag on `compatible` and looked for `reg` afterwards. **Bug**: in the QEMU virt DTB, the PL011 node's `compatible` property comes AFTER its `reg`, so the flag was set too late and the lookup returned false negative.

Fix: per-node accumulator stack. Each `BEGIN_NODE` pushes a fresh `(compat_matched, reg_data)` entry; each property updates the current top of stack; each `END_NODE` pops and checks for completion (both flags set) and returns immediately.

```c
struct {
    bool compat_matched;
    const uint8_t *reg_data;
} stack[DTB_MAX_DEPTH];     // DTB_MAX_DEPTH = 16
int sp = 0;
```

Children of a non-matching parent get their own entry; nesting works correctly. Stack overflow (depth > 16) silently skips deeper nodes — malformed-DTB territory. QEMU virt's tree is < 5 deep; 16 is generous.

### `dtb_get_chosen_kaslr_seed()` (`lib/dtb.c:342-372`)

Walks looking for the `chosen` node, then its `kaslr-seed` property. Returns the property as a `u64` (concatenating the two u32 cells: `(hi << 32) | lo`).

QEMU virt populates this via the random seed generator; on bare-metal Pi 5 the firmware's behavior depends on configuration. P1-C uses a low-entropy boot-counter fallback if the property is absent (per ARCH §5.3).

### `stringlist_contains()` (`lib/dtb.c:84-100`)

DTB property values for `compatible` are NUL-separated strings packed back-to-back. `stringlist_contains(buf, len, needle)` scans the list linearly:

```c
static bool stringlist_contains(const char *buf, uint32_t len, const char *needle) {
    uint32_t i = 0;
    while (i < len) {
        const char *entry = buf + i;
        if (k_streq(entry, needle)) return true;
        size_t l = k_strlen(entry);
        i += (uint32_t)(l + 1);
    }
    return false;
}
```

PL011's `compatible` is `"arm,pl011\0arm,primecell\0"` — a search for `"arm,pl011"` matches the first entry, returns true.

---

## Linux ARM64 image header (in `arch/arm64/start.S`)

The DTB is loaded into RAM by QEMU **only if** the kernel is recognized as a Linux ARM64 Image (Documentation/arch/arm64/booting.rst). For ELF kernels without the Linux image header, QEMU's `load_elf_as()` succeeds with `is_linux = 0`, and `arm_load_dtb()` is never called — the DTB simply isn't present in RAM.

Resolution: every Thylacine kernel has a 64-byte Linux ARM64 image header at offset 0:

```
0x00  u32  code0       b _real_start  (branch over header to actual entry)
0x04  u32  code1       nop
0x08  u64  text_offset 0x80000        (informational w/ flag bit 3)
0x10  u64  image_size  _kernel_end - _kernel_start (linker-resolved)
0x18  u64  flags       0x0a (LE + 4 KiB granule + placement-anywhere)
0x20  u64  res2        0
0x28  u64  res3        0
0x30  u64  res4        0
0x38  u32  magic       0x644d5241 ("ARM\x64", little-endian)
0x3C  u32  res5        0
0x40  ...              _real_start (the actual boot code)
```

The header lets QEMU's `load_aarch64_image()` (in `hw/arm/boot.c:852`) detect us via the `ARM\x64` magic at offset 0x38 and load us as a Linux Image (`is_linux = 1`). Then `arm_load_dtb()` runs, placing the DTB at `info->initrd_start = info->loader_start + min(ram_size/2, 128 MiB)` — which for our 2 GiB RAM is `0x40000000 + 128 MiB = 0x48000000`.

**The build now produces both an ELF (for debugging via lldb) and a flat binary (for `-kernel`)**:

- `build/kernel/thylacine.elf` — ELF, used by lldb / objdump / readelf.
- `build/kernel/thylacine.bin` — flat binary (objcopy `-O binary` of the ELF), used by QEMU `-kernel`.

`tools/run-vm.sh` and `tools/test.sh` invoke `-kernel thylacine.bin` so QEMU takes the Linux Image path. See `kernel/CMakeLists.txt` for the post-link `objcopy` rule.

---

## Spec cross-reference

No formal specs at P1-B. The DTB parser doesn't touch a load-bearing invariant directly (I-15 is satisfied by USE of the parser's results, not by the parser itself). If we extend to support hot-replug or runtime DTB modifications post-v1.0, a spec for parser idempotency might be worth writing.

---

## Tests

P1-B integration test: `tools/test.sh` boots the kernel and verifies the boot banner. The banner now includes:

```
  mem:  2048 MiB at 0x0000000040000000
  dtb:  0x0000000048000000 (parsed)
  uart: 0x0000000009000000 (DTB-driven)
```

— confirming the parser produced correct values for `/memory@40000000`'s `reg` and PL011's `reg` via compatible-string lookup. PASS at landing.

No unit tests yet (test harness is P1-I).

---

## Error paths

| Condition | Behavior |
|---|---|
| `base == 0` | `dtb_init()` returns false; subsequent queries return false / 0 |
| Magic mismatch | `dtb_init()` returns false |
| Version < 17 | `dtb_init()` returns false |
| `/memory` absent | `dtb_get_memory()` returns false |
| `/memory` with `#address-cells != 2` | `dtb_get_memory()` returns false |
| Compat string not found | `dtb_get_compat_reg()` returns false |
| `/chosen/kaslr-seed` absent | `dtb_get_chosen_kaslr_seed()` returns 0 (caller falls back) |
| Nesting depth > 16 | Silently skipped beyond the cap (malformed-DTB territory) |
| `FDT_END` mid-walk | Walker returns; queries return false if no match found yet |
| Malformed token | Walker treats as `FDT_END` (defensive) |

`dtb_init()` failure is non-fatal at P1-B: the kernel falls back to the hardcoded UART base, prints a degraded banner, and halts cleanly. P1-C panic infrastructure may upgrade this to a hard panic since the kernel can't proceed without DTB-driven discovery.

---

## Performance characteristics

- `dtb_init()`: ~20 instructions (one validation pass). < 1 µs.
- `dtb_get_memory()`: walks until first memory node found. ~50-200 token reads on QEMU virt's DTB. < 50 µs.
- `dtb_get_compat_reg()`: walks the entire structure block. ~300-500 token reads on QEMU virt's DTB. < 100 µs.

Boot adds ~150 µs total for DTB queries. Well within the 500 ms boot-to-banner budget (`VISION.md §4.5`).

---

## Status

**Implemented at P1-B**:

- FDT v17 parser with `dtb_init` / `dtb_get_memory` / `dtb_get_compat_reg` / `dtb_get_chosen_kaslr_seed`.
- Linux ARM64 image header in `start.S` for QEMU `load_aarch64_image()` detection.
- Flat-binary build target via `objcopy -O binary`.
- `tools/run-vm.sh` switched to `-kernel thylacine.bin`.
- DTB-driven UART base in the boot banner (replaces the P1-A hardcoded `FIXME(I-15)`).
- Memory size + base in the boot banner.

**Landed**: commit `(pending hash-fixup)`.

---

## Caveats

### Compiler-fusion of be32_load → unaligned u64 fault (mitigated)

Two adjacent `be32_load` calls on consecutive 4-byte regions can be fused by clang into a single 8-byte load. On Device memory (MMU off), an 8-byte load to a 4-aligned-but-not-8-aligned address faults. We mitigate via `volatile` u32 reads inside `be32_load`. Once MMU is on (P1-C+) with cacheable Normal memory, this mitigation is no longer load-bearing but doesn't hurt to keep — preserves the 4-byte access guarantee even in the kernel's bare-metal recovery path.

### `#address-cells` / `#size-cells` assumed 2/2

`dtb_get_memory()` and `dtb_get_compat_reg()` assume the parent node uses 2-cell addresses and 2-cell sizes. This is the QEMU virt + ARM64 Linux convention but isn't universal. P1-C may extend the walker to track inherited `#address-cells` / `#size-cells` per-node if any platform we target diverges.

### Silent depth cap at 16

Beyond 16 levels of nesting, queries silently skip the deeper nodes. QEMU virt's tree is < 5 levels deep so this never triggers in practice. A malformed or attacker-controlled DTB could trigger the cap; the parser's behavior is a clean miss (return false), not a fault. P1-C's panic infrastructure may upgrade to a hard panic if desired.

### Stack-allocated 16-entry node-state array (~256 B)

`dtb_get_compat_reg()` allocates a 16-entry stack of `{bool, ptr}` on the boot stack. ~256 B. Fits comfortably in the 16 KiB boot stack.

### DTB pointer must be passed via x0

If a future bootloader or boot path doesn't follow the Linux ARM64 boot protocol, `_saved_dtb_ptr` will be 0 and `dtb_init` rejects. The kernel boots with degraded info. P1-C may add a probe-fallback for resilience (scan a known range of physical RAM for the FDT magic), but at v1.0 we trust the boot protocol.

---

## See also

- `docs/reference/00-overview.md` — system-wide layer cake.
- `docs/reference/01-boot.md` — boot path; updated for the Linux ARM64 image header.
- `docs/ARCHITECTURE.md §5` — boot sequence design intent.
- `docs/ARCHITECTURE.md §22` — hardware platform model + DTB discipline + invariant I-15.
- `docs/phase1-status.md` — Phase 1 pickup guide; trip hazards including the alignment caveat.
- Linux kernel `Documentation/arch/arm64/booting.rst` — Linux ARM64 boot protocol.
- devicetree-specification.readthedocs.io v0.4 — FDT format specification.
