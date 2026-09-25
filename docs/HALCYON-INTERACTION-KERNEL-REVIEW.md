# Terminal interaction kernel review

September 25, 2026. Single-agent source review and tests under the operator's
explicit direction; no independent adversarial review is claimed. This reviews
the kernel foundation, not the unimplemented clipboard broker or app workflows.

The approved contract is HALCYON-INTERACTION-PTY-ABI.md. Implementation starts
from Aux's cleared `0cb5b244`, merged on Astra as `c252a7f3`.

## Admission and lifecycle

BIND's native front holds the master and observer Spoors through identity capture
and the combined check. It resolves the registered master side and derives the
observer from the real service transport, never a supplied process ID. The
backend holds lifecycle before pts, checks the binder's current seal/taint and
both live incarnations, and refuses replacement of an existing live binding.
The future host spawn path still must establish the seal before user execution.

ACK/CHECK are observer-only. An acknowledged epoch is insufficient by itself:
CHECK freshly compares live subject, controlling session, foreground group,
principal and exact nomination in the same critical section. Subject zero is an
APP acknowledgement and cannot pass CHECK. Identical ACK makes no revision or
readiness change. No CHECK result is an application bearer credential; matching
one request and graphical focus remains the future Tapestry/broker obligation.

Every successful foreground mutation advances the epoch, even redundant seating.
Nominated process membership/exec changes advance it too: clearing only subject
would allow a delayed old ACK to revive the same stripes after image replacement.
Binder/observer death/exec retires before process publication; terminal free/GC
retires before terminal reuse. Fork alone neither transfers role nor registers a
new controller. Process pointers are borrowed only while lifecycle is held.

## Watch lifetime and concurrency argument

The pool is bounded even when clients retain retired watcher fds. Reserving one
of two role slots precedes allocation; failure or retirement during allocation
releases that reservation. Publish rechecks role/liveness. A live binding with
no watch still occupies a slot until explicit/lifecycle retirement. ID and epoch
exhaustion stop interaction admission rather than wrapping into another identity.

A poller holds a Spoor reference until its waiter is unregistered. Register and
revision sample run under pts; a writer either precedes that sample (readiness
is seen) or follows registration (the registered waiter is woken). Before dropping
pts, a writer pins the binding across its post-unlock wake. A concurrent unbind,
last close or new bind cannot recycle that list until the pin is dropped.
The poll callback never sleeps or allocates. Wake takes poll-list then rendez;
no path acquires process lifecycle from under pts/list/rendez.

Dups share the watcher and cursor. Inherited descriptors still check the actual
calling incarnation. Generic navigation clones clear COPEN, so they cannot read,
poll or release the original reservation. No namespace attachment or additional
/srv slot exists. A short read does not consume state; a user copyout fault may
consume a revision and recovers through STATE. Private watch operations cannot
be reached by passing their numbers to the native syscall front.

## Verification boundaries

The phase status owns exact logs and results. Kernel tests exercise roles,
foreground epochs, setpgid/setsid, principal mismatch, APP refusal, watch readiness,
clone/inheritance refusal, bounded retained capacity, and actual image replacement
and death. PTY/poll clean and named-mutant models check their existing mechanisms;
there is no new terminal-ownership model claim.

The expanded qualification boot adds 256 concurrent close/unregister/rebind versus
retirement iterations, WATCH handle-allocation rollback, all three counter boundaries,
and real devsrv/dev9p provenance fronts. The actual native marshaller is called
with invalid user mappings; copy faults do not nominate a subject. Boundary injection
and marshaller access are KERNEL_TESTS-only, without a native diagnostic operation.
An empty-pool guard prevents resetting IDs while any retired borrower remains.
The fixture joins its worker and reclaims resources before reporting a failure.

The first frontend test exposed a fixture error: the old registry-only helper
supplied server_stripes zero. A real observer needs the posted server incarnation;
the test now supplies it, preserving the production refusal. That failed boot and
the corrected 1698/1698 boot are both retained. The older syscall-gate fixture also
stopped overwriting proc_alloc's existing handle table, removing a test-only leak.

The complete repeated matrix now passes all 50 boots (default smp1/4/8 and
UBSan smp4/8, ten each), with zero exception buckets. That closes the repeated
kernel stress obligation for 5ad9ad27. The subsequent full production shape and native EL0 observer probe both pass
with test-only symbols absent. They establish BIND/STATE/WATCH/UNBIND,
readiness/retirement, spawn seals and binder ACK/CHECK refusal without test seams.
Positive observer ACK/CHECK through Tapestry remains an integration obligation. The user-facing clipboard additionally requires all host,
Tapestry, broker and app integrations listed in HALCYON-INTERACTION-STATUS.md.

## Userspace readiness constraint discovered during integration tracing

The kernel watcher limit is not the whole readiness budget. Tapestry can add 32
watchers within its 64-fd poll bound (2 + 8 + 32), but Halcyon's approved terminal
and broker counts reach 72 even before pending writes. The existing main-loop
append order caps writes only, not broker input. This blocks a direct increase
of PanePlaceServer's connection count. HI1-Q2 in the status owns the bounded
userspace aggregation design; the current two-connection service is unchanged.
