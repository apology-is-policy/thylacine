# PCI interrupt domains: shared INTx and MSI-X

Status: design and full implementation APPROVED by the operator, 2026-09-17.
Implemented and verified in the aux/Halcyon integration. The ledger below
preserves the implementation sequence and distinguishes measured QEMU coverage
from untested environments. The operator chose this full design over a standalone
INTx control operation after the integration exposed a shared-line storm.
The final verification includes the full 40-boot SMP matrix and an additional
eight-CPU ITS/TCG UBSan boot; see `AUX-HALCYON-INTEGRATION.md`.

## 1. Problem and measured evidence

Before this change, PCI device ordering was an implicit resource allocator.
The raw IRQ path exclusively claimed an INTID for each KObj_IRQ; userspace drivers claim PCI
functions separately. That is correct for exclusive wire interrupts, but cannot
represent two PCI functions wired to one INTx line. Moving QEMU devices around
merely changes which pair fails.

The integration reproduced two interacting defects on QEMU 10.0.2, HVF,
GICv2, four CPUs:

* Nocturne's separate IRQ waiter re-armed a level interrupt before its NORMAL
  cycle thread acknowledged the device. The INTERACTIVE waiter could starve
  its own acknowledger. Clearing the sound ISR in the waiter fixes this ordering.
* Tapestry's polled keyboard never clears configuration notifications and leaves
  PCI interrupt delivery enabled. NO_INTERRUPT suppresses queue notifications;
  it does not disable the PCI function's interrupt output. Keyboard slot 2 and
  audio slot 6 share INTID 37. QMP observed keyboard ISR=3, sound ISR=0. The
  Nocturne IRQ waiter accumulated millions of dispatches while ordinary work
  stalled. A diagnostic read-to-clear of the keyboard ISR changed it from 3 to
  0 and allowed boot progress without changing the guest binary.

The initial Instrument boot failed when Joey killed the timed-out C++ probe;
subsequent QMP thread snapshots located the starvation. One Instrument run
passed after the sound acknowledgement change, but the excessive IRQ rate
persisted: that pass did not close the bug. Host-side acknowledgement is only
an experiment, never a regression-test workaround.

The architectural correction is function-bound interrupt ownership, with a
shared INTx domain and a kernel-owned MSI-X domain. It must not require a
particular PCI slot order, make one driver's progress depend on another's
acknowledgement, or expose another device's interrupt authority.

## 2. Scope and hardware boundary

Design the complete lifecycle for:

1. PCI functions operated in POLLED mode: INTx disabled; MSI-X disabled.
2. Shared level INTx: per-function subscriptions on a shared routed line.
3. MSI-X: kernel-assigned messages and table entries, with GICv2m and GICv3
   ITS backends. A machine lacking a usable MSI controller uses shared INTx.
4. Existing exclusive non-PCI KObj_IRQ users, preserving their ABI.

No forced change to the accelerator or machine topology to manufacture an MSI
backend. Discover the controller actually described by the DTB. The current
GIC driver handles SGIs, PPIs and SPIs; ITS, LPIs, MSI allocation and MSI-X
mediation are new work. GICv2m alone is not full support for the existing
GICv3/TCG topology. Test both backends and a machine without MSI.

Legacy PCI MSI, bridges/hotplug, SR-IOV, interrupt remapping through an IOMMU,
and arbitrary user-chosen CPU affinity are outside this design. MSI-X must
not be described as DMA containment: the system currently has no SMMU-based
isolation, and a bus-mastering driver remains within that existing trust model.

## 3. Ownership, authority and objects

Keep one exclusive KObj_PCI per function. Add a PCI interrupt endpoint attached
to that object, represented by an interrupt handle with a distinct source kind
and the existing non-transferable, non-duplicable hardware-handle discipline.
An endpoint owns one logical source: the function for INTx, or one allocated
MSI-X table entry. Several endpoints may map to a shared physical INTID without
sharing capability ownership.

