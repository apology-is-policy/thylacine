// KObj_MMIO impl (P4-Ib) — hardware-MMIO range claims with overlap
// rejection.
//
// Per <thylacine/mmio_handle.h> + specs/handles.tla. The g_mmio_claims
// table tracks every currently-alive (pa, size, owner) tuple; create
// scans for overlap before allocating; unref releases the slot.
//
// **Lock discipline**: g_mmio_lock guards g_mmio_claims for all
// reads + writes. Acquired in IRQ-safe spinlock mode — kobj_mmio_create
// can be called from kernel-context test code which may be preempted,
// and kobj_mmio_unref is called from handle_close which runs in process
// context. No nested-lock paths (no other locks taken under
// g_mmio_lock); held only for the constant-time scan + insert/release.

#include <thylacine/dtb.h>           // R10 F154: DTB compat lookup
#include <thylacine/extinction.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/page.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"
#include "../mm/slub.h"

// Claim table. At v1.0 P4-Ib a static array bounds the number of
// alive KObj_MMIO at KOBJ_MMIO_MAX. Real drivers (P4-Ic+) need a
// handful per driver process; 32 is enough headroom for the immediate
// future. Phase 5+ refactors to a growable RB-tree keyed by PA when
// the system has hundreds of MMIO regions live.
#define KOBJ_MMIO_MAX 32

struct mmio_claim {
    struct KObj_MMIO *owner;   // NULL = slot free; non-NULL = claimed
    u64               pa;       // first byte of the claimed range
    size_t            size;     // byte size; pa + size doesn't overflow
};

static struct mmio_claim g_mmio_claims[KOBJ_MMIO_MAX];
static spin_lock_t       g_mmio_lock = SPIN_LOCK_INIT;
static u64               g_mmio_created;
static u64               g_mmio_live;
static bool              g_mmio_initialized;

u64 kobj_mmio_total_created(void) {
    return __atomic_load_n(&g_mmio_created, __ATOMIC_RELAXED);
}

u64 kobj_mmio_live_count(void) {
    return __atomic_load_n(&g_mmio_live, __ATOMIC_RELAXED);
}

// =============================================================================
// Init.
// =============================================================================

void kobj_mmio_init(void) {
    // R9 F151 (P3) close: atomic init guard. Plain bool check +
    // assignment would race if two CPUs reached kobj_mmio_init
    // simultaneously (hypothetical future per-CPU subsystem_init).
    // __atomic_exchange returns the PREVIOUS value: if it was true,
    // someone got here first → extinct. If false, this caller is the
    // first → proceed.
    if (__atomic_exchange_n(&g_mmio_initialized, true, __ATOMIC_ACQ_REL)) {
        extinction("kobj_mmio_init called twice");
    }

    // KP_ZERO at BSS already left g_mmio_claims all-NULL-owners; no
    // explicit zeroing needed.

    uart_puts("kobj_mmio: claims=");
    uart_putdec((u64)KOBJ_MMIO_MAX);
    uart_puts(" slots\n");
}

// =============================================================================
// Claim helpers (caller holds g_mmio_lock).
// =============================================================================

// Check if [pa, pa+size) overlaps any existing claim. Returns true on
// overlap. Caller MUST hold g_mmio_lock.
//
// Overlap formula: two ranges [a, a+s_a) and [b, b+s_b) overlap iff
// a < b + s_b AND b < a + s_a. Symmetric; handles partial + complete
// overlap + adjacency-not-overlap (b == a + s_a is contiguous-but-distinct).
static bool ranges_overlap(u64 pa, size_t size) {
    u64 end = pa + size;        // caller has overflow-checked
    for (int i = 0; i < KOBJ_MMIO_MAX; i++) {
        if (!g_mmio_claims[i].owner) continue;
        u64 c_pa = g_mmio_claims[i].pa;
        u64 c_end = c_pa + g_mmio_claims[i].size;
        if (pa < c_end && c_pa < end) return true;
    }
    return false;
}

// Find a free slot. Returns slot index, or -1 if all slots are in use.
// Caller MUST hold g_mmio_lock.
static int find_free_slot(void) {
    for (int i = 0; i < KOBJ_MMIO_MAX; i++) {
        if (!g_mmio_claims[i].owner) return i;
    }
    return -1;
}

// Find the slot whose owner == k. Returns slot index, or -1 if no
// match. Caller MUST hold g_mmio_lock.
static int find_slot_by_owner(struct KObj_MMIO *k) {
    for (int i = 0; i < KOBJ_MMIO_MAX; i++) {
        if (g_mmio_claims[i].owner == k) return i;
    }
    return -1;
}

