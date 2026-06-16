# MENAGERIE.md — the runtime driver framework: discovery, binding, third-party drivers

**Binding scripture.** Adopted 2026-06-15 (the aux architecture session;
user-ratified). This is the canonical design for Thylacine's driver framework —
the *binding layer* into which drivers drop — and for the real-hardware track
(RPi4 / RPi5) it unlocks. The kernel lift it requires (the **hardware
allowance** — a per-Proc scoping of `CAP_HW_CREATE`) introduces **invariant I-34**
(ARCH §28) and is audit-bearing (ARCH §25.4). Everything above the kernel line —
the **warden** (device manager), the `libdriver` framework, the manifest format,
and every non-DTB discovery source — is native `libthyla-rs` userspace.

Builds on the COMPLETE substrate: `KObj_MMIO` / `KObj_IRQ` / `KObj_DMA` /
`KObj_PCI` (the pci-1b exclusive claim is the first instance of the rule this
generalizes), the namespace (territory + `/srv` + `stalk`), Loom, and the
identity / clearance / imperium arc. The hardware reality (RPi4 BCM2711, RPi5
BCM2712 / RP1) is grounded against vendor datasheets + the brcmstb PCIe
upstreaming; citations in §12.

Cross-refs: ARCH §22 (hardware platform model) + §22.7 (this framework) + §28
(I-34) + §25.4 (audit triggers); ROADMAP §3.5 (no-in-kernel-drivers, the posture
this realizes) + the real-hardware track; `docs/TRUSTED-PATH.md` (the SAK the
third-party authorization prompt rides) + `docs/INSTALLER.md` (the first consumer
of the driver framework on a real disk).

---

## 1. Thesis: a binding *layer*, not a pile of drivers

The goal is not "write the RPi drivers." It is to build the **binding layer** into
which drivers drop — so that supporting new hardware is *adding a driver*, not
*editing the boot path*. Today every userspace driver hardcodes its world:
`usr/lib/netdev/src/virtio.rs:51` bakes in `VIRTIO_MMIO_BASE_PA = 0x0a00_0000` and
`intid = VIRTIO_MMIO_GIC_INTID_BASE + slot`. That works because there is exactly
one board (QEMU virt). The instant a second board exists, that constant is a lie,
and there is no channel through which a driver could *discover* the right value.
The binding layer is that missing channel, plus the policy engine that turns a
discovered device into a running, sandboxed driver serving a file. (This is the
structural blocker logged as gap G19.)

One model serves the whole range — a fixed on-die UART, a hot-plugged USB stick, a
third-party HAT — because we reduce them all to a single event stream and a single
bind decision.

---

## 2. The core fact: discoverability is a property of the *bus*, not the board

The question "can devices be detected and added at runtime on a SoC?" has a
layered, honest answer, and the layering is the whole architecture:

- **The on-die static fabric** (memory-mapped peripheral blocks; the I2C / SPI /
  MIPI / platform-bus controllers) is **non-discoverable** — fixed addresses,
  hard-wired interrupts; the OS must be *told* it exists, via the **DTB**. You
  cannot hot-plug a UART block onto silicon — but that fabric is also the part
  that *never changes*, so "no runtime detection" costs nothing.
- **Self-enumerating buses** — **PCIe, USB, SDIO/MMC** — give genuine runtime
  probe + hotplug. You walk the bus, query each device's identity, and bind. USB
  is built for surprise add/remove; PCIe hot-plugs where the controller supports
  it; MMC has card-detect.
- **The bridge for SoC-bus add-ons**: a HAT/cape carries an **EEPROM that
  self-describes**, and firmware loads a matching **device-tree overlay** so the
  new I2C/SPI device *appears to the OS at overlay-load time* — a runtime
  `DeviceAdded` even on a non-enumerable bus (`dtoverlay` does this live).

So **runtime detection is possible exactly where the hardware provides an
enumeration or a self-description mechanism.** The architecture's job is to model
the static and the dynamic as the *same* thing, so the binder never cares which it
faces.

The board choice helps: on **RPi5 the entire I/O complex — USB, Gigabit Ethernet,
GPIO, UART, I2C, SPI — lives in the RP1 chip behind a PCIe 2.0 ×4 link**, so the
enumerable path is the *main* path, not an edge case; RP1 bring-up is "enumerate
PCIe, then enumerate RP1's sub-functions."

---

## 3. The unifying model: discovery sources → events → the warden → bind + serve

Reduce every kind of hardware to a stream of two events, produced by a pluggable
set of **discovery sources**:

```
  DTB source        (static)   -> DeviceAdded for every `compatible` node, once, at boot
  PCIe source       (dynamic)  -> config-space scan; hotplug where supported; owns MSI routing
  USB source        (dynamic)  -> descriptor enumeration; surprise add/remove
  SDIO/MMC source   (dynamic)  -> card-detect -> enumerate
  overlay/EEPROM    (runtime)  -> HAT self-describes -> apply DT overlay -> DeviceAdded
        |
        v   one uniform stream:  { DeviceAdded(node) | DeviceRemoved(node) }
   [  WARDEN  ]  match compatible -> bind DB -> mint a NARROWED hardware allowance
   ( devmgr  )  -> spawn the driver Proc -> driver serves /dev/net/0 into the namespace
```

A `node` carries: an ordered list of **match identities** (most-specific first —
`compatible` strings for DTB nodes; a bus identity like a PCIe `vid:did`, a USB
`vid:pid`, or a virtio device-id for bus-enumerated nodes), the `reg` MMIO
window(s) (PA + size), the interrupt(s) (wired INTID + trigger, or an MSI
capability), and the parent-bus path. **The warden binds on the identity, not the
transport, and never reads a device register itself.** So a bus whose device type
is only knowable at runtime — a virtio-mmio slot's `DeviceID`, a PCIe function's
class — is enumerated by *its source*, which claims the raw transport nodes and
re-emits **typed** child nodes the warden binds by id. The warden consumes the
stream and never distinguishes static from dynamic: the DTB source fires *all* its
events at boot; USB fires them as you plug.

**The model is naturally recursive: a bound bus driver *becomes* a discovery
source.** Bind the PCIe host bridge → it enumerates → emits RP1 → bind the RP1
driver → it exposes USB/eth/GPIO as a child bus → emits those → bind their
drivers. The entire RPi5 bring-up falls out of one rule — "a driver can be a
source" — which is how Fuchsia DFv2 and Genode's platform-driver compose. (Which
sources/drivers the warden starts from is the board's BSP, read at boot from the
DTB root `compatible` — §17.)

Two refinements make it impeccable, and both substrates already exist in
Thylacine:

