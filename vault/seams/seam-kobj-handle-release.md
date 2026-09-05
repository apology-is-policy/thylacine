---
id: seam-kobj-handle-release
type: seam
title: "No syscall releases an individual KObj_MMIO / IRQ / DMA handle"
status: open
surface: [sub-stratum-bdev]
opened-by: chg-2026-08-02-stratum-sweep
tracker: ""
created: 2026-08-02
updated: 2026-08-02
---
## Owed

A release path for hardware kobj handles — or an explicit statement that
process exit is the only one, promoted from a source comment to a design
position.

## The gap

`bdev_thylacine.c`'s `op_close` frees its own struct, destroys the mutex,
and leaves the MMIO bank, the IRQ handle and the two DMA regions to be
reclaimed at process exit. The comment is honest about why: the kernel has
no syscall to release an individual `KOBJ_MMIO` / `KOBJ_IRQ` / `KOBJ_DMA`
handle.

For the boot stratumd this is exactly right and costs nothing. It is a
single-bdev process; the handles live as long as it does; process exit
reclaims everything, which is also what makes the DEK story in
[[sub-stratum-session]] work.

## When it bites

The moment a process opens a *second* bdev, or opens one, closes it, and
opens another. Then `close` leaks the MMIO bank claim and the DMA regions
for the remaining lifetime of the process, and — because a hardware
resource claim is exclusive — a re-open of the same device fails against a
claim held by nobody.

Invariant B-6 in the driver's header already records the narrowing that
makes today safe: *the MMIO bank claim and map happen once at open; we do
not re-claim across re-opens.* That is a statement about the single-bdev
process model, not about the driver.

## Risk while open

None at v1.0 — nothing opens two block devices. The risk is that the
narrowing is invisible at the call site: a future caller that opens a
second bdev gets a resource leak and an exclusivity failure with no
diagnostic pointing here.