// =============================================================================
// Lifecycle.
// =============================================================================

// R10 F154 (P1) close: sentinel value for slots reserved by the kernel
// for its own MMIO use (not user-allocated). `find_slot_by_owner` won't
// match against this sentinel (callers pass real KObj_MMIO pointers);
// `ranges_overlap` doesn't care about ownership — it only checks for
// PA overlap, which is what blocks user `kobj_mmio_create` from
// claiming kernel ranges.
#define KOBJ_MMIO_KERNEL_RESERVED  ((struct KObj_MMIO *)0x1)

struct KObj_MMIO *kobj_mmio_create(u64 pa, size_t size) {
    if (!g_mmio_initialized)              return NULL;
    if (size == 0)                        return NULL;
    if ((pa & (PAGE_SIZE - 1)) != 0)      return NULL;
    if ((size & (PAGE_SIZE - 1)) != 0)    return NULL;
    // Overflow check: pa + size must not wrap.
    if (size > (u64)-1 - pa)              return NULL;
    // R10 F156 (P2) close: PA range must fit in TCR.IPS = 40 bits.
    // Without this check, a userspace caller with CAP_HW_CREATE could
    // pass an out-of-IPS PA; `kobj_mmio_create` would succeed, but
    // `mmu_install_user_pte` later rejects (mmu.c:1099) on demand-page,
    // returning FAULT_UNHANDLED_USER → kernel extinction. Reject at
    // the syscall entry point instead so a malformed userspace request
    // gets a clean -1 instead of a kernel termination. Mirrors
    // `mmu_map_mmio`'s IPS check at mmu.c:668.
    if ((pa + size - 1) >> 40)            return NULL;

    struct KObj_MMIO *k = kmalloc(sizeof(*k), KP_ZERO);
    if (!k) return NULL;

    k->magic = KOBJ_MMIO_MAGIC;
    k->pa    = pa;
    k->size  = size;
    k->ref   = 1;

    irq_state_t s = spin_lock_irqsave(&g_mmio_lock);

    // R9 F147 (P2) — rollback-asymmetry note:
    // Both failure paths below (overlap, slot-full) call kfree(k)
    // directly instead of going through kobj_mmio_unref. This is
    // INTENTIONAL because:
    //   (a) the claim slot was never allocated for k (we failed
    //       BEFORE g_mmio_claims[slot].owner = k), so there's nothing
    //       to release;
    //   (b) the refcount is 1 by construction (just set above) and
    //       no other holder exists yet, so atomic-dec-to-0 would be
    //       a no-op aside from the free.
    // The magic clobber + kfree pair mirrors kobj_mmio_free_internal's
    // tail. Future state added to KObj_MMIO that needs cleanup BEFORE
    // the claim is wired (e.g., a Burrow attachment installed in
    // kobj_mmio_create's body) MUST be torn down here too — adding a
    // single unref call won't work because unref's free_internal
    // expects find_slot_by_owner to succeed. The "unref" discipline
    // applies post-slot-installation only.
    //
    // P4-Ib spec invariant HwResourceExclusive: no two alive KObj_MMIO
    // overlap. Reject before allocating any slot resources.
    if (ranges_overlap(pa, size)) {
        spin_unlock_irqrestore(&g_mmio_lock, s);
        k->magic = 0;          // clobber to mark dead before kfree (defense)
        kfree(k);
        return NULL;
    }

    int slot = find_free_slot();
    if (slot < 0) {
        spin_unlock_irqrestore(&g_mmio_lock, s);
        k->magic = 0;
        kfree(k);
        return NULL;
    }

    g_mmio_claims[slot].owner = k;
    g_mmio_claims[slot].pa    = pa;
    g_mmio_claims[slot].size  = size;

    spin_unlock_irqrestore(&g_mmio_lock, s);

    __atomic_fetch_add(&g_mmio_created, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_mmio_live,    1u, __ATOMIC_RELAXED);
    return k;
}

void kobj_mmio_ref(struct KObj_MMIO *k) {
    if (!k)                              extinction("kobj_mmio_ref(NULL)");
    if (k->magic != KOBJ_MMIO_MAGIC)     extinction("kobj_mmio_ref of corrupted KObj_MMIO");

    // R9 F148 (P2) close: atomic ref bump. Without this, two CPUs
    // concurrently calling ref+unref could torn-update the count. The
    // returned old value drives the zero-from-positive check —
    // catching the "ref was already 0" case after the fact (the
    // increment has already happened; we extinct on observation).
    int old = __atomic_fetch_add(&k->ref, 1, __ATOMIC_RELAXED);
    if (old <= 0) {
        extinction("kobj_mmio_ref of zero-ref KObj_MMIO (already freed?)");
    }
}

