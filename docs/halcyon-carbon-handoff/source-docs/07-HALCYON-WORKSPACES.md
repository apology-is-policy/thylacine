# Halcyon workspaces -- a PROPOSAL for the operator's vote (2026-09-08)

**Status: PROPOSAL, NOT SCRIPTURE. Nothing in it is built.** The 2026-09-02
vote deferred the workspace model ("one filled indicator" over "pulling the
workspace model forward" -- HALCYON.md 13's status-bar slots), and a vote
that defers is a decision, so this document does the research the fork
needs and stops. It exists because the composition mockup
(`docs/halcyon_text_composition_mockup.png`) shows the bar with three
workspaces (`1` filled, `2 3` idle) and the live rendering shows one --
the last mockup delta with a real mechanism behind it, and the one delta a
renderer must never fake (the live-fact rule of the welcome).

## 1. The question

What is a workspace in Halcyon, where does the bar's list come from, and
what does switching do? HALCYON-VISUAL 6 fixes only the rendering (the
active indicator filled `ember` with `status_bg` text, the idle ones
`status_idle`), and HALCYON.md 13 says "until H-4's layouts supply the
list". H-4 landed (named layouts saved and restored by the session tool;
`halcyon layout save|restore|list`), so the phrase now admits two readings.

## 2. Prior art

- **Plan 9.** rio has no workspaces; the idiom is nesting (a rio inside a
  rio window) and acme's columns (spatial, one screen). Nothing to inherit
  for the model itself; the naming and the bar are ours.
- **i3 / sway (the SOTA for tiling).** Numbered workspaces per output, each
  its own container tree; switching to a number that does not exist
  creates it; an empty workspace that loses focus vanishes; `move
  container to workspace N`; the bar lists them, the focused one
  highlighted; `append_layout` restores a saved tree INTO a workspace.
- **tmux.** Windows within a session: numbered, optionally named, each a
  pane tree; `select-window` / `new-window` / `move-window`; the status
  line lists the windows with the current one marked; a saved session
  (tmux-resurrect) records every window.
- **GNOME.** Dynamic workspaces with a trailing empty one -- the i3 rule
  from the other side.
- **Fuchsia / Genode.** No workspace at the compositor: policy lives in a
  layouter component above the display server. Thylacine's split is the
  same shape -- tapestryd owns the tree, halcyond owns the chrome -- so a
  workspace belongs to the tree (tapestryd), its listing to the bar.

## 3. The two readings, with fit and cost

**(A) Live workspaces (i3 / tmux):** a workspace is a live pane tree; the
display shows one; the others keep their surfaces alive and dormant.
Switching moves the display between trees and disturbs no process.
Fit: the compositor ALREADY has the machinery -- the d-1b backgrounding
stamps a leaf zero-rect + dormant (no FRAME ticks, no CONFIGUREs, its
pixels kept) and un-stamps it the reconcile it returns; a workspace switch
is that stamp applied per root. Cost: a `roots` vector + `active` on the
layout, `recompute` tiling the active root only, the chords, the layout
file's header line, the bar's two numbers, a bound on the count. One
tapestryd chunk, one halcyond line, one gate leg.

**(B) Layout-named workspaces:** the bar lists the NAMED LAYOUTS (device
tier + session tier), the active one being the last restored; switching
= `halcyon layout restore <name>`. Fit: nothing new in the compositor.
Cost: a "switch" SPAWNS the layout's tags (acme's tag-is-the-command-line)
and abandons the live tree -- a workspace that forgets what was running in
it is a layout menu, not a workspace; and the bar would count files, not
places, so the mockup's `2 3` would name things the user has not opened.

**Recommendation: (A), with (B) as the naming.** A workspace is a live
tree; a layout is what you restore INTO one; a workspace restored from a
named layout carries that name (tmux window names) for a later bar that
shows names beside numbers. This is the i3 + tmux consensus, it reuses the
one mechanism the compositor already audited for dormancy, and it keeps
the H-4 file format at v1 (a save is the active workspace's tree).

## 4. The mechanism (A), sketched against the tree as built

- `Layout.roots: Vec<usize>` (one per workspace, in order) + `Layout.active:
  usize`; `root` becomes `roots[active]`. `recompute` tiles the active root
  at the display; every other root's subtree is stamped backgrounded and
  zero-rect (the d-1b arm, one predicate wider: "hosted by a session while
  the console is up" OR "in an inactive workspace"). Focus is per
  workspace (remembered on switch, the i3 rule).
- **Switch** = a structural reconcile: the leaving tree's surfaces go
  dormant (no CONFIGURE fan to them; they keep their last present); the
  arriving tree's surfaces get their CONFIGUREs (a same-size one is the
  redraw request; the atlas F1 rule holds -- a dormant surface paints
  nothing until asked). The Direct/Composed decision reads the active root
  only. The `layout` file's header grows `workspaces N active K`; the
  per-pane rows are the active root's (the D decision's file-walk keeps
  working unchanged); a `workspace/<k>` subtree is NOT proposed -- one
  line is enough for the bar and the tool.
