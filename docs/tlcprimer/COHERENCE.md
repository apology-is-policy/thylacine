# Thylacine OS — Coherence Review

**Purpose**: An honest audit of whether the architectural decisions form a coherent
whole, or whether they are disparate ideas bolted together. Written from the
perspective of a skeptical reviewer who has read the full ARCHITECTURE.md and is
asking: does this system have a soul?

**Verdict up front**: Yes, with one caveat. The design is coherent. The soul is 9P.
Every major decision either derives from 9P or serves it. The one thing that needs
watching is the handle/VMO layer — it is the right idea but its relationship to 9P
needs to be stated explicitly so it does not drift into becoming a separate paradigm.

---

## 1. The core thesis

Thylacine's coherence rests on a single proposition:

> **9P is not a filesystem protocol. It is a universal composition mechanism.**

If you accept this proposition, everything follows. If you don't, the design looks
like a collection of interesting parts. The proposition needs to be internalized, not
just stated.

In Plan 9 this proposition was half-realized — 9P was everywhere but authentication,
graphics, and some device interfaces still escaped it. In Thylacine the proposition
is meant to be total. The coherence review is asking: is it actually total? Where
does it leak?

---

## 2. The features, examined one by one

### 2.1 Namespace model ✓ — core expression of the thesis

Private per-process namespaces composed via `bind` and `mount` are the direct
expression of "9P is composition." Every resource is a 9P server; every process
composes its view of the system by mounting the servers it needs. Containers are
namespaces. Sandboxes are namespaces. The `/dev/` tree is a namespace. There is
no separate container runtime, no separate sandbox API, no separate device
registration system. One mechanism.

**Coherence**: total. This is the thesis made concrete.

### 2.2 Userspace drivers as 9P servers ✓ — correct derivation

If 9P is the composition mechanism, drivers are just servers you mount. A graphics
driver exposes `/dev/fb/`; you mount it. A network driver exposes `/net/eth0/`; you
mount it. The driver is not special — it is a 9P server that happens to hold hardware
handles. This follows directly from the namespace model.

The performance concern (userspace drivers are slow) is answered by the handle
model (hardware access is direct, no kernel in the hot path) and the VMO model
(data movement is zero-copy via mapped memory). The 9P protocol carries control
messages, not bulk data. Bulk data moves through VMO-mapped memory. This is the
right decomposition.

**Coherence**: total. Drivers as 9P servers is not a bolt-on; it is the thesis applied
to hardware.

### 2.3 Halcyon as the graphical environment ✓ — unusual but coherent

Halcyon does not escape the 9P model. It mounts `/dev/fb/` (a 9P server), reads
frames from it, writes pixels to it. It mounts `/dev/video/player/` (a 9P server)
to play video. It mounts Stratum (a 9P server) for the filesystem. It launches
subprocesses over pipes (9P streams). The graphical scroll-buffer model is rendered
by writing to a VMO-backed framebuffer that the GPU driver also holds — zero-copy
by construction.

Halcyon is not a windowing system. It is a 9P client that renders output. That is
the correct identity for it.

**Coherence**: total. The graphical shell is not a foreign body; it is a 9P client
whose output channel happens to be pixels instead of text.

### 2.4 Stratum as the native filesystem ✓ — designed-in fit

Stratum is already a 9P server. Its administration surfaces are 9P file trees. Its
`janus` key agent is a 9P server. Its per-connection namespace model maps to
Thylacine's per-process namespace model. The kernel mounts Stratum exactly the
same way it mounts any other 9P server. There is no special Stratum kernel module
at v1.0 — it is just a server.

The fact that Stratum was not designed for Thylacine and fits perfectly is not
coincidence — it is because both are built on the same underlying proposition.

**Coherence**: total. The strongest evidence for the thesis is that Stratum plugs in
without any adapter.

### 2.5 Handle/VMO model ⚠ — correct but needs explicit grounding

This is the one area that could drift into incoherence if not carefully framed.

The handle model (typed kernel object handles with rights) and VMO model (kernel
object representing a memory region) are borrowed from Zircon. They are good ideas.
But Zircon uses them as the *primary* IPC and resource model — handles are the
fundamental abstraction in Fuchsia, more fundamental than files or 9P.

In Thylacine, the relationship must be explicitly inverted:

