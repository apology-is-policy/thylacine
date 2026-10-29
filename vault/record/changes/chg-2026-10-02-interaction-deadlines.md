---
id: chg-2026-10-02-interaction-deadlines
type: chg
title: "Preserve initial seat membership and bound control lifetimes"
date: 2026-10-02
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The shared interaction owner and controller table accept initial normal seat zero
without confusing it with absent membership. A composed publication/copy/paste/
retirement regression and separate owner-layer mutations cover the correction.

Bind and Publish now share CHECK's admission deadline. Expiry retires provisional
authority and reports once without releasing an in-flight transport slot. Exact
late replies drain; confirmed transport closure permanently disables the owner.
Runtime dispatch and channel teardown remain activation prerequisites. Evidence,
failed-run diagnoses and native coverage are recorded in HALCYON-INTERACTION-STATUS.
Single-agent review; no public clipboard activation or Main landing.
