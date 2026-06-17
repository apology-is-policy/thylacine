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
**The virtio-PCI transport sub-arc is COMPLETE.** **net-2a is LANDED** — `netd`
embeds smoltcp on the PCI NIC and acquires a DHCP lease (`10.0.2.15/24` from
slirp), proving the whole lower stack (Ethernet/ARP/UDP/DHCP). **net-2b-1 is
LANDED** — netd is now a **persistent** service: a libdriver `Lifecycle` manifest
field + the warden's leave-running-on-`READY` policy keep netd resident (holding
the link) instead of reaping the one-shot, while the `netdev-driver` MMIO demo's
`DeviceRemoved` teardown is preserved. **net-2b-2 is LANDED** — the 9P `/net`
server: netd posts `/srv/net` and serves the §3.1 directory skeleton
(`tcp/udp/icmp` + a read-only `stats` file) over a combined accept/stack event
loop; joey mounts it at `/net`, and the boot probe walks+reads `/net/tcp/stats`
(43 bytes). Posting requires `MAY_POST_SERVICE`, conferred joey→warden→netd
(gated on the persistent lifecycle). **net-2c-1 is LANDED** — the `/net/tcp`
`clone` fid state machine (§3.4): a qid-encoded dynamic tree, the
clone-mints-`N` Plan 9 idiom (the kernel dev9p client accepts the rebound-fid
`Rlopen` qid), refcounted connection slots (the *last* clunk frees `N` — the
only free path), and the `libthyla_rs::ninep` `Treaddir` codec (`/net/tcp` lists
its live `N/` directories). **net-2c-2 is LANDED** — the live TCP data path: the
`socket-tcp` feature, the `Net` table owns the iface + socket set (post-DHCP), a
`clone` reserves a real `tcp::Socket` (freed at the last clunk), the `ctl` verb
parser drives `connect a.b.c.d!port`/`hangup` (`announce`/options →
`EOPNOTSUPP`), `status`/`local`/`remote` report the live socket, and `data` is
`recv_slice`/`send_slice` (non-blocking). Boot proof (deterministic,
peer-independent): `connect 10.0.2.2!9` → `remote 10.0.2.2!9` + `local
10.0.2.15!…` + the multi-fid clunk frees + reuses `N`. The NIC-IRQ poll fd is
deferred (a pollable IRQ fd is a kernel ABI surface). **net-2d is LANDED** — the
focused netd audit (Opus-4.8-max prosecutor + a concurrent self-audit) closed
**CLEAN 0 P0 / 0 P1 / 1 P2 / 4 P3, NOT dirty** (F1 [P2] the readdir budget missed
the 11-byte Rreaddir frame overhead → a `rreaddir_budget` parity helper; F2/F3
[P3] the `P9_NOFID`-as-live-fid + the failed-connect port-burn / rolled-back-mint
stat → fail-closed + peek-then-commit; F4/F5 closed-justified; SMP gate 40/40
clean, 0 corruption). **net-2 is DONE.** **net-3a is LANDED** — the TCP server
side: `announce` + the blocking `listen`/accept via a **deferred 9P reply** (netd
holds the Rlopen until an inbound call lands, since a single-threaded server
cannot block in the handler; the socket-swap mints the accepted connection + re-
arms the listener) + the **Tflush/Rflush** cancellation path (also closing a
pre-existing net-2c-2 outstanding-tag leak). The blocking-open question is
**resolved pure-userspace, no ABI** (committed-blocking via the existing dev9p
client; §12's readiness multiplexing is the separate net-6 leg). Boot-proven
deterministically (`announce → Listen` + the gate); the inbound-accept E2E is owed
to net-3d. net-3b (UDP), net-3c (ICMP ping), net-3d (audit + the accept E2E), then
net-4..net-8 (cs/dns, socket-compat, dev9p.poll, TLS/NTP, exit criteria) follow.

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
| `3dbf929` | **6a (#159)**: the **per-`(bus,dev,fn)` PCI allowance axis** — the I-34 fourth axis replacing the fail-closed `SYS_PCI_CLAIM` reject. `struct Allowance` gains `pci[ALLOWANCE_PCI_MAX]` + `pci_count` (each a `PCI_BDF_PACK(bus,dev,fn)`); `HW_RES_PCI` is the `allowance_permits` kind; `kobj_pci_resolve_bdf` (new, `pci_handle.{c,h}`) resolves `virtio_device_id → (bus,dev,fn)` read-only (the same first match the claim picks). `sys_pci_claim_handler` runs resolve → CreateBegin `allowance_permits(HW_RES_PCI)` → `kobj_pci_claim` → CreateCommit `allowance_handle_alloc` (re-check `revoked`); a broad Proc (allowance==NULL) passes (netdev-pci-test unaffected). ABI `t_allowance_desc` 176→216 (append `pci_count`+`pci[8]`+`_pad_pci`, prior offsets pinned; kernel/libt/libthyla-rs mirrors + `TAllowanceDesc::push_pci`). `proc_confer_allowance`/`allowance_confer_within_parent`/`allowance_clone_into` gain the PCI axis. NO spec change (abstract model covers a 4th opaque kind; the 4 buggy cfgs re-run green). Focused Opus-4.8 audit (MODEL start==end, no fallback) + concurrent self-audit **CLEAN 0/0/0/2, NOT dirty** — converged on the whole SOUND set (gate completeness, resolve-vs-claim consistency, packing injectivity, device-enable/rollback, ABI-append safety, monotonic reduction, broad-path); both P3 **fixed**: F1 (stale `allowance.c` doc-comment on `allowance_is_narrowed` still naming SYS_PCI_CLAIM — rewritten to the "drivers are leaves" gate) + F2 (no end-to-end handler witness — added `allowance.pci_claim_handler_gate`). | 923/923 (+`allowance.pci_membership` [the live rng-pci resolve leg] + `allowance.pci_claim_handler_gate` [the live narrowed-denied/allowed handler path] + the PCI legs in confer_within_parent/clone_inherit) + spec gate (clean + 4 buggy cfgs green) + boot OK + login E2E (broad netdev-pci path) + SMP gate (default+UBSan × smp4/smp8 N=10, 0 corruption) |
| `f0878ba` | **6b scripture (no code)**: the **devpci-mediated PCIe discovery source** design fork resolved (user-voted the devpci Dev over a `SYS_PCI_ENUM` syscall / extending devhw). ECAM is I-5-reserved + no userspace PCI-enum → a config-space-scanning source is impossible; Plan 9 (devpci = `/dev/pci/B.D.F/ctl`) + Fuchsia/Genode/seL4 converge on kernel-mediated topology; the kernel already enumerated (`g_virtio_pci_devs[]`) → the source is the `DtbSource` analog (in-process, no Proc, no reconcile). MENAGERIE §7 (the two PCIe-source realizations — mediated vs self-scanning brcmstb) + §16-6b; ARCH §9.4 (devpci as-built + the /hw mount drift reconciled). | scripture only |
| `cffe5ea` | **6b-1: the devpci kernel Dev** — NEW `kernel/devpci.c` (`dc='P'` at /hw/pci; the devdev/devctl read-only dir-Dev: root → `<bus.dev.fn>` dir → `ctl` file over `g_virtio_pci_devs[]`; **NO raw ECAM / no config-space write** — the pci-3 I-5 property; the ctl line `v1 bus= dev= fn= vendor= device= virtio= intid=` the PciSource will parse) + the devhw synthetic `pci` mount-point child (`HW_SYNTH_PCI` bit-62 qid) + the `/hw/pci` mount in joey (after /hw) + register/extern/build wiring. `PGRP_MAX_MOUNTS` 12→16: the new boot mount pushed the joey namespace past the cap (the documented #80 orphan-accumulation), surfaced as a `stalk-2 e2e t_mount` extinction — root-caused to ground (`territory.c:749` cap reject) + fixed, not waved off ([[feedback-no-host-load]]). Reference `docs/reference/120-devpci.md` + ARCH §9.4/§25.4 + CLAUDE.md audit row. The PciSource + netdev-pci-driver bind (the live I-34-on-PCI proof) = 6b-2/6b-3; the focused audit + SMP gate = 6b-4. | **930/930** (+ 6 `devpci.*` + `devhw.synth_pci_child`) + boot OK + /hw/pci mounted + stalk-2 e2e OK + full login E2E + 0 EXTINCTION |
| `b9f356c` | **6b-2: the libdriver PCI axis + the in-process PciSource** (pure userspace, no kernel change) — a NEW `DeviceId::VirtioPci(u16)` (string `"virtio-pci:<n>"`, deliberately DISTINCT from the MMIO `Virtio` so the two transports never collide), a `PciNeed::{None,Node}` manifest axis (`pci = "node"`), `NodeResources`/`BoundResources` gain `pci: Option<(u8,u8,u8)>` (the bdf) carried through `resolve` (the I-34 grant property: granted bdf == node's, never fabricated; no MMIO — BARs are handle-gated) + the descriptor codec (`;pci=<b>.<d>.<f>`) + `to_allowance` (`push_pci`, the 6a kernel gate), the pure `parse_pci_ctl` devpci-ctl-line parser (producer/consumer format ground-truthed vs `kernel/devpci.c::build_ctl`), and the `driver`-gated in-process `PciSource` over `/hw/pci` (the DtbSource sibling — TCB-mediated, no reconcile). Self-audit found+fixed the `to_record` silent-bdf-drop seam (fail-closed loud reject — the pipe codec cannot carry a bdf; the in-process source never pipes; a future out-of-process PCIe source extends the record first). Reference `docs/reference/118-libdriver.md` (status → 6b-2). The warden `PciSource` enumeration + the `netdev-pci-driver` bind = 6b-3; the focused audit + the single SMP gate (6b-1 is the arc's only kernel surface) = 6b-4. | **80/80 libdriver host tests** (+ manifest pci-need, + 8 resource pci, + 9 source pci/ctl-parser) + the device build (`tools/build.sh userspace`) compiles clean (libdriver/warden/virtio-mmio-source/netdev-driver) |
| `f759ff0` | **6b-3: the warden PciSource enumeration + `netdev-pci-driver` narrowed — the live I-34-on-PCI proof** (userspace only; 6b-1 remains the arc's sole kernel surface). The warden enumerates `PciSource::new().enumerate()` over `/hw/pci` **in-process** (the DtbSource analog) and — crucially — with **no `reconcile_reported_node`**: the kernel-mediated `/hw/pci` view is trusted by construction, unlike the non-TCB virtio-mmio bus-source Proc. The bind DB gains a `netdev-pci-driver` manifest binding `"virtio-pci:1"` `needs{ pci="node" irq="node:interrupts" dma="pool: 64 KiB" }` (**no `mmio`** — a PCI function's registers are in BARs, mapped off the claimed `KObj_PCI`, not an MMIO allowance window). NEW `usr/netdev-pci-driver` (`impl Driver`, the `netdev-driver` analog over `VirtioNetPci`): `probe` verifies the conferred identity is `virtio-pci:1` (fail-closed on a mis-bind), claims the net function (`SYS_PCI_CLAIM` by virtio-id; the kernel resolves it to the bdf + gates against the conferred PCI axis), maps BARs, inits; `serve` runs the 24-ARP round-trip, then holds long-lived (quiesce-before-`READY`) until `DeviceRemoved`. `VirtioNetPci::quiesce` made `pub` (the forced-teardown-skips-`Drop` discipline, the 5e-1 move for the PCI sibling). **Retires `usr/netdev-pci-test`** (the warden-bound narrowed driver replaces the standalone broad-cap probe; removed from joey's ladder + workspace + build.sh). Self-audit fixed a `doc_lazy_continuation` in the 6b-2 `parse_bdf` doc. The focused audit + the single SMP gate over the complete devpci + netdev-pci surface = 6b-4. | **930/930** + **the live I-34-on-PCI proof**: `warden: /hw/pci discovered 3 PCI function(s)` → `discovered virtio-pci:1 (0.1.0)` → `bind virtio-pci:1 -> netdev-pci-driver [mmio=0 irq=1 dma=0x10000 pci=Some((0,1,0))]` → `PASS -- 24/24 ARP replies via VirtioNetPci (grant-narrowed PCI claim)` → `up (READY) -> DeviceRemoved` → `torn down`; the tally `4 bound, 3 up, 1 gave up, 0 failed`; `Thylacine boot OK`; 0 EXTINCTION; test.sh exit=0; 80/80 libdriver host + device build + clippy clean |
| `5e12d0e` | **6b-4: the focused audit + SMP gate + close — the 6b arc COMPLETE**. One Opus-4.8-max prosecutor (the holotype-reviewer pins Fable, access-restricted → Opus per the reviewer-model record; `MODEL(start)==MODEL(end)`, no fallback) + a concurrent self-audit over the WHOLE arc (devpci + the libdriver PCI axis + the warden `PciSource` + `netdev-pci-driver`). **CLEAN 0 P0 / 0 P1 / 0 P2 / 4 P3 (3 agent + 1 self), NOT dirty** — converged on the SOUND set (I-5 read-only complete [no ECAM / no config-write], the **no-reconcile soundness** [in-process TCB read of a kernel-published write-less tree + the kernel re-resolves the bdf at claim — strictly stronger than the virtio-mmio reconcile path], qid bounds + the `virtio_pci_dev_get` negative-safe forged-qid backstop, the driver fail-closed + quiesce-before-teardown, no info leak, the dual-same-virtio-id fail-closed). P3s: **F1** (`/hw` readdir omits the synthetic `pci` mount point — cosmetic, no v1.0 consumer; document-and-track → task #191, the audited readdir's cookie scheme is not worth churning); **F2** (`parse_pci_ctl` pub+lenient — strengthened the TCB-only CONTRACT caveat); **F3** (`to_record` PCI-reject lacked a dedicated live-producer test — transitively covered; added a witness note); **F4** (self: `devpci_readdir` lacked the `off<0` guard its `devpci_read` sibling has — added the 1-line symmetry; unreachable [`.seekable=false`, cookie ≥1] + `emit_dirent` bounds-safe regardless). Plus a doc-currency sweep for the `netdev-pci-test` retirement (115/117/120). | **930/930** (all 6 `devpci.*` PASS incl. `devpci.readdir` after the off-guard) + boot OK + 0 EXTINCTION + the live I-34-on-PCI proof + test.sh exit=0 + 80/80 libdriver host + clippy clean + **SMP gate PASS (0 corruption, default+UBSan × smp4/smp8 N=10; the 2 "timing" boots ground-truthed guest-clean — 930/930 + the netdev-pci PASS + boot OK + login, a harness post-marker exit race, [[feedback-no-host-load]])**. Closed list `memory/audit_6b_closed_list.md`. |
| `a2a2614` | **net-2a: smoltcp on the NIC** — `usr/netd`, the network daemon. The warden binds `virtio-pci:1 -> netd` **narrowed** (retiring `netdev-pci-driver`, the ARP demo netd subsumes — the NIC handles are non-transferable per I-5, so the claimer runs the stack). netd embeds **smoltcp 0.12** (`no_std`+`alloc`; the libthyla-rs allocator + monotonic clock + CSPRNG) behind a `phy::Device` over `VirtioNetPci` (owned-frame `NicRxToken` / `&mut nic` `NicTxToken` — the no-alias token pattern), and proves the stack end-to-end by acquiring a **DHCP lease** from slirp (Ethernet TX/RX over the BAR-mapped virtqueues → ARP → UDP → the DHCP client). **ONE-SHOT** (exit 0 → warden `Up`), so NO warden-teardown change — net-2b adds persistence. Pure userspace — **kernel byte-unchanged**. Reference `docs/reference/121-netd.md` + the netd audit-trigger row (ARCH §25.4 / CLAUDE.md); the focused audit is net-2d. | **930/930** + `netd: PASS -- smoltcp brought the link up via the PCI NIC (DHCP)` (lease `10.0.2.15/24`, router `10.0.2.2`, dns 1) → `4 bound, 3 up, 1 gave up, 0 failed` + boot OK + 0 EXTINCTION + SMP gate PASS (default+UBSan × smp4/smp8 N=10, 0 corruption) |
| `3bf8978` | **net-2b-1: the persistent netd lifecycle** — netd goes from a one-shot to a resident service. A new libdriver `Lifecycle` manifest field (`persistent` vs default `transient`; on `Manifest` + parser + `to_text` + host tests) + the warden `run_once` READY arm branched on it: `persistent` → **leave the driver running** (drop the `Child` un-waited; `Child::drop` neither reaps nor kills, so netd reparents to the orphan-adopter when the warden exits, its I-34 allowance bound to its own Proc per #160); `transient` (the `netdev-driver` MMIO demo) → the **unchanged** `DeviceRemoved` revoke+terminate. netd's `serve` now signals `READY` after the DHCP lease and stays resident in a `poll_delay`-paced stack loop (clamped `[50 ms, 1 s]` — a floor against the #108 idle-spin class, a ceiling for responsiveness). **No 9P server yet** (`serves = "/net"` declared; net-2b-2 lands the post + mount). Pure userspace — **kernel byte-unchanged**. Reference `docs/reference/121-netd.md` (the net-2b-1 section) + `118-libdriver.md` + MENAGERIE.md §5/§6. | **82/82 libdriver host** (+ `lifecycle_persistent_parses_and_round_trips` + `lifecycle_rejects_bad_value` + the default-`Transient` assert) + **930/930** + the live proof `warden: netd … up (READY) -> serving (persistent; left running)` with the transient `netdev-driver … -> DeviceRemoved` PRESERVED + `4 bound, 3 up, 1 gave up` + boot OK + 0 EXTINCTION + SMP gate (default+UBSan × smp4/smp8 N=10, 0 corruption) |
| `bbed134` | **net-2b-2: the 9P `/net` server + the joey mount** — netd posts `/srv/net` (9P-mode) and serves the NET-DESIGN §3.1 directory skeleton over a combined event loop: NEW `usr/netd/src/server.rs` (the read-only `/net` 9P2000.L server — a static node table `tcp/udp/icmp` + a `stats` file each; `server::Conn` fid table; Tversion/attach/walk/lopen/read/getattr/clunk via `libthyla_rs::ninep`; the load-bearing `Tgetattr` security trio `0555`/`0444` SYSTEM, which the A-3 dev9p X-search reads); `main.rs` (post→READY→the `t_poll([listener]+conns, timeout=poll_delay)` loop folding the 9P accept into the stack poll). **The MAY_POST_SERVICE chain** (joey→warden→netd): joey grants the warden `T_SPAWN_PERM_MAY_POST_SERVICE` (`t_spawn_with_perms`; joey console-attached + holds it), the warden re-confers it gated on `lifecycle == Persistent` (`cmd.perm(...)`), so netd can `devsrv_post_listener` — the #827b one-hop delegation extended one hop, no new ABI. **joey** mounts `/net` post-pivot (open=connect `/srv/net` → dev9p root → `t_mount`, fail-soft if absent) + a gated walk+read probe. Pure userspace — **kernel byte-unchanged**. Reference `docs/reference/121-netd.md` (the net-2b-2 section) + `119-warden.md` + MENAGERIE.md §5 + NET-DESIGN §20; the netd audit-trigger row (ARCH §25.4 / CLAUDE.md) gains the 9P-server + perm surface (audit at net-2d). | **930/930** + `netd: serving /net (9P over /srv/net)` + `warden: netd … up (READY) -> serving (persistent; left running)` + `4 bound, 3 up, 1 gave up` (the transient `netdev-driver … -> DeviceRemoved` PRESERVED) + `joey: net-2b-2 /net mounted` + `PROBE /net/tcp/stats OK (43 bytes)` + boot OK + 0 EXTINCTION + netd clippy/fmt clean + SMP gate (default+UBSan × smp4/smp8 N=10, 0 corruption) |
| `9368634` | **net-2c-1: the `/net/tcp` clone fid state machine + the ninep Treaddir codec** (pure userspace; **kernel byte-unchanged**) — grows the net-2b-2 static skeleton into the live §3.4 TCP fid machine. `libthyla_rs::ninep` gains the readdir codec (`P9_TREADDIR/RREADDIR` + `parse_treaddir` + `build_rreaddir` + `pack_dirent` + `DT_DIR/DT_REG`; the kernel dev9p client issues `Treaddir`, not a legacy `Tread` stream, so this is required to list at all). `server.rs` is reworked: the static `NODES` becomes a **qid-encoded dynamic tree** (static skeleton in `[0,8)`; a live connection `N` = `CONN_FLAG(1<<40)│proto<<32│N<<8│filekind`, resolvable only while the slot is live). Opening `/net/tcp/clone` (`Tlopen`) **mints** `N` and *rebinds the fid onto its `ctl`* (the Plan 9 clone idiom — the kernel dev9p client accepts the differing `Rlopen` qid, verified `9p_session.c`). A connection is **refcounted** by the fids naming its subtree (`fid_set` refs-new-before-unref-old; `fid_clunk`/`teardown`/Tversion unref); the **last** unref frees `N` — the only free path (I-10/I-11), bounded at `MAX_SLOTS=16` (#65 DoS floor). A slot carries **no smoltcp socket yet** ("N assigned"; the live data path is net-2c-2). netd is single-threaded → the global `Net` table needs no lock. The joey probe exercises the whole machine end-to-end through the dev9p session. Reference `docs/reference/121-netd.md` (the net-2c-1 section) + NET-DESIGN §20; the netd audit-trigger row (ARCH §25.4 / CLAUDE.md) gains the fid machine + readdir codec (audit at net-2d). | **930/930** (kernel byte-unchanged) + `joey: net-2c-1 /net mounted` + `net-2c-1 PROBE OK (clone->0, 0/ctl->0, readdir grew, clunk frees+reuses 0)` + boot OK + 0 EXTINCTION + netd clippy/fmt clean + SMP gate (default+UBSan × smp4/smp8 N=10, 0 corruption) |
| `3547635` | **net-2c-2: the live TCP data path** (pure userspace; **kernel byte-unchanged**; only the `socket-tcp` smoltcp feature is new) — makes the net-2c-1 connection LIVE. **The `Net` table owns the stack**: after the DHCP bring-up `serve()` moves the smoltcp `Interface` + `SocketSet` into `Net` (which the 9P dispatch already holds `&mut`), so a `ctl`/`data` handler reaches both as disjoint fields; `device` stays a `serve()` local (`Net::poll(&mut device)`); the loop drives `net.poll` at the top AND after each dispatch batch (a just-enqueued SYN/data egresses that tick). **`clone` reserves a real `tcp::Socket`** (rx/tx 4 KiB; the §3.4 `ALLOCATED` state), removed at the last clunk (`slot_unref` → refs 0 — the sole free path). **The `ctl` verb parser** (§3.3): `connect a.b.c.d!port` → `socket.connect(iface.context(), remote, local)` with a rotating ephemeral local port (smoltcp requires a non-zero one; it auto-selects only the local ADDRESS), recording the resolved `local`/`remote` in the slot synchronously (peer-independent); `hangup` → `close()`; `announce`/`bind`/options → honest `EOPNOTSUPP` (net-3+). **The files**: `status` = live `socket.state()`, `local`/`remote` = recorded `ip!port`, `err` = recorded reason, `data` = `recv_slice`/`send_slice` (non-blocking). The joey probe writes `connect 10.0.2.2!9` on the ctl fid + reads back `remote`/`local` (deterministic, peer-independent) + the multi-fid clunk frees+reuses N. The NIC-IRQ poll fd is DEFERRED (a pollable IRQ fd is a kernel ABI surface; `SYS_IRQ_WAIT` blocks). Reference `docs/reference/121-netd.md` (the net-2c-2 section) + NET-DESIGN §20; the netd audit-trigger row (ARCH §25.4 / CLAUDE.md) gains the live data path + connect/ctl-verb/socket-reservation surface (audit at net-2d). | **930/930** (kernel byte-unchanged) + `joey: net-2c-2 PROBE OK (clone->0, connect 10.0.2.2!9, local 10.0.2.15!, frees+reuses 0)` + `netd: serving /net` + `4 bound, 3 up, 1 gave up` + boot OK + 0 EXTINCTION + netd clippy/fmt clean + SMP gate (default+UBSan × smp4/smp8 N=10, **0 corruption**; all 31 "timing" boots ground-truthed guest-clean at login — the harness post-marker artifact) |
| `aa364f5` | **net-2d: the focused netd audit + close** (the network arc's central audit-bearing surface, NET-DESIGN §15.2; the only code touched is `usr/netd/src/server.rs` — **kernel byte-unchanged**) — an **Opus-4.8-max prosecutor + a concurrent self-audit**, **CLEAN 0 P0 / 0 P1 / 1 P2 / 4 P3, NOT dirty**. The fid-machine refcount, the socket reservation/free balance, the disjoint borrow, the parser bounds, the fail-closed non-live ops, the single-threadedness (no `thread_spawn` → the lockless `Net` is sound), the I-5 probe gate, the MAY_POST_SERVICE persistent gate, malformed-frame safety (the ninep `parse_twalk`/`parse_twrite` bounds), and the Tgetattr trio all held. **F1 [P2]**: `h_readdir`'s budget omitted the 11-byte Rreaddir frame overhead (`P9_HDR_LEN+4`) that `h_read` reserves → a populated dir read by a small-msize client overran its negotiated msize → FIXED via a `rreaddir_budget` parity helper. **F2 [P3]**: `h_attach`/`h_walk` accepted the `P9_NOFID` sentinel as a live fid → fail-closed reject. **F3 [P3]**: a rejected re-`connect` burned an ephemeral port + a rolled-back clone over-counted `opened` → peek-then-commit port + `tcp_clone_rollback`. F4/F5 + the cross-session liveness closed-justified (single-threaded + the qid re-validates `slot_live`). The deterministic small-msize/NOFID/failed-connect regressions are architecturally unreachable in-VM (the only /net client is the trusted large-msize kernel dev9p mount; /net is 9P-mode; netd has no host-test harness) → owed to a netd pure-protocol host-test module (net-3+); the fixes rest on data-path parity (`h_read`) + the ninep `build_rreaddir` guard + fail-closed correctness. `memory/audit_net2d_closed_list.md`. | **930/930** (kernel byte-unchanged) + `joey: net-2c-2 PROBE OK` + `netd: serving /net` + `4 bound, 3 up, 1 gave up` + boot OK + 0 EXTINCTION + netd clippy/fmt clean + **SMP gate PASS (default+UBSan × smp4/smp8 N=10 = 40/40 clean, 0 corruption, 0 timing)** |
| `da7ffc5` | **net-3a: the TCP server side — `announce` + the blocking `listen`/accept via a deferred 9P reply** (pure userspace; **kernel byte-unchanged**; the only shared-crate change is the `ninep` Tflush/Rflush codec). `announce *!port`/`a.b.c.d!port` puts a connection's socket into LISTEN (`status` reads `Listen`). The blocking `open(/net/tcp/N/listen)` is a **deferred 9P reply** — a single-threaded server cannot block in the handler (it must keep polling the NIC to receive the very SYN that would unblock it = self-deadlock), so `h_lopen` registers a `PendingAccept` + returns a `Disp::Deferred` sentinel (no reply emitted; the client's `open()` stays blocked on the outstanding tag — the dev9p client matches by tag, with no per-op deadline, #811-death-interruptible). The serve loop's `poll_accepts` detects the established listener (`accept_ready` = `is_active() && state ≠ SynReceived`), **swaps** (mints `M` taking the established socket + re-arms `N` with a fresh listener — `N` stays ANNOUNCED), and `complete_accept` rebinds the listen fid onto `M/ctl` (refcount moves `N→M`) + sends the held `Rlopen`. **Tflush** cancels a dead deferred accept + replies `Rflush` — also closing a **pre-existing net-2c-2 outstanding-tag leak** (an ignored Tflush left the kernel tag `awaiting_flush`). This is the committed-blocking realization of §3.4; **no kernel surface** (§12's readiness multiplexing = the net-6 leg). Boot proof (deterministic): `announce *!7777 → Listen` + the `listen` file in the readdir + the not-announced gate. The full inbound-accept E2E is owed to **net-3d** (a deterministic in-guest inbound path — a netd loopback interface). Reference `docs/reference/121-netd.md` (the net-3a section) + NET-DESIGN §3.4/§20; the netd audit-trigger row (ARCH §25.4 / CLAUDE.md) gains the deferred-reply + announce/listen + Tflush surface (audit at net-3d). | **930/930** (kernel byte-unchanged) + `joey: net-3a PROBE OK (announce *!7777 -> Listen; listen file + readdir; listen gated on announce)` + `netd: serving /net` + boot OK + 0 EXTINCTION + netd clippy-clean + **SMP gate PASS (default+UBSan × smp4/smp8 N=10, 0 corruption)** |

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

- **net-2 (LANDED, all sub-chunks)** netd: embed smoltcp, serve `/net`, the
  `/net/tcp` clone/connect/data client path + the fid state machine (NET-DESIGN
  §3.4). **net-2a** smoltcp + the `phy::Device` + DHCP-lease proof; **net-2b**
  the persistent 9P `/net` server + the joey mount; **net-2c** the `/net/tcp`
  clone/connect/data fid state machine (live TCP); **net-2d** the focused audit
  (Opus-4.8-max prosecutor + self-audit, CLEAN 0/0/1/4) + the ARCH §25.4 /
  CLAUDE.md enumeration + close. **net-2 is DONE.**
- **net-3** listen/accept + udp + icmp (ping). **The net arc resumes here.**
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
