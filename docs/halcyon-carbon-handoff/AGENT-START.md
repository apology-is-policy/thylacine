# Paste this into the implementing agent

Implement the native Halcyon Instrument UI from this kit, matching the pinned interactive mockup exactly, with Carbon Optics as the default and all 13 themes available. This is an implementation task, not a new design exercise.

Read README, IMPLEMENTATION-SPEC, THEME-CONVERSION, MIGRATION-GUIDE and ACCEPTANCE-TESTS before making changes. Read the attached historical Halcyon documents where they define the existing architecture. The new visual contract explicitly supersedes old Daylight geometry/type/focus conventions only in the Instrument profile. Security, per-user identity, compositor ownership, isolated kaua-term, CPU-floor rendering, bounds, SAK and transport invariants remain binding.

Inspect the actual repository and record its commit and baseline tests. The kit was written from documents and mockup sources, not a native source checkout. Verify every mentioned symbol/path before editing. Follow repository change controls and preserve unrelated user changes.

Do not claim a palette swap can implement the full UI. `palettes/*.toml` are complete stock-schema compatibility projections. `ui-palettes/*.toml` and instrument-profile.toml define NEW inputs that require a strict versioned loader/profile. Never put them into the old theme.toml slot, ignore unknown fields, loosen the parser, or fake zero bevel with invalid geometry. Preserve all 35 exact CSS color distinctions.

Use one compositor-owned topology and geometry model. A mockup pane is an ordered stack of live tiles; each keeps its own process and state, only one body is expanded, all headers remain in stable order. No duplicate renderer-side layout tree pretending to be authoritative. A collapsed body must be dormant while its stack-owned header stays visible and interactive.

Implement the ordered I-0..I-8 migration slices. Keep legacy profile rollback available. Pin exact Plex font assets before claiming type parity; Cornucopia substitution is a visible deviation, not an invisible refactor. Keep the real terminal/Beacon/selection pipeline and make the lambda prompt semantic. Implement highlighted Rust as text, not a screenshot. Code examples are fixture fragments, not OS implementation code.

The reference simulator copies sample content on split and loses scroll on rerender. Do NOT clone a pts or destroy state. Apply only the explicit production safety exceptions in spec§11: new distinct process on split, dirty-close/reset confirmation, retained scroll/selection, resource/minimum bounds and accessible controls. Do not expand into multiple workspaces or executable header pills merely because older proposals mention them.

Capture exact-source browser reference states and native framebuffer states at matched logical viewport/scale/fonts. Run the full acceptance matrix, real native theme lint and guest security/behavior tests, with negative controls. Host-only success is not compositor pixel evidence. Report limitations honestly and never alter the oracle to hide implementation drift.

Final handoff must include changed source/commits, all 13 working native themes and exact profile tokens, source/font hashes, screenshot/diff evidence, actual test results, any approved exceptions, and safe rollback instructions. Continue until the native UI and real session behavior satisfy the contract; if repository access, assets or a policy approval is missing, identify that precise blocker instead of substituting an approximation.
