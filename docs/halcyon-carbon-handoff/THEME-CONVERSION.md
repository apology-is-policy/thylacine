# Theme conversion and the lossless Instrument palette contract

## 1. Two files, deliberately different jobs

Each of the 13 themes has `palettes/<id>.toml` and `ui-palettes/<id>.toml`.

The first uses exactly the supplied TEMPLATE.toml shape: metadata name plus57 required keys, including one16-entry ANSI array. No `base`; no new tables; no floating numbers; all colors opaque six-digit RGB. It is a **current-schema compatibility projection**. This kit verifies shape and syntax against the attachment, not against a native Halcyon executable that was not provided. Run the real `halcyon theme lint` before installation.

The second is a **new proposed schema**, `instrument-v1`, with 35 exact resolved CSS colors. It carries color roles the old `Theme` cannot express. Do not run the legacy loader on it, rename it `theme.toml`, or silently ignore its fields. Build the new loader first. The profile's fixed geometry, fonts and effect composition are separate from both color files.

This avoids two incorrect approaches: losing distinctions until Carbon looks merely similar, or stuffing unknown fields into an old strict parser that must reject them. The directory split is also a rollout safety boundary: the old gallery bake currently takes every `*.toml` in its theme directory. Sidecars must not be placed there and accidentally advertised as stock themes.

## 2. All themes and defaults

| Stable ID | Display name | Family |
|---|---|---|
| signal | Signal Amber | Dark |
| carbon | Carbon Optics | Dark; new default |
| abyssal | Abyssal Sonar | Dark |
| oxide | Oxidized Archive | Dark |
| combine | Combine Relay | Game-inspired dark |
| deusex | Deus Ex Access | Game-inspired dark |
| shock | System Shock Node | Game-inspired dark |
| sin | SiN Network | Game-inspired dark |
| mesa | Black Mesa Lab | Game-inspired dark |
| strogg | Strogg Process | Game-inspired dark |
| genera | Genera Ivory | Light |
| mineral | Mineral Sage | Light |
| logic | Warm Logic | Light |

Names are descriptive inspirations, not official game palettes. All colors are the already-designed mockup values, not new attempts to sample copyrighted game UIs.

Fresh Instrument selection is carbon. Preserve a user's explicit stored theme. An unknown persisted theme ID resolves to carbon with a diagnostic; it does not crash or silently route to Signal. Existing legacy-profile users remain on their explicit legacy preference until they select Instrument or the operator rolls out the new default. Define migration defaults once, in the profile selector, not differently in the session and compositor.

## 3. Exact stock mapping

| Stock key | CSS role / derivation | Fidelity |
|---|---|---|
| palette.floor | desktop | Exact value |
| palette.surface | open | Expanded document ground |
| palette.header | header | Exact value |
| palette.raised | hover | Compatibility reuse; browser has no pills |
| palette.border | structure | Compatibility structural stroke |
| palette.blank | pane | Empty pane interpretation |
| palette.selection | 15% amber over open | Derived opaque approximation to source-over |
| palette.island_rule | amber-muted | Exact code gutter color |
| palette.fg | text | Primary UI ink |
| palette.fg_dim | body-text | Secondary body tier |
| palette.fg_muted | secondary | Secondary chrome tier |
| palette.fg_subtle | dim | Faint chrome tier |
| palette.ember | amber | Deliberately theme-specific |
| palette.ember_dim / ember_deep | amber-muted | Second legacy role has no distinct mockup equivalent |
| palette.status_bg | rail | Same top/bottom rail ground |
| palette.status_fg | text | Compatibility primary status |
| palette.status_muted | secondary | Actual normal footer ink |
| palette.status_idle | dim | Compatibility idle indicator |
| terminal.bg | terminal-bg | Deliberately different from surface |
| terminal.fg | terminal-text | Deliberately different from palette.fg |
| type.smooth | dark0 / light12 | Halcyon smoothing convention, not browser measurement |

For both `[palette.sage]` and `[palette.cinnabar]`: key=success/error respectively; tint=header; raised=hover; border=separator; fg=text; fg_dim=secondary; fg_muted=dim. Keeping the tints neutral avoids inventing entire green/red header backgrounds absent from the mockup. Stock chrome still draws its old shapes and may apply its old focus rules; only the Instrument profile stops those treatments and uses the exact sidecar roles.