> **9P is primary. Handles are the mechanism by which 9P servers access hardware.**

Handles are not visible to regular programs. Regular programs see only 9P file trees.
Handles are the implementation detail that makes userspace drivers performant —
a driver holds MMIO/IRQ/DMA handles so it can access hardware directly, then
exposes the result as a 9P server. The 9P interface is the public contract; the handle
is the private mechanism.

VMOs are slightly more visible — they can be transferred over a 9P session for
zero-copy buffer passing. But even this is: a 9P message carries a VMO handle as
an out-of-band attachment. The 9P session is still the composition mechanism; the
VMO handle is cargo it carries.

**Coherence**: sound if the subordination is stated and enforced. Risk: if handle
transfer becomes a general-purpose IPC mechanism separate from 9P, you have two
composition mechanisms instead of one. Mitigation: VMO handles are only
transferable via 9P sessions, by design. No other handle transfer path.

**Action**: ARCHITECTURE.md §18 should explicitly state: "Handles are not a
general-purpose IPC mechanism. The only way to transfer a handle between processes
is via a 9P session. 9P remains the sole composition primitive."

### 2.6 Async 9P pipelining ✓ — amplifies the core

Pipelined 9P requests are not a new idea layered on top of 9P — they are 9P used
correctly. The tag field exists for exactly this purpose. Making the client exploit it
is not adding a feature; it is removing an unnecessary constraint. This is the right
framing: Thylacine's 9P client is less broken than a naive implementation, not
more complex.

**Coherence**: total. This is the thesis working at full capacity.

### 2.7 Per-core discipline ✓ — orthogonal, not in conflict

The per-core SMP discipline (per-CPU data by default, cross-core communication as
explicit IPIs) does not touch the 9P model. It is a kernel implementation discipline
that makes the scheduler and interrupt routing correct. It is coherent with everything
else by being beneath the level where 9P operates.

The long-horizon multikernel direction (per-core kernel instances communicating
via 9P) is where the connection to the thesis becomes explicit: if you go full
Barrelfish, the cross-core IPC becomes 9P. That is elegant and worth noting, but
it is not a v1.0 concern.

**Coherence**: orthogonal at v1.0, convergent in the long run.

### 2.8 POSIX surfaces as 9P servers ✓ — the thesis applied to compat

The POSIX compat design principle — every POSIX surface is a 9P server — is the
thesis applied to the compatibility layer. `/proc` is a 9P server. `/dev/pts` is a
9P server. The cons/tty device is a Dev implementation that happens to serve
`termios` semantics over a file interface. POSIX programs see a POSIX-shaped
world; underneath it is all 9P.

This is the correct approach because it means the compat layer has no special
kernel machinery — it is just more 9P servers. Adding a new POSIX surface is
adding a new server, not modifying the kernel.

**Coherence**: total. POSIX compat is not a foreign body; it is the thesis applied
to a legacy interface.

### 2.9 Formal verification / TLA+ ✓ — methodology, not a feature

TLA+ model checking is not an architectural feature — it is a development
methodology applied to the load-bearing invariants. It does not interact with 9P
or the namespace model; it verifies them. The `poll` wait/wake state machine, the
`futex` atomicity invariant, the scheduler IPI protocol — these are specified before
implementation and verified against the spec.

This is the Stratum methodology applied to the OS. It is coherent by inheritance.

**Coherence**: orthogonal. Not a feature, a practice.

---

## 3. What is genuinely novel vs. assembled

A useful way to stress-test coherence: separate what Thylacine invents from what
it assembles.

**Assembled from Plan 9** (proven ideas, correctly applied):
- Namespace model, `bind`/`mount`, per-process namespaces
- `rfork` as the universal process/thread primitive
- Dev vtable, Chan, devtab dispatch
- Notes as the internal signal model
- 9P as the universal protocol
- Factotum pattern → janus

**Assembled from elsewhere** (good ideas, subordinated to 9P):
- Typed handles with rights → Zircon, seL4 (subordinated: handles are private to
  driver processes, transferred only via 9P)
- VMOs for zero-copy sharing → Zircon (subordinated: VMOs are cargo in 9P messages)
- Per-core SMP discipline → Barrelfish (subordinated: beneath the 9P layer)
- Async 9P pipelining → already in the 9P spec, just correctly used

