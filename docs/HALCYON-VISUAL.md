# HALCYON-VISUAL — Daylight

Thylacine's graphical-shell visual identity scripture. Binding for Halcyon and
for every program that draws chrome on Thylacine's own framebuffer. Consumed by
`libhalcyon::theme`; the tag-bar and pane compositor read their values from here
and nowhere else.

**STATUS**: PROPOSED — H-1 chunk, Daylight only.

**Companion**: UTOPIA-VISUAL U-2 (*Bonfire*). Bonfire lights the Utopia terminal
at night; Daylight is the calm day around it. The two are not alternatives —
they meet inside every pane that hosts a console, where Bonfire's `#0e0c0c`
appears unchanged inside Daylight's chrome. Daylight never recolours a terminal
surface. It surrounds one.

**Deferred to H-2**: *Frutiger Aero*, the second Halcyon theme. Its structure is
identical to Daylight's; only the palette differs. Nothing in §2–§6 is
theme-specific and all of it carries forward unchanged.

---

## 1. The palette — *Daylight*

Named for the quality of the surface: `#f2ebe0` is paper under an overcast sky,
not white and not cream. Where Bonfire is a room lit by a distant fire, Daylight
is the same room with the shutters open. The warmth is still there — every value
in the theme carries a red bias — but it is ambient rather than sourced.

### 1.1 Ground

Five steps from the floor the panes rest on to the strokes drawn across them.

| Role | Hex | Description |
|---|---|---|
| `floor` | `#8a7660` | Workspace floor. Visible only in the 2px gaps between panes. |
| `surface` | `#f2ebe0` | Pane surface. Parchment. |
| `header` | `#cec4b6` | Tag bar background; also the inner hairline. |
| `raised` | `#bdb0a0` | Command pill background. |
| `border` | `#a89880` | Explicit strokes, tag bar separators, the cast shadow. |

The floor value is not free. It is the perceptual midpoint between the bevel's
lightest and darkest edges, and it exists so that both edges have somewhere to
go. A floor near the surface value loses the highlight; a floor near black loses
the shadow. `#8a7660` was chosen by sampling the midpoint and then warming it
until it sat in the same family as the surface it surrounds.

### 1.2 Text scale

Four steps, matching Bonfire's structure. Hierarchy is communicated by
recession, never by weight change.

| Role | Hex | Description |
|---|---|---|
| `fg` | `#1a120a` | Primary text, command lines, prose output. |
| `fg_dim` | `#3a2e22` | Trailing tag-bar metadata, secondary output. |
| `fg_muted` | `#6a5a48` | Tag names, prompt paths, inactive chrome. |
| `fg_subtle` | `#9a8878` | Indent guides, decorative brackets. |

### 1.3 Accent

The ember is inherited from Bonfire at full strength. It is the one value shared
verbatim between the two scriptures, and it is what makes a Halcyon workspace
and a Utopia terminal recognisably the same system.

| Role | Hex | Description |
|---|---|---|
| `ember` | `#e07840` | Prompt `⊢`, caret, running indicator, active workspace. |
| `ember_dim` | `#b85f2a` | Pill stroke on an active tile. |
| `ember_deep` | `#c86030` | Separator under the active tile of a resting pane. |

### 1.4 Live-tile keys

The tile holding input takes one of two keys, selected by the exit status of its
last command. This is the only place in Halcyon where a colour carries state,
and it is deliberate: the marker that says *where you are* and the marker that
says *whether it worked* are the same marker, so neither can be missed while
attending to the other.

**Sage — exit 0, or nothing has run yet.**

| Role | Hex | Description |
|---|---|---|
| `sage` | `#1e5844` | Separator, content hairline, active pill. |
| `sage_tint` | `#b8ccc4` | Tag bar background. |
| `sage_raised` | `#a6bdb4` | Pill background. |
| `sage_border` | `#86a096` | Vertical rule, muted pill stroke. |
| `sage_fg` | `#0c2820` | Tag name. |
| `sage_fg_dim` | `#14342a` | Trailing metadata. |
| `sage_fg_muted` | `#33604f` | Muted pill text. |

