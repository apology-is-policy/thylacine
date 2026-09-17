// GICv3 ITS/LPI ownership and synchronization.
//
// Tables and command rings have boot lifetime. Firmware selects the controller
// and DeviceID; an immutable enumeration binding selects its ITT. Dynamic leases
// own only EventID -> LPI mappings. A failed command quarantines the lease and
// faults that controller: hardware may have consumed any prefix of a batch.
//
// lpi_lock protects leases/properties; command_lock protects only ring writes
// and reader accounting. Order: lpi_lock -> command_lock. A ring submission is
// IRQ-safe and bounded; completion waits occur after BOTH locks are released.
// No ITS command wait occurs under a PCI/domain/Rendez lock. Per-entry MSI-X
// masking preserves device notifications while property invalidation completes.
//
// Reuse requires source reset proof, disabled property + INV, CLEAR/DISCARD/SYNC,
// and a CPU IRQ barrier. LPIs lack an SPI active bit: command completion alone
// does not prove that a CPU has finished an interrupt already acknowledged.
#include "gic_its.h"
#include "mmio.h"
#include "mmu.h"
#include "timer.h"
#include "uart.h"
#include "../../mm/phys.h"
#include <thylacine/spinlock.h>
#include <thylacine/virtio_pci.h>

#define ITS_MAX 4u
#define ITS_EVENTS 8u
#define CMD_BYTES 65536u
#define ITS_TIMEOUT_NS 100000000ull
#define GITS_CTLR 0x0000
#define GITS_TYPER 0x0008
#define GITS_CBASER 0x0080
#define GITS_CWRITER 0x0088
#define GITS_CREADR 0x0090
#define GITS_BASER 0x0100
#define ITS_VALID (1ull << 63)
#define ITS_NC (1ull << 59)
#define ITS_INNER_SHARE (1ull << 10)
#define ITS_PA_MASK 0x000ffffffffff000ull
#define GICR_CTLR 0x0000
#define GICR_PROPBASER 0x0070
#define GICR_PENDBASER 0x0078
#define LPI_FREE 0u
#define LPI_BUILDING 1u
#define LPI_LIVE 2u
#define LPI_RETIRING 3u
#define LPI_QUARANTINED 4u

struct its_command { u64 word[4]; };
struct its_device { u32 id; u16 rid; void *itt; };
struct its_controller {
    struct dtb_pci_msi dt;
    volatile u8 *base;
    struct its_command *commands;
    spin_lock_t command_lock;
    u64 produced, consumed;
    u64 targets[DTB_MAX_CPUS];
    struct its_device devices[VIRTIO_PCI_MAX_DEVS];
    u32 device_count;
    bool ready, faulted;
};
struct lpi_lease {
    struct gic_msi_route route;
    struct its_controller *its;
    u32 state, cpu;
};
static struct its_controller controllers[ITS_MAX];
static u32 controller_count;
static struct lpi_lease lpis[GIC_LPI_COUNT];
static spin_lock_t lpi_lock = SPIN_LOCK_INIT;
static u8 *properties;
static void *pending_tables[DTB_MAX_CPUS];
static bool lpi_tables_ready;
static u32 pending_faults;
static gic_msi_fault_handler_t fault_notify;