Four legacy bevel faces are generated together around desktop: top=16%white over desktop, left=9%white, right=22%black, bottom=46%black. This gives a coherent brightest-to-darkest ordering, not exact mockup samples: the mockup has NO bevel. The existing minimum bevel remains2 in the compatible files. A native Instrument frame uses the new1px flat-frame metric and does not consult this legacy bevel width. Never alter one face to approximate the neutral focus frame.

Geometry projection: bevel2, gap 3, hairline1, header_h32, status_h25, tag_pad_x0, tab_strip_h0. Gap3 preserves external workspace padding but cannot reproduce an independent7px divider; top 34 is absent entirely. These are evidence of why a palette install is not the finished migration.

### Syntax mapping into existing role names

The old hue-like names are semantic slots; do not map green to green merely because both are called sage.

| Existing role | Instrument semantics | Exact Carbon |
|---|---|---|
| slate | keyword | #C7B98B |
| sage | type | #8EA4B8 |
| sand | attribute/macro projection | #B58B70 |
| moss | number/constant projection | #A693AD |
| ash | function | #91AA98 |
| dusk | string | #B99A7B |
| smoke | comment | #77807C |
| fen | diagnostic success | #819B85 |
| cinnabar | diagnostic error | #BD7770 |

The old schema has no dedicated lifetime or punctuation slots. The sidecar adds both, plus exact base-code ink. The old `slate` is also object-reference/info and `sand` is member/warning; do not change every semantic consumer because the new editor maps attributes there in compatibility mode. In Instrument code, use the explicit `syntax_*` roles; retain separate object and status semantics. An old nora highlighter using the 9 legacy roles is a usable intermediate but cannot reproduce all reference token categories.

### Terminal defaults and the R2-F1 regression trap

Carbon has palette.surface #121516 and palette.fg #F2F3EF, but terminal.bg #090C0D and terminal.fg #CBD0CC. This is LEGAL according to the supplied template and subsequent audit fix. A terminal cell that has no explicit color is compared with the terminal tier's defaults, never with the UI tier. Theme switching must not turn default cells into parchment/black boxes or break dim/object color hooks. Tests must build a sheet and a terminal pen with deliberately different default pairs.

The compatibility files contain an authored ANSI16 extension because the mockup did not define one. Slot order is conventional black,red,green,yellow,blue,magenta,cyan,white and bright variants. Base slots are pane,error,success,amber,terminal-path,syntax-number,50/50 terminal-path+success,secondary; bright black isdim; bright chromatic colors mix28% toward primary text; final white uses terminal-text. Duplicate RGB values are deterministically incremented by one RGB integer until unique. This is traceable conversion scaffolding, not a claim that ANSI blue must become the theme's chosen path hue in every app. Visually review ANSI black/default-black behavior on dark grounds and yellow on light themes before deploying. Never change explicit24-bit app colors to force the palette.

## 4. Lossless sidecar schema

Grammar intentionally fits Halcyon's small no_std TOML parser family, but needs its own key registry:

```toml
[meta]
schema = 1
id = "carbon"
name = "Carbon Optics"
profile = "instrument-v1"
color_scheme = "dark"

[color]
desktop = "#050607"
# ... all 35 keys, as shipped in ui-palettes/carbon.toml ...
```

Every field required, no inheritance in v1, unknown keys/tables refused, duplicate keys refused. ID is `[a-z][a-z0-9_-]{0,31}`; name has the same presentable Unicode and 64-byte cap as stock theme names; profile literal must match supported version; scheme exactly dark/light; schema integer1. File max16KiB, color keys exactly the 35 in resolved-tokens.json; colors exactly#RRGGBB, no alpha, aliases or CSS expressions. Validate at the privileged consumer too; parsing successfully in the session does not confer authority.

Parser results are immutable `InstrumentColors`; common `InstrumentMetrics` and `InstrumentTypeMap` come from a validated compiled profile or its bounded configuration file. Do not let a theme select executable shaders, paths to fonts, command strings, arbitrary icon fonts, network locations or display scale. `instrument-profile.toml` is a proposed install/build input for that shared profile registry, NOT a new file the old runtime already knows how to load.

Suggested installation AFTER implementation:

- Legacy-compatible gallery: `/lib/halcyon/themes/<id>.toml`.
- Exact companions: `/lib/halcyon/instrument-palettes/<id>.toml`.
- Per-user companions, if supported: `$HOME/lib/halcyon/instrument-palettes/<id>.toml`.
- Profile selection: a new validated session preference resolving `{profile:"instrument-v1", theme:"carbon"}`; exact path/wire format must be registered with repository configuration owners.

Do not install these new locations and imply current binaries consume them. Until the new loader exists, only the first directory has documented meaning.

## 5. Atomic resolution, theme switch and persistence

Keep stock `Theme::resolve(system,user)` fallback policy: invalid user file emits a diagnostic and tries system; invalid system tries built-in. The early design prose saying malformed always paints Daylight is superseded by TH-4a. Do not weaken the parser to make an incomplete dark theme load.

The Instrument loader resolves a **bundle**, not two unrelated current-theme filenames. Registry maps one ID to stock and sidecar files, parses both, checks matching names/ID and internal consistency, builds the profile, and stages one new visual generation. A malformed component rejects the whole candidate bundle. Startup tries the next complete valid tier; runtime selection leaves the current bundle active and reports failure. Never combine user sidecar with a different system theme because one half failed. Carbon may be compiled as the Instrument fallback while the stock legacy loader retains Daylight; these are different profiles with explicit owners, not contradictory fallbacks.

Runtime picker parity is NEW work. The old theme design explicitly does not guarantee live reload. Existing `theme <wire>` pushes the stock fields and cannot carry all 35 colors plus new profile metrics as if they already existed. Design a versioned Instrument visual transaction under the same declared+hosting seat gate, with a bounded payload and generation ID. Minimum transaction contract:

1. Session parses/stages both files, validates geometry and all assets, derives legacy terminal/export values, allocates resources without mutating the live bundle.
2. Session requests compositor application under its existing seat authority. Compositor validates independently and either refuses without mutation or commits the staged revision.
3. Compositor publishes geometry/profile revision and fans redraw even for color-only changes. All visible clients learn the new visual generation; stale-height rail surfaces are retired/reminted through existing lifecycle rules.
4. Session applies matching colors/type/metrics before painting that revision; all layout and glyph caches with changed scale/weight/face are invalidated. Color-only changes need repaint, not necessarily glyph rerasterization.
5. Avoid mixed frames: retain the prior coherent frame until matching revision surfaces are ready, then swap. A client cannot keep the display locked indefinitely: bounded deadline/recovery uses the existing scene fallback and reports the lagging surface.
6. Update future-spawn palette export and notify participating existing applications through a versioned cooperative channel. Existing process environments are not mutable broadcasts. Nonparticipating truecolor apps retain their explicit colors; report that limitation rather than respawn or kill them.
7. Persist selection through the repository's durable atomic-write idiom only after successful application. Failure to save is visible; live selection may remain active but is marked unsaved. No theme click is permission to delete user files or reset a session.

Busy/backpressure is retriable with the existing bounded event-loop cadence, not a spin or flattened generic Protocol error. E_PERM is terminal and never a retry loop. Same bundle hash is a no-op except optional focus restoration; a change to only syntax or status ink still requires repaint. Commit and abort must remain correct across compositor/session disconnect and logout.

This is a mechanism specification, not an invented current ctl command. Add the protocol to the correct ABI registry with positive/negative authorization tests. Do not rename the existing theme wire fields or increase their size without updating both endpoints and the exhaustive structural guard that TH-6 introduced.

## 6. Contrast and no accidental improvements

`contrast-report.json` measures intended pairs using sRGB relative luminance. Carbon's syntax set passes4.5:1. Some existing other themes' inherited dim/comment/number colors do not. This kit preserves them because conversion is supposed to match the UI, not quietly redesign12 themes. Report those warnings in theme lint/QA without declaring all themes accessibility-compliant. A high-contrast variant can be a separately named later theme. Likewise, matching13 themes does not mean every one shares the same contrast ratio or polarity.

The authoritative extraction is the CSS cascade. Earlier handoff `themes.json` lists only a subset of surfaces and should not seed this port. `resolved-tokens.json` includes rail, pane border, focus-neutral, body/code/terminal/dialog/kbd grounds and all syntax roles, with inherited CSS variables resolved separately under each theme.