**Genuinely novel combinations**:
- Graphical scroll-buffer shell (Halcyon) as the sole UI — no windowing system
- POSIX compat surfaces as 9P servers rather than kernel special cases
- Stratum (PQ-encrypted, formally verified, COW FS) as the native filesystem
- Userspace drivers via handle-based hardware access + 9P interface exposure
- The development loop: QEMU + 9P host share + agentic CI

None of the assembled pieces conflict with each other because they are all
subordinated to 9P. The handles enable drivers; the drivers expose 9P servers;
the namespace model composes the servers; Halcyon mounts the composition. It is
one idea expressed at multiple levels of the stack.

---

## 4. The things that are genuinely not here

Coherence also means knowing what you are not. Thylacine is explicitly not:

- **A microkernel in the seL4/Mach sense**: the kernel is not minimal for its own
  sake. It is minimal because 9P enables userspace to do what kernel code would
  otherwise do. The motivation is different.
- **A capability OS in the seL4/Capsicum sense**: handles have rights, but they are
  not the primary security boundary. The namespace is the primary security boundary.
  Handles enforce hardware access control within driver processes. The distinction
  matters.
- **A distributed OS**: 9P works over a network but Thylacine does not manage a
  cluster. The distributed transparency is available; it is not the design center.
- **A research OS**: the formal verification work is real but the goal is a usable
  system, not a proof object.

These non-identities are coherent. Thylacine knows what it is.

---

## 5. The one genuine tension: simplicity vs. feature accumulation

The Oberon principle — "what can be left out?" — is the right counterweight here.

Over the course of the design sessions, the following were added:
- Typed handles (§18)
- VMOs (§19)
- Per-core SMP discipline (§20)
- Async 9P pipelining (§21)
- Hardware platform model / DTB discipline (§22)
- Full POSIX surface specification (§23)

Each of these is justified. But the cumulative effect is a system that is no longer
small. The kernel alone must implement: namespace, scheduler, VM, 9P client,
handle table, VMO manager, IRQ forwarding, poll, futex, signal delivery, PTY
infrastructure. That is a real kernel, not a toy.

This is not incoherence — it is scope. The scope is correct for the stated ambition
("not a toy, a peer of ZFS and btrfs" for Stratum; the equivalent for the OS). But
it must be named honestly: Thylacine is not a minimalist OS. It is a coherent OS
with a minimalist philosophy applied to the interface layer, not the implementation.

The test for any future addition: **does it reduce the number of mechanisms, or
does it add one?** Handles reduced the mechanism count (replaced "privileged
process" with explicit typed grants — a more precise version of the same thing).
VMOs reduced the mechanism count (replaced ad-hoc shared memory with a
first-class kernel object). Any future addition should pass the same test.

---

## 6. Verdict

Thylacine is coherent. The soul is 9P. Every major decision either:

1. **Is 9P** (namespace model, IPC, driver exposure, POSIX surfaces, Stratum
   integration, Halcyon's interface)
2. **Enables 9P to perform** (handles give drivers direct hardware access so the
   9P interface is not a bottleneck; VMOs make 9P zero-copy for bulk data; async
   pipelining makes 9P fast under load)
3. **Is beneath 9P** (per-core SMP discipline, formal verification, DTB-driven
   hardware discovery)

Nothing in the current design contradicts 9P or requires reasoning outside the
9P model. The POSIX compat layer is 9P servers wearing a POSIX costume. The
graphical shell is a 9P client whose output is pixels. The filesystem is a 9P server
with cryptographic integrity. The drivers are 9P servers with direct hardware access.

The one thing to actively protect: handle transfer must remain a 9P-mediated
operation, not a separate IPC primitive. If that invariant holds, the design stays
coherent indefinitely.

---

## 7. Recommended ARCHITECTURE.md amendment

Add the following to §18.1 (Kernel object handles and capabilities):

> **Subordination invariant**: handles are not a general-purpose IPC mechanism.
> The only channel through which a handle may be transferred between processes is
> a 9P session (as out-of-band metadata on a 9P message). No syscall exists for
> direct handle transfer between processes. This invariant ensures 9P remains the
> sole composition primitive in Thylacine. Any design that would require handle
> transfer outside a 9P session is a signal that a 9P interface is missing, not
> that the handle transfer restriction should be relaxed.
