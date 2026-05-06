// FDT (Flattened Device Tree) parser — minimal hand-rolled.
//
// Reads the DTB QEMU loaded into RAM (typical address 0x48000000 on virt
// with 2 GiB RAM and no initrd). Spec: devicetree-specification.readthedocs.io
// v0.4 (mirrors Linux's Documentation/devicetree/booting-without-of.txt).
//
// On-disk format is big-endian; we byte-swap on read. ARM64 has the `rev`
// instruction which clang lowers __builtin_bswap32/64 to directly.

#include <thylacine/dtb.h>
#include <thylacine/page.h>          // pa_to_kva (P3-Bda relocation)
#include <thylacine/types.h>

#include "../mm/phys.h"              // alloc_pages / free_pages (P3-Bda)

#include <stdint.h>

// FDT header (big-endian on disk; struct fields documented for reference;
// we never read this struct directly — only via byte-swapping accessors).
struct fdt_header_be {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

// Internal cached state. Set by dtb_init(); read by all other entry points.
//
// P3-Bda: `relocated` is true after `dtb_relocate_to_buffer` has copied
// the DTB blob to a kernel direct-map buffer. When true, `struct_kva`
// and `strings_kva` are valid and dtb_struct_base / dtb_strings_base
// return them; when false, those helpers fall back to the original PA
// via TTBR0 identity.
static struct {
    bool ready;
    bool relocated;
    paddr_t base;
    const uint8_t *struct_kva;       // direct-map KVA of struct block (post-relocate)
    const char    *strings_kva;      // direct-map KVA of strings block (post-relocate)
    uint32_t totalsize;
    uint32_t off_struct;
    uint32_t off_strings;
    uint32_t size_struct;
    uint32_t size_strings;
} g_dtb;

// ---------------------------------------------------------------------------
// Byte-swap helpers. ARM64's REV instruction is one cycle.
// ---------------------------------------------------------------------------

static inline uint32_t be32_to_host(uint32_t v) {
    return __builtin_bswap32(v);
}

// Read a big-endian u32 from a (possibly unaligned) byte pointer.
//
// IMPORTANT (P1-A through P1-C entry): the MMU is off and all kernel data
// accesses are Device-nGnRnE memory. Device memory mandates natural
// alignment for the access width — an unaligned 4/8/16-byte load faults.
// The DTB structure block is only guaranteed 4-byte-aligned, so we
// constrain every load to 4 bytes via a `volatile` u32 pointer read,
// preventing the compiler from fusing adjacent calls into a wider
// (potentially-misaligned) access. We've observed clang doing exactly
// that fusion when two be32_loads are inlined in close proximity, with
// the resulting `ldr x_, [x_]` faulting on Device memory.
//
// Once the MMU is on (P1-C+) and Normal-WB cacheable memory is in use
// for kernel data, this constraint can be relaxed; for now, volatile
// is the cheapest way to guarantee correctness.
static inline uint32_t be32_load(const void *p) {
    uint32_t v = *(const volatile uint32_t *)p;
    return be32_to_host(v);
}

// (be64_load was previously declared here as two be32_load calls. It was
// unused after switching dtb_get_memory and dtb_get_compat_reg to the
// shared read_reg_pair() helper, which inlines the four 4-byte loads
// directly. Removed at P1-C cleanup to silence -Wunused-function.)

// ---------------------------------------------------------------------------
// Minimal helpers (no libc).
// ---------------------------------------------------------------------------

static inline size_t k_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline bool k_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// True iff the NUL-terminated string `needle` appears in the NUL-separated
// list of strings in `[buf, buf+len)`. The DTB's "compatible" property is
// a list of NUL-terminated strings packed back-to-back; we scan it linearly.
static bool stringlist_contains(const char *buf, uint32_t len, const char *needle) {
    uint32_t i = 0;
    while (i < len) {
        const char *entry = buf + i;
        if (k_streq(entry, needle)) {
            return true;
        }
        // Advance past this entry's NUL.
        size_t l = k_strlen(entry);
        i += (uint32_t)(l + 1);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Header validation.
// ---------------------------------------------------------------------------

bool dtb_init(paddr_t base) {
    g_dtb.ready = false;

    if (base == 0) {
        return false;
    }

    // P3-Bda: dtb_init runs at boot_main entry (pre-phys_init). It reads
    // the FDT header via TTBR0 identity (PA-as-VA at the original PA).
    // Direct-map reads at this point in boot empirically fail — likely
    // due to data-cache state inherited from pre-mmu-on (the cache was
    // off through build_page_tables; first MMU walks may hit stale
    // cache lines on some CPU implementations). The relocation step
    // (dtb_relocate_to_buffer, called from boot_main post-phys_init)
    // copies the blob to a buddy-allocated buffer; from then on,
    // dtb_struct_base / dtb_strings_base read through the direct-map
    // KVA of that buffer, and TTBR0 identity is no longer needed.
    const struct fdt_header_be *hdr = (const struct fdt_header_be *)(uintptr_t)base;

    uint32_t magic = be32_to_host(hdr->magic);
    if (magic != FDT_MAGIC) {
        return false;
    }

    uint32_t version = be32_to_host(hdr->version);
    uint32_t last_comp = be32_to_host(hdr->last_comp_version);
    // We support FDT spec v17 (current). last_comp_version <= 16 means the
    // DTB is backward-compatible with v16, which is what we parse.
    if (version < 17 || last_comp > 17) {
        return false;
    }

    g_dtb.base         = base;
    g_dtb.totalsize    = be32_to_host(hdr->totalsize);
    g_dtb.off_struct   = be32_to_host(hdr->off_dt_struct);
    g_dtb.off_strings  = be32_to_host(hdr->off_dt_strings);
    g_dtb.size_struct  = be32_to_host(hdr->size_dt_struct);
    g_dtb.size_strings = be32_to_host(hdr->size_dt_strings);
    g_dtb.ready        = true;

    return true;
}

bool dtb_is_ready(void) {
    return g_dtb.ready;
}

u32 dtb_get_total_size(void) {
    return g_dtb.ready ? g_dtb.totalsize : 0;
}

// =============================================================================
// P3-Bda: DTB relocation to a kernel-allocated buffer.
// =============================================================================
//
// Pre-relocation, dtb_struct_base / dtb_strings_base read the DTB blob
// at its original PA via TTBR0 identity (PA-as-VA). This works at P3-Bca
// because TTBR0 identity is alive from boot, but BLOCKS retiring TTBR0
// identity entirely.
//
// Post-relocation:
//   1. The DTB blob has been copied byte-for-byte to a buddy-allocated
//      buffer (size = ceil(totalsize / PAGE_SIZE) pages, KP_ZERO so any
//      tail past totalsize is zeroed).
//   2. g_dtb.struct_kva and g_dtb.strings_kva point into the buffer at
//      the appropriate offsets — direct-map KVAs (0xFFFF_0000_*).
//   3. g_dtb.relocated = true. dtb_struct_base / dtb_strings_base now
//      return the KVAs.
//
// The original PA's data is still readable via TTBR0 identity until P3-Bd
// retires that mapping; after retirement, ONLY the buffer is reachable
// — and the parser doesn't care because it's all KVA-relative now.
//
// At v1.0 P3-Bda this runs once at boot from boot_main (post-phys_init).
// Idempotent on re-call (no-op if already relocated). Forward-compat:
// Phase 5+ may need to relocate again if the kernel virtual layout
// changes (e.g., late KASLR re-randomization).

// Compute the smallest order N such that 2^N pages cover `bytes`.
static unsigned bytes_to_order(u64 bytes) {
    if (bytes == 0) return 0;
    u64 pages = (bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (pages == 1) return 0;
    // Round up to next power of two.
    unsigned order = 0;
    u64 p = 1;
    while (p < pages) {
        p <<= 1;
        order++;
    }
    return order;
}

bool dtb_relocate_to_buffer(void) {
    if (!g_dtb.ready)     return false;
    if (g_dtb.relocated)  return true;          // idempotent

    // Allocate enough buddy pages to hold `totalsize` bytes. Real DTBs
    // are tiny: QEMU virt ≈ 1 KiB; bare metal ≈ 30 KiB. order=0 covers
    // up to 4 KiB; order=3 up to 32 KiB; etc.
    unsigned order = bytes_to_order(g_dtb.totalsize);
    struct page *buf_pg = alloc_pages(order, KP_ZERO);
    if (!buf_pg) return false;

    // Copy bytes from original PA (read via TTBR0 identity — still alive
    // at this point in boot) to buffer (write via direct-map KVA).
    paddr_t buf_pa = page_to_pa(buf_pg);
    uint8_t *buf_kva = (uint8_t *)pa_to_kva(buf_pa);
    const uint8_t *src = (const uint8_t *)(uintptr_t)g_dtb.base;

    for (uint32_t i = 0; i < g_dtb.totalsize; i++) {
        buf_kva[i] = src[i];
    }

    // Switch the parser to read through the buffer.
    g_dtb.struct_kva  = buf_kva + g_dtb.off_struct;
    g_dtb.strings_kva = (const char *)buf_kva + g_dtb.off_strings;
    g_dtb.relocated   = true;

    return true;
}

// ---------------------------------------------------------------------------
// Structure-block walker.
//
// The structure block is a sequence of tokens (4-byte big-endian):
//   FDT_BEGIN_NODE name<NUL> [pad to 4-byte align]      open node
//   FDT_PROP       u32 len  u32 nameoff  bytes[len] [pad]    property
//   FDT_NOP                                              skip
//   FDT_END_NODE                                         close node
//   FDT_END                                              end of structure
//
// We walk it with a simple loop. Callers express what they want via small
// callback-style helpers below (find compatible, get reg, etc.).
// ---------------------------------------------------------------------------

static const uint8_t *dtb_struct_base(void) {
    // P3-Bda: post-relocation use direct-map KVA into the kernel buffer;
    // pre-relocation fall back to TTBR0 identity at the original PA.
    if (g_dtb.relocated) return g_dtb.struct_kva;
    return (const uint8_t *)(uintptr_t)g_dtb.base + g_dtb.off_struct;
}

static const char *dtb_strings_base(void) {
    if (g_dtb.relocated) return g_dtb.strings_kva;
    return (const char *)(uintptr_t)g_dtb.base + g_dtb.off_strings;
}

// Align `n` up to a 4-byte boundary.
static inline uint32_t align4(uint32_t n) {
    return (n + 3u) & ~3u;
}

// Walker state. Tracks current position in the structure block + the
// current node depth (for nested children).
struct fdt_walker {
    const uint8_t *cur;       // current byte in struct block
    const uint8_t *end;       // one past struct block end
    int depth;                // BEGIN/END_NODE depth, root = 0 inside
};

static void walker_start(struct fdt_walker *w) {
    w->cur = dtb_struct_base();
    w->end = w->cur + g_dtb.size_struct;
    w->depth = 0;
}

// Decode the next token. Returns the token (FDT_*) or 0 if past end.
// Advances w->cur to the byte after this token's data.
//
// On FDT_BEGIN_NODE: *out_name is the node's unit-name.
// On FDT_PROP: *out_propname / *out_propdata / *out_proplen are populated.
static uint32_t walker_next(struct fdt_walker *w,
                            const char **out_name,
                            const char **out_propname,
                            const uint8_t **out_propdata,
                            uint32_t *out_proplen) {
    while (w->cur + 4 <= w->end) {
        uint32_t tok = be32_load(w->cur);
        w->cur += 4;
        switch (tok) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)w->cur;
            size_t l = k_strlen(name);
            w->cur += align4((uint32_t)(l + 1));
            w->depth++;
            if (out_name) *out_name = name;
            return FDT_BEGIN_NODE;
        }
        case FDT_END_NODE:
            w->depth--;
            return FDT_END_NODE;
        case FDT_PROP: {
            if (w->cur + 8 > w->end) return 0;
            uint32_t len     = be32_load(w->cur);     w->cur += 4;
            uint32_t nameoff = be32_load(w->cur);     w->cur += 4;
            const uint8_t *data = w->cur;
            w->cur += align4(len);
            if (out_propname) *out_propname = dtb_strings_base() + nameoff;
            if (out_propdata) *out_propdata = data;
            if (out_proplen)  *out_proplen  = len;
            return FDT_PROP;
        }
        case FDT_NOP:
            continue;          // skip and try the next token
        case FDT_END:
            return FDT_END;
        default:
            // Malformed DTB. Bail out with FDT_END to stop the walk.
            return FDT_END;
        }
    }
    return FDT_END;
}

// ---------------------------------------------------------------------------
// Memory and compatible-reg lookups.
// ---------------------------------------------------------------------------

// Read a (base, size) pair from a `reg` property. With #address-cells=2 and
// #size-cells=2 (QEMU virt convention), the property is 16 bytes: 4 cells
// × 4 bytes. We do four separate 4-byte loads rather than two 8-byte loads
// to keep accesses 4-byte-aligned (be64_load works similarly via two be32
// loads, but doing it explicitly here makes intent obvious and resists
// any future compiler change that might widen the access).
static void read_reg_pair(const uint8_t *propdata, uint64_t *base, uint64_t *size) {
    uint32_t b_hi = be32_load(propdata);
    uint32_t b_lo = be32_load(propdata + 4);
    uint32_t s_hi = be32_load(propdata + 8);
    uint32_t s_lo = be32_load(propdata + 12);
    *base = ((uint64_t)b_hi << 32) | b_lo;
    *size = ((uint64_t)s_hi << 32) | s_lo;
}

bool dtb_get_memory(u64 *base, u64 *size) {
    if (!g_dtb.ready) return false;

    struct fdt_walker w;
    walker_start(&w);

    bool in_memory = false;
    int memory_depth = -1;

    for (;;) {
        const char *name = NULL, *propname = NULL;
        const uint8_t *propdata = NULL;
        uint32_t proplen = 0;
        uint32_t tok = walker_next(&w, &name, &propname, &propdata, &proplen);
        if (tok == FDT_END) break;

        if (tok == FDT_BEGIN_NODE) {
            // Memory nodes are named "memory" or "memory@<addr>". Match
            // the unit-name form via a 7-byte prefix probe (the trailing
            // '@' is what distinguishes "memory" the prefix from a node
            // whose name happens to start with the letters 'memory' but
            // is something else — defensive even though no such node
            // exists in QEMU virt).
            bool is_memory = k_streq(name, "memory");
            if (!is_memory) {
                int i;
                static const char prefix[] = "memory@";
                for (i = 0; i < (int)sizeof(prefix) - 1; i++) {
                    if (name[i] != prefix[i]) break;
                }
                is_memory = (i == (int)sizeof(prefix) - 1);
            }
            if (!in_memory && is_memory) {
                in_memory = true;
                memory_depth = w.depth;
            }
            continue;
        }
        if (tok == FDT_END_NODE) {
            if (in_memory && w.depth < memory_depth) {
                in_memory = false;
            }
            continue;
        }
        if (tok == FDT_PROP && in_memory && k_streq(propname, "reg")) {
            // QEMU virt convention: #address-cells = 2, #size-cells = 2.
            // Each (base, size) entry is 4 cells = 16 bytes. Take the
            // first entry; multi-bank memory is post-v1.0.
            if (proplen < 16) return false;
            read_reg_pair(propdata, base, size);
            return true;
        }
    }
    return false;
}

// Maximum DTB node nesting depth supported. QEMU virt's tree is < 5 deep;
// 16 is a generous static cap. Exceeding it is malformed-DTB territory
// (or an attacker-controlled DTB; we'd panic, not silently corrupt).
#define DTB_MAX_DEPTH 16

bool dtb_get_compat_reg(const char *compat, u64 *base, u64 *size) {
    return dtb_get_compat_reg_n(compat, 0, base, size);
}

// Walk the structure block; for every node, track its compatible-match
// status and its reg property bounds (data + length). On END_NODE for a
// matched node, check whether reg has at least (idx+1) pairs and emit
// the requested pair.
//
// The reg property under #address-cells = 2, #size-cells = 2 (QEMU virt
// + ARM64 Linux convention) packs each (base, size) entry into 16
// bytes; we generalize over multi-region reg via the index argument.
// Per-call upper bound on idx. Real reg properties never have more
// than a handful of pairs; bound at 64 prevents `(idx + 1) * 16u`
// from overflowing u32 (which would make the bounds check below
// vacuously true and let the caller read past the property buffer).
#define DTB_REG_MAX_IDX   64u

bool dtb_get_compat_reg_n(const char *compat, u32 idx, u64 *base, u64 *size) {
    if (!g_dtb.ready) return false;
    if (idx > DTB_REG_MAX_IDX) return false;

    struct fdt_walker w;
    walker_start(&w);

    // Per-node accumulators in a stack indexed by nesting depth. Each
    // BEGIN_NODE pushes a fresh entry; each END_NODE pops and checks for
    // a complete match. The stack keeps parent-node state from being
    // clobbered when we descend into a child (the bug the previous
    // single-flag implementation had: PL011's compatible property
    // appears AFTER its reg property, so a flag set only on BEGIN/PROP
    // missed the match).
    struct {
        bool compat_matched;
        const uint8_t *reg_data;
        uint32_t reg_len;
    } stack[DTB_MAX_DEPTH];
    int sp = 0;

    for (;;) {
        const char *name = NULL, *propname = NULL;
        const uint8_t *propdata = NULL;
        uint32_t proplen = 0;
        uint32_t tok = walker_next(&w, &name, &propname, &propdata, &proplen);
        if (tok == FDT_END) break;

        if (tok == FDT_BEGIN_NODE) {
            if (sp < DTB_MAX_DEPTH) {
                stack[sp].compat_matched = false;
                stack[sp].reg_data = NULL;
                stack[sp].reg_len = 0;
            }
            sp++;
            continue;
        }
        if (tok == FDT_END_NODE) {
            if (sp > 0) {
                sp--;
                if (sp < DTB_MAX_DEPTH &&
                    stack[sp].compat_matched && stack[sp].reg_data) {
                    // Each (base, size) pair is 16 bytes. Bounds-check
                    // before indexing; reject malformed reg.
                    uint32_t need = (idx + 1) * 16u;
                    if (stack[sp].reg_len >= need) {
                        read_reg_pair(stack[sp].reg_data + idx * 16u,
                                      base, size);
                        return true;
                    }
                }
            }
            continue;
        }
        if (tok == FDT_PROP && sp > 0 && sp <= DTB_MAX_DEPTH) {
            int si = sp - 1;
            if (k_streq(propname, "compatible")) {
                if (stringlist_contains((const char *)propdata, proplen, compat)) {
                    stack[si].compat_matched = true;
                }
            } else if (k_streq(propname, "reg") && proplen >= 16) {
                stack[si].reg_data = propdata;
                stack[si].reg_len  = proplen;
            }
        }
    }
    return false;
}

bool dtb_has_compat(const char *compat) {
    if (!g_dtb.ready) return false;

    struct fdt_walker w;
    walker_start(&w);

    for (;;) {
        const char *name = NULL, *propname = NULL;
        const uint8_t *propdata = NULL;
        uint32_t proplen = 0;
        uint32_t tok = walker_next(&w, &name, &propname, &propdata, &proplen);
        if (tok == FDT_END) break;

        if (tok == FDT_PROP && k_streq(propname, "compatible")) {
            if (stringlist_contains((const char *)propdata, proplen, compat)) {
                return true;
            }
        }
    }
    return false;
}

// Walk /chosen for a single property. Returns the property data bounds
// via *out_data / *out_len if found; returns false if the chosen node
// or named property is absent.
static bool dtb_chosen_prop(const char *prop, const uint8_t **out_data, uint32_t *out_len) {
    if (!g_dtb.ready) return false;

    struct fdt_walker w;
    walker_start(&w);

    bool in_chosen = false;
    int chosen_depth = -1;

    for (;;) {
        const char *name = NULL, *propname = NULL;
        const uint8_t *propdata = NULL;
        uint32_t proplen = 0;
        uint32_t tok = walker_next(&w, &name, &propname, &propdata, &proplen);
        if (tok == FDT_END) break;

        if (tok == FDT_BEGIN_NODE) {
            if (!in_chosen && k_streq(name, "chosen")) {
                in_chosen = true;
                chosen_depth = w.depth;
            }
            continue;
        }
        if (tok == FDT_END_NODE) {
            if (in_chosen && w.depth < chosen_depth) return false;
            continue;
        }
        if (tok == FDT_PROP && in_chosen && k_streq(propname, prop)) {
            *out_data = propdata;
            *out_len = proplen;
            return true;
        }
    }
    return false;
}

u64 dtb_get_chosen_kaslr_seed(void) {
    const uint8_t *data;
    uint32_t len;
    if (!dtb_chosen_prop("kaslr-seed", &data, &len)) return 0;
    // kaslr-seed is two u32 cells (8 bytes total). UEFI fills this
    // with hardware entropy on bare metal.
    if (len < 8) return 0;
    uint32_t hi = be32_load(data);
    uint32_t lo = be32_load(data + 4);
    return ((u64)hi << 32) | lo;
}

u64 dtb_get_chosen_rng_seed(void) {
    const uint8_t *data;
    uint32_t len;
    if (!dtb_chosen_prop("rng-seed", &data, &len)) return 0;
    // /chosen/rng-seed is typically 32 bytes / 8 cells (QEMU virt
    // publishes 256 bits). XOR-fold all 4-byte cells into a single
    // u64 so the result preserves entropy across all bits regardless
    // of how many cells the bootloader provided. Alternate between
    // high and low halves to avoid trivial cancellation across
    // adjacent cells.
    u64 folded = 0;
    uint32_t i = 0;
    while (i + 4 <= len) {
        uint32_t cell = be32_load(data + i);
        u64 c = (u64)cell;
        if ((i / 4) & 1) {
            folded ^= c;
        } else {
            folded ^= (c << 32);
        }
        i += 4;
    }
    return folded;
}

// ---------------------------------------------------------------------------
// CPU enumeration + PSCI method (P2-Ca).
// ---------------------------------------------------------------------------
//
// /cpus/cpu@N nodes are identified by device_type = "cpu". Each holds
// a `reg` property — under /cpus' #address-cells = 1 and #size-cells = 0
// (QEMU virt convention), `reg` is a single u32 cell holding the CPU's
// MPIDR aff bits.
//
// We don't track the parent — instead, we match by device_type =
// "cpu" globally. On QEMU virt this is unambiguous (only the 4 cpu@*
// nodes have device_type = "cpu"). On stranger boards a /cpus
// container check could be added; the trade-off is parser complexity
// vs precision. For v1.0 the global match suffices.

static u32 g_cpu_count_cached = 0;

// Walk callback: for each node with device_type = "cpu", report the
// cpu's reg cell. Stops walking when stop_at_count is reached.
//
// out_mpidrs: pre-allocated buffer of at least DTB_MAX_CPUS u64s.
// returns: count actually populated (≤ DTB_MAX_CPUS).
static u32 dtb_walk_cpus(u64 *out_mpidrs, u32 max_count) {
    if (!g_dtb.ready) return 0;

    struct fdt_walker w;
    walker_start(&w);

    // Per-node accumulator stack — track device_type and reg per node.
    struct {
        bool      is_cpu;
        u32       reg_cell;
        bool      reg_present;
    } stack[DTB_MAX_DEPTH];
    int sp = 0;
    u32 count = 0;

    for (;;) {
        const char *name = NULL, *propname = NULL;
        const uint8_t *propdata = NULL;
        uint32_t proplen = 0;
        uint32_t tok = walker_next(&w, &name, &propname, &propdata, &proplen);
        if (tok == FDT_END) break;

        if (tok == FDT_BEGIN_NODE) {
            if (sp < DTB_MAX_DEPTH) {
                stack[sp].is_cpu      = false;
                stack[sp].reg_cell    = 0;
                stack[sp].reg_present = false;
            }
            sp++;
            continue;
        }
        if (tok == FDT_END_NODE) {
            if (sp > 0) {
                sp--;
                if (sp < DTB_MAX_DEPTH &&
                    stack[sp].is_cpu && stack[sp].reg_present) {
                    if (count < max_count) {
                        out_mpidrs[count] = (u64)stack[sp].reg_cell;
                    }
                    count++;
                }
            }
            continue;
        }
        if (tok == FDT_PROP && sp > 0 && sp <= DTB_MAX_DEPTH) {
            int si = sp - 1;
            if (k_streq(propname, "device_type") &&
                proplen >= 4 &&
                stringlist_contains((const char *)propdata, proplen, "cpu")) {
                stack[si].is_cpu = true;
            } else if (k_streq(propname, "reg") && proplen >= 4) {
                // /cpus has #address-cells = 1; reg is a single u32 cell.
                stack[si].reg_cell    = be32_load(propdata);
                stack[si].reg_present = true;
            }
        }
    }
    return count;
}

u32 dtb_cpu_count(void) {
    if (g_cpu_count_cached) return g_cpu_count_cached;
    u64 ignore[DTB_MAX_CPUS];
    g_cpu_count_cached = dtb_walk_cpus(ignore, DTB_MAX_CPUS);
    return g_cpu_count_cached;
}

bool dtb_cpu_mpidr(u32 idx, u64 *out_mpidr) {
    if (!out_mpidr) return false;
    if (idx >= DTB_MAX_CPUS) return false;

    u64 ids[DTB_MAX_CPUS];
    u32 count = dtb_walk_cpus(ids, DTB_MAX_CPUS);
    if (idx >= count) return false;

    *out_mpidr = ids[idx];
    return true;
}

dtb_psci_method_t dtb_psci_method(void) {
    if (!g_dtb.ready) return DTB_PSCI_NONE;

    struct fdt_walker w;
    walker_start(&w);

    bool in_psci = false;
    int psci_depth = -1;
    dtb_psci_method_t result = DTB_PSCI_NONE;

    for (;;) {
        const char *name = NULL, *propname = NULL;
        const uint8_t *propdata = NULL;
        uint32_t proplen = 0;
        uint32_t tok = walker_next(&w, &name, &propname, &propdata, &proplen);
        if (tok == FDT_END) break;

        if (tok == FDT_BEGIN_NODE) {
            // Match /psci or /psci@... — name starts with "psci".
            if (!in_psci && name[0] == 'p' && name[1] == 's' &&
                name[2] == 'c' && name[3] == 'i') {
                in_psci = true;
                psci_depth = w.depth;
            }
            continue;
        }
        if (tok == FDT_END_NODE) {
            if (in_psci && w.depth < psci_depth) break;
            continue;
        }
        if (tok == FDT_PROP && in_psci && k_streq(propname, "method")) {
            // method is a NUL-terminated string: "hvc" or "smc".
            if (proplen >= 4 && k_streq((const char *)propdata, "hvc")) {
                result = DTB_PSCI_HVC;
            } else if (proplen >= 4 && k_streq((const char *)propdata, "smc")) {
                result = DTB_PSCI_SMC;
            }
            break;
        }
    }
    return result;
}