- **Deferred probe via the namespace.** A peripheral needs its clock/mailbox
  provider bound first (the BCM mailbox → clock → peripheral chain). Rather than
  Linux's retry-list, a driver simply **waits on `/dev/clk0` to appear** — the
  dependency is a *file*, and the bind/serve substrate already blocks on it
  (death-interruptibly, #811).
- **Teardown via I-25.** `DeviceRemoved` → the warden group-terminates the driver
  Proc (the #809/#811 cascade) → its allowance + handles drop → its service file
  vanishes from the namespace. The exact mechanism imperium uses to de-escalate a
  legate scope; restart-on-crash is the same primitive run forward.

---

## 4. The kernel lift: the hardware allowance (the one new mechanism) — I-34

This is the load-bearing addition — the equivalent of imperium's
fork-propagating scope. Everything else reuses existing primitives.

**Today `CAP_HW_CREATE` is coarse** (verified against the tree): at
`sys_mmio_create_handler` / `sys_irq_create_handler` / `sys_dma_create_handler`
(`kernel/syscall.c:216 / 368 / 458`) the gate is a flat
`(p->caps & CAP_HW_CREATE) == 0 -> reject`. A holder may then create a
`KObj_MMIO`/`IRQ`/`DMA` handle for *any* range the kernel has not I-5-reserved.
That is fine for three trusted system servers (stratumd, netd, tapestryd); it is
unacceptable for a fleet of per-device drivers, where a GPIO driver must not be
able to mint a handle over the disk controller's registers.

**The lift: scope `CAP_HW_CREATE` with a per-Proc *allowance*** — a set of
permitted `(PA window | IRQ/MSI | DMA pool)` resources. The three create syscalls
gain a check: *the requested resource must lie within the calling Proc's
allowance.* The warden holds the broad allowance (everything not kernel-reserved);
when it spawns a driver it confers a **narrowed allowance = the bound node's own
resources, intersected with the manifest's declared needs** — so a driver can
never ask for more than its device physically has. The three create-handler
gates (above) are the exact, confirmed insertion points.

Why this shape (not the obvious alternatives):

- **It preserves I-5.** Handles are still *created in-Proc* and remain
  non-transferable. We do NOT pre-mint handles in the warden and pass them down
  (I-5 forbids transferring MMIO/IRQ/DMA handles); we pass down the *authority to
  create*, bounded — the correct capability-theoretic move.
- **It generalizes pci-1b.** pci-1b already enforces an *exclusive*
  per-`(bus,dev,fn)` claim with BAR-bounded MMIO (`SYS_PCI_MAP_BAR` maps only the
  claimed BARs). The allowance is that idea lifted to the whole device space: a
  PCI device's allowance *is* its claimed BARs; a platform device's allowance *is*
  its DTB node's `reg` ranges. pci-1b becomes the first instance of the general
  rule.
- **It makes the grant auditable.** A driver's authority is a small, explicit,
  inspectable set — exactly what the manifest declared — not "anything unreserved."

**Invariant I-34 (driver authority bound)**: *a driver's hardware authority is
exactly its warden-granted allowance — a subset of its bound node's resources,
declared in its manifest, granted only by the warden, never widened, and fully
revoked on unbind, removal, or crash.* The I-25 analog for hardware. This is the
surface to model formally (it extends I-25 + generalizes pci-1b) and prosecute
hard (§11).

---

## 5. The warden (device manager)

A native `libthyla-rs` Proc, spawned by joey (init) with the broad allowance — the
single trusted broker, the Genode platform-driver analog. It is part of the system
TCB; the drivers under it are not. Responsibilities:

- **The bind database**: `compatible[] -> { driver binary, declared needs, the
  served service, restart policy, ABI version, signature }`, populated from the
  system driver set's manifests + any installed third-party manifests.
- **On `DeviceAdded(node)`**: match `compatible` (most-specific wins) → resolve the
  driver → compute the narrowed allowance (node resources ∩ manifest needs) → apply
  the authorization policy (§9) → spawn the driver Proc with that allowance → the
  driver serves its file into the namespace.
- **On `DeviceRemoved(node)`**: revoke the allowance + group-terminate the driver
  (I-25) → its service file disappears → consumers observe the close and re-bind
  (Plan 9 reconnect semantics).
- **Deferred probe**: a driver whose dependency service is not yet present blocks
  on it in the namespace; the warden need not sequence — the filesystem does.
- **Supervision**: a crashed driver is restarted per its manifest policy; the
  service file blinks. Bounded restart (back-off + a give-up threshold) so a
  crash-looping driver does not spin.

The warden owns **all** grant decisions — one auditable chokepoint. Bus drivers are
*sources that report to it*, never spawners of their own children (which would
scatter the privilege decision across N places). **As-built (build-arc 5e-4, audit
F2):** the kernel *enforces* this — `rfork_internal` denies a child Proc to any
hardware-allowance-narrowed Proc (a driver/source), so a driver cannot spawn a
hw-capable grandchild that would survive its `DeviceRemoved` revoke + terminate.
The broad warden/TCB spawns; drivers are leaves. See `docs/reference/117-allowance.md`
("Drivers are leaves").

---

## 6. Anatomy of a driver + the manifest

A driver is a native `libthyla-rs` Proc built on **`libdriver`** (the framework
crate): a small probe/bind/serve scaffold so a new driver is a `probe()` + the
device logic + a served file, not boilerplate. It receives its narrowed allowance
+ the bound node's resource descriptors at spawn, mints its `KObj_MMIO`/`IRQ`/`DMA`
handles via the existing `libthyla_rs::hardware` RAII wrappers (now
allowance-bounded), drives the device, and serves a file (`/dev/net/0`,
`/dev/mmc0`, `/srv/...`). High-throughput drivers (net, block, GPU) ride **Loom**
for the data path (the seL4 sDDF lock-free-queue model Thylacine already has).

The **manifest** is the declarative contract — the thing that makes a driver
*droppable* and its grant *auditable*:

```
driver "rp1-eth" {
    abi      = 1                                  # framework ABI it targets
    binds    = ["raspberrypi,rp1-eth", "brcm,genet-v5"]
    needs {
        mmio = "node:reg"        # its bus node's own reg window(s) -- never more
        irq  = "msi:1"           # one MSI vector  (or "node:interrupts" for wired)
        dma  = "pool: 2 MiB"
    }
    serves   = "/dev/net/%instance"
    restart  = on-crash          # supervisor policy
    sig      = "<ed25519 over the package>"        # optional; drives section 9
}
```

`needs` is bounded by the node: the warden intersects the asks with the actual node
resources, so a manifest cannot widen a driver's reach beyond its device. That
intersection is the auditable-grant property (I-34) in one line.

---

## 7. The discovery sources

Each source is itself a driver (bound first — the recursion), except the DTB
source, the kernel-exported bootstrap root.

- **DTB source (kernel `devhw`)**: the *only* source the kernel provides — a
  synthetic device (Plan 9 `#H` shape) publishing the parsed DTB as a **walkable
  tree**: each node a directory exposing `reg`, `interrupts`, `compatible`,
  `clocks`. This is the honest enforcement of I-15 ("hardware view derives entirely
  from DTB"). **Scope note (ground-truthed):** the kernel already holds the FDT
  blob and parses it, but the *exported* API today is point-lookup-by-compatible
  (`dtb_get_compat_reg{,_n}`, `dtb_for_each_compat_reg`, `dtb_get_compat_prop` in
  `kernel/include/thylacine/dtb.h`) — **not** a node tree. `devhw` therefore needs
  a tree-walk publish layer over the FDT (a Dev exposing the node hierarchy), which
  is a modest real addition, not "merely re-export an existing call." The data is
  all present; the surface is new. Every other source lives in userspace, keeping
  the TCB minimal.
- **virtio-mmio source (QEMU-virt's bus enumerator)**: the first concrete
  userspace source, and the template every self-enumerating-bus source follows.
  QEMU's virt machine exposes ~32 *identical* `virtio,mmio` transport slots whose
  DTB nodes (all `compatible = "virtio,mmio"`) cannot say which device sits in
  which slot — only each slot's runtime `DeviceID` register can. The source is
  granted *only* the virtio-mmio bank (one MMIO window; **no IRQ** — it reads, it
  does not service), reads each slot's `DeviceID`, **suppresses** the raw
  `virtio,mmio` transport nodes, and re-emits one **typed** `virtio:<device-id>`
  node per *populated* slot carrying that slot's exact `reg` + wired INTID. The
  warden then binds `netdev` to the `virtio:1` node, granting exactly that slot's
  window + IRQ — the tightest possible allowance, with the warden never touching a
  device register. This is *why* the whole loop proves out on QEMU with zero
  real-hardware risk: virtio-mmio is the stand-in for the PCIe/USB sources, which
  enumerate the identical way (config-space class / USB descriptor → typed node).
  On real boards there is no virtio-mmio source — its slot in the architecture is
  the PCIe source.
- **PCIe source**: the brcmstb host-bridge driver. Scans config space, emits a node
  per function, and **owns MSI routing** (controller-specific on Broadcom — not a
  generic GIC ITS; §12). On RPi5 this is the spine — RP1 and everything behind it
  arrives here.
- **USB source**: the XHCI host driver. Descriptor enumeration; the canonical
  hotplug source.
- **SDIO/MMC source**: card-detect → enumerate.
- **overlay/EEPROM source** (designed seam, deferred past first-boot): a HAT/cape
  EEPROM self-describes; the source applies a DT overlay and emits the new nodes.
  Real but RPi-firmware-quirky (no overlay-parameter passing today), so the source
  *interface* admits it while the implementation waits.

The minimal **kernel-resident driver set** (the TCB floor, never userspace): the
GIC, the timer, the console UART (the A-4c / trusted-path floor depends on it), the
CSPRNG seed source, and the DTB source itself. Everything else the warden binds in
userspace.

---

## 8. Third-party drivers — the capability-sandbox inversion

This is where the model stops being "as good as Linux" and becomes *categorically
better* (and a NOVEL position — NOVEL.md).

In a monolithic kernel a driver runs in ring 0 with total power — which is
precisely *why* Linux keeps its in-kernel driver ABI deliberately **unstable**: to
force drivers in-tree where they can be reviewed. Out-of-tree third-party drivers
are a permanent security and stability wound. Thylacine inverts every term:

1. **A driver is a userspace Proc holding only its device's narrowed,
   non-transferable allowance.** A third-party driver therefore *cannot* crash the
   kernel, touch another device's MMIO, see another process, or escalate — its
   entire blast radius is its own device. (I-1 isolation + I-5 non-transferability
   + the §4 allowance/I-34 + pci-1b's exclusive claim.) A property Linux
   structurally cannot offer.
2. **So a stable driver ABI becomes *desirable*, not a liability** — and one already
   exists: the `KObj_MMIO`/`IRQ`/`DMA`/`PCI` syscalls + `libthyla_rs::hardware` +
   Loom + the serve-a-file (9P) protocol + the manifest. Third parties target a
   frozen public ABI; no in-tree requirement, no recompile-per-kernel.
3. **A driver package = binary + manifest** (§6). The manifest's `needs` make the
   capability grant bounded and *readable*; the user sees exactly what a driver
   will be allowed to touch before authorizing it.
4. **Authorization reuses the imperium/clearance arc verbatim** (§9).
5. **Conflict, versioning, restart** are already solved: exclusive claims (pci-1b)
   stop two drivers grabbing one device; the bind DB resolves specificity; the
   manifest's `abi` lets the warden refuse a stale driver; supervision restarts a
   crasher.

The tell that this is the *right* design for this codebase: it invents almost no
new invariant. It is I-1 + I-5 + I-2/I-25 + I-15 + I-29/I-30 + pci-1b's claim,
wired together by the warden, plus the one §4 allowance lift (I-34).

---

## 9. Authorization — the convenience ladder (ratified posture: convenience)

Granting a driver its allowance is a privilege decision. The posture (user-chosen)
is **convenience**: the default is "try to make it work, ask once on the trusted
path, remember the answer" — not "refuse unless pre-authorized."

```
  system driver set            -> trusted, auto-bound, no prompt
  third-party, trusted-signed  -> auto-authorized, no prompt
                                  (install a vendor signing key once -> all their drivers just work)
  third-party, unsigned/new    -> the warden ATTEMPTS the bind and surfaces a
                                  trusted-path prompt (SAK -> corvus shows the
                                  *provincia*: "driver X wants device Y [USB vid:pid],
                                  gets MMIO window W + IRQ N, serves /dev/foo --
                                  allow once / always / deny") and REMEMBERS "always"
```

Unsigned third-party drivers are **allowed** — with a single, remembered,
trusted-path prompt — not locked out. That prompt is the *lex curiata* applied to
hardware: the SAK + corvus show the exact device + the exact allowance on the
unspoofable channel (`docs/TRUSTED-PATH.md`) before you consent, so no program can
trick you into a wider grant than you read. A third-party driver is just another
clearance grant.

A **paranoid mode** (require a trusted signature; refuse unsigned) is a single
hostowner toggle — opt-*in* to lockdown, never the default.

---

## 10. Hotplug + surprise removal — the one real teardown hazard

A driver mid-DMA when its device is yanked (USB) is the sharp edge. The capability
model already makes it *safe* (an MMIO access to a revoked window faults the
driver, never corrupts the kernel — `proc_fault_terminate`), but two things must be
deliberate:

- **The data path needs a terminal "device gone."** When the warden processes
  `DeviceRemoved`, an in-flight Loom op against the vanished device must complete
  with a **device-gone terminal CQE**, not hang — so the consumer unwinds cleanly.
  This extends the Loom completion-integrity invariant (I-29) to "the backing
  device disappeared," and wants an explicit terminal error code
  (`T_E_DEVGONE`-class).
- **The allowance must be revocable, and in-flight DMA fenced.** `DeviceRemoved`
  revokes the allowance + tears down the driver (I-25); any outstanding DMA
  descriptor pointing at the gone device is abandoned by the same teardown. The
  driver Proc dies; its pages return through the normal path.

Surprise-removal correctness is the part of this design that most wants a spec + a
focused audit when it is implemented.

---

## 11. Invariants + audit surface

**Composes** (invents almost nothing): I-1 (per-Proc namespace isolation — drivers
are isolated Procs), I-5 (hardware handles non-transferable), I-2/I-25 (capability
narrowing + scope teardown — the allowance grant + the unbind lifecycle), I-15
(hardware view from DTB — now *enforced* by `devhw` + the source model), I-29/I-30
(Loom completion integrity + submit-time pin — the driver data path), and pci-1b's
exclusive claim (conflict resolution).

**The genuinely new surfaces to prosecute** (audit-bearing — the privilege spine of
every future driver; ARCH §25.4):

- **The hardware allowance / I-34** (§4): a driver can mint a handle ONLY within its
  allowance; the allowance is a subset of its bound node's resources; it is never
  widened; it is fully revoked on unbind/removal/crash. *Prosecute*: an
  allowance-bypass on `SYS_*_CREATE`; an allowance that outlives its device; an
  intersection bug that grants a superset; the SMP race between a concurrent
  `DeviceRemoved` revocation and an in-flight `SYS_*_CREATE`.
- **The warden's grant decision** (§§5, 9): the provincia displayed equals the
  allowance conferred; no path widens a grant past the manifest; the authorization
  ladder cannot be skipped.
- **Discovery-source trust**: a malicious or buggy source must not be able to
  *fabricate* a device node that lures a driver into claiming real silicon it
  should not, nor forge a `compatible` to bind the wrong (more-privileged) driver.
  Sources are themselves bound drivers, so this reduces to the allowance + the
  bind-DB integrity — but it is the subtle one.

**I-34** is the candidate invariant, numbered here (ARCH §28).

---

## 12. RPi4 / RPi5 bring-up: what the model forces (and the grounding)

Each requirement maps onto a discovery source or a driver — the model absorbs them
rather than special-casing. Verified facts cited.

| Requirement | Why QEMU virt hid it | Lands as |
|---|---|---|
| **Clocks / power / resets + the BCM mailbox** | virtio is always-on; no firmware mailbox | A mailbox driver + the deferred-probe-via-namespace chain (§3). Most BCM peripherals do not respond until a clock is enabled via the VideoCore mailbox. |
| **brcmstb PCIe host bridge** | QEMU uses generic ECAM | A PCIe *source* driver; pci-1b's claim/BAR/cap-walk core is reusable, but link-training + window setup is controller-specific (`brcm,bcm2711-pcie` / `bcm2712`). |
| **MSI / MSI-X** | virtio-mmio uses wired INTx | Owned by the PCIe source. BCM2711's PCIe does INTx-A + MSI; BCM2712 ships a dedicated MSI-X controller for the RP1 endpoint, in the brcmstb driver — so MSI is a *property of the host-bridge driver*, not a generic GIC ITS/v2m. On RPi5 mandatory: RP1 is the whole I/O complex. (This is the home for the pci-arc's recorded MSI-X v1.x seam.) |
| **Interrupt-controller chaining** | flat GIC on virt | RPi4 has the legacy ARMCTRL behind the GIC-400; RP1 has its own MSI domain. A source/driver registers as an IRQ demuxer. |
| **Non-virtio SoC drivers** | everything was virtio | GENET gigabit eth (RPi4, SoC MAC); SDHCI/EMMC2 (boot medium); **bcm2835/iproc-rng200** (the CSPRNG seed — virtio-rng is *gone*, so the W3 seed path needs a board RNG driver or fails closed); USB XHCI (behind PCIe — VL805 on RPi4, RP1 on RPi5); GPIO/pinmux; thermal; watchdog. |
| **DMA cache coherence** | virtio on virt was coherent | RPi peripherals are frequently non-coherent; the DMA-handle model needs an explicit cacheable-vs-device contract + clean/invalidate. netdev's `prewarm()` hints this is partly handled; real boards make it load-bearing. |
| **Per-board BSP + early console** | one DTB, one PL011 | `chosen/stdout-path` parse for the early console (RPi is mini-UART *or* PL011 by config) + a per-board default driver-set + quirks. Small (the board ships its DTB), but the difference between "boots silent" and "boots with a log." Board identity + the universal image: §17. |

Grounding: RP1 connects to BCM2712 over PCIe 2.0 ×4 and aggregates USB2/3, GbE,
GPIO, UART, I2C, SPI, MIPI behind an AXI→PCIe device controller
([raspberrypi.com/news: RP1](https://www.raspberrypi.com/news/rp1-the-silicon-controlling-raspberry-pi-5-i-o-designed-here-at-raspberry-pi/),
[RP1 peripherals datasheet](https://datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf)).
BCM2711 = GIC-400, PCIe INTx-A + MSI
([DeepWiki BCM2711](https://deepwiki.com/raspberrypi/linux/2.2-bcm2711-soc-(raspberry-pi-4))).
BCM2712 MSI-X controller for the RP1 endpoint in the brcmstb driver
([LWN: PCIe for bcm2712](https://lwn.net/Articles/979758/)). HAT EEPROM →
auto-loaded DT overlay
([Quorten](https://quorten.github.io/quorten-blog1/blog/2020/10/22/rpi-dto-auto-eeprom)).
Bus discoverability dichotomy
([bootlin/Petazzoni](https://bootlin.com/pub/conferences/2014/elc/petazzoni-device-tree-dummies/petazzoni-device-tree-dummies.pdf)).

---

## 13. Resolved forks

1. **Discovery-source placement** → the kernel exports only the raw DTB inventory
   (`devhw`); every other source is a userspace driver registered with the warden.
   Minimal TCB (the Genode posture).
2. **Warden topology** → one central broker owns the bind DB + all grant decisions;
   bus drivers are sources, not spawners. One auditable chokepoint.
3. **Third-party default** → **convenience** (§9): try-bind, ask once on the trusted
   path, remember; paranoid lockdown is an opt-in toggle.
4. **Overlay/EEPROM** → a designed seam in the source interface; implementation
   deferred past first-boot.
5. **Board selection** → auto-detect from the DTB root `compatible` (never a user
   prompt); a **universal image** (one medium, multi-board, the DTB selects at
   boot); unknown board → fail-soft generic profile + loud log (halt is an opt-in
   lockdown). §17.

---

## 14. Thematic naming (proposed)

The existing `bestiary[]` (`dev.h`) is the Plan 9 `devtab` — the catalog of device
*types*. The live, bound, running drivers are the **menagerie** (the captured
beasts), and the broker that keeps them is the **warden**. A discovery source
brings new specimens into the menagerie. Load-bearing terms stay plain (`driver`,
`bind`, `compatible`, `allowance`); the color is **menagerie / warden**, on-brand
with the bestiary.

---

## 15. Dependencies / lane split / unblock order

**Kernel owes (main track):**
1. `devhw` — the DTB inventory published to userspace as a walkable tree (the
   bootstrap source; the I-15 enforcement point).
2. The **hardware allowance / I-34** — scope `CAP_HW_CREATE` per-Proc; bound at
   `SYS_MMIO/IRQ/DMA_CREATE` (§4). The one new mechanism; spec-modeled (extends
   I-25 + generalizes pci-1b). **Audit-bearing.**
3. MSI inside the brcmstb PCIe host-bridge driver path; IRQ-demux registration
   (the real-hardware sub-track — the pci-arc MSI-X seam lands here).
4. A Loom **device-gone** terminal completion (§10).

**Userspace (native `libthyla-rs`):**
5. **`libdriver`** — the probe/bind/serve framework crate + the manifest schema.
6. The **warden** — bind DB, the match→allowance→spawn→serve engine, deferred
   probe, supervision.
7. **The discovery-source layer** — the `DiscoverySource` / `DeviceNode`
   abstraction (libdriver) + the **virtio-mmio source** (the QEMU-virt bus
   enumerator, §7). This is the *proper* realization of §3's pluggable-source model
   and the warden's true discovery surface; 5c's static DTB-walk is just the
   `DtbSource`'s first form. **Pulled forward into the QEMU-virt proof** (resequenced
   2026-06-16): the DTB is type-blind to a virtio-mmio slot's device, so binding the
   existing virtio drivers *correctly* requires the source layer now — not a
   warden-side DeviceID hack deferred to "later." The PCIe/USB sources are then the
   same abstraction, one slot each, added with the real-hardware track.
8. Retrofit `netdev` to a grant-driven `impl Driver` the warden binds by `virtio:1`
   through the source layer (retire `virtio.rs:51` + the bank-probe loop).

**Order**: `devhw` + the allowance are the gate; with them, `libdriver` + the
warden realize the **§3 discovery-source layer** (the source abstraction + the
`DtbSource` + the virtio-mmio source) and bind the existing virtio drivers on QEMU
virt **by typed id** (proving the whole loop with zero real-hardware risk) before
the PCIe/USB sources + the RPi drivers arrive. **net-2 lands after the Menagerie
spine, so `netd` is born discovery-driven and narrowed to just its NIC** (rather
than holding coarse `CAP_HW_CREATE` + a hardcoded base, then being retrofitted) —
the sequencing rationale ratified 2026-06-15; the discovery-source-layer
pull-forward ratified 2026-06-16.

---

## 16. Build sequence (main track)

Scripture-first, model-first for the allowance:

1. **This scripture commit** — `docs/MENAGERIE.md` + ARCH §22.7 + I-34 + the audit
   triggers + ROADMAP. No code.
2. **`devhw`** (the DTB tree-walk publish Dev) — a Dev surface; reference-doc'd.
3. **The hardware allowance / I-34** — the per-Proc allowance + the three
   create-handler checks; **model-first** (a `specs/allowance.tla` extending the
   I-25 scope model), audit-bearing.
4. **The Loom device-gone terminal CQE** (§10).
5. The warden + `libdriver` realize the **§3 discovery-source layer** — the
   `DiscoverySource` / `DeviceNode` abstraction, the `DtbSource` (5c's DTB-walk
   refactored), and the **virtio-mmio source** (the bus enumerator that re-emits
   typed `virtio:<id>` nodes) — and bind the existing virtio drivers on QEMU virt
   **by typed id**, the whole loop proven with zero real-hardware risk. (Resequenced
   2026-06-16: the source layer is built *here*, as the proper form of discovery,
   not deferred — the DTB cannot identify a virtio-mmio device, so a correct bind
   needs the source.)
6. The PCIe/USB sources + the RPi driver set + the real-hardware bring-up — each
   PCIe/USB source is *the same `DiscoverySource` abstraction*, one per bus.
   - **6a (landed, #159)**: the kernel half — the **per-`(bus,dev,fn)` PCI
     allowance axis** (`HW_RES_PCI`; `§4`'s "a PCI device's allowance IS its
     claimed BARs"). `SYS_PCI_CLAIM` now gates on the resolved `(bus,dev,fn)`,
     replacing the I-34-round fail-closed reject, so a warden-narrowed PCI driver
     can claim exactly its function. See `docs/reference/117-allowance.md` §"The
     fourth door".
   - **6b**: the userspace half — a PCIe bus-enumerator source Proc (config-space
     scan → typed `(bus,dev,fn)` nodes) + the warden binds netdev-pci narrowed
     (the live I-34-on-PCI proof). Then **net-2** resumes on the PCI NIC.

---

## 17. Board identity and the universal image

The board is the one thing the system must know *before* any of the above runs —
which BSP, which sources, which early console — and the one thing it must **never
ask the user.** The board self-identifies; we read it, we do not prompt. This is
I-15 taken to its conclusion: even "which board am I" derives from the DTB.

### 17.1 The board self-identifies (the DTB root)

```
RPi4:  compatible = "raspberrypi,4-model-b", "brcm,bcm2711";  model = "Raspberry Pi 4 Model B"
RPi5:  compatible = "raspberrypi,5-model-b", "brcm,bcm2712";  model = "Raspberry Pi 5 Model B"
QEMU:  compatible = "linux,dummy-virt"
```

On RPi the VideoCore firmware reads the board's OTP revision, loads the *matching*
`.dtb`, and hands it to us in `x0`; QEMU generates a virt-correct DTB the same way.
By the time `_start` runs we already hold a board-correct tree (`_saved_dtb_ptr`,
consumed in `kernel/main.c`). Reading the root `compatible` *is* the board ID — no
mailbox call, no config file, no prompt.

### 17.2 The BSP is a board-ID → profile map

The board ID keys the **BSP**: `compatible -> { default driver set, quirks,
early-console choice }`, applied before the warden's first bind (the
`of_machine_is_compatible()` analog). Adding a board is adding a BSP entry — the
same extensibility property as adding a driver.

### 17.3 The bootstrap is self-consistent — there is no human in it

```
  DTB -> early console (chosen/stdout-path) -> board ID (root compatible) -> BSP -> warden -> drivers
```

A prompt is not merely unnecessary, it is *impossible and wrong*: it would need a
console to display it, but which console (mini-UART vs PL011, where) is itself
DTB-derived — chicken-and-egg. It would re-ask a fact we already hold, and
introduce a failure mode that cannot otherwise exist (a human picking the *wrong*
board → a mismatched driver set → a brick). Auto-detection cannot be "wrong" the
way a person can. Every comparable system converges here: Linux ships one arm64
kernel for all boards; Raspberry Pi OS boots every Pi from one image; Fuchsia takes
the board from the bootloader's ZBI. None prompt.

### 17.4 The universal image (resolved: universal)

**One image carries RPi4 + RPi5 + QEMU-virt support; the DTB selects at boot.** QEMU
virt is a board like any other here — detected by the same root `compatible`
(`linux,dummy-virt`), its BSP simply *being* the virtio-mmio driver set already in
the tree — which is why the entire model proves out on QEMU, with zero
real-hardware risk, before the first RPi driver exists (§15). Write it to any
supported medium, boot it on any supported board, it adapts. The DTB detection
makes this nearly free, and it has the property that matters most for a
Stratum-backed OS: **the pool is portable and the board is detected live, so moving
the SD card to a different Pi just boots and re-adapts.** The board is emphatically
*not* baked into the install — the install is the pool contents; the board is read
fresh each boot. (Per-board images were the alternative: simpler to assemble, but
they push a "pick the right file" choice to download time and lose the
move-the-medium property. Rejected.)

### 17.5 Unknown board → fail soft (mirrors the §9 posture)

If the root `compatible` is not in the BSP table, boot a **generic DTB-only
profile** — UART, timer, GIC, memory all come from the DTB, so a generic ARM64
board comes up from its device tree alone — and **log loudly** (`unknown board
"vendor,xyz" -- running generic arm64 from DTB`). A halt-on-unknown is the opt-in
lockdown toggle. Detection is **stateless + per-boot**: the live DTB is the single
source of truth (I-15); never persist a board record that could later fight the
medium it boots on.

### 17.6 First boot is a confirmation, not a selection

Show the detected board as information, never a question: `Thylacine: detected
Raspberry Pi 5 Model B (bcm2712)`. The only board-adjacent prompts a user ever sees
are the §9 third-party / HAT-overlay authorizations — never the board itself.

### 17.7 Lane note

The board-ID read + the BSP map + the generic fallback are warden/BSP logic (off
the `devhw` root `compatible`). The **universal-image assembly** — one boot medium
carrying multi-board support + the per-board DTBs the firmware chooses among — is a
build/boot-medium concern (main-track tooling; `docs/INSTALLER.md`). The
early-console `chosen/stdout-path` selection is kernel.

---

## 18. Status

- **2026-06-15**: scripture adopted (this doc + ARCH §22.7 + I-34 + the audit
  triggers + ROADMAP). No code yet. The build sequence (§16) is the next arc, after
  which **net-2 resumes** on the Menagerie substrate.
- **2026-06-15 (devhw-1, build-sequence step 2)**: the DTB tree-walk publish Dev
  landed. `lib/dtb.c` gains the enumeration API (`dtb_node_at` / `dtb_node_iter`
  / `dtb_prop_at` / `dtb_node_parent`, structure-block-offset-keyed, bounds-
  checked); `kernel/devhw.c` is the Dev (`dc='H'`, read-only, `perm_enforced =
  false`, non-seekable) mapping the FDT node hierarchy onto a walkable namespace
  (nodes = directories, properties = raw-byte files). Reference doc
  `docs/reference/116-devhw.md`. 11 kernel tests (the load-bearing one:
  `readdir_cookie_contract` -- strictly-monotonic non-zero dirent cookies);
  902/902 PASS, boot OK. **Not yet mounted** (`/hw` graft = devhw-2). No new
  invariant (composes I-15); self-audit only -- the formal audit is owed at the
  allowance (I-34) + the mount.
- **2026-06-15 (devhw-2)**: `/hw` mounted in the boot namespace. A 0555
  SYSTEM-owned `hw` synth mount-point dir in devramfs +
  `joey_mount_static_dev(kt, &devhw, "hw", 2)` in the kproc namespace + the
  userspace pre-pivot `O_PATH` grab + post-pivot `MREPL` re-graft (the
  `/srv`/`/proc`/`/dev` idiom). A boot probe (`/hw/cpus/cpu@0/reg`) proves the
  full chain -- stalk cross-mount -> devhw reuse-`nc` walk -> property read --
  every boot. The brittle "files + 4 synth dirs" devramfs test fixed at the
  source (a `devramfs_synth_dir_count()` accessor; the count is now derived).
  902/902 PASS + SMP gate 40/40 (1 ground-truth-verified timing exit: reached
  boot OK + login prompt, 0 corruption). **Next**: the hardware allowance
  (build-sequence step 3 -- audit-bearing + SMP-race-bearing; the spec-first
  re-enablement for `specs/allowance.tla` is a user decision).
- **2026-06-15 (the hardware allowance / I-34, build-sequence step 3)**: the
  per-Proc scoping of `CAP_HW_CREATE` landed. **Model-first** (spec-first
  RE-ENABLED, user-voted 2026-06-15): `specs/allowance.tla` (the clean cfg +
  the 4 buggy cfgs `revoke_race` / `revoke_leak` / `confer_widen` /
  `self_widen`) was written + TLC-green FIRST (commit `1602e37`), then the
  impl. NEW `kernel/allowance.{c,h}`: `struct Allowance` { the conferred MMIO
  PA windows + IRQ INTIDs + a DMA per-buffer cap + the atomic `revoked` flag +
  a `lock` }; `allowance_permits` (CreateBegin, the lock-free gate) /
  `allowance_handle_alloc` (CreateCommit, the under-lock `revoked` re-check) /
  `proc_confer_allowance` (the warden's set-once-at-spawn grant) /
  `proc_revoke_allowance` (DeviceRemoved) / `allowance_clone_into` (rfork
  inherit) / `allowance_free`. The per-Proc `struct Allowance *allowance` field
  is NULL = **broad** (the warden + the existing trusted servers, the as-built
  v1.0 behavior unchanged) or non-NULL = **narrowed**; the three create gates
  at `sys_{mmio,irq,dma}_create_handler` (`kernel/syscall.c` 228/287/494) run
  the CreateBegin check then the CreateCommit install. The central
  revoke-vs-create SMP race is closed by the two-step gate (the lock-free
  permits check, then the install under a `revoked` re-check under the same
  lock `proc_revoke_allowance` takes -- so an in-flight create racing a
  `DeviceRemoved` revoke aborts). Reference doc
  `docs/reference/117-allowance.md`. 912/912 PASS (+10 `allowance.*`, incl.
  `handle_alloc_revoked_aborts` = the race regression); boot OK (the broad path
  preserves the existing virtio drivers). The warden's confer-at-spawn syscall
  wiring + the bind DB + `libdriver` are the **step-5** consumer.
- **2026-06-15 (the Loom device-gone terminal CQE, build-sequence step 4)**: the
  §10 device-gone teardown landed. **Model-first** (spec-first re-enabled for the
  Loom surface, LOOM.md §7): `specs/loom_devgone.tla` (clean + liveness + 3 buggy
  cfgs `drops_reason` / `leaks_inflight` / `double`) was written + TLC-green FIRST,
  then the impl -- a focused module (the `loom_multishot` / `loom_order` precedent;
  the audited `loom.tla` + its cfgs untouched). The mechanism is a **session-death
  reason** threaded to the terminal CQE: the transport recv already distinguishes a
  clean EOF (`0` = the peer/server endpoint vanished = the device/service gone)
  from an error (`< 0`), but the 9P client's reader collapsed both to `-1` and
  always fired `-P9_E_IO`. Now `kernel/9p_client.c::reader_recv_frame` preserves
  the EOF-vs-error split and `client_mark_dead_locked(c, devgone)` threads it: a
  peer-gone EOF -> the device-gone `-P9_E_NODEV` terminal CQE (`T_E_NODEV` = POSIX
  ENODEV, the §10 `T_E_DEVGONE`-class, appended to `errno.h`), a transport error ->
  the unchanged `-P9_E_IO`. The Loom side is **unchanged** -- `loom_async_complete`
  already propagates the status to the CQE. So the device-gone terminal falls out
  of the SrvConn teardown for free: a driver group-terminated by a `DeviceRemoved`
  (step 5) EOFs its consumers' rings, and their in-flight Loom ops complete with
  `-ENODEV` -- **no warden pre-marking needed**. The explicit
  `p9_client_mark_devgone(c)` entry point is the secondary affordance (a
  device-teardown hook that HOLDS the client can proactively fail a doomed session;
  the deterministic test vehicle). Only the **async** path carries the reason -- the
  audited #841 sync surface + the boot path keep `-P9_E_IO`, zero regression.
  915/915 PASS (+2 `9p_client.async_{peer_gone,mark_devgone}_posts_nodev_cqe`; the
  existing `async_session_death_posts_error_cqe` re-confirms the transport-error
  `-EIO` leg); boot OK. Reference `docs/reference/107-loom.md` (Device-gone
  terminal). **Focused audit (one Opus-4.8-max prosecutor + a concurrent self-audit;
  MODEL start == end, no fallback) CLEAN 0 P0 / 0 P1 / 0 P2 / 4 P3** -- the two
  prosecutions cross-confirmed clean on misclassification (a clean EOF can never be
  misread as idle: the deadline backends reset `timed_out` on arm + return `-1`+
  timed_out never `0`+timed_out), the reply-vs-death exactly-once race (untouched
  discipline), lifetime (pure parameter), ABI (pinned, collision-free), the sync-path
  no-regression, and spec fidelity. The 4 P3s are all doc/coverage (F1 the explicit
  `p9_client_mark_devgone` has no production caller yet -> the warden #160 is the
  intended first; F2/F3 the spec abstracts the recv-classification + the per-site bool
  -> a `SPEC SCOPE` note + the 3-reader/10-transport site-count regression anchor; F4
  the trigger is broader than strict device-removal -> the `errno.h` doc broadened);
  closed list `memory/audit_loom_devgone_closed_list.md`. SMP gate (default+UBSan x
  smp4/smp8, N=10): PASS, 0 corruption (the 19 timing exits ground-truth-verified to
  reach boot OK + 915/915 + login prompt). **Next**: the warden + `libdriver` bind the
  existing virtio drivers on QEMU virt (step 5).
- **2026-06-15 (the warden confer-at-spawn allowance ABI + #160 revoke-on-terminate
  fold-in, build-sequence step 5a)**: the I-34 grant path wired to userspace. The
  warden's narrowing rides the existing rich spawn primitive `SYS_SPAWN_FULL_ARGV`
  (the A-1a append-only pattern): `struct sys_spawn_args` grows 80 -> 96 bytes
  (`allowance_va` / `allowance_flags` / `_pad_allow`) + a fixed 176-byte
  `struct t_allowance_desc` (mmio[8] windows + counts + irq[8] + dma_max), pinned
  identically across the kernel header, the libt mirror, and the libthyla-rs
  `TSpawnArgs` / `TAllowanceDesc` (`offset_of!` asserts). The grant is gated in the
  PARENT by `allowance_confer_within_parent` (the I-2 hardware-axis narrowing check,
  reusing `allowance_permits`: a broad warden may confer anything, a narrowed parent
  only a subset of its own) and conferred in the child spawn thunk BEFORE EL0 (the
  `proc_confer_allowance` set-once contract); a too-wide ask is a clean pre-fork
  `-1`. **#160**: `proc_revoke_allowance` folded into `proc_group_terminate`'s first
  step, so the warden's killgrp of a removed driver IS revoke-then-terminate
  atomically (the in-flight-create race closes universally; the step-3 "the warden
  must remember to pair them" caveat becomes structural). Userspace:
  `Command::allowance(TAllowanceDesc)` + the `push_mmio`/`push_irq`/`set_dma_max`
  builder. 917/917 PASS (+2 `allowance.{confer_within_parent,revoke_on_group_terminate}`);
  boot OK + 0 EXTINCTION. Reference `docs/reference/117-allowance.md` (the
  confer-at-spawn section). **Focused audit (one Opus-4.8-max prosecutor + a
  concurrent self-audit; MODEL start == end, no fallback) CLEAN after fixes -- 0 P0
  / 1 P1 / 1 P2 / 2 P3, ALL FIXED.** F1 [P1] the #160 fold-in broke
  `proc_confer_allowance`'s lockless `kfree(old)` "no concurrent reader" contract
  (a killer reaches the proc-tree-linked child in the spawn window ->
  `proc_revoke_allowance` locks the OLD inherited clone while the thunk frees it ->
  SMP UAF, on the narrowed-parent path; the v1.0 broad warden is safe via
  `old==NULL`) -> FIXED by `proc_allowance_install_locked` (the swap under
  `g_proc_table_lock`, the lock the revoke runs under; `kfree` outside). F2 [P2] the
  `fail-allowance` thunk arm leaked the inherited fds' spoor refs -> FIXED (clunk
  them, mirroring the fail-fd-install arm). F3/F4 [P3] non-atomic revoke load +
  test lock-doc -> FIXED. The spec is structurally blind to F1 (it models Confer as
  a set-assignment, no malloc/free) -- the class specs miss + audits catch. NOT a
  dirty close (0 P0, P1+P2 < 6, localized fixes). Closed list
  `memory/audit_5a_allowance_confer_closed_list.md`. SMP gate (default+UBSan x
  smp4/smp8, N=10): PASS, 0 corruption (pre-fix + a post-F1 re-run, the proc.c
  death-path witness). **Next**: 5b -- the `libdriver` framework crate.
- **2026-06-16 (the `libdriver` framework crate, build-sequence step 5b)**: the
  probe/bind/serve scaffold + the manifest schema (§6) landed as a native
  `libthyla-rs` crate, `usr/lib/libdriver`. **No new kernel ABI** -- pure userspace
  over the frozen 5a `Command::allowance` path + argv. Three layers (the
  kaua/netdev split): `manifest` (the §6 brace-block schema -- `Manifest` /
  `Needs` / `MmioNeed` / `IrqNeed` / `DmaNeed` / `Restart` + a tokenizer +
  recursive-descent parser + a binary-unit size parser + a `to_text` round-trip)
  and `resource` (`NodeResources` = what the warden reads from /hw; `BoundResources`
  = the narrowed grant; `resolve(manifest, node, instance)` = the node-INTERSECT-needs
  intersection, **the auditable I-34 grant in one function** -- the node *supplies*
  the MMIO/IRQ values, the manifest *selects* the axes, so a grant can never exceed
  the device; plus the single-argv-slot descriptor codec `to_descriptor` /
  `parse_descriptor`) are PURE and **host-tested** (19 tests: the §6 example parse +
  round-trip, malformed-manifest rejection, the resolve intersection incl. the
  grant-never-exceeds-node property + `NoMatch` + `TooManyWindows`, the descriptor
  round-trip + strict rejection + the delimiter-injection guard). The `driver`
  layer (feature-gated, the only libthyla-rs surface) is the `Driver` trait + the
  `run::<D>()` entry (bind -> probe -> serve -> a lifecycle exit code the
  supervisor reads) + the handle-mint helpers (`DriverVa` bump allocator over a
  private device-VA region clear of the netdev/PCI fixed VAs; `map_mmio` /
  `claim_irq` / `alloc_dma`, each drawing from the conferred, kernel-gated grant) +
  `to_allowance` (the warden's mirror: a `BoundResources` -> the `TAllowanceDesc`
  for `Command::allowance`, so the authority the kernel enforces and the resources
  the driver maps are one value) + a reference `NopDriver` (the compile-proof that
  `run::<D>` monomorphizes end to end in the device build). The conferred allowance
  is the authority; the descriptor only *informs* -- a driver that fabricated a PA
  outside its allowance is rejected by the kernel I-34 gate, not the codec.
  Reference doc `docs/reference/118-libdriver.md`. **Audit-light** (no kernel
  privilege surface -- the kernel validates every `SYS_*_CREATE`; a buggy driver or
  warden corrupts only its own view): self-audit + the 19 host tests + the device
  build + the whole-workspace device build + clippy-clean. **Next**: 5c -- the
  warden (the DTB source over /hw + the bind DB + the match -> `resolve` ->
  `Command::allowance` -> spawn engine), which is the first consumer of every
  `libdriver` surface.
- **2026-06-16 (the warden + the bind-loop proof, build-sequence step 5c)**: the
  hardware broker, `usr/warden` (native `libthyla-rs`), and the first live
  `impl libdriver::Driver`, `usr/menagerie-probe`. **No new kernel ABI** -- pure
  userspace over the frozen 5a `Command::allowance` + libdriver. The engine reads
  `/hw` (the devhw DTB tree) via the new pure `libdriver::dtb` decode (compatible/
  reg/interrupts -> `NodeResources`, host-tested against the real QEMU-virt
  bytes), matches a compiled-in bind database (`best_match`, most-specific
  `compatible` wins), `resolve`s the I-34 grant, and spawns the driver `/<name>`
  with the descriptor (`to_descriptor` -> argv) + the narrowed allowance
  (`to_allowance` -> `Command::allowance`) + `CAP_HW_CREATE`, both derived from the
  one `BoundResources` so authority and information cannot drift. The 5c bind DB
  is the pl061 GPIO (a single-instance, undriven, unreserved QEMU-virt device);
  `menagerie-probe` proves the loop -- it maps its granted MMIO (the allowance
  permits the grant) AND verifies an out-of-grant create is rejected (I-34
  enforced) -- then exits, and the warden reaps it. Proven in joey's
  `THYLA_BOOT_PROBES` ladder: `45 /hw nodes discovered -> bind arm,pl061 ->
  1 bound, 1 up -> joey: warden ok -> Thylacine boot OK`, 0 EXTINCTION, + the SMP
  gate (default + UBSan x smp4/smp8). One notable bring-up find: a boot-probe Proc
  has no stdio fds, so `Command`'s default `Stdio::Inherit` (which bumps the
  parent's fd 0/1/2) fails before the kernel even resolves the binary -- the
  warden hands each driver `/dev/null` for the three slots (the driver logs via
  the console-direct `t_putstr`). Reference docs `docs/reference/119-warden.md`
  (new) + `118-libdriver.md` (the `dtb` module). **Audit-light** (no kernel
  privilege surface; the kernel validates every `SYS_*_CREATE`): self-audit + 27
  host tests + the boot-probe E2E + the SMP gate; the composed focused audit
  (grant path + warden grant-decision + discovery-source trust) is **5f**.
  **Next**: 5d -- retrofit `usr/lib/netdev` to be a spawnable `impl Driver` the
  warden binds NARROWED (the live I-34 exercise; retires `virtio.rs:51`'s
  hardcoded base).
- **2026-06-16 (the discovery-source layer pulled forward; build-sequence
  resequence -- design decision, no code)**: the proper realization of §3 promoted
  into step 5. 5c's warden does *static DTB-walk* discovery; binding the QEMU-virt
  virtio-net device exposed that the DTB is **type-blind** to a virtio-mmio slot's
  device (all ~32 slots are `compatible = "virtio,mmio"`; only the runtime
  `DeviceID` register identifies the net device). Rather than hack the
  type-blindness into the warden (read `DeviceID` in the TCB, or spray ~32
  self-selecting driver spawns), the proper fix is to build §3's pluggable-source
  layer *now* (the depth-first dependency: build the foundation, don't hack the
  parent around its absence; user-directed 2026-06-16, "pull it forward without
  hesitation"). The resequenced **5d** is the **discovery-source layer**: **(5d-1)**
  the `DiscoverySource` / `DeviceNode` / typed-`DeviceId` abstraction in `libdriver`
  + the `DtbSource` refactor + bind-by-id; **(5d-2)** the **virtio-mmio source** as
  a *separate, capability-sandboxed Proc* (granted only the bank, reads `DeviceID`,
  re-emits typed `virtio:<id>` nodes to the warden over a pipe -- so the warden
  stays **hardware-free** and the `DeviceID`-poke is isolated in a single-purpose
  enumerator); **(5d-3)** `netdev` -> a grant-driven `impl Driver` the warden binds
  by `virtio:1` (retire `virtio.rs:51` + the bank-probe loop); **(5d-4)** the
  composed focused audit (the source-trust boundary joins §25.4) + close.
  **Foundational, not throwaway**: the source-Proc + reporting-channel
  infrastructure is exactly what the PCIe/USB sources (step 6) reuse, and net-2's
  `netd` is then born discovery-driven. §3/§5/§7/§15/§16 + ARCH §22.7 updated to
  match. **Next**: 5d-1.
