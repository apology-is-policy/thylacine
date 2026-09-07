# The diorama — the synthetic Linux world [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-diorama-doc-absorb`).
`usr/diorama` (`/sbin/diorama`) — a read-only 9P server that re-presents
Thylacine's native introspection (`/proc`, `/ctl`) in the shapes an unmodified
Linux binary expects (Linux's `/proc` and `/sys`). The *world* half of Phase 8's
Linux-compat pole; the *ABI* half is the phenotype. Its content lives,
code-verified and current — and in several places **ahead of this doc** — in one
audit:hard dossier:

    vault/system/userspace/services/sub-diorama.md   (audit: hard, I-43)

The dossier carries, as-built:

- **The defining rule** — the diorama renders only from sources the calling Proc
  could already reach natively; it is a reformatter, never an authority
  ([[inv-i43]] made structural, not review-dependent). Both corollaries (never
  source through a path the client couldn't use; never accept a client-supplied
  answer, a client-named pid being the canonical mistake).
- **The deputy-as-authority boundary (§6.2)** — why `/self/environ` is sound
  (target is the connection's own peer) while `/<pid>/environ` would leak (this
  server is `PRINCIPAL_SYSTEM`, so it could read any SYSTEM Proc's environ and
  hand the bytes to a client of any principal) and is therefore absent.
- **The `self` resolution** (the connection peer = the mounter, so the tree
  belongs in a per-container territory) and **the two modes** — the default
  `/srv/diorama` and the `--vivarium <runner-pid>` V-7 mode (`/srv/viv-dio`,
  ppid-descent membership, the attach gate against the first-come-service
  cross-mount, fail-closed on a bad argument).
- **The renders** — the `/proc` tree, the `/proc/sys/kernel` sysctls, the `/sys`
  cpu tree (online/possible/present + cache line size), `/proc/stat` +
  `/proc/cpuinfo`, and the `maps` Linux-column translation.
- **The whole V-4c-3 audit close** — the msize-underflow-terminates-the-server P2
  (saturating subtraction), the walk-by-name-vs-enumeration P2 (the cache subtree
  that resolved but did not `readdir`), the checked/saturating parser arithmetic,
  the `#71` walk-into-a-file gate, and the `#72` environ-is-a-window fix.

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The dossier is AHEAD of this doc on V-7.** This reference treats the vivarium
  mode as future ("that containment question is owed at V-7"); the dossier
  describes it as built — the `--vivarium` mode, `/srv/viv-dio`, the ppid-descent
  membership, the attach gate, and the `#182` finding that the `/ctl/procs` read
  buffer is half the kernel's. Read the dossier, not this doc, for the current
  containment story.
- **Two P3 code-deltas folded** (`chg-2026-09-07-diorama-doc-absorb`; `updated:`
  2026-08-15 -> 09-07, clearing a stale-flag on +324 lines of post-dossier code):
  the **SA-4 vDSO clock fast-path** (`clock_pair_ns`/`render_uptime` now take the
  #343 vDSO page via `libthyla_rs::time` instead of raw `t_clock_gettime`, and
  derive both clocks from one counter sample) into Performance, and the
  **MIDR-0x00 legitimate-zero + harness lesson** into Caveats (a green `test.sh`
  on HVF `-cpu host` is not a sufficient gate for a hardware-register assertion —
  the interactive TCG `-cpu max` harness reports a different `MIDR_EL1`).
- **`diorama-probe` was an UNOWNED orphan** the dossier's proof story relies on
  (its selftest + `/bin/diorama-probe` gate every mechanism, boot-fatally). Added
  to the code list.
- **Everything else was covered** — the qid families, the bounded renders, the
  read-only-at-open, the constants-exception (osrelease 6.1), the accept-loop
  decline, and the selftest-gates-the-boot proof position are all as-built in the
  dossier. Zero code change.
