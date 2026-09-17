// GIC MSI domains. Firmware identifies frames; MSI_TYPER identifies their SPIs.
// Reservation is independent of allocation: an idle MSI SPI is never a raw IRQ.
// Permanent controller/frame state avoids callback references to freed leases.
#include "gic_msi.h"
#include "gic_its.h"
#include "mmu.h"
#include "mmio.h"
#include "timer.h"
#include "uart.h"
#include <thylacine/dtb.h>
#include <thylacine/irqfwd.h>
#include <thylacine/spinlock.h>

#define MSI_FRAMES 8u
#define MSI_VECTOR_LIMIT 64u
#define V2M_TYPER 0x008u
#define V2M_SETSPI_NS 0x040u
#define V2M_IIDR 0xfccu
#define VECTOR_FREE 0u
#define VECTOR_LIVE 1u
#define VECTOR_RETIRING 2u
#define VECTOR_QUARANTINED 3u

struct msi_frame {
    struct dtb_pci_msi dt;
    u32 first, count, data_offset;
    bool usable;
};
struct msi_vector {
    u64 generation;
    u32 state, frame;
};
static struct msi_frame frames[MSI_FRAMES];
static u32 frame_count, live_vectors;
static bool reserved[GIC_NUM_INTIDS];
static struct msi_vector vectors[GIC_NUM_INTIDS];
static spin_lock_t msi_lock = SPIN_LOCK_INIT;
static u64 generation;

