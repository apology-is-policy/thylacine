// Kernel-private GICv3 ITS backend. No ITS address or command crosses EL0.
#ifndef THYLACINE_ARM64_GIC_ITS_H
#define THYLACINE_ARM64_GIC_ITS_H
#include "gic_msi.h"
#include <thylacine/dtb.h>
void gic_its_init(gic_irq_handler_t handler, gic_msi_fault_handler_t fault_handler);
#ifdef KERNEL_TESTS
bool gic_its_test_command_ring(void);
#endif
// 1=lease returned, 0=no lease consumed, -1=failed lease quarantined.
int gic_its_alloc(const struct dtb_pci_msi *dt, u32 event, u64 generation,
                  struct gic_msi_route *out);
bool gic_its_retire(const struct gic_msi_route *route, bool source_quiesced);
// IRQ-safe property update + asynchronous invalidation. Never waits on ITS.
bool gic_its_set_enabled(u32 intid, bool enabled);
bool gic_its_enabled(u32 intid);
// Kernel regression injection, serialized with mapping retirement.
bool gic_its_set_pending(const struct gic_msi_route *route);
#endif
