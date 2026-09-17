// PCI-function-scoped interrupt endpoints. See docs/PCI-INTERRUPTS-DESIGN.md.
#ifndef THYLACINE_PCI_IRQ_H
#define THYLACINE_PCI_IRQ_H
#include <thylacine/types.h>
struct KObj_PCI;
struct KObj_IRQ;

#define PCI_IRQ_INTX 1u
#define PCI_IRQ_MSIX 2u
#define PCI_IRQ_DISARMED 0u
#define PCI_IRQ_ARMED 1u
#define PCI_IRQ_DELIVERED 2u
#define PCI_IRQ_REVOKED 3u
#define PCI_IRQ_FAULT 4u
#define PCI_IRQ_EVENT 1u
#define PCI_IRQ_RETRY 2u
#define PCI_IRQ_COOLDOWN 3u

// WAIT is replayable until COMPLETE spends the ticket. A failed copy-out
// cannot lose interrupt ownership. Generation and sequence are both required.
struct pci_irq_event {
    u64 generation, sequence;
    u32 count, reason;
    u64 retry_after_ns;
};
struct pci_irq_info {
    u64 generation, deliveries, retries, cooldowns;
    u32 mode, state, table_index, reserved;
};
_Static_assert(sizeof(struct pci_irq_event) == 32, "PCI IRQ event ABI");
_Static_assert(sizeof(struct pci_irq_info) == 48, "PCI IRQ info ABI");

void pci_irq_init(void); // boot, before SMP/EL0
struct KObj_IRQ *pci_irq_create(struct KObj_PCI *pci, u32 mode, u32 ordinal, int *error);
int pci_irq_arm(struct KObj_IRQ *irq);
int pci_irq_wait(struct KObj_IRQ *irq, u64 timeout_ns, struct pci_irq_event *event);
int pci_irq_complete(struct KObj_IRQ *irq, u64 generation, u64 sequence);
int pci_irq_disable(struct KObj_IRQ *irq);
int pci_irq_get_info(struct KObj_IRQ *irq, struct pci_irq_info *info);
struct KObj_PCI *pci_irq_owner(struct KObj_IRQ *irq); // borrowed while IRQ pinned
void pci_irq_revoke_function(struct KObj_PCI *pci);
void pci_irq_free(struct KObj_IRQ *irq); // refcount reached zero
#endif