Endpoint creation requires CAP_HW_CREATE plus an acquired, writable KObj_PCI
handle belonging to the caller. Warden's PCI BDF allowance authorizes that
function. The kernel derives routing from its immutable enumeration and DTB;
the caller cannot supply a GIC INTID, message address, message data, foreign
BDF or foreign table index. Retaining a numeric routing hint in PCI_INFO must
not make that hint authority. Existing raw IRQ_CREATE must refuse PCI domain
lines and MSI-reserved vectors, including lines with no current subscriber.

The endpoint holds a PCI reference. The PCI object's endpoint list holds weak
membership links protected by its lifecycle lock; avoid a PCI<->endpoint
reference cycle. Dispatch takes a bounded in-flight pin before dropping the
membership lock. Parent-handle close alone cannot destroy mappings or hardware
still retained by an endpoint or BAR mapping. Process teardown explicitly
revokes the function's endpoints before releasing ordinary handles.

Claims begin electrically quiescent: INTxDisable set, MSI-X Function Mask set
and MSI-X Enable clear. Preserve unrelated PCI Command bits with 16-bit
accesses; never write the adjacent write-one-to-clear Status register during a
Command update. Enumeration also quiesces unclaimed supported endpoints before
shared domains become live. A polled driver needs no interrupt endpoint and
remains quiescent even when its queue flags allow a stray notification.

## 4. Userspace contract

Introduce a PCI-scoped interrupt interface, not unrestricted config writes.
Names below are proposed; append syscall numbers only after the ABI review.

* `pci_irq_create(pci, mode, vector_ordinal)` creates a disarmed endpoint.
  INTx accepts ordinal 0; MSI-X allocates a validated table entry and reports
  its index. Requested MSI-X fails explicitly if unavailable. A userspace
  `Auto` helper may then request INTx and reports which mode was selected.
* `pci_irq_info(endpoint)` reports mode, table index when applicable, generation,
  state and counters. It does not expose writable routing registers.
* `pci_irq_arm(endpoint)` is valid only for the initial disarmed state, after
  the driver has published queue state and acknowledged initialization residue.
* `pci_irq_wait(endpoint, timeout)` returns a fixed-size event containing an
  opaque generation/sequence ticket, a reason and a collapsed count. Timeout
  returns no ticket. One waiter per endpoint; a second gets a clean busy error.
* `pci_irq_complete(endpoint, ticket)` completes that delivery and re-arms after
  the driver has drained the relevant device state and completed its MMIO ack.
* `pci_irq_disable(endpoint)` terminally revokes the endpoint, makes the source
  quiescent and invalidates any outstanding ticket. Reconfiguration allocates a
  new endpoint generation; ARM cannot revive a revoked endpoint.

WAIT NEVER RE-ARMS. This separates sleeping from the authority-bearing operation
and prevents the current implicit wait/unmask bug. Duplicate, stale, wrong-mode
and cross-endpoint tickets fail without changing state. Sequence wrap is a
terminal endpoint error, not ABA reuse. Count saturation is explicit and cannot
alias an errno. Readiness is derived from a pending event; implement real poll
readiness for these endpoints if exposed as pollable, with registration-before-
check and close wakeups. Do not add an AsFd implementation over a non-pollable
handle again.

Drivers may coalesce multiple device queues onto one MSI-X entry. Initial
Nocturne uses one config/control/TX/RX event endpoint; Tapestry's GPU uses one
for its control/cursor completions; netd may use one initially. More entries
are an allocation policy, not a different lifetime protocol. Queue and config
vector indices are written while endpoints remain masked, then read back;
NO_VECTOR or a rejected index triggers full rollback before INTx fallback.

## 5. Shared INTx domain

One kernel domain owns each DTB-routed, level-triggered PCI SPI. It holds a
bounded set of function subscriptions, not a set of arbitrary INTID claimants.
PCI Interrupt Status identifies an asserting function; the kernel need not read
virtio ISR or consume any device-specific completion.

Dispatch, with no allocation or blocking:

1. Mask the physical line during the bounded scan and pin its membership view.
2. For each ARMED function, read its PCI Interrupt Status. On assertion, set
   that function's INTxDisable and complete the config write before recording
   one pending delivery and ticket. Only that function becomes DELIVERED.