static void kobj_mmio_free_internal(struct KObj_MMIO *k) {
    if (k->magic != KOBJ_MMIO_MAGIC)
        extinction("kobj_mmio_free_internal of corrupted KObj_MMIO");
    if (k->ref != 0)
        extinction("kobj_mmio_free_internal with ref > 0");

    // Release the claim slot before the kfree so a racing
    // kobj_mmio_create can immediately re-use the PA range.
    irq_state_t s = spin_lock_irqsave(&g_mmio_lock);
    int slot = find_slot_by_owner(k);
    if (slot < 0) {
        spin_unlock_irqrestore(&g_mmio_lock, s);
        extinction("kobj_mmio_free_internal: no claim slot for owner (UAF or double-free?)");
    }
    g_mmio_claims[slot].owner = NULL;
    g_mmio_claims[slot].pa    = 0;
    g_mmio_claims[slot].size  = 0;
    spin_unlock_irqrestore(&g_mmio_lock, s);

    // Defensive: clobber magic before kfree so a stale-pointer
    // dereference between free and SLUB-list-write extincts on the
    // magic check.
    k->magic = 0;

    kfree(k);
    __atomic_fetch_sub(&g_mmio_live, 1u, __ATOMIC_RELAXED);
}

void kobj_mmio_unref(struct KObj_MMIO *k) {
    if (!k) return;
    if (k->magic != KOBJ_MMIO_MAGIC)
        extinction("kobj_mmio_unref of corrupted KObj_MMIO");

    // R9 F148 (P2) close: atomic ref decrement. ACQ_REL ordering on
    // the dec ensures (a) all prior accesses to *k by ANY CPU happen
    // before the dec is observed (release on the decrement), and (b)
    // the post-dec free_internal sees the final state of *k coherently
    // (acquire on the returned value's interpretation).
    //
    // The returned OLD value drives the decision to free: only the
    // caller that observed old==1 (i.e., dec was 1→0) calls
    // free_internal. Concurrent decrements that produced old==2 or
    // higher don't see the zero edge — guaranteed by atomic semantics.
    int old = __atomic_fetch_sub(&k->ref, 1, __ATOMIC_ACQ_REL);
    if (old <= 0) {
        extinction("kobj_mmio_unref of zero-ref KObj_MMIO (double-free?)");
    }
    if (old == 1) {
        kobj_mmio_free_internal(k);
    }
}

void kobj_mmio_destroy(struct KObj_MMIO *k) {
    kobj_mmio_unref(k);
}

// =============================================================================
// R10 F154 (P1) close: kernel-MMIO reservation.
// =============================================================================
//
// Pre-claim PA ranges that the kernel uses directly (via mmu_map_mmio +
// the direct-map / vmalloc paths) so userspace `kobj_mmio_create` for
// overlapping PAs returns NULL. Each reservation occupies a slot in
// g_mmio_claims with a sentinel `owner = KOBJ_MMIO_KERNEL_RESERVED`;
// `ranges_overlap` rejects any subsequent overlapping create regardless
// of which proc requests it.