**Cinnabar — last exit non-zero.**

| Role | Hex | Description |
|---|---|---|
| `cinnabar` | `#982818` | Separator, content hairline, active pill. |
| `cinnabar_tint` | `#dcb8b0` | Tag bar background. |
| `cinnabar_raised` | `#d0a89e` | Pill background. |
| `cinnabar_border` | `#b88c80` | Vertical rule, muted pill stroke. |
| `cinnabar_fg` | `#3c1008` | Tag name. |
| `cinnabar_fg_dim` | `#521a10` | Trailing metadata. |
| `cinnabar_fg_muted` | `#7a4034` | Muted pill text. |

Sage was chosen over the warmer candidates because it is cool against a warm
surround and therefore separates without raising its voice, and over the other
cool candidates because it is not already spoken for. Cinnabar is already the
error colour in Bonfire; reusing it here costs nothing and means a Thylacine
user learns one association rather than two.

Two states only. A console has more conditions than two — running, never-run,
warning-only — but two states are readable in peripheral vision and four are
not. Warnings do not promote a tile to cinnabar; the exit code is the whole
rule.

### 1.5 Syntax colours

Light-shifted from Bonfire §1.4, same names, same semantic roles. These apply to
content Halcyon renders itself. Content rendered inside an embedded terminal is
Bonfire's business, not Daylight's.

| Name | Role | Hex |
|---|---|---|
| `slate` | keyword / info / object reference | `#3a4878` |
| `sage` | type | `#1e5844` |
| `sand` | member / warning | `#7a5020` |
| `moss` | constant | `#3a5818` |
| `ash` | function / identifier | `#6a3828` |
| `dusk` | string | `#4a3868` |
| `smoke` | comment | `#6a7060` |
| `fen` | success | `#1e5828` |
| `cinnabar` | error | `#982818` |

`sage` and `cinnabar` are shared between the syntax layer and the live-tile
keys, as `sand` and `slate` are shared in Bonfire. The same reasoning applies:
the palette stays small, and a live tile's chrome and a type annotation in
source do not occupy the same visual frame.

### 1.6 The embedded terminal

When a tile hosts a console, the console's surface is Bonfire, unmodified. The
values are reproduced here for reference only; UTOPIA-VISUAL U-2 is canonical.

| Role | Hex |
|---|---|
| `bg` | `#0e0c0c` |
| `surface` | `#2a1f1c` |
| `fg` | `#e4ddd8` |
| `fg_subtle` | `#5a4e48` |
| `path` | `#9a8f8a` |
| `ember` | `#e07840` |

Daylight's chrome frames this surface and never enters it. A live tile hosting a
console gets its sage or cinnabar hairline on the *outside* of the terminal
rectangle. Nothing inside changes.

---

## 2. Light

Halcyon has one light source. It is fixed, it is the same in every theme, and
every edge value in the specification derives from it.

**The light comes from the north-north-west.**

Not from the north-west. The 45° diagonal is the Win95 and Motif convention, and
it produces a bevel with two values: top and left identical, bottom and right
identical. NNW is roughly 22° off vertical, which gives four distinct values —
the top face is nearly perpendicular to the light and takes the most, the left
face is grazed and takes less, the bottom faces fully away, and the right face
catches a little bounce.

The gain is small and it is real: the top edge becomes the brightest line in the
chrome, which reinforces the top-weighted reading that the tag bars already
establish.

### 2.1 The bevel

| Edge | Hex | Description |
|---|---|---|
| `bevel_top` | `#f8f2e6` | Key light, near-perpendicular. |
| `bevel_left` | `#e2d6c0` | Grazing incidence. |
| `bevel_right` | `#362410` | Facing away, some bounce. |
| `bevel_bottom` | `#221405` | Fully shadowed. |

Width: **2px**, uniform, on every pane.