3. Drop domain locks, publish/wake endpoint waiters using pinned objects, and
   complete GIC EOI in the architecture's documented order.
4. Re-enable the shared line after all recognized asserting functions have
   been individually masked. Other functions can now generate interrupts.

Never wait for every subscriber to acknowledge before unmasking the shared
line. A stalled or crashed function stays individually disabled; another
function on that wire remains serviceable. POLLED and unclaimed functions are
already disabled, not subscribers that must be woken to clear an ISR.

On COMPLETE, validate the ticket under the endpoint/domain lock. If PCI status
still reports assertion, leave that function masked and return retry-required;
it may be a new completion racing the acknowledgement, not driver misconduct.
The driver drains again. Otherwise finish the old delivery and enable the
function. A new assertion between the status check and enable remains a level
and triggers a fresh event. No queue edge is required for this race.

Bound immediate repeats and dispatch work per domain. Budget exhaustion puts
only a known offending function into COOLDOWN, with a timed retry notification;
it does not grant an unbounded syscall-spin loop. Thresholds are explicit
kernel policy, measured with network/audio/GPU workloads before claiming a
latency bound. Preserve pending work and report cooldown counters. A stuck
physical line despite disabling all known asserting functions is a controller
or device fault: mask and report the domain, with bounded recovery. In that
hardware-fault case affected peers may lose service; do not promise electrical
isolation that the wire cannot provide.

Recovery is attempted by a new endpoint creation only after every previous
subscriber has closed and a 100ms domain cooldown expires. One non-waiting
controller probe must observe the line inactive and not pending after pending
cleanup; unfinished GIC writes defer recovery. Failed probes keep the line
masked and restart the cooldown. Successful recovery creates a fresh endpoint
and generation; old tickets never regain authority. A repeatedly reasserting
fault is bounded by the same cooldown and 32-stray dispatch limit.

## 6. MSI-X mediation and memory isolation

The kernel parses the MSI-X capability, table BIR/offset/size and Pending Bit
Array ranges with overflow and BAR-containment checks. Bound allocation by a
per-function and system vector quota. Capability walking remains bounded and
rejects cycles/malformed lengths. Do not assume the QEMU BAR layout.

MSI-X table pages and PBA pages are kernel-owned from claim time. Current
PciDev::claim eagerly maps all small BARs, which would expose message address
and data to userspace. That behavior MUST change before enabling MSI-X:

* Report mappable BAR windows separately from physical BAR geometry.
* Reject EVERY user mapping overlapping a protected page, including alias,
  hostmem and later mapping paths. Existing live mappings must never precede
  reservation: reserve at claim, before the first map succeeds.
* The userspace helper maps allowed windows and resolves capability regions
  against those windows; it does not fail merely because a routing-only BAR
  is intentionally unavailable.
* If an essential device register shares a page with protected routing state,
  MSI-X is unavailable for direct-mapped operation. Use shared INTx while still
  retaining the routing-page restriction; if that also makes essential
  registers inaccessible, fail the claim with a clear unsupported-layout error.
  A mediated register interface is a separate supported layout, not a silent
  writable-table exception.

The kernel owns message address/data and MSI-X function/entry masks. Userspace
may select only its allocated vector indices through device queue configuration;
that cannot select another function's entries. A deliberately wrong local index
can break that driver's queues but cannot retarget another owner's interrupt.

Allocate a backend vector, install a disarmed handler, populate the table under
Function Mask, issue device barriers/readbacks, then enable MSI-X. Keep INTx
disabled in MSI-X mode. Entry unmask occurs only after ARM. On delivery, mask
that entry and issue an event. COMPLETE unmasks it; the device's PBA preserves
notifications arriving while masked. The driver checks ring state even when
notifications coalesce. Do not use ISR reads as an MSI-X completion source.

Mode transitions require every endpoint disarmed, no live dispatch pin, and a
quiesced device. No live migration from MSI-X to INTx underneath a blocked waiter.
Auto fallback is an initialization transaction with full rollback, not a runtime
retry inside interrupt context.

## 7. ARM MSI backends

