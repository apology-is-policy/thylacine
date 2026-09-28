---
id: fnd-b1a-prime-r1-f2
type: fnd
title: "The fork clone charged the child the resident data pages only, while it mirrors every node page and the child's takes refund nodes: page_count and the pool drift below the truth"
round: adt-b1a-prime-r1
severity: P1
status: fixed
surface: [sub-kernel-addrspace, sub-kernel-pagemap, sub-kernel-burrow]
threatens: [inv-i32]
fixed-by: chg-2026-09-23-b1a-prime-capacity
regression: "capacity.fork_clone_charges_pages_and_nodes; RED noclonefootprint"
created: 2026-09-23
---
## Prosecution

`burrow_clone_cow` mirrors every NODE page of the source map
(`pagemap_node_count(src)` nodes, allocated uncharged on the promise that the
caller charges the clone's footprint), and `pagemap_take` refunds the nodes it
empties to the address space that releases them -- but `clone_one_vma` charged
the child `burrow_lazy_resident_count(minted)`, the resident DATA pages alone.
`burrow_lazy_footprint` (pages plus nodes) existed, documented as "what a clone
charges", with zero callers. A forked child that detaches or decommits a touched
mapping therefore refunds nodes it never paid for: its `page_count` drifts below
the truth and the pool with it -- fork and detach in a loop drive the machine-wide
count below what is held, and the bound is not a bound. Twelve tests and a green
boot did not see it because none forked a touched lazy mapping and released it
from the child.

## Fix

The one call (`burrow_lazy_footprint`), found by the chunk's own self-audit
before the round and independently by the reviewer;
`capacity.fork_clone_charges_pages_and_nodes` is the witness (the parent's 3
pages + 5 nodes charged again to the child, a detach in the child refunding
exactly one mapping's footprint, the pool exact throughout) and its RED is the
bug itself.
