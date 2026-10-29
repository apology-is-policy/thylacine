---
id: spec-tapestry-storage
type: spec
title: "tapestry_storage.tla"
models: [sub-tapestryd, sub-libtapestry]
pins: [inv-i40]
cfgs:
  - "tapestry_storage.cfg -- clean: 287 states, depth 20"
  - "tapestry_storage_buggy_offer.cfg -- OfferFresh"
  - "tapestry_storage_buggy_fid.cfg -- FidFresh"
  - "tapestry_storage_buggy_dead.cfg -- NoResurrection"
  - "tapestry_storage_buggy_mapping.cfg -- ClientBacked"
  - "tapestry_storage_buggy_device.cfg -- DeviceBacked"
  - "tapestry_storage_buggy_scanout.cfg -- SuspendedUnbound"
  - "tapestry_storage_buggy_partial.cfg -- FirstFrameComplete"
gate: "hidden storage visibility, generation admission, resume publication or retirement"
created: 2026-10-04
updated: 2026-10-04
---
This companion to [[spec-tapestry-present]] models cooperative hidden storage
with independent client, server, backend and display holders. Two fresh
pixel generations and three visibility epochs cover queued stale offers and
fid reuse across suspension. Seven named mutants establish nonvacuous detection.

The client must drain presents before suspension; the existing present model
owns that proof. No liveness claim when allocation or the backend refuses.
Geometry, protocol encoding, exact quotas and independent SAK progress require
native evidence. Design: docs/TAPESTRY-STORAGE.md, approved scripture cfa478824.
Production implementation is pending at this model-first checkpoint.