### GICv2m

Discover the MSI frame via DTB compatibility/phandle relationships and `reg`,
validate its SPI range, and reserve that range against legacy raw IRQ_CREATE
and ordinary wired interrupts. Respect firmware-supplied SPI range overrides
where the binding permits them. Allocate only within that domain. Message
address/data are computed from the controller's documented frame interface,
never from a user input or a QEMU slot-number assumption. Configure allocated
SPIs as edge-triggered and route them to a supported CPU.

Free requires device entry masking, posted-write completion, source quiescence,
in-flight handler drain and pending-controller-state cleanup. Do not recycle
an INTID immediately after clearing a software callback: a late MSI contains
no software generation and could otherwise target its next owner.

### GICv3 ITS and LPIs

Extend discovery for the PCI `msi-map`/`msi-parent` relationship, requester IDs,
ITS register range and redistributors. The ITS translates a DeviceID/EventID to
an LPI/collection; allocating an arbitrary SPI is not a substitute.

Provide kernel-owned, correctly aligned ITS command/device/collection/interrupt
translation tables, plus LPI property and pending tables for participating CPUs.
Validate physical sizes, cacheability/shareability and controller readback;
initialization fails cleanly if unsupported. Command submission uses one
serialized, bounded ring with explicit completion/synchronization deadlines.
Never hold an IRQ/domain spinlock while waiting for ITS completion.

Maintain mappings from PCI requester identity -> DeviceID and allocated entry
-> EventID -> LPI, with per-CPU collection routing. Discover redistributor
capabilities before enabling LPIs. Extend the GIC dispatch namespace to LPIs
without indexing them into the existing SPI-sized array or truncating their
interrupt IDs. Handle reserved/special IDs according to the GIC architecture.

Teardown masks the device, drains posted writes and handlers, invalidates the
ITS mapping, synchronizes the controller, and only then releases IDs/tables.
A timed-out hardware invalidation quarantines the allocation; it must not be
reused under another function. CPU-offline routing is unsupported until its
collection migration protocol exists; normal boot-time SMP is supported.

## 8. Concurrency, failure and teardown obligations

Locks protect membership/state, not device waits. Establish and document one
order: PCI lifecycle -> domain membership -> endpoint event lock. Dispatch
must not acquire a PCI lifecycle lock while holding the domain lock; immutable
config-access information and pinned membership make that unnecessary. Wake
outside domain locks. Scheduler locks are never nested under a PCI config
transaction. Audit every path against actual irqfwd/rendez lock order before
implementation; this order is a proposal, not evidence of an audit.

Endpoint lifecycle: DISARMED -> ARMED -> DELIVERED -> ARMED, with optional
COOLDOWN and a terminal REVOKING -> DEAD path. Revocation first disables the
source, prevents fresh publication, detaches membership, wakes waiters with a
terminal event, then drains in-flight pins. No raw pointer may outlive the
registration that pinned it. Death while blocked, during COMPLETE, during a
shared-line scan, or during ITS synchronization must all converge on this path.

All allocations unwind in reverse order. A failed user copyout cannot leave a
live unowned endpoint or enabled function. Retain a handle reference across
syscall operations so concurrent close cannot free an object between lookup
and use. Reject driver-requested routing after its function is revoked.

An endpoint's PCI reference pins BAR/config access for dispatch; device
quiescence must precede DMA and BAR release. Process death is not proof that
a device has stopped bus mastering. Existing PCI teardown already owns that
obligation; the new endpoint path composes with it rather than inventing a
second independent teardown owner.

## 9. Driver migration

* **Tapestry input:** POLLED by default. No endpoint means no PCI interrupt
  delivery, regardless of queue notification hints or configuration changes.
  Poll config generation and acknowledge ISR residue for INTx bookkeeping;
  neither operation grants authority to change another function's line.
* **Nocturne:** create one function-bound endpoint. Its waiter handles tickets,
  acknowledges INTx only in INTx mode, pokes the cycle and completes the ticket
  before waiting. Keep the cycle's bounded backstop and device-ring ownership.
