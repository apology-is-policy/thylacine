# Halcyon / Carbon Optics — implementation kit

Version 1.0 · 2026-09-14 · Target: native Thylacine Halcyon.

This is an implementation contract, not a second design exploration. Reproduce the Instrument Panel mockup's geometry, color, typography and interactions, with Carbon Optics as the new default. Preserve Halcyon's process isolation, authority boundaries, rich transcript semantics and terminal compatibility.

## Start here

1. Read `IMPLEMENTATION-SPEC.md`, then `MIGRATION-GUIDE.md`.
2. Read `THEME-CONVERSION.md` before installing any palettes.
3. Use `AGENT-START.md` as the implementing agent's first message.
4. Open `prototype-offline.html` in a browser. It defaults to Carbon and includes all 13 themes. No server, build, or network is required for interaction. Fonts are not bundled: install IBM Plex Sans and IBM Plex Mono for a typography match; fallback fonts are NOT a pixel-accuracy reference.
5. Use `ACCEPTANCE-TESTS.md` as the release gate, not an optional checklist.

`manual.html` is a self-contained, searchable, printable text edition of the documents. Markdown files are the agent-oriented originals. `reference/` contains the unchanged HTML/CSS/JS from the existing mockup. `source-docs/` contains all nine supplied Halcyon documents, unchanged. The new specification does not edit those historical sources.

## Deliverable map

| File or folder | Purpose |
|---|---|
| IMPLEMENTATION-SPEC.md | Native model, exact metrics, renderer ownership, states, interaction contract |
| MIGRATION-GUIDE.md | Ordered implementation slices, explicit policy amendments, rollout and rollback |
| THEME-CONVERSION.md | Current-schema mappings, missing roles, lossless extension and runtime switching |
| palettes/*.toml | 13 complete current-schema theme files; compatibility projections |
| ui-palettes/*.toml | 13 exact 35-color companions for the NEW Instrument profile loader |
| instrument-profile.toml | Proposed profile configuration: shared geometry/typography/behavior, not a stock theme |
| resolved-tokens.json | Lossless resolved CSS colors for every theme |
| PALETTE-REGISTER.md | Human-readable exact colors and Carbon syntax contrast |
| contrast-report.json | Contrast measurements, including preserved weak pairs |
| ACCEPTANCE-TESTS.md | Pixel, behavioral, parser, authority, lifecycle and regression gates |
| fixtures.json | Deterministic reference scenario and interaction expectations |
| scripts/build_bundle.py | Reproducible extraction/conversion/checking/packaging; Python 3.11+ |
| SHA256SUMS / VALIDATION.json | Integrity and honest test-status record |

## Authority and limitations

Pinned mockup commit: `074bc5646b2f7a03872859890c2891017ceaaf9d`, existing Site version 5. Its files were present in this conversation's workspace. The native OS repository and executable were NOT provided. Native source paths below are grounded in the attached documents, but the implementing agent must verify their current symbols and ownership before modifying them. No native build, guest lint, screenshot parity or OS functional match is claimed by this kit.

The old 57-key format cannot encode the entire mockup. Its bevel minimum is 2; it couples workspace padding to gap; it has no upper rail or distinct focus-neutral, code-ground, terminal-path and several syntax roles. Therefore the compatible TOMLs alone are NOT sufficient and are deliberately labeled projections. Pair them with the new UI profile and exact color companions after implementing that loader. Do not put a sidecar or `instrument-profile.toml` into the old `theme.toml` slot: unknown keys are supposed to be refused.

The offline convenience page differs from the frozen source only by inlining assets, removing remote font loading and using a separate Carbon-default theme preference. It has no real terminal backend or file editor. The native implementation must supply those capabilities through the existing Halcyon architecture.

Rebuild/check: run `python3 scripts/build_bundle.py` from anywhere. It reads the frozen reference, regenerates all palettes, reports limitations, checks key completeness against the attached template, and creates the archive next to this folder. It never installs files into the OS or changes the live Site.
