---
id: chg-2026-10-01-clipboard-seat-ordering-review
type: chg
title: "Identify the missing clipboard cancellation order at trusted-seat takeover"
date: 2026-10-01
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
At Astra fda6de374, Lictor's hardware/key quiescence ACK does not include the
separate Halcyon clipboard owner. Tapestry suspends its own interaction contexts,
but cannot retract an HIA1 receipt already queued to Halcyon. Broker::seat
cancels correctly when notified; notification timing does not prove cancellation
before trusted input. The public application clipboard endpoint remains off.

Two controlled schedules compiled against production broker/store sources
confirm late notification allows publication and prior cancellation prevents it.
Evidence: work/oct1-hi-seat-review, with source hashes. These checks are blind
to live scheduling, kernel transport retirement and graphical takeover. No new
runtime or security-key exposure result is claimed. Existing graphical SAK
input isolation is a separate property.

The draft docs/HALCYON-INTERACTION-SEAT-REVIEW.md offers a bounded userspace
cancellation barrier, with a renderer responsiveness dependency at SAK entry,
or an explicitly weaker completion contract that preserves SAK independence.
Neither alternative is ratified or implemented by this documentation change.
Review is single-agent; all four separate authority/settings drafts are kept.