* **Tapestry GPU:** migrate its bounded submit waits and completion pump to the
  endpoint API; retain timeout/device-failure recovery. Avoid a second waiter
  competing with the compositor's synchronous path.
* **netd:** same mode-aware endpoint lifecycle; verify both interrupt-driven
  service and its polling/ring fast path under sustained traffic.
* **Warden/libdriver:** PCI allowances remain BDF-scoped. Stop presenting raw
  IRQ grants as the PCI driver's authority. Non-PCI grants retain their shape.

After all resident PCI IRQ users migrate, remove the device-order requirement
from run-vm.sh and the machine dossier. Retain only actual topology constraints.
No driver calls raw IRQ_CREATE on a PCI-derived INTID after migration.

## 10. Verification and review gates

Before code, write the invariant/interleaving argument. The general new-TLA
requirement is suspended, but existing handle and scheduler/IRQ-related models
must be checked where semantics change. Do not redefine hardware exclusivity
as "one handle per wire": preserve one owner per endpoint/function resource
while explicitly representing a shared routing domain. Run relevant existing
negative controls. The operator requested single-agent work; record self-review
honestly, not as independent adversarial review.

Required tests:

* Two functions on one INTx line: simultaneous assertions, either service order,
  one silent peer, one stalled peer, close during dispatch, and source reassert
  between acknowledgement and COMPLETE. Verify the other function progresses.
* POLLED keyboard config and input events while audio runs: assert its delivery
  is disabled and verify the audio IRQ rate settles when idle. Preserve the
  current pre-fix evidence as the regression's negative control.
* Wrong function, missing capability/right, raw PCI INTID, duplicate/stale
  ticket, second waiter, generation reuse, denied table/PBA mapping and
  overflowed capability ranges all fail without disturbing a neighbor.
* MSI-X config and queue vectors, masked pending events, table readback failure,
  allocation exhaustion, and complete rollback to explicit INTx initialization.
* IRQ close concurrent with wait/complete/dispatch; device death and restart;
  controller synchronization timeout quarantines vectors rather than reusing.
* GICv2/HVF with v2m, GICv3/TCG with ITS, and MSI-unavailable shared INTx.
  Include the existing default/UBSan x SMP4/SMP8 boot matrix.
* Device-order permutations intentionally collide NIC/GPU/audio INTx routes.
  Audio chord/ring/SDL/capture gates, Halcyon input/workspace transitions,
  GPU rendering, and network traffic run with each supported interrupt mode.

Capture counters for deliveries, coalescing, masked time, completion retries,
cooldowns and fault quarantine. Bound idle rates and queue-service latency from
measured workloads, with host/accelerator/configuration stamped. Screenshots
verify Halcyon remains responsive, but cannot stand in for interrupt evidence.

## 11. Delivery order and decision boundary

1. Ratify this design and the revised PCI/IRQ ownership invariant.
2. Implement quiescent PCI claims and protected MSI-X table/PBA mappings.
3. Implement function-bound endpoints and shared INTx; migrate resident drivers
   and reproduce the keyboard/audio collision as a passing regression.
4. Implement GICv2m MSI-X and test explicit mode selection/fallback.
5. Implement ITS/LPI MSI-X and controller teardown/quarantine tests.
6. Run the cross-mode/device-order/SMP matrix, update Vault, then resume and
   finish the aux visual integration on the validated kernel.

Stages are reviewable commits, not permission to label stage 3 "full MSI-X".
Do not land an aux integration that depends on unverified interrupt behavior.
The operator explicitly approved the full design and implementation after
reviewing this document on 2026-09-17. No new syscall number, kernel routing
change, or MSI-X enable is implemented merely by this design's ratification.

## 12. Sources and repository owners

External references, checked 2026-09-17:

* [Virtio 1.3 specification](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html):
  PCI transport ISR/config notifications, queue vector selection and MSI-X
  notification behavior. Check the document's stated revision; the served HTML
  identifies itself as Committee Specification Draft 01.
