# Thylacine Operator's Manual

This index describes the *Thylacine Operator's Manual*: whom it is written for, how its source is organised, how it reaches a running system, and which sections exist. The manual itself is the set of section files under `docs/manual/`.

---

## Readers

The manual is written for an operator who is comfortable with commands, files, processes, and configuration, but who does not yet know Thylacine's architecture or terminology. A section serves two readers at once: one who needs a working invocation, and one who wants to understand the mechanism behind it.

The technical reference in the vault (`vault/system/`) is written for the developers of Thylacine itself. The manual describes the same system from the operator's side and is never authoritative on internal semantics; where the two disagree, the vault and the code decide.

---

## The writing guide

Every section is written to `docs/thylacine-operators-manual-writing-guide.md`, which is binding for all manual prose. The guide fixes the shape of a section (an untitled opening, then **In Practice**, then **Technical Details**), the reference voice, the language to avoid, and the requirement that every command, option, default, and error be verified against the current tree. Read it before drafting or revising a section.

---

## Source, installation, and rendering

Each section is one Markdown file, `docs/manual/NN-<name>.md`. The number orders the sections for sequential reading and does not appear in any heading; the name is what a reader types to open the section. Operating-system topics are numbered below 40; ported applications start at 40.

The source is installed in the operating system under `/manual` and read with the `manual` command, which emits Beacon markup (`docs/BEACON.md`). Under Halcyon a section is rendered with its headings, tables, and preformatted blocks; on a serial console or through a pipe the same section reads as plain text.

`docs/MANUAL-DESIGN.md` is binding for the source format, the reader, and the installation. It defines the Markdown subset a section may use: every accepted form has both a Beacon and a plain-text realization, and anything else is rejected by `manual --check`. The reader and the installation are built before further sections are written (operator decision, 2026-09-16).

---

## Sections

No section is installed yet. The first sections, in the order they are being written:

1. Utopia: the shell language, with examples; the native utilities; and how familiar `sh` syntax is written in Utopia.
2. Imperium.
3. Vivarium.
4. Containers.
5. Alpine: each of its utilities in a subsection with an example, its `sh`, and a note on `git`.
6. Haul.
7. View.
8. Gallery.
9. The bundled games, Quake, GLQuake, VkQuake, and DOSBox-X, with how to launch and configure each.

The writing order is not the book order; each section is numbered by the rule above.

Three pages written before the guide are kept as drafts in `docs/manual-drafts/`, which is neither checked nor installed. A draft returns to `docs/manual/` when it has been rewritten to the guide and passes `manual --check`.

| Draft | Subject | State |
|---|---|---|
| `manual-drafts/00-overview.md` | Overview | A Phase-0 plan (2026-05-04) that describes intended phases rather than the current system. |
| `manual-drafts/40-dosbox.md` | DOSBox-X | Written 2026-09-05 to the earlier page template. |
| `manual-drafts/41-audio.md` | Audio (Nocturne) | Written 2026-09-05 to 2026-09-07 to the earlier page template. |

---

## Maintenance

A section is added when the facility it describes has settled, so sections appear one at a time as deliberate work. Once a section exists, a change to the user-visible behaviour of its facility updates that section in the same commit, and the update follows the writing guide.

---

## Revision history

| Date | Change | Reason |
|---|---|---|
| 2026-05-04 | Scaffolded as `docs/USER-MANUAL.md` (Phase 0). | Index and page template for a user manual. |
| 2026-09-05 | Revived; added the `/manual` plan and the first chapter (`40-dosbox.md`). | Operator decision; supersedes the 2026-05-31 deferral to v1.0-rc. |
| 2026-09-16 | Renamed to `docs/OPERATORS-MANUAL.md`; the writing guide adopted and the earlier page template withdrawn; the reader and `/manual` installation to be built before further sections. | Operator decisions. |
| 2026-09-16 | `docs/MANUAL-DESIGN.md` adopted (source format, the `manual` reader, installation); the three earlier pages moved to `docs/manual-drafts/`. | Operator sign-off: nothing is installed until it is written to the guide. |
| 2026-09-16 | The first sections and their writing order set, starting with Utopia; Containers remains a section of its own. | Operator decision. |
