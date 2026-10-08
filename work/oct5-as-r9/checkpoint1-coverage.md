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
| charge refused (`addrspace_charge_pages` over the space's cap) | `addrspace_private_end` releases BOTH the ring count and the lifetime reference | UNRUN: leg "refused charge leaves no guard, reference or charge", with a confined RED mutant (`refusal-leg-run.sh`); arms 18/18 off-lease |
| ring layout allocation fails (`loom_create_layout` returns NULL after the charge) | uncharge `metadata + backing`, then `addrspace_private_end` | STRUCTURAL; runtime unwind OPEN. No allocation fault seam exists in `kernel/`. A minimal, test-only, locally scoped seam is owed for review after the charge-refusal leg (astra t67). |
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