* [QEMU 10.0.2 ARM virt documentation](https://github.com/qemu/qemu/blob/v10.0.2/docs/system/arm/virt.rst):
  version-matched MSI controller topology. Do not assume newer QEMU machine
  options are available in the installed 10.0.2 binary.
* [QEMU 10.0.2 virtio implementation](https://github.com/qemu/qemu/blob/v10.0.2/hw/virtio/virtio.c):
  configuration notifications are separate from available-ring suppression.
* ARM GICv2/GICv3 architecture and the platform DT bindings are implementation
  prerequisites for register-level backend work. This design does not assert
  unverified ITS register values or alignment constants.

Local owners: kernel irqfwd, pci_handle, handle/syscall, ARM GIC and DTB;
libthyla-rs hardware; libdriver/warden; Tapestry input/GPU; netd; Nocturne;
the substrate machine and generated ownership views. Update their dossiers
as each stage changes an invariant; keep proposed and as-built state separate.

## 13. Implementation ledger

As of 2026-09-17, this work remains uncommitted in the aux/Halcyon integration
worktree. The following are implemented:

* Protected PCI mapping windows and function-bound interrupt tickets, including
  shared INTx dispatch, POLLED claim defaults, explicit COMPLETE and terminal
  revocation. Warden grants PCI functions without raw IRQ authority.
* Permanent raw-IRQ and PCI-domain callbacks, weak ownership registries and
  dispatch pins. Teardown unpublishes before draining and freeing its owner.
* DTB requester/controller discovery, protected controller frames, GICv2m vector
  allocation, generation leases, quarantine and reset-backed retirement.
* Stable per-function BAR placement and cached kernel control/table mappings.
  Repeated claims retain placement but acquire fresh exclusive capabilities.
* Kernel-mediated MSI-X table programming and readback. Fixed-form architecture
  MMIO accessors prevent post-indexed instructions that lack HVF's required ISV.
* Resident sound, GPU and network drivers use `PciIrq::for_virtio`: reset, select
  masked vectors, verify queue/config readbacks, then initialize DMA and arm.
  Auto prefers MSI-X and rolls back before INTx fallback. `PCI_IRQ_MODE=intx`
  and `msix` force a mode; unset/auto permits fallback. MSI-X does not read ISR.

Verified evidence:

* Shared INTx Instrument desktop workflows passed three times on HVF,
  129 seconds each. Read-only HVF counters showed idle sound 158 -> 158 over
  10.56 host seconds while GPU advanced 301 -> 365; no retries or cooldowns.
* Real virtio-RNG MSI-X tests pass on GICv2m/HVF: DMA completion while masked,
  PBA delivery after ARM/COMPLETE, replay and stale-ticket rejection. The boot
  suite passes all 1,562 kernel tests. The allocator test covers masked leases,
  raw denial, generation reuse, quarantine/reclaim and exhaustion. Sixteen
  claim/release cycles preserve BAR placement and live-handle accounting.
* Resident MSI-X Instrument workflows passed twice on HVF, 129 seconds each.
  Read-only RAM snapshots during the second run showed idle sound 173 -> 173
  over 46.23 seconds, GPU 290 -> 519, and no retries/cooldowns. Networking,
  sound and GPU reported MSI-X. These observations used no host ISR reads,
  acknowledgements, or VM pause/resume. A live 1280x800 screenshot was inspected.
* The native DTB harness exercises the actual parser. All 29 fixtures pass,
  including duplicate optional properties and owning-GIC association. The new
  owning-GIC guard passes a fresh HVF boot (25.695 seconds).
* Twenty-four relevant existing handle, Burrow, allowance and scheduler mutant
  configurations produce their expected counterexamples.

ITS/LPI implementation now passes a GICv3/TCG boot (57.187 seconds, 1,563 kernel
tests), including the real RNG MSI-X/PBA test and allocator quarantine/reuse.
All three resident drivers select MSI-X at boot. ITS command rings, tables,
per-CPU collections/pending tables, LPI properties, full-width dispatch and
source/controller/CPU-synchronized retirement are implemented. Commands have
100ms completion deadlines; IRQ paths only enqueue invalidation, never wait.
The shared system quota counts both backends, including uncertain failed leases.

Harness correction: Instrument hard-coded HVF even when its wrapper requested
TCG. Historical Instrument runs and snapshots are therefore HVF evidence. The
scenario now accepts `LS_CI_GFX_ACCEL=tcg`. Actual ITS desktop verification
passes in 173 seconds with both BOOT accelerator and ITS-ready witnesses.
Sound remained at 187 deliveries while GPU advanced 223 -> 310 over eight
seconds, with zero retries/cooldowns.

The latest TCG boot passes 1,565 tests. Fault tests cover table programming
rollback/quarantine, live arm/dispatch readback failure, peer waiter wakeup and
controller fault fanout. Private-register tests exercise the real ITS command
ring wrap, saturation, stalled-reader and completion deadline paths. Controller
fault stops the engine and defers endpoint notification through SGI 14, outside
ITS locks; uncertain DMA storage remains retained.

Both HVF and TCG pass 1,566 kernel tests, including two shared-line fault and
recovery cycles. The asynchronous GPU ticket pump passes ITS/TCG Instrument
(176 seconds) and no-MSI GICv3/TCG Instrument (174 seconds).

The forced benchmark-child failure control now passes on ITS/TCG alongside the
normal benchmark (1,567 kernel tests). It kills/reaps and releases the DMA and
Burrow references after a bounded progress deadline, and refuses a missing
benchmark fixture. The shared-a INTx desktop run passes with all three resident
functions on line 36; Gallery/View/manual pass on the shared-b ITS layout.

The 64-round last-close/dispatch stress test passes on both HVF and TCG:
the disappearing endpoint releases its parent reference, the live peer receives
every assertion, and IRQ live counts return to baseline (1,568 kernel tests).
All twelve audio/controller cases pass: byte-stream mixing, Weft ring playback,
SDL playback and capture-source permissions on v2m/HVF, ITS/TCG and shared
INTx/TCG. The shared-b no-MSI topology places NIC/GPU/sound on line 36.
Captured waveforms contain both tones simultaneously and a silent tail;
capture checks include allowed SYSTEM reads and denied user/mount paths.

The byte-verified 8 MiB NIC workload passes on v2m/HVF (61 seconds), ITS/TCG
(105 seconds), and shared INTx/TCG (105 seconds). Each exercises 200 immediate
and delayed round trips and 40 dials, then requires the host to receive every
byte after immediate client close. It exposed and drove the approved bounded
TCP retirement repair (`NET-CLOSE-DESIGN.md`) and preserved open errno through
the kernel. The current kernel suite passes 1,570 tests on all three backends.

Post-repair manual, real-Pi Haul mount/post, hangup, and four DOSBox gates
pass (eight scenarios). The Halcyon media gate also passes after the repair.
The SDL metadata/session gate passes in 71 seconds with reviewed captures.
The full default/UBSan by SMP4/SMP8 matrix passes 40/40 with zero failure
classifications. An additional eight-CPU ITS/TCG UBSan boot passes in 121
seconds, including 1,570 kernel tests and resident MSI-X drivers. Resident drivers have been exercised with automatic selection on all
three controller configurations; the environment-variable forcing path has
not been separately exercised in a resident-driver boot. Kernel tests cover
explicit mode claims and allocation failure/fallback prerequisites.
View/Gallery/manual and DOSBox session integration are verified; see
`AUX-HALCYON-INTEGRATION.md`. The Lex curiata visual/trust specification is
approved and retained with the source; its trusted graphical sink is not
implemented.

Additional primary references:

* [PCI MSI DT binding](https://github.com/torvalds/linux/blob/v6.12/Documentation/devicetree/bindings/pci/pci-msi.txt)
* [GICv2m binding](https://github.com/torvalds/linux/blob/v6.12/Documentation/devicetree/bindings/interrupt-controller/arm,gic.yaml)
* [QEMU 10.0.2 GICv2m model](https://github.com/qemu/qemu/blob/v10.0.2/hw/intc/arm_gicv2m.c)
* [GICv2m implementation quirks](https://github.com/torvalds/linux/blob/v6.12/drivers/irqchip/irq-gic-v2m.c)