void gic_msi_init(gic_irq_handler_t handler, gic_msi_fault_handler_t fault_handler) {
    gic_its_init(handler, fault_handler);
    struct dtb_pci_msi dt;
    for (u32 i = 0; dtb_msi_controller_n(i, &dt); i++) {
        if (dt.kind != DTB_MSI_V2M) continue;
        if (dt.size < 0x1000 || dt.pa & 0xfffull ||
            !dtb_msi_parent_matches(dt.node, gic_dist_pa())) continue;
        volatile u8 *base = mmu_map_mmio(dt.pa, 0x1000);
        if (!base) continue;
        u32 typer = io_read32(base + V2M_TYPER);
        u32 first = dt.spi_count ? dt.spi_base : (typer >> 16) & 0x3ffu;
        u32 count = dt.spi_count ? dt.spi_count : typer & 0x3ffu;
        if (first < GIC_SPI_MIN || !count || first > gic_max_intid() ||
            count > gic_max_intid() + 1u - first) continue;
        bool usable = handler != NULL && frame_count < MSI_FRAMES;
        for (u32 id = first; id < first + count; id++) {
            bool level;
            if (reserved[id] || kobj_irq_intid_claimed(id) || dtb_pci_intid_is_level(id, &level))
                usable = false;
        }
        // Even unsupported/conflicting frames must deny raw claims. Do not
        // overwrite a live wired/kernel handler when firmware ranges conflict.
        for (u32 id = first; id < first + count; id++) reserved[id] = true;
        if (!usable) {
            // A later frame can overlap one already discovered. Disable every
            // participating allocator; no controller gets to win by DT order.
            for (u32 old = 0; old < frame_count; old++)
                if (first < frames[old].first + frames[old].count && frames[old].first < first + count)
                    frames[old].usable = false;
            continue;
        }
        u32 iid = io_read32(base + V2M_IIDR);
        u32 data_offset = 0;
        // Documented GICv2m implementations with a relative SPI payload.
        // The standard frame and QEMU's SBSA model use the absolute INTID.
        if (iid == 0x06000170u) data_offset = first; // X-Gene
        else if (iid == 0x0000013fu) data_offset = 32; // Broadcom NS2
        struct msi_frame *f = &frames[frame_count];
        f->dt = dt; f->first = first; f->count = count;
        f->data_offset = data_offset; f->usable = true;
        for (u32 id = first; id < first + count; id++) {
            gic_disable_irq(id);
            gic_set_spi_edge_triggered(id);
            gic_attach(id, handler, NULL);
            vectors[id].frame = frame_count;
        }
        frame_count++;
        uart_puts("gic-msi: v2m first="); uart_putdec(first);
        uart_puts(" count="); uart_putdec(count); uart_puts("\n");
    }
}
bool gic_msi_reserved(u32 intid) {
    return (intid < GIC_NUM_INTIDS && reserved[intid]) ||
           (intid >= GIC_LPI_MIN && intid - GIC_LPI_MIN < GIC_LPI_COUNT);
}
bool gic_msi_alloc(u16 requester_id, u32 event_id, struct gic_msi_route *out) {
    struct dtb_pci_msi dt;
    if (!out || !dtb_pci_msi_route(requester_id, &dt)) return false;
    if (dt.kind == DTB_MSI_ITS) {
        irq_state_t flags = spin_lock_irqsave(&msi_lock);
        if (live_vectors >= MSI_VECTOR_LIMIT || generation == ~0ull) {
            spin_unlock_irqrestore(&msi_lock, flags); return false;
        }
        u64 gen = ++generation;
        live_vectors++; // reserve quota before the out-of-lock command wait
        spin_unlock_irqrestore(&msi_lock, flags);
        int result = gic_its_alloc(&dt, event_id, gen, out);
        if (result == 0) {
            flags = spin_lock_irqsave(&msi_lock);
            live_vectors--;
            spin_unlock_irqrestore(&msi_lock, flags);
        } // result=-1 retains quota for the backend's quarantined mapping
        return result == 1;
    }
    if (dt.kind != DTB_MSI_V2M) return false;
    irq_state_t flags = spin_lock_irqsave(&msi_lock);
    if (live_vectors >= MSI_VECTOR_LIMIT || generation == ~0ull) goto fail;
    for (u32 fidx = 0; fidx < frame_count; fidx++) {
        struct msi_frame *f = &frames[fidx];
        if (!f->usable || f->dt.node != dt.node) continue;
        for (u32 id = f->first; id < f->first + f->count; id++) {
            struct msi_vector *v = &vectors[id];
            if (v->state != VECTOR_FREE) continue;
            v->state = VECTOR_LIVE; v->generation = ++generation;
            live_vectors++;
            *out = (struct gic_msi_route){ .address = dt.pa + V2M_SETSPI_NS,
                .data = id - f->data_offset, .intid = id, .controller = dt.node,
                .device_id = dt.device_id, .event_id = event_id, .generation = v->generation };
            spin_unlock_irqrestore(&msi_lock, flags);
            return true;
        }
    }
fail:
    spin_unlock_irqrestore(&msi_lock, flags);
    return false;
}
bool gic_msi_retire(const struct gic_msi_route *route, bool source_quiesced) {
    if (!route) return false;
    if (route->intid >= GIC_LPI_MIN) {
        if (!gic_its_retire(route, source_quiesced)) return false;
        irq_state_t flags = spin_lock_irqsave(&msi_lock);
        live_vectors--;
        spin_unlock_irqrestore(&msi_lock, flags);
        return true;
    }
    if (route->intid >= GIC_NUM_INTIDS) return false;
    u32 id = route->intid;
    irq_state_t flags = spin_lock_irqsave(&msi_lock);
    struct msi_vector *v = &vectors[id];
    if ((v->state != VECTOR_LIVE && v->state != VECTOR_QUARANTINED) ||
        v->generation != route->generation ||
        frames[v->frame].dt.node != route->controller) {
        spin_unlock_irqrestore(&msi_lock, flags); return false;
    }
    v->state = VECTOR_RETIRING;
    spin_unlock_irqrestore(&msi_lock, flags);
    // No domain/allocation lock is held across controller completion. State
    // RETIRING excludes reuse while an IRQ already acknowledged on another
    // CPU finishes EOI. Without source quiescence no amount of waiting proves
    // that a late message will not arrive, so quarantine instead.
    bool drained = false;
    gic_disable_irq(id);
    if (source_quiesced) {
        u64 deadline = timer_now_ns() + 10000000ull;
        do {
            drained = gic_drain_spi(id);
            if (drained) break;
            __asm__ volatile("yield" ::: "memory");
        } while (timer_now_ns() < deadline);
    }
    flags = spin_lock_irqsave(&msi_lock);
    v->state = drained ? VECTOR_FREE : VECTOR_QUARANTINED;
    if (drained) live_vectors--;
    spin_unlock_irqrestore(&msi_lock, flags);
    return drained;
}

bool gic_msi_set_pending(const struct gic_msi_route *r) {
    if (!r) return false;
    if (r->intid >= GIC_LPI_MIN) return gic_its_set_pending(r);
    if (r->intid >= GIC_NUM_INTIDS) return false;
    irq_state_t f = spin_lock_irqsave(&msi_lock);
    struct msi_vector *v = &vectors[r->intid];
    bool ok = v->state == VECTOR_LIVE && v->generation == r->generation &&
              frames[v->frame].dt.node == r->controller && gic_set_pending_spi(r->intid);
    spin_unlock_irqrestore(&msi_lock, f); return ok;
}

u32 gic_msi_live_count(void) {
    irq_state_t f = spin_lock_irqsave(&msi_lock);
    u32 count = live_vectors;
    spin_unlock_irqrestore(&msi_lock, f); return count;
}