These four values are a derivation, not four independent choices. An
implementation that adjusts one edge to taste has broken the lighting model, and
the break will be invisible until someone puts two panes side by side. Store the
light direction as the primitive and the four values as its consequence.

The bevel is uniform across all panes. It says *this is a pane*. It never says
*this pane is focused* — focus is §5, and it lives elsewhere.

### 2.2 Draw edges as borders

The bevel, the inner hairline, and the cast shadow are all drawn as real
borders. None of them may be drawn as a shadow.

The reason is mechanical: an inset shadow paints inside the element's box, and
any opaque child painted afterwards covers it. Tag bars and content regions are
opaque. A border reserves layout space that children cannot occupy, so it
survives.

The corner treatment follows from the same choice. Adjacent border edges of
different colours are mitred at 45°, which is the correct bevel corner and comes
for free. Shadow-based approaches produce square corners and lose it.

`border-style: outset` and `inset` are forbidden. Browsers and toolkits derive
the per-edge values themselves and do it inconsistently, which defeats §2.1.
State all four colours explicitly.

### 2.3 The floor

The workspace floor is `#8a7660`, and it appears in exactly two places: the 2px
gap between adjacent panes, and the 2px padding between the outermost panes and
the workspace boundary.

Gap: **2px**. Workspace padding: **2px**.

Total chrome between the interiors of two adjacent panes is therefore 6px —
2px bevel, 2px floor, 2px bevel.

### 2.4 The inner hairline

Every pane carries a 1px hairline immediately inside its bevel, in `header`
(`#cec4b6`).

Its colour is chosen so that it disappears where it runs alongside a tag bar and
appears where it runs alongside content. This is not a compromise; it is the
point. Against the tag bar it is invisible and the bar appears to run full width
to the bevel. Against the parchment surface, or against an embedded terminal, it
is a step of contrast that separates the surface from the lit bevel edge — which
would otherwise vanish into it, since `bevel_top` `#f8f2e6` and `surface`
`#f2ebe0` are nearly the same value.

This is the one place Daylight departs from Win95's construction. There the
button face was `#c0c0c0` against a `#ffffff` highlight, a wide gap that needed
no help. Daylight's surface is already near the top of its range, so the gap has
to be manufactured on the inside.

One colour, context-sensitive behaviour, no conditional logic.

---

## 3. The pane

A pane is a rectangle of the workspace holding one or more tiles. Panes tile;
they never overlap. The tiling structure is i3-derived and is not this
scripture's concern.

### 3.1 Geometry

| Property | Value |
|---|---|
| Bevel width | 2px |
| Inner hairline | 1px |
| Gap between panes | 2px |
| Workspace padding | 2px |
| Corner radius | 0 |

### 3.2 Tile stacking

Tiles within a pane stack as Acme file columns. Every tile contributes its tag
bar; exactly one tile shows its content, and the others are collapsed to their
bar alone.

A tile's tag bar keeps its position in the stack. It does not float to the top
when the tile becomes active. A bar above the content belongs to a tile earlier
in the stack; a bar below the content belongs to a later one. The stack order is
stable and the user's spatial memory of it is worth more than the marginal
tidiness of a floated header.

Consequently a tag bar may sit above or below its own content. §5.4 covers what
that does to the cast shadow.

---

## 4. The tag bar

Acme's inheritance: the bar is not a title. It is a line of executable text. The
pill contents are commands, and clicking one runs it.

### 4.1 Anatomy

```
 helix │ hx eevdf.rs   :w                    NORMAL · 221
 ^───^ ^ ^──────────^  ^^                    ^──────────^
 name  │  active pill  muted pill                trail
       │
   vertical rule
```

| Segment | Typeface | Role |
|---|---|---|
| Name | proportional | The tile's program. Never truncated. |
| Rule | proportional | `│` U+2502. Present only when pills follow. |
| Pills | monospace | Commands. First is active; the rest are available. |
| Trail | monospace | Right-aligned status. Never truncated. |

