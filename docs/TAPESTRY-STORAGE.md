# Cooperative hidden surface storage

Binding scope approved October 4, 2026, option 1 of
HALCYON-HIDDEN-WEAVE-REVIEW.md. This is the protocol and implementation contract;
implementation and qualification are still pending. I-40 and the existing
Tapestry ownership, placement, event and resize contracts continue to apply.

## Purpose and bounds

Hidden terminal tabs retain semantic state but need not retain pixel buffers.
Halcyon already uses the growable ThylaAlloc private heap. HI1-R30 instead hit
PROC_SHARED_MAP_MAX_PAGES: 30387 existing pages plus 2613 requested pages exceeds
32768, or 128 MiB. The kernel returned a generic map refusal, not a diagnosed
private-heap allocation failure. No panic or compositor crash occurred in that
run. The new surface was unwound; existing tabs survived. A visible explanation
for admission failure is also owed.

Keep the current shared-map ceiling, surface/pane/fid/event bounds, sixteen PTYs
and all service quotas. This extension creates no kernel role, syscall, mapping
exemption or clipboard authority. It adds bounded metadata per existing surface
and generation stamping within the existing 160-byte per-fid reservation. If
that reservation cannot hold the fields, revise the layout, not the bound.
Unsupported clients retain the existing lifecycle. Initial adoption is Halcyon
terminal content; chrome, menus, status/rail and external GL sources are excluded.

## Wire contract

A client writes `storage 1` to an owned, created content surface's ctl before
opening its weave/present fids. This is an irreversible opt-in for that surface
incarnation. Repeat version 1 is harmless; unknown versions, wrong roles or an
external GL source are refused. Enabling storage and later GL adoption are
mutually exclusive. No existing create or tpresent encoding changes.

TEV_STORAGE is tevent kind 13, retaining the 24-byte tevent record. `code` is
1 for Suspend, 2 for Resume; `value` and `rune` are the low/high halves of a
nonzero u64 offer token. `mods` and `flags` are zero; `tick` retains its normal
meaning. Only opted-in surfaces receive it. Unread storage state coalesces to
the latest offer, within the existing queue. A stale already-delivered event
is harmless because the ctl verb revalidates the token and current visibility.
Offers and pixel generations never wrap. Exhaustion refuses further storage
transitions rather than reusing an identity.

The client acknowledges with `storage suspend TOKEN` or `storage resume TOKEN`.
The token is decimal u64. Only the current matching offer is actionable. A
visibility reversal invalidates it. Stale/wrong-state acknowledgements return
E_AGAIN and have no side effects. Authority remains the existing owned ctl;
a token is a freshness check, not transferable authority. No unbounded retry,
pending RPC or additional thread is introduced by this extension.

`storage abort TOKEN` discards an unpresented resume generation after mapping
or setup failure. It is accepted only for the exact resume token, before any
successful full present. It cannot discard another/newer generation. This
leaves the semantic surface dormant. One failed resume attempt is reported per
visible episode; retry follows a subsequent hide/reveal or explicit user retry,
not every FRAME tick. The protocol must not allocate repeatedly on an idle loop.

Each weave and present fid captures the current pixel generation at open.
For opted-in surfaces every geometry read, Tweft request and present validates
that generation and a resident weave. Old fids never resolve to new pixels.
An already-mapped old fid still refers only to its original pinned mapping
until clunk. After any reweave, including ordinary resize, clients reopen BOTH
weave and present fids. Legacy clients keep their existing resize behavior.
This uses 9P fid incarnations instead of extending the tpresent header.

## State and ordering

The server has one placement authority: surface_target plus session
backgrounding determines visibility. Focus, lack of FRAME ticks and client
assertions do not determine visibility. Visibility is reconciled before offers;
a new layout can supersede an offer before its acknowledgement.

A storage client has Resident, Dormant and Repainting storage states. A pending
Suspend offer leaves it Resident and drawable. A pending Resume offer leaves
it Dormant. Surface identity, owner, event/ctl handles, hosting leaf, terminal
model, job and dimensions survive every storage transition. SurfState's initial
creation state must not be confused with storage residency.

Suspend ordering:

1. Tapestry offers suspension only when its placement authority says hidden.
2. The client stops borrowing/writing pixels and completes its submitted
   presents. Halcyon's single renderer owner and synchronous present path
   provide that ordering; a future asynchronous client must explicitly drain.
3. The client sends Suspend while retaining its old mapping. Server dispatch
   rechecks offer, hidden state, lack of GL adoption and backend-use guards.
   Refusal leaves the current mapping usable. There is no unmap-before-ack race.