// Helper: insert one (pa, size) into g_mmio_claims as kernel-reserved.
// Caller is in boot-time context; we don't bother with the spinlock
// (single-CPU boot, no other claim path is active yet).
//
// Page-aligns the range (MMU works at PTE granularity) and DEDUPES
// against existing reservations — multiple sub-page entries that
// expand to the same page (e.g., QEMU virt has 8 virtio,mmio slots
// per page, spaced 0x200 apart) collapse to one reservation.
//
// Extincts on table-full because the static slot count is sized
// assuming kernel reservations + a few driver claims fit comfortably;
// an exhausted table at boot is a system-design error worth screaming
// about.
static void reserve_kernel_range(u64 pa, size_t size, const char *what) {
    if (size == 0)                              return;
    // Page-align: kernel may register a sub-page range (e.g. some MMIO
    // is < 4 KiB). Round outward to page boundaries so the claim
    // covers the full range the MMU has mapped.
    u64 aligned_pa  = pa & ~(u64)(PAGE_SIZE - 1);
    u64 end         = pa + size;
    u64 aligned_end = (end + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
    size_t aligned_size = (size_t)(aligned_end - aligned_pa);

    // Dedupe: if the page-aligned range already overlaps a prior
    // reservation, skip. ranges_overlap is the same predicate
    // kobj_mmio_create uses, so the dedupe semantics are exact.
    if (ranges_overlap(aligned_pa, aligned_size)) return;

    int slot = -1;
    for (int i = 0; i < KOBJ_MMIO_MAX; i++) {
        if (!g_mmio_claims[i].owner) { slot = i; break; }
    }
    if (slot < 0) {
        extinction("kobj_mmio_reserve_kernel_ranges: g_mmio_claims full "
                   "(KOBJ_MMIO_MAX too small for kernel reservations + driver headroom)");
    }
    g_mmio_claims[slot].owner = KOBJ_MMIO_KERNEL_RESERVED;
    g_mmio_claims[slot].pa    = aligned_pa;
    g_mmio_claims[slot].size  = aligned_size;

    uart_puts("  kobj_mmio: reserved kernel range ");
    uart_puts(what);
    uart_puts(" PA=");
    uart_puthex64(aligned_pa);
    uart_puts(" size=");
    uart_puthex64((u64)aligned_size);
    uart_puts("\n");
}

// Try to reserve a single DTB compatible's reg range, if present.
// Silently no-ops on DTB-absent (some compatibles may not exist on every
// platform — e.g., pci-host-ecam-generic isn't present on QEMU virt
// before -machine virt,gic-version=3 was added; we don't extinct on
// missing entries).
static void reserve_compat(const char *compat) {
    u64 pa = 0, size = 0;
    if (!dtb_get_compat_reg(compat, &pa, &size)) return;
    reserve_kernel_range(pa, (size_t)size, compat);
}

void kobj_mmio_reserve_kernel_ranges(void) {
    if (!g_mmio_initialized) {
        extinction("kobj_mmio_reserve_kernel_ranges before kobj_mmio_init");
    }

    // GIC v3: distributor + redistributor + (optional) ITS. Two reg
    // pairs at the same node; we reserve both via dtb_get_compat_reg_n.
    u64 gic_pa = 0, gic_size = 0;
    if (dtb_get_compat_reg_n("arm,gic-v3", 0, &gic_pa, &gic_size)) {
        reserve_kernel_range(gic_pa, (size_t)gic_size, "arm,gic-v3 dist");
    }
    if (dtb_get_compat_reg_n("arm,gic-v3", 1, &gic_pa, &gic_size)) {
        reserve_kernel_range(gic_pa, (size_t)gic_size, "arm,gic-v3 redist");
    }
    // GIC v2: two reg pairs (distributor + GICC cpu-interface). The GICC
    // region carries the kernel's IRQ ack/EOI registers, so reserving it is
    // load-bearing under v2 -- a CAP_HW_CREATE driver that claimed it could
    // scribble on live IRQ state (I-5). Both QEMU virt's gic-version=2
    // (arm,cortex-a15-gic) and real GIC-400 hardware (arm,gic-400, the Pi 4)
    // are covered; gic_init's autodetect accepts the same pair of compats.
    static const char *const gicv2_compats[] = {
        "arm,cortex-a15-gic", "arm,gic-400",
    };
    for (size_t i = 0; i < sizeof(gicv2_compats) / sizeof(gicv2_compats[0]); i++) {
        if (dtb_get_compat_reg_n(gicv2_compats[i], 0, &gic_pa, &gic_size)) {
            reserve_kernel_range(gic_pa, (size_t)gic_size, "gic-v2 dist");
        }
        if (dtb_get_compat_reg_n(gicv2_compats[i], 1, &gic_pa, &gic_size)) {
            reserve_kernel_range(gic_pa, (size_t)gic_size, "gic-v2 cpu-iface");
        }
    }

    // PL011 UART: kernel diagnostic console. A userspace driver could
    // not legitimately claim this and userspace-driven UART access
    // would corrupt the kernel's UART register state (clobber the
    // boot banner ABI, EXTINCTION delivery, etc.).
    reserve_compat("arm,pl011");

    // PCIe ECAM: kernel uses for VirtIO PCI enumeration (P4-H). The
    // ECAM mapping is per-bus; we reserve the WHOLE ECAM range (typically
    // 256 MiB on QEMU virt). Drivers should use individual VirtIO PCI
    // device handles, not raw ECAM.
    reserve_compat("pci-host-ecam-generic");

    // VirtIO MMIO transports — DELIBERATELY NOT RESERVED at P4-Ic5b1a
    // (refinement of R10 F154).
    //
    // R10 F154 originally reserved every `virtio,mmio` compatible in
    // DTB as part of the kernel-MMIO defense. The rationale matched the
    // GIC/PL011/ECAM ones: a userspace driver could otherwise claim a
    // PA range the kernel actively uses, creating undefined-behavior
    // overlap. The reservation was correct in spirit but over-broad in
    // application: the kernel touches virtio-mmio only during
    // `virtio_init` boot probe (reads `MagicValue` / `Version` /
    // `DeviceID` / `VendorID` to enumerate transports). After
    // `virtio_init` returns, the kernel holds the `mmu_map_mmio`
    // kernel-VA mapping but issues no further reads or writes — the
    // entire virtio-mmio register surface is dormant from the kernel's
    // perspective and exists for driver-process consumption.
    //
    // Leaving the reservation in place meant the legitimate Phase 4
    // driver-process consumer (P4-Ic5b2 virtio-blk driver) could not
    // claim its own device's MMIO without first un-reserving it — a
    // delegation API. At v1.0 trust boundary (only kproc grants
    // `CAP_HW_CREATE`, and kproc is the project root of trust for
    // hardware access) the delegation API adds complexity without
    // adding protection: any `CAP_HW_CREATE` holder is already trusted
    // by kproc, and we want them to claim virtio-mmio.
    //
    // The relaxation: virtio-mmio slots are NOT in
    // `g_mmio_claims[i].owner == KOBJ_MMIO_KERNEL_RESERVED`. A
    // `CAP_HW_CREATE` holder can claim any virtio-mmio PA. Multi-driver
    // contention on the same PA is still rejected by
    // `kobj_mmio_create`'s overlap check (HwResourceExclusive).
    //
    // What this DOES NOT cover: cross-trust-boundary leakage in Phase
    // 5+ when cap-grant becomes more permissive. If a non-kproc Proc
    // can grant `CAP_HW_CREATE` (currently impossible — only kproc has
    // `CAP_ALL` and rfork can only AND-down per `RforkWithCaps`), the
    // un-reserved virtio-mmio slots become a wider attack surface.
    // Phase 5+ that adds general cap-grant SHOULD revisit this: either
    // re-reserve and add a per-slot delegation API, or add a
    // KObj_VIRTIO_DEV intermediate kobj that's claimable only via a
    // higher-level mechanism that enforces "you must own this VirtIO
    // device" semantics.
    //
    // Lazarus W3 exception (the virtio-rng slot): the kernel CSPRNG
    // (kernel/random.c) now DRIVES the virtio-rng slot -- not just the
    // boot probe -- so "dormant after virtio_init" is no longer strictly
    // true for that one slot. It is still NOT page-reserved, because the
    // slot shares a 4 KiB page with up to 7 sibling slots that userspace
    // legitimately drives (net / input / gpu); a page-granular kernel
    // reservation would lock those out. The RNG slot stays under the
    // same v1.0 posture: kproc-only CAP_HW_CREATE; the kobj_mmio overlap
    // check; the kernel touches it transiently (reset-to-dormant between
    // pulls, serialized by random.c's g_rng_dev_lock); and no userspace
    // driver targets device-id RNG. A misbehaving CAP_HW_CREATE holder
    // writing the RNG slot is the SAME residual the virtio-blk slot
    // already carries -- the Phase-5+ revisit above (re-reserve + a
    // per-device-ownership kobj) closes both uniformly.
    //
    // GIC / PL011 / ECAM reservations stay — those are actively
    // accessed by the running kernel and a userspace claim would
    // produce real concurrent-access bugs (UART register clobber, GIC
    // configuration drift, ECAM enumeration interference).
}

int kobj_mmio_kernel_reserved_count(void) {
    int n = 0;
    irq_state_t s = spin_lock_irqsave(&g_mmio_lock);
    for (int i = 0; i < KOBJ_MMIO_MAX; i++) {
        if (g_mmio_claims[i].owner == KOBJ_MMIO_KERNEL_RESERVED) n++;
    }
    spin_unlock_irqrestore(&g_mmio_lock, s);
    return n;
}

bool kobj_mmio_pa_claimed(u64 pa, size_t size) {
    if (size == 0) return false;
    if (size > (u64)-1 - pa) return false;  // overflow ⇒ caller mis-use
    irq_state_t s = spin_lock_irqsave(&g_mmio_lock);
    bool claimed = ranges_overlap(pa, size);
    spin_unlock_irqrestore(&g_mmio_lock, s);
    return claimed;
}
