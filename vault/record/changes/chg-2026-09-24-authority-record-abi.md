---
id: chg-2026-09-24-authority-record-abi
type: chg
title: "Reserve canonical authority record ABI"
date: 2026-09-24
arc: arc-user-authority
commits: ["*(pending)*"]
touched: [sub-corvus-authority]
established: [abi-user-authority]
closed: []
opened: []
mirrors-checked: [abi-user-authority]
depth: skeletal
created: 2026-09-24
---
Reserve the canonical record bytes, enum/action values and Corvus verbs before
codec consumers. Aux confirms no conflicting Corvus/LCUR reservations (Yip0119).
Byte-array layouts avoid alignment/endian casts; mirror source check matches all
constants and fields. This is a partial UA-0 reservation: kernel Admin/admission,
request/reply and LCURv2 shapes are still owed before their consumers.

Compile-time C/Rust assertions are present; their compiler execution is queued
behind Main's mac lease. Source mirror verification passed 47 constants and
both complete layouts. No runtime consumer is enabled by this reservation.