// Always clean CPU-produced bytes, including on coherent emulators. Property
// bytes are CPU-owned; pending/device/ITT tables are hardware-owned after their
// initial clean and are never dirtied by the CPU again. No cache-line writeback
// can overwrite a hardware update. Table register cache/shareability readback
// is checked before enabling an engine.
static void clean_range(const void *p, u64 bytes) {
    u64 ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    u64 line = 4ull << ((ctr >> 16) & 15u);
    u64 start = (u64)(uintptr_t)p & ~(line - 1);
    u64 end = (u64)(uintptr_t)p + bytes;
    for (u64 v = start; v < end; v += line)
        __asm__ volatile("dc cvac, %0" :: "r"(v) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void *table_alloc(unsigned order) {
    struct page *page = alloc_pages(order, KP_ZERO);
    if (!page) return NULL;
    void *p = pa_to_kva(page_to_pa(page));
    clean_range(p, PAGE_SIZE << order);
    return p;
}
static bool controller_faulted(struct its_controller *c) {
    return __atomic_load_n(&c->faulted, __ATOMIC_ACQUIRE);
}
static void notify_controller_fault(u32 index) {
    __atomic_fetch_or(&pending_faults, 1u << index, __ATOMIC_RELEASE);
    __asm__ volatile("dsb ishst" ::: "memory");
    (void)gic_send_ipi(0, IPI_MSI_FAULT);
}
static void controller_fault(struct its_controller *c) {
    if (!__atomic_exchange_n(&c->faulted, true, __ATOMIC_ACQ_REL) && c->base) {
        // IRQ-safe containment: stop accepting translations/commands without
        // waiting here. Every potentially DMA-owned table and lease remains
        // quarantined even if the engine never acknowledges this disable.
        io_write32(c->base + GITS_CTLR, 0);
        __asm__ volatile("dsb sy" ::: "memory");
        if (c->ready) {
            // Notification must not call back into PCI while an ITS or domain
            // lock is held. A permanent SGI on CPU0 drains a bounded mailbox.
            for (u32 n = 0; n < controller_count; n++) if (c == &controllers[n]) {
                notify_controller_fault(n);
                break;
            }
        }
    }
}
static void fault_dispatch(u32 intid, void *arg) {
    (void)intid; (void)arg;
    u32 pending = __atomic_exchange_n(&pending_faults, 0, __ATOMIC_ACQ_REL);
    for (u32 n = 0; n < controller_count; n++)
        if ((pending & (1u << n)) && fault_notify) fault_notify(controllers[n].dt.node);
}
// Called with command_lock held. Offsets wrap; absolute counters cannot alias
// because one command slot is always left empty. A stalled or impossible read
// faults the controller rather than fabricating a completion.
static bool command_progress(struct its_controller *c) {
    if (controller_faulted(c)) return false;
    u64 rd = io_read64(c->base + GITS_CREADR);
    if ((rd & ~((u64)CMD_BYTES - 1)) || (rd & 31u)) goto fault;
    u64 delta = (rd - (c->consumed & (CMD_BYTES - 1))) & (CMD_BYTES - 1);
    if (delta > c->produced - c->consumed) goto fault;
    c->consumed += delta;
    return true;
fault:
    controller_fault(c); return false;
}
// Copies at most four commands. Hardware read is observational, never a wait.
static bool submit(struct its_controller *c, const struct its_command *commands,
                   u32 count, u64 *ticket) {
    if (!count || count > 4) return false;
    irq_state_t f = spin_lock_irqsave(&c->command_lock);
    bool ok = command_progress(c);
    u64 bytes = (u64)count * sizeof(*commands);
    if (!ok || c->produced - c->consumed + bytes >= CMD_BYTES ||
        c->produced > ~0ull - bytes) {
        spin_unlock_irqrestore(&c->command_lock, f); return false;
    }
    for (u32 i = 0; i < count; i++) {
        u32 slot = (u32)(c->produced & (CMD_BYTES - 1)) / sizeof(*commands);
        for (u32 word = 0; word < 4; word++)
            c->commands[slot].word[word] = commands[i].word[word];
        clean_range(&c->commands[slot], sizeof(*commands));
        c->produced += sizeof(*commands);
    }
    *ticket = c->produced;
    io_write64(c->base + GITS_CWRITER, c->produced & (CMD_BYTES - 1));
    __asm__ volatile("dsb sy" ::: "memory");
    spin_unlock_irqrestore(&c->command_lock, f); return true;
}
static bool complete(struct its_controller *c, u64 ticket) {
    u64 start = timer_now_ns();
    do {
        irq_state_t f = spin_lock_irqsave(&c->command_lock);
        bool ok = command_progress(c), done = c->consumed >= ticket;
        spin_unlock_irqrestore(&c->command_lock, f);
        if (!ok) return false;
        if (done) return true;
        __asm__ volatile("yield" ::: "memory");
    } while (timer_now_ns() - start < ITS_TIMEOUT_NS);
    controller_fault(c); return false;
}
static struct its_command command(u32 op, u32 device, u64 w1, u64 w2) {
    return (struct its_command){ .word = { op | ((u64)device << 32), w1, w2, 0 } };
}
static bool issue_sync(struct its_controller *c, struct its_command cmd, u32 cpu) {
    struct its_command pair[2] = {cmd, command(5, 0, 0, c->targets[cpu])};
    u64 ticket;
    return submit(c, pair, 2, &ticket) && complete(c, ticket);
}

static bool init_lpi_tables(void) {
    static bool attempted;
    if (attempted) return lpi_tables_ready;
    attempted = true;
    u32 typer = io_read32((void *)(uintptr_t)(gic_dist_base() + 4));
    u32 cpus = dtb_cpu_count();
    if (!(typer & (1u << 17)) || ((typer >> 19) & 31u) < 13 ||
        !cpus || cpus > DTB_MAX_CPUS) return false;
    // Architectural minimum ID width is 14 (IDbits=13), hence space through
    // INTID 16383 even though this kernel currently leases only 64 LPIs.
    properties = table_alloc(4);
    if (!properties) return false;
    for (u32 i = 0; i < 8192; i++) properties[i] = 0xa2; // Group1, disabled
    clean_range(properties, 65536);
    for (u32 cpu = 0; cpu < cpus; cpu++) {
        u64 va, pa, t;
        if (!gic_redist_for_cpu(cpu, &va, &pa, &t) || !(t & 1u)) return false;
        // Do not replace firmware-owned live pending tables without a reset.
        if (io_read32((void *)(uintptr_t)(va + GICR_CTLR)) & 1u) return false;
        pending_tables[cpu] = table_alloc(4);
        if (!pending_tables[cpu]) return false;
    }
    for (u32 cpu = 0; cpu < cpus; cpu++) {
        u64 va; gic_redist_for_cpu(cpu, &va, NULL, NULL);
        volatile u8 *r = (void *)(uintptr_t)va;
        u64 prop = kva_to_pa(properties) | 13u | (1ull << 7) | ITS_INNER_SHARE;
        u64 pend = kva_to_pa(pending_tables[cpu]) | (1ull << 62) |
                   (1ull << 7) | ITS_INNER_SHARE;
        io_write64(r + GICR_PROPBASER, prop);
        io_write64(r + GICR_PENDBASER, pend);
        __asm__ volatile("dsb sy" ::: "memory");
        // Exact readback deliberately declines unsupported attribute/layout
        // combinations. The CPU always cleans to PoC even on coherent hosts.
        if (io_read64(r + GICR_PROPBASER) != prop ||
            io_read64(r + GICR_PENDBASER) != pend) return false;
        io_write32(r + GICR_CTLR, io_read32(r + GICR_CTLR) | 1u);
        u64 start = timer_now_ns();
        while (io_read32(r + GICR_CTLR) & (1u << 3)) {
            if (timer_now_ns() - start >= ITS_TIMEOUT_NS) return false;
        }
        if (!(io_read32(r + GICR_CTLR) & 1u)) return false;
    }
    // Partial initialization intentionally retains these bounded boot-time
    // allocations: a redistributor may already own their physical addresses.
    lpi_tables_ready = true;
    return true;
}
static bool setup_baser(struct its_controller *c, u32 index, u32 entries) {
    volatile u8 *reg = c->base + GITS_BASER + index * 8;
    u64 old = io_read64(reg);
    u64 entry_size = ((old >> 48) & 31u) + 1;
    u64 bytes = (u64)entries * entry_size;
    unsigned order = 4;
    while ((PAGE_SIZE << order) < bytes && order < 12) order++;
    if (bytes > (PAGE_SIZE << order)) return false;
    void *table = table_alloc(order);
    if (!table) return false;
    // 64KiB pages; at most 256 pages. All current PAs fit IPS=40, so the
    // special BASER high-address encoding for >48-bit PA is unnecessary.
    u64 value = ITS_VALID | ITS_NC | ITS_INNER_SHARE | (2ull << 8) |
        (((PAGE_SIZE << order) >> 16) - 1) | kva_to_pa(table);
    io_write64(reg, value);
    __asm__ volatile("dsb sy" ::: "memory");
    u64 writable = ITS_VALID | (1ull << 62) | (7ull << 59) | (7ull << 53) |
                   0x0000ffffffffffffull;
    return (io_read64(reg) & writable) == value;
}
static bool init_controller(struct its_controller *c) {
    c->base = mmu_map_mmio(c->dt.pa, 0x10000);
    if (!c->base) return false;
    io_write32(c->base + GITS_CTLR, 0);
    u64 start = timer_now_ns();
    while (!(io_read32(c->base + GITS_CTLR) & (1u << 31))) {
        if (timer_now_ns() - start >= ITS_TIMEOUT_NS) return false;
    }
    u64 typer = io_read64(c->base + GITS_TYPER);
    u32 devbits = (u32)((typer >> 13) & 31u) + 1;
    if (!(typer & 1) || ((typer >> 8) & 31u) < 2) return false;
    u32 max_device = 0;
    for (int n = 0; n < virtio_pci_dev_count(); n++) {
        const struct virtio_pci_dev *v = virtio_pci_dev_get(n);
        u16 rid = ((u16)v->bus << 8) | (v->dev << 3) | v->fn;
        struct dtb_pci_msi dt;
        if (!dtb_pci_msi_route(rid, &dt) || dt.node != c->dt.node) continue;
        if ((u64)dt.device_id >= (1ull << devbits) || dt.device_id == 0xffffffffu ||
            c->device_count == VIRTIO_PCI_MAX_DEVS) return false;
        // An msi-map mask may alias requester IDs. Two separately owned PCI
        // functions cannot share the same ITS device/event namespace.
        for (u32 i = 0; i < c->device_count; i++)
            if (c->devices[i].id == dt.device_id) return false;
        struct its_device *d = &c->devices[c->device_count++];
        d->id = dt.device_id; d->rid = rid; d->itt = table_alloc(0);
        if (!d->itt) return false;
        if (d->id > max_device) max_device = d->id;
    }
    if (!c->device_count) return false;
    bool device_table = false, collection_table = false;
    for (u32 i = 0; i < 8; i++) {
        u64 old = io_read64(c->base + GITS_BASER + i * 8);
        u32 type = (u32)(old >> 56) & 7u;
        if (type == 1) {
            if (device_table || !setup_baser(c, i, max_device + 1)) return false;
            device_table = true;
        } else if (type == 4) {
            if (collection_table || !setup_baser(c, i, dtb_cpu_count())) return false;
            collection_table = true;
        } else {
            io_write64(c->base + GITS_BASER + i * 8, 0); // no unused live tables
        }
    }
    if (!device_table || !collection_table) return false;
    c->commands = table_alloc(4);
    if (!c->commands) return false;
    u64 cb = kva_to_pa(c->commands) | ITS_VALID | ITS_NC | ITS_INNER_SHARE | 15;
    io_write64(c->base + GITS_CBASER, cb);
    io_write64(c->base + GITS_CWRITER, 0);
    if (io_read64(c->base + GITS_CBASER) != cb || io_read64(c->base + GITS_CREADR))
        return false;
    if (!init_lpi_tables()) return false;
    io_write32(c->base + GITS_CTLR, 1);
    if (!(io_read32(c->base + GITS_CTLR) & 1)) return false;
    for (u32 cpu = 0; cpu < dtb_cpu_count(); cpu++) {
        u64 pa, t;
        if (!gic_redist_for_cpu(cpu, NULL, &pa, &t)) return false;
        c->targets[cpu] = (typer & (1ull << 19)) ? pa : ((t >> 8) & 0xffffu) << 16;
        if (!issue_sync(c, command(9, 0, 0, ITS_VALID | c->targets[cpu] | cpu), cpu))
            return false;
        if (!issue_sync(c, command(13, 0, 0, cpu), cpu)) return false;
    }
    for (u32 i = 0; i < c->device_count; i++) {
        struct its_device *d = &c->devices[i];
        // Eight EventIDs => MAPD.Size=log2(8)-1. ITT entry size is <=16
        // bytes by the architectural TYPER field, so one page holds it.
        if (!issue_sync(c, command(8, d->id, 2, ITS_VALID | kva_to_pa(d->itt)), 0))
            return false;
    }
    return true;
}
void gic_its_init(gic_irq_handler_t handler, gic_msi_fault_handler_t fault_handler) {
    if (gic_version() != GIC_VERSION_V3 || !handler) return;
    fault_notify = fault_handler;
    gic_attach(IPI_MSI_FAULT, fault_dispatch, NULL);
    gic_enable_irq(IPI_MSI_FAULT);
    for (u32 id = GIC_LPI_MIN; id < GIC_LPI_MIN + GIC_LPI_COUNT; id++)
        gic_attach(id, handler, NULL);
    struct dtb_pci_msi dt;
    for (u32 n = 0; dtb_msi_controller_n(n, &dt); n++) {
        if (dt.kind != DTB_MSI_ITS || dt.size < 0x20000 || (dt.pa & 0xffffu) ||
            !dtb_msi_parent_matches(dt.node, gic_dist_pa()) || controller_count == ITS_MAX)
            continue;
        struct its_controller *c = &controllers[controller_count++];
        c->dt = dt;
        c->ready = init_controller(c);
        if (!c->ready) controller_fault(c);
        uart_puts("gic-msi: ITS "); uart_puts(c->ready ? "ready" : "unavailable");
        uart_puts(" devices="); uart_putdec(c->device_count); uart_puts("\n");
    }
}

static bool lease_matches(const struct lpi_lease *v, const struct gic_msi_route *r) {
    return v->route.generation == r->generation && v->route.controller == r->controller &&
           v->route.device_id == r->device_id && v->route.event_id == r->event_id;
}
int gic_its_alloc(const struct dtb_pci_msi *dt, u32 event, u64 generation,
                  struct gic_msi_route *out) {
    if (!dt || !out || event >= ITS_EVENTS || !generation || !lpi_tables_ready) return 0;
    struct its_controller *c = NULL;
    for (u32 n = 0; n < controller_count; n++)
        if (controllers[n].dt.node == dt->node) c = &controllers[n];
    if (!c || !c->ready || controller_faulted(c)) return 0;
    bool device_found = false;
    for (u32 n = 0; n < c->device_count; n++)
        if (c->devices[n].id == dt->device_id) device_found = true;
    if (!device_found) return 0;
    irq_state_t f = spin_lock_irqsave(&lpi_lock);
    u32 chosen = GIC_LPI_COUNT;
    for (u32 i = 0; i < GIC_LPI_COUNT; i++) {
        struct lpi_lease *v = &lpis[i];
        if (v->state == LPI_FREE) { if (chosen == GIC_LPI_COUNT) chosen = i; continue; }
        if (v->its == c && v->route.device_id == dt->device_id && v->route.event_id == event) {
            spin_unlock_irqrestore(&lpi_lock, f); return 0;
        }
    }
    if (chosen == GIC_LPI_COUNT) { spin_unlock_irqrestore(&lpi_lock, f); return 0; }
    struct lpi_lease *v = &lpis[chosen];
    v->its = c; v->state = LPI_BUILDING; v->cpu = 0;
    v->route = (struct gic_msi_route){ .address = dt->pa + 0x10040,
        .data = event, .intid = GIC_LPI_MIN + chosen, .controller = dt->node,
        .device_id = dt->device_id, .event_id = event, .generation = generation };
    properties[chosen] = 0xa2;
    clean_range(&properties[chosen], 1);
    spin_unlock_irqrestore(&lpi_lock, f);
    bool ok = issue_sync(c, command(10, dt->device_id,
                         event | ((u64)v->route.intid << 32), v->cpu), v->cpu) &&
              issue_sync(c, command(12, dt->device_id, event, 0), v->cpu);
    f = spin_lock_irqsave(&lpi_lock);
    v->state = ok ? LPI_LIVE : LPI_QUARANTINED;
    if (ok) *out = v->route;
    spin_unlock_irqrestore(&lpi_lock, f);
    return ok ? 1 : -1;
}
bool gic_its_set_enabled(u32 intid, bool enabled) {
    if (intid < GIC_LPI_MIN || intid - GIC_LPI_MIN >= GIC_LPI_COUNT || !lpi_tables_ready)
        return false;
    u32 index = intid - GIC_LPI_MIN;
    irq_state_t f = spin_lock_irqsave(&lpi_lock);
    struct lpi_lease *v = &lpis[index];
    if (v->state != LPI_LIVE || controller_faulted(v->its)) {
        spin_unlock_irqrestore(&lpi_lock, f); return false;
    }
    u8 value = enabled ? 0xa3 : 0xa2;
    if (properties[index] == value) { spin_unlock_irqrestore(&lpi_lock, f); return true; }
    properties[index] = value; clean_range(&properties[index], 1);
    struct its_command cmd = command(12, v->route.device_id, v->route.event_id, 0);
    u64 ticket;
    bool ok = submit(v->its, &cmd, 1, &ticket);
    if (!ok) {
        // A failed invalidation must not permit the caller to unmask its
        // device. Mask memory too and fault the controller; teardown retains
        // every uncertain mapping. There is no IRQ-context completion wait.
        properties[index] = 0xa2; clean_range(&properties[index], 1);
        controller_fault(v->its);
    }
    spin_unlock_irqrestore(&lpi_lock, f); return ok;
}
bool gic_its_enabled(u32 intid) {
    if (intid < GIC_LPI_MIN || intid - GIC_LPI_MIN >= GIC_LPI_COUNT || !lpi_tables_ready)
        return false;
    irq_state_t f = spin_lock_irqsave(&lpi_lock);
    u32 index = intid - GIC_LPI_MIN;
    bool enabled = lpis[index].state == LPI_LIVE && (properties[index] & 1) &&
                   !controller_faulted(lpis[index].its);
    spin_unlock_irqrestore(&lpi_lock, f); return enabled;
}
bool gic_its_retire(const struct gic_msi_route *r, bool source_quiesced) {
    if (!r || r->intid < GIC_LPI_MIN || r->intid - GIC_LPI_MIN >= GIC_LPI_COUNT)
        return false;
    u32 index = r->intid - GIC_LPI_MIN;
    irq_state_t f = spin_lock_irqsave(&lpi_lock);
    struct lpi_lease *v = &lpis[index];
    if ((v->state != LPI_LIVE && v->state != LPI_QUARANTINED) || !lease_matches(v, r)) {
        spin_unlock_irqrestore(&lpi_lock, f); return false;
    }
    v->state = LPI_RETIRING;
    struct its_controller *c = v->its;
    u32 cpu = v->cpu;
    properties[index] = 0xa2; clean_range(&properties[index], 1);
    spin_unlock_irqrestore(&lpi_lock, f);
    bool drained = issue_sync(c, command(12, r->device_id, r->event_id, 0), cpu);
    if (source_quiesced && drained) {
        drained = issue_sync(c, command(4, r->device_id, r->event_id, 0), cpu) &&
                  issue_sync(c, command(15, r->device_id, r->event_id, 0), cpu) &&
                  gic_synchronize_cpu(cpu, ITS_TIMEOUT_NS);
    } else drained = false;
    f = spin_lock_irqsave(&lpi_lock);
    v->state = drained ? LPI_FREE : LPI_QUARANTINED;
    spin_unlock_irqrestore(&lpi_lock, f);
    return drained;
}

bool gic_its_set_pending(const struct gic_msi_route *r) {
    if (!r || r->intid < GIC_LPI_MIN || r->intid - GIC_LPI_MIN >= GIC_LPI_COUNT)
        return false;
    irq_state_t f = spin_lock_irqsave(&lpi_lock);
    struct lpi_lease *v = &lpis[r->intid - GIC_LPI_MIN];
    bool ok = v->state == LPI_LIVE && lease_matches(v, r);
    if (ok) {
        struct its_command cmd = command(3, r->device_id, r->event_id, 0);
        u64 ticket;
        ok = submit(v->its, &cmd, 1, &ticket);
    }
    spin_unlock_irqrestore(&lpi_lock, f); return ok;
}

#ifdef KERNEL_TESTS
// Exercise the real mailbox/SGI path without destroying the boot controller.
// Engine containment itself is tested with private registers below.
bool gic_its_test_fault_notify(u32 node);
bool gic_its_test_fault_notify(u32 node) {
    for (u32 n = 0; n < controller_count; n++) {
        if (controllers[n].ready && controllers[n].dt.node == node) {
            notify_controller_fault(n);
            return true;
        }
    }
    return false;
}

// Fake register storage exercises the actual ring arithmetic and failure
// containment. It never replaces a live controller's mappings or registers.
bool gic_its_test_command_ring(void) {
    _Static_assert(sizeof(struct its_controller) <= PAGE_SIZE, "test controller fits one page");
    struct its_controller *c = kpage_alloc(KP_ZERO);
    void *registers = kpage_alloc(KP_ZERO);
    void *ring = table_alloc(4);
    bool ok = false;
    if (!c || !registers || !ring) goto out;
    c->base = registers; c->commands = ring;
    struct its_command pair[2] = { command(4, 7, 2, 0), command(5, 0, 0, 0) };
    u64 ticket = 0;
    // Two commands straddle the end and beginning of the ring.
    c->produced = c->consumed = CMD_BYTES - 32;
    io_write64(c->base + GITS_CREADR, CMD_BYTES - 32);
    if (!submit(c, pair, 2, &ticket) || ticket != CMD_BYTES + 32 ||
        io_read64(c->base + GITS_CWRITER) != 32 ||
        c->commands[CMD_BYTES / 32 - 1].word[0] != pair[0].word[0] ||
        c->commands[0].word[0] != pair[1].word[0]) goto out;
    io_write64(c->base + GITS_CREADR, 32);
    if (!complete(c, ticket) || c->consumed != ticket) goto out;
    // A full ring declines without overwriting unread data or ringing again.
    c->produced = CMD_BYTES - 32; c->consumed = 0;
    io_write64(c->base + GITS_CREADR, 0);
    if (submit(c, pair, 1, &ticket) || controller_faulted(c) ||
        io_read64(c->base + GITS_CWRITER) != 32) goto out;
    // A hardware stalled flag disables the engine and never advances consume.
    io_write32(c->base + GITS_CTLR, 1);
    io_write64(c->base + GITS_CREADR, 1);
    if (submit(c, pair, 1, &ticket) || !controller_faulted(c) ||
        io_read32(c->base + GITS_CTLR) != 0 || c->consumed != 0) goto out;
    // A reader beyond the published producer is equally invalid.
    c->faulted = false; c->produced = 32; c->consumed = 0;
    io_write32(c->base + GITS_CTLR, 1);
    io_write64(c->base + GITS_CREADR, 64);
    if (submit(c, pair, 1, &ticket) || !controller_faulted(c) ||
        io_read32(c->base + GITS_CTLR) != 0 || c->consumed != 0) goto out;
    // Lack of hardware progress has a real deadline and the same containment.
    c->faulted = false; c->produced = 32; c->consumed = 0;
    io_write32(c->base + GITS_CTLR, 1);
    io_write64(c->base + GITS_CREADR, 0);
    if (complete(c, 32) || !controller_faulted(c) || io_read32(c->base + GITS_CTLR)) goto out;
    ok = true;
out:
    if (ring) free_pages(pa_to_page(kva_to_pa(ring)), 4);
    if (registers) kpage_free(registers);
    if (c) kpage_free(c);
    return ok;
}
#endif
