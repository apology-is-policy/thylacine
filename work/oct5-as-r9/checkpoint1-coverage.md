# Checkpoint 1 coverage map -- private owner construction, admission, bounded tables, metadata charges

Handoff section 2, checkpoint 1: "cover all refusal/unwind edges before
publication". This map exists so geometry is not mistaken for the whole
checkpoint (astra, yip 0161 t67). Four states, never merged:

- **DRIVEN**: a guest test executes the edge and asserts its outcome.
- **STRUCTURAL**: the code is present and read; no runtime witness. A runtime
  obligation stays OPEN, and this is NOT closure or a pre-activation waiver.
- **NOT YET REACHABLE**: the owning mechanism is not implemented on this branch.
  The obligation travels with that implementation.
- **UNRUN**: authored, compiled, no guest result yet.

Cited by symbol. Fixture = `kernel/test/loom_private_fixture.h`
(`loom.private_owner_lifecycle`).

## Construction and admission (`loom_create_private`)

| Edge | Unwind on refusal | State |
| --- | --- | --- |
| invalid geometry (`loom_measure`) | none needed; refused before guard and charge | DRIVEN: "bad geometry has no guard or charge" |
| image not exclusively owned (`addrspace_private_begin`, owners != 1) | none needed; the guard is not taken | DRIVEN: "shared image refuses private owner" |
| charge refused (`addrspace_charge_pages` over the space's cap) | `addrspace_private_end` releases BOTH the ring count and the lifetime reference | DRIVEN, with RED witnessed (2026-10-08, run `refusal-leg-20261008T083538Z`): control 1836/1836 with the leg PASS in its own block; the one-site mutant FAILs that leg at "refused charge leaves no guard, reference or charge" and nowhere else. Mac axis, one boot each |
| ring layout allocation fails (`loom_create_layout` returns NULL after the charge) | uncharge `metadata + backing`, then `addrspace_private_end` | DRIVEN, RED witnessed for BOTH halves (2026-10-08, run `layout-leg-20261008T115213Z`): control 1836/1836 with the leg PASS; M1 (uncharge deleted) FAILs it at "layout failure returns the charge", M2 (private_end deleted) at "layout failure releases the guard, reference and owner", each the only FAIL; KERNEL_TESTS-off shape: no seam symbol, loom_create_private identical incl. all 13 relocations. Mac axis, one boot each. Built with a `KERNEL_TESTS` one-shot seam (arm/disarm/armed, taken on entry to `loom_create_private`; scope approved by astra t71) and a leg with SEPARATE charge and guard assertions, RED by two independent mutants (`layout-leg-run.sh`); arms 21/21 off-lease. Isolation precondition: the fixture is the only caller of `loom_create_private` -- rescope the seam before any concurrent or engine caller. `loom_create_layout`'s OWN internal failure path is a SEPARATE obligation: the next two rows. |
| ring Burrow refused INSIDE `loom_create_layout`, after the Loom metadata was allocated | `kfree(l)` in the layout itself, THEN the caller's uncharge + `addrspace_private_end` | UNRUN (authored 2026-10-08; boundary accepted by astra t79 after her t77 corrections). A `KERNEL_TESTS` one-shot KEYED TO THE ARMING THREAD, with a non-NULL CAS taken after the metadata allocation, plus a single-slot `KERNEL_TESTS` watch on `kfree`'s validated LARGE branch (the Loom is over 2048 bytes; the watch records ENTRY to the free site, not the buddy outcome). The leg runs a same-class self-check first, then asserts the refusal, the spent shot, the watched metadata, the charge and the guard/ref/owner, with the ORACLE "the inner ring failure frees the unpublished Loom" LAST in the leg. Runner `inner-leg-run.sh`: M1 deletes `kfree(l)` and must fail ONLY the oracle; it leaks one large kmalloc, intended and NEVER reclaimed (unobserved is not owned). M2 blinds the watch and must fail ONLY the self-check, so the inner leg does not run there. Shape: `kfree` + every emitted Loom constructor compared with relocations. Oracle arms 21/21 off-lease. |
| Loom metadata `kmalloc` refused inside `loom_create_layout` | none inside the layout (nothing allocated yet); the caller's unwind is the same NULL at the same call site | STRUCTURAL, by bounded statement (astra t77/t79): no locally acquired Loom to release, and the caller-side NULL unwind is the one the layout-failure row above DISCRIMINATED. This is NOT runtime closure of every allocation failure. |
| retirer not ready (`service_retire_ready`) | none needed; refused first | STRUCTURAL: boot-order guard, unreachable after boot |
| private-ring count saturation (`private_rings == ~0`) | none needed; the guard is not taken | STRUCTURAL: needs 2^32 - 1 live rings on one image |

## Metadata and backing charges

| Item | State |
| --- | --- |
| ring backing + one descriptor page + Loom metadata charged BEFORE publication | DRIVEN: "ring and metadata charged before publication", against `burrow_backing_pages` |
| exact image and creator captured, no Proc pointer kept | DRIVEN: "exact image and permanent creator captured" |
| refund at retirement from the settled drop (final and nonfinal) | DRIVEN, plus the off-lease 5-row double (`private-retire-matrix.log`) |

## Bounded tables

| Table | Source of the bound | State |
| --- | --- | --- |
| SQ/CQ/receipt geometry | `loom_measure` | DRIVEN (invalid geometry refused) |
| legacy setup on a private owner (registered handles, buffers, SQPOLL, enter) | refused outright | DRIVEN: "private owner refuses legacy execution" |
| destination slot reservation before a fid-creating op | ASYNC-SERVICE-LIFECYCLE.md, operations table | NOT YET REACHABLE: no private scope engine |
| fid/scope/target slots sharing the 64-entry registered-handle envelope | ASYNC-SERVICE-LIFECYCLE.md, same section | NOT YET REACHABLE |
| bounded request/completion storage (preallocated protocol storage, worker tickets) | committed foundation | NOT YET REACHABLE from a private owner: the admission/rollback ownership arrives with the engine |
| provided-buffer pool memberships (`loom_pool_prepare` / `_publish` / `_rollback`) | committed transactional core | NOT YET REACHABLE from a private owner; the same |

## Kept separate, on purpose

- The retirement's RELEASE sensitivity stays OPEN. Its structural pairing is not
  runtime evidence, and the charge-refusal leg does not replace it (t65, t67).
- Engine-dependent checkpoint 2 obligations (terminal publication, pool/payload
  reuse, `loom_post_pool_cqe`'s missing `service_closing` check) stay with the
  engine.
- The settled-drop conversion at `loom_private_destroy` is done and stays closed.