4. Accepted suspension invalidates pixel-fid admission before retiring the
   current and at most one displaced generation. A generation still bound to
   scanout cannot be detached here. Existing completed-transfer/composition and
   Lictor retirement rules govern all backend references. Held damage and
   shown-slot history are cleared. No displayed reference names retired pixels.
5. After successful Rwrite the client closes weave and present fids, clearing
   all pixel pointers/ages before drawing can resume. The mapping pin protects
   backing until close even if the server retired its own reference first.

Reveal ordering:

1. Tapestry offers Resume when the same live pane is visible and dormant.
2. Resume rechecks the token/visibility and allocates one fresh generation at
   the latest valid target dimensions. Allocation failure keeps the pane and
   its terminal state; there is no destroy/close of the semantic surface.
3. Fresh fids read geometry and map that generation. Partial setup failure
   closes new fids, then Abort retires only that unpresented generation.
4. The renderer updates its pixel geometry and PTY size if necessary, resets
   buffer ages/rotation, and fully repaints the retained model. Until a complete
   unheld present succeeds, no stale shown-slot or uninitialized image is used
   by prefill/direct/composed presentation. Server admission requires full
   damage for this first frame. Newly allocated backing is zeroed as usual.
5. Successful full presentation makes the fresh generation drawable normally.
   A hide during repaint may offer suspension again; every operation retains
   exact generation ownership. At most current plus displaced generations
   exist, as in the existing resize contract.

Hidden resize updates the desired semantic geometry without allocating pixels.
Resume uses the current target, not a stale CONFIGURE retained in a queue.
Old CONFIGURE serials must not resize the new generation. Same-size redraw
notifications still invalidate the model's paint cache. Repeated visibility
changes coalesce; an obsolete acknowledgement cannot free visible pixels.

## Backend lifetime proof

Invisible is not a release fence. Tapestry's current normal present dispatch
completes transfers/composition synchronously, and its bound_res check prevents
retiring scanout. Lictor separately pins imported backing. Device retirement
requests require quiescence; a failed detach retains its pin; an unref may be
deferred, and Device::reap drops backing only after a completed retirement
journal entry. Closing the client or Tapestry mapping cannot override those
pins. An ambiguous device result can retain storage, never authorize early reuse.

The proof has four independent holders: client mapping, Tapestry generation,
backend DMA/import work and display reference. Physical pages are reclaimable
only after all holders retire. Storage acknowledgement is not a claim that
physical memory has already been freed. A future Pi/other display backend must
supply the same completion/retirement contract; a QEMU success is not hardware
qualification. No new backend-specific shortcut is permitted here.

## Progress, failure and interaction

Slow clients keep their charged mappings and existing bounded event behavior;
Tapestry never waits for their acknowledgement. Lictor/clipboard SAK cancellation
continues on the already-independent control owners, including if normal display
RPCs park. Do not move cancellation back onto a render thread. SAK does not imply
hidden-buffer suspension and must not recycle tokens or reopen retired handles.

Halcyon keeps processing terminal output and job lifecycle while dormant, keeps
its paint dirty bit, and never calls pixels/present on absent mappings. Dormant
or failed-reveal tiles must not forward blind text input to a program before its
first repaint; compositor global navigation remains usable. Paint a bounded
compositor-owned placeholder for an awaiting image and a visible session notice
on allocation/mapping refusal. A failed reveal leaves the job alive and can be
retried by switching away and back. New-tab allocation refusal also needs a
visible explanation. Never silently shrink a tab to 1x1 or cap the pressure
fixture below the failing workload to claim the repair.

Close/logout follows ordinary retire, including either resident or dormant
state. The event/ctl identity outlives temporary pixel absence but not actual
surface retirement. Connection death cannot leave a resumable orphan. This is
not process stop/resume and creates no second transcript/model owner.

## Verification and delivery

The graphics lifecycle is a spec-first re-enabled surface. Before code, model
visibility reversal, stale fid/offer, mapping/backend retention, resume failure,
full-frame publication, termination and finite token exhaustion. Retain existing
tapestry_present invariants and named counterexamples; compose the storage model
with their release obligations rather than weakening I-40. Production state
helpers and dispatch tests must exercise the same transitions, with intentional
mutants that fail the named obligations. Record single-agent self-review honestly.

Native acceptance must repeat the original full-size tab pressure within the
unchanged 128 MiB ceiling, up to the existing sixteen-PTY bound; inspect all
hide/reveal repaints, output received while hidden, geometry changes, stale
fid/offer refusal, close/logout, allocation refusal and physical F10 restoration.
Retain source and paired image manifests, failed runs and screenshots. Audit
legacy clients/media/manual as well as the opted-in terminal path. Full required
SMP/sanitizer gates precede qualification/Main landing; focused WIP checkpoints
must disclose unrun gates. HI1-R30 is not closed by this design commit.
