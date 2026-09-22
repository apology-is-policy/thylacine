// Kernel-owned MSI routing. No address/data from this API crosses into EL0.
#ifndef THYLACINE_ARM64_GIC_MSI_H
#define THYLACINE_ARM64_GIC_MSI_H
#include "gic.h"

struct gic_msi_route {
    u64 address;
    u32 data, intid, controller, device_id, event_id;
    u64 generation; // allocator identity, independent of PCI event tickets
};
// Boot-only, after GIC and kernel MMIO reservation, before secondaries/EL0.
// The permanent callback must resolve endpoints through pinned membership.
typedef void (*gic_msi_fault_handler_t)(u32 controller_node);
void gic_msi_init(gic_irq_handler_t handler, gic_msi_fault_handler_t fault_handler);
bool gic_msi_reserved(u32 intid);
u32 gic_msi_live_count(void); // includes quarantined and in-progress leases
// Returns a masked route; requester identity is kernel-derived from the BDF.
// Unsupported controller/layout or resource exhaustion fails explicitly.
bool gic_msi_alloc(u16 requester_id, u32 event_id, struct gic_msi_route *out);
// Caller has masked the device entry, completed posted writes, removed endpoint
// membership and drained its dispatch pins. source_quiesced additionally means
// no further message can reach this routing identity. A missing proof or failed
// controller drain quarantines the vector; it is never reassigned on a timer.
bool gic_msi_retire(const struct gic_msi_route *route, bool source_quiesced);
// Kernel tests may inject a pending notification into their held route.
bool gic_msi_set_pending(const struct gic_msi_route *route);
#endif