Pills are monospace because they are code. The name and the surrounding chrome
are proportional because they are prose. This is the same two-typeface rule that
governs the rest of Halcyon: monospace is reserved for things that are literally
text a machine will read.

When the bar overflows, muted pills shrink and ellipsise. The name, the active
pill, and the trail never do.

### 4.2 States

| State | Background | Separator | Name | Active pill |
|---|---|---|---|---|
| Resting | `#cec4b6` | `#a89880` | `#6a5a48` | — |
| Resting, active tile | `#cec4b6` | `#c86030` | `#1a120a` | `#e07840` |
| Live, exit 0 | `#b8ccc4` | `#1e5844` | `#0c2820` | `#1e5844` |
| Live, exit ≠ 0 | `#dcb8b0` | `#982818` | `#3c1008` | `#982818` |

*Resting, active tile* is the tile a resting pane would return to. It carries
the theme's own ember, which is background information — present, legible,
making no claim on attention.

*Live* is the tile holding input. There is exactly one in the workspace.

For a bar in below position, the separator moves from the bottom edge to the top
edge. Nothing else changes.

### 4.3 Metrics

| Property | Value |
|---|---|
| Height | 20px |
| Horizontal padding | 6px |
| Gap between elements | 5px |
| Name size | 10.5px |
| Pill / trail size | 9.5px |
| Pill line height | 14px |
| Pill padding | 0 4px |

The 20px height matches the status bar exactly, so the chrome has a single
vertical unit throughout. Below 18px the pills stop being reliable click targets
and the bar starts reading as a title rather than a command line.

---

## 5. The live tile

### 5.1 What is marked

Focus is a property of a tile, not a pane. The pane is a container; the tile is
what holds input. Marking the pane would draw a boundary around the wrong
object.

This has a consequence worth stating plainly: **no pane-level focus treatment
exists.** The bevel is uniform, the pane hairline is uniform, no pane is dimmed,
no pane is outlined. A reader looking for the focused pane finds it by finding
the live tile inside it.

### 5.2 Content must not be dimmed

Several conventional approaches were considered and rejected, and the reason is
worth recording because it will come up again.

The dominant pattern in tiling window managers is to dim inactive panes — tmux
does this with `window-style` against `window-active-style`, and it is genuinely
effective, because a whole-surface luminance shift registers in peripheral
vision where a thin edge does not.

Halcyon cannot use it. Thylacine plays media. A user watching video in one tile
while working in another must get the video at full fidelity, and a shell that
dims it is broken. The same argument rules out dimming a live console someone is
monitoring.

So the signal must be carried entirely by chrome, and the chrome must therefore
work harder than a dimming scheme would need to.

### 5.3 The bounded content region

The live tile's content is bounded on three sides by a 1px hairline in the
status colour — left, right, and bottom. The live tag bar's own separator closes
the top.

Header and content are therefore drawn as one bounded object. The bar is tinted,
the content is outlined, and the separator between them is the same value as the
outline, so the eye reads a single unit rather than two coincidentally-coloured
elements.

When the tag bar sits *below* its content (§3.2), the bottom edge is left open
and the bar's top separator serves as the boundary. The unit does not end at the
content in that arrangement; it ends at the bar.

### 5.4 The cast shadow

Immediately beneath the live tile's lower boundary sits a 1px line in `border`
(`#a89880`).

Dark accent line, lighter line beneath: this is the two-tone signature of a cast
shadow, the same construction as the bevel rotated to the horizontal. It lifts
the live content over whatever sits below it.

It is consistent with §2. NNW light falls from above, so content casts downward
and only downward. There is no corresponding line above the live tile, and the
asymmetry is the lighting model asserting itself rather than an oversight.

**The shadow is owned by the live tile.** It is not the top border of the
element below. This distinction matters in two cases that a borrowed line would
get wrong:

- The live tile is last in its pane's stack. There is no element below to borrow
  from, and a borrowed shadow would silently vanish.
- The live tile's tag bar is in below position. The unit ends at the bar, so the
  shadow falls beneath the bar, not beneath the content.

