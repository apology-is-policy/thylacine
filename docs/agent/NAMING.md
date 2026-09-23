# Thematic naming

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). 
> Section headings keep their original levels.

## Thematic naming — keep an eye out

Thylacine names things. Where a function, file, mechanism, or concept would otherwise carry a generic Unix/POSIX-shaped name, **look for a fitting thylacine-related word** that conveys the same meaning. The project's identity is a marsupial apex predator declared extinct in 1936 (and a Plan 9 lineage given a similar narrative); the naming should reflect that wherever it adds clarity or color without sacrificing communicative intent.

Examples already in use:
- **`extinction()`** for kernel panic / "panic level event" (ELE = Extinction Level Event). Function in `kernel/extinction.c`; `EXTINCTION:` is the agentic-loop ABI prefix.
- **Thylacine** — the OS itself.
- **Stratum** — the filesystem (a record preserved in layers, geological stratigraphy).
- **Halcyon** — the graphical shell (the calm before; the impossible return).
- **janus** — the key agent (two-faced; the boundary between worlds; from Stratum).

Sources to draw from:
- **Marsupial / dasyuromorph biology**: torpor (deep-sleep state), pouch / marsupium, joey, lineage, taxon, clade, crepuscular (active at twilight), nocturnal.
- **Thylacine specifics**: the wide-jaw display, the striped pelt, the high-pitched yip-bark vocalization, the Tasmanian bushland habitat (eucalypt, spinifex), the disputed late-20th-century sightings (cryptozoology / Lazarus species).
- **Apex-predator behavior**: stalk, ambush, hunt, run.
- **Extinction / rediscovery**: lazarus (a species presumed extinct then rediscovered), specimen, holotype, last known.
- **Plan 9 lineage** — already saturating the architecture (namespace, bind, mount, 9P, factotum-pattern, Dev, Chan). Don't double up; don't rename Plan 9-derived concepts.

Discipline:
- **Propose, don't unilaterally rename load-bearing identifiers.** Tooling ABI surfaces (`Thylacine boot OK`, `EXTINCTION:`), public function names already documented in reference docs, and cross-project surfaces (anything Stratum-aligned) require explicit signoff before renaming. The `panic → extinction` rename in P1-C set the precedent: user proposed mid-chunk; we coordinated the change across `kernel/`, `tools/test.sh`, `TOOLING.md`, `CLAUDE.md` in a single commit.
- **Hold for explicit signoff**:
  - `_hang` (the WFI halt loop) → `_torpor` candidate; held.
  - Audit prosecutor agent → potential rename to "tracker" / "hunter" candidate; **held with preference for "prosecutor"** — Stratum already uses the term and cross-project continuity matters more than thematic novelty.
- **Don't force it.** Some things should keep their standard name because the standard name is what readers expect (e.g. `mmu_enable`, `dtb_init`, `uart_putc`, `boot_main`). The bar: a thematic name should add clarity OR color without obscuring intent. If the rename makes the code less obvious to a reader who doesn't know the project's identity, the standard name wins.
- **Document the choice.** When a thematic name lands, the reference doc for the affected subsystem (`docs/reference/NN-*.md`) gets a short "naming rationale" paragraph. See `04-extinction.md` for the pattern.

When you spot a candidate while implementing — note it in the chunk's commit message or `phase1-status.md` trip-hazards as a held proposal. The user has explicitly invited more thematic suggestions ("don't be shy"); respond by surfacing the option, not by silently renaming.

---