- **Chords** (free in the default table): Super+1..9 switch to workspace
  N (creating it when N == count + 1, i3), Super+Shift+1..9 move the
  focused leaf there. Keycodes 2..10 are unbound today.
- **Vanishing**: an inactive workspace with no hosted leaf is dropped at
  the next reconcile (i3); the active one never is. **Bound** (I-32):
  `MAX_WORKSPACES` = 9 -- Super+N is the whole keyboard's worth, and a
  hostile client's `workspace` verb (if one is admitted at all -- the seat
  class, like `scale`) cannot mint more.
- **The bar**: `StatusModel.workspaces`/`active` (already fields, 1/0
  today) read from the header line by the session's reconcile; the
  rendering is unchanged (HALCYON-VISUAL 6). The console (pre-login
  halcyond) shows one.
- **H-4**: `halcyon layout save <name>` saves the ACTIVE workspace's tree
  (v1 format, unchanged); `restore` rebuilds into the active workspace.
  `halcyon.rc` may `halcyon workspace N` (a new tool verb, the seat-gated
  ctl `workspace N` behind it) before a restore to fill several. Saving
  every workspace at once (a v2 format with a `workspace` header) is
  named, not proposed.
- **Under a session**: the workspaces are the SESSION's (the console leaf
  stays backgrounded in every one, as today). On logout the tree collapses
  to the console as today, workspaces and all.

## 5. What it touches (the audit-bearing surfaces)

`usr/tapestryd/src/pane.rs` (roots/active; recompute; the vanish rule),
`server.rs` (the switch as a structural pass; the backgrounding predicate;
the chords; the header line; an admitted `workspace N` verb under the seat
gate), `chords.rs` (the nine keys), `usr/halcyond/src/{session,status}.rs`
(the two numbers), `usr/halcyon` (the tool verb), `docs/HALCYON.md` 13 +
`HALCYON-VISUAL` 6 (the model, once ratified), AUDIT-TRIGGERS (a new row:
the dormancy predicate widened; I-32 on the count; the switch's fan).
Sub-chunks: W-1 the tree + the switch + the header (tapestryd, gated by a
battery leg: switch, dormant, return, vanish); W-2 the bar + the tool;
W-3 the gate leg in ls-gfx-session (Super+2 creates, the bar reads 2/2,
Super+1 returns, the bar reads 2/1). One Fable round over W-1..W-3.

## 6. Also owed a vote (the other two mockup-adjacent items; same doc so one round-trip settles all three)

- **The tag bar's pills** (HALCYON-VISUAL 4.1: `name │ active pill · muted
  pills … trail`; "the pill contents are commands, and clicking one runs
  it" -- acme's tag). Nothing produces pills today: the registry has no
  mark for them. Proposed: `mark k=pill;text=<command>` (BEACON 12.2
  amendment, the `cmd`/`prog` growth policy), emitted by a program that
  wants a command on its bar (nora: `:w`, `:q`; ut: none), rendered as the
  muted pills; the first one active; a click types it into the tile (the
  H-3c menu's "types the verb" path, no new authority). The welcome mockup
  shows no pills, so this is the editor's delta, not the composition's.
- **The condition count** (HALCYON-VISUAL 6's `⊢ 1 error`). Proposed:
  `mark k=diag;count=<n>` from a program with diagnostics (nora's LSP
  count), shown in the condition slot while that tile is focused, cleared
  by the next `prog` mark; absent, the slot shows the exit word as today.
  The welcome mockup shows `⊢ ok`, so this too is the editor's delta.

Both are registry amendments with a producer in nora and a consumer in
halcyond; neither changes authority. Neither is built.

## 7. For the operator

1. (A) live workspaces with layout names, or (B) layouts as the list, or
   keep the deferral.
2. If (A): the vanish rule (i3) and the bound of 9; Super+1..9 /
   Super+Shift+1..9 as the defaults; the `layout` header line as the
   channel (no `workspace/` subtree).
3. The two marks (`pill`, `diag`) as 12.2 amendments -- or a different
   channel for the editor's chrome.