---

## 6. The status bar

One bar, at the bottom of the screen, spanning the full width. 20px, matching
the tag bars.

```
 1  2  3          transcript · ~/kernel/sched · make check        ⊢ 1 error  14:22
 ^─────^          ^────────────────────────────────────^          ^───────^  ^───^
 workspaces                    focused context                     condition  clock
```

| Role | Hex |
|---|---|
| `status_bg` | `#1a120a` |
| `status_fg` | `#f2ebe0` |
| `status_muted` | `#c8b89a` |
| `status_idle` | `#3a2e22` |

The active workspace indicator is filled `ember` with `status_bg` text. Inactive
indicators are `status_idle` on the bar background.

The condition slot carries the same sage/cinnabar distinction as the live tile,
so the tile's state and the bar agree. The bar is the redundant channel; the
tile is the primary one.

The bar is dark against a light theme deliberately. It is the one piece of
chrome that belongs to the system rather than to any pane, and its darkness is
what says so. It also grounds the composition — without it the workspace floats.

---

## 7. Typefaces

Two, as in HALCYON.md §4, unchanged by this scripture.

| Role | Face | Use |
|---|---|---|
| Proportional | DejaVu Sans Condensed | Tag names, prose output, all chrome. |
| Monospace | Cornucopia | Pills, paths, commands, terminal content, trail. |

Condensed is not a stylistic preference. Tag bars are dense and horizontal and a
condensed face fits more legible characters into the same run.

The monospace face appears as islands inside proportional text, never the
reverse. A monospace island always means the content is literal.

---

## 8. Discipline summary

- One light source, NNW, fixed. Four bevel values derive from it and are
  regenerated together or not at all.
- Bevel, hairline, and cast shadow are borders. Never shadows. Never
  `outset`/`inset`.
- The bevel is uniform on every pane. It marks a pane, never a focused pane.
- No pane-level focus treatment exists.
- Content is never dimmed, tinted, or overlaid. Thylacine plays media.
- Focus and status are the same marker on the live tile's chrome.
- Two status states only: sage for exit 0, cinnabar for non-zero. Warnings do
  not promote.
- The live tile's content is bounded left, right, and bottom; its tag bar closes
  the remaining edge.
- The cast shadow belongs to the live tile and is never the neighbour's border.
- Tag bar and status bar are both 20px. One vertical unit.
- Bevel 2px, gap 2px, hairline 1px, radius 0.
- Monospace means literal. Everything else is proportional.
- Bonfire surfaces inside a pane are never recoloured.
- Ember is shared verbatim with UTOPIA-VISUAL U-2 and is the link between the
  two surfaces.

---

## 9. Open design questions

1. **Status in resting panes.** A build failing in a pane the user is not
   looking at currently shows the ember active-tile treatment, identical to a
   passing one. A muted cinnabar separator on resting active tiles would surface
   it. The argument against is that it gives resting panes a second colour
   dimension and may compete with the live tile.

2. **The never-run state.** A tile whose console has not yet run a command takes
   sage, which asserts success that has not happened. A third neutral key is the
   obvious fix and is rejected under §1.4's two-state rule; the alternative is to
   accept the small inaccuracy.

3. **Long-running commands.** A tile whose command is still executing shows the
   status of the *previous* command. Whether an in-flight indicator is needed,
   and whether it belongs on the pill or the trail, is unresolved.

4. **Frutiger Aero.** H-2. Structure carries forward unchanged; only §1 is
   rewritten.

---

## 10. References

- `docs/UTOPIA-VISUAL.md` — Bonfire, U-2. The night half of the identity.
- `docs/HALCYON.md` — the graphical shell design this scripture serves.
- `docs/ARCHITECTURE.md` — Halcyon's place in the system.
- `docs/ROADMAP.md §8` — the Halcyon execution phase.
- `share/halcyon/halcyon-daylight.css` — token source, canonical.
- `share/halcyon/halcyon-daylight-mockups.html` — rendered specification sheet.
