# Phase 8 status — Linux compat + network

Phase 8 (ROADMAP §9, §2.2): the network arc → container runner (#70) →
on-system toolchain (#67) → Linux binary shim. This doc tracks the **network
arc** (NET-DESIGN.md), the first Phase-8 work.

## TL;DR

The #68 network charter (net-0) is bound scripture; net-1 (the reusable
virtio-net frame-transport driver) is landed + proven. net-1 surfaced **#140**
(two persistent userspace drivers cannot soundly share a 4 KiB virtio-mmio
page), so net-2 is **preempted** by the **virtio-PCI transport sub-arc**
(pci-0..pci-3, `docs/VIRTIO-PCI-DESIGN.md`, the user-voted DFS fork 2026-06-15).
**pci-0..pci-3 are LANDED** — the scripture, the kernel `KObj_PCI` + the 3
syscalls, the userspace `PciDev` + `VirtioNetPci` driver (a PCI virtio-net ARP
round-trip passes on its own page-aligned BAR), and the focused soundness audit
(Opus 4.8 max + self-audit, CLEAN 0/0/0/4 — all P3, F2/F3/F4 fixed + F1 tracked).
**The virtio-PCI transport sub-arc is COMPLETE.** net-2..net-8 (netd + smoltcp,
listen/accept, cs/dns, socket-compat, dev9p.poll, TLS/NTP, exit criteria) now
resume on the PCI NIC.

## Landed chunks

| Commit | What | Tests |
|---|---|---|
| `54a6bf6` | **net-0**: the #68 network charter (`docs/NET-DESIGN.md`) — scripture, no code; the 3 votes (shared netd + view-narrowing; pouch socket-compat; ns-restriction firewall) | n/a (scripture) |
| `ff3a621` (+audit-close) | **net-1**: `usr/lib/netdev` — the reusable `VirtioNet` RX+TX frame driver (send/poll_rx/recycle/quiesce) + host-tested `ring` + the `netdev-test` boot probe | 7 host ring tests + `netdev-test: PASS 24/24` + boot OK + SMP gate 0 corruption + Opus-4.8 audit 0/0/1/4 (F1 P2 + F2/F3 P3 fixed; F4/F5 closed) |
| `378c667` | **pci-0**: the virtio-PCI transport scripture (`docs/VIRTIO-PCI-DESIGN.md` + ARCH §13.2/§25.4/I-5 + NET-DESIGN §17 + ROADMAP) — the #140 DFS fork; net-only, INTx | n/a (scripture) |
| `5a1b36c` | **pci-1a**: the DTB/config primitives — `dtb_pci_intx_route` (interrupt-map → GIC INTID), `dtb_pci_mem_window`, `dtb_get_compat_prop`, `virtio_pci_cfg_write{8,16,32}`; no ABI/kobj change | 883/883 (+3: `dtb.pci_intx_route` full swizzle + `dtb.pci_mem_window` + `virtio_pci.cfg_write_bounds`) + boot OK; SMP-inert (gate at pci-1b) |
| `89a6c69` | **pci-1b**: the `KObj_PCI` kernel object — `kernel/pci_handle.{c,h}` (exclusive `g_pci_claims` per-(bus,dev,fn) claim; bump-assign BARs from `dtb_pci_mem_window` with width-correct 32/64-bit sizing; `VIRTIO_PCI_CAP_*` cap-walk into `regions[]`, bounded + OOB-validated; INTx swizzle; quiesce-before-free) + `KOBJ_PCI=11` joining `KOBJ_KIND_HW_MASK` (I-5 non-transferable + NoHwDup for free) + `kobj_pci_init` at boot | 889/889 (+6: `pci.bar_decode_size` + `pci.claim_rng`/`claim_unknown`/`claim_exclusive`/`unref_releases_bars`/`live_count_balances`, claiming the live idle rng-pci) + boot OK + SMP gate; **self-found+fixed**: 32-bit BAR size mask must invert in 32-bit width (a 64-bit invert yields a bogus exabyte size) |
| `11eb559` | **pci-1c**: the 3 syscalls — `SYS_PCI_CLAIM=76` / `SYS_PCI_MAP_BAR=77` / `SYS_PCI_INFO=78` (`kernel/syscall.c` handlers + dispatch) + the 208-byte `struct t_pci_info` ABI (`_Static_assert`-pinned in the kernel header, the libt mirror, and the libthyla-rs `TPciInfo` with `offset_of!` asserts) + libt (`t_pci_claim`/`t_pci_map_bar`/`t_pci_info`) + libthyla-rs raw wrappers. CLAIM: `CAP_HW_CREATE` gate → `kobj_pci_claim` → mint fixed R\|W\|MAP (no TRANSFER). MAP_BAR: mirrors `sys_mmio_map` via the `kobj_pci_bar_mmio` seam (bar_index `>= 6` rejected before the u32 narrowing). INFO: zero-init + fill + byte copy-out (no implicit padding → no stack leak). | 891/891 (+2: `pci.syscall_reject` + `pci.syscall_claim_info`) + boot OK + SMP gate |
| `2908e0e` | **pci-2**: the userspace PCI transport. `libthyla-rs::hardware::PciDev` (claim + info + map-BARs over the pci-1c syscalls; per-BAR VA window `PCI_BAR_VA_STRIDE`; `region()` bounds each cap-region within its BAR; non-transferable I-5 like Mmio/Irq/Dma) + `netdev::virtio_pci::VirtioNetPci` (the PCI sibling of `VirtioNet`: reuses `ring` VERBATIM + the same RX/TX API + audit hardenings; the virtio-pci-modern `common`/`notify`/`isr`/`device` register transport; `CCFG_MIN_LEN`/`DEVICE_CFG_MIN_LEN` region-size guards [self-audit]; device-reset quiesce-on-drop) + `usr/netdev-pci-test` boot probe + `run-vm.sh` adds `virtio-net-pci,disable-legacy=on` (separate `net1` slirp; the MMIO net + `netdev-test` kept). `virtio.rs` left byte-identical (net-1 proof intact). | host ring 7/7 + 891/891 + boot OK (both `netdev-test` + `netdev-pci-test` PASS 24/24) + 0 EXTINCTION + login E2E + SMP gate |
| `44fc3fb` | **pci-3**: the focused soundness audit (Opus 4.8 max prosecutor + concurrent self-audit) over pci-1b/1c/2 — **CLEAN 0 P0 / 0 P1 / 0 P2 / 4 P3**. F1=R1 partial-map leak **tracked** (the suggested `t_burrow_detach` fix is a verified no-op: `SYS_BURROW_DETACH` is confined above 4 GiB, the driver-VA windows live below it by design → proc-exit-bounded like every virtio mapping; single-BAR NIC never triggers it). F2=R2 notify-doorbell bound **fixed** (`off*mul+2 <= notify_len` → `NotifyRegionTooSmall`). F3 `TPciInfo` `offset_of!` asserts completed. F4 `pci.walk_caps_hostile` test added (cap-loop / OOB-bar / unassigned-bar / oversized-region / valid-control over a synthetic config). + the `KObj_PCI` reference doc (`115-pci-claim.md`) + the `37` cross-reference. | 892/892 (+`pci.walk_caps_hostile`) + both NIC probes PASS 24/24 + 0 EXTINCTION + boot OK + login E2E + SMP gate (default+UBSan × smp4/smp8 N=10, 0 corruption) |
| `38cc1a7` | **#188**: the kernel virtio-rng boot-pull intermittent (surfaced by the 5e-4 SMP gate; **pre-existing**, proven causally disjoint from 5e). `random_virtio_pull` polled the **async** virtio-rng completion with a fixed *iteration* budget (`1<<22`); under HVF the boot vCPU spins natively (a normal success polls 0–82k iters, sub-ms) while QEMU delivers entropy via a bottom-half on a **separate host thread**, so under smp8 contention the fast native spin occasionally exhausted the budget before the completion landed → `g_rng_seeded` stayed false → whole-CSPRNG fail-closed → 6 random tests + `EXTINCTION`. **Confirmed by deterministic fault-injection** (shrinking the cap reproduced the exact evidence) + elimination. Fix: **wall-clock poll deadline** (`RNG_VIRTIO_POLL_MS=250ms` via CNTPCT/CNTFRQ; unconditional `1<<30` iteration backstop) + **bounded retry** (boot 3×, threshold 1×) + a precise per-site boot diagnostic (retires the lying "no RNG device" message). Focused Opus-4.8 audit + concurrent self-audit **CLEAN 0/0/1/3** (F1 frozen-counter non-termination = independent convergence, fixed; F2/F3/F4 = comment/doc, fixed). | 922/922 (+`kern_random.virtio_deadline_ticks`) + 40/40 smp8 HVF boots reseed OK (`any_188_signal=0`) + 0 EXTINCTION + boot OK + SMP gate (default+UBSan × smp4/smp8 N=10, 0 corruption) |
| `*(6a, pending)*` | **6a (#159)**: the **per-`(bus,dev,fn)` PCI allowance axis** — the I-34 fourth axis replacing the fail-closed `SYS_PCI_CLAIM` reject. `struct Allowance` gains `pci[ALLOWANCE_PCI_MAX]` + `pci_count` (each a `PCI_BDF_PACK(bus,dev,fn)`); `HW_RES_PCI` is the `allowance_permits` kind; `kobj_pci_resolve_bdf` (new, `pci_handle.{c,h}`) resolves `virtio_device_id → (bus,dev,fn)` read-only (the same first match the claim picks). `sys_pci_claim_handler` runs resolve → CreateBegin `allowance_permits(HW_RES_PCI)` → `kobj_pci_claim` → CreateCommit `allowance_handle_alloc` (re-check `revoked`); a broad Proc (allowance==NULL) passes (netdev-pci-test unaffected). ABI `t_allowance_desc` 176→216 (append `pci_count`+`pci[8]`+`_pad_pci`, prior offsets pinned; kernel/libt/libthyla-rs mirrors + `TAllowanceDesc::push_pci`). `proc_confer_allowance`/`allowance_confer_within_parent`/`allowance_clone_into` gain the PCI axis. NO spec change (abstract model covers a 4th opaque kind; the 4 buggy cfgs re-run green). Focused Opus-4.8 audit (MODEL start==end, no fallback) + concurrent self-audit **CLEAN 0/0/0/2, NOT dirty** — converged on the whole SOUND set (gate completeness, resolve-vs-claim consistency, packing injectivity, device-enable/rollback, ABI-append safety, monotonic reduction, broad-path); both P3 **fixed**: F1 (stale `allowance.c` doc-comment on `allowance_is_narrowed` still naming SYS_PCI_CLAIM — rewritten to the "drivers are leaves" gate) + F2 (no end-to-end handler witness — added `allowance.pci_claim_handler_gate`). | 923/923 (+`allowance.pci_membership` [the live rng-pci resolve leg] + `allowance.pci_claim_handler_gate` [the live narrowed-denied/allowed handler path] + the PCI legs in confer_within_parent/clone_inherit) + spec gate (clean + 4 buggy cfgs green) + boot OK + login E2E (broad netdev-pci path) + SMP gate (default+UBSan × smp4/smp8 N=10, 0 corruption) |

## Remaining work

### virtio-PCI transport sub-arc (pci-0..pci-3) — the #140 resolution

net-1 surfaced **#140**: QEMU packs 8 virtio-mmio slots per 4 KiB page, so net
(slot 30) + the Stratum-pool blk (slot 31) share page `0x0a003000`; the
page-exclusive `KObj_MMIO` claim over a hard 4 KiB MMU granule cannot give two
*persistent* userspace drivers (`netd` + `stratumd`) sound, isolated
co-residency (net-1 dodged it by running + exiting pre-stratumd). Per the
depth-first-dependencies principle (user-voted 2026-06-15), net-2 is preempted
to build the future-proof virtio-PCI transport (`docs/VIRTIO-PCI-DESIGN.md`):
net moves to its own page-aligned PCI BAR, dissolving the contention by
construction. net-only scope; INTx interrupts; blk→PCI + MSI-X are v1.x seams.

- **pci-0** the transport scripture (this doc + ARCH §13.2/§25.4/I-5 +
  NET-DESIGN §17 + ROADMAP). **LANDED.**
- **pci-1a** DTB/config primitives: `dtb_pci_intx_route` + `dtb_pci_mem_window`
  + `dtb_get_compat_prop` + `virtio_pci_cfg_write*`. **LANDED** (883/883;
  SMP-inert).
- **pci-1b** kernel `KObj_PCI` (exclusive per-function claim, non-transferable)
  + BAR assignment + `VIRTIO_PCI_CAP_*` resolve + the DTB `interrupt-map` INTx
  swizzle. **Audit-bearing** (the claim table is the first shared state).
  **LANDED** (kobj-only; the 3 syscalls split out to pci-1c). 889/889 + boot OK.
- **pci-1c** the 3 syscalls: `SYS_PCI_CLAIM=76` / `MAP_BAR=77` / `INFO=78` +
  `struct t_pci_info` ABI + libt/libthyla-rs wrappers + syscall tests.
  **LANDED** (891/891; the syscall envelope over the pci-1b kobj). The success
  copy-out (SYS_PCI_INFO) + the BAR burrow-map (SYS_PCI_MAP_BAR) need a user
  buffer + address space, so they are proven by the pci-2 userspace probe; the
  in-kernel tests cover the cap gate / mint / rights / reject paths.
- **pci-2** userspace: `libthyla-rs::hardware::PciDev` + virtio-pci-modern +
  port netdev's transport half (reuse `ring`); `netdev-pci-test` over PCI;
  `run-vm.sh` adds `virtio-net-pci`. **LANDED** (`VirtioNetPci` PASS 24/24
  alongside the kept MMIO `netdev-test`; host ring 7/7 + 891/891 + boot OK).
- **pci-3** focused audit + SMP gate + the kernel reference docs. **LANDED**
  (CLEAN 0/0/0/4; Opus 4.8 max + self-audit). The §7 surfaces all verified sound
  (KObj_PCI lifecycle / I-5 / exclusive-claim / BAR-assign / config-mediation /
  cap-walk / INTx); the 4 P3s: F1 partial-map leak tracked (no v1.0 detach path —
  proc-exit-bounded by design), F2 notify-doorbell bound fixed, F3 ABI asserts
  completed, F4 `pci.walk_caps_hostile` test added. New `115-pci-claim.md` +
  `37-virtio_pci.md` cross-reference. **The pci sub-arc is COMPLETE.**

### the Menagerie build arc (preempts net-2; authoritative status: `docs/MENAGERIE.md` §18)

After pci-3, the user voted (2026-06-15) to **preempt net-2 with the Menagerie
driver framework** (`docs/MENAGERIE.md`), so `netd` is born discovery-driven +
allowance-narrowed rather than holding coarse `CAP_HW_CREATE` + a hardcoded base
to be retrofitted later. The full per-chunk status (SHAs, tests, audits) lives in
**MENAGERIE.md §18**; the compact ledger:

- **devhw-1/2** (`4d3950d`/`85921fe`) — the DTB tree-walk publish Dev + `/hw`
  mounted in the boot namespace. **LANDED** (902/902; ref `116-devhw.md`).
- **the hardware allowance / I-34** (`1602e37` spec + `ad814b0` impl + `30f062c`
  audit) — per-Proc scoping of `CAP_HW_CREATE` at the 3 create gates; **model-first**
  (`specs/allowance.tla` clean + 4 buggy cfgs); audit CLEAN 0/1/1/2. **LANDED**
  (913/913; ref `117-allowance.md`).
- **the Loom device-gone terminal CQE** (`6db71fa` spec + `ad74e39` impl +
  `d5d52ca` audit) — §10 surprise-removal; `T_E_NODEV` async terminal; audit CLEAN
  0/0/0/4. **LANDED** (915/915).
- **5a** confer-at-spawn allowance ABI + #160 revoke-on-terminate fold-in
  (`fea1e7d`) — the I-34 grant path wired to userspace (`Command::allowance` over
  `SYS_SPAWN_FULL_ARGV`); audit CLEAN 0/1/1/2 (the #160 fold-in SMP-UAF F1 fixed).
  **LANDED** (917/917).
- **5b** the `libdriver` framework crate (`usr/lib/libdriver`) — the §6 manifest
  schema + parser, the `resolve` node-INTERSECT-needs grant (the auditable I-34
  property, host-tested), the single-argv descriptor codec, and the `Driver` trait
  + `run` scaffold + handle-mint helpers + `to_allowance`. Pure userspace, no kernel
  ABI; audit-light (the kernel validates every `SYS_*_CREATE`). **LANDED** (19 host
  tests + device build + whole-workspace build + clippy-clean; ref `118-libdriver.md`).
- **5c** the warden (`usr/warden`) + the first live `impl Driver`
  (`usr/menagerie-probe`) + `libdriver::dtb` (the `/hw` property decode, +8 host
  tests → 27). The full bind-loop — discover `/hw` (45 nodes) → match the pl061
  GPIO bind DB → `resolve` the grant → spawn `/menagerie-probe` with the
  descriptor + narrowed allowance + `CAP_HW_CREATE` → the driver maps its granted
  MMIO (allowance permits) + an out-of-grant create is rejected (I-34 enforced) →
  reap → `joey: warden ok`. Pure userspace, no kernel ABI; audit-light. **LANDED**
  (27 host tests + boot-probe E2E `1 bound, 1 up` + 0 EXTINCTION + the SMP gate;
  refs `119-warden.md` (new) + `118-libdriver.md`). Bring-up find: a boot-probe
  Proc has no stdio fds → the warden gives each driver `/dev/null` for the 3 slots
  (Command's default `Stdio::Inherit` bumps the parent's fd 0/1/2, which fails
  pre-resolution).
- **5d** the **discovery-source layer** — user-directed (2026-06-16) "pull it
  forward without hesitation": the MENAGERIE §3 proper architecture, where the
  warden binds by typed IDENTITY and **never reads a device register**; a sandboxed
  bus-enumerator Proc does the DeviceID-poke (the type-blind DTB cannot tell which
  of ~32 identical `virtio,mmio` slots holds the NIC). Kernel BYTE-UNCHANGED.
  **5d-0** (`7d9d6f5`) scripture resequence (no code). **5d-1** (`8468c24`)
  `libdriver::source` — typed `DeviceId`/`DeviceNode` + the source→warden
  node-record codec (strict + bounded = the trust boundary) +
  `DiscoverySource`/`best_match` + `DtbSource`. **5d-2** (`4b3bd07`+`8b7c686`)
  `usr/virtio-mmio-source` — a separate Proc granted ONLY the bank; reads each
  slot's DeviceID, pipes typed `virtio:<id>` records to the warden. **5d-3**
  (`1264673`) netdev → grant-driven `open_slot` (retires `virtio.rs:51` + the
  bank-probe) + `usr/netdev-driver` (`impl Driver`, the 24-ARP proof) bound by
  `virtio:1`; retired `netdev-test`. Self-found at 5d-3: a sub-page slot grant is
  unmappable (MMIO is page-granular) → `to_allowance` page-rounds each window.
  **5d-4** (`1535ad8`) the **composed focused audit close** (Opus-4.8-max prosecutor
  + concurrent self-audit, independently converged): **0 P0 / 1 P1 / 2 P2 / 2 P3,
  all fixed/dispositioned**. F1 [P1] the warden conferred a source's reported
  reg/INTID unchecked against the granted bank (a non-TCB source could inflate a
  peer driver's allowance) → FIXED by `reconcile_reported_node` (the source supplies
  IDENTITY, the warden supplies RESOURCES from its own trusted DTB view — enforced,
  not trusted); F2/F3 [P2] unbounded id-list + `read_to_end` DoS → bounded
  (`MAX_IDS` + `slurp_capped`); F5 [P3] source slot-read extent gate; F4 [P3] the
  page-round co-residency over-grant is the documented #140/net-2 I-34-deviation (no
  code). 41 libdriver host tests + boot `2 bound, 2 up` + `netdev-driver: PASS
  24/24` + 0 EXTINCTION + the SMP gate. **LANDED** (`memory/audit_5d_closed_list.md`;
  refs `118-libdriver.md` + `119-warden.md`).
- **5e** `DeviceRemoved` revoke+terminate + supervision — the driver-lifecycle
  half of the Menagerie spine. **5e-1** (`8874ec5`) **long-lived serve +
  DeviceRemoved teardown**: `netdev-driver` goes long-lived (24-ARP proof →
  `READY` → quiesce → block in `Irq::wait` until removed, never self-exiting);
  the warden's `bind_and_run` reads a readiness line via `await_readiness` (a
  `try_wait` + bounded poll — a libdriver driver's pipe does NOT EOF on exit
  because a single-thread Proc's `SYS_EXIT_GROUP` defers the handle-close to
  reap, the #926 asymmetry), then group-terminates a long-lived service via
  `Child::kill` → `/proc/<pid>/ctl` `killgrp` (revoke FIRST, atomic #160). New:
  `libthyla-rs Child::kill`; `VirtioNet::quiesce` made `pub` (explicit
  quiesce-before-block keeps the forced teardown DMA-safe). Boot: `netdev-driver`
  up → `PASS 24/24` → `serving` → warden `DeviceRemoved` → `torn down` → `2
  bound, 2 up` → boot OK + 0 EXTINCTION; stratumd claims the freed (page-shared)
  virtio-blk slot post-pivot. Device build + clippy clean + the SMP gate. The
  live-DMA-at-forced-teardown is the documented MENAGERIE section-10 hazard (an
  IOMMU / cooperative-quiesce fix owed net-2/real-hw). Ref `119-warden.md`.
  **LANDED**. **5e-2** (`ac400df`) **bounded restart-on-crash supervision**:
  the warden's `bind_and_run` is refactored into `run_once` (confer + one attempt
  → a `RunOutcome`) + `supervise` (the restart loop) driving the new PURE,
  host-tested `libdriver::supervise::next_step` (restart-vs-settle per the
  manifest `restart` policy, exponential back-off 50/100/200ms capped at 500,
  give-up after `RESTART_LIMIT`=3). Accounting splits three ways: **Up** /
  **GaveUp** (crashed + restarts exhausted — SOFT, does NOT fail the boot) /
  **Failed** (structural — HARD, exit 1). Proven by a new `crash-probe` driver
  (binds the undriven `virtio:16` GPU id; `probe` always `Err`s BEFORE any HW
  claim → its page-rounded allowance is never a live claim, benign vs netdev's
  shared-page bind). Boot: `crash-probe` restarts 3× (50/100/200ms) → gives up →
  `3 bound, 2 up, 1 gave up, 0 failed` → netdev still up → boot OK + 0 EXTINCTION.
  10 `supervise` host tests + device/clippy clean + the SMP gate. v1.0 SEAM: the
  kernel collapses non-"ok" exits to 1 (`sys_exits_handler`), so the supervisor
  reads clean-vs-crashed only, not specific exit codes (the finer policy lands
  with the structured 64-bit exit_status, docs/ERRORS.md). Refs `118`/`119`.
  **LANDED**. **5e-3** (`0d486c2`) **device-gone `-ENODEV` CQE proven over the
  production SrvConn transport**: a kernel test
  (`9p_srvconn_transport.devgone_posts_nodev_cqe`) stands up a real byte-mode
  `SrvConn` pair, handshakes a 9P client over the production
  `p9_srvconn_transport`, leaves a `Tfsync` in flight, then calls the actual
  `srvconn_teardown` (the path a `DeviceRemoved` group-terminate drives) and
  pumps the reader — proving the real `SrvConn` returns recv-0 on teardown (not
  recv-`-1`), so the device-gone reason reaches a real Loom CQ as `-ENODEV`. Its
  companion `transport_err_posts_eio_cqe` arms a past recv deadline on the same
  setup so the recv returns `-1` → `-EIO`, pinning the distinction over the wire
  a driver's Loom rides (neither passes unless the classifier discriminates).
  Step-4 (#162-165) landed the production path + the loopback proof (`force_eof`
  *synthesizes* the recv-0); 5e-3 closes the gap end-to-end over the production
  transport with no production code change (`canonical_responder` is shared —
  un-static'd — to pre-stage the handshake replies). Tests 918 → 920/920; boot
  OK + 0 EXTINCTION; the SMP gate. Ref `107-loom.md` (device-gone). **LANDED**.
  **5e-4** (`367c2c8`) **composed focused audit over the whole 5e arc + close**:
  one Opus-4.8-max prosecutor (MODEL start==end, no fallback) + a concurrent
  self-audit, **0 P0 / 2 P1 / 0 P2 / 1 P3, all fixed; NOT a dirty close**.
  **F1 [P1]** (both passes, independent) — `read_ready_line` read the driver
  readiness pipe ONE BYTE AT A TIME with *blocking* reads, so a non-TCB driver
  that wrote a partial line (`"READ"`, no newline) and held would stall the
  warden FOREVER, escaping the give-up budget — a boot-availability DoS on the
  TCB warden (the untrusted-driver model the framework exists to sandbox). Fixed:
  the line assembly moved to a pure host-tested `libdriver::readyline::feed_ready_line`
  (accumulator + scan, capped at `READY_LINE_MAX`); the warden does ONE bounded
  read per poll-readable event (returns the available bytes without blocking),
  feeding chunks across the poll loop. **F2 [P1]** (prosecutor) — DeviceRemoved
  did not fully revoke a driver that spawned an allowance-bearing **child Proc**:
  `proc_group_terminate` is thread-group-scoped, so a grandchild inheriting a
  clone of the narrowed allowance (`revoked==0`) survives reparented to init with
  live MMIO/IRQ/DMA the warden never tracks (I-34 "fully revoked" violated under
  an untrusted *spawning* driver). The fork was already **resolved in scripture**
  (MENAGERIE §13.2 "drivers are sources, not spawners; one auditable chokepoint"),
  so the fix *implements* it fail-closed: `rfork_internal` denies a child Proc to
  a hardware-allowance-narrowed parent (closes both the confer + clone-inherit
  paths; does not block step-6 recursive sources, which *report* to the warden).
  Regression `allowance.narrowed_proc_cannot_spawn`. **F3 [P3]** — documented the
  readiness contract (a driver's first stdout line MUST be exactly "READY";
  diagnostics → the console). libdriver host tests 51 → **61** (+10 readyline);
  kernel suite 920 → **921**; boot OK (`warden: 3 bound, 2 up, 1 gave up, 0
  failed`) + 0 EXTINCTION; SMP gate 0 corruption. Refs `117-allowance.md`
  ("Drivers are leaves"), `118-libdriver.md` (readyline), `119-warden.md` (F1+F3),
  `memory/audit_5e_closed_list.md`. **LANDED**. THE 5e ARC IS COMPLETE. → step 6
  (PCIe/USB sources + the per-(bus,dev,fn) PCI allowance axis #159).

### the net arc (resumes on the PCI NIC, after the Menagerie spine)

- **net-2** netd skeleton: embed smoltcp, serve `/net`, the `/net/tcp`
  clone/connect/data client path + the fid state machine (NET-DESIGN §3.4).
- **net-3** listen/accept + udp + icmp (ping).
- **net-4** cs/dns/ndb + ipconfig/DHCP. **net-5** socket-compat pouch patch.
- **net-6** dev9p.poll + reserved `net_poll.tla` + Loom-multishot accept.
- **net-7** TLS root bundle + SNTP + `SYS_CLOCK_SETTIME` + observability.
- **net-8** server + soak exit criteria + one focused audit over the arc.

## Ground truth (the DTB + the live device, read 2026-06-15)

The DTB dump (`qemu-system-aarch64 -machine virt,... ,dumpdtb` + `dtc`) of the
`pcie` node, reconciled with the **actual** in-boot enumeration (the kobj is
DTB-derived at runtime, so it is config-agnostic):

- **ECAM** PA varies by machine: `0x3f00_0000` (16 MiB) on the **default
  `gic-version=2`** machine that `tools/test.sh` boots; `0x40_1000_0000`
  (256 MiB, highmem) on the `gic-version=3` variant my pci-1a manual dump used.
  The kobj never hardcodes it — `kernel/virtio_pci.c` maps it from the DTB.
- **BAR window** PA `0x1000_0000`, ~768 MiB (32-bit, page-aligned, not reserved
  → a per-device BAR is page-aligned + userspace-claimable). Stable across both
  machines (`dtb.pci_mem_window` asserts the base).
- **INTx** swizzle `SPI = 3 + ((slot + pin − 1) mod 4)` → GIC INTID **35–38**.
  A single PCI device (net-only) lands on **one unshared INTID** → no irqfwd
  multi-waiter needed. The live rng-pci at bdf 0:1.0 routes to INTID 36.
- **MSI** present in HW but **undriven** → INTx is the v1.0 path; MSI-X v1.x.
- **BARs start unassigned** (no UEFI) → the kernel assigns them from the BAR
  window (pci-1b).
- **The live test device (rng-pci) is TRANSITIONAL**: it presents the *legacy*
  device_id `0x1005` (`is_modern=0`) but carries the *full* modern interface —
  a 32-bit legacy MMIO BAR1 (4 KiB) **and** a 64-bit modern BAR4 (16 KiB) with
  all four `VIRTIO_PCI_CAP_*` structures (common/notify/isr/device) packed into
  BAR4. So `pci.claim_rng` exercises BOTH BAR widths + the full cap-walk. **For
  pci-2/net-2**: prefer `virtio-net-pci,disable-legacy=on` for a clean
  modern-only NIC (device_id `0x1041`, no I/O BAR) — the kobj handles either.

## Exit criteria (refined per W4-F8; see NET-DESIGN §16)

- [ ] Client: `ping`, `curl`/`wget` (TLS), `ssh`-client, a native `TcpStream`.
- [ ] Server: native `TcpListener` echo (≥2 conns) + the Loom-multishot accept
      loop + a ported `listen`/`accept` server.
- [ ] Soak: N conns × M s, no fd/connection/Burrow leak, under the SMP gate.
- [ ] No Utopia regressions; no P0/P1 on the reserved net audit surfaces.

## Build + verify

```bash
tools/build.sh kernel              # kernel + userspace + ramfs
tools/test.sh                      # boot + netdev-test PASS + boot OK
cargo test -p netdev --no-default-features --target aarch64-apple-darwin   # host ring tests
tools/ci-smp-gate.sh               # SMP soundness gate
```

## Trip hazards

- A virtio driver Proc needs `CAP_HW_CREATE` (spawn via `t_spawn_with_caps` /
  `rfork_with_caps`); plain `t_spawn` → `SYS_MMIO_CREATE` rejected at the cap
  gate (net-1's root-cause).
- netdev-test runs PRE-stratumd in the joey ladder (the net page must be free).
- `THYLACINE_NO_QMP=1` makes the virtio-input probe fail (#34, unrelated).

## References

- `docs/NET-DESIGN.md` (the #68 charter), `docs/reference/114-netdev.md`.
- ARCH §10.1 (network is 9P), §28 (no new net invariant), ROADMAP §9.
