# The autonomous-run journal

**What this is for.** After a long autonomous run the operator needs to
reconstruct what happened without stitching together `git log`, six phase-status
rows, and a memory directory. This is that single thread: what landed, in order,
why, what it cost, and what it left open.

**What it is NOT.** Not a changelog — `git log` already has the commits, and
duplicating them here would rot. Not a status doc — `docs/phaseN-status.md` owns
per-chunk rows. What lives here is the *narrative*: the reasoning, the wrong
turns, the findings that were not in anyone's plan, and the decisions that
needed the operator.

**Conventions.**

- Newest run first. Within a run, chronological.
- Every claim carries its evidence: a hash, a measured number, a file:line.
- **A wrong turn is worth more than a win** — record the ones that were caught
  and how, because those are the reusable part.
- **Say what is still open, and be exact about what "fixed" covers.** A half a
  defect closed is written as a half.

---

## 2026-09-01 (run 16, Fable) — H-2: the transcript MVP lands; the E2E hunt that vindicated the gate

**H-2 built end to end in seven chunks** (a `05d837a7` / b `1c053bb6` / c
`9efe031c` / d-1 `30dfa16c` / d-2 `d02ed02f` / d-3 `ef22fe38` / d-4 at this
entry): the vt extraction (15/15 ls-gfx, zero-diff; 9 dormant tests LIVE),
the cartoon crate (the display list + CPU executor; the thematic name is
real weaving vocabulary), the vendor step (fontdue's no_std claim VERIFIED
at vendor time on the bare target — the §13.9 fallback unneeded; the whole
139-crate registry closure went hermetic because cargo source replacement
is all-or-nothing), the transcript core (the streaming-determinism property
test caught the `safe_cut` last-ESC bug ON ITS FIRST RUN: an OSC's ST
terminator is itself a later ESC, so cutting there strands the opener and
the wire parser rightly drops it whole), layout (a +2 stretch clamp I wrote
against §13.5 was WRONG and the box test said so; scripture stood), the bin
(aurora's loop shape; the held-feed policy lib-side with aurora's selftest
arms as real host tests), and the E2E.

**The first boot of halcyond died silently; the second was dark.** Two real
defects, each generalizable: (1) the libthyla-rs heap is a FIXED 4 MiB and
a no_std OOM panics into a bare `t_exits(1)` — fontdue's parse of two
DejaVu faces never fit; the fix is `ThylaAllocN<BYTES>` (const-generic;
`ThylaAlloc` unchanged for every other binary) with halcyond at 64 MiB
LAZY (demand-zero — physical pages only as touched). (2) my loop waited
before presenting — the scanout is first-present-wins and frame ticks
reach only VISIBLE surfaces, so a renderer that waits before its first
present is dark AND event-starved forever; the render pass moved to the
loop top and console-up now witnesses a real presented frame.

**Then the E2E showed zone frames but NO ls/stat frames, and the hunt took
five probes to vindicate the innocent.** The wrong turns, in order, each
killed by an instrument: "env inheritance broken" (the `env` tool — itself
a child — listed BEACON=rich); "the dc arm broken in children" (ps framed
under the SAME auto gate, same session); "ls's --beacon=always broken"
(the 32-char typed line died of the #60 quadratic-echo relay cliff BEFORE
running — a diagnostic leg poisoned by its own length); "the
multi-operand/-l forms unwired" (probe4 ran the exact forms; all silent);
finally a temporary in-bin diagnostic printed `env=rich dc=Some(99)
rich=true` — THE GATE WAS FINE — and the raw transcript held the answer
one line later: `ls: /version: no such file or namespace entry`. **The
operands were boot-ramfs paths that vanish at the pivot**; every
path-taking emitter failed on its operand, the `; pwd` sequencing passed
hollowly over the stderr error (the #212 skip-reported-as-pass shape
wearing my own output-only discipline), and pathless `ps` framed the whole
time. Two more scenario findings en route: a chord SPLIT focuses the NEW
empty leaf (`pane.rs`), so the zoom chord hid the renderer until a
Super+Left; and the no-such-file negative control must use the THYLACINE
wording — the generic phrase matches delve's boot-time history noise.

**The close bar**: ls-halcyon 12/12 on the lever image (45 s) — the
parchment-dominant pixel proof (~40k ink px, dom exactly 241,234,224), the
full rich chain on the wire (table + obj frames from real session `ls`),
the split/zoom reflow round-trip (128 → 63 → 128 cols), F10 silent; the
default-build suite re-run after the allocator/repl/ut changes. F10 landed
with the first rich advertiser exactly as the H-1 round ordered; F7's
transport+zone halves are now proven through a REAL advertisement; F11's
decision (open-zone attribution, the ut-side refinement named) is recorded
in the transcript module.

### The H-2 close (same run, after the second self-compaction) — the audit found the two bugs the self-audit couldn't

The batched round (Fable 5, start==end, one round for all seven H-2 commits
per the doubled cadence) returned **0 P0 / 3 P1 / 2 P2 / 2 P3** against the
threat model that matters here: halcyond parses arbitrary session output, so
every OSC 1936 annotation is attacker-controlled, and halcyond IS the console
— a panic or an OOM is the machine's face going dark. Under the shipped
`overflow-checks = true` + `panic = "abort"` profile, an integer overflow is a
hard process death.

Two of the three P1s were **panics I did not find in my own parallel
self-audit**, and the reason is the instructive part. My self-audit converged
exactly with the prosecutor on the *resource-exhaustion* family (F3 unbounded
growth, F5 the cost-accounting asymmetry, the obj u16 wrap) — I had SA-1..SA-5
written before the round returned. But I biased entirely toward "what grows
without bound" and never looked for "what single crafted value panics":
**F1**, a ~10-digit CSI parameter (`\x1b[9999999999m`) whose accumulator did
`saturating_mul(10) + digit` — the multiply saturated, the *add* did not, so
the tenth digit overflowed u32 and killed the console to one escape sequence
(and the identical line in the extracted `vt` crate kills aurora *today* —
pre-existing, and ours); and **F2**, `mark k=exit code=-9223372036854775808`,
where the exit badge negated the magnitude with `n = -n` and i64::MIN has no
positive i64. Two prosecutors, two blind spots, one union — this is the whole
argument for the second reader stated in a single round.

The fixes: `saturating_add` in both scanners; `unsigned_abs()` for the badge;
for F3 a set of incremental fail-safe ceilings (the budget machinery only ran
at producer-chosen boundaries, which a hostile producer simply never emits) —
an endless line soft-wraps into the eviction path, objs/styles/table-rows cap,
and an in-progress table carries its own byte budget; for F4 (the wire depth
cap resets every `feed()`, so nesting leaks across chunk boundaries) the
em/obj stacks get suppressed-open counters that preserve LIFO balance exactly,
mirroring the wire layer; for F5 the obj bytes now charge `stored_cost`
symmetrically so eviction cannot drift the budget to zero. Seven regression
tests, two of them the exact panic reproductions (they fail on the pre-fix
code under overflow-checks, pass after). While in the files I swept two
pre-existing `vt` and three `beacon` clippy lints (inherited verbatim from
aurora at the H-2a extraction, surfaced now that the crates are linted
independently) so the whole Halcyon userspace is clippy-clean.

This is a **dirty close** (3 P1s + F3's incremental-cap restructure is
structurally invasive). Per the doubled-cadence direction, the re-prosecution
of the fixes themselves is carried FORWARD into the H-3 round rather than spent
as a dedicated round-2 — the focus is recorded in
`memory/audit_h2_closed_list.md`. Verification: host 91/91 across the four
crates (+7 new), clippy-clean, the default production bake boots clean (0
EXTINCTION, arc 2/2 + clade 3/3); LS-CI full + the lever ls-halcyon re-proof
close the surface. The kernel 1435 unit suite was deliberately not re-run —
the change is userspace-only (halcyond/vt/beacon compile into no kernel object)
and the production boot already exercised the userspace end to end.

### H-3 opens: the design pass, then the first chrome landing (same run)

The operator dropped the **Daylight** visual scripture mid-run
(`docs/HALCYON-VISUAL.md` + a token CSS + a rendered mockup) and, asked, ratified
it as binding + chose a design-pass-before-code. So H-3 began as a design
conversation, not code. I surveyed the real compositor first (a subagent, so the
proposal was grounded in what exists, not what I imagined): tapestryd already
carries `Role::Chrome` on every leaf but it is **inert**, already CPU-paints
strip segments + a flat 1px frame into the composed screen buffer, and already
has the gated default-deny global-ctl that the menu verb will ride — while
menus, input-grab, dismiss, chrome text, real bevels, and a status bar are all
absent. That survey turned two forks into legible votes: halcyond paints the
*whole* tag strip into a chrome surface (opaque, no alpha overlay — the
compositor just places it and is told the status key), and the full Daylight
chrome lands across four sub-chunks. Both ratified; the design went into
HALCYON.md §13.6 as scripture (`345245c5`) before a line of code — the recorded
pattern.

**H-3a-1** (`6a812348`) is the first landing: `usr/lib/libhalcyon`, a no_std
crate whose `theme` module is the whole Daylight §1 palette as code (tests pin
every value to the scripture), and halcyond's transcript rebuilt through it. The
one non-obvious part was a coherence trap: the transcript's "default ink" test
(`st.fg == sheet.ink`, the hook that colours presentation refs and dim text)
only fires when the pen's *default* fg — which comes from the vt palette —
equals the Sheet's ink. Swapping the Sheet to Daylight's fg while leaving the
palette on PARCHMENT broke it (they differ by a hair, `0x2B2320` vs `0x1A120A`),
and a layout test caught it immediately. The fix is `daylight_palette()`: the
transcript's palette is now grounded in Daylight too, so palette and Sheet agree
by construction. The pixel proof's pinned ground moved with it — from the
PARCHMENT surface (241,234,224) to the Daylight surface (242,235,224) — and
ls-halcyon stayed 12/12 on the lever, confirming the new ground renders.
**Next: H-3a-2**, the tapestryd side — the NNW bevel, the hairline, and the
live-tile cast shadow, the compositor's half of the Daylight chrome.

### H-3a-2: the compositor chrome, and the "flaky" hang that was a real pre-existing race (same run)

The scope was bigger than the resume note framed it. The note assumed the
tapestryd chrome was a Halcyon-only concern; the blast-radius sweep found
tapestryd is **warden-spawned unconditionally** (joey.c:10197), so its chrome
painter runs on the default (aurora) image too — and two default-image gfx tests
already hardcode its old colours (`ls-gfx-panes` asserts BORDER `58 58 68` +
FOCUS `122 158 204`; `ls-gfx-chords` asserts BG `16 16 20`). The Daylight
scripture settles the direction rather than opening a fork: its header binds
*"the pane compositor"* to read its values, and HALCYON.md §13.6 ratifies H-3a as
the unconditional compositor geometry. So the chrome is Daylight system-wide, and
those tests are stale consumers to sweep by ground truth — not a design question.

The mechanism: the chrome lives in the `inset`-wide ring between a leaf's `rect`
and its `content`. A 1px flat frame fit in `inset=gaps` (default 1); a 2px bevel
+ 1px hairline + floor does not, so `recompute`'s inset grew from `gaps` to
`gaps + bevel(2) + hairline(1)`. `paint_borders` now paints, per ring pixel, by
distance-to-nearest-edge: floor / four-value bevel (mitred, horizontal winning
ties for the top-weighted 45° corner) / hairline; the cast shadow is a 1px
`border` line at the focused leaf's innermost bottom floor row, downward-only,
owned by the tile. The bevel is uniform — the old focus-coloured frame is gone
(§5.1: no pane-level focus treatment). The battery hardcodes the inset to derive
its tab-strip sample coords, so its `1`→`4` was part of the sweep.

**The finding nobody planned — and what caught it.** The first ls-halcyon run on
the lever ran **15 minutes** against a 45-second baseline. The convenient reading
was "flake"; the playbook forbids it as a conclusion, so it got hunted. Ground
truth, in order: QEMU was at **6.8% CPU, sleeping** — idle, not a busy-loop, so
not my painter running away. A live screendump of the stuck guest showed the
**center was logged-in parchment with 15268 ink pixels** but the **left/top edges
were parchment too (0 off)** — logged in, but single-pane, *no bevel* — i.e. the
guest was stuck **before the split**, before any line of my chrome code runs. A
re-run with a tight timeout printed the exact verdict: **"no login prompt within
160s."** The cause was a pre-existing boot-output race: login's `Thylacine login:`
and halcyond's `console up` print on the *same* serial from *different* procs, so
their order is nondeterministic; the scenario's inline `lc_expect "console up"`
consumes the stream up through that line, and when the login prompt raced ahead of
it (as this boot), the prompt was swallowed and `lc_login` waited out the whole
boot timeout. H-3a-1 had simply drawn the lucky ordering. Attribution: the racing
procs and the `lc_expect`/`lc_login` sequencing are all upstream of and untouched
by my bevel edit, and ls-gfx-panes/chords never hit it because they assert no
"console up" between boot and login. Owned in passing (yip note + this entry, no
`TaskCreate` in-session): the two bring-up assertions moved to order-independent
`tgrep` at the end, so `lc_login` finds the prompt regardless of ordering. The
re-run passed [35s].

Verified: **ls-halcyon lever** — `Daylight NNW bevel painted (left 226,214,192 !=
top 248,242,230 -- four-value)`, plus parchment+ink, split reflow (63 cols), zoom
restore; **ls-gfx-panes default** — `tab strip segments exact (Daylight: inactive
header, active-focused ember)` + the battery structure asserts (the new inset
geometry) + client pixels; guest build RC 0, my code clippy-clean (the 231
tapestryd warnings are pre-existing, none in the edited lines), libhalcyon host
3/3. The vault owns `sub-tapestryd` (server.rs + pane.rs) — the Daylight-chrome
dossier delta was rung to vault via yip 0031; `usr/tapestry-battery/src` is
unowned (sweep filed). **Next: H-3b**, the executable tag bar (audit-bearing).

### H-3b-1: the per-leaf tag-bar geometry, and the batch that starved its own witness (same run, after the operator-directed self-compaction)

The tag bar's first sub-chunk is pure geometry — no capability, no gate — so it
landed without an audit round; the gated `tag status` verb (H-3b-4) is where the
round attaches. Three things worth keeping from it.

**A token that already existed.** The ratified plan said "add `TAG_BAR_H` to
`METRICS`"; `theme.rs` already carried `header_h: 20 // tag bar height`. Adding a
second name for the same 20 would have been two tokens for one value — exactly
the drift the single-token-source rule (`libhalcyon::theme` "and nowhere else")
exists to prevent. So the carve reads `METRICS.header_h` and the only move is the
genuine one: `TAB_STRIP_H` (5) out of pane.rs into `METRICS.tab_strip_h`, with
the libhalcyon test pinning it. Scripture §13.6 said "(= `METRICS.header_h`)" in
one place and "a new `TAG_BAR_H`" in another; the as-built follows the first and
the second is reconciled below.

**Why the painter needed one addition, not a rewrite.** `paint_borders` derives
the ring `inset` from `content.x - r.x` — horizontal. A tag bar carved off the
content TOP changes only `content.y/h`, so every band, the mitre, and the cast
shadow are untouched by construction; the single new paint is the strip filled
`header`-bg (the same colour as the inner hairline, so hairline + strip read as
one header band — §2.4's intent). That fill is load-bearing, not cosmetic: a full
repaint clears the screen buffer to `BG_COLOR`, and the strip is no longer a
content rect, so without it every leaf would wear a dark 20px gap until halcyond
binds a chrome surface (H-3b-3). `content` is now the client rect, tag bar
excluded, which is what `surface_at`, `visible_hosted`, and the `geometry` file
all wanted anyway.

**The sweep, by ground truth, and the witness it left behind.** The first boot
failed exactly where the survey predicted a consumer hardcodes geometry: the
battery back-computes the tab-strip row from B's content (`sy = tb.y - inset - 5
+ 2`), and B's content had moved down 20, so the probe landed inside the strip
and read `206 196 182` — Daylight `header` — instead of ember. That failing pixel
was an accidental proof that the fallback fill paints where the tag bar is; the
fix subtracts the tag bar (`sy` = 22 → 2, the strip's centre row, `cy+2`).
Content-centre probes never moved (the carve is top-only), and every other
scenario passed unchanged. But a fix that moves a probe OFF the new surface
leaves that surface unwitnessed, so the battery now reads `pane/<A>/tagbar`,
asserts it abuts A's content (same x/w, `ty + th == pa.y`), and probes its
centre; ls-gfx-panes samples the same point host-side. Both halves read
`206 196 182` at `battery: tagbar A 960 14` [PASS 42 s]. ls-halcyon [27 s] and
ls-gfx-chords [36 s] passed untouched — their bevel/floor samples sit at
x,y = 1,2 and mid-height, outside a content-top strip.

**The stall I caused — and what it actually was.** Single-leaf scenarios
cannot regress here: with one visible leaf `inset` is 0, the `else { content =
r }` branch is byte-identical to before, `tagbar` stays ZERO, and the painter
skips on `rect == content`. I ran the eight CPU single-leaf gfx scenarios anyway
at `LS_CI_JOBS=3` — three 4-vCPU guests on 8 cores. Seven passed; ls-gfx-age
sat 12 minutes with its log frozen at `age: grid ... region`, and I read that as
oversubscription starving its `yes | head` fill. That was the convenient
reading, and the solo re-run the operator chose refuted it in 327 s: **at
jobs=1 it failed identically — `no login prompt within 300s` against a healthy
guest** (aurora up, scanout direct). The log carried the mechanism two lines
apart: `Thylacine login:` at 2817, `aurora: console up` at 2819. The age exp
parsed its console grid from an inline `expect` on the console-up line BEFORE
`lc_login`, so whenever login raced ahead the expect consumed the prompt and
`lc_login` waited out the budget — the exact race H-3a-2 fixed in
ls-halcyon.exp, in a second file. The batch had merely stretched the
jobs-scaled budget past my patience, so I killed it before it could report the
same verdict. Two lessons, both already on the wall: a harness race is a
PROPERTY and H-3a-2 fixed one LOCATION — a comment-filtered sweep of every exp
for "console-up expected before lc_login" (run now; BSD grep has no `\s`, the
first sweep's comment filter was silently broken) finds exactly this file and
no other; and a stall under a batch I oversubscribed is not thereby explained —
the cheap solo measurement is what separates duration from mechanism, and here
it found a mechanism. The cure is ls-halcyon's: `log_file -a` records
everything the pty delivers, consumed or not, so age logs in first and then
parses the grid from the transcript (closed to flush, re-opened) — the same
128x36 / 10x22 / region x 20..640 y 132..726 as before. PASS [40 s]: positive
control 4/4, negative leg 8/8, worst 0 px.

Vault: `sub-tapestryd` owns pane.rs + server.rs — the tagbar/PFK_TAGBAR/fallback
delta rung via yip 0031; `theme.rs`, the battery, and the .exp are unowned
(reference section here, sweep filed). **Next: H-3b-2**, the surface-create ABI
(`create W H role=chrome bind=<pane-id>`) + `Role::Chrome` activation.

### H-3b-2: the chrome role becomes a capability, not a flag (same run; the operator left mid-wait — see the standing authorization)

The ratified text said `role=chrome` makes a surface "non-auto-hosted,
non-focusable, excluded from the Direct count" and that the compositor places
it at the bound pane's strip. It did not say who may create one. Read as
written, any client could bind a chrome surface to any pane and paint over its
tag bar — a fake tag bar on someone else's pane is the graphical twin of a
spoofed prompt, and the whole reason the tag bar exists is that a user trusts
what it says about the tile beneath it. So chrome creation joined the gated
class: `peer_is_renderer` at create, E_PERM otherwise, the cfg-3 default-deny
that every other chrome operation in §13.6 (menu place/dismiss, `tag status`)
already rides. Syntax is judged first and separately (E_INVAL for every peer),
so a peer cannot use the gate to learn whether a pane id exists, and the
battery — a non-renderer — can witness the parser and the gate as two
different verdicts on the same line. The positive twin, the same line from a
renderer composited at the strip, is halcyond's at H-3b-3, one variable away.
Recorded in §13.6 as as-built; the H-3b-4 round prosecutes it.

The rest of the sub-chunk is the consequence of one observation about the
compositor: a surface's screen rectangle is decided in exactly one place,
`compose_geometry` (`find_hosting` → the pane's content rect, then letterbox
or crop). A chrome surface is not hosted, so rather than teach hosting a
second kind of leaf, that decision moved into a helper, `surface_target`,
with two arms — content rect for a hosted surface, `tagbar` strip for a bound
chrome one (crop only: its owner sizes it to the strip) — and the same helper
now answers `compose_visible` and `note_present`. Everything the design
listed as properties of chrome then falls out of "not hosted": no leaf means
no focus, no Direct candidacy (`visible_hosted` never lists it), no pointer
hit (`surface_at` walks leaves). The two fans that reach every visible surface
gained a second half: the structural CONFIGURE fan sends chrome its STRIP
size — which is the relayout hook halcyond will repaint and resize on — and
the frame fan includes it. I first wrote that this carries the standing
wedge-retire contract for an owner that never drains; reading `push_event`
says otherwise — FRAME is a droppable, coalesced class and CONFIGURE
coalesces by replacement, so an idle chrome owner cannot wedge its surface,
and the events that do wedge (KEY, pointer) never reach chrome: it is not
focused and `surface_at` walks leaf content rects. A claim true of a
different event class, caught by reading the function it was about.

One hazard was mine from the previous sub-chunk. H-3b-1's `paint_borders`
refills the strip with the resting `header` and pushes that rect; on a
focus-only repaint that would paint over a chrome surface's pixels and push
the clobbered strip to the display. The fill is now structural-only
(`fill_tagbars`): a focus change alters the shadow and the strip highlights,
never the strip's content, so the refill there was always redundant and,
with chrome present, wrong.

Verified at jobs=1 after a ~75-minute wait for the mac (aux's audit close held it; I queued, declared `busy`, and waited on a 90-s-quiet-of-aux-QEMU signal rather than a lease expiry that `resources` later explained as a re-hold's TTL artifact): ls-gfx-panes PASS [42 s] (the four gate probes: `battery: chrome-create gate OK` + the exp's `discriminates (E_INVAL syntax / E_PERM non-renderer)`; the H-3b-1 tag-bar witness intact) + ls-halcyon PASS [27 s] (split/zoom reflow + the four-value bevel) + ls-gfx-chords PASS [36 s] (chord focus moves + `gaps` changes over the structural-vs-focus-only repaint split), all at LS_CI_JOBS=1; tapestryd/libtapestry/battery build clean (only the 3 pre-existing warnings); bake OK, keys paired. Vault: server.rs (sub-tapestryd) + lib.rs (sub-libtapestry) deltas rung on yip 0031; the battery + the .exp unowned (sweep filed). **Next: H-3b-3**, halcyond's per-leaf chrome surfaces — the positive twin of this sub-chunk's E_PERM probe.

### H-3b-3: halcyond paints the tag bar — and the greens that were skips (same run, at the 600k line)

**The finding first, because it reaches back two sub-chunks.** H-3b-3's first
verify came back `PASS: ls-halcyon [27s]`. Before claiming the witness I
grepped the scenario's log for my own new pass lines — and found none. Not one
`LS-CI PASS` in the whole log; the steps file read boot → `exit` → quit. The
scenario is env-gated to the halcyond-renderer lever image and had SKIPPED:
`puts "LS-CI SKIP: ..."` then `lc_quit; exit 0` — and the harness scores exit
0 as PASS. Its SKIP code is 77 (three gfx scenarios already use it, and the
summary then says "SKIPPED — NOT coverage"). So every "ls-halcyon PASS [27 s]"
in today's H-3b-1 and H-3b-2 records was a skip read as a witness — the 27 s
should have said so (H-3a-2's genuine lever run took 35 s). The sweep for the
shape found one more: ls-gfx-chords prints a bare `==> SKIP` (no `LS-CI SKIP:`
marker the harness could even quote) and exits 0; its own comment said "the
runner counts it PASS". Its H-3b-1/H-3b-2 greens were skips too — H-3a-2 had
written "SKIPs sans cfg-4" honestly, and I then read the PASS rows as
evidence. Both exps now print `LS-CI SKIP:` and exit 77; both status rows are
corrected. This is #212 (a skip reported as a pass) recurring in my own hands
one day after it was pinned, and what caught it was the discipline of reading
the log for the witness line rather than the verdict — the verdict was true
and irrelevant. The code those rows describe is unchanged and was exercised by
ls-gfx-panes; what was hollow was the *halcyon-side* claim, which this
sub-chunk's real lever run now supplies.

**The chrome.** `halcyond::chrome` owns one `Role::Chrome` surface per visible
leaf that carries a strip. `reconcile` reads the pane 9P tree the §13.7 way —
`layout` for the visible leaves and the focused `*`, `pane/<id>/tagbar` for the
strip (ZERO = bar-free), `pane/<id>/tag` for the name — and diffs it against
the live tiles: gone → drop (a dropped `Surface` closes its conn; the compositor
retires), new → `Surface::chrome_on`, kept → repaint. halcyond names its own
pane once through the same `tag` file ("halcyon" — §4.1's "the tile's program";
§13.6 already named the tag file as the name source). One cartoon list per
strip: the header ground, a 1 px separator on the bottom edge (`ember_deep` on
the focused leaf's tile — "resting, active tile" — else `border`), the name in
the proportional face at 10.5 px, centred via `line_metrics`. No pills: a
Resting bar has none, and pills are commands, which is H-3c. `pump` drains
every tile's events and returns whether a CONFIGURE was seen, which triggers a
reconcile in the same pass — painting in the pump would flash the stale
state. It runs only after the console's first successful present (the scanout
is first-present-wins; chrome must never precede it), then on every
main-surface CONFIGURE or FOCUS.

**A gap in my own design, found by asking what wakes it.** The plan said
"react to relayouts": a structural relayout fans every visible surface a
CONFIGURE, so the main surface's CONFIGURE is the wake. But a focus move is a
focus-only epoch, which fans nothing — so the separator that is supposed to
follow focus would go stale on exactly the event it exists to show. The
compositor's focus-only branch now fans the visible chrome surfaces a
same-size CONFIGURE (the existing redraw request, coalesced by replacement),
and the ls-halcyon witness asserts the round trip: after Super+Left the two
separators swap.

Verified, this time on the levers and by the lines, not the verdict: ls-halcyon on the THYLACINE_HALCYON=1 lever PASS [37 s] -- `pre-split control -- no tag-bar strip on a single leaf (dom 242,235,224)`, `left tag bar is Daylight header with name ink (186 4672 off/total)`, `tag-bar separators carry focus (left border, right ember_deep)`, `focus move re-keyed the tag-bar separators (left ember_deep, right border)`, `zoom dropped the tag bars (single leaf, strip ZERO)`, plus every H-2/H-3a-2 leg (parchment+ink, 63-col reflow, the four-value bevel, the rich chain); no `halcyond: chrome ... failed` line. ls-gfx-chords on the THYLACINE_AURORA_CFG4=1 lever PASS [34 s] with its real body (Super+H split, gaps 8 = the Daylight floor, Super+F unbound, Super+G zoom rebind + toggle) -- the first time today that scenario RAN. Default fixtures restored (0 lever lines in the restore bake) and ls-gfx-panes PASS [41 s] on them with both H-3b legs (`per-leaf tag bar renders Daylight header-bg`, `chrome-create gate discriminates`). All at LS_CI_JOBS=1; builds clean; keys paired. Three bakes for one sub-chunk (the halcyon lever, the cfg-4 lever, the default restore) is the cost of the lever design, and it is the right cost: a verdict that cannot tell a skip from a pass is the expensive thing. Vault: the server delta (the focus-epoch fan) rung on yip 0031; halcyond + both exps are unowned (`150-halcyond.md` gained the chrome section; sweep filed). **Next: H-3b-4** -- the gated `tag <pane-id> status` verb, the live sage/cinnabar states in both painters, and THE audit round covering H-3b-2's gate + this create path, with the AUDIT-TRIGGERS row.


### H-3b-4: the tile status becomes a fact the compositor keys by focus — and the halcyond lib split gets enforced (same run, after the self-compaction)

**The design call, made under the standing authorization.** §13.6 ratified a
three-valued verb (`tag <pane-id> status ok|err|resting`, "name provisional")
and left open who decides what shows. Vote 1's wording ("the per-tile
status-key COLOR it is told") reads as: the renderer sends a display key per
tile and the compositor paints it. I did not build that, and the reason is
the H-3c gate's own principle applied one chunk early: a display key sent per
tile needs *two* verbs on every focus move (the old tile to `resting`, the new
one to its key), and until the second lands — or forever, if halcyond is
wedged — a tile that has lost input still carries the marker that §1.4 says
means "where you are". So the verb carries only the fact the renderer alone
holds, the LAST EXIT, and the compositor keeps focus as its own fact and
combines the two at paint time: the live tile shows sage unless the record is
`err`; a tile that is not live shows no key at all. A stale or wedged
renderer cannot mark the wrong tile, and a focus move re-keys the hairline
inside the compositor's own focus-only repaint with no second verb. `resting`
survives as the explicit reset (= nothing recorded = sage when live), so the
ratified enum stands with exact semantics: it is the record, not the display
state, and the new per-pane `status` file reports exactly that. Recorded as
ratified under the 2026-09-01 authorization; the as-built paragraph in §13.6
says so.

**Where the hairline goes, derived rather than chosen.** §5.3 outlines the
live *content* on three sides; §2.4 wants the hairline to vanish alongside a
tag bar. With a tinted live bar the header-coloured hairline would no longer
vanish, so alongside the bar (the top row and the strip's flanks) it takes the
bar's tint, and alongside the content it takes the key. The bottom row is the
§5.4 shadow's dark half, above H-3a-2's lighter `border` row; the two dark
bevel rows between them are the pane's and stay uniform, because §2.1 says a
bevel never says "focused". The exp asserts both halves separately: the left
column reads the key, the top row reads the tint.

**What the scripture changed under H-3b-3's feet.** H-3b-3 painted the
focused leaf's separator `ember_deep` as a stand-in for focus. §4.2 read
carefully says otherwise: *Resting, active tile* is "the tile a resting pane
would return to" — with one tile per leaf, that is every unfocused leaf, and
the focused one is Live. So every resting pane now carries the ember on its
separator with the name in full ink, and the plain Resting row (border
separator, muted name) is reserved for a stack's collapsed tiles, which do
not exist yet. The tab-strip painter had already made the same call
(`ember_deep` = active-but-not-focused); the tag bars now agree with it.

**The regression I inherited from myself.** The halcyond lib's charter is
"everything that thinks, and nothing that syscalls", host-tested through a
documented recipe. H-3b-3 put `chrome.rs` — surfaces, fds, the event pump —
in the lib. The recipe then failed to build (`unresolved import libthyla_rs`)
and nobody ran it at H-3b-3, so it was found here, by running it. The fix is
the charter: `halcyond::chrome` is now the pure half (the layout and rect
parsers, `key_for(focused, status)`, the per-key colours, the strip display
list) with five host tests pinning the §4.2 table and the parse edge cases,
and the bin's `chromeset` is the surfaces. 47/47 host tests, up from a
recipe that did not compile.

**A witness that would have gone hollow.** The H-3b-3 zoom leg asserted the
strip region "no longer reads header". Under H-3b-4 that region reads the
live tint before the zoom lands, so the old assertion would have passed
immediately for the wrong reason. It now demands the transcript's parchment
— the state only the zoom can produce.

**The feed.** The transcript already parsed the shell's `exit` mark into the
block; a latch (`take_exit`) exposes the last one, and the chrome step sends
`tag <own-pane> status ok|err` on the console surface's own conn (the gate
reads the conn's peer, and halcyond holds the renderer role). Held until the
console is up and its pane is known; a refusal is said once and the feed
stops — display only, nothing else degrades.

Verified on the levers, by the lines: ls-halcyon on the THYLACINE_HALCYON=1
lever PASS [43 s] — `tag-bar keys follow focus (left resting ember_deep, right
live sage; left hairline header)`, `focus move re-keyed the tiles (left live
sage: strip + content hairline; right resting ember_deep)` including the top
hairline row reading the sage tint alongside the live bar, `pwd; false` →
`a non-zero exit keyed the live tile cinnabar (strip + separator + content
hairline)`, `pwd` → `a zero exit re-keyed the live tile sage (the status is
the last exit)`, and `zoom dropped the tag bars (single leaf, strip ZERO,
parchment at the top)`; no `tag status refused`, no `chrome ... failed`; every
H-2/H-3a legs intact. Default fixtures restored (0 lever lines) and
ls-gfx-panes PASS [42 s] with the battery as a non-renderer: `tag-status
gate refuses a non-renderer (E_PERM; state unchanged)` — the write returns −1
and the pane's `status` file still reads `resting` after it; the H-3b-1/-2
legs intact. Three target crates clean; 47/47 host. **Next: the H-3b audit
round** (holotype-reviewer, Fable max) on this commit — H-3b-2's create gate +
H-3b-3's chrome path + this verb + the H-2 dirty-close re-prosecution — then
the AUDIT-TRIGGERS row already landed with the chunk is the round's scope.

### The H-3b audit close: two prosecutors, three P1-class findings, and a self-report that could not see its own fallback (same run)

**The round's shape.** One prosecutor on the full brief (H-3b-2/-3/-4 plus
the H-2 dirty-close re-prosecution), spawned through the agent definition
that pins Fable. Its transcript's `model` field tells a story its report does
not: 46 turns on Fable 5.1, then Opus 4.8 for the rest — the fallback landed
exactly as it began the server.rs deep read, and its final report ran on
Opus while its `MODEL(end)` line said Fable. A model cannot observe its own
fallback; the self-report is a claim, the API field is the measurement, and
from now on the latter is what the closed list records. Rather than discard
fifty Fable turns or accept an Opus read of the load-bearing half, a second
prosecutor went out scoped to exactly that half, spawned with the explicit
model override (a probe agent confirmed Fable resolved that way), and stayed
on Fable throughout. Two independent reads of the compositor; one of the
transcript.

**What they found, distinct: 0 P0, three P1-class, four P3 — all fixed.**

*The transcript's open block escaped the budget* (R1, P1 — the H-2
re-prosecution paying off). The budget evicts frozen blocks only; the open
block froze on a 10 000-line count alone; a newline-free stream soft-wraps
into 4096-cell lines, so 320 MiB could sit in one open block against a 64 MiB
heap. `cat` of a large minified file was a silent renderer death. The H-2 F3
disposition had said "flush → budget bounds it" — true of each line, false of
the block. The open block now freezes on bytes too. The regression's first cut
asserted that several frozen blocks survive under a 64 KiB budget and failed:
one 32 KiB line per block means the budget keeps exactly one. The failure was
the mechanism working, and the test now asserts the bound instead of a count.

*The pane tree was ungated* (R1 + R2, P1 — pre-existing G-6, owned). The
sharper prosecution came from R2: 128 `focus` flips fit in one 32 KiB service
pass, the victim cannot drain between frames of a pass, FOCUS was
non-droppable, so any client could wedge-retire any other client's surface —
the console's included — and a focus steal routes the graphical login prompt's
keystrokes to the thief. H-3b had made this worse without noticing: the tag
bar renders `tag` as *the tile's program* and the live key follows focus, so
a rename or a focus steal became a lie the chrome tells. The fix is a trust
model, decided under the standing authorization and written into §13.6: rio's
line — a client drives its own window, the environment drives the rest. A
subtree mutation needs every hosted surface in it to be yours; taking or
naming a tile needs the leaf to host your surface; the renderer may do
anything; reads stay ungated. FOCUS now coalesces like CONFIGURE, and one conn
lands at most four layout mutations per pass. The battery — a non-renderer by
design — kept every one of its scenarios, because every one acts on its own
subtree; the single thing it lost was an end-of-run refocus of the console, a
pane it does not own, which the close path restores anyway. Its new leg
witnesses the gate from the wrong side of it — and its positive control, one
variable away from the three refusals ("focus on OUR pane succeeds"), caught
the first cut of the fix: ownership was keyed on the conn, and a client holds
one session per Surface plus a driver session, so the battery's own pane was
foreign to the conn that asked. The owner is the process, keyed on the
kernel's per-Proc `stripes` tag. A gate leg that only proved refusals would
have passed that bug straight through.

*My own H-3b-3 design exhausted the pools* (R2, P1). One session per tag bar,
against global pools of eight conns and eight surfaces: a third window filled
both, and past it every chrome mint was a five-second blocking connect inside
the renderer's single-threaded loop — the listener is un-armed when full, so
the connect sat on the kernel's handshake deadline — repeated on every
reconcile, with keyboard autorepeat during the stall enough to wedge-retire
the console surface. The gates run at two leaves and never reached the bound.
Now a tag bar is minted on the renderer's existing pane-tree session, the
renderer's cap is widened by one surface per pane and the pool is sized so
every conn can reach its cap at once, and the listener stays armed and refuses
at once when full, so a connect can never block its caller.

**The close is dirty by the invasive-fix rule** — two load-bearing mechanisms
were restructured — and the fixes ride forward as the H-3c round's
re-prosecution focus rather than a round of their own.

Verified by the lines: halcyond host 48/48; ls-halcyon on the lever PASS
[41 s] with every H-3b-4 key line; ls-gfx-chords on the cfg-4 lever PASS
[34 s]; default restored: ls-gfx-panes PASS [42 s] with the
pane-tree gate leg, ls-gfx-age PASS [39 s], the 7 single-leaf CPU gfx
PASS. All at jobs=1, after a lease wait behind aux's SMP gate.

### H-3c: THE GATE — the obj verb menu, the grab, and a dismiss that needs nothing from its owner (same run)

**What the chunk is.** The H-3 exit gate reads "menu-dismiss-by-compositor
proven vs a wedged client". The whole design rests on two properties the
compositor must hold alone: while a menu is up every key and pointer event
goes to it and nothing reaches the leaf underneath; and the menu comes down
on Esc, a click outside it, a chord, or its owner's death without the owner's
cooperation. The as-built (HALCYON.md §13.6) makes the second property fall
out of one mechanism: the menu is a `Role::Menu` surface and *dismissing it is
retiring it* — `retire` carries the unplace and the heal, so Esc, click-away,
a chord, the owner's own verb, ctl `destroy`, the conn's death, a wedge and a
replacement all land in the same arm. A wedged halcyond cannot strand a modal
because the modal's lifetime was never halcyond's.

**Decisions taken under the standing authorization**, each with its
alternative: the heal under a dismissed menu is targeted (chrome
intersections re-pushed, the resting fills, a redraw CONFIGURE to the
intersecting surfaces) rather than a structural repaint, which blanks every
pane until each re-presents — a whole-screen flash per menu. Rio's save-under
was the heritage answer and was rejected on a measured fact about this
compositor: on the GPU composed path the screen buffer holds no client
pixels, so a save-under would restore chrome and blank content there, and
4.5.9 wants both paths identical from outside. The menu composes last by
re-composing its shown slot over any screen write that lands under it — one
`menu_reassert` on the three device-visible steps, both paths. The rules
engine lives in the beacon crate (the vocabulary owner hosts the verb table),
its file format is plumber-style, the ref is rc-single-quoted so the chosen
command acts on exactly what the menu displayed, and a template starting with
`#` is an internal action a production build drops at parse. The session tier
is deferred honestly: halcyond runs pre-login and has no `$home`; the
aurora-config precedent says the session tier arrives over the settings
channel, and BEACON.md now says so too.

**The proof.** The lever bake prepends a `wedge-test` rule; choosing it puts
halcyond to sleep for six seconds with the menu placed. The scenario then
sends Esc — the compositor's `dismissed (esc)` line is the only thing that can
say it — and types `ipwd` over the real keyboard while the owner is still
frozen. Those keys can only queue on the console surface if the grab is
gone; after the wake, `pwd` runs and its output-only path appears. Had the
grab stood, the keys would have sat on a menu surface halcyond later found
dead; had the compositor wedge-retired the frozen console, no output could
follow and a `WEDGED` line would be in the transcript (it must not be).

**What the survey found before a line was written.** The compositor retires
a surface on ctl `destroy`, conn teardown, or a wedge — a clunk is
bookkeeping. The H-3b close's fix for the chrome pool moved every tag bar
onto the renderer's shared session and dropped them with a `Drop` that only
closed fds. Every zoom leaked a server-side surface per dropped bar; the
renderer's cap of 36 fills after about seventeen zoom cycles, after which
every tag bar fails to mint. The H-3b verify ran one zoom and could not see
it. A resource freed by the replaced path's implicit cleanup must be freed by
the replacement: `Drop` now writes `destroy` on a shared-session surface, and
the scenario reads `surfaces 1` from `/dev/tapestry/ctl` after the zoom.

**What the lever caught, in order — three mechanisms, none of them the
menu.** Run one: the split-state Enter opened nothing, silently. Test-mode
diagnostics (a say of the resolution, mirrored into the very transcript
under test) showed the flat list at 56 rows on Esc and 61 on Enter: the
scenario's cue was the serial, halcyond's drain runs after its keys within a
pass, and Esc froze the cursor one command behind. Esc now drains first.
Run three: the menu composed as a solid black rectangle. `MenuSet::open`
painted, presented, placed, presented again — the weave slots rotate per
present, and the second present showed the next slot's zeros. Place first,
then one painted present. Run six: the menu opened, the Enter on it was
never processed, yet a frame-tick witness proved the loop alive. The
menu's redraw CONFIGURE had only ever been consumed at dismiss time, and
the same was true of every chrome tile since H-3b-3: a 9P session's replies
are read only by a thread inside a wait or an RPC on that session (ARCH
8.8.1.1's elected reader). halcyond's one thread parks on the console's
private session; the menu and the tiles live on the pane-tree session,
whose wire nobody reads until a reconcile's synchronous reads happen to.
While a menu is up halcyond now waits on the menu's ring — bounded by its
frame ticks and the dismiss's EOF — and polls the console. The tiles'
CONFIGUREs were never load-bearing (the reconcile repaints them), which is
why H-3b's witnesses could not see it. Each of the three was found by an
instrument, not a theory, and each instrument left a line in the transcript
that moved the thing it measured — the witnesses now read the compositor's
clamped rect and a fresh per-run report, and subtract the report's own row.

**A mistake worth recording.** A "syntax check" of the scenario by `expect -c
'source ...'` ran it: `lc_boot` spawned a real QEMU on the default image with
no lease held, and the pipeline's `head` left it orphaned for 36 s before a
PID kill. Any check that would run the file is a run; expect/Tcl syntax is
checked with `tclsh` + `info complete`.

Verified by the lines, jobs=1: beacon host 35/35, halcyond host 54/54;
ls-halcyon on the lever PASS [88 s] with every H-3c line; default
restored: ls-gfx-panes PASS [42 s] with the menu-gate negatives,
ls-gfx-age PASS [39 s].

### The H-3c audit close: a release that followed the wrong thing, and a synthesis that ran on the fallback (same run, after the third self-compaction)

The round ran as one holotype prosecutor with the explicit `model: fable`
override, and the override did what it did last time: every READ landed on
Fable 5.1 -- 72 turns, every file in scope, the scripture, the witnesses --
and at turn 73 the transcript's `model` field flipped to Opus 4.8 for the
rest: the host suites, the build, and the entire synthesis (21 turns). The
report's last line said `MODEL(end): Claude Fable 5.1`, exactly as the H-3b
round's did; the self-report is not a fallback detector and never will be.
The standing rule closes a fallback round that finishes, and the parallel
self-audit was the second independent read, so the round stands, with the
Opus synthesis named in the closed list rather than laundered.

**The P1 both readers found, from opposite ends.** The click-away's press
was swallowed as designed; its RELEASE was not. `ptr_btn` set the swallow
record and THEN called `menu_dismiss`, whose retire arm cleared the record
along with the menu -- so the release fell to the live routing and reached
the pane under the pointer as a button-up with no button-down. The lever's
click-away leg could not see it: halcyond ignores releases, and the leg's
negative ("no second menu placed") is satisfied by the press alone. The
prosecutor added the variant the self-audit had missed: a button pressed
INTO the menu and released after an Esc dismiss leaks the same way. The fix
is the chord layer's rule made general -- a release or a repeat follows its
press: the compositor records where every key and button press went (slot +
generation, in two tables because BTN_LEFT & 0xff is KEY_Q) and routes the
release there, dropping it if the surface retired. That also closes the
self-audit's stuck-key case (a key held before the grab whose release went
to the menu) and the stray-release case (a key pressed into the menu whose
release, after the dismiss, went to the leaf). The witness is the branch
itself: the compositor says both swallow lines from inside them, and the
leg expects both.

**What each reader found alone.** The prosecutor: a placed menu with nothing
hosted under it left the scanout Off (an invisible grab; unreachable with
halcyond); a verb-rich type asked for a surface taller than the display and
got NO menu (the compositor refuses it) -- now capped and scrolling; the
wheel could wedge a frozen owner's menu (now summed); `--` where the
programs take it (the prosecutor assumed hexdump did; a grep said no -- the
rest is queued); the chord swallow-set aliased key codes past 255. The
self-audit: re-placing the same menu healed nothing under its old rect; the
heal left the menu's pixels in the floor around a letterboxed surface, which
no client can repaint; a chosen verb was APPENDED to a half-typed prompt
line (`echo fo` + `ls` ran `echo fols -l ...`) -- halcyond now feeds ^E ^U
first, and the lever's new command-path leg proves it (`menu ran: ls -l --
'/lib/aurora/<ref>'`, the draft's echo present, `halfls` absent); and the one
that is not a menu bug at all: a menu opened on a Direct console blinked the
console BLACK for one pass, because entering Composed is the structural
repaint and the structural repaint blanks every pane until its redraw lands
-- every split has done this since G-6. The repaint now pre-fills each pane
from its client's last-presented slot.

**The finding that became a chunk.** Reading the session-reader model for
the round, the self-audit generalized what the lever had measured for the
menu: the chrome tiles' events -- the same-size CONFIGURE on a focus-only
epoch, a resize offer, the orphan CLOSE -- complete only while some thread
is inside an RPC on the pane-tree session, and halcyond's loop waits on the
console's. A focus move between two NON-console panes re-keys no tag bar
until something else wakes a reconcile; the two-pane lever never exercises
it. The H-3b-3 contract ("pump reports a CONFIGURE so the caller
reconciles") is hollow. The right fix is io_uring's: one ring per thread --
a libtapestry event set with every surface on one session -- and it goes
BEFORE H-3d, whose status bar is another shared-session surface. Not a
per-pass poll; that is the workaround the operator's standing rule forbids.

Verified by the lines, jobs=1: halcyond host 55/55 (+1), beacon 35/35;
ls-halcyon on the lever PASS [97 s] with the two swallow lines
and the command-path leg; default restored (0 lever lines): ls-gfx-panes PASS [41 s] incl. `menu gate refuses place/dismiss from a non-renderer (E_PERM; none placed)` + the H-3b legs; ls-gfx-age PASS [40 s]; ls-gfx-font [67 s] / live [74 s] / mode [59 s] / mp [44 s] / osd [30 s] / osd-persist [56 s] / osd-push [35 s] PASS (the pre-fill touches every structural repaint, so the whole single-leaf CPU set re-ran); all at LS_CI_JOBS=1 LS_CI_ATTEMPTS=1.

### H-3c-2: the event set -- one ring, one session, and the kernel fact that made it necessary (same run)

The H-3c close had queued the chrome tiles' event latency as its own
chunk, ahead of H-3d, and the first thing the chunk did was read the
kernel: `loom_wait_for_completions` pumps the session of the ring's FIRST
in-flight op and no other, and a non-blocking `enter` on a non-SQPOLL ring
demuxes nothing at all. That is the whole mechanism behind the lever's
session-reader finding. With two rings on two sessions, the thread parked
on one ring never reads the other session; with ONE ring over two sessions
it would read only the session of whichever op happened to be first in
flight and starve the other, silently. So the design is not "a shared
ring" but "ONE session AND one ring per client" -- io_uring's one ring per
thread, with the session invariant made explicit because the kernel makes
it load-bearing.

`tapestry::EventRing` holds both. Every surface takes a slot: its event
queue, its place on the registered-handle table, a region of one staging
buffer. `wait` arms every idle slot's read and blocks once; `poll` only
submits. Presents became a synchronous write of the tpresent on the
present fid -- the compositor composes inside the write's dispatch, so the
Rwrite was always the recycle gate and the Loom WRITE bought nothing but a
second registered handle per surface (which would have halved the ring's
capacity to 32 surfaces against halcyond's 36 worst case). The slot
lifecycle is the I-7-shaped hazard the audit row names: a dropped surface
says `destroy` first so its in-flight read EOFs promptly, leaves the table
at once, and holds its slot RETIRING until that read's completion frees
it, so a stale completion can never write a re-minted surface's region.
halcyond's console, tiles and menu now share the ring; the loop blocks in
`ring.wait()` when the console's queue is empty, and the H-3c dance
(`service(block_first)` waiting on the menu's own ring while the console
was polled) is gone rather than generalized.

The witness is the case the two-pane lever could never reach: a second
split makes three leaves, and Super+Left / Super+Right between the two
non-console panes must re-key both bars each way -- a move that sends the
console nothing. The leg reads the bar rects off the pane tree through
the console (ids depend on the layout's history).

Verified by the lines, jobs=1: halcyond host 55/55; every libtapestry
client builds; ls-halcyon on the lever PASS [113 s] with the
3-leaf leg both ways; default restored: ls-gfx-panes PASS [42 s] (the battery's two surfaces on private rings; the menu-gate + pane-tree-gate legs), ls-gfx-age [40 s], font [66 s], live [74 s], mode [59 s], mp [44 s], osd [30 s], osd-persist [56 s], osd-push [35 s] all PASS (aurora on a private ring), 0 fails, all at LS_CI_JOBS=1 LS_CI_ATTEMPTS=1.

### H-3d: the status bar -- the display-level carve, and the audit that caught a byte-budget drift the self-audit could not (same run, after a self-compaction)

H-3d (`e3b5ba1e`, audit close `3587d8f2` + `9896c3c2`) closed the H-3 arc.
The status bar is not a pane: it is a display-level carve
(`Comp.status`), taken out of the layout height in `reconcile` so the tiling
tree never sees it, with a `role=status` chrome surface the renderer paints
into `status_rect`. ut reports the cwd (OSC 7) and marks each command
(`mark k=cmd`) so the bar can show where you are and what ran.

The audit (0 P0 / 0 P1 / 1 P2 / 4 P3, not dirty) found the one bug that
mattered: the transcript charges every stored block against a shared byte
budget (`stored_cost`), and eviction subtracts `dead.cost` from it -- but the
`Op::Mark k=cmd` arm charged only `open.cost`, not `stored_cost`. So each
command mark grew the block's cost without growing the budget it is later
subtracted from; after enough eviction the budget underflows toward zero and
`max_cost` never enforces again. The instructive part is HOW it slipped the
self-audit: I read `self.open.cost += t.len()` and concluded "accounted."
The invariant is not "the increment exists" -- it is "the PAIR moves
together" (`open.cost` and `stored_cost` in lockstep at every charge site),
and the self-audit verified the half it could see. The Fable prosecutor,
which had not watched me write it, read the eviction path and the charge
site together and saw the asymmetry. The fix mirrors `stored_cost` at the
mark site and adds a regression asserting `stored_cost == sum of live block
costs` (sabotage-checked: pre-fix 96 vs live 108). Recorded as the standing
lesson: verify the INVARIANT, not the line.

The reviewer flipped Fable -> Opus 4.8 mid-run; the JSONL `model` field
caught it (a subagent's self-report cannot see its own fallback). A finished
fallback round is closed -- no Fable re-run owed.

### H-4 opens: the layout-authority fork, a Fable consult, and the D decision (same run)

H-4 is layouts (save / reload / respawn). The hard part is not the format --
it is the AUTHORITY: who may rebuild a saved pane tree, and with whose
identity do the respawned programs run? I surfaced A/B/C/D to the operator,
who asked (aligned with Thylacine's values) that I spawn a Fable 5.1
consultation agent to help decide. That consult ran 200 turns with no
fallback, in parallel with my own analysis, and the two CONVERGED on D --
after the consult corrected three premises I had wrong:

1. I assumed a new kernel grant was needed to key pane authority on the user.
   The kernel ALREADY stamps `srv_peer_info.principal_id`; tapestryd just
   uses it nowhere yet. No new syscall / CAP / SPAWN_PERM.
2. I assumed the restorer needed renderer authority to build the empty
   skeleton. `actor_owns_subtree` is vacuously true on an all-empty subtree
   -- any client can arrange empty leaves -- so the real gap is narrower:
   placement + naming (a leaf the tool can target and tag).
3. I assumed SAVE could live in halcyond. halcyond is a pre-login SYSTEM
   process with no `$home`; the session layout is in the user's namespace,
   so SAVE must be the user's own tool.

My initial self-analysis (Option A: a login-spawned manager DAEMON plus a
new grant) was over-built; the tree already had the pieces. This is the
wrong turn worth recording: the consult's value was not a second opinion on
my design, it was catching that I had assigned costs to capabilities the
tree already provides -- verify the facts before pricing the options.

Option D (`e233d390`, scripture-first, no code): layout save/restore is a
user-authority session tool run as the user (the Plan 9 riostart / acme
Dump-Load idiom); tapestryd gains a `Session(principal)` actor keyed on the
already-stamped principal; the read side is the pane-file walk; placement is
a one-shot `claim` token (the Wayland xdg_activation / Fuchsia
ViewCreationToken shape). The operator ratified D and, deliberately,
same-principal mutual pane authority -- a program running as you may
close/refocus/rename your other tiles, strictly weaker than the same-owner
process kill I-26 already grants; the console (SYSTEM) and other users'
tiles stay protected. The operator also ratified the first-launch welcome:
the shipped device-tier default layout IS a self-demonstrating two-pane
Genera tour (a live rich transcript on the left whose runnable objs drive
the shell on the right) -- sell the differentiator by BEING it.

### H-4a-1: the layout format core (same run)

`cdce7f3f` (+ `f99dd6b1`): `libhalcyon::layout` -- the `halcyon-layout v1`
serializer plus a bounded, no-panic parser (MAX_DEPTH 32 / NODES 256 / TAG
1024; every malformed input returns an Err, never a panic, because a panic
in a no_std tool is a silent exit(1)). It lives in libhalcyon, not in either
consumer, because BOTH restore paths need it. libhalcyon host 11/11, clippy
0, rustfmt clean, device build clean. The fixup filled the hash and folded a
pre-existing theme.rs rustfmt drift that `cargo fmt -p libhalcyon` swept
(the whole-crate reformat trap -- watch the staged set).

### H-4a-2: the SAVE tool + the render_text bridge (same run, this entry)

`30551d8f`: `halcyon layout save <name>` -- the syscalling half. The
authority story is the whole point: the tool runs AS THE USER, takes no
capability and adds no server verb, and writes only the session tier
($HOME/lib/halcyon/layouts/<name>). It reads the compositor's live tree from
/dev/tapestry (the `layout` dump + each `pane/<id>/tag`) and folds it back
into a LayoutNode via the new `libhalcyon::layout::from_render_text`.

The one design call worth recording: render_text (tapestryd's introspection
dump) and our v1 save format share the depth-indented pre-order, differing
only in the per-row lead (render_text leads with the pane id + a focus `*`
and trails a geometry rect; our format leads with the mode/leaf token and
carries the tag). So rather than a second tree-builder, `from_render_text`
tokenizes render_text rows into the SAME `Row` stream `parse` produces and
both feed one `assemble` stack machine -- the child-count validation and the
tree assembly are written once. A leaf's tag is resolved through a closure
(the tool backs it with `pane/<id>/tag` reads; host tests back it with a
map), and a tag past MAX_TAG_LEN is dropped to empty rather than truncated,
so the fold always round-trips through serialize/parse.

The durable write is aurora's config::save discipline verbatim (the
gfx-status cfg-2a lesson): write-tmp, content fsync, atomic rename, then a
STRICT metadata fsync on the SAME OWRITE fd -- because the fid follows the
file across the rename and a fresh OREAD reopen would fail SYS_FSYNC's
RIGHT_WRITE gate, which is exactly how aurora's first two barrier attempts
silently failed. Non-audit-bearing (read + serialize + write, no new
authority). `layout restore` is H-4b (the Session actor + the claim token --
audit-bearing); the tool prints "not yet implemented" for it today.

## 2026-09-01 (run 15, Fable) — H-1c-2: the emitters + the --color=auto unification; the pipe-budget deadlock caught twice

Resumed from the run-14 self-compaction mid-H-1. The chunk: the four Beacon
emitters (BEACON.md §12.7 items 2–5) + the H-1 close prep.

**What landed** (one commit, hash in the close):

- **ls**: rich short mode wraps each name in `obj type=path` (cleaned absolute
  ref via the new `coreutils::path::abs`; strip(rich) == the plain listing
  byte-exactly); rich `-l` realizes a beacon `table` (`llrlll`, header row,
  obj name cells — no box; the renderer restyles). The long-parked
  `--color=auto` flip: `stdout_is_console()` → `libthyla_rs::stdout_is_terminal()`
  and the default `Always` → `Auto`.
- **grep**: `obj path` on the filename prefix (normal, `-o`, `-l` arms),
  `em class=strong` frames on match spans (byte-span emission via `beacon::wire`
  directly — grep lines may be non-UTF-8, so `Sink::text(&str)` was the wrong
  tool there). strip(rich) == the plain emission byte-exactly.
- **stat**: the block + `obj path` on the subject. §12.7's `table` op is
  DEFERRED (recorded in BEACON.md §12.5): the GNU block is deliberately not
  tabular, and a table realization would break the strip identity.
- **ps — did not exist and was built** (the §12.7 list assumed it; the
  chunk-completeness pull-forward). One atomic `/ctl/procs` read (the kernel
  renders the whole table under `g_proc_table_lock` — no readdir race);
  verbatim pass-through when unstyled (the ns discipline); boxed at cells
  (state colored by the REAL kernel vocabulary — ALIVE/ZOMBIE/STOPPED, checked
  against `devctl.c::procs_state_name` after first writing a fictional
  RUN/SLEEP set); a beacon table with `obj type=pid` at Rich; any row-parse
  failure degrades the WHOLE output to the verbatim text.
- **The unification sweep**: all 15 remaining `stdout_is_console` `true` stubs
  → the real probe (each stub's own comment promised exactly this swap), and
  all 17 color defaults → `Auto` — grep's `Never` included, because
  COREUTILS-THYLACINE-DESIGN names grep by name and ordains the end-state
  ("both unify to `auto`… the default is simply color iff a terminal"). The
  pickup card had scoped the default flip to ls only; the design doc's
  ordained end-state is the binding text, so the wider sweep is scripture
  compliance, not scope creep. Piped consumers were enumerated first: the
  joey netstat/nslookup/ping probes assert plain substrings (survive; get
  cleaner), the LS-CI stat assertion is substring-through-SGR (unchanged
  interactively — auto resolves ON at the console).
- Shared plumbing: `coreutils::path` (lexical normalize moved out of
  realpath.rs + the cwd-anchored `abs`), `coreutils::beacon_gate` (the
  per-bin tier resolution + the `SinkOut` adapter). Docs: BEACON.md §12.5
  H-1c-2 deviations (7 entries), the SPEC's consumer-#1 LANDED note, the
  COREUTILS-design UNIFIED note, the AUDIT-TRIGGERS.md `SYS_FD_DEVCLASS`
  row + the LS-8 BEACON TIER addendum, the CLAUDE.md index line.

**The wrong turn, caught twice — the pipe-budget deadlock.** The first
version of the coreutil-smoke Beacon legs used `ls /` and `ls -l /` as
subjects. coreutil-smoke's REAP-BEFORE-READ pattern (its own module header
documents it) deadlocks when a child's output exceeds `PIPE_BUF_SIZE` (4096):
the child blocks mid-write, `wait()` never returns, and the BOOT HANGS — a
timeout, not a red. `ls -l /` is ~8 KB today. The self-audit pass (run while
the first bake+suite was already in flight) caught it by arithmetic; the
suite then confirmed it empirically: the boot log ends at exactly
`ls auto pipe clean ok` (the 1.4 KB `ls /` leg fit) and the harness reported
`FAIL: timeout (300s)` — the very next leg was the 8 KB one. Fix: bounded
subjects (`/etc`, one entry, bounded forever) + the ps-rich leg re-measures
the raw snapshot first and SKIPS LOUDLY (a greppable marker, not a pass)
when the frame-multiplied estimate nears the cap — the proc table grows with
boot services, and a future hang would be the worst failure shape. Lesson
banked: **self-audit BEFORE launching the expensive gate, not while it
runs** — the flaw was findable by arithmetic before the bake; running the
audit in parallel cost one full bake+suite cycle (~12 min).

**The second wrong turn — the flat-ramfs subject.** The deadlock fix's first
replacement subject was `ls /etc` ("one entry, bounded forever") — and the
re-run failed 3/55 legs with `ls` exiting 1: **the boot ramfs is a FLAT
namespace** (devramfs_readdir's own header: root lists every cpio file +
the synth mount dirs; `/etc` is not a directory OBJECT, just a name prefix
of a flat file, so readdir on it returns -1). The extinction in that log
(`proc_pgtable_destroy` from `joey_run`) is joey's ordinary boot-gate
teardown after the smoke's exit 1 — not a kernel bug. The durable fix:
explicit FILE operands (`ls /version /welcome`) — bounded by ARGUMENT
COUNT, immune to both directory growth and the flat-namespace shape (and a
directory subject was doubly wrong: the only listable pre-pivot dirs are
root and the EMPTY synth mounts, and an empty listing would satisfy the
clean check vacuously — the #215 broken-fixture shape). The same failed run
banked real positives: grep/stat/ps rich legs ALL PASSED in-guest (the P1
strip identity on real spawns, the obj-pid table, the `/env/BEACON`
inheritance chain) — only the ls legs rode the bad subject.

**The audit round (0 P0 / 1 P1 / 0 P2 / 10 P3, Fable 5 start==end — NOT
dirty).** The P1 (F1) was the round's justification in one finding: **the
production tier transport was dead on arrival.** ut read the consctl mode
line through the INHERITED fd — one Spoor threaded joey→login→ut whose
offset every non-positioned mode write advances; by ut's read the offset
sat at ≥132 against a ≤67-byte line, so the read returned EOF every
session, `/env/BEACON` was never written, and zones never armed. Masked
because u-repl-test drives the rich arm directly (the wired-gate trap ONE
LAYER UP from where H-1c-1 guarded it) and no witness printed on the
silent path. The fix took three attempts to even be possible: `t_pread`
at 0 — killed by the `dev->seekable` gate (the RW-4 R2-F2 narrowing;
devdev is non-seekable, verified in `spoor_read_common` BEFORE shipping);
a fresh consctl open — killed by the I-27 attach gate (attach never
propagates; #94-B inherits the fd for exactly that reason); so the fix is
the **ungated read-only `/dev/beacon` leaf** (the `/dev/winsize`
precedent; BEACON.md §12.3's "no new leaf" bound revised by its own
revisit clause — the named consumer arrived carrying a proof). ut
fresh-opens it, exports ANY tier, and prints the
`ut: beacon <tier> exported` canary every session; ls-3a greps the canary
from its transcript (the regression); `devdev.beacon_leaf` pins the leaf.

The P3 set: F2 (drain-close reset reordered before the disarm — the
disarm is the successor renderer's open window), F3 (the depth-8 cap
ENFORCED in `parse()` rather than amending three scripture sites —
suppressed-pair counting, payload never eaten; converged with the
parallel self-audit's finding), F4 (`Sink::obj` guards on the ENCODED
ref length — the parser bounds the escaped field, so a raw-length guard
admitted frames every conforming parser then discards), F5 (12.4's
"(floor Cells)" dropped — always trusts the advertisement, never invents
one), F6 (OSC 1936 pinned ADOPTED; §3/§6 zone/mark wording aligned with
the normative registry), F8 (three stale comments), F9 (ALL ps smoke legs
now gate on a DIRECT `/ctl/procs` read — the measurement cannot itself
deadlock), F7/F10/F11 recorded → H-2 (the rich-forced E2E needs a rich
advertiser; the per-prompt tier re-read; the idle-delivery zone
attribution). Full dispositions: `memory/audit_h1_closed_list.md`.

**Two catches during the fix run, both the guards working.** (1) My new
kernel test double-unref'd the leaf Spoor (`devdev_open` is
`dev_simple_open` — it returns the SAME Spoor, not a clone) and
`spoor_unref`'s corruption check EXTINCTED the suite — a correct catch of
a test bug. (2) The stat_native arm I added for the leaf pair (0444 char
files, closing what the reviewer and I both read as the winsize fstat
gap) FAILED `devdev.winsize_leaf`, which had deliberately PINNED the
statless posture — not a gap but a pinned default (the #240 shape:
know which tests pin the negative you are about to flip). Kept the new
shape (a leaf that opens + reads should stat; no consumer keys on the
failure) and updated the pinned test + its three comment sites.

**The close**: `140db874` (the audit close; all fixes + docs) + `435161a2`
(hash fixup). ls-3a re-ran green WITH the canary leg (41 s, HVF). **PUSHED
both mirrors, ls-remote verified at `435161a2`** — 7 commits
(H-1a/b/c-1/c-2 + the run-14 journal + the close pair), the first push
since the concretization. The H-1 gate closed in full: suite 1435/1435,
smoke 55/55, beacon host 29/29, LS-CI 37/37 + the canary leg, audit round
1 NOT dirty, 0 EXTINCTION.

**Vault + close prep**: quaestor run on the changed paths — MIXED: the
kernel/cons/ls/build.sh prose is vault-owned (and the vault's
`sub-coreutils-presenters` dossier — "fifteen tools, and fifteen copies of
one stub" — is now wrong-in-subject: the stubs are real probes); the beacon
crate is UNOWNED (sweep filed). Deltas queued on yip 0034 note 2. The
`chg-2026-08-15-build-targets` PIN checked: the `usr_rs_bins` staging list
(where `ps` was added) is not in its co-update set. The audit-round prompt
drafted (scratchpad; spawns after the green commit).

## 2026-09-01 (run 14, Fable) — H-1 opens: the Beacon foundations, three sub-chunks landed

**The charter**: the operator's "Let's start and proceed ourselves" — H-1 per
BEACON.md §12, the build card written hours earlier by the concretization
pass. Effort max (verified). Landed this run, each green before the next:

- **H-1a @7cd1ab94** — `SYS_FD_DEVCLASS` (80, the June reservation held) +
  the consctl `beacon <tier>` verb (ARCH §23.5.4). The spec's open decision
  BOUND: the walked `/dev/cons` leaf normalizes to `'c'` via
  `devdev_fd_devclass` (only the cons DATA leaf; every other leaf 'd').
  Ground-truthing corrected two stale spec rows (the June dc table had 'C'
  and 'r' wrong) and one scripture claim (winsize never reset on detach —
  the tier resets at `cons_drain_close` + `cons_test_reset`; ARCH/BEACON
  corrected in the same commit). **The suite caught a real blast-radius
  miss on the first run**: the kernel's OWN consctl staging buffer
  (`devdev.c tmp[64]`) sat below the new 67-byte render floor — every
  consctl read EOF'd; the external-reader sweep (pouch 0021 buf[96],
  digit-walk parser, /dev/winsize leaf asserts) had been done and the
  same-file consumer missed (#254's enclosing-function lesson, relearned).
  1434/1434 + the joey five-arm E2E (`probe H1 ... OK`).
- **H-1b @5d638fec** — the `beacon` crate: the OSC 1936 wire (emit/parse/
  strip; foreign escapes are payload — aurora's 7770 passthrough tested),
  the Sink/Table per-tier API, and the cells relocation (boxd/color/palette
  moved verbatim, git 100%-rename; the 15-test host baseline reproduced
  exactly as 11 moved + 4 stayed). Three recorded deviations from the §12.5
  sketch added to scripture (explicit zone open/close — the shell's zones
  are not lexically scoped; em/obj payload-only at Cells; over-VALUE_MAX
  obj refs emit no frame). One self-caught bug pre-test (a garbled bounds
  check in pct_decode); one test-design error caught by the crate's own
  debug_assert (the at-cap frame must be raw-built — the emitter correctly
  refuses over-cap values).
- **H-1c-1 @04186229** — the tier plumbing: aurora advertises `cells`; ut
  reads the advertisement off its consctl fd, exports `/env/BEACON` (the
  Plan 9 way — `env_clone_into` copies to children; verified in-tree
  against builtin.rs's stale "envp does not exist" note), arms Repl zones
  iff rich AND stdout answers 'c'. The u-repl-test leg is the rich arm's
  REAL driver (nothing advertises rich until Halcyon — the wired-gate
  trap): the same script through rich and plain Repls, frames
  present/absent, strip(rich)==plain byte-identical, in-guest every boot.

**The wrong turn worth more than the wins**: H-1c-1's first build FAILED
(E0308) and the failure was invisible — the background chain's trailing
`echo EXIT: $?` reported the final grep's status, `&&` had silently skipped
test.sh, and the suite verdict I graded was the PREVIOUS run's
`build/test-boot.log` (green, stale). The #184 gauge class exactly. What
caught it: the missing `beacon zones OK` witness — the new leg's line was
absent from an "all OK" log, which is impossible for the new binary. The
unraveling ran: witness absent → binary strings clean → cpio strings clean
→ cpio mtime 7 min older than the binary → no bake lines in the build log →
"build failed, waiting for other jobs" at the log tail. Re-run reports each
exit separately; the log's mtime is checked before grading. **A chained
background command must never end in a status-masking echo, and a suite
verdict is only as fresh as the log's own timestamp.**

**Open at the boundary** (the compaction rides here): H-1c-2 — the four
emitters (ls rich table + obj + the `--color=auto` flip; grep; ps; stat) +
smoke legs; then the H-1 close: the AUDIT-TRIGGERS row + CLAUDE.md index
line, docs/memory, the focused audit round batched over H-1a..c-2
(holotype-reviewer, Fable-max), push both mirrors only after the round.
Nothing pushed yet — three commits await the close per the discipline.

---

## 2026-09-01 (run 13, Fable) — the Halcyon concretization pass: vision → implementation-grade

**The charter** (operator, same day as the kickoff): "research and detail the
architecture and implementation... in as much detail and rigor as you can, so
that after Fable quota is reached this week, a lesser model could comfortably
continue on it." The TAPESTRY §18 precedent applied deliberately: a
ground-truthed design pass between vision and build.

**The substrate survey** (every claim file:line-verified, none recalled) paid
for itself four times over:

1. **Aurora already swallows unknown OSC** (`vt.rs:332-352`, 256-byte cap,
   oversize-discard) and already owns a private OSC channel (**7770**, the
   config push) — so Beacon's degradation tier ships in code that exists, and
   the private-OSC pattern has in-tree precedent.
2. **The coreutils presentation layer is already a shared pure-no_std crate**
   (`usr/coreutils/src/`: boxd/color/palette + the crate-doc discipline) —
   the Beacon cells realization is a RELOCATION, not new code.
3. **The consctl winsize verb** (`cons.c:2054-2177`, staged/atomic/readback/
   reset-on-detach) is the exact template for the `beacon <tier>` verb.
4. **The process-architecture fork COLLAPSED under research** (the
   research-before-fork rule working as intended): no Rust-std-pouch lane
   exists; native-links-ported is an explicit escalation by doctrine;
   native-SPAWNS-ported is explicitly blessed — so **halcyond (native, all
   parsing/state) + a display list + two dumb executors** (in-process CPU
   floor over libtapestry; pouch `halcyon-gpu` vk executor later) is the
   only shape that is doctrine-clean, vk-capable, and Rust-safe at once.
   Made the call rather than asking; the rejected alternatives are recorded
   in HALCYON.md §13.1 so no future session relitigates blind.

**The finding that resequenced the arc**: the morning sketch put "vk pane
renderer" at H-2 — but a vk client in a *pane* requires the composed present
arm (only fullscreen binds DIRECT), while the CPU executor needs nothing
new. So: H-2 = transcript MVP on the CPU floor (usable Halcyon before any
compose), H-5 = compose (tapestryd-side, zero halcyond code), H-6 = the vk
executor. A second consequence: **the CPU executor IS the universal floor**,
so guest-lavapipe dropped off the critical path entirely (demoted to a
post-v1.0 CPU-3D curiosity) — the H-6 investigation the kickoff scheduled
became unnecessary within hours of being scheduled, which is what a
concretization pass is for.

**Landed (scripture only, no code)**: BEACON.md §12 (implementation-grade
H-1: the normative wire grammar + op registry with the five rules, the tier
mechanism bound [no new leaf; the decision recorded], the SYS_FD_DEVCLASS
console-normalization decision BOUND ['c' both ways], the crate + relocation
plan, the ut hook sites by file:line, the five emitters, the P1-P3 test plan
[P1 = the strip property: strip(rich) == none byte-identical], audit
obligations, four open items); HALCYON.md §13 (the ground-truth table, the
§13.1 architecture verdict + rejections, the display-list v0 vocabulary, the
store-semantics/derive-pixels transcript model, the VT-core extraction plan
[vt.rs verified pure alloc], fonts/metrics-mixing/images, menus mechanics,
the exact layout save format [render_text + the two missing fields],
per-chunk audit obligations) + §11 resequenced; ARCH §17 update blockquote +
NEW §23.5.4 (the Beacon tier as the winsize contract's sibling); ROADMAP
§11.1-§11.9 REWRITTEN wholesale (the H-table with gates; exit criteria
amended with Beacon/presentation/compose/layout criteria; the §11.3 stale
PTY-exception note corrected — PTY is built and spec-gated; risks refreshed;
§11.9 gains the lavapipe/shaping/cells-realization lines).

**Open**: the §12.10/§13.9 named items (OSC number confirm, env name,
devpipe dc, fontdue no_std verify-at-vendor, VT crate + binary names, menu
verb grammar, layout read-side, JPEG vehicle) — all bounded, none blocking.
H-1 is ready to open cold.

---

## 2026-09-01 (run 12, Fable) — the Halcyon kickoff: the design conversation → scripture

**Not an autonomous run** — the operator-directed design session the W-4 close
scheduled. The flow ran exactly as declared: I presented the recorded Halcyon
state (the four-layer evolution stack — Phase-0 scroll buffer → the 2026-06-08
anti-window compositor → the 2026-06-15 two-environments statement → the
2026-07-17 §18 concretization — plus the honest tensions: "pure 2D" vs the
measured vk substrate; VISION §3.3's un-updated "there is no compositor"; the
Phase-0 §11.1 deliverables; the G-arc labels vs the as-built tree, with G-6
largely built and G-7 surpassed by the W-arc). The operator then delivered the
new vision.

**The vision (operator, verbatim intent)**: three sources — **i3wm** (tiling;
any tile runs a shell or any graphical app), **acme** (vertical tab stacking;
executable text), **Symbolics Genera** (the wild one: a rich textual shell
render — proportional + monospace mixed, programs communicating output through
a markup language that Halcyon renders richly and Aurora ignores or receives as
today's emission). Vulkan rendering (HW on Pi, lavapipe-lane on QEMU GL hosts).
DejaVu Sans Condensed as the proportional face. Mouse for focus, selection,
context menus, arrangement. A layout save/reload system.

**My analysis, all points ratified by the operator**: the three sources land at
three different layers (i3 → compositor, largely as-built; acme → chrome,
ratified; Genera → pane content, the genuinely new territory). For the markup —
the piece the operator explicitly asked for research on — the recommended shape
held: in-band OSC-framed (text-as-payload bracketing; the OSC 8/133 precedent;
degradation free because every VT parser already discards unknown OSC),
semantic-only closed vocabulary (renderers own typography via stylesheets),
`none|cells|rich` capability tiers with the isatty+tier emission gate, ut as a
relay-not-transformer (propagate the tier, emit its own zones, never parse
children), and presentations whose object references are **9P paths** with a
plumber-style verbs file — the Genera×acme×Plan-9 fusion, and machine-readable
ground truth for the agentic loop. Key transport proof: I-20 byte conservation
already certifies the frames survive the PTY plumbing.

**Findings along the way**: (1) "Bonfire" — I initially misread it as the
operator's name for the markup; it is the *palette* (UTOPIA-VISUAL U-2), and
the check surfaced that the coreutils **already speak a Bonfire visual
language** (box+SGR, COREUTILS-THYLACINE-DESIGN.md) — so the cells tier
already exists as code, and the emission library relocates it rather than
inventing it. (2) The same doc records that `--color=auto` is PARKED because a
console fd and a pipe fd are indistinguishable — the markup's emission gate
pulls the owed `SYS_FD_DEVCLASS` forward (one kernel mechanism, both gates).
(3) My best name candidate "Ember" was disqualified by the record — EMBER is a
Bonfire palette role. The operator chose **Beacon** from the candidate set
(Blaze/Beacon/Genus/Brindle). (4) Wine: an example only, post-1.0; the
Wayland-bridge arc is named in ROADMAP §12 rather than folded into Halcyon.

**Landed (this commit — scripture only, no code)**: `docs/HALCYON.md` born (the
environment: three layers, vk rendering + the guest-lavapipe open question,
typography + the paper-light "two lights" theme identity, the two pane classes,
presentations/menus/mouse, layouts-respawn-from-tags, scope, H-0..H-7
sequencing) + `docs/BEACON.md` born (the markup: OSC 1936 wire [proposal of
record], the v1 vocabulary, tiers + the gate, transport proofs, ut's three
obligations, verbs + the security clause, the migration). Reconciliation:
ROADMAP §11 EVOLVED (pure-2D retired; §11.1 superseded; Beacon named) + §12
Wayland line; VISION §3.3 rewritten + §9 non-goal reworded (the letter had
rotted; the intent — no overlapping windows — unchanged) + §14 superseded
banner; NOVEL Angle #4 second EVOLVED block (the Genera layer; the
presentations-in-a-POSIX-OS novel claim); TAPESTRY §14 tag-bar RATIFIED +
context-menu amendment + §17 pure-2D superseded; COREUTILS-design forward
pointer. **Open**: the OSC number + `BEACON` env name are proposals-of-record,
confirm at first implementation; H-1 (SYS_FD_DEVCLASS + the beacon library +
ut zones + first emitters) is the natural opening chunk, operator sequences.

---

## 2026-09-01 (run 11, Fable) — W-4 fork C: the bind+flush pair under one wait

**The charter**: the operator picked C from the §8.2 fork — batch the
steady-state present pair under one wait, no contract change. A (MAILBOX)
stays parked pending its semantics signoff; B stays unbuilt.

**The mechanism as landed** (`gpu.rs` + `server.rs`): a second synchronous
chain on the controlq — descriptor pair 34/35 (the first past the fenced
lane), small carves at the request/response region tails (RESP_REGION_LEN
0x1000 → 0xF00 funds the response carve; `get_capset` clamps to the
constant, so the change is self-adjusting and announced at the one
consumer that could ever collide). `submit_pair_and_wait` queues both
chains before waiting either; `drain` attributes head 34 explicitly,
BEFORE the fenced mapping that would alias it to the first out-of-pool
slot; the wait loop is the old one verbatim, extracted predicate-driven.
Two fused ops ride it: `set_scanout_blob_then_flush` (the vk rotated-poke
paint — the per-frame steady state) and `transfer_then_flush` (the
console's `screen_push`/`screen_flush_full`). **[Corrected at the round-10
close]**: the claim made here at landing — that the console path makes
every local boot a mass witness — was FALSE, discovered when the
engagement witness stayed silent through a green suite: the direct-mode
console paints through the unconverted weave loop, and `screen_push` is
composed-mode machinery, so no local boot reached a batched op at all.
The anomaly-grep "witness" had been a negative that cannot distinguish
running-clean from not-running. The fix makes the claim true instead of
retracting it: a boot-time pair-protocol selftest (two batched
GET_DISPLAY_INFOs — resource-free, side-effect-free, the full
two-in-flight machinery) now runs on every test-mode boot and fires the
witness say. The bind's verdict
semantics are §8.2's letter: checked first, a refusal latches exactly as
before, the already-queued flush is a no-op on a resource the display
does not reference. The GL weave-direct per-rect loop is deliberately NOT
converted: its census splits Xfer from FlushDirect, and under a shared
wait that attribution is meaningless — the instrumentation redesign is
named for the compose chunk.

**The prediction, pre-registered before the Pi run** (the double-paint
precedent). From run 6's measured state — PokeBind avg 11.6 ms is the
whole rotated paint (scanout wait + flush wait), and a lone flush massed
8–11 ms — the model says the batched pair completes in one flush-quantum:
saving ≈ 2.1 ms/frame. Point estimates: **linear 47.6 → ~52.9 (band
50–55); blit 51.3 → ~57.5 (band 54–60)**; the post-C PokeBind histogram
mass moves into the 8–11 bucket where lone flushes massed, the 11–14
bucket largely emptying. **The falsifier is explicit**: if the host paces
per COMMAND rather than per wait, the pair still pays two quanta — fps
unchanged (±1), histogram unmoved — and the direction goes to B/A with
that measurement in hand.

**Local bar before the measurement**: userspace build clean (no new
warnings in the touched files), suite exit-0 with the banner, arc gates
2/2, clade gates 3×PASS — the batched 2D pair ran the whole boot's
console painting with zero ring-corrupt or anomaly lines.

**MEASURED — the falsifier fired, and the failure is the finding.**
Linear 47.5 (was 47.6), blit 51.3 (identical), PokeBind avg 11.55 ms
(was 11.6), histogram mass unmoved — the exact pre-registered failure
signature. Both verdict halves PASS; resize and the 3 s restore green.
Before accepting it, the alternatives were re-derived: a stale binary is
excluded by the provenance chain (the local suite exercised the batched
console pair all boot before the chunk-verified sync), and no sequential
fallback exists in the code. The refined model — the only one consistent
with runs 4–6 AND this run: SET_SCANOUT_BLOB completes ~immediately; the
~10–11 ms quantum lives entirely in RESOURCE_FLUSH's completion; the
double-paint fix had already consumed the only removable quantum, and
§8.2's "wall halves" expectation described the pre-fix state. C is
retained (sound, free, and the pair primitive §8.3's GL item 1 needs,
where BOTH commands measure paced and batching should truly halve); the
vk wall's remaining directions are B/A, parked with the operator behind
compose. One observability gap became a fix: the measurement run could
not prove from its own log that the batched path executed — success is
silent by design — so a once-per-boot test-mode witness say now marks
the pair slot's first use. Also this window, operator-directed: the
GL-parity backport ledger landed as WSI-DESIGN §8.3 + the ROADMAP §11
addendum (compose opens under the Halcyon phase; the parity arc is
deferred behind it, not dropped) at `581de48e`.

**Round 10 (fork C + the carried-forward sweep, batched per the
doubled-cadence directive): 0 P0 / 0 P1 / 0 P2 / 4 P3 — NOT dirty,
Fable 5 start==end, all four fixed at the close.** The pair mechanism
survived every arm — the extraction proven exact line-by-line, drain
attribution enumerated over the whole id space, every error path
absorbed by the dead latch. The four: duplicated wire layouts against
the code's own one-copy rule (three shared builders now serve both
forms); no assert-at-the-hazard on the pair's runtime lengths (a future
resp2 overrun would have the DEVICE writing past the ring allocation —
plain asserts in both primitives now); the tyrquake gate's
libSDL2×tyr-glquake axis, the one input×output pair the sweep missed —
**the prosecutor correctly falsified run 10's own "closes the sweep"
claim**, and the record says so; and clade's verify conflating a
missing readelf with a failed binary (now a loud SKIPPED +
REUSED-UNVERIFIED arm). Every fix carries a discrimination proof.

**The same trap, twice in one day — and the structural answer.**
Probing F4's fix by running `build.sh clade` after editing build.sh
armed `build.sh -nt CMakeCache → rm -rf $bdir` and destroyed the
restored llvm-build tree a SECOND time — by the author of that
morning's own M-PIN. Two occurrences in one day settle it: this is not
a memory lapse to re-note, it is a structural hazard, and the fix is
structural — the destructive arm now REFUSES (loud,
CLADE_FORCE_RECONFIGURE=1 to override) whenever a structurally-valid
bin/llvm exists, because a recipe edit does not invalidate pulled
builder artifacts. Both guard arms proven (aged-but-valid → refusal,
tree intact; fresh → REUSED via the verify). The re-restore added its
own lesson: the keep's /tmp is cleared across stop/start, so a `$( )`
over a file there fails INVISIBLY inside a remote command — the first
re-pull silently shipped zero archives; the retry verified the tar's
member count remotely (73) before pulling, and the manifest check ran
against the local tree after. Full set restored: bin (md5-exact),
lib/clang, all 73 archives, include, CMakeCache, cxx-rt.

---

## 2026-09-01 (run 10, Fable) — the r9-F3 sibling sweep: every reuse gate made interrupt-sound, and the sweep's own probe destroys what it was protecting

**The charter**: the autonomous residue behind round 9's F3 — the "sibling
build functions' partial-output class" that the close TRACKED rather than
silently claimed fixed. The W-4 arc itself sits parked at its three
operator forks (the C/B/A wall mechanism, compose, the blit default flip);
none were touched.

**The class has two faces, and the survey found both.** Face one is r9-F3
as found: an interrupted final publish leaves a fresh-mtime partial `$out`
every later mtime sweep trusts. Face two only appears in multi-output
functions: an interrupt *between* outputs leaves a mixed old/new set, each
file individually intact — and every gate staleness-checked only one
representative, so the older sibling shipped as REUSED. tyrquake was the
clean example: inputs were compared against `tyr-quake` alone, so a build
killed between the two links served a stale `tyr-glquake` forever.

**What landed (tools/build.sh, one commit):** completion sentinels with
invalidate-first/commit-last discipline for the sysroot
(`build/sysroot/.complete` — the predicate's libc.a+builtins proxy
under-covered the artifact set: an interrupt after compiler-rt but before
libsodium/GL/thyla headers read as fresh) and the stratum host tools
(`build/host-stratum/.complete` — that one was a STUCK state: a truncated
cmake-built binary stays "fresh" until someone hand-deletes it); the SDL2
sentinel gained the missing invalidate-first half (it was written last —
correctly — but never removed first, so on a REBUILD the old sentinel
survived a killed `ar` and vouched for the partial archive); tyrquake got
atomic publishes for both links plus staleness against BOTH outputs;
gnumake an atomic publish; libcxx a publish-order fix (the gate keys on
libc++.a, which was cp'd FIRST — it now lands last, atomically, after the
completeness gate); clade's REUSED gate gained outd freshness terms and a
structural verify. Discrimination proven by fixtures BEFORE the real tree:
9/9 (both sentinels fire on absence, yield on presence, old logic intact
both sides; the tyrquake leg detects exactly the mixed set and stays quiet
on a coherent one).

**The verify arm's own negative control refuted it.** The first clade
verify used `readelf -h` — and the control showed a 1KB truncation of the
126MB multicall PASSES it, because -h reads only the 64-byte header. A
detector on an untested premise, caught by the fixture discipline before
shipping. Replaced with `-S` (lld places the section header table at EOF,
so any short-truncation fails — measured on 1KB and 60MB truncations),
with the residual class stated honestly in the comment: a killed mmap
writer can leave a full-size file with unflushed middles that -S cannot
see; the guest storm is that class's backstop.

**The wrong turn, in full, because the catch is the reusable part.** To
exercise the clade REUSED arm I touched the binaries newer than build.sh —
then edited build.sh again (the -S rewrite), invalidating my own setup —
then ran `build.sh clade` with a 2-minute timeout as the only brake. The
gate correctly fell through, hit the `build.sh -nt CMakeCache.txt` arm,
and `rm -rf`'d the certified Aug-5 keep-pulled llvm-build tree before the
timeout killed the configure. That is the recorded M-PIN — a capability
probe on a context something depends on — recurred on the very sweep that
was hardening against interrupted builds. Recovery: thyla-keep's
`/build/src/thylacine/build/clade/llvm-build` (START → scp → STOP, twice),
md5-verified both binaries; then the consumer set surfaced SERIALLY — the
reader-set lesson recurred — stage_clade wanted `lib/clang/22/include`
(243 headers, restored) and `cxx-rt/` (the #156 trio, restored from the
keep's stage2 sysroot, md5-matched), gl_link_program wants
`lib/libLLVM*.a`, the llvm-config shim wants `include/` + CMakeCache.txt
(restored). The last gap — the keep's Aug-24 tree lacked the 8
ExecutionEngine/JIT archives that llvm-libs.list (an Aug-5 meson closure)
names and llvmpipe's shader JIT links — was enqueued, then **closed in
the same run**: `ninja -n` on the keep's tree discriminated the two
readings of "absent" (excluded by config vs never demanded by the built
target list — it was the latter; the wrong reading would have re-closed
the list and laundered the 8 out permanently), the 8 were built there
(142 steps), pulled, 73/73 manifest closure, and the end-to-end witness
ran: a forced tyrquake rebuild relinked tyr-glquake through
gl_link_program against the full set (OSMesa entry points resolved),
re-stage + re-bake (zero staleness warnings), suite with the gl gate
certifying the reconciled-libs binary in-guest. The restored Aug-24
multicall is NOT byte-identical (post-strip) to the Aug-5 one the stage
carried — same fork tip (Jul 31), divergence is rebuild-level; the
re-staged tree + the storm/gl gates are its functional certification.

**The #250 recurrence, caught by baseline comparison.** The sentinel
births forced a one-time sysroot cascade (`build.sh all` run 1: REBUILT
down the line; run 2: every gate REUSED — including vkquake's hash-match
and clade's -S arm on the restored binaries). But the pool re-bake
defaulted `THYLACINE_BAKE_CLADE` unset, and the suite came back green with
`CLADE-GATES cl4=SKIPPED gl=SKIPPED storm=SKIPPED` — where every prior
window's log says `PASS PASS PASS`. A green suite that silently downgraded
three gates to skips is the fixture-mutation trap verbatim. Re-staged
(which also cleared the bake's announced staleness warnings for the two
GL provers rebuilt in the cascade), re-baked with CLADE=1 (payloads
verified PRESENT: GOROOT GOCACHE GO4C CLADE STORM QUAKE), re-ran the
suite.

**Also this window**: MEMORY.md pruned 19.7→18.5KB (the closed W-4 line
compressed to its inverted-comparison headline; three resolved lines
archived); yip 0009 closed from my side (aux's prose bye stood; formal
bye registered); the vault sweep material queued for the vault track
(build.sh + server.rs prose are vault-owned — this sweep is the FOURTH
instance of the sub-substrate-build dossier's "guard added after a stale
artifact shipped" genesis).

**The charter** (operator-ratified at the run-8 boundary): port vkQuake
fullscreen, measure FPS on thyla-pi against a FRESH glQuake baseline, hunt
impediments if vk lags, and gate the composed arm on the numbers. Mid-run the
operator added: register the proven port with aux's Forage/build-config system
when done (tracked in the pickup), and THE EFFORT GATE landed in both trees'
CLAUDE.md (main @454a01cb, aux @95529ac9, yip 0032/0033) with
`~/.claude/effort-report.sh` as its "am I at max?" instrument — whose first
honest reading exposed that `CLAUDE_CODE_EFFORT_LEVEL` in the env pins the
effective effort while the transcript stamps the setting (the operator was
experimenting; back at max now, and the util reports the override first).

**The port** (`third_party/vkquake` 1.05.3 + `third_party/volk`,
`usr/ports/vkquake`, `build_vkquake`): 1.05.3 chosen deliberately — the exact
vintage v3dv used for its own bring-up validation on this same V3D, a
single-threaded renderer (the FPS delta must measure the render+present path,
not a task scheduler), and engine-generation parity with tyrquake 0.71. The
loader story is the port's whole difference: no loader, no dlopen — volk over a
2-name filter (`thy_vkloader.c`: vkCreateDevice + vkGetDeviceProcAddr direct to
`vn_*`, the W-1 trampoline caveat; everything else to icd-gipa), and the
venus link set fetched from keep (mesa 26.1.6 headers + the witness link's
archive closure, thin archives repacked fat — `usr/ports/mesa/README.md`
carries the recipe).

**Three walls, each found at the cheapest layer that could have found it:**

1. **Run 1 (Pi): the SDL_VERSIONNUM overflow.** The legacy encoding
   (X*1000+Y*100+Z) overflows at minor >= 10: SDL 2.32.10 -> 5210 >=
   "3.0.0"=3000, so stock vkQuake refused a current SDL2 as "SDL3". Patch 0002,
   field-wise compares. A local 2D smoke was then built (~4 min/iteration vs
   ~10 on the Pi) and caught patch 0003 the same hour
   (`SDL_GetWindowWMInfo` Sys_Error on a backend with no native handle — the
   fetched global is write-only on every platform).
2. **Run 2 (Pi): the weak-dispatch-table extraction hole.** Mesa's GENERATED
   entrypoint tables reference every `vn_*` as WEAK-undefined; a weak undefined
   extracts no archive member; `vn_pipeline.o` linked as NULL table slots; the
   common-runtime fallback then misread vn-native structs
   (`vk_pipeline_layout_init`'s push_descriptor assert on layouts that never
   named a push descriptor — the flags were GARBAGE past `vn_object_base`). The
   witnesses never hit it: their ~50 hand-declared strong externs covered their
   own use sets, and upstream never can (a DSO has no extraction semantics).
   Fix: `--whole-archive libvulkan_virtio.a` ALONE — the fat repack is a
   flattened superset of the three runtime archives (80 members >= 49+1+3), so
   whole-archiving beside them collides. THE SAME ELF RULE, third strike this
   arc (OSMesa weak externs -> `-u vk_icd...` -> the generated tables). The
   `-u`-alone closure claim from r6-F5 is now MEASURED in passing.
3. **Run 3 (Pi): the holistic backing cap.** vkQuake's first 32 MiB texture
   heap hit `WARP_CTX_BACKING_MAX` = 64 MiB — the forward constraint the
   `MAX_WARP_IMGS_PER_CTX` rationale had recorded in advance. The holistic
   shape conflated two I-32 axes; split by PHYSICAL POOL: guest family
   (bos+rings+leaked, pins guest kernel memory) keeps 64 MiB;
   `WARP_CTX_HOSTMEM_MAX` = 192 MiB for mems+imgs (QEMU-window blobs, bounded
   by the 256M window physically — the cap is per-ctx fairness inside it). The
   prove's F2 leg was value-COUPLED (3x24 MiB had to refuse — a raise would
   fail the witness on its own arithmetic, the #230 class): reworked
   allocate-until-refusal, bound = 3x the window (mesa @6fde181, patch 0022).
   ctl publishes `hostmem-bytes-cap` + per-ctx `hostmem-bytes` so no client
   bakes the value in. venus-verdict 89/89; suite exit-0.

**Runs 4-6: the transport, the watcher, and the stall.** Run 4's attached ssh
died mid-stream (the cloudflare tunnel; "closed by remote host") and took the
ENTIRE record with it — the detached expect played the whole demo into a dead
pipe; `lc_step` writes only under LS_CI_STEPS. The `quake-vk` verb now runs the
remote expect nohup-detached into a remote file, polls, fetches: a lost
connection costs a retry, never the run. The first poll REBUILT the recorded
watcher defect verbatim (`while ssh ... kill -0` — a dropped connection reads
exactly like a dead pid) and the same tunnel drop caught it within the hour;
the poll now prints a VALUE, treats empty as no-information, and requires two
consecutive GONEs.

**The open stall (the hunt in progress).** Runs 5+6, deterministic: the game
inits fully, binds DIRECT (`scanout direct 1 img res 925` — vkQuake's menu on
the display through the presentable path), then the timedemo NEVER produces a
frame count: silence from `3 demo(s) in loop` (map-load territory: the first
big staging/texture uploads) to the fps timeout. ^C kills the game cleanly;
the console restore completes but ~40 s LATE (slow, not wedged). The one
post-mortem number: **`fenced-free 15` of 16 — one fence slot permanently in
flight after the ctx died.** Working hypothesis: a chain submitted around
map-load whose fence never signals; the game parks forever in a fence/acquire
wait. A candidate unconstructed state: the W-3d witness presented exactly 3
frames on a 3-image swapchain — image RECYCLING (acquire of a released image)
has never been exercised; vkQuake's 2-image chain needs it at frame 3. Also
caught by run 6: the exp's census read targeted the WARP ctl but the C-4 cost
rows live in the TAPESTRY ctl (P_CTL) — the poke-count discriminator is still
unread. Landed for the next runs: `fenced-held` ctl rows (slot + fence id +
ctx + ring + rb/comp + age_ms) so a wedged chain is NAMED, not inferred from a
missing count.

**State at this checkpoint** (the 600k line): everything above committed;
round 7 rides the W-4 close as batched, now carrying the stall hunt, the split
prosecution, the F1 resize driver, and the run-6 instrument lessons. The FPS
comparison has not begun: the number the charter wants is on the far side of
the stall.

**Post-checkpoint (the same day, across the self-compaction): the stall was
never a stall.** The hunt resolved it as TWO stacked instrument illusions with
zero GPU defects underneath — the paragraph above is preserved as the worked
example of both:

1. **The denominator phantom.** `fenced-free 15` was read as "one of 16 slots
   held", but `test_fenced_free` counts `0..COMP_FSLOT` = the FIFTEEN client
   slots (the reserved compositor slot reports via `rb-slot`): 15 IS the
   healthy all-free reading. The `fenced-held` rows built to name the wedged
   chain printed EMPTY — which is what exposed the premise (the #143 shape:
   the detector supplied its own reference). The ctl now prints a
   `fenced-pool` sibling row; a new key, not a `15/15` reformat, because
   warp-prove parses the existing key numerically (#91 consumer sweep).
2. **The real cause of zero fps: shareware content drops every `+command`.**
   vkQuake's johnfitz-rewritten `Cmd_StuffCmds_f` reads the `cmdline` CVAR,
   and `COM_CheckRegistered` sets that cvar ONLY on the registered arm — with
   shareware pak0 the `+timedemo demo1` was silently discarded, and the
   QuakeSpasm no-fitzmode `startdemos` path goes straight to `menu_main` (no
   demo loop). The game sat at the MAIN MENU rendering ~17 fps for entire
   runs. tyrquake parses `com_argv` and is immune — why the GL leg's
   `+timedemo` works on the same pak. **Patch 0004** mirrors the cvar-set
   onto the shareware arm.

The instrument that broke the case: the **warp-watch ledger** (tapestryd,
test-mode) — one say-line per live warp ctx per 30 s with the serve-loop pass
counter, park counts, and the per-ctx fence ledger
(`inflight/sig/rep/again/tl[0..3]/poisoned`). Its second sample
(`sig=906 inflight=0 fparked=0 tl=[906,0,0,0]`) proved the whole venus path
healthy and the game LIVE mid-"stall" — flipping the hunt from the GPU to the
game's command pipeline in one line. Two generalizable catches along the way:
serve-loop PASS COUNTS are not clocks (they meter pump activity — equal
counts at different wall times misled a cross-run comparison), and the
capture's chronology honesty differs by writer (tapestryd say-lines are
real-time; the boot probe's stdout is joey-buffered; the game's stdout is
line-buffered cons — 0029 made isatty true).

**With patch 0004: `969 frames 29.7 seconds 32.6 fps`** — the first vkQuake
timedemo figure through the whole stack (demo1, 1280x800, DIRECT scanout,
thyla-pi KVM/V3D 4.2.14). Landed alongside: the GL `quake` verb got the same
DETACHED transport as quake-vk (it was the last attached long-ssh
measurement); the vkq restore leg became measure-then-bound (the hard 6-tick
bound was GL-calibrated — the vk teardown additionally pays the venus
double-ctx-destroy and completed unforced seconds past it; the tick count is
now the reported datum with a 20-tick cap); and `warp-prove tctl` reads the
TAPESTRY ctl so the poke census stops reading the wrong tree. The same-day GL
baseline run was in flight as this was written; the comparison and round 7
ride the close. Commit: `309dd209` (+ the instrument follow-ups).

**The comparison + the A/B (the same night).** GL fresh baseline: **44.7 fps**
(969 frames, 21.7 s, gate VERIFIED). vk reproduced at **32.5** — 73%, with the
composed-stretch asymmetry biasing FOR vk, so the honest direct-vs-direct gap
is wider. The lag hunt's designed experiment landed the same night as one
build carrying both arms (mesa patch 0023 + vkquake patch 0005 + the two-leg
exp): the LINEAR presentable (the postprocess pass writes ~4 MB/frame into a
linear target on a tiled GPU) versus the BUFFER_BLIT chain (tiled images + a
per-present resolve-blit into the linear presentable, the I-40 bracket
covering the blit because it rides the present submit). The `-wsiblit` lever
rides argv because ut has NO env machinery at v1.0 — an in-process setenv
before Vulkan init is all mesa's getenv needs.

**The A/B's first run**: leg A (linear) reproduced **32.7** — the third
consistent sample (32.6/32.5/32.7: the measurement is solid). Leg B (blit)
REFUSED at the presentable mint — `RESOURCE_CREATE_BLOB(HOST3D)
resp_type=0x1200` — before the engine ever initialized. The shaped cause sat
in my own design notes, written and then not done: wsi's default
`select_blit_dst_memory_type` picks a HOST_COHERENT type for CPU chains,
while the working linear arm registers device-local image memory; the
override is two lines plus a keep cycle. The experiment is unresolved, not
refuted — the harness (two legs, one boot) worked exactly as built, and leg
B failing loud at init is the gate doing its job.

**Round 7 (the batched audit): 0 P0 / 0 P1 / 3 P2 / 6 P3, NOT dirty.** The
I-32 split's core mechanics survived prosecution intact (table-derived sums
make half-charging structurally impossible). The findings clustered in the
batch's own instruments, and two are the project's recurring classes caught
red-handed: F1 — the fenced-held rows were inserted ABOVE the `w210` fixed
prefix, reintroducing the round-2 F6 snapshot blindness one screen above the
comment recording that lesson (rows that exist exactly when a wedge hunt runs
pushed the custody mirror past the 512-byte reader); F3 — the slot-arm
attempt-report we enqueued was confirmed with two additional gaps its fixed
sibling closed (no REFUSED say, no storm guard). F2 (the detached transports
trust the spawn's echo blindly) fired LIVE in a third flavor the same hour
the report landed: the spawn ssh HUNG ~17 minutes on the tunnel — the local
substitution never returned while the remote run proceeded healthily — a
flavor even the finding's three scenarios didn't name (drop, garbage, and
slow-run abandonment; not channel hang). Seven of nine findings fixed
in-session; F2 blocked on the running gate, F9 (crafted-log fixtures for the
quake-vk verdict) tracked for the next batch. The r6-F1 resize driver did NOT
ride W-4 — its second deferral is on the record; it rots on a third.

### The blit-arm iteration: the checkpoint's own fix was aimed at the wrong mechanism

The resume note carried a diagnosis for leg B's mint refusal — wsi's default
blit-dst memory selector picks HOST_COHERENT, the working linear arm registers
device-local, so override the selector — and the note's next-action block said
to build exactly that. **The log said otherwise, and reading it first saved the
keep cycle**: the refusal line's neighbor was `vkr: mem has been exported`
(`build/warp-quake-vk.log:3070` region), which is virglrenderer refusing a
SECOND blob export of the same memory. The real chain: wsi's CPU create_mem
(`wsi_create_cpu_buffer_image_mem`, wsi_common.c:3183) is the blit-context
alloc PLUS a `vkMapMemory` of the blit buffer for its CPU-copy present — and
under venus a first map lazily mints the memory's renderer bo, spending the
memory's ONE export before the registration's mint. Two sharpeners: (1) the
hypothesized fix would have made things WORSE twice over — device-local breaks
the very map that was minting the bo, and the mappable presentable mint NEEDS a
host-visible type, so the override attacked a requirement, not the defect; (2)
**the design doc had already named this failure mode** — WSI-DESIGN's W-3d
as-built record says "an eager bo consumes the export and registration refuses
(swapchain creation fails)" — the A/B's first build violated its own documented
one-export discipline, and the diagnosis was derivable from scripture without
the log. Fix (mesa fork `5c2dcbd`, squashed into patch 0023): a blit-arm
`create_mem` that is `wsi_create_buffer_blit_context` alone, no map — nothing
CPU-reads the buffer (the blit CB is the only writer; the display consumes it
host-side), and `cpu_map == NULL` is already destroy-guarded.

Two harness defects surfaced en route, one per side of the same lesson.
`build_vkquake`'s REUSED check swept sources + `venus-libs.list` but not the
venus ARCHIVES — a keep cycle that changes only `libvulkan_virtio.a` leaves the
list's mtime alone, and the check said REUSED on the first rebuild attempt (the
comment above it even claimed "a refreshed venus set must invalidate the
binary" — true about the intent, false about the test). Caught live because the
rebake was watched; the stale binary would have measured the OLD driver on the
Pi and returned a confident wrong experimental verdict. And twice in one window
a pipeline laundered an exit code: `format-patch ... | tail` swallowed a bad
flag's failure (regenerating nothing while printing nothing), and the A/B verb
itself — which correctly printed UNVERIFIED and exited 1 — was invoked as
`... | tail -25`, so the harness recorded exit 0. The verb was right; the
wrapper lied.

### Run 2 of the A/B: the fix is proven, the figure is loud, and the witness is missing

Leg A reproduced (32.2 fps — fourth consistent linear sample, and the in-boot
venus-prove/vk-sdl-prove controls passed on the new build). Leg B **ran**: the
mint refusal is gone, the swapchain minted + registered + played the full
969-frame demo — **12.1 s, 79.9 fps, 2.5x the linear arm and 1.8x the same-day
GL baseline**. Then the gate failed it anyway: `no img direct-switch say line
within 120s (blit leg)` — the demo ran UNDISPLAYED, so 79.9 is PROVISIONAL
(a never-promoted surface skips whatever the live-scanout arm costs).

The first theory — the console's `pending-direct 0` claim at legswap starving
later claimants, the known 30–40 s restore latency turned queue-blocking — fell
to leg A's own timeline within the hour: `pending-direct 1` fired only AFTER
the exp's Super+F chord, one line before promotion. A freshly launched game
pane sits in a TWO-pane composed layout, and `reconcile()` computes Direct only
for a single visible display-sized leaf — so without a zoom, Direct is never
even a candidate. The exp zoomed once, on the linear leg, on the design
assumption that pane zoom persists across the legswap; it dies with the game's
pane at ^C. Leg B never had a chord, so its surface could not be promoted no
matter what the server did. Fix: the chord fires per leg, and the Composed
arm's silent pending-clear got the same say-line the Off arm always had — the
silence is what let two wrong theories survive one log. Run 3 relaunched with
both. The restore-latency question itself stays open as its own item
(`bug_pending_direct_claim_starves_claimants.md` records the theory's rise and
fall; the r7 verified-list line "the promotion has NO time deferral" is now
half-refuted for the legswap shape and must not be leaned on).

### Run 3: the A/B answers — and the answer indicts a different suspect

**W-4 VK GATE: VERIFIED** (both legs witnessed through the direct-scanout
say; the restore leg came back in 2 s this time). Leg A 32.1 fps — the fifth
consistent linear sample. Leg B, displayed, **34.4 fps: +7%**. Section 8's own
rule says that does not close the gap → **no default flip**, and the
LINEAR-target hypothesis is refuted as the dominant cost. The valuable number
is the one run 2's accident produced: the SAME pipeline undisplayed ran
**79.9 fps** — render, blit, venus marshaling, throttle bracket, poke send,
all present — so the ~17 ms/frame separating 79.9 from 34.4 lives in the
direct-arm per-present display work behind `img_poke_complete`. That also
retro-explains §8's original suspects list: venus marshaling and
throttle-bracket serialization are in the 79.9 run and therefore exonerated as
dominant. The display-present path is the next measurement target. (A
side-benefit catch: the default-flip contingency was pre-checked while the run
booted, and would have broken venus-prove's no-eager-mint counter — the blit
chain's tiled-image allocs are unmarked, and UMA V3D's device-local types are
host-visible, so each image would eagerly mint a bo. Recorded in the pickup
for whenever a flip is actually warranted.)

### The poke census printed avg > max — and the instrument's fix was the bug

The run's `cost present-poke-img 1957 41734792 3393` row is arithmetically
impossible: `sum <= n * max` is a THEOREM of the one writer (`cost_add_ns`),
and 41.7 s over 1957 pokes averages 21.3 ms against a 3.4 ms max. The row was
real (raw in the log, no console splice), the print site divides both fields
by the same 1000, and only one code path writes the cell — which forces the
conclusion off the writer entirely: the READ is incoherent. The ctl file
composes FRESH on every read() and serves `[offset..]` of the new string, so
round 7's F6 fix (`read_string_all`, loop to EOF) reads chunk N from
generation N — digit-length drift in EARLIER lines shifts every later row's
offset window, and the assembled row splices two generations. **F6's fix
traded truncation for splicing**; the in-tree 511-byte-snapshot idiom was
never a quirk — one read can't cross a generation. Fix in tapestryd: the
offset-0 read of a regenerating text file (ctl, layout, warp ctl) pins the
composed bytes to the fid (`text_snaps`, cleared at clunk); later offsets
serve the pinned generation — the classic synthetic-file discipline. The fps
figures are untouched by this (game-side timedemo numbers relayed by tagged
say lines); the census re-measure rides the next Pi run, which the
display-present hunt needs anyway. Suite exit-0 on the fixed state;
venus-verdict 89/89.

### The follow-through: four instruments, one run to witness them

`97c79a7d` turned the two open hunts and two owed items into one Pi run's
worth of instrumentation. The poke census split (`poke-bind` / `poke-flush` —
the steady-state img poke is bind + flush EVERY frame, since a swapchain
rotates presentables and `bound_res` never matches; whichever row carries the
~17 ms/frame names the display-present bottleneck). The warp-watch line gained
`conn=`/`surf=` and the mint a test-gated say — the restore-latency hunt's
missing WHO; the boot's own self-test ctxs immediately witnessed the fields
(and printed the u64::MAX selftest sentinel raw, fixed to `conn=selftest` the
same hour). The r6-F1 resize driver landed as the exp's third phase — unzoom
with the composed transition REQUIRED (proof the chord landed and the scanout
actually left direct; without that witness a later direct say would be the old
state, the #184 gauge rule), then re-zoom requiring the recreated swapchain to
re-promote, which is exactly where the audited wedge would bite. And F9
closed: the quake-vk verdict extracted to a file-driven verb
(one-implementation-two-callers — it had only ever run at the tail of a
~25-minute remote run) with five fixtures whose strings are copied from run
3's real fetched log; venus-verdict 94/94. Second suite exit-0. Run 4
launched to witness all four.

### Run 4: everything witnesses — and the impossible triple's REAL cause surfaces

**W-4 VK GATE: VERIFIED again** (linear 32.4 / blit 34.1 — sixth and fourth
consistent samples), and the new instruments all reported: the watch line's
`conn=`/`surf=` live from the boot's own self-test ctxs, **the resize driver
PASSED** (`re-promoted after unzoom/re-zoom — surface 1 img res 959`, a fresh
registration two generations past the demo's 957: the r6-F1 wedge did not
fire, and its owed regression leg is now a standing gate), and the poke
decomposition answered the display-present question: **poke-bind avg 11.6 ms
(n=1837) + poke-flush avg 10.5 ms (n=1959) ≈ 22 ms/present, split almost
evenly** — two synchronous display roundtrips per frame, each alone larger
than the 12.5 ms full render pipeline. The populations cross-check (whole
1963 = 1959 flushes + 4 promotion presents; 122 same-image pokes skipped the
rebind; the parts' sums total the whole's to within ~12 ms over the boot).

But the whole-row max printed `3` — impossible against the parts' 24943/16295
— WITH the generation pin live, which killed the splice attribution and
forced the byte-level read. `cat -e` on the fetched log settled it: the real
row was `... 41848671 3` + `6322` THREE LINES LATER, severed by the exp's own
tagged output — **the expect regex fired on a PARTIAL line** (serial output
arrives in chunks; `(\d+)` with no trailing anchor happily matches a prefix,
then the exp's `puts` interleaves into the capture mid-row). The
composed-screen exp documents this exact trap and calls its trailing `[\r\n]`
load-bearing; the cost-row captures lacked it, both runs. So the corrected
history: run 3's `max=3393` was also a truncation (real max ≥ 33931, which
makes that row arithmetically POSSIBLE), **the "impossible triple" was
capture-side all along, and the generation-splice attribution was wrong** —
though the pin it motivated stands on its own: the per-read regeneration is
real (the layout file's old comment documented the tear), the multi-chunk
read genuinely crosses generations without it, and run 4 ran with it live.
Fix: all three cost-row captures anchored to a complete line. The corrected
numbers: **poke max 36.3 ms; avg 21.3 ms — real, both runs agreeing on the
sum to within 0.3%.** The next design question (not this run's): why the
direct arm pays two ~10 ms display roundtrips per frame — whether the
SET_SCANOUT-class flip already implies the display update the flush then
repeats, and what QEMU's scanout/flush contract actually promises.

**Round 8 (the batched close: three commits + the mesa half): 0 P0 / 0 P1 /
1 P2 / 2 P3, NOT dirty** — and the audit-in-flight discipline paid in both
directions at once, which is the whole argument for it. The prosecutor found
F1: `drop_all_fids` (the Tversion session reset) clears the three sibling
cancel-lists and NOT the new `text_snaps` — the "die at clunk" contract false
on a surviving Conn, the exact sibling-omission pattern the reader-set-growth
lesson describes, sitting in the one fid-death path the self-audit didn't
open (it stopped at `fid_clunk`, which was correct). The self-audit found
SF-1, which the prosecutor read past: the new resize phase's failure arms
`lc_fail` IMMEDIATELY, eating the poke census — the glq deferred-verdict
lesson that the FPS and restore arms encode ONE SCREEN AWAY in the same
file. Two independent readers, two disjoint catches, same file. F3 (the
composed-transition expect also matched the `BIND FAILED` form) fixed by
anchoring to the success form; F2 (the staleness sweep is mtime-based; a
content-changed archive with a preserved-older mtime slips it) tracked with
the content-hash-stamp fix named. Close `4a0e7323`; suite exit-0 the fifth
time this window; venus-verdict 94/94; the push followed (`445d8798`, both
mirrors verified).

### The display wall: §8.2, the histogram verdict, and the double-paint hiding inside the bind

The residual arithmetic reframed the whole comparison — GL 44.7 fps is
22.4 ms/frame ≈ the SAME ~22 ms display wall + ~0.4 ms of render, so **both
engines were display-wall-bound** and the GL-vs-VK gap was mostly venus
residual. §8.2 landed as the seam record (scripture-first): the pacing-
quantization suspect, and the A/B/C mechanism fork pre-researched with
MAILBOX (option A) marked operator-signoff — the WSI advertises FIFO ONLY,
so vkQuake never had a choice. The discriminator (per-step latency
histograms, test-mode) went to the Pi as run 5 and came back decisive:
**zero of 3,796 steps under 8 ms** — bind massed at 8–14, flush at 8–11. A
hard floor under an idle-menu-to-full-scene workload is pacing, not work.
F2's content-hash stamp also landed en route, discrimination-proven three
legs including the mtime-preserving sabotage `-newer` is structurally blind
to (`528e4539`).

Then reading the bind with the quantization lens surfaced the sharper fact
the census had been pointing at: `direct_bind_adopted` **already flushes
internally on success** — so the steady-state rotated poke paid
set_scanout + flush + a SECOND redundant flush, a double paint of the same
res at the same geometry with nothing intervening. The bind arm was written
for the once-per-switch case; presentable rotation made it the steady state
— two individually-correct pieces composing into paying the ~10 ms
quantized roundtrip twice per frame, in the hot path (the recorded class,
live). Fix `69ff4cdd`: the outer flush runs only on the same-image re-poke
arm. **The prediction, stated before run 6 measures**: removing ~10 ms from
the wall puts leg A (linear) near 45–48 fps and leg B (blit) near 50–53 —
past the 44.7 GL baseline, whose Bo path never double-painted (single bo,
no per-frame rebind) and is therefore unchanged. If the numbers land there,
the pacing model holds and the residual single-flush ~10 ms floor becomes
the remaining target (options C/B/A). If they do not move, the double-paint
theory is wrong and the record will say so.

### Run 6 + the fresh baseline: the prediction lands inside both bands, and the comparison inverts

**Linear 47.6 (band 45–48). Blit 51.3 (band 50–53).** The census confirmed
the mechanism exactly — poke sum 41.8 s → 22.8 s (the removed flushes),
flush n 1958 → 144 (same-image re-pokes only), bind unchanged at 11.6 ms
avg carrying the paint; gate VERIFIED, the resize driver passing again,
restore 2 s. And because the headline now crossed a day boundary, the GL
baseline re-ran fresh (#236): **44.8** — yesterday's 44.7 reproducing to
0.2%, so the host is stable and the comparison is same-day honest:

| Path | fps | vs GL |
|---|---|---|
| GL — tyrquake/virgl direct | 44.8 | — |
| VK — vkQuake/venus, LINEAR direct | 47.6 | +6.3% |
| VK — vkQuake/venus, BUFFER_BLIT | **51.3** | **+14.5%** |

**The W-4 charter's question closes inverted**: the vk lag was never venus
— it was the display path paying a ~10 ms quantized roundtrip twice per
frame, plus FIFO-only pacing. One redundant-flush removal later, the first
Vulkan game on the Thylacine display outruns the GL port on the same
silicon. The remaining ~10 ms wall (the single quantized flush inside the
bind) is §8.2's C/B/A fork — C mechanical, A (MAILBOX) operator-gated —
and the compose gate's numbers-condition is now met, with the chunk itself
still the operator's call.

**Round 9 (the display-wall batch): 0 P0 / 0 P1 / 0 P2 / 3 P3, NOT dirty,
all three fixed at the close** (`02b3b7ee`). The load-bearing commit — the
double-paint fix — survived every prosecution arm: the prosecutor proved
from the diff that the removed flush only ever ran after the bind's
internal flush of the same res at the same geometry (pure repetition, no
protective role), swept all four `direct_bind_adopted` callers, and walked
the teardown chain to show nothing anywhere consumed a poke-time outer
flush. All three findings were residue around the stamp commit, each its
own small lesson: a hash guard added for soundness silently killed the
whole build when `lib/` was empty (a guard's own failure mode needs the
same fail-loud bar as what it guards); the stamp covered `lib/*` while
`venus-whole.list` — the closure-correctness input — sat in NO tier (the
category-vs-property class landing on the fix itself); and an interrupted
link left a fresh-mtime partial binary every tier then REUSED (the atomic
publish idiom, at last). Suite exit-0 the eighth time this window;
venus-verdict 94/94; the push follows, and the arc hands the C/B/A pacing
fork and the compose decision to the operator.

## 2026-08-31 (run 8, Fable) — W-3e: the SDL Vulkan glue, and the bind that had no trigger

Resumed from the run-7 self-compaction with W-3e fully designed and zero code.
The design survived contact almost intact — the five hooks, the weak externs,
the two-sided consent, the display-sized window — but the run's two real
findings were things no design pass had seen, and both are the class of thing
only building finds.

**Finding 1: the Direct bind had no trigger for a pure-Vulkan client.** The
W-3c-2 Direct arm completes the scanout switch at the surface's *next
present-COMPLETE* (the F16 rule), which for every existing client means a weave
tpresent — and `warp-prove img-direct` (the arm's only driver until now) binds
because it *presents the weave in a loop* while waiting
(`usr/warp-prove/src/main.rs:1072`). A pure-Vulkan SDL app never writes a
tpresent, so the poke would arm the consent, reconcile would set
`pending_direct`, and nothing would ever complete it — a dark pane with all
machinery green. The design answer landed as `img_poke_complete` (tapestryd):
**the `present-to … img` poke IS the img family's present-COMPLETE** — mesa's
`queue_present` issues it after the per-image throttle-fence wait, so the I-40
stage-0 bracket already orders it behind the frame's GPU work. The arm
completes a pending switch with the same say line the weave arm emits (the
gate's bind witness), then flips each newly-poked presentable silently — a say
per swapchain frame would be the C-0d storm — and the `flip_in_place` gate in
the verb handler keeps img→img-while-Direct out of the pending soft-Off route,
which would have re-run the switch (and its say) at frame rate. The poke also
advances the surface lifecycle (Woven→Live, presents, the #164 clock): it is a
real present, and a vk window that stayed "Woven forever" would have been a
lie waiting for a consumer.

**Finding 2: the linking model bit its own author.** The SDL hooks reference
venus through weak externs so GL-only programs still link — and the header
comment I wrote for that file states the flip side: a weak ref does not
extract archive members, so an SDL-only Vulkan app needs
`-u vk_icdGetInstanceProcAddr`. The witness is exactly such an app (it reaches
Vulkan through `SDL_Vulkan_GetVkGetInstanceProcAddr`, the honest shape), and
its first keep link produced `w vk_icdGetInstanceProcAddr` — unresolved weak,
NULL at run time, LoadLibrary would have reported no-ICD on every boot. The
`nm` census caught it before any boot did (two sibling `vk_icd*` symbols were
`T`, the one that matters was `w` — the census must read the SPECIFIC symbol,
not the family). The fix is the documented recipe applied to our own link
(`-Wl,-u,vk_icdGetInstanceProcAddr` in the witness's meson entry), which
converts the vkQuake recipe from documented-untested to proven.

Two smaller catches, both instrument-side: my patch-ASCII check read the rc of
`head`, not `grep` (the pipe-rc trap from this session's own trap list —
re-ran as `grep -c`); and the new display-half verdict grep was hollowed by
its own failure form — `scanout direct N img res R (WxH) bind REFUSED …`
shares the success line's prefix, so the unanchored pattern matched it (#240).
The anchor that fixes it is `[[:space:]]*$` rather than bare `$`, because the
serial capture may end lines CRLF and a bare `$` would be green on every
crafted fixture and red on every real boot — the crafted-log-blindness class,
caught at authoring time for once. Both directions now have sabotages
(venus-verdict 83 → 89, incl. the REFUSED-form and ABSENT-form arms).

Landed (thylacine `db855cbf` + mesa `997e371` = patch 0020): the
glue + wiring, the poke-completion, the warp-ctx-pub getter, the witness
(`thylacine-vk-sdl-prove`, linking the pouch `libSDL2.a` shipped to the keep
— the mesa cross env is the pouch sysroot, same musl + outline-atomics), the
joey probe + ramfs staging (venus-prove's twin, ABSENT-degrading on
no-display boots), and the gates (capture alternatives + both witness halves
required + 89 checks). Suite exit-0 local.

**The verdict, first run, VENUS GATE VERIFIED exit 0 both legs**: on the test
leg the vk window came up as the boot's sole surface (`pending-direct 0`),
armed the consent, and the first poke completed the switch —
`tapestryd: scanout direct 0 img res 893 (1280x800)` — **the first Vulkan
frame on the Thylacine display**, followed by the display-safe teardown
handing the screen back (`pending-direct 0` → `direct 0 slot 0`, the weave
arm re-taking) when the app exited. The control leg proved the glue's degrade
end-to-end on a 2D host (window created, extensions enumerated, stub
instance, ABSENT before CreateSurface) and carried neither witness half. One
chronology footnote worth keeping: the capture's line order LIES about
pre-pivot timing — joey's `smoke_drain` buffers child stdout and prints it
post-exit, while tapestryd's says are real-time; reading the control capture
without knowing that briefly suggested a pane-split (Composed) world in which
the DIRECT witness could never fire, and the code (`pane.rs host()` splits;
`server.rs:4865` clears pending silently on retire) plus the buffered-drain
fact resolved it before the test leg reported.

**Round 6** (Fable, start==end; batched r5-residue + W-3e): 0 P0 / 1 P1 /
0 P2 / 6 P3, all fixed, not dirty; the r5 residues all came back clean. The
P1 is the instructive one: my own concurrent self-audit had found the same
seam — the poke path missing the displaced-generation retire — and filed it
at P3 as a "bounded leak-until-close", having stopped at the geometry
mismatch making pokes inert. The prosecutor carried the chain two steps
further: the SDL events layer auto-acks resize offers for resizable
windows, `resize_ack` refuses new offers while `old_weave` stands (E_AGAIN,
which the client reads as a stale serial and drops), so the pin is also a
**permanent resize wedge** — one split/unsplit and a resizable vk window is
stuck at pane size, never Direct-eligible again, presents display-inert
forever, precisely the vkQuake-under-pane-management scenario this arc
exists for. Same facts, two more steps of chain, two severity classes
apart: the concrete value of the second prosecutor, recorded as the run's
second wrong-turn-caught. The fix is a shared guarded drain
(`release_displaced_gen`) at the top of the poke path and in all three
tpresent tails — the guard (skip while the displaced generation still
names `bound_res`) also closing F2's comment-right-for-the-wrong-reason
hazard. A regression driver for the resize path is owed at the next close
(the witness is fixed-size by construction; the leg needs the split/resize
choreography W-4 brings anyway). The mesa half: the present-poke dedup
latch — correct when the poke named a standing consent, wrong the moment
W-3e promoted the poke to the frame event — deleted, with a defensive
minImageCount clamp; plus three claim-precision fixes (F4-F6) and a census
row for the new present path (F7).

---

## 2026-08-31 (run 7, Fable) — W-3d: the mesa WSI DIRECT path, and the machinery that was already there

The self-compaction resume worked exactly as designed: the note said "CHECK
FIRST: `vn_wsi_present_async` before assuming the vtable queue_present path",
and that check reshaped the chunk before any code existed. The finding:
**venus already implements our stage-0 present bracket, for exactly our
backend class.** `vn_wsi_present_async` does not bypass the vtable — its
thread calls `wsi_common_queue_present`, and for a renderer with
`has_external_sync = has_implicit_fencing = false` (ours; the vtest class)
every `vn_QueueSubmit*` ends with `vn_wsi_fence_wait` (vn_queue.c:1270),
which detects the async-present tid and does the empty-submit + fence +
synchronous wait. So the design's "queue_present stage-0 = wait the present
semaphores (empty submit + fence + wait)" was already written by upstream —
our vtable `queue_present` keeps a belt-and-suspenders wait on the per-image
throttle fence only so the bracket survives `VN_PERF=no_async_present`.

The second recon finding cut the chunk's size in half: `wsi_image_info` has
`create_mem`/`finish_create` function-pointer hooks — the DESIGNED injection
points — so image creation rides stock `wsi_create_image` (CreateImage →
our marked alloc → BindImageMemory → our registration) instead of a
hand-rolled image path. One flag (`wants_linear`, which exists precisely to
force the CPU-config NO_BLIT LINEAR arm) makes the stock configure produce
exactly the image our registration needs.

The third finding was a live hazard, not a convenience:
`vn_AcquireNextImage2KHR` dereferences `mem->dedicated_img` and calls
`vn_renderer_bo_export_sync_file(renderer, mem->base_bo)` with our
`base_bo == NULL`. Traced to ground: the no-libdrm build's
`export_sync_file_internal` returns -1 without touching the bo, and fd = -1
is an EXPLICIT venus contract ("already signaled", vn_queue.c:1956) that
`vn_sync_valid_fd` admits — so the acquire semaphore/fence import degrades
to pre-signaled, which is correct for a free-list acquire. No code needed;
the dedicated-alloc requirement (which keeps `dedicated_img` non-NULL) was
load-bearing and is now commented as such.

One discovery flipped a build assumption: **the thylacine meson branch never
compiled `vn_wsi.c`** — every prior build ran the `VN_USE_WSI_PLATFORM`
stub inlines (vn_wsi.h), so all the machinery above was verified in source
but DORMANT in our binaries. Turning the define on also flips
`KHR_swapchain` and `can_sync2` onto `renderer_sync_fd.semaphore_importable`
— a HOST property venus queries through the ring. vkr passes
`KHR_external_semaphore_fd` through (vkr_common.c:98) and v3dv advertises
it, so the Pi resolves TRUE — but the prove step PROBES and FAILS rather
than skips on absence (#212), because a host without SYNC_FD semaphores
would otherwise silently un-advertise swapchains.

The chunk itself (mesa patch 0018, W-3d): `vn_wsi_thylacine.c` (the
headless-slot interface), the `'THLW'` chain-head marker routing marked
allocations to `alloc_simple` (no renderer bo — the ONE vkr export left for
`img/new`; both wrong orders fail loudly, which makes swapchain-creation
success itself the no-eager-mint proof), the `warp_img_*` client family
(the mem family's three-valued contract, with the E_INVAL and E_IO arms
re-derived rather than copied: E_INVAL covers duplicate AND validation for
img — indistinguishable, so inputs stay client-valid and the arm keeps;
E_IO includes the clean one-export refusal but is indistinguishable from a
transport EIO after a commit, so it reclaims), and prove step 10 (the
counter-delta no-mint proof with a nonzero-baseline positive control, a GPU
clear INTO a presentable read back pixel-exact, three presents through the
async path — the driver's first in-driver pthread on Thylacine).

Wrong turn, caught by the self-audit before any prosecutor: the
create-loop failure path destroyed the failed image TWICE (SA-1) —
`wsi_create_image`'s own fail arm already runs `wsi_destroy_image`, which
does not null the struct's handles, so the cleanup loop's `i <= image`
re-destroyed stale handles. Fixed to `i < image`; the failed image can
never be `registered` (finish_create is the last fallible step and sets the
flag only on success). The build was already green when this was found —
one rebuild folded it with the witness re-run, since a failure-path-only
fix still changes the shipped binary and a witness on a different binary is
a witness on a different binary.

Gates extended before the first real run (the round-4 F2 lesson applied in
advance): `wsi swapchain OK` joined the warp-host venus wkey loop (required
on TEST, forbidden on CONTROL) and test-venus-verdict grew to 83 checks
(missing / control-leak / replaced-by-FAIL). The venus verb re-ran on the
Pi under the extended gate the same session.

**Round 5 (batched: r4 fixes + the 1a probe + W-3d; Fable, start==end) came
back 0 P0 / 1 P1 / 1 P2 / 6 P3 — and the P1 inverted the arc's standing
capability verdict.** F1: the "real-class" compose probe had fired at the
first `wmem_mint` of every boot, which is the pre-READY self-test's
`mem_id=0` mint — the blob_id=0 SHM stand-in, the exact class the probe
existed to escape — and its one-shot was consumed before any client blob
existed. Run 6's `settype=latched` "measurement" (and the vkr-mechanism
story inferred from it) was therefore about the stand-in, AGAIN — the #95
disarmed-by-its-own-test class recurring on the stand-in lesson's own
remediation, proven from the certified boot log itself (the probe line
prints mid-self-test, res 74, 42 lines before the client exists). The
prosecutor could not reach the vendored virglrenderer source; this session
could, and confirmed its F7 suspicion with a sharper consequence: SET_TYPE
types its subject GLOBALLY and one-shot (vrend_renderer.c:13452 installs
`res->pipe_resource` on the global resource), which pinned the fix's shape
— the probe's subject must be a client MEM blob (never legitimately typed
again), never a presentable (whose future compose bind the bogus typing
would poison).

With the class gate landed (skip-without-consuming on `mem_id == 0` and on
a too-small first client mint), the re-measured probe — genuinely against
the first client VkDeviceMemory blob at last — reported
**`settype=ok blit=landed`**: v3dv answers the TRANSFER_SRC-buffer dma_buf
export query EXPORTABLE, vkr exports the blob as a dma_buf, vrend types
it, and the compositor's blit from it LANDS. **The composed arm is
host-AVAILABLE on thyla-pi** — the fork's contingency disposition
("windowed presentables await the v3d fork or option B") is void, replaced
by the fork's original resolution with its gate now green: the composed
arm is buildable as its own audit-bearing chunk, with the PDrained drain
in the same commit. F2 closed an app-reachable permanent wedge (WSI caps
advertised extents whose registration drew the img keep-arm — the
per-object bytes cap is now E_NOMEM-after-taken in both families, and the
caps clamp to 4096x4096 = exactly the 64 MiB budget at 4 B/px). The six
P3s: an episode-reset gap (F3), three record-precision defects on
soundness arguments (F4/F5/F7 — each now states the verified mechanism
instead of the convenient one), the settype attribution caveat pinned
(F6), and an upstream-inherited maintenance1 race documented (F8).

---

## 2026-08-31 (run 6, Fable) — the composed-arm fork dissolved at the source level

The operator brought Fable onto the arc to decide the open fork (run 5's
close: "a presentable is not blittable", three options). The fork did not
survive its first ground-truth pass — not because the measurement was wrong,
but because **it was a measurement of the stand-in class, and none of it
transfers to the class the fork was about**.

### The premise, prosecuted

W-3c-1's presentable is class-scoped to `blob_id=0` (gpu.rs:3154 — "the real
allocation case lands at W-3d"). Reading virglrenderer 1.1.0 — the exact
source of the Pi's shipped `1.1.0-2` — every measured negative is explained
*by the stand-in*:

- `vkr_context_create_resource` (vkr_context.c:347-353) shm-paths only
  `blob_id==0 && flags == USE_MAPPABLE`, **exact equality**. Any other flags
  fall into the device-memory path, which looks up object id 0 and refuses.
  W-3c-1's "SHAREABLE refused" was foreordained by the stand-in — it was
  never host policy. (The 4.1 amendment's operational content — mint
  MAPPABLE, never map — survives for both classes; vkr does not consult
  SHAREABLE on real memory at all.)
- vrend's attach parks any pipe-resource-less res in an **untyped list**
  ("defer to vrend_renderer_pipe_resource_set_type",
  vrend_renderer.c:13018) — which is also W-3a's attach-blindness,
  explained. A blit naming an untyped res fails `ctx_res_lookup` →
  `ILLEGAL_RESOURCE` → `ctx->in_error = true` (vrend_renderer.c:1131) →
  every later command refused. That IS `compose=noreadback`, mechanism and
  all.
- `pipe_resource_set_type` refuses `fd_type != DMABUF` — an SHM blob is
  categorically untypeable. The stand-in could never have passed.

### What the real class has instead

The designed path exists in-tree — the ChromeOS cross-context compositing
route: vkr **forces dmabuf export onto every HOST_VISIBLE allocation** when
the host driver supports it, even with no guest export info
(vkr_device_memory.c:274-356, the "XXX Force dma_buf/opaque fd export"
block) → the blob is a DMABUF-typed resource → attach to the compositor's
vrend ctx (parks untyped) → `PIPE_RESOURCE_SET_TYPE{format,bind,w,h,
modifier,strides}` → `virgl_egl_image_from_dmabuf` → EGLImage-backed
texture → the C-3 blit is an ordinary texture blit (the blitter handles
`egl_image` sources at 10696/11259).

Host conditions verified on the target, by discrimination, no boot spent:
`ENABLE_GBM = have_egl` (meson.build:283); the Pi's libvirglrenderer1 links
libgbm, carries the GBM-branch string "failed to create egl image" (2) and
lacks the disabled-branch "no EGL/GBM support" (0); v3dv 26.2.0 advertises
`VK_EXT_external_memory_dma_buf`; `GBM_FORMAT_ARGB8888 ↔
VIRGL_FORMAT_B8G8R8A8_UNORM` is in the conversion table
(vrend_winsys_gbm.c:103) — our exact declared format. And the design clicks:
**the img registration's declared shape (w/h/format/stride, built at
W-3c-1) is the SET_TYPE payload, field for field.**

### The disposition

**No scripture change — WARP-WSI-DESIGN §4 stands as ratified, both arms.**
Option B's *necessity* is refuted (no compositor-in-Vulkan re-architecture
required for windowed presentables; B remains a Halcyon-era quality option).
Option A's "composed has no implementation" framing is refuted. Option C's
measurement is delivered by source + binary verification; the end-to-end
conjunction gets its witness at W-3d slice 1 — re-run the landed compose
probe against the **first real-class blob**, plus a `settype` witness arm.
If a hop fails there, the outcome is A-with-proof at zero wasted work.

The W-2 dma-buf paradigm rejection is untouched: the dmabuf is host-internal
representation inside the trusted renderer (GPU-DESIGN §9.2); no fd or
ambient authority crosses the guest ABI. On hosts without dmabuf export
(lavapipe), composed-for-presentables fail-closes to DIRECT-only as a
per-host capability verdict — the W-3a pattern, not a design narrowing.

Two constraints recorded for W-3d: `mem->exported` is **one-shot** ("a
memory can only be exported once", vkr_device_memory.c:487) — one mint per
memory, registration adopts; and swapchain images should mint LINEAR first
(the declared stride must match the dmabuf layout SET_TYPE imports).

Sequencing: **W-3c-2 = `present-to img` DIRECT arm** + adoption
generalization + display-MODE accept half + cross-conn I-45 leg — creates
no compose reader, so the `PDrained` drain is not yet owed; **W-3d slice 1**
= the first real blob + the decisive re-probe + the composed arm **with the
drain in the same commit** (the landmine rule).

The generalizable lesson, filed under the run-5 probe's own discipline: **a
stand-in-class measurement does not transfer to the real class** — the
probe's three-way verdict and controls were exactly right, and still the
conclusion drawn from them quietly widened from "this blob" to "this class".
The check that caught it cost one source read and zero boots.

### W-3c-2 in the same run: the Direct arm, resequenced honestly

With the fork dissolved, the chunk landed as DIRECT-only by design rather
than by limitation: `present-to` generalized to two PUB-keyed families
(`PresentSrc {Bo, Img}` — an img *handle* resolves to its pub id at the
verb, so a freed handle's later tenant can never inherit a consent),
`gl_adoption` grew the img arm (the display-MODE half of the accept set —
round-2 F13 discharged where the bind is chosen, not at registration), and
`direct_bind_adopted` became the one copy of the family dispatch
(`SET_SCANOUT` | `SET_SCANOUT_BLOB` at the declared shape — the spec's
`PPresentBind`).

The load-bearing negative is sharper than the positive: **every
composed-machinery consumer is hard-gated to the Bo family**, because
`rb_issue` host-DMA-writes into `g.va` and an img adoption's `va` is 0 —
the gate is memory safety wearing a sequencing hat. `same_adoption` gained
a kind pin for the same reason a bare pub compare is unsound: img and bo
pub ids are independent monotonic sequences. And the `PDrained` claim was
re-derived rather than inherited: the Direct adoption reads `imgs` but
creates no `pinflight` member (a standing binding tracked by
`Comp.bound_res`, completed inside one dispatch), so the drain stays owed
by the W-3d compose arm, in the same commit as that arm — stated now in
the trigger row, the teardown comment, the WSI §7 record, and the spec
map, so the obligation cannot be dropped by any single stale copy.

Two drivers landed with the chunk, because a gated path with no driver is
not a gate: `img_prove` gained the cross-conn I-45 leg (round-2 F10's one
undriven property — a foreign conn resolves neither info nor a consent,
and the probe must not damage the owner's object), and `warp-prove
img-direct` + the extended `warp-img.exp` drive the whole Direct
choreography — mutual adoption, the zoom chord, the bind observed from
BOTH vantages (the compositor's `scanout direct N img res R` say line and
the guest's `bound` field in `img/0/info` — two vantages, one fact),
destroy-WHILE-BOUND (the display-safe teardown's first client-driven,
repeatable execution — exactly the path round 3's F2 fix anticipated), and
the weave arm re-taking the scanout. The `warp-host.sh img` verdict is a
four-witness conjunction; any subset can be produced by a partial run, the
conjunction cannot.

**Witnessed on real V3D, first run, both gates `VERIFIED`**: armed → the
composed-arm deferred one-shot (live, pre-zoom — a bonus witness nobody
asserted) → `scanout direct 1 img res 896 (1280x800)` → `bound observed`
(the guest vantage) → destroy-while-bound → `scanout direct 1 slot 1` → PASS
→ console heal. Suite 1432/1432; `test-venus-verdict` 75/75.

And the boot handed back an independent confirmation of the fork
resolution: QEMU's display layer logs `Failed to get v3d handle for dmabuf
N` + `eglCreateImageKHR failed` on **every** stand-in bind — including
W-3c-1's selftest on every venus boot before today, unremarked. The
stand-in is a *memfd*; the egl-headless scanout import wants a *dmabuf*;
the virtio-level response is OK regardless (`RESP_OK` is not the renderer's
verdict — the known pin, now witnessed one layer further out, at the
display). At W-3d the real class's dmabuf imports cleanly, and the absence
of these lines becomes a witness.

One small operational lesson re-earned: the first sync of the day hung 14
minutes on `ssh thyla-pi` — the LAN alias, wedged, exactly as the standing
note says (`thyla-pi-cf` ONLY). The note was in the resume note; the
muscle memory was not.

### Round 4, batched: the condemn net had one wired producer

The round (Fable, MODEL start==end, scope = r3 fixes + the W-3c-2a probe +
W-3c-2) returned 0 P0 / 2 P1 / 0 P2 / 2 P3, all fixed same-day.

**F1 is the kind of finding the batching exists to catch**: the
condemn/defer/drain machinery — built across three audit rounds
specifically to close `punbind_skipped` — had its producer wired at
exactly ONE of four display-disable sites. The three raw sites (reconcile's
Off arm, both `retire` arms) discarded the disable verdict and zeroed
`bound_res`, and the conn-teardown ordering (surfaces before warp ctxs)
then *blinded the guarded path behind them*: the later eviction scans
compared against the already-zeroed field, so `wimg_teardown`'s unref went
raw. A client dying with a bound presentable — the most ordinary teardown
path in the system — could hand the display freed memory, silently, with
all the machinery present in the tree. The fix is the machinery's own
lesson applied to its other half: `display_disable()` centralizes the
producer side exactly as round 2's F3 centralized the defer side. The
drill now exercises it; witnessed on real V3D post-fix.

**F2 is the fourth recurrence of the crafted-suite divergence, and this
run's own making**: the fork resolution changed what `noreadback` *means*
(attributable, control-gated ctx-latch — the expected stand-in outcome)
and left the venus certification gate coded to the old meaning — so the
gate was deterministically RED on the certified host, while the crafted
fixture asserted `compose=landed`, a token no host has ever produced. The
docs-only commit was a semantics change wearing a prose hat. Fixed by
making the attributable outcome its own token (`poisoned`), re-keying both
gate halves and the fixture, adding the landed-direction check (76/76),
and re-running the actual venus verb on thyla-pi — green, both boots, the
capset discrimination intact.

Round 4 also *confirmed* the load-bearing negative claims by independent
enumeration: the `PDrained` vacuity argument holds (every `imgs` reader
walked), the va-0 DMA class is closed at every composed consumer, and the
fork resolution's virglrenderer citations were all verified against the
vendored source. Dirty (2 P1s) → the fix prosecution rides round 5 with
W-3d slice 1.

### W-3d slice 1a, same run: the real class measured — and the composed arm is off for this host, at the designed formulation

The fork resolution ended with a contingency: if the SOTA formulation is
refused on the real class, "we land at A-with-proof at zero wasted work."
That arm was taken today, with the proof.

> **[SUPERSEDED — round-5 F1, run 7.]** Everything from here to the end of
> this run's 1a account measured the STAND-IN, not the real class: the
> probe fired at the first mint of the boot, which is the pre-READY
> self-test's `mem_id=0` mint, and the one-shot was consumed before any
> client blob existed. The corrected probe (run 7) measured
> `settype=ok blit=landed` — the composed arm IS host-available here. The
> paragraphs below stand as the record of a wrong turn, per this
> journal's own conventions.

The measurement needed zero mesa changes: the V-3b-3c-2b prove already
performs a real `vkAllocateMemory` → mem mint on every venus boot, so a
**one-shot server-side probe on the first post-READY mem mint** got the
existing prove as its driver. Three legs against a live client
VkDeviceMemory blob: control (`ctlok` — instrument proven), settype-only,
settype+blit. The verdict on real V3D: **`settype=latched blit=skipped`**
— and the host log named the exact branch:
`failed to dispatch PIPE_RESOURCE_SET_TYPE: 22` with *none* of the
function's loud error lines, which isolates the one silent EINVAL in
`vrend_renderer_pipe_resource_set_type`: **`fd_type != DMABUF`**.

The mechanism, completed in source: vkr gates dma_buf export on
`vkGetPhysicalDeviceExternalBufferProperties` for a TRANSFER_SRC *buffer*
(vkr_physical_device.c:188-228, its own "XXX ... workaround" comment), and
v3dv answers exportable-as-OPAQUE-only — so every venus allocation's blob
is opaque-fd-typed, `set_type` (DMABUF-only) refuses, and no guest-side
formulation escapes: the gbm fallback opens only when *neither* export
works, udmabuf is debug-gated, and a CROSS_DEVICE mint refuses outright
without dma_buf. The run-6 fork resolution's source-read was right about
the design and wrong about this host's traversal of it — the buffer-export
query is the hop nobody's citation covered, and only the end-to-end probe
found it.

Dispositions, per the resolution's own contingency: **no scripture
narrowing** — WSI §4.3 already framed the composed arm as a host
capability, and the probe now *is* the per-host gate, re-measured every
boot (on a dmabuf-for-buffers host it reports `settype=ok blit=landed`
and the composed arm lights up). Windowed presentables on this host wait
for the v3d fork (where the export gap becomes ours to close) or the
Halcyon-era option B. The Direct arm is untouched; W-3d proceeds as the
mesa WSI DIRECT path, and the `PDrained` landmine stays defused — no
compose reader gets built, so the drain stays owed by whichever future
chunk first builds one, on whichever host allows it.

## 2026-08-31 (run 5) — W-3c-1 re-witnessed, then round 2 found the hole between two round-1 fixes

Fresh context (self-compact at the 600k line, at the W-3c-1 committed
boundary). One chunk carried to close: re-witness the presentable on real
hardware under the corrected mint, run the owed round-2 audit, push. The
middle item is the one worth reading.

### The re-witness: the amendment measured a second time, by a second boot

W-3c-1 landed committed-but-unpushed because its central arms had never run
green. The boot that discovered the `USE_SHAREABLE` refusal *was* the boot
meant to witness the feature, so `mint=` / `bind=` / `unbind=` had only ever
been observed failing. A design amendment ratified on one measurement, with
the corrected code never itself measured, is a claim.

On thyla-pi (KVM, real V3D, `venus=on,blob=on,hostmem=256M`):

    tapestryd: warp presentable self-test: shape=1 mint=1 bind=1 unbind=ok
               disable=1 flags=mappable (64x64 BGRA8 stride 256)
    BOOT-smoke: PASS   THYLACINE-VENUS-PROVE PASS   EXIT=0

The load-bearing lines are the two immediately above the verdict:

    tapestryd: gpu RESOURCE_CREATE_BLOB(HOST3D) resp_type=0x1200 (expected 0x1100)
    tapestryd: gpu RESOURCE_CREATE_BLOB(HOST3D) resp_type=0x1200 (expected 0x1100)

— the flag probe's SHAREABLE and MAPPABLE|SHAREABLE arms being refused, with
MAPPABLE accepted third. The amendment's evidence is **re-derived on this
boot**, not inherited from the one that motivated it. The probe reports WHICH
combination the host took rather than merely that one did, which is the whole
reason a second measurement is worth having.

### A wrong turn, and what caught it

Verifying the re-baked ramfs carried the fixed tapestryd, a content probe said:

    flags=mappable : ABSENT  <-- BAKE DID NOT PICK UP THE FIX

It had not. `flags=mappable` is assembled at RUNTIME from a format string and
a `&str`; **it is a literal in no binary of any vintage**, so the probe could
not have succeeded against a correct artifact either. Its ABSENT verdict was
byte-identical to a real staleness report while carrying zero information —
a *fabricated* defect, the expensive direction, since acting on it means
re-running a bake, a ship and a boot to fix nothing.

What caught it was not care but **batch siblings**: two other probes in the
same call returned PRESENT, one of them a string that exists only in the
post-audit build. One odd member out of three is the tell — a stale artifact
fails all three. Rule adopted: a content probe needs literals only (never a
runtime composition), a positive sibling so all-absent is distinguishable from
a broken probe, and a negative control. The whole-file md5 answers a different
question — "is the far side the same bytes?" — and neither substitutes for the
other. Both sides matched at `61b745f1`.

### Round 2: the defect was in the space BETWEEN two correct fixes

`0 P0 / 1 P1 / 5 P2 / 9 P3`, all fixed. The P1 is not a coding error, and that
is what makes it worth recording.

Round 1 had produced two fixes on the same teardown, each right:

- **F5** rebuilt the unbind witness to observe the ISSUE ORDER, because an
  end-state check cannot distinguish an OMITTED unbind from an INVERTED one.
- **F8** exposed the device's SUCCESS verdict, because a live device can
  refuse the unbind.

Nothing wired the second into the first. `set_scanout` stamps its order tick
at ISSUE (`gpu.rs:2842`, before the wire response) and `gl_evict_res` cleared
`bound_res` either way — so on a REFUSED unbind all three conjuncts of
`destroy_ok && bound_res == 0 && ordered` held, and the arm whose doc-comment
says it "witnesses the modeled bug's ABSENCE" reported absence while
`buggy_punbind_skipped` was live on the device.

**When one round adds two mechanisms observing the same event, ask what each
actually answers and whether the consumer reads both.** F5 answers *was it
issued, in what order*. F8 answers *did it succeed*. The gate read only the
first. Neither finding was wrong; neither owned the hole between them. Found
independently by the concurrent self-audit and by the prosecutor, approaching
from opposite ends — "what does the verdict consume" versus "what does a
refusal satisfy".

Round 2's F3 then caught the *philosophical* half round 1 got wrong: F8 made
the refusal AUDIBLE, and audibility is not safety. The unref still ran, so the
implementation deliberately took the buggy-cfg's transition and announced it.
The refusal belongs to the device; **the unref was always ours to withhold.**
It now is: a refusal CONDEMNS the resource and `Gpu::resource_unref` defers on
it until an accepted scanout proves the display has moved on.

That guard is deliberately centralised on `Gpu` rather than added at the call
site, because round 2's F2 was precisely that the round-1 fix had been applied
**to the site the finding named rather than to the class** — two of three
`gl_evict_res` callers still dropped the verdict and then unref'd, both live
by construction. A guard every unref path passes through cannot be forgotten
by the next caller added; a fixed call site can.

### The stale-prose sweep that kept growing

Round 2's F4 — comments still asserting the superseded "SHAREABLE,
deliberately NON-MAPPABLE" mint — is the run's best lesson about sweeps. Three
passes, three different answers:

| Pass | Method | Found |
|---|---|---|
| self-audit | grep `shareable` in `server.rs` | 4 |
| self-audit again | grep `non-mappable` too | 5 (one site says only the second term) |
| prosecutor | its own read, including docs | 6 |
| tree-wide | both vocabularies, whole tree | **13** |

The last pass found `specs/tapestry_present.tla` (×2), `SPEC-TO-CODE.md`,
`ARCHITECTURE.md` — and **`CLAUDE.md` itself**, always-loaded binding
scripture asserting the flag posture the hardware refuses. Dangerous
specifically because it aims the next editor at the one change measurement
rules out.

**Sweep the PROPERTY through every vocabulary that can express it AND every
location that can hold it.** My first pass covered one vocabulary in one file;
my second covered two vocabularies in one file; only the tree-wide pass over
both covered the property. Cheapest form: grep each term separately and diff
the hit sets — a term that finds a site the others miss is proof the sweep is
not yet complete.

### Round 3 on Fable: both P1s inverted a claim the code made about itself

The reviewer-model rule prefers Fable for family diversity, and this surface
had gone two consecutive rounds on the implementation agent's own lineage.
Round 3 got Fable and immediately justified the preference — `0 P0 / 2 P1 /
2 P2 / 3 P3`, and neither P1 was a thing the code failed to say. Both were
things the code said *and got backwards*.

**F1 — the refusal net was capture-dead.** Round 2 moved the refusal `say!`
out of `wimg_teardown` and into `gl_evict_res`, which is correct: it now
serves every family, not just presentables. That move changed its prefix from
`tapestryd: warp presentable` to `tapestryd: warp display`, and boot-probe's
capture filter carries the first and not the second. So the new verdict check
could not fire on any real boot, while the crafted-log suite stayed green
because its sabotage appends the line by hand.

This is the **third** recurrence of the verdict/capture pairing defect on this
one surface — and the galling part is that I had verified the pairing in this
very session, *before* making the move, and left the comment asserting it
afterwards. The comment was true about a line that no longer existed. The rule
worth keeping: **a prefix change is a capture change.** Moving a say-line
between functions silently re-scopes it against every filter matching on
prefix. It now says so in boot-probe.sh, and the pairing is checked
mechanically with a negative control rather than by reading — my first attempt
at that check was itself broken (zsh has no `read -ra`), and reported "yes"
for the wrong reason on all three inputs.

**F2 — the overflow arm did not leak, it freed.** `resource_unref` deferred on
list *membership*. An overflowed `condemn` does not record the id — so the
unref went straight through, freeing a resource the device had just refused to
stop scanning. The `punbind_skipped` UAF the entire mechanism exists to close,
reached **at the mechanism's own boundary**, while its log line said "leaks
for the life of the process" and its comment said "leak it forever: the safe
direction." My own self-audit had graded this arm "real but ACCEPTED and
correctly signed."

Fable also did the work to show it was unreachable *today* (deriving
`condemned_n <= 1` structurally) and reachable at **W-3c-2**, because
`set_scanout_blob` was the one accepted bind that didn't drain, and W-3c-2 is
exactly what makes blob binds client-driven and repeatable. That is the
difference between "you have a bug" and "here is when it detonates."

**F3** then found that the round-2 drain had introduced the first
unref-before-quiesce path in tapestryd — freeing a retiring ctx's resource at
a moment chosen by an unrelated scanout. The fix generalises nicely: the
deferral now only ever **defers**, never **accelerates**. Each parked entry
records whether its owner actually asked for a free, and the drain issues only
those; anything parked at a pre-quiesce eviction is merely un-parked and freed
later by its owner, at its own safe moment.

### The sweep that would not finish

Round 2's stale-prose sweep reported 13 sites and wrote a lesson about
sweeping thoroughly. Round 3 found three it had missed — one of them *in the
amended scripture itself*, naming the function to edit — plus prose the round-2
batch had **newly written**, ten lines below its own sweep-lesson table. A
fourth pass with a third vocabulary then turned up a fourteenth in a spec cfg
that neither Fable nor I had reached.

    4 -> 5 -> 6 -> 13 -> 14

**A sweep is not done when it stops finding things. It is done when a
differently-shaped probe also stops finding things.**

### The mechanism nobody had ever run

Not a prosecutor finding — the self-audit's. The entire condemn/defer/drain
path had **no driver**. `condemn`, the deferral branch and `drain_condemned`
had never executed anywhere; `unbind=REFUSED` was a token only a `sed` in the
verdict suite had ever produced. A safety mechanism whose sole evidence is a
crafted log has been *described*, not tested — and both of round 3's P1s lived
on precisely that path.

It has a driver now: a lever that fails the next display disable without
issuing it, so the refusal is indistinguishable downstream from a real one.
The arm asserts refusal-observed, resource parked, **the free NOT ISSUED** (the
unref tick is the direct witness — checking the park alone would pass an
implementation that parked and freed anyway), and drained-for-real at the next
accepted scanout. It is deliberately not a client verb: the self-test runs
pre-READY, so it needs no external surface and cannot become the box-wide
kill-switch its `ring-inject` sibling is bounded against.

### Cadence change, on operator direction

Mid-run: *"this week let's focus more on progress — let's double the distance
between long gates and audits."* Recorded as binding. It changes the
**frequency**, not the bar: batch roughly twice the work per round, and carry
a dirty close's residue **forward** into the next round rather than spending a
whole round on the fixes alone. Round 4 therefore rides with W-3c-2 instead of
auditing round 3's fixes on their own. The thing to watch is that a wider
scope at the same effort reads each half less carefully — compensated by
naming focus areas harder in the prompt, not by hoping.

### W-3c-2a: the probe that earned its keep twice before producing an answer

W-3c-2 needed one thing settled first, and reading the code rather than the
design prose settled half of it immediately: `rb_issue` host-DMA-writes into a
resource's guest *backing* and `comp_readback_retired` reads it back through
`va`. A presentable has neither, by construction — that absence is what I-7
rests on and what W-3c-1 exists to establish. So the C-6 readback fallback is
not differently-parameterised for this class, it is **structurally
impossible**, and the composed arm reduces to GPU-blit-only. Whether *that*
works was unmeasured, so it got a probe rather than an assumption.

**Earned its keep the first time before it ran.** Building it surfaced a
constraint in the existing `BlitConv` probe: it runs on throwaway contexts and
never on `COMPOSITOR_CTX`, because **a request the renderer refuses latches the
context it ran on**. Acceptance is exactly what a capability probe measures —
so such a probe must never run on a context anything else depends on. On the
compositor context, the refusal this probe went on to measure would have taken
the display down for the whole boot.

**Earned it a second time on its first run**, which returned `noreadback` — the
*scaffolding* failed, not a verdict about the blit. A boolean probe ("did the
destination change? no → refused") would have reported a host capability as
absent, and I might have narrowed ratified scripture on it. The three-way
verdict refused to collapse that.

The fix was two controls rather than a guess. The destination moved to the
resource kind the conv probe demonstrably round-trips every boot — an unknown
in the *instrument* must not read as an answer about the *subject*. And a
no-blit **control one variable away** made the reading attributable at all:
without it, `noreadback` covers both "my instrument is broken" and "the blit
latched the context out from under the readback", which demand opposite
responses.

With both in place the discrimination was clean, on one host, one boot:

| arm | result |
|---|---|
| no blit at all (control) | staged pattern round-trips |
| blit from an ordinary resource | lands, rows readable (`blit-conv … CONFIRMED`) |
| blit from a **presentable** | readback writes nothing; ctx poisoned |

**A presentable is not blittable.** Likely mechanism — inferred, not measured
— is that a blob bound by `blob_id` to a venus allocation has no virgl texture
representation and is opaque to virgl's 3D pipeline.

### The fork that stopped the run

Both halves of `WARP-WSI-DESIGN` §4's composed arm are therefore unavailable
for this class: the blit is refused, the readback is impossible. A presentable
can be scanned out **Direct** (fullscreen — measured working every boot) but
not **Composed** (windowed). That is a narrowing of ratified scripture, so it
is the operator's call, not mine.

Prior art was gathered before surfacing, per the fork rule: Wayland
compositors import a **dmabuf** and sample it in their own API, which works
because both APIs agree on a representation — and the dma-buf path was already
**rejected at W-2** on paradigm grounds (Linux ambient authority does not map
onto per-Proc capabilities). With no dmabuf, the structurally available answer
is a compositor that speaks the client's API. Three options went to the
operator: direct-only now (unblocks vkQuake fullscreen, the arc's actual
target), composed-in-Venus (the Halcyon-on-vk substrate, a much larger arc), or
one more bounded probe to prove rather than infer that no formulation works.

The operator took the decision to **Fable**. Nothing was narrowed; the probe
and its measurement are landed at `c1261dd7`, and
`design_w3c2_composed_arm_fork.md` carries the options with the research
attached.

**Not lost in the handoff:** the `PDrained` landmine. `wimg_teardown`
implements `PUnbound` but not `PDrained`, and is sound *only* because no
submission path reads `imgs` — now enumerated four times independently. Any
chunk that adds a compose reader must add the drain **in the same commit**.
Option A creates no such reader; option B does.

### Left open, exactly

- **Round 3 is owed** — round 2 is itself a dirty close (a returned P1, and
  P1+P2 = 6). The prosecutor also flagged, correctly, that this I-40/I-45
  surface has now had **two consecutive rounds from the implementation agent's
  own lineage**; round 3 should get Fable if Fable is available at all.
- **A named landmine for W-3c-2**, from the self-audit rather than the
  prosecutor: `wimg_teardown` has NO drain. The spec requires `PUnbound` AND
  `PDrained`; only the first is implemented. It is sound today *only* because
  no submission path reads `imgs` (all readers enumerated, twice,
  independently). W-3c-2 adds the compose arm — the first such reader — and
  **must add the drain in the same commit**, because a green suite between
  them proves nothing: the only thing holding the invariant is an absence that
  commit removes.
- The cross-conn I-45 leg of the `img` ABI still has no runtime driver; the
  `ring-xproc` machinery it would reuse exists.

---

## 2026-08-26 (run 4) — W-3b: the presentable becomes a proved object before it becomes code

Fresh context (self-compact at the 600k line, at the W-3a shipped boundary).
One chunk, spec-only, exactly as the W-2 scripture ordered it: the
`tapestry_present.tla` FOURTH in-flight class lands TLC-green **before** any
W-3c server code exists to be wrong.

**The shape of the model was the work.** The presentable is not a weave arm —
it is a new object with no guest pages, so the thing its invariant protects is
inverted from everything the module held so far: not "the host must not touch
freed guest memory" but "the DISPLAY must not observe a destroyed host
resource" (`gl_evict_res`'s unref-of-a-scanned-out-resource, host-side UAF,
cross-client blast radius). Two observer arms (the standing `SET_SCANOUT_BLOB`
binding; the transient compose read), two holder refs (venus allocation,
registration), and the display-safe teardown as two omitted-conjunct sabotage
flags — one per direction, the house discipline. Three modeling calls worth
recording: the compose arms collapse to ONE read class (blit + readback source
— the readback's guest-page WRITE side stays the existing `inread` class, per
the design's own "the C-6 bookkeeping carries over unchanged"); `ServerDeath`
is atomic totality for this class alone, because its backing AND its observers
are all device-side and die in one reset — the weave arms deliberately keep
their in-flight classes across the crash precisely because guest pages have an
observer that outlives the reap window, and the asymmetry is the point; and
`pbound` is independent of `displayed` as a directional over-approximation
(both-held states only ADD enforced states).

**Measured, in order** (`b6c1mqcj9`/`bjpggvhq1` gate logs): the pre-change
baseline re-verified live (12/12, 5413/5413/94680/94680 — "re-verify any
figure before quoting" held); the extension in, all four pre-existing clean
counts reproduced EXACTLY; the all-features presentable pair explores
**1,557,073** distinct states green including both liveness properties; both
new buggy cfgs violate exactly `NoTornPresentable`, and the canonical 7-state
traces were captured single-worker and match the cfg comments action-for-action.
The composed pair is now PINNED at 94680 in `check-tapestry.sh` — the C-6
close had left it unpinned, so the fingerprint the gate can enforce got
stronger in the same stroke. Full gate: **6 clean + 10 buggy, 16/16 AS
CLAIMED**, ~15 min wall (the liveness leg on 1.55M states dominates).

**The audit round** (holotype Fable 5 max): **0/0/0/4 P3, all fixed, CLEAN in
one round.** The catch worth keeping: F1 — the Fairness comment said the
teardown sweep "reaps the client's refs", which is true for exactly one of
three real firers, and the wrong reading is a license for a W-3c server-side
venus-binding yank — the I-7 violation the whole holder discipline forbids. A
comment can be a UAF license. F2 was the same M-PIN one row over: the
additivity comment claimed count-reproduction for all twelve cfgs when the
gate's own header proves buggy counts are scheduler noise (counts for the
clean FOUR, verdicts for the buggy — the C-1 sibling sentence carried the same
imprecision and was fixed with it). F3 (the blit arm's cross-ctx attach
lifecycle off the record) was ALSO self-found in the parallel self-audit — the
W-3a coverage lesson ("enumerate every object the instrument names") held this
time. F4: `inread`'s comment still pinned the virgl-only source, and
`filled[g]` is a virgl-arm over-restriction for a presentable-sourced readback
— prosecuted to INERT (no invariant, no drain conjunct reads `filled`), kept
tight, revisit at the W-3c binding. The self-audit's own wrong turn is worth a
line: the proposed `~destroyReq` guard on `PRegister` was REFUTED by the
independent read — registration is ctx-scoped, so post-surface-destroy
registration is real behavior the guard would have deleted. Protocol miss
recorded: the agent never emitted `MODEL(start)` (MODEL(end)=Fable 5; round
accepted as finished per the standing rule).

Also brought current while in there: the ARCH section 25.2 spec-table row was
STALE at C-1 — it had never learned about C-6's readback class at all — and
`SPEC-TO-CODE.md` still said "7 buggy cfgs". Two doc rots caught by walking
the co-update surface instead of trusting it.

**Then W-3c-1, in the same run: the presentable stopped being a modeled
object and became a real one.** The chunk split cleanly in two, and the split
is worth recording because it was chosen for *witnessability*, not size:
**W-3c-1** is the object and its lifetime (the `img/` ABI, the HOST3D mint
— described as *shareable non-mappable* when this was written, amended to
`USE_MAPPABLE`-and-never-mapped by the measurement recorded below — the
display-safe teardown), which a server-side
boot self-test can witness end-to-end with no client at all; **W-3c-2** is the
client-facing present path (the generalized adoption, `present-to … img`, the
compose arm), which is also exactly where the spec's `PDrained` conjunct
becomes reachable code. Splitting the other way would have left half the
ordering rule with nothing to exercise it.

The design's own framing turned out to be the useful one while writing it:
this class is defined **as much by what it lacks** as by what it has. No guest
mapping — so no weft share, no hostmem offset, no reclaim park, no #847 dual
count. The entire mappable lifecycle its `WarpMem` sibling carries is absent,
and with it every hazard that lifecycle brings. What replaces them is one
hazard running the other way: the *display* holds a reference, so
`wimg_teardown` unbinds before it unrefs — reusing the existing `gl_evict_res`
rather than open-coding a second copy of an ordering rule, since two copies is
how one of them rots.

**Two self-audit catches, both before the prosecutor saw it, and both of the
same family — trusting a second copy of a fact.** The unbind was first written
gated on the per-object `bound` flag; that is the stale-flag direction, where a
stale FALSE skips the unbind and unrefs a live binding. The authoritative
record is `Comp.bound_res`, which `gl_evict_res` already self-guards on, so the
call is now unconditional: a redundant no-op costs nothing, trusting the copy
costs the display. Separately, the self-test drives the *real* teardown, so
`gl_evict_res` mutates the compositor's own scanout state machine on the way
through — both resting states are stable, but a witness that quietly leaves the
machine in a different state than it found has changed its subject, so it now
snapshots and restores.

**The witness is four arms, and the fourth is the point**: `shape=` (three
refusals one variable away, so the accept set is shown to be a gate rather than
a rubber stamp), `mint=`, `bind=`, and `unbind=` — destroy the presentable
**while the display is bound to it** and observe the binding dropped first.
That is the runtime twin of `tapestry_present_buggy_punbind_skipped.cfg`: it
witnesses the *modeled* bug's absence, not a generic teardown success. On a
host that refused the bind it reports `n/a`, never a pass.

**Two process notes.** The **Fable audit round died on credit exhaustion**
without producing a report; policy is that a round is never skipped for want of
Fable, so it re-spawned straight to the Opus fallback with the framing that
matters there — family diversity is *not* what a same-family prosecutor brings,
context independence is, so it must re-derive every load-bearing comment claim
rather than accept it (this code is comment-dense and several comments make
confident safety claims). And the **bake trap bit again, exactly as recorded**:
`build.sh disk` does not re-bake `ramfs.cpio`, so the first local boot ran the
*old* tapestryd — visible only because the W-3a probe's line was present while
the new self-test's was absent. Content-verify the artifact the consumer reads;
the md5 of the whole artifact is the honest check, and it caught the
pre-fix/post-fix distinction on the Pi ship too.

**Open onward**: W-3c-2 (the generalized adoption + `present-to … img` + the
compose arm, which brings `PDrained` into code) → W-3d (mesa `wsi_interface`)
→ W-3e (SDL Vulkan glue + the first Vulkan frame on the Thylacine display) →
W-4 (vkQuake). The spec's not-modeled list remains the W-3c-2 prosecutor's
checklist: the adoption gate, the attach lifecycle, the `filled` trigger shape.

---

## 2026-08-26 (run 3) — "can we test VkQuake?": the vkQuake arc opens; the first triangle renders

Fresh context (self-compact at the 600k line). The run began at a resting point
— the multi-queue chunk was closed and pushed (`3685cfd7`) — so the first act
was surfacing the next arc to the operator. Their question reframed it: **"Are
we in the state where we can test VkQuake?"** The honest answer was *not yet,
and here are the three gaps* — no WSI/swapchain (mesa's WSI layer is unbuilt for
Thylacine), no SDL2 Vulkan surface glue (the SDL port is GL-only), and — the one
that matters for correctness — **every GPU submission witnessed so far was
transfer-class**; vkQuake is the first thing that would push a render pass, a
SPIR-V pipeline, and a draw through venus. The operator voted the **vkQuake
arc**, with vkQuake itself as the eventual E2E exit criterion (the role tyr-quake
played for the GL side). It decomposes W-1 (pipeline witness) → W-2 (WSI design)
→ W-3 (mesa WSI + SDL glue) → W-4 (the port + gate).

**W-1 landed this session, and it did the thing the arc exists to de-risk: the
first render-pass/SPIR-V/draw traffic through the venus transport rendered a
triangle correctly on real V3D.** The prove's step 8 clears a 64×64 attachment
blue, rasterizes an embedded glslang triangle red, copies the image to a
host-visible buffer, and asserts **both** pixel classes — center red *and*
corner blue. The pair is the control, not decoration: an all-red readback means
the clear was lost, all-blue means the draw was lost, and only a correct render
passes both. It passed (`offscreen triangle OK ... center red, corner blue`).
The shaders were generated + `spirv-val`-clean on thyla-keep and embedded as
words; every graphics entrypoint the prove added is a defined `T` symbol in
`libvulkan_virtio.a` (the nm census — the link is the census), so the
loader-less direct-symbol link held and the build was clean first try.

W-1 also closed two of the multi-queue chunk's tracked witness gaps, on the
surface while it was fresh. The **F3** per-timeline retirement arithmetic was
shipped sound-by-*policy* (the copy proof rode one timeline; the lift leg never
submitted). Step 6b makes it sound-by-*witness*: fenced copies on two logical
devices (V3D is one queue family × one queue, so the second timeline needs a
second logical device), waited in **reverse** submit order — a misrouted
retirement leaves the stranded lane's fence unsignaled and the 10 s wait fails.
It passed. The **F2** no-burn proof was un-discriminating at 3/256 slots; step 9
allocates to refusal across three cycles with full frees between and asserts
cycles 2 and 3 refuse at **equal** counts — a burned handle or leaked slot
shrinks the third. Measured `255/255/255`, steady.

**The F9 convergence** is the run's only non-test code change. The multi-queue
chunk tracked a wedge: `warp_mem_new`'s maybe-installed failure arms (an E_IO
device fault under *global* hostmem exhaustion, an info-read failure) kept the
guest handle marked — a bounded leak that a cross-client fault loop could drive
to the 256-slot ceiling. W-1 converges them with a best-effort
`mem/<handle>/ctl` destroy: `-ENOENT` asserts the slot was never installed, a
full write confirms teardown, either freeing the handle; only a reclaim whose
own transport fails keeps it. The **duplicate `-EINVAL` arm is deliberately not
converged** — there the slot is a *live earlier mint*, and a destroy would tear
down real memory under it (the ring-F1 wedge's data-loss twin). The reclaim is
scoped to the connection's own ctx and keys "safe to free" on `-ENOENT`, which
is pinned `== 2` at the kernel registry, `p9::E_NOENT`, and the musl sysroot.
The F9 arm has no runtime witness — it needs cross-client global-pool pressure
the single-client prove cannot produce — so it is code-verified + self-audited,
and a focused Fable round on the reclaim path (I-45 device-memory surface) was
spawned.

**One wrong turn, caught by content-verification.** The first witness boot came
back green — but reading the *banner* showed it was the **old** multi-queue
prove, not W-1 (the banner ends "the multi-queue GPU-submit chunk", W-1's ends
"the vkQuake-arc W-1 pipeline witness", and none of the three new witness lines
were present). The cause: I shipped the new `ramfs.cpio` to `~/warp/` and
sha-verified it there, but `boot-probe` boots `$REPO/build/ramfs.cpio` via
`-initrd` (run-vm.sh:93) — a different path. **An md5 match on a sibling path is
a match on a file nothing boots.** The fix was to ship to the path the consumer
actually reads and content-verify *there* (`cpio -id` the prove out of the
booted cpio, `strings` for the new witnesses). The re-boot was green. The lesson
is filed in the pickup's HOW note: verify the artifact the consumer reads,
established from the consumer's own path variable — not a plausible-looking
sibling.

**The hostile-park analysis** (the last tracked multi-queue item, I-45) was
resolved against virglrenderer 1.1.0 source (the thyla-pi version), landed as
`GPU-DESIGN.md` §8.1. A malicious guest can submit `vkWaitRingSeqnoMESA` for a
seqno that never arrives; the finding is that virglrenderer already guards it —
the per-ring thread detects `tail < wait_seqno` and sets the context fatal,
waking the park, fatal to that context alone (`vkr_ring.c`). Our guest-exposure
half is intact (the timeline is server-derived from the owner-gated file name; a
client cannot name another's ring), and the residual shared surface (QEMU's
serial controlq) is the documented-trusted host half, ours to enforce only at
the v3d fork F3. No v1.0 code owed.

**W-2 — the operator's standard reset the recommendation, and the design was
ratified the same day.** My first WSI fork recommended the CPU-blit-into-a-weave
path (every mechanism proven, fastest to vkQuake). The operator's reply is now
binding feedback (`feedback_highest_standard_no_workarounds.md`): highest
standard, no workarounds, cost/speed not a factor, redoing built architecture is
welcome — and Halcyon may run on Vulkan. Under that bar the recommendation
inverts: the blit path's cheapness rests on vkQuake's tiny surface, exactly the
property Halcyon-at-full-resolution does not have — a workaround wearing a
"proven" badge. The dma-buf path fails for the opposite reason (paradigm: Linux
ambient-authority import/export does not map onto per-Proc capabilities). The
ratified design (`docs/WARP-WSI-DESIGN.md`) is the zero-copy unification: a
"presentable" object (a venus VkImage whose shape is declared at registration —
the Fuchsia sysmem lesson as one 9P verb), direct blob scanout fullscreen +
host-side compose windowed (both arms zero guest copies), one adoption model
absorbing the virgl-only `present-to` special case, the fourth
`tapestry_present.tla` in-flight class specced before code, and the
`gl_evict_res`-class display-safe teardown WarpMem never needed before. The
mechanism research that grounded it (two Explore agents, file:line-cited) found
every gap named and none architectural: the substrate (shared host pages,
blob_id binding, the C-3/C-6 compose machinery, the I-40 bracket) already
exists; what is missing is vocabulary (`SET_SCANOUT_BLOB`), typing
(`present-to` reads `bos[]` only), and the spec's fourth class. The one
external unknown — host `SET_SCANOUT_BLOB` support on this QEMU/virglrenderer
chain — is probe-gated (W-3a) with the no-guest-copy Composed fallback, so no
architecture rests on an unverified host claim.

**W-3a ran the same day, and its negative control earned its keep twice.** The
probe (a boot-time tapestryd self-test: mint a shmem-class HOST3D blob, hit
`SET_SCANOUT_BLOB` with a bogus id FIRST, then the real one, restore, then the
cross-ctx attach pair) came back `dispatch=present neg=0x1203 pos=0x1100
attach=0x1100 attach-neg=0x1100` on real V3D. The first half is the good news:
the bogus id drew exactly `INVALID_RESOURCE_ID`, so the vocabulary exists and
the probe can see a refusal — and the real blob's scanout was *accepted* (held
as acceptance, not pixels, per the RESP_OK-is-not-a-verdict lesson). The second
half is the negative control catching its own instrument: the attach leg's
bogus id was ALSO accepted, so `attach=OK` proves nothing — the host defers
resource resolution past attach, and the compose-arm capability question moves
to the blit-use at W-3c/W-3e. Without the paired negative that blindness would
have been recorded as a capability. Two smaller catches the same hour: the
ramfs scp to the pi silently truncated mid-transfer ("lost connection") and the
md5 mismatch caught it — while a content-grep of the truncated cpio PASSED
(tapestryd sits early in the archive), which is exactly why the verifier is
md5-of-the-whole-artifact, not a string probe; and the local curl was broken by
a stale emsdk CA path (`env -u SSL_CERT_FILE` bypassed it) when fetching
tla2tools. The W-3b baseline is pinned before any spec edit: 12/12 tapestry
cfgs as claimed, clean pair 5413 distinct states, composed pair 94680.

**The W-3a audit chain then caught the probe measuring a ghost.** Round 1's P1:
the cross-ctx attach legs ran before `COMPOSITOR_CTX` exists (its only creator
runs after READY), so `attach=OK` had measured the response's indifference to a
*nonexistent context* — the unconstructed-state class applied to the instrument
itself, and my parallel self-audit missed it (I checked the request-buffer
margins and the scanout state machine but never asked whether the object my
commands NAMED existed at probe time — the reusable lesson: an instrument's
preconditions are part of the unconstructed-state checklist). The fix
(`ensure_comp_ctx` first) re-measured byte-identical values against the real
ctx, which upgraded the blindness claim from artifact to finding: even with the
target present, a bogus resource draws OK — resource resolution is deferred
past attach, so the compose-arm capability is only observable at the blit-use.
Round 2 on the fixes came back clean (two P3s, landed: the BLIND-host
negative-leg bind can also leave restore residue; the gate needed its
replaced-by-SKIP sabotage — 55/55). W-3a closes with the Direct arm's
vocabulary CONFIRMED present and discriminating on the real chain.

---

## 2026-08-26 (run 2) — the multi-queue design ratified: four forks closed, two by operator vote

Fresh context (self-compact at the 600k line); the run's charter is the
GPU-submit chunk with multi-queue pulled forward, implementing against
`docs/WARP-MULTIQUEUE-DESIGN.md`. Scripture before code: the first act was the
design conversation on the doc's four open forks, not the implementation.

Two forks were mine to dispose of and were disposed inline: the F6/F8
per-renderer mutex is **pulled into the chunk** (concurrent multi-queue submits
make the torn-RMW live — the "pull dependencies forward" default, noted not
asked), and #210's per-ring FIFO assumption is **bounded at the I-45 audit**
(it is a risk to verify, not a decision to take). The other two went to the
operator, the second because a mesa<->tapestryd fence surface is an ABI fork
(escalation list): **queue count = 4 timelines / 3 queues** (CPU + gfx +
async-compute + async-transfer; `max_timeline_count = 4`), and **the fence ABI
= the Option-1+2 hybrid** — ring_idx on `FenceTag`, per-(ctx,ring) retirement
into a per-ring `fence_signaled`, exposed through the EXISTING
`ring/<ridx>/fence` file for host3d rings sourced from the GPU pump, with the
hard ring-flavor guard (echo vs pump must never cross) ratified as part of the
ABI, not an implementation nicety. Both votes matched the doc's
recommendations. The doc is flipped DESIGN FOUNDATION -> RATIFIED and section G
now records decisions, not questions.

Vault check on the surfaces (the step-0 discipline): the design doc is UNOWNED
(kept in-tree as today); `usr/tapestryd/src/{server,gpu}.rs` are OWNED by the
`sub-tapestryd` dossier (audit:hard) — so the implementation's mechanism prose
rings vault rather than growing a parallel reference section. The vault
checkout is 264 commits behind main; its sweep is the vault track's queue, not
this run's.

**The implementation, and the first GPU round-trip ever executed on
Thylacine.** The full stack landed in one pass — server (`FenceTag.ring_idx`
+ per-timeline `timeline_signaled[]` bumped alongside the ctx total, the
`ctx/<id>/timelines` file, `submit1..3`, INFO_RING_IDX on the virtio-gpu
header for nonzero timelines, the vindication carrying its lane per the #242
pattern) and client (per-timeline ledgers, the transport mutex + one-parker
condvar protocol, the non-parking venus submit, `max_timeline_count` 2→4
gated on the host capset). Witnessed GREEN on thyla-pi/KVM real V3D 4.2.14
in one boot: **`GPU round-trip OK (vkCmdCopyBuffer 4 KiB, fenced submit,
pattern survived FIRST map)` — the F1 reify-at-alloc fix-proof the V-3b-3c-2b
audit chain owed, and the first vkQueueSubmit ever completed on this OS** —
plus the F4 de-advertisement check (76 exts, complete scan), the timeline-2
lift (a second logical device, refused under the old count), and the F2
cap-exhaustion recovery cycle. Zero hazards. mesa `deed314`/patch 0015,
round-trip tree `0575a6189666` exact.

**The audit chain: two rounds, one sharp catch, converged.** Round 1 (Fable
5): 0/1/0/6 — the P1 was the shmem cache's *eviction* path
(`cache_add → remove_expired_locked → the registered destroy callback`)
mutating `ring_bitmap` and the warp connection under only the cache mutex; my
"its other caller is cache_fini" comment had enumerated two of the three
callers — the enumerate-callers-by-enclosing-function lesson (aux#254)
replayed on the very mutex this chunk added. Fixed with a `_locked`/locking-
wrapper split; round 2 (Fable 5, scoped to the fixes) verified the
`cache→tly` lock order edge-by-edge — no reverse edge, no evict-on-get, no
re-entrancy, teardown destroys the mutex after `cache_fini` — and returned
0/0/0/1 (the F6 assert's 32-byte/row constant holds only for single-digit
timeline indices; the assert now pins both). CLEAN. Of the six round-1 P3s:
F5 (the `min()` clamp silently crediting timeline 3 on a corrupted tag —
now drop-loud, and round 2 proved the arm dead-but-armed by closed producer
enumeration), F6, F7 (a skip that could ride into the PASS banner — now a
loud FAIL) fixed; F4 documented as the LEDGER-CLASS INVARIANT; F2/F3 are
witness-strength gaps TRACKED to the owed two-timeline submit+wait harness
(the copy proof cannot distinguish a stubbed fence from a real one — the
per-timeline arithmetic is sound-by-policy on the trusted host, not
sound-by-witness). Re-witnessed green on real V3D after the fixes; the
round-2 additions are compile-time only.

**A second deadlock corner caught at self-audit, before any run.** The
pre-code design said "one parker" but the transport's own throttle
(`fenced_write`) parks the fence file too — and a venus submit runs under the
transport mutex, so a parker (mutex dropped) plus a throttling submitter
(mutex HELD) made two parked readers against a server whose pending-fence
sweep woke exactly one (first-match consumed `fence_reported`,
server.rs ~12009). The stranded reader could be the mutex HOLDER — a
permanent instance wedge. What caught it: walking the park machinery
server-side before trusting the client protocol (the I-9 reflex). Closed at
both layers: `poll_fences` now advances `fence_reported` AFTER the sweep
(deliver-to-all — the doorbell contract is exactly as satisfied, and the
seam is no longer one client bug from a self-strand), and `warp_venus_submit`
is non-parking (its throttle waits run through the renderer's one-parker
cycle; foreign-contention waits yield off-mutex). Design doc §E.2 records it.

**The wrong turn, caught within the hour (the reusable part of this run so
far).** The ratified hybrid fence ABI — "expose per-queue fences through the
queue's `ring/<ridx>/fence`" — rested on a premise the pre-implementation
code-read refuted: **a queue timeline has no host3d ring.** `vn_device.c:83-92`
acquires a bare bitmask index and binds it host-side via the protocol; no
shmem is minted per queue; vkQueueSubmit rides the polled primary ring
(`vn_queue.c:1037`, never the renderer op); the renderer op sees nonzero
ring_idx only on sync-export batches (`vkWaitRingSeqnoMESA` + syncs,
vn_queue.c:1918-1930). What caught it: the discipline of re-verifying the
design doc's claims against the code before writing any — the doc itself
warned "every claim carries file:line — re-verify before relying", and the §C
namespace trap it documented (timeline vs host3d slot) turned out to be the
exact conflation its own §D/§E had committed. Two further server facts shaped
the correction: the fence park is a per-ctx SINGLE cursor
(`fence_signaled`/`fence_reported`, server.rs ~9798 — two parked readers would
steal each other's wakes), and the ctl's client-critical prefix is a guarded
255-byte budget (~9674) with no room for per-timeline rows. Re-escalated with
the evidence; operator ratified **v2: a new tiny read-only `ctx/<id>/timelines`
file + the existing shared park file untouched + a one-parker/condvar client
protocol with the transport mutex dropped across the park**. The superseded
vote stays in the doc (§E) with its refutation — deleting it would delete the
lesson.

The chunk was "fill the three pre-wired mesa `vn_renderer` bo_ops over the `mem/`
ABI and witness a `vkAllocateMemory`+`vkMapMemory` E2E on real V3D." The backend
(`create_from_device_memory`/`bo_map`/`bo_destroy` + a 256-slot `mem_bitmap`) and
the transport (`warp_mem_new`/`map`/`unmap`/`destroy`, a clean mirror of the ring
verbs) went in and compiled first try. Then the E2E surfaced something nobody had
planned for: **`vkCreateDevice` on real V3D Venus had never actually run.**
V-3b-3b's prove stopped at `vkDestroyInstance` (its "PASS" is instance bring-up),
so the entire device path was unexercised — and it hid a two-layer masking-bug
stack, exactly the shape `DEBUGGING-PLAYBOOK.md` warns is disproportionately
common in elusive bugs.

**Layer 1 — the loader trampoline (`pc=0`).** The prove crashed
`snare:segv addr=0x0 pc=0x0 lr=0x2c33a4`. `pc=0` with `lr` still in `main` is a
*tail-branch to null*. Disassembling the crash `lr` named it: `create_device`
(from `vk_icdGetInstanceProcAddr`) is `vk_tramp_CreateDevice`, a mesa runtime
dispatch trampoline — `ldr x4,[x0,#0x1380]; br x4`. It loads the physical-device
dispatch slot for CreateDevice and tail-branches to it, and that slot is
**built for the Vulkan loader's object layout**: the ICD populates it only under
a real loader, so loader-less it is null. `vn_physical_device_entrypoints` binds
`.CreateDevice = vn_CreateDevice` *and* `.GetPhysicalDeviceProperties` in the
same table, and the latter's trampoline worked — the tell that this is a
loader-layout quirk, not a missing entrypoint. **Fix:** call the Venus
entrypoints directly by symbol (`extern vn_CreateDevice` etc.), the same
loader-less pattern the prove already used for `vk_icdGetInstanceProcAddr`. The
flat `vn_MapMemory`/`vn_UnmapMemory` are not defined symbols (Venus implements
the 1.4 `vn_MapMemory2`/`vn_UnmapMemory2`) — `nm` on the objects caught that
before a wasted build. Host `vkCreateDevice` then returned `rc=0`.

**Layer 2 — the timeline cap (a hang that was mine).** With the trampoline gone,
the prove reached `vn_device_init_queues` and stopped — no crash, no return.
`vn_queue_init` unconditionally calls `vn_instance_acquire_ring_idx`, which
reserves ring 0 for the CPU timeline and hands a queue the first free index (1),
then rejects it when `ring_idx >= info.max_timeline_count`. The backend
advertised `max_timeline_count = 1` (a V-3b-3b F3 decision to avoid mis-fencing
on the ring_idx-less seam) — so **every `vkCreateDevice` with a queue was
impossible**, i.e. no device could ever be created. The cap-to-1 was a latent
bug, not a safe conservative default. **Fix:** `max_timeline_count = 2` — one
queue timeline (ring 0 CPU + ring 1 queue). A lone queue submits on ring 0
regardless, so there is nothing to mis-attribute; the F3 seam-carries-ring_idx
fix stays OWED and gates a *second* queue timeline (multi-queue submit), which
this allocate+map path never exercises (it creates the queue, never submits).

**Then it was green.** `device-memory sentinel OK (zero-at-map + c0deface
round-tripped)` then `THYLACINE-VENUS-PROVE PASS ... V-3b-3c-2b` on thyla-pi/KVM
real V3D 4.2.14 — `vkAllocateMemory` -> `create_from_device_memory` ->
`warp_mem_new`, `vkMapMemory` -> `bo_map` -> `t_weft_map`, the backing observed
**zero at map** (the server's disclosure floor, a genuine cross-boundary read
through the weft mapping, not a self-write) before the sentinel. Re-witnessed
green on the clean probe-free build (0 VNDBG residue).

**The wrong turn worth recording: a build-system phantom.** Mid-hunt, VNDBG
probes I added to `vn_device.c` appeared absent from the binary (`grep -a VNDBG`
= 0) even after a forced re-archive + relink — I burned ~two cycles theorizing a
thin-archive/`--gc-sections` link failure. They were in the binary the whole
time. The reason they never *printed* earlier was simpler and upstream of the
link: the crash was in the trampoline, *before* `vn_CreateDevice`'s body ran, so
the probes never executed. Once the extern fix reached the body, all A–G + Q1–Q4
probes printed and localized the hang in one shot. The catch: `grep` for the
string is not the same question as "did this code run" — and I let a
false-negative on the former send me hunting the linker. Ground truth (the
probes firing once the path was reachable) ended it.

**Decisions the operator made.** When the queue-init hang localized and the
device-bring-up revealed itself as a multi-layer prerequisite larger than the
chunk (each layer a ~10-min GCP build+witness cycle), I surfaced the scope fork
— keep grinding vs land-the-backend-and-split. Operator chose **grind to
completion**.

**Cost + gates.** Eight thyla-keep builds, five real-V3D boots (the mesa cross
only builds on thyla-keep; the LAN mDNS name wedged mid-run — the documented
`thyla-pi.local` failure — and the Cloudflare tunnel carried the rest). #245:
the client E2E witness `device-memory sentinel OK` joined `venus-verdict` +
`test-venus-verdict` (DISCRIMINATES 37/37, +3 arms) + the boot-probe filter, so
the witness is read by a gate, not only by hand. Mesa is 4 files
(`vn_renderer_thylacine.c`, `warp_client.{c,h}`, `thylacine_prove.c`); the core
Venus driver stays pristine (the probes were reverted).

**The dirty-close re-audit (round 2), resumed after a self-compaction.** The
round-1 close was dirty (2 P1); the re-audit on the fix state came back **0 P0 /
1 P1 / 2 P2 / 1 P3** — not clean, and the two P2s were exactly what a dirty-close
round exists to catch: hazards the *fixes* created.

- **F5 [P2] was my own self-audit finding, and the independent prosecutor rated
  it correctly higher than I had.** I had flagged (as a P3) that `warp_mem_new`
  freed the handle whenever the write *failed*, not only when the server
  *refused* — the rare "server committed, then the reply was lost" residual. I
  reasoned the server's duplicate-handle refusal would degrade any collision to
  one clean alloc failure. The prosecutor showed the degradation is not one
  failure but an **instance-wide wedge**: `thylacine_mem_alloc`'s first-fit scan
  keeps re-handing the same low bit, which keeps colliding — the exact F2
  symptom, for the ctx's life. That is the value of two prosecutors on one
  surface: same finding, but I under-read the blast radius.

- **F4 [P1] was a round-1 escape neither of us caught the first time.** Venus
  advertises `VK_EXT_map_memory_placed` unconditionally (`vn_physical_device.c`
  :1253, `memoryMapPlaced=true` at :436, nothing renderer-gated), but
  `thylacine_bo_map` **discarded** `pPlacedAddress` — the weft map picks the VA.
  A legal app that enables the feature and requests a placed map gets a silent
  `VK_SUCCESS` at the *wrong* address → app-side corruption. The old comment
  argued only the `has_guest_vram` case produced a non-NULL `placed_addr`; the
  *extension* is the other producer, and it was advertised. Fixed by
  de-advertising (a `cannot_map_placed` renderer bit Thylacine sets) plus a
  defensive refuse in `bo_map` — belt-and-suspenders for a corruption-class bug.

- **F6 [P2] was the F2 fix biting itself.** `vkAllocateMemory` is not externally
  synchronized, so two threads race `thylacine_mem_alloc`'s plain RMW and both
  get the same handle. Pre-F2 the loser's refused mint kept the bit (a one-bit
  leak). Post-F2 the loser's `E_INVAL` refusal *freed* the bit the winner owns →
  the F5 wedge with no kernel change. The deferral comment blamed "multi-queue";
  the real trigger is any two allocating threads, available now.

**The reconciliation, and a divergence from the prosecutor's remedy worth
recording.** The prosecutor suggested discriminating the errno and putting
`-EINVAL` in the *free* set. That reproduces F6 — a duplicate `-EINVAL` means the
slot **is** installed, so freeing frees the winner's slot. The reconciled fix
frees **only** on provably-not-installed (an open failure, or the routine
`-ENOMEM` holistic-cap refusal) and keeps the handle marked on everything else.
This is one predicate that closes both F5 and F6's invariant-break; the
prosecutor is authoritative about the smell, not the remedy
([[audit-15-closed-list]]). The `-ENOMEM` comparison is load-bearing for F2, so
it was ground-truthed both ways before trusting: the server returns `E_NOMEM` for
the `WARP_CTX_BACKING_MAX` cap (`server.rs`), `kernel/dev9p.c` propagates the
real Rlerror ecode (`t_write` returns the negative errno, "test with `< 0`, never
`== -1`"), and `ENOMEM == 12` on both sides (kernel `errno.h` `_Static_assert` +
the sysroot `bits/errno.h`). Get any one wrong and the routine refusal maps to
"keep" and F2 silently regresses.

**Fixed + re-witnessed GREEN** at mesa `d7f4ef1` (patch 0014): build rc=0
(prove md5 `c0ccddda`→`e386bc27`), ramfs sha verified on the pi, the logical
device created against the *de-advertised* placed-map extension, `device-memory
sentinel OK` → `THYLACINE-VENUS-PROVE PASS`. The E2E confirms the happy path +
device creation; the **fix proofs** (F4 placed-refusal, F5 EINTR-keep, F6
concurrent-alloc, F2 ENOMEM-still-frees) all need error-path witnesses the
allocate+map E2E structurally cannot provide — owed at the GPU-submit /
cap-exhaustion chunk. By the convention that a returned P1 is a dirty close, a
**round 3** on the round-2 fixes is in flight as of this writing.

**Round 3 converged the chain, and found the F6-closure had a hole I'd have
missed.** R3 (0 P0 / 0 P1 / 1 P2 / 1 P3) re-derived the errno chain at every link
-- the conflation hazard I hunted hardest (a *kernel*-synthesized `-ENOMEM` after
the Twrite committed) provably does not exist (kernel OOMs on this path are
`-P9_E_IO` or a shortened write, never `-12`). But **F8 [P2]**: tapestryd's
`wmem_mint` checked the holistic cap BEFORE the duplicate, so in the
`over && taken` corner a concurrent-alloc race loser's *duplicate* handle at a
cap-adjacent ctx got `-ENOMEM` (over wins the tiebreak) for a handle that IS
installed -> my client's "free on `-ENOMEM`" freed the *winner's* live slot. My
round-2 errno fix was defeated by the server's check *order* -- the two halves of
the same predicate colliding in exactly the cap-adjacent corner where the F2
memory-pressure loop lives. Two-line server swap (taken before over), re-witnessed
GREEN. The whole chain closed + pushed at `e34760d8` (mesa `d7f4ef1`/patch 0014).
Lesson, filed: an errno-keyed free/keep decision is only as exact as the
*producer's* errno assignment, and a check-order bug at the producer defeats it in
the collision corner.

**Then: the GPU-submit chunk's design research, and the redesign that a fork
proved.** Operator chose the GPU-submit milestone, then chose to pull MULTI-QUEUE
forward (not single-queue-only). I verified the F1 proof is viable on the current
path (the WARP ctx fence retires on GPU-work completion via VIRTIO_GPU_FLAG_FENCE
-- not stream delivery), then researched the multi-queue fence model. The key
finding, which a design-brief fork proved decisively: **multi-queue is a
NEW-MECHANISM redesign, not a mapping.** Neither existing fence path carries a
per-VkQueue GPU-completion signal -- the per-ring `ring/<ridx>/fence` is the V-3a
echo-drain ACK and is *structurally disabled* for Venus rings (`wring_kick`
returns E_OPNOTSUPP: "virglrenderer POLLS a host3d ring"), and the real
GPU-completion fence is ctx-wide with no `ring_idx` in `FenceTag`. So per-queue
fences require threading queue identity through the completion pump. Plus a trap
worth the whole research: mesa's `ring_idx` (a *timeline* index) and tapestryd's
`ridx` (a *host3d ring slot*) are two numerically-overlapping namespaces nothing
documents. Captured as `docs/WARP-MULTIQUEUE-DESIGN.md` (the design foundation +
the open forks: queue count, the fence-file ABI, pulling the F6/F8 mutex in as a
now-dependency, the #210 per-ring-FIFO assumption to defuse). Operator directed
the IMPLEMENTATION to a fresh session -- so this session lands the design
foundation; the fresh session runs the design conversation (the forks) into
scripture, then implements.

---

## 2026-08-26 — V-3b-3c-2a: the device-memory substrate, the split the code sized for, and the two P3s that were real hazards

3c-2 is the device-memory milestone. This session built its server-side half —
the tapestryd `mem/` substrate — and stopped before the mesa backend (3c-2b), on
a deliberate split.

**The ABI fork, and why the pickup's lean was only half-right.** The prior
session left a fork: expose device memory as (a) a lean new `mem/` subtree or (b)
a host3d flavor on the existing `bo/` tree. I took (a) — a `bo` is built for
textures (create3d geometry + the C-2c compositor-import + the #240 leak-park
graveyard), none of which a flat device-memory blob wants, and this is
audit-bearing (I-45) where isolation beats ABI economy. But the deeper read
corrected the pickup on two axes it had guessed. (1) *Addressing*: the pickup
leaned bo-style (server-assigned pub_id, create-on-open). Reading the paths,
ring-style is cleaner — a one-step `mem/new` write-verb "&lt;bytes&gt; &lt;handle&gt;
&lt;mem_id&gt;" (the client owns the handle, like a ring's ridx) has no empty-slot
corpse between open and build, so the #218 minted-but-unbuilt starvation hazard
has no analog. Device memory is a lean hostmem+weft blob — structurally a ring,
not a bo. (2) *The split*: the pickup offered "one E2E chunk OR split." I started
toward one chunk, then measured the edit — ~200 LOC threaded through ~24 sites in
an 11.6k-line audit-bearing file — and split, so the substrate lands + audits as
its own reviewable unit and gives a known-good base before the remote mesa/E2E
loop. The reusable lesson: **size the chunk by reading its code, not by trusting
the plan's estimate.**

**A pre-existing I-32 hole surfaced while composing the cap.** Making
`WARP_CTX_BACKING_MAX` count the new mems axis meant touching the cap sum — and
that exposed that `wbo_create` summed bos + leaked ONLY, never the existing
rings, while `wring_mint` counted both. A bo mint therefore did not charge live
rings against the 64 MiB ctx cap: bounded (rings are ≤1 MiB each) but a real
accounting hole. Rather than extend the asymmetric sum, I factored a
`ctx_backing_total` = bos+rings+mems+leaked used by all three mint paths —
holistic by construction, closing the pre-existing hole as a consequence.

**Witnessed green on real V3D, then audited.** The boot self-test
(`warp_mem_selftest` — alloc a blob, round-trip a sentinel through tapestryd's
own map, destroy, re-mint the freed handle) printed `warp mem-recreate
handle-reuse OK` on thyla-pi/KVM (V3D 4.2.14), boot exit 0, beside the
ring-recreate + host3d-ring markers. The Fable prosecutor (context-independent,
`cargo check` run so the non-Copy/ownership claims are compiler-witnessed)
returned **0/0/0/4 P3, CLEAN** — "a faithful transplant of the closed ring
pattern; every mem-specific deviation STRENGTHENS the ring properties."

**The four P3s — two drift, two real hazards.** F1/F2 were comments I falsified:
the holistic cap left two ctl comments claiming `bo-bytes` "is what the cap
gates" (now only the BO share), and `wmem_mint` arming the venus ctx falsified
`wctx_has_venus`'s "iff it minted a ring" contract. Fixed both; added a
`backing-bytes` ctl key so the holistic quantity the cap actually compares is
observable (the #184 gauge rule). But **F3 and F4 were the findings worth the
round.** F3: I had copied the ring's disclosure-zeroing — per-8-byte SeqCst
stores — into the mem path without re-deriving it for the size. The ring tops out
at 1 MiB; a 64 MiB device-memory blob is ~8.4M *barriered* stores on the single
serve thread the console shares — CACHED it is weave-create league, but UNCACHED
(host-dictatable) ~1 s/mint, a client-repeatable console-freeze lever. The
codebase already zeroes client-mappable memory with `write_bytes` in three
shipped sites (alloc_weave et al.); the SeqCst loop's own "atomics prevent
elision" comment was refuted by those sites — the backing escapes through a
syscall and cannot be proven dead. Fixed at BOTH host3d sites. F4 is the subtle
one: the disclosure zero, added for *security*, bakes a client-ordering
*contract*. A `blob_id=mem_id` blob exports the live VkDeviceMemory's host pages;
the mint zeroes them, correct only if the client mints at `vkAllocateMemory`
time, before any GPU use. A 3c-2b client minting lazily at first `vkMapMemory` —
after a legal Vulkan GPU-copy-into-then-readback — would have the server zero
away the GPU's results, a data-corruption bug every current gate passes.
Documented the contract on `wmem_mint`/`WarpMem` and carried it into the 3c-2b
intent, so the next chunk does not re-derive it by accident.

**Open:** 3c-2b — the mesa `vn_renderer` bo_ops (`create_from_device_memory` /
`bo_map` via weft / `bo_destroy`) + extending the prove to
`vkAllocateMemory`+`vkMapMemory` + the E2E on real V3D. Carried debt: the
V-3b-3b F2 convergence code on the mesa shmem_create error arms, and the
single-thread `warp_conn`/`ring_bitmap` mutex when VkQueues make the driver
multi-threaded. Landed `54e2f334` (tapestryd-only; mesa unchanged at `77fc80a`).

---

## 2026-08-26 — V-3b-3c-1: the F1 full fix (a per-ring destroy verb), and why device-memory is the *next* chunk not this one

V-3b-3c's operator-ratified milestone is device memory (`vkAllocateMemory` +
`vkMapMemory`). Before touching it I split the arc: **3c-1 closes the V-3b-3b
audit's owed P1 (F1); 3c-2 is the bo milestone.** The split is sequencing within
a ratified arc, not a scripture fork — but it is load-bearing, because F1 is the
exact hazard device-memory churn would trip, so it lands on a sound ring
lifecycle rather than atop a known-open P1.

**What F1 was.** V-3b-3b's backend allocated host3d command-ring slots (ridx
0..63) and tapestryd retired a ring only at ctx death — there was no per-ring
destroy. Freeing + reusing a ridx therefore collided with the still-installed
server slot (`ring/new host3d <ridx>` -> E_INVAL), permanently wedging
`shmem_create`. The interim dodged it by making ridx alloc **monotonic** (never
freed): a bounded 64-slot/ctx leak, benign for a single bring-up, fatal under
the ring churn a real driver does.

**Reconnaissance corrected a load-bearing belief.** An Explore pass over the
hostmem plumbing found the observe-and-reap engine (`retire_host3d_ring` /
`reap_hostmem_parked` / `SYS_HOSTMEM_REFCOUNT`) *already built* at 1c-2b — F1
was never missing machinery, only a client-invocable trigger wired to it. It
also corrected the mechanism I had half-wrong for 3c-2: a Venus app client holds
**no PCI handle**, so it cannot call `t_burrow_from_hostmem` (needs RIGHT_MAP on
a KObj_PCI). The client maps hostmem via **weft** (`t_weft_map`), exactly like
the command ring; tapestryd is the only actor that calls `burrow_from_hostmem`.
The design op-table's "bo_ops -> SYS_BURROW_FROM_HOSTMEM" describes tapestryd's
internal step, not the backend's — which would otherwise be an impossible client
capability. That correction reshapes 3c-2 (device-memory bo ~= the command ring
in substrate, differing in `blob_id=mem_id` + exposure) and is recorded here so
3c-2 does not re-derive it.

**The fix.** A `ring/<ridx>/ctl destroy` verb (`WFK_RING_CTL`) whose handler
(`Comp::wring_destroy`) takes the WarpRing out of its ctx slot — freeing the
slot — then runs the existing `wring_teardown` (disarm the weft share ->
`retire_host3d_ring`: observe-and-reap the hostmem backing). Ownership-gated by
the same conn scan as `wring_kick` (I-45). The backend
(`thylacine_shmem_destroy_now`) now unmaps, then issues the destroy verb, then
frees the guest ridx **only if the verb succeeded** — preserving the invariant
"guest ridx free <=> server slot free", so a reused ridx can never collide with
a still-installed slot; a refused destroy falls back to the interim's
leak-until-ctx-death for that one slot (fails safe).

**A subtlety worth the ink: the reap arm is decided by ordering.** The backend
`t_close`s the map fid before the destroy RPC. `t_close` synchronously drops the
client's kernel mapping (`dev9p_close` -> `weft_binding_release`), so by the time
`retire_host3d_ring` reads `SYS_HOSTMEM_REFCOUNT` the count is back to 1
(tapestryd's own map) and the ring reaps *immediately* rather than parking. If
that ordering were reversed the ring would park and reap later — still correct,
never a cross-client alias, because the observe-and-reap refuses to free an
offset under any live-or-pending client reference. The self-test proves the
immediate arm: it re-mints the freed offset in the same breath.

**The witness the old one was blind to.** The V-3b-3b bring-up creates a handful
of rings and never re-mints a ridx, so it could not see F1 at all. A tapestryd
boot self-test (`warp_ring_recreate_selftest`) mints at ridx 0, destroys via the
verb, asserts the slot freed, and re-mints at ridx 0 — the re-mint is the load-
bearing assertion (E_INVAL "already minted" here *is* the F1 divergence).
Witnessed on thyla-pi/KVM real V3D: `warp ring-recreate ridx-reuse OK (destroy
-> re-mint ridx 0)` at boot-log line 2206, `THYLACINE-VENUS-PROVE PASS` at 2230
(the backend's new destroy-on-teardown did not regress bring-up), `Thylacine
boot OK` at 2862, no extinction. The mesa cross-build on thyla-keep was clean
(2.7 s) and the new `ctx/%u/ring/%u/ctl` format string is baked into the binary
(behavioral proof, not a build-chain inference).

**What this does NOT cover.** F5 (the wait ns-timeout) stays a documented P3
deferral: there is no live finite-timeout caller (teardown wants completion,
`UINT64_MAX`), and a monotonic clock IS reachable (`t_clock_gettime` +
`os_time_get_nano`), so it is buildable when a finite-timeout waiter exists — it
is pulled into 3c-2 if one emerges, else it stays tracked. Bundling a new
poll-loop + its timeout-expiry test into the F1 chunk would only dilute the
focused audit.

**Audit — 0 P0 / 0 P1 / 1 P2 / 2 P3, and the P2 was the reusable catch.** The
holotype (Fable 5) re-derived the whole mechanism sound — the I-45 double gate,
double-destroy-impossible (the non-Copy `HostRing`), the observe-and-reap
ordering under the new caller (including the claim-race: share disarmed before
the refcount read, so sum>=2 parks and sum==1 reclaims, never a cross-client
alias), and the two-sided ridx invariant. Then it found F1 [P2], which my own
self-audit missed: **the regression witness I was so pleased with was wired into
no gate.** The boot-probe capture filter (`grep -aE "tapestryd: gpu|tapestryd:
warp host3d-ring"`) does not match `warp ring-recreate`, so the line was dropped
from the captured evidence, and no venus-gate leg or verdict-test sabotage
covered it — I had read `ring-recreate ridx-reuse OK` by eye from a raw log and
called it a witness. That is precisely the pinned #245 class ("a witness with no
caller rots") and the exact half the 1c-2a precedent had shipped *with* its
selftest. The catch is the context-independence dividend: a same-family reviewer
that re-derives claims from the code rather than trusting the run. Fixed all
three legs — the filter (capture reconfirmed against the real boot log), a
warp-host.sh venus-gate test+control leg, and three test-venus-verdict sabotage
arms (`DISCRIMINATES` 31/31). F2 [P3] (the mesa create-path error arms kept the
interim leak under now-false "monotonic/reaps-at-ctx-death" comments) —
comments corrected, the convergence code deferred to 3c-2's mesa rebuild
(tracked, not dropped). F3 [P3] (the v3d-fork note prescribed vindication-defer,
wrong for the mid-life `wring_destroy` caller whose shape is `wbo_destroy`'s
`fences_in_flight` defer) — comment extended. Not dirty; no re-audit. None of
the three touched a binary (tool + comment fixes), so the witnessed binaries
stood — no rebuild, no re-boot. Closed list: `memory/audit_v3b3c1_closed_list.md`.

---

## 2026-08-25 — V-3b-3b: real Venus vkCreateInstance on real V3D silicon (two root causes, both caught by the boot log)

The V-3b-3b chunk: turn the V-3b-3a skeleton (stubs) into a working `vn_renderer`
backend and drive a real `vkCreateInstance` + physical-device enumeration over
`/srv/warp`. Two subagents mapped the ground first: the tapestryd `/srv/warp`
server ABI (exact verb strings, the fence surface, the connect handshake) and
the Mesa Venus bring-up flow (when each op is called, what the capset gate
demands, whether wait/sync are on the path). That reconnaissance was worth it --
it turned the ops from guesses into a byte-exact spec, and it produced the single
most useful finding of the run before a line of backend code ran: **only two
capset fields are load-bearing gates** (`wire_format_version==1`,
`vk_xml_version>=1.1`), and a zero capset does not crash -- it silently yields a
device-less STUB instance. That fact is what let me READ the first two failures
instead of guessing at them.

**Impl.** The backend (`vn_renderer_thylacine.c`, mesa-thylacine): `shmem_create`
mints a HOST3D ring on the client's venus ctx (`ring/new host3d`), reads its
res_id from `ring/<ridx>/info`, weft-maps it; `submit` forwards opaque venus
bytes to `ctx/<id>/submit` (the vkCreateRingMESA bootstrap + doorbells);
`wait`/`sync` are a guest-side u64 timeline bound to the WARP ctx fence
(needed only at teardown -- bring-up's replies are pure ring-head polls, no
`vn_renderer_wait`); `info` reads the venus capset. The transport
(`warp_client.c`) gained the host3d-ring verbs + a raw venus-submit + a
venus-caps read, reusing `warp_open` + the issued/signaled fence model verbatim.
It built + linked clean on thyla-keep, first try (the editor's clang can't see
the cross-file includes, so the cross-build IS the check).

**Wrong turn 1 -- the capset.** First witness on thyla-pi/KVM: `instance
created`, then `ABSENT (device-less stub)`. Per the pre-computed gate fact, a
stub means the capset gate failed. Ground truth (gpu.rs:1702): tapestryd's
capset fetch ranks **VIRGL2 > VIRGL > anything**, keeps ONE blob, and serves it
as `caps` -- so on a venus host it serves the VIRGL2 capset, and the OpenGL
winsys reads exactly that (virgl_thylacine_winsys.c:529, so re-ranking was out).
My backend read those virgl bytes as `virgl_renderer_capset_venus` -> a garbage
`wire_format_version` -> stub. This is precisely the dependency WARP-V3-DESIGN
0.14 flagged ("sequence V-3c before 3b if the caps blob must validate"): a
genuine pull-forward of V-3c's SERVING half (the enforcement half stays V-3c).
Fix: tapestryd fetches the venus capset SEPARATELY (`get_capset` refactored to
return the blob) and serves it on a new `caps-venus` file; the backend reads
that. A synthetic capset would have been the forbidden shortcut -- wrong
versions/mask break the protocol encoding.

**Wrong turn 2 -- the ordering, and why witness 2 still said ABSENT.** With the
venus capset now fetched (`GET_CAPSET id=4 -> 160 bytes; caps[0] = 0x00000001`,
a VALID capset), witness 2 STILL reported ABSENT. The capset was right; the
timing was not. The boot log line numbers were decisive: **venus-prove ran at
line 2046, but the warden did not bind tapestryd until 2184** (READY 2211). The
venus smoke sat in joey's PRE-warden boot-test suite -- it was placed there at
3a, correctly, because 3a needed no transport (`vkEnumerateInstanceVersion`
only). So `warp_open` found no `/srv/warp`, failed, and stubbed. A retry inside
that pre-warden suite would DEADLOCK (the suite runs before the warden that would
start tapestryd). Fix: move the smoke to the POST-warden probe block (joey.c),
where the warden's readiness handshake guarantees tapestryd is serving and the
ramfs root still resolves the binary pre-pivot.

**Result (witness 3, real V3D, boot green).** `THYLACINE-VENUS-PROVE PASS
(bring-up over /srv/warp: instance created, 2 physical device(s), dev0
'Virtio-GPU Venus (V3D 4.2.14.0)', 1 memory heap(s), instance destroyed)`.
That is the full 3b scope end to end: shmem_create (command ring + the
lazily-grown reply shmem, both host3d rings), submit (vkCreateRingMESA + the
reply-stream `vkSetReplyCommandStreamMESA` + doorbells), the ring-head reply
poll, the capset gate, and -- at DestroyInstance -- the fenced teardown that
exercises sync + wait + vkDestroyRingMESA. `Thylacine boot OK`, no extinction,
venus-prove at line 2229 (after tapestryd serving at 2205).

**A design-doc correction it forced.** 0.14's op table says `wait -> t_poll on
ctx/<id>/fence`. The server-ABI map proved that is wrong as literal: tapestryd's
qids never carry QTPOLL, so `t_poll` returns POLLIN immediately and would
busy-spin. The mechanism (wait on `ctx/<id>/fence`) is unchanged; the syscall is
a BLOCKING Tread, which `warp_fence_wait` already implements -- the backend
reuses it.

**Open.** The bo / device-memory path is V-3b-3c (stubbed here; bring-up never
allocates device memory). The backend documents a single-thread assumption on
the unguarded `warp_conn` -- sound for the single-threaded bring-up (the prove
harness is the only caller), owed a per-renderer mutex at 3c when VkQueues make
it multi-threaded. The Fable holotype round (I-45-adjacent, the first
client-writable bring-up path) is in flight at the time of writing.

## 2026-08-25 — reply-shmem: the design pass that dissolved itself; then V-3b-3a builds on real silicon

With both V-3b-2 chunks shipped (`b7b712dc`), the operator said "Let's open it" --
the reply-shmem design pass. The interesting part is that **the research reversed
its own premise.** The resume note framed reply-shmem as "a NEW FD_SHM ABI ->
design-fork -> scripture-first." A Mesa deep-read (a subagent over
`mesa-thylacine` + virglrenderer `7fcfce49`) traced `vkSetReplyCommandStreamMESA`
end to end and found: it is a **RING command** Mesa writes into the command ring
(cmd_type 178, tapestryd forwards nothing), the reply region is a **second host3d
ring** (`ring/new host3d ... 1`, `WARP_RINGS_PER_CTX`=64 already), its res_id is
exposed by the existing `ring/<ridx>/info`, and the host writes replies zero-copy
while the guest polls the ring `head` (release/acquire). So reply-shmem needs
**no new Thylacine substrate at all** -- it folds into V-3b-3 (Mesa), no separate
V-3b-2b. Ratified + landed as scripture `cc1870fe` (WARP-V3-DESIGN 0.13). The
lesson: "a new ABI" was a claim about the tree that the tree did not support --
the design pass's job was to find that out, and its best outcome was to delete a
chunk.

Then the V-3b-3 design + a 4-sub-chunk plan (`b6435106`, WARP-V3-DESIGN 0.14). A
fork-map subagent settled the shape: the backend is **vtest-shaped** (a userspace
transport, not the DRM-ioctl virtgpu shape), the `vn_renderer` vtable is 20
pointers (3 optional), and there are two build blockers -- a hard libdrm dep
(from the cross-file's `system='linux'`) and a shared-library ICD (Thylacine is
static/no-loader). Sections 4/3.4-3.6 were V-3a-framed (tapestryd in the ring hot
path); Model B moved that out, so the section reconciles them.

**V-3b-3a then built + linked clean on thyla-keep** (started the VM, built,
stopped it). The Mesa Venus backend (`vn_renderer_thylacine.c`, written blind --
compiled 0-error) + a loader-less ICD prove harness link into a 17 MB static
aarch64-thylacine ET_EXEC. The build-system integration -- the novel 3a risk --
worked: `system_has_kms_drm=false` for thylacine (blocker 1, the honest value --
`dep_libdrm=null_dep`, virgl-on-thylacine uses the warp winsys not DRM), a static
loader-less ICD (blocker 2), and a `renderdoc_app.h` platform port (the
vk-runtime includes it). Six mesa-fork files, all applied to both trees.

Wrong turns caught: (1) the first configure used the **system meson 1.3.2** and
failed the `>=1.4.0` gate -- the builds use a **`/build/venv-meson`** (1.11.2);
(2) my info-init had a `STATIC_ASSERT` comparing a `uint32_t[32]` to a single
capset word -- the editor's field-size check caught it before the build (the
capset copy is V-3c's job anyway, so it was dropped); (3) the **mesa fork has
diverged** -- my local `b7f9ed2` (Warp-4) is NOT in the builder's `9b2fef7`
(#198 bundle) history, and there is no shared Thylacine remote (origin = upstream
freedesktop) -- caught by checking the builder HEAD before applying edits.
`src/virtio/vulkan/` is pristine on both, so the edits apply either way, but
where 3a commits is the operator's call (they chose: settle it at commit time).

Open, and exact about it: the 3a **build+link half is proven**; the **runtime
resolve half is not** -- `thylacine-venus-prove` is a pouch-ABI ET_EXEC, so
`THYLACINE-VENUS-PROVE PASS` needs a Thylacine guest boot (not the builder's
Linux), which needs a verified/fresh paired build + venus-prove staged into the
ramfs. Deferred rather than rushed: the local `build/` ramfs (16:01) vs pool
(10:41) are of uncertain pairing (bake-trap risk), and blind-booting a mispaired
set EXTINCTs. Scripture pushed to both mirrors; the 6 mesa-fork files uncommitted
pending the divergence decision.

**Continuation (same run): the runtime resolve half is now PROVEN.** The deferred
half above landed. Rather than chase the uncertain local pairing or pay a fresh
clade bake, I took the cheaper true path: the ICD dispatch needs no pool/GL/FS,
so I booted POOL-LESS (`THYLACINE_POOL_IMG=/nonexistent`, run-vm's supported
no-pool branch) with venus-prove in the RAMFS, not the /clade pool tree. Staging
is a guarded copy in `build_ramfs` (mirroring osmesa-prove) plus a NON-FATAL joey
boot-test smoke right after `do_pouch_hello_smoke` (`usr/joey/joey.c`, pre-pivot
6456 so it is pool-independent) spawning through the audited `pouch_smoke_one`.
`build.sh userspace && build.sh ramfs` (the pool untouched), then
`scratchpad/v3b3/boot-venus.sh`. The console (`venus-boot.log:2047`):
`THYLACINE-VENUS-PROVE PASS (loader-less ICD: vk_icdGetInstanceProcAddr resolved
+ dispatched; vkEnumerateInstanceVersion -> 1.4.354)`. The Mesa Venus backend and
the Thylacine vn_renderer ran at EL0 and dispatched a real Vulkan global call --
the first Venus code to run on Thylacine. joey then ran GREEN through every ramfs
boot-test past it. The boot's terminal EXTINCTION is the EXPECTED pool-less path
(`stratumd: run failed rc=-2` = ENOENT, no pool device; run-vm even announced
"no pool image ... booting without /srv/stratum-fs") -- and I read it to ground
rather than wave it off (the stewardship reflex): it is orthogonal to venus,
proven not asserted. The harness is committed local; the push is gated on a
paired full-green boot (test.sh wants "Thylacine boot OK", which a pool-less boot
never reaches), and the mesa-fork base is still the operator's call.

**Close (same run): audit, then the fork question dissolved.** The harness went
to a holotype round (Fable 5; joey.c is the "Initial bringup" audit trigger).
Its one P2 is the run's best catch and pure context-independence: `pouch_smoke_core`'s
OWN header declares a per-caller "check it before adding a caller" constraint --
reap-before-drain deadlocks on any child writing > the 4 KiB pipe ring before
exit -- and venus-prove is the FIRST out-of-tree, actively-GROWING third-party
child wired to it. Safe today (three one-line outputs; Mesa logs to the absent
fd 2), but 3b/3c grow it and a chatty failure would hang the boot with NO
extinction -- the worst outcome, and "non-fatal" is only true of the return, not
a hang. The author (me) glossed the header the reviewer re-derived. Fixed by a
`drain_first` param routing venus to the `run_viv_bundle` drain-then-reap order;
the 15 existing smokes stay reap-first and re-verified green. Two P3s (strip
17->2.6 MiB; a size+sha freshness witness against the #120/#139 stale-fetch
trap) landed too. Re-verified: full paired boot GREEN post-fix. Then the
operator's reserved "fork at commit" question **dissolved on research**: the
`usr/ports/mesa/README.md` makes the patch series canonical and the fork
disposable, so the backend is simply **patch 0010** -- and its 6 files are
disjoint from 0008/0009. Generated + round-trip-verified locally (no builder
spend): `git am 0001..0010` on a fresh `mesa-26.1.6` reconstructed a tree equal
to the venus commit's, exactly. V-3b-3a is complete at `4dc56542` (harness +
audit + patch 0010, both mirrors). The lesson worth keeping: a same-family
reviewer with no context still earned its round by reading a constraint the
author wrote and then walked past.

## 2026-08-25 — V-3b-2 cross-Proc E2E: the audit that diagnosed the GL failure

With V-3b-2 shipped, the operator chose the cross-Proc E2E witness (before the
reply-shmem design fork). Before writing it, a mapping agent + a kernel read
refined the scope, and the refinement mattered: the literal deferred item -- "the
full cross-client-alias reproduction" -- turns out to be a **kernel-internal SMP
race**. `t_weft_map` claims the share and maps it in a single syscall
(`weft_map_claimed`), so the pin-but-no-map window the alias would exploit is
never visible to userspace; it is durably covered by the white-box kernel test
`weft.hostmem_refcount`, which manufactures that exact state. Chasing it E2E would
have been a flaky non-test. So two *achievable* witnesses landed instead: the
host3d-ring **park->reclaim lifecycle** (the only leg that drives tapestryd's
`retire_host3d_ring`/`reap_hostmem_parked` under a real cross-Proc refcount) and a
cross-conn **ownership-isolation** probe, witnessed via a new gpu-side park/reap
diagnostic counter in the warp ctl.

The design had one correction worth recording. The mapping agent proposed
releasing the ring via `t_burrow_detach`. Reading `weft_map_claimed` showed the
transferred registration pin is owned by the *binding* (`priv->weft`), not the VA
mapping -- so `t_burrow_detach` would drop the mapping but leave the pin, and the
refcount would never fall to 1. The correct release is closing the map fd:
`dev9p_close` runs `weft_binding_clunk_unmap` (the VA) *and* `weft_binding_release`
(the pin) inline. The audit later re-derived this from the kernel and confirmed it
-- and confirmed `t_close` is already imported, so the "correction" also removed a
dependency.

**The run's real lesson is procedural: I ran the Fable audit in PARALLEL with the
slow GL boot, and the audit diagnosed the boot's failure.** The GL witness failed
`WARP-RING-XPROC FAIL: r` -- the message truncated by the expect hard-fail match,
so the boot log alone left the cause a guess (I spent a while reasoning it was the
park phase; it wasn't). The audit, reading `wctx_mint`, named it exactly as F1
[P1]: tapestryd enforces **one ctx per connection**, and my witness minted four
ctxs on one `root` connection. The park->reclaim *mechanism* worked silently; the
reclaim *mint* was refused because the park-held ctx was never destroyed -- the
"r" was "reclaim mint refused". A structural false-red on a fully healthy system,
in the safe direction (never false-green), but a reproducibly broken deliverable
whose own error message blamed venus. Running the audit concurrently paid for
itself in one round: it turned a truncated, mis-reasoned symptom into a named root
cause before a second ~4-minute boot was burned.

The audit found four, all fixed: F1 (the one-ctx-per-conn structural red --
destroy the park-held ctx, whose unmapped ring immediate-drops without perturbing
the ledger, and reuse the reclaim ctx as the isolation target); F2 [P2] (the
isolation negative had no conn-B positive control -- the pinned aux#215 shape,
where a wholesale-broken second connection satisfies both refusals vacuously;
fixed by requiring conn B to first succeed on the ownership-free ctl read); F3/F4
[P3] (a #186-hollow verdict conjunct the harness's own timeout text could match,
and a mid-poll ctl failure on the wrong fail channel). Critically, the audit
verified the **counters sound** -- exactly-once, pure observation, width-guarded,
leak-free -- and re-derived every kernel/server assumption the lifecycle leans on.
The re-witness on the fixed code passed: `WARP-6 V-3b-2 XPROC GATE: VERIFIED` on
thyla-pi's real V3D. Two witnesses, one diagnostic counter, no tapestryd logic
change, and a reusable lesson about where the slow gate and the fast reviewer
belong: side by side.

---

## 2026-08-25 — V-3b-2: the SUBMIT_CMD forward, and a "genuinely new mechanism" that already existed

With 1c-2b closed (`1ab6245e`), the ring is minted, mapped, and lifetime-safe --
but nothing yet tells virglrenderer to POLL it. V-3b-2 forwards the venus
SUBMIT_CMD stream (chiefly the `vkCreateRingMESA` bootstrap) so the host maps the
same shmem and begins polling. Operator chose this over the deferred cross-Proc
warp-prove E2E, with a design pass first.

**The design pass turned on one spike finding: it is standalone-witnessable.** A
source spike (Mesa main `0cd184e9` + virglrenderer `7fcfce49` + venus-protocol
`e94b12f3`) established that `vkCreateRingMESA` is a ~124-byte PURE-serialization
encode Mesa builds on a stack buffer before any Vulkan instance exists, and that
only four commands ride SUBMIT_CMD (the ring-bookkeeping quartet, all `void`) --
everything else threads the polled ring. So V-3b-2 needs NO Mesa to witness: hand-
build the bytes, submit, observe `status & IDLE` flip. That collapsed the scope --
the reply-shmem (`vkSetReplyCommandStreamMESA` + a second FD_SHM) is a SEPARATE
mechanism for synchronous ring-command replies, split off to V-3b-2b/3 rather than
bundled (it corrected an earlier 0.4 draft that had them together). Landed as
scripture first (WARP-V3-DESIGN 0.12, `f458bf12`).

**Impl recon caught a load-bearing correction before any code.** The forward
plumbing already existed (`ctx/<id>/submit` -> `warp_submit` -> `gpu.submit_3d`),
but it targets `c.dev_ctx` (the VIRGL ctx) -- and a host3d ring's resource is
created under `c.venus_ctx`, so `vkr_context_get_resource` resolves the ring's
res_id ONLY on the venus decoder. The forward delta is therefore a venus-ctx-
targeted submit, not a reuse of the existing verb. Corrected 0.12 (`6a1cdd21`)
before writing the code. Sub-step A (`836855da`): `warp_venus_submit` (submits on
`venus_ctx` via `wctx_venus_ensure`, reusing the fenced lane + admission +
accounting byte-identically to `warp_submit`) + `wctx_has_venus` + the WFK_SUBMIT
handler routing (a Venus client's submit -> venus_ctx, a virgl client's ->
dev_ctx; per-client unambiguous) + a `WARP_SUBMIT_MAX`=32 KiB cap (I-32).

**Sub-step B is the reusable lesson: a design's "one genuinely new robustness
mechanism" already existed.** 0.12 scoped the round-3-F1 OWED host-side rescue as
a bounded serve-loop follow-up drain for the fenced-submit path -- "the self-
reschedule the single-RPC V-3a serve loop lacks." Ground-truth (server.rs +
main.rs) showed it is `warp_service_fences`, built at W2d: it runs every serve-loop
iteration, is bounded per pass by `FENCED_SLOTS` (16, the device fence-slot ring),
and `warp_venus_submit` posts a CTX fence retired by it identically to a virgl
submit (delivered by `poll_fences` on the same `ctx/<id>/fence` surface -- NOT
`poll_ring_fences`, a mis-attribution in the 0.12 draft I also corrected). So no
new mechanism lands; the verification IS the deliverable. What caught the over-
build was the resume note I wrote at the compaction ("verify whether a genuinely
new mechanism is needed or the OWED note is discharged") -- the note, not the
design doc, carried the doubt. The round-3-F1 note's LITERAL subject is a
DIFFERENT path (the V-3a echo drain in `wring_kick`, non-host3d rings, which Model
B routes `E_OPNOTSUPP` -- virglrenderer polls); its cap-and-re-kick contract
stands (prover-honored), and its own rescue is a robustness-NOT-soundness item on
a superseded POC ring ("not Venus's ring", `34dbe5d3`) -- tracked, deferred, not
owed by V-3b-2. Landed `c1477a91` (docs + comment; no functional code).

**Sub-step C: VERIFIED on thyla-pi V3D (`84ac8a27`).** The `warp-prove ring-host3d`
client mints a host3d ring, submits a hand-built `vkCreateRingMESA`, and observes
the host set `status & IDLE` -- proof virglrenderer mapped the ring's shmem and ran
its poll thread, no Mesa. The encoding was not guessed: a source spike compiled +
ran Mesa's OWN generated `vn_encode_vkCreateRingMESA` and dumped the bytes, and a
host-side `enc_check.py` confirmed every offset before any GL boot -- which caught
that my pre-spike field guess was wrong four ways (flags-first, a missed
`idleTimeout` u64, offset-only head/tail/status, size_t=8B). A second spike traced
that NO `CTX_ATTACH_RESOURCE` is needed (create-blob-on-the-venus-ctx IS the attach
for venus; the res_id resolves by co-location), settling from source what would
otherwise have been a failed 200s GL boot.

**The reusable wrong turn: the first GL run came back UNVERIFIED, and it was NOT the
witness.** Ground-truth on the log showed the boot never reached "Thylacine boot OK"
-- the Cloudflare SSH tunnel dropped the STREAMING guest pty during the `go8e-2`
clangd probe's quiet SD-card indexing phase. What caught the false attribution:
grepping the log for the boot-OK banner (absent) and the actual last line (`clangd
session starting` -> `closed by remote host`), then noticing `boot-probe.sh` boots
the SAME venus image fine because it POLLS a log file (link stays busy) where the
exp STREAMS (goes idle). The fix was one `ServerAliveInterval` on the streaming ssh
leg; the re-run booted clean and passed. Had I retried blindly it would have dropped
at clangd again -- the diagnosis, not a retry, is what fixed it.

**Sub-step C's gate scope (verified, not assumed):** the whole arc
(`6a1cdd21..84ac8a27`) changes only tapestryd + the warp-prove test tool + tools +
docs -- zero kernel/arch/mm/specs, zero console/login. So the SMP gate, the specs,
and LS-CI test surfaces this chunk provably does not touch; the venus-submit hazard
is verified by the GL witness + the sub-step D audit + the compositor's
non-regression in `test.sh` (PASS).

**Sub-step D, round 1 (Fable 5, `4e5a1a40`): 0 P0 / 1 P1 / 0 P2 / 3 P3.** The
forward itself survived every lane the prosecutor tried -- ownership gate, cross-ctx
escape, fence pairing, DoS bounds, witness honesty -- all sound. The one P1 was not
in the new code but in what the new code was contractually obligated to change and
did not: `wctx_finish`'s wedge (leak) arm destroyed the client's venus_ctx
UNCONDITIONALLY, on a 1c-2a premise ("quiesced by construction -- no submit targets
it"). The instructive part is that the SAME block carried a FORWARD comment naming
V-3b-2 as the deadline to move it -- the author of 1c-2a had seen the future and
written it down, and sub-step A (which landed venus submits onto exactly that
venus_ctx, sharing `fences_in_flight`) had crossed the deadline without moving the
code. So a wedge could now leave a venus chain live device-side while the arm
destroyed its context -- the destroy-with-live-work breach the dev_ctx defer exists
to avoid -- reachable in the SHIPPED build via the mode-0666 warp-abandon ctl. My
own parallel self-audit confirmed the same sound lanes and found F3's accounting
asymmetry, but MISSED F1: the classic self-audit blind spot is the recovery path the
changed function never opens. The context-independent prosecutor caught it. FIXED by
mirroring dev_ctx: a new `warp_ctx_venus_vindicate[slot]` flag; the leak arm records
it; the vindication destroys `WARP_VENUS_CTX_BASE+slot` only once the poisoned-slot
gate proves the device finished (that gate covers venus chains -- they tag with the
same pub_id). F2/F3/F4 were comment/accounting P3s.

**Round 1 was a dirty close** (a P1 returned + the fix restructures the
teardown/vindication lifecycle), so a **round 2 re-audit on the fix itself** was
owed. **Round 2 (Fable 5, scoped to the F1 restructure): 0 P0 / 0 P1 / 0 P2 / 2 P3
-- CLEAN.** The prosecutor's null results on all five focus lanes -- partial-destroy
re-fire, flag lifecycle, the device-finished-proof coverage over venus chains, the
clean-arm premise, and the F3 accounting -- matched a second independent parallel
self-audit exactly. My own initial "partial-destroy re-fire" worry (dev destroyed,
venus refuses, retry re-destroys dev) DISSOLVED on tracing `take_vindications`: it
drains once, so there is no second pass -- the failure mode is a permanently
condemned slot (fail-closed, bounded at <=8), not a re-fire. The two P3s were both
real and both fixed. F1: the vindication destroyed dev FIRST and `continue`d on a
refuse, never reaching the venus attempt -- asymmetric with the clean arm, so a
healthy-engine dev mismatch stranded venus_ctx too; fixed to attempt both and
reclaim only when both are gone. F2 is the lesson of the run: the ring-teardown
comment STILL stated the dead V-3a "no submit lands" premise -- the EXACT
stale-premise shape that produced round 1's F1, now one function over. The ring free
is guest-safe today, but via a chain the comment does not state (host-memory
backing, not guest DMA; a fresh-blob re-mint; trusted-host renderer robustness;
monotonic res_id), and on the v3d fork -- where the renderer becomes ours -- that
comment would be an actively false safety argument at the first site the next
implementer reads. Rewrote it to the real chain plus the v3d obligation. A comment
is not a footnote when it is load-bearing for a future implementer's safety
reasoning; the same rot caught twice in two rounds is the argument for saying so.

**The F1 regression is the durable-reasoning form, and that is a decision, not a
dodge.** The fully-discriminating test -- force a wedge on a venus chain, finish the
ctx, observe that venus_ctx is NOT destroyed until the vindication -- is feasible
(`gpu.test_abandon_ctx` gives a synchronous wedge) but needs a venus device
(thyla-pi only, not the CI mac), new gpu `ctx_destroy` observability, and thyla-pi
iteration to make the abandon->vindication timing deterministic (a `vkCreateRingMESA`
create-fence completes -> a vindication, but the ring itself persists). Per the
regression carve-out for hard-to-deterministically-trigger lifecycle findings (the
Weft-arc precedent), the durable record is the R-1/R-2 audit trail + the in-code F1
invariant comments; the thyla-pi wedge-leg is a tracked deferral, named in
`memory/audit_v3b2_r2_closed_list.md`, not a silent drop. The fix is fail-closed,
bounded, and now double-audited sound.

**Shipped.** The R-2 close (`b0978d6c`) + hash fixup (`c51568bc`) rebuilt with the
R-2 tapestryd baked into a fresh ramfs (pool PRESERVED, key kept -- paired, no
STM_EBADTAG); `test.sh` non-regression PASS (boot OK, arc 2/2, clade 3/3, 0
FAIL/EXTINCTION -- the change is teardown-path only, so this confirms the shipped
artifact boots + the base suite holds, exactly the by-construction bet). Pushed
`6a1cdd21..c51568bc` to both mirrors; `ls-remote` verified codeberg == github ==
HEAD. The whole V-3b-2 chunk -- the Venus SUBMIT_CMD forward, standalone
GL-witnessed, double-audited -- is done. NEXT on the arc: warp-prove cross-Proc
E2E (witness), or the reply-shmem / Mesa deferrals (design-forks).

---

## 2026-08-24 — V-3b-1c-2b: the client-claimable host3d ring, and a reap predicate that watched the wrong count

The operator voted **A** on the parked 1c-2b fork: design F2 now rather than build
V-3b-2's thin plumbing over a blocked path. That vote was itself the fruit of a
correction — my resume note claimed V-3b-2 was "self-test-provable," and recon
showed that oversold it: V-3b-2's only honest witness is a real venus stream,
which needs either the parked 1c-2b client-map or Mesa, both blocked on the same
F2. So the productive move was to unblock F2, and the operator's own reason for
parking (an un-rushed design pass, not a post-600k scramble) had become true — we
were fresh past a compaction. I surfaced that, they chose A.

**The design.** F2 is a lifetime split: a hostmem ring's HOST bytes (a QEMU
subregion tapestryd owns) live OUTSIDE the kernel's #847 dual-count, so
`drop_host3d_ring` reclaiming the offset while a client still maps the GPA
re-hands it under live PTEs — a cross-client alias. The kernel side is already
sound (the V-2 death-quiesce keeps the BAR decoded across a client mapping); the
gap is purely tapestryd's offset reuse. Prior art settled the shape: Fuchsia VMO /
Genode dataspace keep device-memory lifetime IN the kernel so no userspace free
races the refcount; Thylacine can't (the unmap is a controlq op only tapestryd
issues), so the next best is to make tapestryd OBSERVE the count before it frees —
exactly `image.c`'s eviction check (`handle_count==1 && mapping_count==0`), lifted
to userspace. That needs one new read-only syscall. Options (reaper+syscall vs
leak-on-claim vs kernel-owns) went to the operator; (a) was ratified with a
`SYS_HOSTMEM_MAPCOUNT` returning `mapping_count`. Landed as scripture first
(`5f8cf9c2`), then the impl (`7696540a`).

**The suite caught my first bug before the prosecutor did.** The F1 regression
test I added called `weft_binding_release` — which drops the registration pin
`weft_share_claim` holds in production — without providing that pin, so it ate the
construction handle and the teardown's `burrow_unref` double-freed. EXTINCT, mid
`weft.hostmem_share`. The gpu_bo sibling survives the identical call because it
gets the pin from `sys_weft_share_for_proc` and drains mappings instead of a final
unref. A `burrow_ref` to stand in for the claim's pin fixed it. A test's own
refcount can carry the bug it exists to catch.

**Then the prosecutor caught the one that mattered — and it was mine.** The Fable
holotype (context-independent, same family) refused the ratified `mapping_count`
predicate. `weft_share_claim` consumes the share and TRANSFERS the registration
pin — a `handle_count` ref — to the client, and returns BEFORE `burrow_share_into`
bumps `mapping_count` later in the same `SYS_WEFT_MAP`. In that window a client is
irrevocably going to map GPA(off), yet `mapping_count` still reads 1. My reap
would have freed the offset under the pending map — the exact cross-client alias
the chunk exists to prevent. And the tell was in my own citation: `image.c`'s gate
is `handle_count==1 && mapping_count==0`, and *the handle half is the mechanism
that excludes the in-flight mapper* — I quoted the precedent and dropped the half
that did the work. The fix makes the syscall return the SUM
(`SYS_HOSTMEM_REFCOUNT` = handle + mapping): the transferred pin makes it >= 2, so
the reap parks; the full image.c predicate folded to one value. Latent only
because no in-tree program claims a host3d ring yet — but this chunk wires the
claim path end to end, so it had to close before exercise. Two more: the reaper
ran at mint, not the completion pump its own comment + the design claimed (fixed
the comment + the design + added a per-pass cap); and the reap decision had zero
coverage, so the redesigned `weft.hostmem_refcount` now asserts the F1 window
directly — a pin-but-no-map burrow reads total 2 (PARK) though `mapping_count`
alone is still 1. Audit-close `748be17e`.

The reusable lesson is sharper than "cite carefully": a lifecycle predicate ported
from one actor's vantage (the kernel cache holds a HANDLE) to another's (tapestryd
holds a MAPPING) must re-derive which refcount half carries the safety, not
transliterate the one that happened to be visible. Dirty close (a P1 back + the
predicate changed), so a round-2 holotype prosecuted the fix — and found the SUM
itself unsound: `handle_count + mapping_count` is two SEPARATELY-ACQUIRE'd loads,
and which is read first is *unspecified for the operands of `+`*. A mapping-first
read could see `mapping==1` in the claim window, then — while a peer CPU completes
the claimant's map + fork + pin-release — read `handle==0` and sum to 1,
reclaiming under a live map. Today's binary is
handle-first only because clang emits left-to-right; GCC routinely goes the other
way. The very act of folding image.c's two-count gate into one observed value had
reintroduced the non-atomicity the kernel's counts avoid by living under
`v->lock`. Fix: `burrow_total_refs` reads both under `v->lock`. The reusable half:
**a sum of two lock-free counters is not one read** — operand order is a
correctness variable, and "under a lock" must mean under the lock that guards *the
counters*, not merely *a* lock (the round-1 `as->lock` comment asserted exactly
that false comfort). The round-2 fix is a 5-line locked accessor — the
prosecutor's prescription, verified not transliterated. Audit-close `f7021c7a`.

Round-3 (the dirty-close discipline, a P1 having returned in round-2) came back
**clean: 0/0/0/4 P3**, all mechanism-accuracy prose, no runtime change. It didn't
just re-assert soundness — it PROVED it: a complete enumeration of all 22
`v->lock` sites showed none takes an AddrSpace lock under a Burrow lock, so the
`as->lock -> v->lock` order has no reverse edge and cannot ABBA; all five count
writers mutate under `v->lock`, so the snapshot is genuinely coherent; the mint
reap-loop strictly shrinks the parked list, so it terminates. The catch worth
recording: F1 flagged that *my own round-2 rationale* had pinned the exploit window
on IRQ-preemption ("`as->lock` leaves IRQs open") — wrong, because the production
syscall caller runs IRQ-masked end-to-end; the hazard was always SMP cross-CPU and
masking never entered into it, only `v->lock` does. I had corrected the fix and
mis-explained it in the same breath; a context-independent reader caught the
explanation the family-shared one would have read straight past. The other three:
the reap doc's per-mint bound (a pressured mint loops several passes → per-PASS,
list-length-bounded); server.rs's teardown comment still naming the pre-round-1
`mapping_count` predicate; and a phantom `d9c...` hash this run committed for the
round-2 close instead of `*(pending)*` + a fixup (→ f7021c7a). All fixed;
round-3 close `3de39ad0`.

**Runtime witnessed.** The suite is green on both `f7021c7a` and the
round-3-close tip (`weft.hostmem_share` + `weft.hostmem_refcount` PASS, 1387/0,
boot OK). The GL self-test on real V3D (thyla-pi KVM) printed `warp host3d-ring
venus-ctx=512 MAPPED+ROUNDTRIP refcount=1 teardown OK` — the `refcount=1` is
`burrow_total_refs` returning the coherent snapshot at runtime, `teardown OK` is
`retire_host3d_ring` taking its ==1→drop path — with the 2D control correctly
`skipped`. On the local 2D device the host3d self-test skips, so no local boot
drives the new syscall; the GL boot is its only runtime witness, not an SMP
re-run. The `warp-prove` cross-Proc E2E (a real client Proc claims + maps + the
cross-client-alias reproduction) stays a tracked follow-on.

## 2026-08-24 — V-3b-1c-2b-a: a green gate over a dead claim path (reverted, parked)

Rolled straight into 1c-2b after 1c-2a landed: the client-claimable host3d ring.
The tapestryd change was one line — `wring_weft_ensure`'s `if r.dma_fd < 0` bail
became `&& r.host3d.is_none()`, so a host3d ring's hostmem burrow gets
`t_weft_share`d and routes to the kernel's `WEFT_BIND_HOSTMEM` (weft.c:401). I
extended the boot self-test to weft-share the ring, added a `WEFT_SHARE` gate leg,
and it all went green: build clean, 29/29 discriminator, and — the part that
should have been reassuring and was in fact the trap — **VENUS GATE: VERIFIED on
real V3D**, `warp host3d-ring venus-ctx=512 MAPPED+ROUNDTRIP WEFT_SHARE teardown
OK`.

The Fable holotype refused the green. **F1 [P1]: the client CLAIM is structurally
dead.** Four kernel sites must admit a kind for a weft share to be *claimable* —
register, kind-decision, client-map, and the binding alloc. V-2 widened the first
three for HOSTMEM; `weft_binding_alloc_maponly` (weft.c:472) still requires
`BURROW_TYPE_DMA` and handles only weave/gpu_bo, so a hostmem burrow returns NULL
and the client's `t_weft_map` unwinds to -1. My WEFT_SHARE gate certified the
*register* half — `t_weft_share` succeeds — as "the client-claim substrate," and
the self-test never once exercised the binding alloc, so it was green over a claim
that cannot happen. This is the [[bug-240-new-gate-hollows-old-negative]] shape
inverted: a widen that touches N-1 members of a property set the "must widen
together" comment (syscall.c:6004) itself names, leaving the last a dead half — and
a self-test written to prove the whole path that only ever drives the live part.

**F2 [P2] is worse and masked by F1.** For guest-blob rings the #847 dual-count
pins the guest-RAM *pages*, so a client's mapping survives teardown. A hostmem
ring's backing lives behind the GPA (`map_blob`'s subregion) — the dual-count pins
only the kernel Burrow *object*, and `drop_host3d_ring` yanks the host backing +
re-hands the offset unconditionally. The instant F1 is fixed, tearing down a
claimed ring exposes one client's live mapping to another's ring. The teardown
comments asserting "the client's mapping survives via its own ref" were vacuous
under 1c-2a and would have become load-bearing and false. tapestryd cannot even
see the kernel `mapping_count`, so the fix is a real lifetime design (a reaper with
a new syscall, or leak-on-claim, or a kernel primitive) — not a patch.

**My self-audit missed both**, the same way as 1c-2a's F1: I verified ownership,
extent, and teardown of the *unclaimed* (self-test) case exhaustively and never
traced the client *claim* through all four kernel sites — I stopped at the surface
I changed. And critically the delta *regressed* the client path (from a clean
`E_NOMEM` to a half-broken map fid), so it could not land. Surfaced the F2 design
fork to the operator (design-conversation pattern); they voted **park 1c-2b, do
V-3b-2 next** — F2 deserves a design pass, not a rushed post-600k call. Reverted
the delta (tree pristine at `3e12ef12`); findings enqueued in
`memory/bug_v3b_1c2b_hostmem_weft_claim_gap.md`. The reusable lesson, twice this
run: a green gate proves what its self-test *drives*, and a self-test that exits
before the load-bearing call is green over a hole.

## 2026-08-24 — V-3b-1c-2a: the server host3d-ring path (three catches the local gates could not make)

Same autonomous run, resumed past a second self-compaction. 1c-1 was the engine;
1c-2a wires it into the `/srv/warp` server so a HOST3D ring is a client-mintable
flavor under a per-client venus device-ctx. The plumbing was routine — the value
of this entry is the three defects, each caught by a *different* instrument
because the cheaper one was structurally blind to it.

**The recon was wrong, and a grep caught it before a line was written.** The
pickup pinned the venus ctx id as `COMPOSITOR_CTX + 1 + slot`. Before writing it
I enumerated every `ctx_create*` id in the daemon (the enumerate-mirrors reflex)
and found `CONV_PROBE_CTX_BASE = COMPOSITOR_CTX + 1` — the conv-probe throwaways
occupy exactly that base. The two families were temporally separated (conv probes
die before any client mints), so the alias was latent, not live — which is
precisely how it would have survived review. Moved the band to a dedicated
`0x200 + slot` with a `const _` gap assert. The lesson is old (a recon note is a
hypothesis, not a fact) but the mechanism is worth naming: a pure-function id
scheme (`base + slot`) collides silently with any *other* pure-function scheme
sharing the base, and only a full enumeration — not a spot check — finds it.

**F1 was mine to catch and I didn't; Fable did.** My self-audit reached the
`wctx_finish` leak arm, saw it did not destroy the venus ctx, and reasoned:
"consistent with dev_ctx (which the leak arm also leaves alive), and the slot is
poisoned so the id can't be reused — fine." That is half the machinery. What I
did not trace is the *vindication* path: when the device finishes the abandoned
chain, it destroys dev_ctx, **un-poisons the slot**, and recycles it — destroying
only dev_ctx, never venus. So a wedged-then-recovered slot leaks its venus ctx
*and* the next client that lands there re-mints `WARP_VENUS_CTX_BASE + slot` into
a still-live host context (EEXIST → that slot permanently loses host3d). The fix
is the holotype's option (a): destroy venus in the leak arm too — it is quiesced
by construction at 1c-2a (no submit path targets it, its rings were dropped
unconditionally just above) — and on a refused destroy skip the vindicate stamp
so the slot is permanently condemned rather than recycled into the collision.
This is the exact shape of the whole-system-stewardship failure mode: I stopped
tracing at the boundary of the function I changed; the bug lived one call away in
the recovery path I did not open. A same-family reviewer with *context
independence* (Fable had not watched me talk myself into "consistent with
dev_ctx") is what closed it.

**F2 is a disclosure armed one rung ahead.** The 1c-1 free-list hands back a
reused hostmem extent verbatim — `drop_host3d_ring` reclaims the offset but does
not scrub — and `wring_install_host3d` wrote nothing into the ring. At 1c-2a the
client claim path fails closed (`wring_weft_ensure` returns None on `dma_fd < 0`),
so it is latent; but this chunk is the substrate for the very next rung that makes
the memory client-visible, and the next author greps for "claim", not "zero". Zero
the ring at install. The 1c-1 probe's own physical-reread leg is built on the fact
that freed bytes persist across re-mint, so this was not hypothetical.

**The GL boot caught what the no-boot gate never could.** `test-venus-verdict.sh`
is 28/28 and discriminates — against *crafted fixtures*. It tests the verdict
logic, not the capture. The real venus boot on thyla-pi came back UNVERIFIED: the
control leg emitted no `warp host3d-ring skipped` line at all. Ground truth (read
the 13-line filtered log, don't theorize): `boot-probe.sh` captures only
`grep "tapestryd: gpu"` — and my self-test lives in `server.rs` with a `warp`
prefix, so the filter dropped it before the gate could see it. The 1c-1 line was
`tapestryd: gpu hostmem-ring` (gpu.rs); mine is `tapestryd: warp host3d-ring`, and
that one-word prefix difference is invisible to a fixtures-only test. Broadened
the filter to `gpu|warp host3d-ring`. This is the `test.sh(HVF) != test-interactive(TCG)`
class restated for capture: a discriminating gate can still be blind to whether
its evidence line is ever *recorded*, and only a real boot exercises the record.

Audit: holotype-reviewer Fable 5 (max, MODEL start==end, family diversity),
**0 P0 / 1 P1 / 1 P2 / 3 P3, all fixed** (F3 = my pre-landed kick guard; F4
"teardown OK" now reads the poisoned flag; F5 named the structural bound over the
compiled-out `debug_assert`). Not dirty. Re-verified on real V3D after the fixes.
Owed to 1c-2b/V-3b-2: when venus submits land, both the leak-arm
"venus quiesced by construction" argument and the kick fail-closed graduate to
real venus discipline — named in the code.

## 2026-08-24 — V-3b-1c-1: the persistent hostmem ring engine (a deliberate split)

Same autonomous run, resumed on the far side of a self-compaction at the 600k
checkpoint (the run-through rule). The pickup named V-3b-1c as one chunk: hoist
the allocator, build the client-claimable `/srv/warp` ring, drive teardown. On
reading the ground I split it, and the reasoning is the interesting part.

**Why the split.** The Model B ring, for a cross-Proc client, is a HOST3D blob
weft-shared as a `BURROW_TYPE_HOSTMEM` burrow -- and `weft.c:401` already admits
exactly that (`WEFT_BIND_HOSTMEM`, the V-2 surface built but never exercised by a
real client), reached through the same `t_weft_share(va,size)` tapestryd already
calls for the V-3a guest-blob ring. So the client path needs no kernel work --
which means the whole thing is buildable, and pull-forward would say build it
all. But it decomposes at a clean seam: the *engine* (a persistent allocator + a
reusable mint/teardown lifecycle, provable by the probe alone) versus the *client
surface* (a per-client tapestryd-owned venus device-ctx + the weft-share of a
hostmem burrow + a `warp-prove` cross-Proc leg). The engine is a complete,
non-forking, independently-auditable foundation; the client surface is a larger
new kernel-exercised path that earns its own audit. 1c-1 is the engine; 1c-2 is
the surface. This is a sub-chunk split, not a deferral of scope -- deliverables
#1 (hoist) and #3 (teardown lifecycle) are 1c-1; #2 (client claim) is 1c-2.

**What landed.** `HostmemAllocator` hoisted into `Gpu.hostmem`, sized once at
probe, with a first-fit free-list so a persistent daemon reclaims a retired
ring's offset (bump-only would exhaust the 256 MiB region). A reusable
`mint_host3d_ring` / `drop_host3d_ring` pair with full error-path unwinding
(offset -> resource -> subregion). The probe rewritten to PROVE the engine, not a
single map: two rings at distinct offsets (`0x0`, `0x1000`), a sentinel through
each guest VA, teardown of both, then a re-mint that must reuse a freed offset --
one verdict line, emitted only when all four hold.

**The gate got a real discrimination, not a token check.** The probe emits its
success line only on `a_ok && b_ok && distinct && reuse`; a lifecycle regression
(e.g. `reuse=false`) emits `hostmem-ring FAIL (...)` instead. `test-venus-verdict`
gained a leg that REPLACES the success line with a `reuse=false` FAIL line and
asserts the verdict rejects it -- so the free-list reclaim is a tested property,
not one that rides an absent-token check (the M-PIN: anchor on what only success
produces; sabotage the path under test). 24/24 discriminates, no boot.

**The holotype's best catch was a type-system one (Fable 5, 0/0/1P2/3P3, all
fixed).** F1 [P2]: `HostRing` was `#[derive(Copy)]`, `drop_host3d_ring` took it
by `&ref`, and `free()` validated nothing -- three innocuous choices that
COMPOSE into a silent double-free. The probe drops each ring exactly once, so
1c-1 is correct today; but this rung's deliverable IS the reusable engine API,
and the day 1c-2 lands a second retire path (a death reaper AND a close verb,
the shape tapestryd already has for BOs), two `Copy` handles each drop the same
ring, `free()` pushes the offset twice, and two later mints hand ONE hostmem
offset to two clients' rings -- cross-client aliasing, no log line. The fix is
the type system: drop `Copy`, take the handle BY VALUE, so a double-drop is a
compile error; the `free()` oob/overlap guard is the belt to that suspenders.
The reusable lesson: a resource handle that is `Copy` is a double-free waiting
for a second caller -- make it a move-only single-use token and let the compiler
hold the contract the doc comment cannot. F2 [P3] was the same instinct on the
probe: it proved the ALLOCATOR handed distinct offsets, not that the two guest
mappings were PHYSICALLY distinct (one sentinel constant, A never re-read after
B) -- so a kernel aliasing bug would have passed it. Offset-derived sentinels +
re-reading both after both writes makes it witness the physical fact. My own
self-audit had F1 (as two P3s) and F3, and converged with the round on the rest;
the round's upgrade of F1 to P2 (the API IS the deliverable) was the right call.

**Cost/open.** userspace + tools + docs only; kernel byte-unchanged, so no
specs/SMP delta. GL verification owed on thyla-pi (the two-ring distinct-offset +
reuse line under a real venus ctx). V-3b-1c-2 (the client-claimable ring) is next
and is where the weft-share-of-hostmem and per-client venus-ctx forks live.

## 2026-08-24 — V-3b-1b: the guest-map, and the Result alias the compiler caught

Same autonomous run as the V-3b-1a entry below, continued past that chunk's push
(the run-through rule -- a checkpoint is not a stopping point). V-3b-1b guest-maps
the HOST3D ring blob: the client binding for SYS_BURROW_FROM_HOSTMEM (which V-2
built kernel-side but never wrapped -- "client delivery exercised only by unit
tests until V-3 drives it E2E") + a tapestryd hostmem-offset allocator + a probe
that round-trips a sentinel through the guest VA.

**The build earned its keep.** The compile-check caught a real error before the
GL boot: `PciDev::burrow_from_hostmem` was declared `-> Result<u64, PciError>`,
but hardware.rs has a 1-arg `Result` alias (`crate::err::Result<T>`) in scope, so
the 2-arg form is E0107, and `Err(PciError::MapBar)` then mismatched (E0308). The
existing PciError-returning methods (claim / claim_nth) spell it
`core::result::Result<Self, PciError>` for exactly this reason; the fix matched
them. Read by CONTENT, not by the wrapper script's exit code -- that was 0
because a trailing `echo` masked build.sh's real status, a reminder to grep the
log for `error[` rather than trust `$?` through a pipeline.

**The sentinel proof, and its limit, stated.** `hostmem_sentinel` writes a u32 to
the guest VA and reads it back at the same address. ARM same-address same-core
coherency round-trips it with no barrier, so a MISMATCH means the VA does not
alias the mapped BAR. It proves the guest can ACCESS the blob -- NOT that
virglrenderer sees the guest's writes (host-visibility), which is deliberately a
later rung (the ring poll, V-3b-1c/2) and is not claimed. The returned VA
reaching the BAR is the kernel's V-2 guarantee; the sentinel confirms the mapping
is live.

**The audit caught a design bug I would have shipped.** Fable 5 (family diversity
restored this round), 0 P0 / 0 P1 / 1 P2 / 3 P3. F1 [P2]: the probe hardcoded
`T_CACHE_WC` and *discarded* `map_blob`'s `map_info` -- but the host dictated
`map_info=0x1` (CACHED), and GPU-DESIGN 6.2 is signed-off that the guest maps the
attribute "honored exactly". A guest-WC vs host-WB alias is the ARM64
mismatched-attribute hazard the scripture's own field-agreement warning forbids;
it would have surfaced TWO rungs later at V-3b-1c as a "host never sees the kick"
coherency mystery on real-silicon KVM, with a comment on the FFI actively
pointing the debugger at write-combining (the x86 intuition, wrong on ARM). I had
written that comment myself. Fixed by consuming `map_info` -> `map_info_to_cache`
-> passing the host-dictated attribute, and rewriting the comment to state the
rule. The reusable lesson: an attribute you *choose* for a shared mapping is a
claim about the other side's mapping -- derive it from what the other side
dictated, never from what feels right for your access pattern. F2-F4 [P3] all
fixed (zero-size alloc alias; the leaked offset-0 mapping now `t_burrow_detach`'d
-- the "no detach primitive" comment was wrong, tapestryd already uses it; doc rot).

**Cost**: two pi boots (the venus verb, re-run after the F1/F3 fixes changed the
probe's behavior).

## 2026-08-20..24 — V-3b-1a: the HOST3D substrate, and the render server that wasn't there

Model B's first rung: the tapestryd primitive that mints a HOST3D blob and maps
it through the hostmem window (`create_host3d_blob` / `map_blob` / `unmap_blob`
in `usr/tapestryd/src/gpu.rs`), plus a two-arm `host3d_probe` init self-test that
proves the path against the real host. Small code; the run's weight was in
proving it on GL, and the proof took a four-boot hunt through a host-side blocker.

**The wire-format groundwork paid off twice.** Before a line of code, the
constants were re-derived against QEMU v10.0.2's verbatim `virtio_gpu.h` enum,
not the plan. Two catches: `RESP_OK_MAP_INFO` is `0x1106`, not the plan's
`0x1105` (that value is `RESOURCE_UUID` -- a silent off-by-one that would have
made every MAP_BLOB read the wrong response type); and a WebFetch fast-model
"summary" miscounted `CMD_RESOURCE_CREATE_BLOB` as `0x010d` -- refuted by the
already-GL-proven shipped GUEST-blob code, which uses `0x010c`. Ground truth
(the verbatim enum + working shipped code) beat both secondary sources.

**The GL hunt (four pi boots + a source build).** Boot 1: HOST3D create refused
`resp_type=0x1200` (RESP_ERR_UNSPEC) under both a virgl context and
device-global, while a GUEST blob still created fine. Source-cited to the vkr
(venus renderer) shm path -- a `blob_id=0` `USE_MAPPABLE` HOST3D blob is reached
ONLY via a capset-4 context (`vkr_context.c:369-372`) -- so the fix was to mint
Arm A under a venus context. Boot 2: STILL refused under the venus context. That
refuted the venus-ctx-alone theory and pushed the hunt down into virglrenderer,
where a `-d guest_errors` boot named the real error: `virgl blob create error:
Invalid argument` = `EINVAL` from `virgl_renderer_resource_create_blob`, with NO
fork-fail line anywhere.

**The wrong turn, and what caught it.** That missing fork-fail line led me to
write "the render server is likely irrelevant, in-process mode" and to tell the
operator their earlier "build the render server" instinct was refuted by the
errno. That was WRONG. The operator had ratified building the RS on my first
(render-server-missing) diagnosis; I then talked myself out of it on an *absence*
-- no fork-fail log -- which is not evidence. The catch was instrumented
ground-truth, not more reasoning: a non-destructive LD_PRELOAD boot with an
instrumented virglrenderer (my `libvirglrenderer.so.1.9.0` + my
`virgl_render_server` via the `RENDER_SERVER_EXEC_PATH` getenv override) traced
`ctx_lookup(202)=<registered> ... get_blob ret=0` and printed the substrate's own
proof line -- `tapestryd: gpu host3d-map venus-ctx MAPPED (map_info=0x1)`. The
render server WAS the root cause: Debian's `libvirglrenderer1` is process-mode
and ships no `virgl_render_server` binary (no package provides it), and without
it `get_blob` returns a bare `EINVAL` -- no distinct fork-fail log, which is
exactly the absence that misled me. The operator's original instinct was right;
my errno-based refutation was the wrong turn.

**The reusable lesson**: an absent error log is not evidence of absence of a
cause. Three rounds of reasoning (render-server-missing -> refuted-by-errno ->
re-diagnose) chased their own tails; one instrumented boot that printed the
actual `get_blob` return value ended it. When a mechanism has no distinct failure
signature, instrument it -- do not infer its absence from silence.

**The fix + the rigorous confirmation.** The RS binary was built from
virglrenderer 1.1.0 source and installed to `/usr/libexec/virgl_render_server`
(additive; does not touch `libvirglrenderer.so`). The proof was then re-run with
the PURE SYSTEM library -- no LD_PRELOAD, no env override -- and still showed
`host3d-map venus-ctx MAPPED`, with the device-global arm refused (the negative
control). So the instrumented lib was never load-bearing; the RS binary alone is
the fix, and thyla-pi venus is now functional for all HOST3D work.

**Cost**: four pi boots (~220 s KVM each) + one ~30 min virglrenderer source
build, ~1.9 h of pi lease.

**Decisions that needed the operator** (three AskUserQuestion votes): confirm the
errno then build the RS; instrument virglrenderer non-destructively; install the
RS to `/usr/libexec/` permanently.

**Still open**: this is rung 1a of Model B. Ahead: V-3b-1b (the hostmem-offset
allocator + the guest map via `SYS_BURROW_FROM_HOSTMEM`), 1c (the Model B ring
subtree), V-3b-2 (the SUBMIT_CMD forward of the raw venus stream + reply-shmem),
V-3b-3 (the Mesa `vn_renderer_thylacine` backend -- the thyla-keep cross-build).
A provisioning note now lives in WARP-V3-DESIGN section 0.6: any fresh venus GL
host needs the `virgl_render_server` binary, or HOST3D resource ops fail with a
bare EINVAL.

---

## 2026-08-20 — V-3b design pass: the ring Venus can't use, caught before the code

The operator chose "V-3b Venus, design first" after V-3a pushed. The pass -- two
prior-art research agents + a focused fork-resolution spike -- found the V-3
arc's foundational premise was WRONG, and caught it before a line of the
~1.2 kLOC Venus backend was written against it.

**The premise that failed.** WARP-V3-DESIGN (section 2) had it that the V-3a
coherent ring IS Venus's command ring. The spike proved otherwise, source-cited
against Mesa 25.0.7 + virglrenderer main: (1) unpatched Venus creates its ring
UNCONDITIONALLY (`vn_instance.c:320`, no gate, fatal on failure) and routes every
real Vulkan command through it -- only 4 bookkeeping commands use SUBMIT_CMD; (2)
the ring MUST be host-allocated shmem (`HOST3D`/`FD_SHM`) -- virglrenderer fatally
rejects a non-FD_SHM ring (`vkr_transport.c:201`) and Venus's driver hard-codes
`HOST3D`, refusing guest memory (`vn_renderer_virtgpu.c:1457`; host process
isolation can't deref guest sglists). The V-3a ring shipped at `f12d7317` is a
`blob_mem=GUEST`, tapestryd-consumed ring with head=producer/tail=consumer --
wrong backing AND the opposite head/tail convention from Venus's
virglrenderer-consumed HOST3D ring. It cannot be Venus's ring.

**Why this is the design pass earning its keep.** The premise was about an
EXTERNAL system's requirements (how Mesa's Venus + virglrenderer expect their
ring), and it was never verified against their source before the V-3a substrate
was designed, built, audited (three rounds), and shipped. The design pass -- not
the implementation -- caught it, before the backend was written against a ring
Venus would reject at instance creation. The reusable lesson: a design premise
about an external system must be checked against that system's source before you
build the substrate that depends on it; three green audit rounds on V-3a proved
the ring SOUND, not that it was the RIGHT ring.

**The resolution.** The fork was surfaced with the research attached; the
operator ratified Model B (virglrenderer polls a HOST3D ring, minted by tapestryd
via the V-2 hostmem path, tapestryd staying venus-agnostic -- the upstream model,
with production precedent). WARP-V3-DESIGN section 0 now records the finding +
Model B + the corrected premise. V-3a is not wasted: a valid coherent-ring
primitive for a native (non-Venus) client, its /srv/warp ring ABI surface partly
reusable -- but its tapestryd-consumer core is off the Venus path. Also settled
in the pass (fork-independent): the OWED host-side rescue (a needs_drain
serve-loop sweep) and `ops.wait` (t_poll on the fence fd -- the one backend risk,
dissolved). No code landed; the next step is the Model B implementation.

## 2026-08-19 — V-3a green on virgl, and the DoS one thread couldn't show

Resumed from a self-compact with the `1<<43` encoding fix committed (`2fb542c6`)
but its ramfs never rebuilt or re-verified on virgl. This run took it green --
and a dirty-close re-audit found a box-wide DoS that round 1, and the
single-threaded prover, were both structurally blind to.

**Green on virgl.** Rebuilt with `1<<43`, synced to thyla-pi, ran the ring gate
under KVM on real V3D: `WARP-6 V-3a GATE: VERIFIED` -- the full round-trip (map +
doorbell + feedback + fence), F2 geometry, the two-conn I-45 ownership gate, and
the I-9 re-scan discrimination all pass on virgl. The encoding is now
build-enforced disjoint (the `const _: () = assert!` over all six qid tags
compiles, which is the proof), so the class that took two GL boots to catch last
run cannot recur silently.

**The DoS the prover couldn't build (round-2 F1 [P1]).** With the encoding sound,
the owed dirty-close re-audit (Opus 4.8 fallback, MODEL start==end) re-derived the
round-1 dispositions instead of trusting them -- and F6 ("the per-kick drain
bound is V-3b's; at V-3a the guest is blocked on the kick RPC, so head is fixed
and the loop is one pass") fell. The premise conflated two actors. The KICK RPC's
caller is blocked, yes -- but `head` is not the caller's to hold still: it is
CLIENT-WRITABLE shared memory (the ring maps RW into the client via weft; the
prover itself writes `head` at `warp-prove/src/main.rs:534`), and tapestryd is
single-threaded (`main.rs`, zero thread spawns). So a client with a SECOND thread
spins `head += 64` while the first kicks: `wring_kick`'s `loop` re-reads `head`
fresh every pass, always sees `head > tail`, sets `tail := head`, and never
terminates -- one unprivileged client freezes the compositor for every other
conn. It is reachable at V-3a, not V-3b; V-3b's real submit only makes each spin
iteration costlier. The single-threaded prover cannot construct the
concurrent-advance window, which is exactly why round 1 read green -- the textbook
latent-P1.

**The fix, and a regression the prover CAN run.** Cap one kick's drain at
`WARP_RING_MAX_DRAIN_PER_KICK` (4096) passes: on the cap, publish `idle=1` and
return, and the guest re-kicks for the rest, so no one kick monopolizes the serve
thread (a legit V-3a kick drains in ONE pass, so the cap is never hit in normal
use). The regression is the interesting part: the prover is single-threaded, so
it cannot reproduce the concurrency -- so I generalized the `ring-inject` lever
from a one-shot bool to a COUNT (`ring-inject <ridx> [count]`, one advance
consumed per re-scan pass; `count==1` preserves the I-9 witness exactly). A
512 KiB ring + `count=5000` (> the 4096 cap) drives ONE kick's drain past the cap;
the leg asserts `0 < delta < 5000` (bounded), then re-kicks to stable and asserts
the full 5000 eventually drain (the cap DEFERS work, it must not DROP it). It
fails on the pre-fix code (one kick drains all 5000) and passes on the fix -- and
it passed on virgl in the gate above. Also fixed F2 [P3]: the inject arm's
`tail + WARP_RING_HDR` -> `saturating_add` (a client can set `tail` near
`u64::MAX`; an overflow-checked build would abort). Everything else round 2
re-derived sound (encoding, SeqCst, per-ring noscan, I-45 end-to-end, F5 rewind,
`PendingRingFence` lifetime, I-32, I-7).

The lesson worth keeping: a "defer to a later phase" disposition is only as good
as the actor model it rests on. "The guest is blocked so the shared word is
fixed" was true of the WRONG actor -- the caller, not the client's other threads
-- and a shared-memory word has no single writer to be blocked. When a deferral's
safety argument names one actor for a resource that several can touch, re-derive
it.

Committed the round-2 close at `07767462` (on the stop-hook synchronous-await
enforcement `76975050`, an orthogonal ratified-feedback re-land recovered this
run from a self-compact clean-tree revert).

**Round 3 (the dirty close a P1 owes) found the fix's SECONDARY claim was
overstated (F1 [P2]).** The cap-break publishes `idle=1` and breaks WITHOUT the
post-drain re-scan below it -- so it silently drops the host's half of the I-9
register-then-observe promise for any advance still pending at the cap. My fix
comment claimed "the guest re-kicks for the rest, so no work lost"; the
documented doorbell protocol never obliged the guest to re-kick. A doc-conformant
multi-threaded client that advanced head while `idle==0` (eliding its kick,
relying on the host re-scan) would strand its own advance and park on the fence
forever. It is LATENT -- the only V-3a ring client is the single-threaded prover,
which re-kicks explicitly, and a malicious client only strands itself (the DoS
bound, the fix's PRIMARY goal, is sound and effective). It materializes at V-3b's
Venus (a doc-conformant pipelined ring). Two lessons stack here: round 2 was "a
deferral premised on the wrong actor"; round 3 is "a fix that solved its primary
job and quietly shifted an unstated obligation onto a future consumer to claim
the secondary one." Fixed correct-by-CONTRACT now (the guest obligation is a
documented term at the cap-break + the const + the `wring_kick` doc, and the
prover honors it); the robust host-side rescue -- a follow-up drain the serve
loop runs after other conns -- is OWED at V-3b, where the pipelined drain
replaces this echo and a self-reschedule primitive (absent at V-3a) exists
([[design-v3b-ring-kick-rescue-owed]]). F2 [P3]: warp-prove leg 8's `flood`/`big`
were silently coupled to the server-private cap const -- pinned with a comment
both sides. Both round-3 fixes are pure comments (no binary change), so the green
gate above still holds byte-for-byte. Round-3 close: `067849b6` (the arc is userspace + docs + tools ONLY -- zero
kernel/arch/mm/specs from the pushed `60f6c929` -- so specs + the SMP gate are
non-regression on a byte-identical kernel; the ring is GL-verified + suite-green;
the interactive gate is the userspace boot confirmation). Everything else
re-derived sound across three rounds.

**The close (this run): green, documented, pushed.** The interactive gate
(LS-CI, `brs0ccizd`) went 37/37 -- the last push-bar gate. The vault was rung:
`sub-tapestryd` gained "The coherent ring lane" (the mechanism is vault-OWNED --
server.rs/gpu.rs -- so the prose belongs there, not in the reference doc), plus
`inv-i9` in guarded-by and a Tests pointer; the prover binary `usr/warp-prove/src`
is UNOWNED, so its ring-verb reference went to `docs/reference/149-warp.md` and
the coverage decision is filed as `seam-warp-prove-unowned` (vault commit
`6da4b11e`, on the local-only `vault/bootstrap` branch). The whole stack pushed to
both mirrors and was verified by ls-remote on each URL: `85526127`. The
reference/vault split is the reusable part -- `quaestor owner` returns MIXED on a
mechanism+prover diff, and BOTH actions are owed, not the one the exit status
names.

## 2026-08-19 — V-3a: the coherent ring, and the "local" premise that wasn't

Built the Warp-6 V-3a coherent-ring mechanism whole in one pass (the design
`60f6c929` said it does not decompose into stubs, and it doesn't): the
`ctx/<id>/ring/<ridx>/{info,map,kick,fence}` subtree in tapestryd
(`usr/tapestryd/src/server.rs`) -- a weft-shared, coherently-mapped GUEST blob
with a control header (head/tail/idle/seq), the doorbell with the I-9
register-then-observe re-scan, the fence feedback slot + a blocking fence file,
F2 geometry validation (refused-not-clamped), the I-32 backing charge, and the
I-45 owner gate. Plus a `warp-prove ring` client exercising the round-trip + F2
+ I-45 + the I-9 discrimination (a `ring-inject`/`ring-noscan` test-lever pair:
an injected mid-drain head advance is DELIVERED with the re-scan and LOST
without it). Compiles clean, zero new warnings.

**The wrong turn, and what caught it.** The design's sub-chunk table called
V-3a "local, no builder." The first local run hung silently: `warp-prove ring`
produced NO output for 90 s, three deterministic attempts. Ground-truth-first
(no theorizing): a warmup `echo` proved ut runs commands, isolating the failure
to `/warp-prove ring` specifically; stripping ANSI from the raw console showed
the command ran and returned to a clean prompt having printed nothing. The
decisive read was the device banner -- `virgl=0 blob=0` -- and `server.rs:8127`:
`ctx/new` is virgl-gated (`E_OPNOTSUPP` on a 2D device), the twin of the SUBMIT
gate. The ring lives UNDER a warp ctx, so **the mechanism cannot be minted on a
2D device at all** -- the "local, no builder" premise was wrong. (The silent
"hang" was actually a fast clean exit whose prover output never appeared,
because I first ran it as `/warp-prove` -- an absolute path -- and the relative
`warp-prove` form ut resolves via PATH worked and printed everything: a separate
ut absolute-vs-relative exec oddity, enqueued, not chased.)

**What that means, exactly.** V-3a's mechanism proof needs a virgl DEVICE (the
GL host), not local 2D. The local 2D path is now proven-GRACEFUL: the prover
prints `RING SKIP -- no virgl on this device (ctx mint unavailable)` and
tapestryd does NOT hang (`ctx/new` fails clean). A local "deviceless ctx" test
lever was considered and REJECTED -- it would green an unconstructed state (a
configuration production's 2D devices can never reach). The test moved to
`tools/warp/warp-ring.exp` (GL host, via `tools/warp-host.sh ring`), mirroring
`warp-prove`; the design doc + this journal record the correction.

**The GL-host loop, and two encoding traps only virgl could catch.** The
prosecutor (Opus 4.8 fallback, MODEL start==end) INDEPENDENTLY found the headline
P0 -- `WARP_RING = 1<<37` collides with the 30-bit id field, so `warp_id` can't
round-trip a ring path and nothing resolves -- the same bug the first GL boot hit
(`ring ctx minted` then `open-for-read` on `ring/0/info`). Its suggested fix
(`1<<40`, "bits 40/41 are free") was ALSO wrong: `1<<40` is `SURF_FLAG`, so
`is_surf(ring)` went true and the walk misrouted to the surface arm -- the second
GL boot failed identically. Ground truth pinned it: `say!` diagnostics showed
the ring minted + the ridx walk resolved, but the `info` walk arm never fired,
because an EARLIER `is_surf` arm swallowed it. The real fix is `1<<43` -- bits
38..42 are ALL taken (WARP_BO/WARP_CTX/SURF_FLAG/PANE_FLAG/WARP_FLAG) -- plus a
`const _: () = assert!` that now checks all SIX qid tags mutually disjoint, the
guard whose absence let both my `1<<40` and the reviewer's suggestion through.
The lesson: a qid-tag-bit choice must be checked against the WHOLE tag namespace
(surf + pane + warp), not just the `WARP_*` half; and 2D-local testing is
STRUCTURALLY blind to it (2D SKIPs before a ring resolves). The other 6 findings
were dispositioned: F2 (I-9 SeqCst doorbell + a documented store-buffer contract,
replacing the AArch64-only Acquire/Release), F3 (per-ring `ring-noscan`, not a
global box-wide I-9 kill switch -- the #178 shape), F4 (a two-conn I-45 OWNERSHIP
test replacing the liveness-only one), F5 (VA rewind on the mint failure arms),
F6/F7 (documented: the drain bound is V-3b's, the seq wrap is the shared class,
`wctx_of_conn` is unambiguous by one-ctx-per-conn).

**Verified so far:** the mechanism COMPILES clean, the local 2D graceful-skip
path is runtime-confirmed, and the ring now MINTS + RESOLVES on virgl (the walk
reaches `info`). **NOT yet green:** the full round-trip + F2 + I-45 + I-9 on
virgl -- the `1<<43` rebuild was blocked mid-run by mac contention (aux's ~53m
pts trace), so the re-verification is the immediate next step. Nothing is pushed
until it is green on virgl.

## 2026-08-19 — V-2: host-visible memory, and the death path a shared BAR opened

Two threads. First, a stray `/compact`: the operator saw two `/compact` lines
after a self-compaction and asked which agent issued the second. Ground truth
(the selfcompact ledger + both scripts) showed it was neither an agent nor the
nudge watcher — it was a *premature* self-compact cancelled earlier at 560k,
whose Enter-queued `/compact` a `tmux send-keys C-u` never actually retracted; it
rode the input queue ~4 hours and fired against the already-compacted session (a
harmless "Not enough messages"). Landed as contract (`19103efe`): a queued
self-compaction is NOT yours to cancel — only the operator's (raise a blocking
question); invoke the script only on the real 600k signal. While in the ledger I
found the belay gate keyed on the mutable `@thyla-role` tag — main's compacts
logged as `aux`, colliding with aux's state and silently defeating the governor;
rekeyed it on the git toplevel (`83c7f56d`).

Then **V-2** — the first kernel memory-authority path of the Warp-6 arc: map a
subrange of a PCI hostmem BAR (Venus HOST_VISIBLE memory) into a client VA. The
ratified design (6.2.1) was wrong about the tree in two places:
- It said "add the NORMAL_NC MAIR index." The recon measured it: NC has been in
  the MAIR since P1-C (index 1). V-2 *plumbs* it — widening the fault path's
  `bool device_memory` to a MAIR index — and adds no byte. A design claim wrong
  about the tree, caught by ground truth, not by re-reading its prose.
- It said the client map "rides the existing SYS_WEFT_SHARE." The code showed the
  weft path fail-closes on unknown burrow types AND carries a duplicate admission
  gate that "MUST widen together" (its own comment, from the Warp-2b bug).
  Delivering a client mapping meant wiring the I-37 weft kind-machinery — more
  than "one syscall." Surfaced as a scope fork; the operator chose to complete it
  in V-2 (both gates widened in lockstep, `WEFT_BIND_HOSTMEM`).

The widening carried a footgun: `false == 0 == MAIR_IDX_DEVICE`, so a naive
bool->index widen would silently map every existing `false` caller as Device.
Handled by keeping `mmu_install_user_pte(bool)` as a semantics-preserving wrapper
over the new `_attr(u32)` — zero churn on the ~13 callers, no inversion.

The Opus holotype round (Fable out of credits) closed **0 P0 / 1 P1 / 1 P2 / 3
P3**, verifying the whole bounds/lifetime/W^X/charge/lockstep core sound. The P1
(F1) is worth recording: V-2 introduces the first cross-Proc-shareable
*PCI-BAR-backed* Burrow, and on the owning server's DEATH the unconditional
device quiesce clears the BAR's MEM decode under a client's live mapping. The
prosecutor refused to guess the terminal severity — an EL0 access to a quiesced
RAM-backed BAR is either benign garbage or a box-fatal external abort — and said
measure it, not reason it away. Surfaced as a design fork; the operator chose the
partial-quiesce fix: on death, for a claim with a live hostmem burrow, clear
BUS_MASTER (stop the dead device's DMA) but KEEP MEM_SPACE, deferring its clear
to the last unref — so the client never observes a decode-disabled BAR and the
measurement is moot. F2 (the handler's bounds had no test) was closed by
extracting a pure `hostmem_resolve_subrange` + testing it; F3/F4/F5 tracked P3.
Re-audit of the fixes: CLEAN (0 P0 / 0 P1 / 0 P2 / 3 P3 cosmetic; Opus 4.8 fallback -- Fable out of credits). Suite 1431/1431; commit 7973f8dc. Merge follow-ons (71306b60 + the libthyla/gate close): P3-1 landed the /proc/maps hostmem arm; the SMP gate PASSED (40 boots, 0 corruption across default+UBSan x smp4/smp8), the burrow/weft buggy cfgs FIRED and the clean cfgs stayed green, LS-CI console PASSED; the libthyla-rs ABI mirror (107) landed. The GL venus regression was DEFERRED, not failed: the thyla-pi LAN mDNS name stopped resolving mid-run -- a sync ssh wedged 36 minutes on its first mkdir, a bounded probe returned nodename-nor-servname, and the Cloudflare tunnel then proved the pi healthy (up 7 days, idle). venus is not in the push-bar and V-2 new code is unexercised until V-3, so the push proceeds; venus reruns when the LAN name resolves (or via the CF tunnel).

What V-2 does NOT ship: a real client. The weft delivery is exercised only by
unit tests — V-3 (vn_renderer) drives it E2E on real hardware, where the residual
P3s land with a driver to exercise them.

## 2026-08-19 — V-1: a guest blob creates, and the scope hidden in "blobs"

Resumed from my own self-compaction; the resume note ordered V-1 (blobs) next.
The ladder names V-1 "blobs (`RESOURCE_CREATE_BLOB` + the blob object model)",
which reads as a large chunk. Reading the design collapsed it to something
smaller and sharper.

The load-bearing fact is in GPU-DESIGN §2.4: **Venus's command ring is a guest
blob** — its head/tail/status cachelines are guest pages the host also reads.
That is why V-1 is Venus's real prerequisite. But "guest blob" is the whole
point: a guest blob's storage *is* its own guest `mem_entry` pages — the host
registers a resource referencing them, with no host allocation and no hostmem
BAR. The host3d blob (host-allocated storage the guest reaches through the
hostmem window via `MAP_BLOB`) is a *different* thing, and it is exactly the V-2
delta the reference already flagged (149-warp "Mapping a subrange is the §6.2
Venus-chunk delta"). So V-1 is the guest-blob *create* path — nothing maps,
nothing is coherent yet — and it rides the existing venus gate's two legs
unchanged: the venus device offers `F_RESOURCE_BLOB`, the plain `-gl` control
does not. The whole chunk is a tapestryd-side device command; no kernel path
(that arrives at V-2, which maps MMIO into a client VA).

Two wrong turns, both caught before they cost anything.

First, the opcode. I reached for `RESOURCE_CREATE_BLOB = 0x0212` from memory —
and it is wrong. Counting the virtio-gpu 2D enum forward from the code's own
anchor (`GET_CAPSET = 0x0109`, already in the tree) puts it at **0x010c**
(`GET_EDID` 0x010a and `RESOURCE_ASSIGN_UUID` 0x010b sit unused between). 0x0212
was a confabulation. The "a number recalled is a number unverified" rule earned
its place again — I verified against the tree's anchors, not memory.

Second, a lifetime bug in my own probe (self-audit SF1). `blob_probe` backs the
blob with a dedicated one-page DMA and unref's it, then the buffer Drops
(unmaps + frees the pages). If the *unref* fails while the engine is alive, the
host may still reference those pages — and Drop would unmap them out from under
a live reference. The probe issues no transfer so it is theoretical, but the
correct discipline is to **leak, not unmap, under a live reference**: one page
at init beats a UAF. `core::mem::forget(backing)` on the unref-fail path.

I also heeded a prior lesson rather than re-learning it: `init_device` returned
a positional `(u64, bool, bool)`, the exact shape that let V-0b's `ctxinit` go
briefly unreturned. Adding a third bool to a positional tuple is how that bug
happens again, so the three feature flags now ride a named `DevInit` struct.

The probe's resource id (`0x2b`) is collision-free by the same timing argument
the ctx-capset probe uses (it runs before the Server exists) plus a numeric
guard: the server mints ids from `SCREEN_RES + 1` upward and never down, so any
id `<= SCREEN_RES` is unmintable forever. I sabotaged the guard to prove it
fires — `id = 0x40` fails the build with the guard's message, `0x2b` compiles.

It creates. On thyla-pi (KVM, real V3D 4.2): `blob-create guest CREATED` with
venus, `blob-create skipped (F_RESOURCE_BLOB not offered)` on the control, and
the venus leg boots fully clean with the feature negotiated — so negotiating
blob does not disturb the compositor path (a self-audit worry, answered by the
boot). VENUS GATE VERIFIED, `test-venus-verdict` 13 → 16 arms, all discriminating
without a boot.

One measurement worth keeping for the next GL run: the control boot took **268s**,
not the ~220s the notes cite. A combined `warp-host.sh venus` run (both legs in
one call) would have been ~536s — close enough to the 600s foreground cap that a
slightly slower host would have moved it to a background task and killed the
second boot mid-run. Running each leg as its own sub-600s call was the right
call, and the number says why.

The prosecutor round closed **CLEAN (0 P0 / 0 P1 / 0 P2 / 3 P3)** on the Opus
4.8 fallback (Fable was out of credits — the round is a real degradation on the
independence axis, family-shared with the author, and it said so; a Fable re-run
is not owed because it finished). It caught one thing worth the round on its own:
**F1**, an inconsistency in my *own* SF1 fix. SF1 leaks the backing on a failed
unref (the host may still hold the pages); but the sibling branch — a create
that fails because the *engine died* — Dropped the backing, and a deadline-dead
create was already *published* (the doorbell rings before the wait), so the
device may equally hold that PA. Two branches, opposite dispositions, one
principle. Fixed to leak on both. Inert today (the probe issues no transfer, and
the dead path triggers a proc-death device reset), but it is exactly the kind of
disagreement that reuses the wrong disposition at V-3, where transfers exist. The
round also filed two forward notes: **F2** (V-3 must validate a client's
`pa`/`len` before they become a host `mem_entry` — an I-45/I-32 boundary) and
**F3** (when V-2 adds host3d, the gate should assert the blob mem-type from
evidence, not the hardcoded "guest" string).

The operational miss, recorded because the catch is the reusable part: partway
through the run the **host went to sleep**. It killed the prosecutor mid-response
("your computer went to sleep") and hung an LS-CI chunk into a 590s timeout doing
nothing — and I had forgotten `caffeinate`, the exact trap
`feedback_caffeinate_long_tasks.md` names. The tell was two failures at once with
one cause; the fix was a background `caffeinate -dis` plus `caffeinate -i` on
every LS-CI chunk, after which the heavies ran to 468s clean. The prosecutor's
partial output before it died was already a real finding (the missing runtime
guard on `resource_create_blob`), so the sleep cost time, not correctness.

A note on what "37/37 on the shipped binary" actually rests on: the guard and F1
are provably **unreachable** on the 2D device LS-CI boots (`blob_probe` is
virgl-gated, so `resource_create_blob` is never called there), so the 26
scenarios I ran before the fixes are byte-identical to the final binary, and I
re-ran only the remaining 11 on it. The venus gate I *did* re-boot on the final
binary directly — the test leg exercises the guard (which falls through, since
`self.blob == true`) — rather than lean on the same unreachability argument for
the load-bearing claim.

SMP stands (kernel byte-unchanged). Ahead: V-2 (host3d + the hostmem-BAR mapping,
the first real kernel memory-authority path of the arc) → V-3
(`vn_renderer_thylacine` + the coherent ring) → V-4/5/6.

## 2026-08-18 — V-0b: a Venus context creates, and the seam size I recalled wrong

I had classified V-0b as blocked this session — the arc's next step is
audit-bearing `gpu.rs` work and I'd been treating the Agent tool as barred. The
Stop hook pushed back: a checkpoint is not a stopping point, and the standing
operator grant (`feedback_prosecutor_agents_permitted.md`) authorizes the
`holotype-reviewer` for exactly this. So I opened it.

The question V-0b answers is narrow and real: V-0 proved the host *advertises*
capset id=4; it did not prove a Venus *context* can be created. That gap mattered
because `/usr/libexec/virgl_render_server` is in no Debian package, and §9.2
calls the render server Venus-only-by-construction — so "the capset is
advertised" could have meant venus init reached capset reporting and no further.

It creates. On thyla-pi (KVM, real V3D): `ctx-capset id=4 CREATED` with venus,
`skipped` without, `id=2` virgl the positive control on both legs. The absent
render server does not block it — virglrenderer's in-process venus init handles
context creation. That is the empirical answer the inference could not give.

The design point worth keeping: this is a **feature-bit** change, not a field
change, and the naive version is a *convincing* false pass. `ctx_create` wrote
`context_init = 0` under a comment saying the feature was not negotiated, and the
device ignores that field unless `F_CONTEXT_INIT` is negotiated — which the
driver never offered back. So "pass capset 4 and see" would have written into an
ignored field, collected `RESP_OK_NODATA`, and produced an implicitly-virgl
context reporting success. The negative control is what proves we avoid it: on a
no-venus boot the id=4 create is *skipped* because the capset was not enumerated,
never spuriously CREATED.

Then my own self-audit, run beside the prosecutor, caught me doing the exact
thing this whole run has been about. My commit message and code comment said the
probe's ctx ids (200/201) sit "above the client range (slot+1, <=128)". The
client range is not 1..128. `MAX_WARP_CTXS = 8` — one grep away — so it is 1..8.
The collision-safety conclusion holds (200/201 are far above 8 and below
`COMPOSITOR_CTX` at 0x100), but I cited a number I recalled instead of the one in
the tree, and the "128" is a real but *different* limit from Warp-3a. A number
recalled is a number unverified; the session's refrain, landing on me one more
time. Folded the correction into the round's disposition rather than amend under
a running reviewer.

Committed at `bf448929`, **not pushed** — `gpu.rs` is an audit-trigger surface
and this changes the device negotiation contract plus adds context creation, so
the round runs before the push. Fable was out of credits, so the round is on the
Opus fallback tier at max effort — context-independent even if same-family,
which is what the fallback rule preserves.

**The round closed CLEAN -- 0 P0 / 0 P1 / 1 P2 / 2 P3 -- and it converged with
the self-audit.** F1 (the "128-slot seam" that is really 8) was my SF1; F2 (the
debug_assert that vanishes in release) was my SF2. Two independent prosecutors,
the same two findings -- the reassurance the discipline is designed to produce.
The round added the part I had left as prose: F1 is not just a wrong comment, it
is a *missing compile-time guard*, because collision-freedom was argued from a
numeric window (liftable) instead of from timing (the probe runs before any
client and destroys before returning, which cannot be lifted). Fixed both ways:
the comment states the timing guarantee, and a `const _: () = assert!(...)` ties
the probe ids to `MAX_WARP_CTXS`/`COMPOSITOR_CTX` so a future seam lift past 199
fails the BUILD. Sabotaged it (probe id -> 5) to confirm it fires, then
reverted. F2 I closed early rather than deferring to V-3: the debug_assert
became a real `return Err` so a client-influenced capset in a release build
cannot silently mint a wrong-kind context. F3 was the round's own -- the gate
control leg asserted absence of "id=4 CREATED" without presence of "id=4
skipped", a negative a broken fixture satisfies -- now paired.

Honesty note the round pressed and I am keeping: it ran on **Opus 4.8**, a step
below the intended Opus-5 fallback (the `model: opus` override resolved low),
and it said so itself. A finished fallback round is closed per scripture, so no
re-run is owed -- but the tier is on the record, and the convergence with an
independent self-audit is what carries the confidence, not the tier alone.
---

## 2026-08-18 — An owed test, and the audit premise that was wrong when written

The extinction round (`5de6093f` F2) left an owed item: exec's failure
diagnostics were "compile-verified and never executed", because "no boot log
contains a single `exec:` line". I went to close it and found the premise was
half wrong — which is worth more than the test.

`exec_report_fail` was **already covered, and had been for seventeen days when
the round ran**. `test_execve_failed_load_leaves_target_drainable` (2026-08-01,
`e47bfa31`) drives a W+X-union failure and emits a real `exec:` line that sits in
the current suite boot log. The round's measurement — "no `exec:` line" — was
simply false when it was written. I know because I wrote it, and I did not
re-check it before turning it into an owed item.

`exec_say` was the actual gap: the dynamic-Linux-binary and dynamic-PT_INTERP
rejects had no test and appeared in no log. Genuinely never executed — the #244
class exactly, a diagnostic whose only witness was that it compiled.

Closing it was small: an ELF with a PT_INTERP naming a musl loader makes
`elf_load` return `HAS_INTERP` and `elf_brand_hint` answer `LINUX_LIKELY`, so
`exec_load_body`'s native arm runs `exec_say` and rejects the load. The suite
boot log now carries `exec: dynamic Linux binary rejected — ...` where before
there was nothing, which is the direct witness that `exec_say` runs without
faulting. Suite 1427 → 1428.

The reusable part is not the test. It is that **an audit finding's premise is a
claim about the tree, and it decays like any other.** This one asserted "never
executed" on top of a measurement that was already wrong, and the owed item
inherited the error. It is the same failure as the three throwaway verifiers
earlier in the run and the "currently broken" cross-reference before them: a
statement about what the tree does, trusted because someone once checked it,
that nobody's step re-checks. The whole session kept landing on one lesson from
different directions — a check is only worth the last time it actually ran.

---

## 2026-08-18 — The gate refused the host, and it was right to

V-0's remaining half was to stop *assuming* thyla-gl and boot it. Both halves
are now closed, and the interesting part is that the first attempt **failed**.

**The gate said UNVERIFIED, and the reason was real.** On thyla-gl's own Aug-12
artifacts, tapestryd **hung** under `venus=on,blob=on,hostmem=256M` — `warden:
tapestryd gave no readiness/exit signal -> terminating`, three restarts, `gave
up after 3 restart(s)` — while the control leg, same host, same build, came up
clean. A hang, not a crash: `Readiness::Timeout` means neither signalled nor
exited.

Two explanations suggested themselves, and both died by measurement rather than
by argument, which is the only reason I trust the third:

- *"the Aug-12 build predates #166's oversized-BAR skip."* Refuted in one
  command: `git show 534f3869:usr/lib/libthyla-rs/src/hardware.rs` carries the
  identical `if bar.size > PCI_BAR_VA_STRIDE { continue; }`, comment and all,
  and `git log -S` dates that code to 2026-06-15.
- *"lavapipe is slow to enumerate, so venus init stalls the control queue."*
  Weakened: `vulkaninfo --summary` returns in **248 ms** on that host, and
  `SUBMIT_DEADLINE_MS = 500` already bounds our controlq wait — so whatever hung,
  it was not our driver blocking forever on a device response.

Syncing the current build and re-running the same host with the same declaration
came up clean and VERIFIED. **So the attribution is the stale artifacts, not the
host** — but one sample each way across two different builds is not an
explanation, and I have written it down as unexplained rather than let "current
build works" quietly become "we know what that was." There is nothing to fix in
the tree, which is a different statement from knowing why.

The gate behaving correctly under a real failure is worth as much as the pass:
it refused to promote a host that could not show the capset, and it named the
reason.

**The driver was throwing away the answer to the arc's next question.**
`gpu.rs` reads `dev_feat_lo` during feature negotiation, uses exactly one bit of
it (VIRGL), and discards the rest. So "does this host offer `CONTEXT_INIT`?" —
the question that decides whether a Venus context is reachable at all — had no
answer short of writing a new build, about a value the driver already had in a
register. One `say!` line fixed that, and it immediately changed what V-0b *is*:

`CONTEXT_INIT` turns out to be offered on a **plain `-gl` device**, no venus and
no blob required. Meanwhile `ctx_create` writes `context_init = 0` under the
comment "F_CONTEXT_INIT not negotiated" — and the device honours that field only
when the feature is negotiated, which this driver never offers back. So the
obvious form of V-0b — pass capset 4 and see — would have written a 4 into a
field the device ignores, collected `RESP_OK_NODATA`, created an
implicitly-virgl context, and reported success. **A false pass, and a
particularly convincing one.** V-0b is a feature-bit change.

The same line settled V-1's host question for free: `RESOURCE_BLOB` appears only
with `blob=on`, and the default dev device offers neither (it is `virgl=0`), so
blob work cannot be exercised on the local dev loop at all. That is #166's
inert-hostmem-under-HVF constraint wearing different clothes, and it is the
concrete reason promoting thyla-gl was worth a morning.

**And a hole in my own gate, found by prosecuting it rather than admiring it.**
The gate asserted "the control leg does NOT see capset id=4". A control that
measured *nothing* — virgl not negotiated, 2D fallback, no capset lines at all —
satisfies that trivially, and the gate would read "venus absent" where the truth
is "capsets absent". That is the standing lesson about negative assertions and
broken fixtures, reappearing **inside the very gate I wrote to honour the
discrimination rule**: I had put the control in the *boots* and forgotten to
require that the control leg had measured anything. It now demands the baseline
pair (`id=1` and `id=2`), with two sabotages for it. 5/5 became 7/7.

Re-verified against the real thyla-pi logs from the passing run — still VERIFIED
under the strengthened verdict, so no re-boot was owed for that.

Both hosts, finally, return **byte-identical feature words** (`0x30000013`
without venus, `0x3000001b` with) — a cross-host agreement the arc did not need
but is better for having.

**Postscript, because repeating a pinned lesson is worth more written down than
quietly fixed.** Going into the pre-push bar I ran every TLA+ spec through a
one-liner that declared a spec green iff `tail -3` of its output contained
*"Model checking completed. No error has been found."* Every spec came back
FAIL. The specs were fine: TLC prints that line about twelve lines in and
finishes with state-graph statistics, so my verdict window could never contain
the string it was looking for. **A guard on the reporting path fabricating the
defect it reports — key on the exit code, never the prose** — is already an
M-PIN in this project's memory, and I wrote the same bug anyway, in a checker I
composed in one line because it felt too small to get wrong.

Two things follow. The pinned lesson does not fire from *reading* it; it fires
from noticing the shape "I am grepping prose for a verdict", and that shape is
easiest to miss in throwaway code. And the tell was available immediately:
*every* member of a large set failing at once is almost always the classifier,
not the set — which is itself the other half of a pinned lesson ("when ONE
member of a family misbehaves, suspect the classifier"; here it was all of them,
which is even louder). Confirmed in one command: exit code 0, success line at
line 12.

The run was not owed in the first place — clean-cfg TLC has been suspended since
2026-05-21, and a `say!` line in a virtio driver touches no modelled mechanism —
so the whole excursion cost ten minutes to learn something about my own reflexes
rather than about the specs.

**And then it happened twice more in the same session, which is the actual
finding.** (2) A shell loop meant to re-verify three real log pairs under a
changed predicate reported all three FAILING with an empty verdict string; run
directly, every one passed — the loop's `$?` was not measuring what I thought.
(3) A one-liner checking that my new documentation tables were not broken
flagged the GPU-DESIGN row as suspect, because I had hard-coded the pipe count
of a *four*-column table onto a *three*-column one; every sibling row had the
same count, so the doc was fine and the checker was not.

Three throwaway verifiers in one session, three false alarms, zero real defects
among them. Each was caught the same way — by checking the surprising result
against a known-good reference before acting on it — and none cost more than
minutes. But the shape is worth naming, because the pinned lessons are all about
distrusting *gates I build deliberately*, and every one of these was a scrap of
shell I wrote in passing to confirm something I already believed. **The care I
give a committed checker does not automatically extend to the one-liner that
checks it**, and the one-liner is the one nothing else will ever review.

The practical rule that fell out: when an ad-hoc check reports that *everything*
failed, or that something I just verified by hand is broken, the first suspect
is the check. That is the same instinct as the pinned "when one member of a
family misbehaves, suspect the classifier" — it just has to fire for code that
never gets committed.

---

## 2026-08-18 — Warp-6 opens on a probe, and the blocker that wasn't

Warp-C closed, so Warp-6 (Venus) is next. `GPU-DESIGN.md` §9.1 makes the first
move non-negotiable: *"Nothing can be **run** locally. This must be settled
before code starts, not discovered after."* So the arc opens with a gating
probe, the Warp-C C-0 shape, and `vn_renderer_thylacine` waits.

**The measurement, with its control.** Two boots on thyla-pi differing in the
device declaration alone. Control (`virtio-gpu-gl-pci`): capsets `id=1`, `id=2`.
Test (`+venus=on,blob=on,hostmem=256M`): additionally **`id=4` — VENUS,
`max_version=0`, `max_size=160`**. Both legs `BOOT: PASS` (215–225 s under KVM),
which is the part that makes it evidence: had the control merely failed to boot,
the missing capset would have been attributable to that instead of to the
declaration.

**No guest change was needed, and I nearly bought a boot to learn that.**
`probe_capsets` (`usr/tapestryd/src/gpu.rs`) already enumerates to
`GPU_CAPSET_ENUM_MAX = 8` and prints one `gpu capset[N] id=..` line per index.
My first grep filtered them out — the pattern was `GET_CAPSET`, and the lines
say `gpu capset[`. The evidence was on disk in the logs I had already produced.
A pattern that matches the wrong thing returns a confident partial answer, not
an error; the tell was that a boot which *did* enumerate three capsets reported
nothing about what the third one was.

**QEMU documented its own requirement better than I would have.** `venus=on`
alone is refused, and so is `venus=on,blob=on`, both with
`venus requires enabled blob and hostmem options`. Only the triple realises.
That is a **realise failure, not a degradation** — a caller declaring less does
not get "GL without Venus", it gets no device, and must not read that as a
negative Venus result. It also settles V-2's position in the ladder by
measurement rather than judgement: hostmem cannot be a late refinement of a
chunk whose device will not come up without it.

**The blocker that wasn't, and why it is written down anyway.** The host's
`libvirglrenderer.so.1.9.0` carries Venus (`VK_MESA_venus_protocol`,
`vkr_ring_thread`, `vkr_dispatch_vkWaitVirtqueueSeqnoMESA`) and names
`/usr/libexec/virgl_render_server` as `RENDER_SERVER_EXEC_PATH` — **and Debian
ships that binary in no package**; `virgl-server` is the unrelated *vtest*
server. §9.2 calls the render server Venus-only-by-construction, which reads as
"no server, no Venus", and for about ten minutes I had a dead arc. The capset is
advertised regardless, so venus initialises in-process at least far enough to
answer a capset query.

The discipline point is what I did **not** then write. "Venus works on
thyla-pi" is not what was measured. What was measured is that venus init reaches
capset reporting; whether a *context* creates is a different claim, and the
render server could still bite there. That became V-0b (`CTX_CREATE` with
`capset_id=4`) — a rung that settles it empirically instead of by inference in
either direction.

Instrument note worth keeping: `nm -D --defined-only` finds **zero** venus
symbols in that library, because they are internal. Had I run the export census
first and stopped there, I would have concluded Venus was absent from a library
that plainly contains it.

**The measurement was then made into a gate, because a hand-run measurement is
not one.** `warp-host.sh venus` runs both legs and asserts the discrimination in
**both directions** — present with the declaration, absent without. One
direction is not enough: "the test leg saw `id=4`" is satisfied by a host that
advertises the capset unconditionally, and by a guest printing a line it never
derived from the device.

Then the gate's own problem: it costs two ~220 s remote boots, which makes its
verdict the least affordable thing in the tree to test by running it — and #245
is three days old and says exactly what happens to a checker reachable only by
hand. So the verdict is its own verb (`venus-verdict`), and
`tools/test-venus-verdict.sh` drives **the real implementation** against crafted
logs: five cases, four one-variable sabotages plus the clean pair. The clean
case is not decoration — without it, four negative cases are satisfied by a
verdict that always fails. `5/5, DISCRIMINATES`, wired to `make
test-venus-verdict` and into CLAUDE.md's command block, which #245 measured to be
the property that actually prevents rot.

**Open, and named as open.** thyla-gl (Parallels, lavapipe) has the same QEMU
10.0.11 and a venus-carrying virglrenderer but has **never booted with
`venus=on`** — it is checked to the property level only, and promoting it is
V-0's remaining half. It matters beyond tidiness: if it works, Venus has a fast
local-ish iteration loop; if not, the whole arc iterates over the Pi's SD card.

The V-0..V-6 ladder is now in GPU-DESIGN §12, and V-2 is flagged audit-bearing
on I-45 and I-32 *independently of the rest of the arc*, because mapping MMIO
pages into a client VA is a new kernel memory-authority path and not a graphics
detail.

**And then the wrong turn, caught about twenty minutes after it landed.** I
wrote V-2 as carrying "the `PciDev::claim` eager-map-every-BAR fix, pulled
forward as a dependency" — because §6.2 ends with *"Also required and currently
broken: `PciDev::claim`'s eager map-every-BAR policy (§3)."* It is not broken.
It was fixed at **Warp-2a (#166)**, and §3 — **the section §6.2 points at** —
has said `[FIXED at Warp-2a (#166)]` in bold for weeks, along with the exact
remaining delta: *"Mapping a subrange of the shm window remains the §6.2
Venus-chunk delta."*

What caught it was not re-reading the doc. It was going to look at the tree for
an unrelated reason — how big is V-2, really? — and finding
`kernel/pci_handle.c` already resolving `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG`, a
`pci.walk_caps_shm` test passing in the boot log I already had open, and
`hardware.rs` carrying a `#166` comment at the exact line that skips an
oversized BAR.

Two things worth keeping. First, **a cross-reference pointing AT the correction
is not the same as being corrected** — §6.2 pointed straight at the section that
refuted it, and the pointer kept its own verdict; a reader who follows the
pointer has already believed the pointer. Second, **a "currently broken" note in
a design doc is a claim about the tree, and it ages exactly like a status field:
nobody's step flips it.** The fix's own commit updated §3 and did not think to
hunt the other half of the sentence one section away.

So V-2 is **smaller than I wrote it**: discovery is done, the claim policy is
fixed, and what remains is the mapping half alone — an owner-minted,
client-mappable, revocable, budgeted map of a *subrange* of the shm window at
the host-dictated cache attribute. Corrected in §6.2, §12, and the status row;
the original claim is left visible in §6.2 rather than quietly overwritten, so
the next reader can see which half of a self-contradicting document was stale.

---

## 2026-08-18 — A reroute from a blocking primitive to a dropping one, and the budget I left behind

The audit `extinction.c` owed — it is a declared trigger surface and #246 put a
fault-injection hook on it — came back **0 P0 / 1 P1 / 2 P2 / 4 P3**. Clean by
the numeric rule. F1 was mine and had to land before merge.

The round opened by naming its own degradation rather than reciting a caveat:
the code was Opus-authored and so was the reviewer, so **family diversity is
forfeit here** and only context independence survives. It then used that
independence properly — it re-derived the EL1-sync depth ladder, measured the
shell predicate against twelve adversarial inputs, and **withdrew two of its
own prosecutions** against the code.

### F1: I moved the diagnostic and left the accounting where it was

`uart_puts` spins per byte and always emits. `cons_diag_line_emit` is
**all-or-nothing** and drops silently. I swapped the first for the second and
left the dedupe bit and the report budget being consumed *before* the emit.

Under back-pressure from a guest writing `/dev/cons` — the room-wait wakes on
**one** free byte and immediately refills, so the 8192-byte ring sits at
capacity — a 107-byte all-or-nothing unit never fits. So the drop is not racy,
it is **deterministic**, and it is the regime a container bring-up produces.
The syscall number is then marked seen forever and the budget is one lower. The
census under-reports and still reads as a measurement.

That is verbatim the failure the function's own header says the per-Proc rework
existed to kill: *"worse than no diagnostic, because it reads as a
measurement."* I re-opened it one step down, by changing the primitive and not
re-examining what was spent around it. **A reroute from a blocking primitive to
a dropping one changes the failure mode of every budget spent around it.**

The emit now reports whether the unit landed; the bit and the cap are taken
only when it does, so a dropped line is retried on the next decline.

### F2: I fixed the bounded emitter and left the unbounded one

The commit's own headline was "route the EL0-triggerable diagnostic through the
ring", singular. `exec_report_fail` is five raw calls, twice per failed spawn,
with **no dedupe and no cap**, and every `SYS_SPAWN_*` reaches exec through it —
so an unprivileged Proc spawning a malformed ELF in a loop drives it at will.
Strictly worse than the site I closed, and the severity ordering was inverted
relative to the fix that landed.

Converted, with a **global** cap rather than per-Proc: a per-Proc bound is
re-armed by spawning, which is the attack. The old comment defending the raw
loop ("to stay non-blocking") no longer selects it — `cons_diag_line` is also
non-blocking, never spins, and takes no console role — so that sentence went
too.

### F3: I wrote the lesson and then didn't apply it

My commit said *"a set with four independent spellings has no spelling anything
can be checked against."* I then enumerated only the file I was already
editing. Six more spellings were stale: two in `CMakeLists.txt` (a cache
docstring at four-of-eight, a comment block reading as complete at
three-of-eight), two in a **binding** reference doc at three-of-eight, and a
Makefile help line saying "seven" and "7 boots" against eight — in a line I had
just tagged `#245`. All now point at `ALL_VARIANTS` instead of re-duplicating,
because duplication is the thing that rotted.

### F6: the arm my test reached but could not fail on

I claimed the hook's placement put `cons_tx_claim_for_dump`'s
already-owned-by-this-cpu arm under test. It *reaches* it. Delete that arm and
the re-entrant claim burns its bound, returns false — and the banner still
prints, because the miss path is "torn beats silent". The expected string is
present and the variant passes, twenty milliseconds slower. Detection, not
discrimination.

Closed with a `forbid_for` table asserting the log must **not** contain
`console-ring: NOT held`, wired into the PASS arm rather than merely defined.
The round was also exact about what my sabotage proved: sensitivity to *"the
claim primitive does not dereference TPIDR_EL1"* — not to *"the ring lock is
actually held"* or *"the bound is honoured"*.

### Measuring the block instead of asserting it

I twice reported myself blocked on hardware with twelve files edited and none
compiled. The third time I checked: `ps` showed **37% of 800%** and nothing of
the lease-holder's on the cores — their concurrent work was a prosecutor round,
which is network-bound. The standing rule permits exactly this case (a check
while a peer holds the lease, when nothing of theirs is running, *checked with
ps and announced by note*). One kernel-only compile, seconds: **clean**, the
`void`→`bool` signature change harmless to its five callers, the sole warning
pre-existing in a file I never touched.

I was blocked on a *lease*, not on *cores*, and had not distinguished them. The
peer turned out to be genuinely mid-build a few minutes later, so the window
was real and narrow — which is why the rule says to measure at the moment
rather than reason from the lease. Boots still wait for the lease; I said "no
boots" in the note and that holds.

## 2026-08-18 — The round found the inverse defect: my fix for an over-permissive gate had landed as an over-restrictive one

The follow-up round the dirty C-6b close owed came back **0 P0 / 1 P1 / 1 P2 /
3 P3** — clean on both triggers. `MODEL(start) == MODEL(end)`, Opus fallback,
no mid-run drop. Worth saying which way the diversity caveat pointed, because
it **flipped**: the previous round audited Fable-authored code, so Opus was
genuinely cross-lineage; these fixes are Opus-authored, so this round was
same-family and its entire contribution was context independence. The spawn
said so and named the reflex to fight. The round named it back:

> I would have written that brace too, keyed on the same format, thinking
> about compressed textures and not about a driver that declares one byte on
> purpose.

### F1: the guard that refused what it had to admit

The P0 I closed last chunk was real — a 512×512 BO declaring 4096 bytes made
the compositor read 1 MiB out of a 4 KiB mapping. I fixed it in two places:
an exact bound at the **read** gate, and a "belt" brace at the **create**
door keyed on B8G8R8A8.

The brace refuses ordinary Mesa resources, and the proof was already in this
repo — in a comment, written by this project, at the exact line that chooses
the size (`usr/ports/mesa/patches/0006-*.patch:1511`):

> The seam refuses unaligned or zero backings; the driver's staging-path
> textures legitimately ask for size 1.

Mesa's virgl driver declares one byte on two paths that keep the real
width/height — the staging path (`alloc_size = 1`) and MSAA (*"don't create
guest backing store for MSAA"* → `total_size = 0`) — and our winsys rounds
that to one page. So `create3d … 512 512 … 4096` is **byte-for-byte both the
attack shape and a perfectly ordinary staged or multisampled BGRA texture**.
There is nothing to tell apart. Only the reader can distinguish them, by
whether it is about to read the backing — which is exactly what the read gate
does, and why it was the load-bearing half all along.

**The part worth carrying is why every gate stayed green.** The staging arm
hangs on a virglrenderer capset bit that *nothing in this tree measures*, and
thyla-pi's 1.1.0 evidently does not set it. The MSAA arm needed no host bit at
all: every multisampled BGRA render target above 32×32 was refused outright,
and no gate we have would notice, because a gate proves what the system *does*
and an over-refusal shows up only as something a client can no longer do.
**A guard whose activation no gate can see is worse than the hole it closes.**

And the prover leg I'd added to guard the P0 was asserting that a legitimate
allocation must fail. It is re-targeted as `C0-STAGING`: the door must *admit*
the one-page shape, with an unaligned backing as the control so "admitted"
cannot pass against a door that admits everything. The read gate's own runtime
regression test is **owed and tracked**, not quietly dropped.

**My parallel self-audit did not find this**, and the reason generalizes: I
prosecuted seven fixes and asked of each "is this gate sound?" — never "does
this gate refuse what it must admit?" Only the second question reaches a
client the tree does not contain. The round confirmed all seven of my
soundness findings and then found the one I had no question for.

### Rejecting the round's suggested fix (F4)

The DEEP arm's bar was stated three different ways and the code matched none.
The round proposed asserting the round's **max** via a census delta. I
re-derived it and **rejected it**: `Cost.max_ns` is a *global running maximum
that is never reset*, so a per-round max is not derivable — after round one a
delta detects only a new global record. But `mean ≥ T` does entail `max ≥ T`,
so the code was already a sound lower-bound witness and only the *prose*
overstated it. Fixed as prose, reconciled across three documents.

That it mattered showed up on silicon an hour later: round 3 measured a mean
of 128 ms over 2 retires, so the old "every compositor readback waited ≥ 100
ms" would have been false on that round.

### The deterministic failure that was my own fixture

`decomp gl` then failed twice, deterministically, at
`rp6 never confirmed the /env write (60s)`. I had just changed the compositor,
so it read as my regression.

It was my **pool**. `tools/test-fault.sh` re-bakes `pool.img` with `CLADE=0`
on every variant — and I had just run it ten times — so `/clade` was gone, and
`glq-decomp.exp` builds its `rp6` wrapper on-device with `/clade/bin/clang`.
The scenario's `echo rp6-ready` runs *whether or not clang succeeded*, so the
harness reported "rp6 built" and then failed 60 s later naming `/env`, a
subsystem with nothing to do with the cause. **A step that confirms the next
command instead of the one under test will always misattribute the failure.**

My own failure inside that: I verified the **ramfs** by content before syncing,
exactly as the discipline demands, and did not verify the **pool**. Verifying
one paired artifact by content and trusting the other is not verifying by
content. The build's output had said so plainly — `bake config CLADE=0`,
`payloads verified PRESENT: GOROOT GOCACHE GO4C QUAKE`, no CLADE — and I read
past it. A one-command check settles it: 917M with clade, 449M without.

Also recorded because it cost real context: **do not `grep` the pool image.**
It is an encrypted Stratum image; grepping it dumped megabytes of binary into
the transcript and told me nothing.

Re-baked both paired artifacts with `PRESERVE=0`, re-synced, and the same code
passed: **GLQ-DECOMP PASS gl**, 969 frames at 37.9 fps composed on real V3D.
Same code, different fixture — the attribution is settled, not assumed.
`test-fault.sh` mutating a shared fixture other gates depend on is filed
(main#250); it should restore the operator's bake config or refuse, the same
shape as `test-interactive.sh` refusing when a VM is already running.

### #243 and #246, from the extinction work

`uart_puts` takes no lock, so the ring claim serializes against ring traffic
only. The class was **observed live and fixed once already** — #76 removed the
same raw loop from `SYS_PUTS` after it shredded a login prompt byte-for-byte —
and `viv_report_unserved` reached for it again, on a path an unprivileged EL0
program triggers by choosing an unserved syscall. Now one `cons_diag_line`
unit; verified live in the boot log.

`el1_sync_runaway` had no test and `7dd5be19` had just put three calls on it.
Confirmed by reading why: the depth ladder tops at 3, the #806 guard extincts
at 2, so only a fault from *inside* the extinction path reaches it — #244's
shape, on purpose. **Discrimination proven** by sabotaging the claim back to
the counted trylock and watching the variant fail. Stated exactly: that
sabotage does *not* reproduce #244's silent park — the counted trylock trips
`lock-across-sleep` first — so what it proves is sensitivity to the claim
path's correctness, not reproduction of the original bug.

And `test-fault.sh` enumerated its variant set **four times**; adding one
updated two of them, so `test-fault.sh el1_sync_runaway` answered "Unknown
arg" while `make test-fault` ran it happily. The arg arm and `--help` now
derive from the one list.

## 2026-08-18 — Two gates nobody ran, and the count that refuted my first explanation

Spawned the follow-up prosecutor round the dirty C-6b close owed (`c8c83348` +
`2f3c0bcc` — a P0 returned and P1+P2 hit six, so CLAUDE.md's re-audit rule
fires). Fable is out of credits, so it went straight to the Opus fallback per
scripture. **Worth stating which way the diversity caveat points this time,
because it flipped**: the previous round audited Fable-authored code, so Opus
was genuinely cross-lineage; these fixes are Opus-authored, so this round is
*same*-family and its whole contribution is context independence. The spawn
says so explicitly and tells the prosecutor which reflex to fight — agreeing
with a construction because it is the one it would also have written.

While it ran, the audit-in-flight discipline: non-colliding work, then
prosecute the same surface myself.

### The non-colliding work turned out to be the more interesting half

main#245 said `test-fault.sh` is wired into no gate. A census over `Makefile` +
`tools/`, with a control at each end (`ci-smp-gate.sh` must resolve to a target,
`test-fault.sh` must not), found **two** orphans rather than one:
`tools/verify-kaslr.sh` has no caller either. The only references to either are
two *comments* in sibling scripts.

Neither is decorative. `test-fault.sh` is the only witness that the seven
hardening protections actually **fire** rather than merely being compiled in —
the canary, kernel-image W^X, BTI, the two stack guards, the boot-CPU idle
guard, the recursion arm. `verify-kaslr.sh` is I-16's only runtime witness:
ROADMAP §4.2 requires the kernel base to differ across boots, and `make test`
accepts any *single* boot, so it is structurally blind to a slide that never
moves. This is how #244 hid for a month.

**Then the interesting part: my first explanation was wrong, and its own
measurement said so.** The obvious hypothesis is that the survivors are in
CLAUDE.md and the orphans are not — CLAUDE.md is auto-loaded every session, so
that would be a clean anti-rot story. The count refutes it: `test-fault` and
`verify-kaslr` appear in CLAUDE.md **twice each**, exactly like `test-a72` and
`check-v80-floor`, which did not rot.

The difference is *where*. The survivors sit in the "Build + test commands"
block, as commands. The orphans appear only in the boot-banner paragraph's
prose, named as **consumers of the ABI literals** — things that would *break*
if you reworded one, never things to run. Every session learned they existed
and nothing about invoking them. Which is precisely the mention-versus-program
distinction that same paragraph teaches about its own co-update list, applied
to itself and not noticed.

So the remedy is both halves, in the idiom this project already uses for the
class (`check-production`/#228, `test-a72` and `check-floor`/#91): a named
target with a WHY comment, **plus** an entry in the command block. `55c5d2f8`.

**A second wrong turn, caught after the commit.** The census as first run also
grepped `.github` — which does not exist. There is no CI in this repo at all,
so that arm searched nothing and contributed no evidence, while the commit
message reports "no Makefile target, no gate, no CI step" in a list that reads
as three findings. The claim is true; one third of it is *vacuous*. An empty
arm of a census must not be reported as though it were a negative result, and
the tell is that the arm was never given a control the way the other two were.

**A wrong turn caught before it shipped.** The first draft of the help text put
backticks around `make test` inside a Makefile `@echo "..."`. Backticks inside
double quotes command-substitute — `make help` would have *run the full test
suite*. Caught by rendering the target rather than trusting the diff.

**What this does not close, stated rather than glossed:** neither script now
runs *automatically*. They are named targets a human or agent invokes, exactly
like `test-a72`. Whether test-fault joins the pre-push bar costs 7 builds + 7
boots, and the gating evaluation is the operator's call, so it is surfaced.

### The vault gains a fourth failure class

`quaestor owner` routed the change to `abi-boot-banner`, whose taxonomy
enumerates three ways a co-update list member fails — *phantom* (named, never
existed), *inert* (exists, matches nothing), *document* (matches, only goes
stale) — against an implied healthy fourth, the **program** that "breaks
silently and immediately".

Two of its fifteen derived mirrors were programs nothing ran. That class has a
program's full co-update obligation and **no failure behaviour at all**: it does
not break loudly, and unlike a document it never even becomes visibly wrong,
because nothing evaluates the mismatch. Strictly worse than the document class.

The mirror rule itself is unaffected — it answers "who must be co-updated", and
an unrun program must still be co-updated. What the note now guards against is
reading a fifteen-member derived set as *defence in depth*. **A mirror set
bounds the co-update obligation; it says nothing about detection latency, and
only the members something actually runs contribute to detection at all.** Same
shape as the extinction seam one level up: a contract on a value is silent about
its delivery; a contract on the set of readers is silent about whether any of
them reads. Vault `60095c97`, lint 946/0/0.

### Self-audit: seven fixes prosecuted, seven sound, one suspicion withdrawn

Re-derived from the code rather than from each fix's own comment. The P0 repair
is covered better than its comment claims: the pre-existing `b.w == s.w` check
sits before the new size guard on the same path, so the guard's geometry *is*
the reader's; and `comp_readback_retired` re-runs `gl_adoption` as
`same_adoption` at retire, so the guard re-validates at **read** time and the
issue→retire TOCTOU is closed by construction. The "sole `Some(va)` caller"
claim was re-derived, not accepted: exactly two call sites, one `Some`, one
`None`, and the Warp-4 synchronous arm that originated the P0 no longer exists.

`FenceTag.ok` has one construction site, fail-closed at `false`, and two
textually identical assignments. `FenceVindication.comp` takes its
discriminator and its ctx from the same loop index at both sites, so they cannot
disagree. The `COMP_FSLOT` exemption is conditional on scope and correct in
*both* directions — the client-driven scoped lever cannot touch the reserved
slot, the internal unscoped callers still can, because a wedge that is real is
genuinely global.

**One suspicion raised and withdrawn by measurement**: `rb_coalesced` looked
mis-charged (the `+= 1` sits outside the match, so both arms reach it) — the F9
class again. Two checks killed it: `git show 24e6753d` proves the unconditional
increment is pre-existing and untouched by my fix, and `149-warp.md` defines the
key as "presents that enqueued instead of issuing", which is exactly what
`rb_enqueue`'s two callers are. Recorded as withdrawn rather than dropped
silently, because a fabricated defect eats the budget a real one needs.

Findings in `memory/audit_c6b_followup_selfaudit.md`, to be **merged** with the
round's report when it lands, not segregated from it.

## 2026-08-18 — The owed C-6b round: a deviation is dangerous everywhere else that reads the same field

Fable ran out of credits mid-spawn — the prosecutor died after loading the
preamble and before producing findings, which is an **absent** round, not a
clean one. Per CLAUDE.md that goes straight to the fallback tier rather than
retrying Fable, so it ran on Opus 5.

**The family-diversity caveat is INVERTED here, and reciting it would have been
wrong.** The standing rule assumes an Opus prosecutor shares the author's
priors because Opus is this project's implementation agent. But `ef58d639` and
`24e6753d` were written by **Fable 5** earlier the same session — so an Opus
prosecutor is genuinely cross-lineage against *this* author. I said so in the
spawn, told it its contribution was context independence, and warned it that
the code's own justifications (dense comments, the AS-BUILT paragraphs, the
audit row's prosecute-on-change list, five closed lists of "VERIFIED SOUND"
arms) are the author's argument and not evidence. It came back with **1 P0 /
3 P1 / 3 P2 / 3 P3**, and three of the findings are corrections to claims the
tree makes about itself.

### The lesson, and it is specifically about AS-BUILT 1

C-6b deviated from the design's letter in one recorded place: the compositor
readback's fence tag carries the **client's** `ctx_pub` rather than 0. That was
argued carefully and it is right — 0 is `warp_ctx_vindicate`'s no-slot
sentinel, and the client's own vindication has to wait for our poisoned slot.

What was never enumerated is the deviation's **cost**. Every mechanism keyed on
a tag's ctx now reaches the compositor's reserved slot, and two of those are
*shipped, client-drivable levers* (`warp-hold` / `warp-abandon`, since
`default = ["test-mode"]` and nothing passes `--no-default-features`). Their
safety argument is #178's: "the worst a client can do is wedge its own ctx,
which it could already do." C-6b made that false one resource over, silently,
and the round found it (F4) by prosecuting the documented deviation **as a
design change rather than as a footnote**. Worse, `drain` cleared
`fslot_since` one line *before* the hold check, so a held slot could never
reach `reap_abandoned`'s staleness test — the pin was indefinite, not bounded
by 30 s. Compositor-wide: every other client's readback frozen, the 500 ms sync
deadline disabled process-wide, and a ~1 kHz spin in the console for the life
of the box.

**A deviation is sound for the reason it was taken and dangerous everywhere
else that reads the same field.**

### The P0 was pre-existing, and its guard was a comment about the wrong subject

F1: `wbo_create` validated the client-declared backing with two gates and
**both are upper bounds** — its comment states the one-directionality outright
("a 1x1 texture cannot ask for 64 MiB"). `gl_adoption` compared `w`/`h` for
*equality*, never capacity. And `compose_cpu` reads `sw * sh_full * 4` from the
BO's `va` with the dims taken from the **surface**. So a 512×512 BO declared
with 4096 bytes — page-aligned, under both caps, `Y_0_TOP` so it takes the
readback arm — was admitted, adopted, and composed by reading **1 MiB out of a
4 KiB mapping**: a bump-allocated neighbour (another client's pixels, painted
onto the attacker's own pane) or a fault in the process that *is* the console.

`compose_cpu` carries a `SAFETY` comment asserting the rows are in range
"because damage was validated against the surface geometry". True of the
**weave**, whose size derives from that geometry. False of a client-declared BO
backing. The same function reads both.

Pre-existing from the Warp-4 synchronous arm and in none of the five
preambles — attribution, not ownership. Fixed at the read gate (exact:
`b.size >= b.w * b.h * 4`, exact because adoption already pins the dims, and
`comp_readback_retired` is the only `Some(va)` caller — enumerated by enclosing
function, not by grep hit) and at the door (keyed on `B8G8R8A8_UNORM` alone: a
general per-texel floor would refuse legitimate *compressed* textures, and it
must not key on `composable` because the attack shape is precisely
non-composable — that is how it reaches the readback arm).

### Converging with my own pass, and the one I sharpened afterwards

I ran the self-audit in parallel per the audit-in-flight discipline and found
F3 independently (a vindicated compositor readback bumps the **client's**
`fence_signaled`, so `warp_fence_wait` — which returns on `signaled >= seq` —
returns one fence early for the ctx's life). Filing it before the round
reported is the useful part: two prosecutors reaching the same defect from
different directions is the strongest signal either one produces.

The round also sharpened something I had noticed and under-read: `rb_wanted`'s
growth. I saw it was unbounded in principle; the round pinned *why the comment
was wrong* — the dedup key included `gen`, drawn from a monotonic counter, so
"bounded by MAX_SURFACES" bounded `n` and not the pair.

### The fix that broke the gate, and what that is worth

My fix to F8 (DEEP asserted a **sum** over an unknown retire count against a
per-readback threshold) required *exactly one* retire per round. The gate went
**red on a healthy build**: `comp-rb landed 1->7` across three rounds — **two**
retires each, because the flight loop's later presents each request a readback
and the pump issues the next the moment the first lands.

Every round satisfied the substance (waits 794 / 1007 / 260 ms, each observing
draw 1199 of 1200 by its pixel witness) and failed my arithmetic. **I had
replaced a wrong statistic with a claim about the mechanism's scheduling**, and
the claim was false. The round had offered the right alternative in the same
breath and I took the wrong half of it. Now it asserts the round's **mean**:
robust to any retire count, still rejects the case the sum admitted (one long
readback plus one instant one averages below threshold), and the pixel witness
still carries which draw was observed. The per-round line prints the count and
the mean so the next red is diagnosable without a re-run.

Worth recording plainly: the gate caught my own fix, on real silicon, one
commit after I wrote it. That is the system working — and it is the second time
this run that a control earned its keep by going red for a reason that was not
a defect.

### What is NOT closed

F7 [P2] is a **measurement debt**, not a code change, and saying otherwise
would be the worse outcome. The readback gate cannot *discriminate* a sabotage
that removes the deadline widening: the certifying run measured `F2B max
267 ms` against a `SUBMIT_DEADLINE_MS` of 500, so a build without the widening
passes identically. Sharper still — the deadline is evaluated **only at a stale
wake**, and the stall it exists for (a synchronous host
`TRANSFER_FROM_HOST_3D` on QEMU's serial main loop) raises no interrupts. So
whether the widening is load-bearing *at all* depends on INTx sharing nobody
has measured. GPU-DESIGN 4.5.13 now says that instead of "correct by
construction", and names what closes it. Tracked as main#253.

The close is **dirty** (a P0 returned; P1+P2 = 6) and several fixes are
structurally invasive, so **a follow-up round is owed on the fixes themselves**.

---

## 2026-08-18 — The extinction line, source 2 of 3: the fix found a fault gate that had been printing nothing for a month

Same run, after C-6b landed and pushed at `f525cea3`. Next on the resume note
was the follow-up Fable round on the C-0d fixes + C-6b; it was spawned first
(read-only, no cores), and this chunk ran alongside it.

### What was owed

The `EXTINCTION:` ABI line has **three** tearing sources and the names are
close enough that I have conflated them before. Source 1 —
extinction-vs-extinction — was closed 2026-08-16 by `extinction_claim_console`
(one `__atomic_exchange_n`; losers park silent). Source 2 —
**extinction vs a peer's ordinary console write** — is the vault's
`seam-extinction-line-unserialized`, and it is the one that matters most by
readership: the seam's own census found **fourteen of fifteen** declared
mirrors match the crash prefix, against eight for the boot-success line that
got the guarantee. Source 3 is `IPI_HALT`, still a commented-out reservation.

### The prescribed remedy was a hypothesis, and it was wrong in one specific

The seam prescribed a **try**-acquire of the *writer role* (never a park).
Checking it against the drain path says no: the role (`g_cons_tx.writing`)
serializes whole `cons_output_write` calls, but **the drain never consults the
role** — that is main#144, already written down in `cons.h` — so bytes a peer
had already pushed would still pop into the FIFO from cpu0's TX IRQ or from a
peer's `cons_tx_kick`, landing inside the banner while the role sat held.

What actually owns the wire is **the ring lock**: every steady-state producer
pushes its unit under `g_cons_tx.lock` (`cons_tx_push_bulk` — SYS_PUTS through
the role, the echo, `cons_diag_line`) and every ring→FIFO drain pops under the
same lock. So the winner takes *that*, and never lets go
(`cons_tx_claim_for_dump`, `kernel/cons.c`). The role is also the wrong
primitive on a second axis: a healthy peer holds the ring lock for one bounded
push or one FIFO-depth drain — microseconds — where the role is held across a
whole write, room-waits included.

Every property is deliberately the **opposite** of the console word one file
over, and the reason is the same in each case — *who holds the thing you are
waiting for*:

| | console word (source 1) | ring lock (source 2) |
|---|---|---|
| holder you contend with | a **dying** peer that never releases | a **healthy** peer that will release in µs |
| therefore | **try once**, never spin | **bounded spin**, because try-once fails exactly when it matters |
| primitive | raw atomic (a spinlock could fault on a dying machine) | **raw** trylock, same reason — new `spin_trylock_raw` |
| on failure | park silent (a missing line is visible; a torn one reads as a clean boot) | emit anyway, and **report the miss** after the dump |

IRQs are masked before the acquire and never restored: with the ring lock held
on this CPU, its own TX IRQ arm (`cons_tx_drain_from_irq` → `spin_lock_irqsave`)
would self-deadlock — a silent hang in place of the dump. The caller parks in
`_torpor`, so nothing is owed back. And the flush under the lock became the
*full* bounded ring rather than one FIFO's worth, because holding forever means
whatever is still queued when the flush stops is lost, where the predecessor's
release let the rest trickle out behind the dump.

### The compile found the emitter the census had missed

`cons_tx_flush_for_dump` had a second caller: `arch/arm64/exception.c::
el1_sync_runaway`, the #214 recursion guard's terminal banner — which prints
`EXTINCTION: el1-sync recursion ...` **without going through `extinction()`**,
and was therefore enrolled in *neither* serializer. Not in the 2026-08-16
console-word fix, and not in the vault's `abi-boot-banner` mirror set either:
`quaestor owner` flags it as matching the ABI literal *outside* the set. It now
takes both, via a new `extinction_console_claim_or_own()` — claim the word, or
confirm this CPU already owns it, since the runaway is reachable from a chain
that claimed it at depth 1; a *peer* holding it means a peer is dumping, so it
parks silent like any loser, counted.

Worth noting how it surfaced: **not** by the census I ran, but by deleting the
old symbol and letting the build fail. A rename is a census that cannot lie.

It also reports a ring-claim miss after its own banner, which cost the SMP gate
a restart: I noticed the asymmetry (only `extinction()` reported) five boots
into the matrix. Killing it there and re-running cost ~10 minutes; letting it
finish and re-gating afterwards would have cost ninety, and shipping the green
from an ELF that no longer matched the source would have been a *misleading*
green, which is worse than a red.

**And that path is exercised by no test at all — this chunk just put three
calls on it (main#246).** In a healthy kernel the #806 guard extincts at the
*second* kernel fault, so `g_el1_sync_depth` never reaches 3; reaching the
runaway needs the extinction/Halls path itself to fault — which is precisely
the base-tree defect below, and precisely what this fix removed. The fix
deleted the only thing that was reaching the path it also modified. "No current
path drives it" is the latent-P1 trap, not a safety argument, so it is filed
rather than glossed.

### Then the base measurement, which is the actual finding

`tools/test-fault.sh` passed 7/7 on the change. To be sure the pass meant
something I stashed the work and ran the sharpest variant on the base tree:

| tree | `recursive_kernel_fault` |
|---|---|
| base `f525cea3` | **TIMEOUT (60 s)** — last guest line is `fault-test: invoking recursive_kernel_fault...` |
| this change (raw try-spin) | PASS — `EXTINCTION: recursive kernel fault (handler re-entered) 0xdead000000000000` |
| this change, counted `spin_trylock` restored | TIMEOUT, symptom byte-identical to base |

**The base tree printed nothing at all.** That variant installs
`TPIDR_EL1 = 0xdead000000000000` deliberately — a wild `current_thread()` is
its entire premise. `extinction()` flushes the ring *before* the banner (on
purpose: causal order), the old flush took the lock with the **counted**
`spin_trylock` → `spin_preempt_inc` → `current_thread()->magic` → **fault,
inside the extinction path**; the nested EL1-sync faults climbed to depth 3 →
`el1_sync_runaway` → which called the *same* flush → faulted again → depth 4 →
the `depth > MAX` arm parks **silently**.

So the one fault variant whose whole point is a destroyed `current_thread()`
could not print its own banner — and failed by **silence**, not by a wrong
message, which is the shape that reads as "the harness is slow" rather than
"the protection did not fire". Broken since `ed56f21f` (#75 P1-F, 2026-07-20)
met `ce7bd352` (#360's counted spinlocks, 2026-07-04): about a month, because
**`test-fault.sh` is wired into no gate** — grep-proven over the Makefile,
`ci-smp-gate.sh`, `test.sh`, `test-interactive.sh` and `.github`. It is the
only runtime witness that W^X, BTI, the stack guards and the #806 guard
actually fire, and it runs when someone remembers. Filed main#244 (the defect,
closed here) and main#245 (the ungated harness, open).

**The rule that generalizes, and the reason `spin_trylock_raw` exists:** a
dying-machine path may not call a primitive that reads state the crash may have
destroyed. #360 retrofitted that `current_thread()` deref under *every* existing
`spin_trylock` caller — including one on the extinction path — without anyone
re-asking whether that caller could survive it. The `spin_lock_raw` comment now
enumerates its two legitimate holders instead of naming one and calling every
other use a bug.

### A defect I nearly fabricated, and what stopped it

The sabotage run's failure lines came out as
`[test] cons.ring_claim_core_returns_holding ...   [runnable-dump returns HOLDING: a second taker must fail while the claim is held]`
and I read that as a live tear of exactly the residual class I had just filed
(main#243: direct-`uart_puts` diagnostics outside the ring lock). It is not.
`test_fail(msg)` calls `sched_dump_runnable(msg)`, which prints
`"  [runnable-dump " + tag + "]"` — the tag **is** the failure message. Intended
output, read as an interleave because I was primed for one. Withdrawn within
the minute, by reading the caller instead of the line. *A fabricated defect
outranks a missed one*: it would have eaten the budget a real one needs, and it
would have "confirmed" a bug I had filed an hour earlier — the worst direction
for a confirmation to arrive from.

### Posture

Suite **1427/1427** (was 1424 — three new legs), `test-fault.sh` **7/7**, both
sabotage arms verified in one run (1427 → 1424/1427, each failure naming its
own assertion; source restored byte-identical to the verified WIP and re-run
green). The kernel changed, so the SMP gate is owed and running.

**Still open, exactly:** source 3 (`IPI_HALT`) — untouched. And the ring lock
reaches only writers that go *through* the ring: steady-state kernel
diagnostics that still call `uart_puts` directly (`sched.c`'s runnable-dump,
`syscall.c`'s vivarium unserved / `viv-trace`, `exec.c`'s exec-failure,
`9p_client.c`'s ownerless-frame) sit outside it and can still land inside the
banner from a peer CPU. `cons.h`'s contract already says those callers should
use `cons_diag_line`; converting them is main#243, and they carry the #126
20-ms-per-byte exposure too. **This closes one of three sources, and the third
would subsume the residual of the second.**

---

## 2026-08-18 — C-6b: the readback arm off the console's dispatch, and the load that measured which GL context a queue is on

Resumed from the self-compaction at `64ded01d` (the C-0d Fable close + the
C-6a spec pushed). The mac was aux's for the first hours (its SMP gate, then
its round-B P1 fix), so this run did its reading, code and docs cold and
queued on the lease for every build — three times, because the gate's
positive control kept saying "the queue you built is not the queue you
think", which is the finding worth writing down.

### The implementation (`server.rs` / `gpu.rs`) — one refinement the design's letter did not have

GPU-DESIGN 4.5.13 said the compositor-owned tag would carry `ctx_pub = 0`.
Reading the driver's abandonment bookkeeping said no: `fslot_poison_ctx`,
`FenceVindication.ctx_pub` and `ctx_has_poisoned_slot` all key on the tag's
ctx, and 0 is `warp_ctx_vindicate`'s "no condemned slot" sentinel — an
abandoned compositor readback under ctx 0 that the device later retired
would push a vindication for ctx 0, `position(p == 0)` would match an
arbitrary un-condemned slot, and `ctx_destroy(slot+1)` would hit a live host
context. And the client's own vindication has to WAIT for our abandoned
readback of its BO (round-4 F1: one late retire proves nothing about the
rest), which only holds if the slot is attributed to the client. So the tag
carries the CLIENT's `ctx_pub` plus explicit `readback` / `comp` bits; the
pump routes on the bit and poisons / decrements the right ctx. Recorded as
AS-BUILT 1 in 4.5.13. Everything else is the design as written: the
reserved slot (`COMP_FSLOT` = 15; the client pool is 0..15 and
`lane_exhausted` / `fenced-free` read only that), `Comp.comp_rb` +
the gen-pinned `rb_wanted` FIFO (one in flight compositor-wide — the slot IS
the bound), `comp_readback_retired` BEFORE `warp_pump_retires` in the pass
(the pump's decrement can quiesce a retiring BO; the compose must read `va`
first, and `gl_adoption` refuses a retiring BO/ctx so a destroy in flight
drops the frame), `fences_in_flight` + `comp_rb_in_flight` symmetric on
issue and retire, the admission subtraction, the sticky 30 s deadline while
any readback is in flight, `Cost::ReadbackWait`, the `comp-rb` census (keys
prefixed — `abandoned` was already the test-mode key and `parse_field` takes
the first hit).

### The gate, and the two loads that were not the load

`warp-prove readback` (its own verb, like `reject`: it stalls the device on
purpose) with named arms — ARM (a present on an idle queue issues and lands
a compositor readback), DEEP (the readback the device paid waited ≥ 100 ms:
the positive control that the queue existed), LIVE (while it is in flight,
the adopting surface's own presents and warp ctl reads answer inside 50 ms —
under the old arm the first present takes the whole wait), DEADLINE (a
client's OWN fenced readback of its busy BO, then ten bystander presents
behind it: all succeed, engine alive — busy read as busy), F2B (the
bystander's latency, reported), CLEAN. `C6-READBACK DONE` is a verdict (the
F6 shape); `warp-readback.exp` hard-fails on `INCOMPLETE(<arm>)`.

**Run 1** (800 1:1 NEAREST full-frame blits, ping-pong BO ↔ scratch): ARM
PASS, LIVE PASS, DEADLINE PASS — and DEEP FAIL: `readback-wait max 16 ms`.
1.6 GB of copies do not finish in 16 ms on a Pi. `vrend_renderer_blit`
(1.1.0) takes the `glCopyImageSubData` shortcut for a 1:1 same-format RGBA
NEAREST blit; whatever those became, they were not GPU work the readback
waited on. Without the control LIVE would have passed on a light queue —
which is exactly why the control is there.

**Run 2** (SCALED blits, 512² ↔ 1024²): the 8 submits retired in **1335 ms**
— real work — and DEEP still FAILED: the compositor readback of the same BO
waited **84 ms**, and the client's own readback stalled the bystander by at
most 149 ms. LIVE FAILED too (94 ms), which turned out to be the same
mechanism seen from the other side. A scaled blit goes through
`vrend_renderer_blit_int` → the BLITTER, and vrend's blitter owns its **own
GL context** (`vrend_blitter.c`); a client-context fence and a
client-context `glReadPixels` are not ordered behind another context's
work. The queue was deep; the readback was not behind it. **A claim about a
lane must be re-derived per COMMAND CLASS** was C-0d's lesson; this is its
sibling: **a queue is deep only on the GL context the wait is on.** A real
client's draws land on its own context, so the honest load is client-context
work: **run 3** queues clear PAIRS (the BO to an index-encoded colour, then a
2× scratch, alternating framebuffers so mesa v3d cannot fold them — each a
full-surface store), and the leg now prints the queue's fence timeline and
**which clear index the compositor readback observed** (the BLUE byte of the
pixel it landed): "the readback waited for the queue" is a pixel, not a
duration.

**Run 3** (alternating full-surface clears, BO ↔ a 2× scratch, index-encoded
colour): the readback observed clear **639 of 640** — it DID wait for the
whole queue, the mechanism is right — and the whole queue took 122 ms: mesa
v3d keys jobs by framebuffer (`v3d_get_job`), an FBO switch does not flush,
and 1280 clears folded into two jobs. **Run 4** (draws — hand-encoded from
the Mesa tree's `virgl_encode.c` field for field, a `verify` after the prime
so a rejected stream names itself): DEEP PASS at last (readback-wait 130 ms,
draw 2399 of 2400 observed) — and LIVE FAIL on the SECOND present (140 ms
inside a 168 ms flight; the issuing present 0 ms). **Run 5** made LIVE the
issuing present over three rounds and reported the rest: LIVE 0/0/0 ms;
DEEP failed one round at 88 ms because the eight 24 KiB Twrites
themselves took 130–290 ms and the ~415 ms queue was nearly drained at
issue. **Run 6** deepened the queue (3 triangles per draw) and added the
census of OTHER console work per round: `slot-presents +1` in EVERY round —
the console renderer's cursor-blink present — and the sends took 478 / 794 /
1062 ms. That named the deterministic blocker: on egl-headless a present's
`RESOURCE_FLUSH` is the display backend's `glReadPixels` of the screen (the
C-4 lane cost), queued behind the compositor's blit, behind the client's
draws on V3D's one hardware FIFO; the single-threaded loop waits there for
everyone, and my own sends waited behind it too, so a readback issued after
them met a drained queue. **Run 7** halved the send exposure (4 submits × 6
triangles) and made a round self-validating — issued into a queue with less
than the floor left = UNCONSTRUCTED, retried, never judged — and the gate
went green: `WARP-C C-6 GATE: VERIFIED`, issuing present 0/0/0 ms,
readback-wait 497/1001/1027 ms, draw 1199/1200 observed every round, two
unconstructed rounds retried; DEADLINE 10/10 alive; F2B max 1034 ms mean
119 ms. The final artifact re-ran green (805/1005/1005 ms, F2B max 267 ms).

**Sabotage S1** — the issuing present made to WAIT for the readback (the
pre-C-6 arm): first run read as `deep-unconstructed`, because the prover
stamped the issue time AFTER the present returned; stamped before it, the
sabotage fails LIVE with the issuing present at 269 / 969 / 1017 ms — the
arm discriminates the defect and nothing else. Not run: a sabotage of the
deadline widening — no stale wakes were observed during ~1 s stalls on this
lane, so the old 500 ms deadline may never have fired here; the widening is
correct by construction and the DEADLINE arm is its net where wakes arrive.

What the run says about C-6 under QEMU/virgl, honestly (AS-BUILT 3 in
4.5.13): the console never waits inside the present that issues the
readback, and one readback is in flight at a time — but any sync step the
console issues while a client's queue is deep inherits the stall, and on
egl-headless every present is such a step. C-6 removes the per-present
multiplication and the false dead-latch; the stall itself is the host's
(F2b) until Venus / v3d.

### The bar

Local: suite 1424/1424 + arc gates 2/2 + clade 3/3 + G-4 CONSOLE VERIFY OK
(kernel byte-unchanged; SMP 40/40 @401d4b27 carries). thyla-pi (KVM, V3D,
virglrenderer 1.1.0): `readback` VERIFIED on the final artifact; `reject`
C-0d DETECTOR VERIFIED; `prove` WARP-2 VERIFIED; `quake` WARP-4 VERIFIED
(969 frames 44.2 fps; `comp-rb issued 0`); `decomp gl` PASS (composed gpu
1106 cpu 0; `readback 0`, `readback-wait 0` — the blit arm untouched). LS-CI
gfx subset (ls-ci + 15 ls-gfx-*) 16/16, 0 retries, run alongside the Pi's
final gate (the mac idle otherwise). Every ramfs verified by content before
each sync (`cpio` extract + `strings`), and the `cd usr` trap paid three
more times before I split the build from the bake.

## 2026-08-18 — the C-0d Fable close: C-4's lesson had been applied to one pair and not the other, and the readback arm's remedy is not what it looked like

Resumed from the self-compaction at `401d4b27` (the merge pushed; the C-0d
Fable verdict in hand: 0 P0 / 2 P1 / 1 P2 / 2 P3, nothing fixed). The mac was
aux's for the first ~1.5 h of the run (its viv-run LS-CI legs), so this run
did all its reading, editing and design with no cores and queued on the lease
for the build — which is what the leases are for.

### The close (F1 / F5 / F6 fixed, F3 recorded) — `ef58d639`

**F1 was C-4's own residue.** §4.5.12 had measured that a texture transfer
or readback on a tiled renderer is a blit job behind everything the *device*
has queued, and moved the compositor's health pair to buffers — and left the
per-ctx #240 probe (`warp_probe_build`) a texture pair, because the
compositor's helpers (`health_upload` / `health_readback` /
`comp_copy_region`) had `COMPOSITOR_CTX` hardcoded and the client verify kept
its own texture-only transfers. So every client `verify` was still the drain
C-4 had just priced, and — the part the round added — one client's verify
paid for *another* client's queue, which the verify admission gate (F7's
`fences-in-flight`/`poisoned`, reading only the caller's gauges) cannot see.
The fix is structural rather than local: `CtxProbe.buffer`, the buffer mint
first for every ctx (`warp_hprobe_build`), the texture pair only where that
mint fails and counted (`probe-texture` on the global ctl — a say line at
ctx-create rate would be a storm), and ONE helper set for both pairs
(`probe_upload` / `probe_readback` / `probe_copy_region`) so the compositor
and the clients cannot drift again. The prover's C0-F1 leg had to change with
it: it attacked from a TEXTURE BO, and a texture->buffer
`RESOURCE_COPY_REGION` is not a legal copy — the renderer would have dropped
it and the leg would have printed DEFENDED for the wrong reason (a control
the operation erases). The attack source is a buffer of the probe's own
shape now (`mint_buffer_bo`, `rcr_stream` with a width).

**F5** (`present-to N bo`/`off`/`N bo` re-running the whole import witness on
the SHARED compositor context at 9P-write rate): the `verify_tick` shape,
one witness per ctx per compositor tick — but DEFERRED, never dropped: a
same-tick second consent sets `import_pending` and `frame_tick` replays the
import of whatever `present_to` names by then. The winsys re-consents only
when its front buffer changes, so the only legitimate second write in one
frame is a resize storm, and coalescing those onto ticks costs it one tick of
the readback arm.

**F6** (warp-prove printed `C0-REJECT DONE` unconditionally, so a blind
detector passed the scenario and only the host-side 5-term grep gated it):
DONE is a verdict now — every C0 arm records pass/fail and the token prints
iff all three passed, else `C0-REJECT INCOMPLETE(<arm>)`, which
`warp-reject.exp` hard-fails on through a new `lc_run_expect_hardfail_re`
(a regexp fail arm, so the prover's own `FAIL --` shares it). The 5 terms
stay as the belt: a scenario that passed for a reason the list does not know
about should still fail there.

**F3** recorded on #171 with a comment at `warp_probe_res_kind`: the probe's
two page mappings ride the never-rewound `weave_va_next` bump — a ctx-churn
driver on the same monotonic-VA class. Also noticed while writing it: the
detach names `size` while the bump rounds it up to pages — equal today (both
PAGE), and written down so a differently-sized probe cannot silently leak.

**Also found: the #240 detector's four rounds were never in
`AUDIT-TRIGGERS.md`.** r1–r3 lived in phase7-status rows and memory files
only. The tapestryd row now carries the addendum (all four rounds, this
close's fixes, five prosecute-on-change items).

### F2, and the design that came out of reading QEMU before writing it

F2 [P1] is the composed-GL present's readback fallback: `transfer_from_3d_
sync(g.dev_ctx, ...)` of the whole frame on the compositor's SYNC slot, so
the console's dispatch waits for the frame — for everything the client has
queued ahead of it, a length the client picks — and `fence_poisoned` cannot
guard it (the poison comes from `reap_abandoned` on the loop that is
blocked). The pickup note prescribed "the fenced / bounded readback". Reading
QEMU's `virtio-gpu-virgl.c` + vrend before designing it (the §4.5.4c habit)
changed what "fenced" buys: **vrend executes `TRANSFER_FROM_HOST_3D`
synchronously at DECODE time on QEMU's serial main loop** — `glReadPixels`
into the guest iov, returning only when every job writing the resource has
completed, which on V3D's in-order queue is every job queued before it — and
`FLAG_FENCE` changes only when the *response* is written. So a readback of a
busy resource stalls the DEVICE (every other client's commands, the
compositor's own sync steps, QEMU's display refresh) for the resource's GPU
backlog; fencing it frees the *guest* thread and nothing else; and a sync
step queued behind it inherits the stall — which makes `submit_and_wait`'s
"pending fences ahead cannot delay this chain" comment (true for fenced
SUBMITs, a decode) false for fenced readbacks (a GL wait), and its 500 ms
`SUBMIT_DEADLINE_MS` a false-`dead` hazard on a merely busy device.

That reframed the goal from "make the readback free" (impossible under
QEMU/virgl by construction) to three narrower things: the console's dispatch
never blocks on a client-chosen duration; the compositor never latches
`dead` because a device was busy; the compositor's OWN contribution to
device stalls is bounded and coalesced. GPU-DESIGN 4.5.13 (C-6, RESERVED) is
that design: the fenced readback with DEFERRED present completion, one in
flight per surface / latest wins, a reserved fenced slot (compositor-wide
bound of one, which loses nothing against a device that executes them
serially anyway), counted in the owning ctx's `fences_in_flight` for retire
safety but subtracted from admission so the client's share and its #210
ledger are untouched, and the sync-slot deadline widened to
`FENCE_ABANDON_MS` while any readback — ours or a client's — is in flight.
Two forms rejected on the record: a bounded sync wait (the command is already
in the device's queue; the next sync step waits behind it — bounds the wrong
thing) and gating on quiescence (a single-buffered client at its throttle
depth never quiesces; the §4.5.9 safety net would compose it once and never
again). The spec extension is named (`ComposeReadbackIssue`/`Complete`
behind `ALLOW_COMPOSE`, the retire guard generalized from `DrainedOfBlits`,
a `buggy_readback_free` cfg) and the Pi gate legs with it.

**And a new finding fell out — F2b.** Consequence 3 of the reading: *any*
client already holds the device-stall lever through its own `transfer_from`
of its own busy BO (the fenced verb every winsys has), repeatedly. F2 was the
compositor doing to itself what a client can do to it. Filed
(`memory/bug_f2b_readback_stalls_the_device.md`; GPU-DESIGN 4.5.13's F2b
paragraph): guest-side it can be not-added-to (C-6), not-mistaken-for-death
(the deadline half), and MEASURED (a warp-prove leg — client A reads back its
busy BO while surface B presents — owed with C-6's gate); it is removed for
real only by Venus (transfers become VkCommandBuffer copies the client
fences) or v3d-native (the queue is ours). Recorded under §9.2's host-side
exposures precisely so "trusted host" never reads as "no client can reach
it".

### Two things the bar found before it passed

**The C0-F1 leg's DEFENDED was a negative assertion with no positive
control** — "verify-ok still advanced after the attack" is satisfied by an
attack that never landed (the aux#215 class), and the texture-era leg had
leaned on a one-time host-log measurement for that; the buffer form did not
inherit it. Added in-guest before the first Pi run was trusted: after the
attack the client copies the mark BACK into its own buffer (the same command
the other way), reads its buffer back through the fenced verb, and requires
its own green. It printed `C0-F1 ATTACK LANDED -- the mark read back through
our own buffer as 0xff00ff00` — so the leg now proves a client can WRITE and
READ the probe's resources (the finding, re-measured on the buffer pair)
before it claims the repaint held; an unlanded attack is INSTRUMENT and F1
counts as not-defended.

**`warp-host.sh sync`'s uncommitted-scripts list omitted
`tools/interactive/lib.exp`** — the library every warp `.exp` sources. The
first sync shipped the new `warp-reject.exp` (in the list) against HEAD's
`lib.exp` (not in it), so the scenario would have died on `invalid command
name lc_run_expect_hardfail_re` — a list that claims to carry your edits and
does not carry the one file they all depend on. Caught by checking the Pi's
copy for the new proc before running (`grep -c` on both files, 1 vs 0);
`lib.exp` is in the list now.

### C-6a — the spec first (`tapestry_present.tla`, same run, after the push)

With the close pushed and ~100k of context left before the checkpoint line,
the next chunk was opened at its spec-first step rather than its code, so
that a compaction lands on a boundary and C-6's code has a model to be
audited against. `ComposeReadbackIssue`/`ComposeReadbackComplete` (a fenced
host DMA-WRITE into the client BO's pages, one in flight per generation),
`NoTornReadback`, `DrainedOfReadbacks` on `ServerRelease` + `Free`, and
`BUGGY_READBACK_FREE` as an omitted conjunct — the C-1 house style, for the
C-1 reason (a twin action drifts in more ways than the one under test). Two
deliberate absences, argued in the header: no `FillLanded` guard on Issue
(the device serializes the read against the fill — the very side effect P2
credits the sync readback with, now read in vrend 1.1.0 rather than
assumed) and no `attached` (the readback runs under the CLIENT's ctx; it is
the arm for the un-imported BO). `check-tapestry.sh`: ALL 12 CFGS AS
CLAIMED — the six direct-path cfgs at **5413** states exactly (the
additivity control, held twice now), the composed clean cfgs at 94680 with
liveness, and `buggy_readback_free` violating `NoTornReadback` in 11 states
(… `ClunkMap` → `ComposeReadbackIssue` → `Destroy` → `ServerRelease` →
`Free`: the pages freed with the device still writing them). SPEC-TO-CODE
names the sites the impl binds at; ARCH §28 I-40 / CLAUDE.md say 8 buggy
cfgs now.

### The bar

Local (mac): `cargo build -p tapestryd -p warp-prove --release`; ramfs
rebaked with `THYLACINE_BAKE_CLADE=1 THYLACINE_MKFS_PRESERVE=1`, verified by
CONTENT (`C0-REJECT INCOMPLETE` ×3, `probe-texture` ×1, `ATTACK LANDED` ×1
in `build/ramfs.cpio`); `tools/test.sh`: 1424/1424, arc gates L-6c/D-5 PASS,
clade 3/3, the G-4 console gate `CONSOLE VERIFY OK`. The kernel is
byte-unchanged (userspace + tools + docs only), so the SMP gate 40/40 at
`401d4b27` carries. thyla-pi (KVM, V3D, virglrenderer 1.1.0): `reject` →
`C-0d DETECTOR GATE: VERIFIED` (ANSWER=REPORTED-AS-SUCCESS as measured
before; DETECT PASS; STICKY PASS; C0-F1 first res 83 → mark 81 (the buffer
pair minted exactly two ids), ATTACK LANDED, DEFENDED; DONE; LS-CI PASS);
`prove` → `WARP-2 GATE: VERIFIED`; `quake` → `WARP-4 GATE: VERIFIED` (969
frames 21.7 s 44.7 fps on the egl-headless lane — 44.4/44.8 before;
`comp-attach witnessed 5 refused 0`; `comp-health verify on buffer pair`;
`probe-texture 0`). Both leases released the moment the resource freed;
the mac was aux's for the first ~1.5 h and its LS-CI legs were never
contended.

## 2026-08-17 — the aux-2 merge: two tracks fixed one UAF, and 23 conflicts said which one to keep

Resumed from the self-compaction at `a9a4a4fe` (Warp-C closed). The note said
"merge aux-2 first", and the reason it was first is the interesting part: the
main#243 Fable round had found a P1 (exec leaves `in_handler` set) plus two P2s,
and every one of them was ALREADY FIXED on aux-2 — aux had found the same UAF
(`#254`) the same week, from the other direction. Two independent proofs of the
same defect are worth more than one; two independent FIXES of it are a merge
conflict, and the conflict is where the decision lives.

### The merge itself (`8a58112d`)

104 aux commits over the common base `72ab319d`; 216 main commits the other
way; 23 conflicted files. The rule for every conflict was "which side's version
is the RATIFIED one", not "which is mine":

- **The sigtab UAF, twice.** main `a41fc9eb` reset the table in place through a
  public `proc_exec_reset_dispositions`; aux `c2a09473` + `8690cfb3` + `d3a11c8e`
  did the same through a static `proc_exec_drop_image_state` that ALSO clears
  the in-handler latch (#247 = main F1) and applies the operator-voted
  phenotype rule (F4). Aux's is the superset and is kept as THE one place; main's
  function is gone. What main had that aux did not was the per-8-byte-FIELD
  paragraph and an every-byte-zero test — folded into aux's comment, and the test
  ported onto aux's `_for_test` hook rather than deleted, because it asserts a
  property aux's test does not (a reset that stops early passes aux's).
- **`cons.c`'s mode write.** main's side was a COMMENT change (#233: login must
  set the mode before the prompt); aux's was a semantics change ratified in
  PTY-DESIGN and audited (a write clearing ICANON DELIVERS the pending line).
  Aux's code, plus main's corollary — the disclosure half of #233's race exists
  under either semantics, so the sentence still binds.
- **The bin lists** (`tools/build.sh`, `usr/Cargo.toml`): the union, verified
  programmatically against the base — no member dropped by either side.
- **AUDIT-TRIGGERS.md** was an add/add (both trees created it from CLAUDE.md's
  table on the same day and each appended rows): resolved ROW BY ROW against the
  base row, so main's vault-#170 path fixes and pipe escapes and aux's addenda
  both survive; the LS-8 row carries both sides' addenda in order.
- **147-execve.md's sigtab row** was stale on BOTH sides (main said "zeroed in
  place", aux said "zeroing is exact POSIX because SIG_DFL == 0" — aux's own later
  commit had made the reset phenotype-conditional). Rewritten to the MERGED rule
  rather than picking a stale side; the note-mask and in-handler rows added.
- **Seven ragged doc rows** (six pre-existing on both tips, one in aux's newest
  addendum) escaped with the two controls `85c1ee9c` used: the checker to zero,
  and de-escaped-line == original with only the named lines differing.

**One thing the resume note did not say and the build did:** aux's DISTRO gates
are pool-resident and SOFT-SKIP without the Alpine tarball, which main's cache
did not have. A green `tools/test.sh` with two skipped arc gates is a gate not
run — so the fixtures were copied from aux's cache and the pool + ramfs re-baked
PAIRED (`PRESERVE=0`, fresh key both sides). `arc gates: 2/2 ran -- L-6c=PASS
D-5=PASS` on the merged tree; suite 1424/1424; clade 3/3.

### The main#243 residuals, on the merged tree (F2/F5/F6/F7/F8)

The round's F6 was the sharpest: the 8-byte store width that the whole lock-free
argument rests on was a MEASURED codegen property (a struct assignment happened
to give `stp`), not a construction. It is a construction now — every entry field
is one `__atomic_*` op on an aligned u64 (`_Static_assert`ed), the install
publishes `handler` last with release and readers acquire it, the reset zeroes
`handler` first; objdump shows `str xzr` per field and `stlr`/`ldar` on the
gate. F2 wrote the load-bearing sentence AT `notes_proc_has_live_handler`
("a cross-Proc reader that acts on `handler` alone; the copy is discarded"),
which is the sentence the three earlier statements of the argument had each
left implicit. F5's discrimination was checked the only way that counts: two
sabotages (a reset one entry short; the gate field only) each went RED on the
named assertions, and the tree was reverted with text replacement, not
`git checkout`. F8 clears `clear_child_tid` at exec beside `in_handler`. F7
retired four stale sentences (three of them "X is not a table row" claims that
the LINEAGE arc had falsified without anything failing).

### The C-0d Fable round came back while the bar ran: two P1s the three Opus rounds could not see

The #240 detector's first read from a different lineage (98 of 101 model
turns Fable; the last three, the write-up, fell back to Opus 4.8 — recorded):
**0 P0 / 2 P1 / 1 P2 / 2 P3, dirty on the P1 criterion.** Both P1s are the
same blind spot from two sides, and it is exactly the one family independence
exists to buy: three Opus rounds gated the synchronous lane on the CALLER's
fence gauges, and none re-asked the cross-context question after C-4 measured
that a texture readback on a tiled renderer drains the whole device queue.

- **F1**: the CLIENT-ctx probe is still the TEXTURE pair. C-4 moved the
  compositor's health pair to buffers for precisely this cost and left the
  client detector as it was — so a `verify` on client A drains behind client
  B's queue while the gate reads only A's gauges, and 149-warp.md promises
  clients the opposite. Fix: the buffer pair for clients too (the C0-F1 leg's
  attack source has to become a buffer BO, or it "defends" for the wrong
  reason — a texture-to-buffer copy is refused, not repainted away).
- **F2**: the composed READBACK arm — the CPU fallback — is a synchronous
  full-frame readback of the client's render target on the client's own
  queue; the client picks its length; and `fence_poisoned`, round 3's gate,
  cannot protect it because the poison is produced by the reaper on the very
  serve loop that is blocked. Only READBACKS carry this (a blit's SUBMIT_3D
  response is written at decode time, before the GPU runs it), so the fix is
  not a gauge but the fenced form C-4 measured its way past — a bounded or
  deferred readback: **Warp-C C-6**, the next chunk. Gating the fallback on
  `fences_in_flight == 0` was weighed and rejected: it would collapse the
  safety net GPU-DESIGN 4.5.9 keeps for every continuously-rendering client.
- F3 (probe VA rides the never-reclaimed `weave_va_next`, a second driver
  for #171), F5 (`present-to` re-import witness storm on the shared ctx, no
  rate limit), F6 (the reject scenario's pass token is printed unconditionally;
  the real 5-term gate lives only in `warp-host.sh`). Dispositions in
  `memory/audit_c0d_fable_closed_list.md`; the close is the next chunk after
  the push, then the dirty-close follow-up round.

### The bar found one more thing, and it was ours from the merge

The merged tree's first LS-CI (JOBS=3) came back 37/37 — with **three attempt-1
failures at t=0-1 s**, every one `-qmp unix:build/qmp-gate.sock ... Failed to
bind socket: File exists`, every one classified INFRA by aux's failure-time
probe ("the VM never started, so this attempt says NOTHING about the guest").
aux's #230 had given run-vm.sh a SECOND QMP monitor for test.sh's screendump
gate — a fixed path — and test-interactive.sh's per-slot export list, written
for #127's lesson that "a fixed host resource is a DETERMINISTIC collision at
N>1, not a flake", predates it. Three VMs launched in one batch interleave
run-vm.sh's `rm -f` and bind, and the loser dies before boot. A retry budget
turned a deterministic collision into three green retries; the count is what
gave it away. `e680fdd5` exports `THYLACINE_QMP_SOCK2` per slot; the re-run
was **37/37, 0 retries, wall 1744 s** against 2569 s before — and the SMP gate
on the merged kernel: **40/40, 0 corruption / 0 external-kill** across
default+UBSan x smp4/smp8. Pushed to both mirrors at `e680fdd5`.

---

> **Two tracks, one thread.** Entries marked `(aux)` were written on `aux-2`
> and merged into this file when aux-2 merged into main (2026-08-17); the two
> tracks ran concurrently, so a main run entry and the aux entries beside it
> overlap in wall-clock time. The `(aux)` block below is in the order aux
> wrote it -- oldest first, `c8ab2744` to `01f076f2`; main's run entries
> below it are newest-first as the convention says.

---

## 2026-08-17 (aux) — the c8ab2744 audit close, and the positive control that caught a second bug

Resumed from aux's **first** self-compaction (the change-of-watch scripts had
been main-only until `4525023a`; the operator had compacted this track by
hand). The nudge fired and the resume note said, correctly, "execute the plan;
do not re-derive it" — the Fable 5 round on `c8ab2744` had reported the audited
change CLEAN and four PRE-EXISTING findings three lines above it, and the fix
plan was already written in `memory/audit_15_closed_list.md`.

### The four fixes (`93a91c6c`)

- **F1 [P1] — both class scans read the sigtab per note.** The terminate scan
  gated on `handler_va` (0 for every Linux guest) and returned the first
  latch-class name at ANY index, so a `SIG_DFL` candidate that fell through
  from the phenotype branch let it name a CAUGHT `tty:hup`/`interrupt` behind
  it and the guest died with its handler installed. #251's per-Proc predicate
  had reached three sites and not this one — the fourth "site N+1" on the row
  (V-8 F2 → #251 → maskstop → F1). Fix: `notes_proc_default_applies(p, name)`
  INSIDE both scans; the fixed-name outer gate on the stop scan retired.
- **F2 [P2] — a `SIG_DFL` `pipe` on PHENO_LINUX reached no arm** (no native
  latch, #237) and sat as the dispatcher candidate for life. Fix,
  phenotype-scoped: `viv_signote_default_is_terminate` + `exits(canonical)`
  from the phenotype branch on the candidate. Native `pipe` untouched; #237
  stays the ABI question it is.
- **F3/F4 [P3]** — the dead drain call deleted with its reasoning; three "an
  uncaught susp is never queued" sentences reworded (caught / all-masked /
  thread-less).

### The wrong turn worth recording: J and L passed on an empty capture

The E2E for F2 is three L-6c legs sharing one fixture — `err=$( { WRITER 2>&3
| head -n 1 ...; } 3>&1 )` — J and L asserting the writer printed NOTHING (killed
by SIGPIPE), K the positive control (`trap "" PIPE` in the writer's own process
→ EPIPE returned → `write error` reported). Boot A: **J green, L green, K red,
`L6C-K-RAW:` empty**, and once per leg on the console:
`/gate/run.sh: line 9: fcntl(3,F_DUPFD,10): No file descriptors available`.

busybox ash's `redirect()` probes the TARGET fd of every `N>&M` with
`fcntl(N, F_DUPFD, 10)` to learn whether N is open — `EBADF` means "not open,
nothing to save"; anything else is "strange" and aborts the command. The
vivarium's `VIV_FCNTL_DUPFD` arm answered `EMFILE` for BOTH of
`handle_dup_posix`'s folded failures, on a comment arguing that a guest which
just used the fd knows it exists. True about the wrong caller. So the whole
capture never ran, the substitution yielded "", and two negatives were
satisfied by a broken fixture — aux#215's class, caught by the remedy aux#215
prescribes. Without K this would have shipped as two green legs proving
nothing. Fixed in the same commit (a liveness re-check after a failed dup:
closed → `EBADF`, residual → `EMFILE`; `vivarium.fcntl_dupfd_errnos`).

Boot A2 then showed a second fixture wart: `head -n 1 >/dev/null` printed
`can't create /dev/null: Function not implemented` — ash opens `>` with
`O_CREAT|O_TRUNC` and `O_CREAT` is a KNOWN unserved openat flag (#201, designed
around). The legs still measured SIGPIPE correctly (the reader slot died before
reading instead of after one line), but a fixture must not lean on a known
gap: the reader now writes its one line INTO the capture, so J's assertion is
the sharper "the capture is EXACTLY `y`" — the reader really read, the writer
was silent.

### The bar

Suite 1405/1405 (+2). Sabotages, each reddening its named assertion and
nothing else: S1 (terminate gate dropped) → `A: the terminate scan does NOT
name the CAUGHT interrupt`; S2 (stop gate dropped) → `D: the stop PREDICATE
declines a caught susp`; S3 (phenotype `exits()` disabled) → suite green,
L-6c `first-missing=L6C-J`, L missing, K present. pty + pty_stop: 4 clean/
liveness cfgs green, 6 buggy cfgs violate (rc 12/13) — after fixing the runner,
which first "passed" all ten legs in 0 s because `/usr/bin/java` is the macOS
stub and every rc was 1 for the wrong reason (the buggy legs read as
violations). Keyed on the exit code AND the `TLC2 Version` banner now. SMP gate
40/40 (default+UBSan × smp4/smp8, N=10, 0 corruption). LS-CI 33 PASS + 2 SKIP (GL not
baked) — and pty-4 burned a retry AGAIN, this time INTO the failure-time probe
landed at `11173762`: see the next entry, because the probe answered.

### Still open leaving this run

- #237 (native `pipe` has no latch) is sharper, not closed: the phenotype
  answers SIG_DFL SIGPIPE for its own Procs; a native handler-less, fd-less
  program still keeps a stranded `pipe` note.
- The tail's delivery-time SIG_IGN discard arm is reached by nothing (second
  unconstructed state on this row); its own chunk.
- `>/dev/null` from a Linux shell under viv fails on `O_CREAT` (#201) — the
  most common redirection in existence; the L-6c fixture routes around it.
- pty-4's burned retry: instrumented, not diagnosed.

## 2026-08-17 (aux) — pty-4's burned retry, diagnosed on the probe's first miss: the ldisc flushed type-ahead

The failure-time probe landed at `11173762` the day before, on the theory that
INPUT truncation and OUTPUT loss are indistinguishable in a plain capture and
only the guest can say which. Its first miss (LS-CI batch 6 of the c8ab2744
close bar) said, in order: `[listen]` — the raw stream showed `sle` as PLAIN
echoed text after `PTY-INNER`, then only SIX empty editor redraws where the
passing attempt shows NINE (`sleep 30\r`); `[jobs]` — nothing listed;
`[channel alive?]` — the editor answered; VM alive, bridge alive. The editor
never echoes typed text (the harness header says so), so plain `sle` can only be
the pts line discipline echoing in cooked mode.

So: `lc_run_expect` returns the instant `PTY-INNER` is SEEN — before `ut` has
reaped the pipeline, restored PROMPT_MODE and redrawn — and `lc_send "sleep 30"`
fires at once. On TCG the window is sometimes wide enough that `s`,`l`,`e` land
in CHILD_MODE (+icanon +echo): assembled, echoed, then ut writes PROMPT_MODE and
ptyfs `ctl_apply` does `p.line_len = 0; // TCSAFLUSH: a mode change resets the
assembly` — the three bytes are gone and `ep 30\r` reaches the raw editor. A
race, and a real one — but the DEFECT is the guest's: Plan 9's `devcons` `rawon`
pushes the partial line to the reader ("flush output on rawoff -> rawon", the
clumsy-hack zero byte), Linux's `n_tty_set_termios` never discards on a canon
change, and TCSAFLUSH is a caller-chosen flush that bash/readline deliberately
do NOT use (`TCSADRAIN`). Thylacine's ctl grammar offered no choice: every mode
write flushed. Type-ahead across a job's end — a paste of two lines, a script
driving a pts, LS-CI — lost the HEAD of the next line and executed the TAIL.

The posture came from the LS-8b audit's F1 remedy ("a fragment stranded across
canonical→raw→canonical prepends the next line"), copied per-pts by PTY-2c, on
the stated premise that "no current consumer flips mid-line". The premise was
falsified by the one consumer that flips around every foreground job. Both
ldiscs now DELIVER on ICANON-clear and touch nothing otherwise
(`c62eb738` scripture, PTY-DESIGN "Mode writes deliver, never discard"; the impl
`ccb597b8`): the F1 hazard stays closed because canonical→raw delivers, so nothing is
stranded, and I-20's byte conservation now holds across a mode write. A
delivery into a full ring is a real drop under a new counter
(`rx_drop_modeflush`, the #95 rule). Not built: an explicit flush verb — pouch's
`TCSETS/SW/SF` all map to the one write, which now behaves like `TCSANOW`.

Two things worth keeping from this: (1) the instrument earned its keep on its
FIRST miss, and the reason it could is that it asked the guest in a fixed order
with a control at the end (`channel alive?`); (2) a "posture" chosen as an audit
remedy is still a claim about consumers, and consumers change — the sentence
"no current consumer flips mid-line" was true when written and had no test.

## 2026-08-17 (aux) — the "reached by nothing" discard arm, and why the right fix moved the mechanism instead of reaching it

Resumed from aux's **second** self-compaction (`05708496`). The resume note's
first item was to ask the operator for the owed prosecutor round on `ccb597b8`;
the ready-to-paste prompt was written first
(`memory/audit_ccb597b8_prosecutor_prompt.md`), my own self-audit of the
mode-write delivery found nothing, the operator said yes, and the round is
running in the background while this chunk lands (its files — `cons.c`,
ptyfs — are disjoint from this one's).

### The chunk: Stream 4's "delivery-time SIG_IGN discard arm is reached by nothing"

The open item was the second member of the unconstructed-state class found by
sweeping after the maskstop one: `notes_deliver_at_el0_return`'s phenotype
branch discards a candidate whose disposition is `SIG_IGN`, and its own comment
named the only way in — "a note queued BEFORE the install is still sitting
here … this is the only place that can happen." The post-time hook prevents the
state in every ordinary ordering, so the arm needed a CONSTRUCTED one.

Constructing it in-guest turned out to be easy — `viv-pheno-probe` has raised
SIGPIPE at will since V-6c (fd 0 is a reader-less pipe write end), so
`block → write → SIG_IGN → unblock` is deterministic. But writing the legs
forced the question the arm's comment had skated over: **what does POSIX say
happens to a pending, blocked signal when its disposition becomes `SIG_IGN`?**
It is discarded AT THE INSTALL, "whether or not it is blocked" (2.4.3; Linux
`do_sigaction` → `flush_sigqueue_mask`). Thylacine discarded at the next
delivery instead. Same answer for `pending → SIG_IGN → unblock`; a DIFFERENT
answer for `pending → SIG_IGN → handler → unblock` — Linux fires nothing, the
tail ran the handler for a signal POSIX says had died. So the arm was not merely
uncovered; the mechanism it implemented was wrong on the ordering nobody had
tested, and the honest fix is not to reach the arm but to move the discard.

What landed: `notes_discard_name(p, name)` — under `q->lock`, remove every
queued note of one name, mask-blind, each removal draining the class latch as a
dequeue does (an `interrupt` armed under `SIG_DFL`, then ignored while blocked,
must not leave a Proc whose every sleep is `*_INTR`), `kill` refused; the
phenotype `rt_sigaction` shell calls it after the store whenever the new
disposition ignores (`SIG_IGN`, or `SIG_DFL` for a default-ignore signal — the
no-table `SIG_DFL` shortcut now skips only the store); and `notes_post`'s
disposition read moved UNDER `q->lock`, so store-then-lock against
read-under-lock leaves no interleaving with a stale ignored note. The tail's
arm stays as defense-in-depth — its absence would hand a stale note to the
`SIG_DFL`-terminate arm — with its comment rewritten to say exactly that.

The proof: `notes.discard_name_purges_pending` (mask-blind, per-CLASS latch
drain — tty:hup out leaves the TTY latch armed for tty:quit — survivor order,
`kill` refused, a purged FULL ring really empty: 16 out, 16 in) and probe legs
L205–L216. Round A: pending → `SIG_IGN` → unblock survives with nothing fired
(L209 is PRE-STAMPED and rewound so a death names its leg instead of leaving
joey's `??` — the marker channel is fail-only by design, and this is the one
place a marker is written before the verdict is known), then a handler
installed after is not handed a stale note (L210). Round B: pending →
`SIG_IGN` → handler → unblock fires NOTHING (L215 — the install-vs-delivery
leg; red on the tree before this chunk). Each round ends with a fresh SIGPIPE
delivered exactly once, so a queue wedged by the experiment cannot read as
"nothing fired".

### Found on the way, enqueued not fixed

Reading `proc_exec_drop_image_state` for the exec-time sigtab reset: it zeroes
every row and the mask, and its comment says "Zeroing is exact POSIX". True of
CAUGHT handlers; false of `SIG_IGN` and of the blocked mask, both of which POSIX
and Linux keep across `execve` (`nohup`, `sh -c 'cmd &'`, `trap '' INT; exec`
all depend on it). ARCH §7.6 names the clear as the NATIVE rule, so the fix is
phenotype-conditional and a scripture decision — surfaced with options in
`memory/bug_exec_resets_sigign_and_mask_phenotype.md`; recommendation:
phenotype keeps `SIG_IGN` + mask.

### The bar (`7580c1f7`)

Suite 1406/1406 (+1); V-1b PASS (L205–L216 green); L-6c PASS. Sabotages, each
reddening exactly its named assertion: S1 (the shell never purges) → V-1b
`marker=L215` — and NOT L209, because the tail's arm still saved that ordering,
which is the whole reason the arm stays; S2 (S1 + the tail's `SIG_IGN` disjunct
deleted) → `marker=L209` — the guest died at the unblock and the pre-stamp named
the leg; S3 (purge without the latch drain) → the unit test at "removing the
last interrupt drained the latch", 1405/1406. SMP gate + LS-CI ran over the tip
together with the round close below (see the fixup).

## 2026-08-17 (aux) — the ccb597b8 round came back: sound delivery, an unwitnessed counter

The operator said yes to the round while the chunk above was being built; the
prosecutor (Fable 5, read-only) ran ~20 minutes and reported 0 P0 / 0 P1 / 2 P2
/ 6 P3 — every finding on the NEW DROP SITE's witness, none on the delivery it
was asked to break. It re-derived the I-9 wake pairing, the poll relay, the SMP
ordering under `g_cons.lock`, the hook/production parity and ptyfs's
single-threaded ordering line by line and found them as claimed.

What it found instead is worth keeping. **F1**: the fifth drop site's counting
path had only a NEGATIVE test in both ldiscs — leg B "it fit, no drop counted"
against an empty ring — so a misattribution to `rx_drop_ring` (the must-stay-
zero witness) or not counting at all read green. The tree's own
`test_cons_rx_drop_counters` header says exactly why that is worse than no
counter, and I had shipped one anyway because the negative FELT like coverage.
Legs (d)/(e) now drive the site (512 filler + 10 pending → 10 counted here,
every sibling asserted unmoved, filler intact; 507 + 10 → the 5-byte PREFIX
delivered in order); the ptyfs selftest drives its site on a fresh pts.
**F2**: ptyfs had folded that drop into `drop_flush` — against PTY-DESIGN,
which named "its own counter" for BOTH ldiscs, and against `drop_flush`'s own
documented shape (a short cooked flush loses tail + newline so the line never
runs; a short mode-flush loses the tail and the terminator arrives raw, so the
truncated command RUNS — #95's exact shape, hidden under a name whose doc said
it could not produce it). One of two twins diverged from a rule written for
both, and a re-read of the scripture would have caught it. **F3/F4/F6/F7**: the
one-shot report did not name the new site; the "reachable only by a wedged
reader" claim was false (ut re-arms before it drains, so a paste can reach it);
three comments still said TCSAFLUSH; 111-cons.md carried the deleted test with
the reversed semantics. **F8**: pty-4's type-ahead leg had no ARMED witness —
bytes landing raw before CHILD_MODE or after the re-arm satisfied the cursor-35
anchor too, under the old posture as well; it now first requires the pts's
cooked echo as plain text directly after the CRLF, which only CHILD_MODE cooking
produces. **F5** stays open as a scripture vote: an ISIG-consumed ^C/^\/^Z does
not flush the pending canonical line (POSIX and Linux do; Plan 9 does not) —
the old reset masked it, delivery makes it visible; recommendation: adopt POSIX
in both ldiscs.

Closed at `56b5a412`: suite 1406/1406; S7 (kernel misattributes to
`rx_drop_ring`) → "(d) modeflush counts exactly the 10 bytes the full ring could
not take"; S8 (ptyfs folds into `drop_flush`) → `ptyfs: selftest FAIL:
modeflush-drop-not-counted`, boot-fatal.

### The bar over the tip (`56b5a412`, both commits)

One run for both (disjoint surfaces): SMP gate 40/40 — default + UBSan ×
smp4/smp8, N=10, 0 corruption / 0 external-kill / 0 other, in two halves —
then LS-CI in six batches on TCG: 33 PASS + 2 SKIP (the GL half is not baked
into this pool; not a guest result, not coverage). pty-4 passed WITH the new
armed witness (the pts's cooked echo matched before the cursor-35 anchor — the
delivery path was exercised, not merely reached). Pushed to both mirrors after
the fixup.

## 2026-08-17 (aux) — the votes came back: ISIG discards, fork/exec goes POSIX, and the 7580c1f7 round

The operator answered all three questions in one round: spawn the 7580c1f7
round (yes), F5 (adopt POSIX — an ISIG character discards the pending line in
both ldiscs), and the exec item (the phenotype keeps `SIG_IGN` + the mask). Each
landed scripture-first.

**F5** (`e69e9baf` scripture, `4df51c30` impl): the kernel ISIG arm and the
ptyfs ISIG arm zero the pending assembly when ICANON is set — a disposition like
an erase, not a counted drop, deliberately narrower than POSIX's full flush
(committed lines in the ring stay; output is never flushed — the console TX ring
carries kernel diagnostics). The PTY-3 pouch probe's leg H had pinned the OLD
posture (`x` ^C `y` CR → `xy\n`) and went red on the first boot — the fixture
that encoded the divergence, found by the change that removed it; updated to
`y\n` as on Linux. Sabotages S9/S10 each red on the named check.

**fork/exec** (`c484a7d1` scripture): reading `proc_exec_drop_image_state` for
the exec half surfaced the fork half too — task #127, recorded at L-3d as "two
behaviours and a design decision", never landed. So the chunk is the pair:
`rfork` copies the parent's sigtab into the child's OWN table (before the child
is postable) plus the caller's `note_mask`; `execve` resets caught rows only and
keeps `SIG_IGN` + the mask; native keeps the Plan 9 clear. Probe legs L217–L228
drive a real fork and a real exec (the children name the first wrong fact
through the report dup); the unit test pins the two primitives.

**The 7580c1f7 round** (Fable 5, 0/0/0/4) re-derived the install-time discard
SOUND — the linearization, the primitive, the shell, the pre-stamp arithmetic —
and found the one ordering nobody had tested: `block; SIG_IGN; raise; handler;
unblock`. Linux queues a blocked ignored signal ("the handler may change by the
time it is unblocked") and discards at dequeue; Thylacine drops at generation,
mask-blind. POSIX 2.4.1 permits both, so it is recorded as a stated divergence
rather than matched — but the docs had said "exactly as Linux", and the lesson
worth keeping is that "exactly as X" is a claim about every ordering. F1: the
SIG_DFL/default-ignore purge disjunct had no driver → L229–L232 with a positive
control (S13 reddens only the negative). F2/F4: an over-claiming comment and two
stale sentences.

### The bar over the tip (`d3a11c8e`: F5 + fork/exec + the round close)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves); LS-CI 33 PASS + 2 SKIP (GL not baked);
suite 1408/1408 per commit; sabotages S9/S10 (F5) and S11–S15 (fork/exec)
each red on the named check — S14/S15 are the WIRING witnesses (the unit test
cannot see proc.c; the probe legs L223/L226 can, and they went red). Pushed to
both mirrors after the fixup.

## 2026-08-17 (aux) — the d3a11c8e round: the fork rule was one field short

The operator said spawn; the round (Fable 5, read-only, 0/0/1/6) re-derived
both mechanisms sound — the fork copy is published before the child is
reachable and aliases nothing, the exec reset uses the same "caught" predicate
delivery uses, the ISIG discard is one field under the right lock in both ldiscs
— and found the one place the voted RULE was short. "fork copies everything
(POSIX fork(2))" copied what POSIX names: dispositions and mask. This design has
a third piece of thread signal state POSIX never has to name, because Linux
keeps it on the user stack: the kernel-side handler-execution snapshot (the
sigframe here is written for reading; `rt_sigreturn` restores from the
per-Thread save block). A `fork()` issued from INSIDE a handler — async-signal-
safe, POSIX-permitted — therefore produced a child whose user stack said "in a
handler" while its KP_ZERO thread said "not"; its handler return was refused
and it ran on past the svc into whatever followed the restorer (musl: silent UB;
the probe: `brk #0`). Fork+exec and fork+`_exit` from a handler were fine, which
is why nothing had surfaced. Fixed by copying the block with the mask
(`in_handler` written last, before `ready()`); phenotype only — a Plan 9 child
is not notified. Lesson: enumerate what the RESTORE path reads, not what the
standard lists.

The witness leg cost two extra boots for a reason worth keeping: its first
draft had the child exit 3 and the parent reap "exactly 3", and it went red on a
WORKING fix — v1.0's phenotype exit path collapses every non-zero
`exit_group(N)` to 1 (VIVARIUM task #91, "`exit(N)` is boolean"). A diag with
`exit(5)` read as 1 too. So the oracle is exit 0 versus anything else, and the
child's own marker (re-emitted by the parent on failure) carries the why. A
status oracle must be a value the status channel can carry.

Six P3s: a pre-#254 "known hazard" paragraph in `proc_exec_replace` that
contradicted the in-place reset it now calls; a phantom `viv_sigtab_copy_into`
in 145; PTY-DESIGN naming leg (f) for (e4); the ptyfs (e4) leg with no witness
for "m2s/s2m are NOT flushed" (both were EMPTY at the VINTR, so an over-broad
discard passed — it now commits `x\n` unread and leaves the echoes unread and
asserts both survive); the fcntl test's header comment migrated onto the sigtab
test; and the ISIG-DISCARD + ccb597b8-ROUND addenda living only on the
AUDIT-TRIGGERS rows that declare ARCH 25.4 authoritative (mirrored). Enqueued
from the observations: `Proc.socktab` is not cloned at fork (the fork half of
the LINEAGE dup3 note — a real L-6 gap for fork-per-connection servers), the
handler mask discipline (sa_mask|sig never applied during a handler; sigreturn
does not restore the mask), and `pty.tla`'s CookSignal echoing a char neither
ldisc echoes.

## 2026-08-17 (aux) — the console TX ring pushes UNITS now

Main handed over the byte-atomic tear it measured on thyla-pi: `proc: orphan
pid=2119 name="ttaappeessttrryydd"` — the kernel's orphan-adoption burst and
tapestryd's posture line on another CPU, byte for byte, because every producer
pushed each byte under its own `g_cons_tx.lock` hold and the writer role cannot
serialize a diagnostic emitter (IRQ context; the role sleeps). ARCH 23.5.2 had
already named the missing piece — "full echo-exclusion via a bulk-push fast
path" was #79, a documented v1.x item withdrawn from an earlier draft because it
"carries a two-ring lock-ordering design". The design point resolved as: never
nested. Tap under the drain lock, release, push under the ring lock, release.

The rule now: every producer pushes a UNIT under one hold. A kernel diagnostic
is a line assembled on the caller's stack (`struct cons_diag_line`) and pushed
once, all-or-nothing — the per-token trio is gone, because a per-token API
cannot be line-atomic without hidden state, and a per-CPU accumulator would
splice an IRQ handler's line into the process-context line half-assembled below
it; a caller-owned object is nesting-safe by construction. Echo pushes its
staged unit whole (half a `\b \b` walks the cursor over the prompt). The role
writer stages a 512-byte chunk, cuts it back to the last NL when the input
continues, pushes what fits and room-waits for the rest — so a ring-fitting
write, which is every console line, is whole against every producer. The
residual is named and Linux-equivalent: a long write spans chunks; a FULL ring
splits at a chunk boundary, because progress beats atomicity under congestion.

Three tests, one of them the tear's own witness: two kthreads hammer a STALLED
ring with 64-byte units from two CPUs, the ring is read back through a new peek
hook and parsed as frames, and every frame must be one producer's unit — with an
overlap witness so the test says whether the interleave was exercised (it was).
The other two pin the boundary deterministically on one CPU: room = len-1 moves
the count by zero and `dropped` by exactly len; room = len lands whole.

### The bar over the tip (`277b02cc`: the round close + the TX-ring unit)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves — the kernel byte-changed, so the whole
matrix re-ran); LS-CI 33 PASS + 2 SKIP (GL not baked; six batches, TCG); suite
1408/1408 (`920bbfca`) and 1411/1411 (`277b02cc`) per commit; sabotages
SF1/S16/S17/SP5 (the round close) and S1–S3 (the unit rule) each red on the
named check. Pushed to both mirrors after the fixup.

A number corrected on the way: three earlier bar stanzas and four status rows
said "LS-CI 34 PASS + 2 SKIP". Every bar today measured 33 + 2 over the same 35
scenarios, and so did the two before it; the 34 came from the c8ab2744 close's
"36 scenarios" — an `ls tools/interactive/*.exp` count that included `lib.exp`
— minus the two SKIPs. A derived figure propagated as a measured one, six
times, before a run's own tally was set beside it. The tally is now taken from
the harness's `==> LS-CI:` lines only.

## 2026-08-17 (aux) — the handler-time mask is Linux's; three socket findings; a file count that was not a scenario count

Item 7 of the notes line was the smallest thing on the queue and the only one
without a vote in front of it (the #237 `pipe` default and the socktab posture
both alter user-signed scripture), so it went first while the votes ride the
report. The d3a11c8e round had recorded two permissive-direction divergences:
delivery never applied `sa_mask | sig` while a handler ran — N-3's blanket
`in_handler` guard stood in for it — and `rt_sigreturn` did not restore
`note_mask`, so a handler's own `rt_sigprocmask` outlived the handler, and an
`execve` from inside a handler handed the image the PRE-handler mask where
Linux hands it mask | sa_mask | sig.

The change is three lines and a field. `notes_deliver_linux_locked` saves the
pre-handler mask into a new `Thread.note_saved_mask` and stores Linux's
`signal_delivered` value — mask | sa_mask | sig, sig omitted under
`SA_NODEFER`, both additions through the same coarse translation as
`rt_sigprocmask` (a tty-family `sa_mask` entry blocks the family; SIGKILL is
dropped); the phenotype's `rt_sigreturn` restores the saved mask, gated on
`t->proc->phenotype` because a PHENO_LINUX Proc reaches delivery only through
the Linux path and a native Proc never does; and the fork-from-inside-a-handler
copy from the round's F1 gained the field — the round's own lesson, "enumerate
what the restore reads", applied to the next field. Delivery is untouched: the
guard still holds every note for the handler's duration (VIVARIUM 6.22's stated
conservative imprecision), so what changed is the mask a handler OBSERVES and
PASSES ON. The frame's `uc_sigmask` still carries the pre-handler mask and is
written for reading — a handler that edits it changes nothing, which Linux
would honour; recorded as the conservative-direction divergence of this frame
design. Native `noted` keeps the as-built rule.

Two things the witness taught. A signal with no note (SIGUSR1/2) reads back
CLEAR whatever is blocked — the translation has nothing to set — so a
`sa_mask = {SIGUSR1}` witness would have proved nothing; the legs use SIGINT,
SIGCHLD, SIGWINCH and SIGPIPE, one note bit each. And the pre-handler mask is
{SIGCHLD}, non-zero on purpose: a restore that puts back ZERO is
indistinguishable from a correct one against an empty pre-handler mask, and
the fork leg (the child forked from inside the handler restores at ITS
sigreturn) is exactly the leg a missing copy would pass with zero. The Thread
grew by a u64 and its size did not change — the 8 bytes landed in the pad
before the 16-aligned FP area — and that was measured with
`-fdump-record-layouts` before the size assert's message said so, not derived.

The first boot reddened the "handler's own block undone" leg on a WORKING
restore, and the reason is the reusable part: probe leg L26, far above, blocks
SIGWINCH to assert the tty family's honest over-report — and nothing since
unblocks it. So the pre-handler mask carried the tty bit, the restore put it
back exactly as it should, and the leg read that as "the block persisted". A
premise assumed is a premise that can be false without anyone's fault; it is
now asserted as its own leg (L237: the pre-handler mask is exactly {SIGCHLD}),
with the tty family unblocked first and re-blocked after so the legs below run
under the state they always had.

Sabotages, each red on exactly its named check: SM1 (no handler-time store) →
probe L239 (the mask inside lacks sa_mask|sig; 1413/1413); SM2 (no restore) →
`notes.phenotype_sigreturn_restores_mask` leg A (1412/1413 — the suite fails
first, so the probe is not reached; L240/L241 had already shown they
discriminate, on the premise failure above); SM3 (the fork copy skips the
field) → probe L244 only (the child forked from inside the handler restores
zero, and zero is not {SIGCHLD}).

### Three socket findings, from reading before touching

The socktab item (fork does not clone it) was researched instead of started,
and the research moved it. The enqueued plan said "a refcounted entry"; a
refcounted ENTRY cannot carry the ctl->data handle swap `connect` performs in
one table, so it reproduces Linux no better than a per-process copy — and a
per-process copy is Plan 9 APE's own posture (rocks live in process memory;
fork copies them). Every fork shape that occurs (accept-then-fork,
prefork-accept) works under a copy; the divergence — a state mutation through
one alias not seen through another — is the one LINEAGE already published for
dup3. VIVARIUM 5.5.2 states today's "not rfork-inherited" as design, so the
flip is the operator's vote (`memory/design_socktab_across_images.md`).

Alongside it, two defects verified in the tree. `handle_close_on_exec` closes
a close-on-exec socket handle and pays no socktab drop, and `fcntl(F_SETFD,
FD_CLOEXEC)` is a served row — so `socket; fcntl; execve` leaves a stale
(proto, N) entry keyed on a number the new image's next fd-creating call is
handed: the "dial verb to a stranger" class the V-5 header names as the
sharpest this table can have, reached through exec rather than dup. And the
reach is wide, because of the third finding: `socket()` answers EINVAL for
`SOCK_CLOEXEC|SOCK_NONBLOCK` "rather than masking them off", and EINVAL is
exactly musl 1.2.5's fallback trigger (`third_party/musl/src/network/socket.c`):
it retries without the flags, then issues `fcntl(F_SETFD, FD_CLOEXEC)` — served,
so every musl `SOCK_CLOEXEC` socket reaches the stale-entry path — and
`fcntl(F_SETFL, O_NONBLOCK)` — unserved, ENOSYS, and musl ignores the result. The
guest ends up holding a BLOCKING socket it believes non-blocking, the very
failure the refusal's comment says it prevents. A refusal is only as honest as
the libc that receives it; the claim was verified on the artifact, not on the
kernel's return value. Both enqueued (memory + AUX-ROADMAP), main told
(V-5 is theirs).

Also to verify, not yet verified: holotype R5-F9 (longjmp out of a handler
wedges `in_handler`) was registered against pouch programs, but busybox ash's
`raise_interrupt` longjmps out of the SIGINT handler when interrupts are
enabled, and the phenotype population is every musl-static shell. One VM
experiment settles it; if real it is a P1 for interactive shells and needs an
abandoned-frame rule (design).

### The count that was a file count

The push bar over `277b02cc` measured LS-CI at 33 PASS + 2 SKIP; the record —
three JOURNAL stanzas, four status rows, this session's own resume note — said
34 + 2. Every bar today measured 33 + 2 over the same 35 scenarios, and the two
full runs before them said "32/34; 2 SKIPPED" in the harness's own words. The
34 was the c8ab2744 close message's "36 scenarios", an `*.exp` count that
included `lib.exp`, minus the two SKIPs: a derived figure that propagated as a
measured one six times before a run's tally was set beside it. Corrected
everywhere; the tally now comes from the harness's `==> LS-CI:` lines only.

### The bar over the tip (`01f076f2`: the handler-time mask)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves — the kernel byte-changed); LS-CI 33 PASS +
2 SKIP over 35 (GL not baked; six batches, TCG); suite 1413/1413; sabotages
SM1/SM2/SM3 each red on the named check. Pushed to both mirrors after the
fixup.

---

## 2026-08-16 — Warp-C C-1, the per-slot decision, and one third of the extinction tear

Resumed from a self-compaction at the 600k checkpoint. **The nudge fix worked
on its first live test** — the detached watcher fired behind `/compact` and the
far side woke itself, which is the loop the operator had been closing by hand at
every boundary.

### Warp-C C-1 — the composed present, modelled (`ee581fbd`, fixup `ae9a25df`)

GPU-DESIGN §4.5.6 is binding here: `tapestry_present.tla` is model-first, so the
model is extended *before* the impl. Added the GPU-composed present behind
`ALLOW_COMPOSE` — `Attach`/`Detach` (P1b's authority-conferral point),
`ComposeBlit`/`ComposeComplete`, `DrainedOfBlits` on `ServerRelease` + `Free`,
and two invariants repeating T-1's own LIFETIME/CONTENT split: `NoTornCompose`
and `NoStaleCompose`. Eleven cfgs, gated by the new `specs/check-tapestry.sh`.

**The control was set before the work, which is the only reason it meant
anything.** I recorded every cfg's distinct-state count *before* touching the
module, so "this extension is additive" became checkable: with `ALLOW_COMPOSE =
FALSE` the six pre-existing cfgs must reproduce 5413 exactly. They do — and the
check earned its keep, catching that tracking `filled` unconditionally cost the
direct path 5413 → 10413 states.

**Two measurement traps, both mine, both caught by controls rather than by
reasoning:**

- My first comparison harness reported all six cfgs as DIFFERING. The harness
  was broken (`set --` inside the loop clobbered the positionals, lagging every
  expectation by one row). But under the bad labels the raw numbers still said
  something real, and chasing *that* was the right move.
- The buggy cfgs genuinely did differ — and it turned out **the metric was of
  the instrument**. A buggy cfg halts at the first violation, so with parallel
  workers "states explored before tripping" is scheduler noise: measured
  129/141/155 across three *identical* runs. Buggy cfgs are now judged on exit
  status plus the *name* of the invariant reported. (Never on TLC's prose — it
  writes both "is violated" and "was violated" depending on property kind.)

**Then TLC refuted my model, and the tree refuted the premise under it.** I had
carried the in-flight blit as the *slot* it reads, reasoning that a client
filling a *different* slot during a composition is legitimate pipelining — and I
wrote that justification into the module header as though it were established.
It is false. `usr/tapestryd/src/gpu.rs:1515-1518`: tapestryd allocates one 2D
resource per surface, attaches the whole weave as backing, and transfers at a
per-present *offset* that selects the slot. Guest-side slots buy **no** host-side
concurrency. The guard also had the shape of a known trap — `intransfer = 0` is
a gauge reading zero, equally true of "the fill landed" and "no fill was ever
issued" — now closed by an explicit `filled`.

The exclusion is symmetric, so it gets a sabotage *per direction*
(`buggy_blit_during_fill`, `buggy_fill_during_blit`) rather than one flag opening
both gates, which would only ever demonstrate whichever end TLC reached first.

Non-vacuity was measured, not assumed: coverage shows the composed actions fire
`0:0` with the switch off and `ComposeBlit` 2264 / `ComposeComplete` 7328 with it
on, so the green sits over a constructed state.

**Verification:** 32 spec modules green + the 11-cfg tapestry gate. `corvus` and
`handles` deliberately not re-run — 87 minutes, and nothing `EXTENDS`
`tapestry_present`, so they cannot be reached by this change. Zero build inputs
changed (proved by `git diff --name-only`), so the full bar's other legs carry
from `ca50a164` by construction rather than by assertion.

### The design fork it forced — and the operator's vote (`14f8c1ed`)

C-1 surfaced an obligation **the prose did not have**: the D1 recycle gate does
not survive the composed path unchanged. In the direct path a present's terminal
CQE genuinely means "the host has finished reading" — until the compositor
becomes a second, async reader of that one host resource, at which point the CQE
stops meaning the resource is free and nothing in the old rule notices.

Researched before posing it (Wayland `wl_buffer.release` + `drm_syncobj`, Android
BufferQueue acquire/release fences, Fuchsia buffer collections), which showed the
SOTA answer is *two* mechanisms, not one: buffer-release semantics for software
clients, explicit fences for GPU ones. Posed the fork with that attached.

**Operator chose one host resource per slot (3×).** Landed as a scripture commit
with no code, per the design-conversation pattern: GPU-DESIGN §4.5.8, with the
two rejected alternatives and their reasons, and the cost stated rather than
buried (3× host VRAM; ~100 MB at 4K, against a 64-MiB weave cap that already
cannot hold a triple-buffered 4K weave). The landed model does not change with
the vote — `NoStaleCompose` is whole-generation, correct today and merely
conservative once slots become distinct host objects.

### The extinction tear — one third of it (`44a8d53f`)

A surfaced soundness defect outranks the perf arc, so I stopped C-2 and took
this. The `EXTINCTION:` ABI line is emitted as four separate unlocked
`uart_puts` calls; every consumer anchors its match (`^EXTINCTION:` in
`tools/test-fault.sh`, and bare-token matchers elsewhere). A torn banner is
therefore not cosmetic — it is **a real extinction the harness cannot see**,
fail-open on the one channel the whole test discipline trusts.

**The vault already carried an adjacent seam, and I nearly conflated them.**
There are **three** tearing sources with confusingly close names:

1. extinction vs extinction — the re-entrancy guard is per-CPU *by design*, so
   two dying CPUs both print. **Fixed** (`extinction_claim_console`).
2. extinction vs a peer's *normal* console write — the vault's
   `seam-extinction-line-unserialized`. **Open.**
3. `IPI_HALT` — would subsume both. **Open**, a commented-out reservation.

The fix is one `__atomic_exchange_n`: a raw atomic rather than a kernel spinlock
(this runs on a dying machine, often inside a fault handler, and a primitive
carrying lock-order assertions could itself fault), try-once rather than spin
(the winner never releases, since every path ends in `_torpor`), and losers park
emitting nothing — because the failure modes are asymmetric: a torn line can be
read as a clean boot, a missing one leaves the guest visibly hung. Take the loud
failure.

**The fix introduces its own fail-open, and that is what most of the design
guards.** Nothing releases the console, so anything claiming it spuriously
silences every later extinction in the boot — the same defect from the other
side. Hence the deliberate interface split: the claim core is exported to be run
on a *caller-supplied* word, and nothing exports a way to claim the live one. A
test that took the real console would disable extinction reporting for every
test after it, silently.

**Both new tests were sabotage-verified** (1367/1367 → 1365/1367, each failing
on its own distinct assertion message). And the first one is documented for what
it does *not* cover: it is sequential and the property is a race, so a non-atomic
`if (*w) return 0; *w = 1; return 1;` passes it identically. Covering the real
regression needs a multi-CPU fault-injection arm with a **forced** interleaving —
without forcing it the pre-fix build garbles only sometimes, and a discriminator
that fails only sometimes is not a regression test. Tracked, not skipped quietly.

Also corrected a phantom that had propagated into two files: both
`kernel/extinction.c` and the header told readers to co-update
`tools/agent-protocol.md`, which was planned in Phase 1 and never written, and
`tools/run-vm.sh`, which matches neither literal because it only launches QEMU
and never reads boot output. Both now point at the vault's `abi-boot-banner`
mirror set instead of a transcribed list.

**Verification (the full bar, since this is a kernel change):** build clean;
suite 1367/1367 (was 1365; +2); SMP gate 40/40 with 0 corruption across
default-smp4/smp8 + ubsan-smp4/smp8; LS-CI 35/35 PASS; v8.0 floor OK.

**A killed gate is not a green gate.** The first LS-CI run was stopped by the
harness (`Terminated: 15` on its scenario subprocesses) after I ended a turn
while it ran; the SMP gate had survived the identical foreground → background
migration earlier in the same run, so what differed was ending the turn. Re-run
as a tracked background task, staying in-turn.

**And then I got the reasoning for that right conclusion wrong, twice, the same
way.** I first wrote that the killed run "recorded zero verdicts", inferring it
from a stdout log containing only `==> start:` lines. Then, waiting on the
re-run, I read the same channel and concluded it had produced no results after
eight minutes. Both readings were of the wrong channel:
`tools/test-interactive.sh` says so in its own comment — *"The verdict is a
FILE, not a counter"* — and writes results to per-slot `timings.tsv`, never to
stdout. The re-run was healthy the whole time (`go8d PASS` already on disk).

So: **a pattern that matches the wrong thing returns a confident wrong answer,
never an error** — a lesson already pinned in memory, re-learned twice in one
hour on one command. What makes it worth writing down again is that the wrong
instrument produced a *plausible* story both times (a killed gate really had
been killed; a slow gate really can be slow), which is precisely why it was not
self-correcting. The fix is to find where a tool actually writes its verdict
before reading any verdict from it.

### Before C-2 wrote a line: the composed path cannot run on the dev loop

Checked the precondition rather than assuming it, and it changed the arc. The
boot log of the very run I had just gated says
`tapestryd: gpu up -- 1280x800, pci intid=35, virgl=0 capsets=0`, and
`tools/run-vm.sh` defaults to `virtio-gpu-pci` — a device with no GL. So
`CTX_CREATE` / `RESOURCE_CREATE_3D` / `SUBMIT_3D` are unavailable on the primary
dev loop, and with them every mechanism §4.5 describes.

Three consequences, recorded as GPU-DESIGN §4.5.9. C-2/C-3 must be verified on
**thyla-pi**, not here. The composed path must be capability-gated on the
negotiated feature bit — a tapestryd that assumed GL would take the console dark
on the default device. And the third corrects the roadmap: **"C-4 retire the
readback path" cannot mean delete it.** That is forced twice over — by the plain
`virtio-gpu` that is the default here, and more fundamentally by bare metal,
where there is no virtio-gpu at all and virgl is a *virtualization* transport
with nothing to negotiate. The CPU path is the universal one; GPU composition is
the accelerated path where a GPU seam exists.

The cost is stated rather than left to be discovered: tapestryd carries **two
composition paths permanently**, and they must stay behaviourally identical from
the outside or the gate that proves one is silent about the other.

### The C-2 verification host, proven rather than assumed

Having established the dev loop *cannot* run the composed path, the next
question was whether anything can. Synced HEAD to thyla-pi (all 80 pool chunks
hash-verified, artifacts paired) and booted `virtio-gpu-gl-pci` under KVM on
real V3D:

```
tapestryd: gpu virgl -- num_scanouts=1 num_capsets=2
tapestryd: gpu capset[1] id=2 max_version=2 max_size=1384
tapestryd: gpu up -- 1280x800, pci intid=35, virgl=1 capsets=2
CAPSET GATE: VERIFIED
```

So C-2 has a working verification host, and the two figures — `virgl=0` here,
`virgl=1` there — are the whole argument for §4.5.9 in one line each. Worth
doing before the implementation rather than after: had C-2 been written first,
its first symptom on the dev loop would have been a dark console, which is a
long way from its cause.

### C-2a — the capability gate and the compositor context

The first landable piece of C-2: a reserved compositor virgl context
(`COMPOSITOR_CTX = 0x100`, far above the client `slot + 1` range so a client's
stream can never author against the screen), minted only where `virgl`
negotiated, and a startup line reporting which composition path the host can
actually take.

**The first cut reported nothing, and the boot passed anyway.** I had hung the
posture report off `ensure_screen`, beside the other display resources — but
`ensure_screen` runs only under `Scanout::Composed`, a state a normal boot never
enters, so the line sat behind an unconstructed state and printed on neither
host. The suite went 1367/1367 with the feature effectively absent. Which
composition path is *available* is a property of the HOST, fixed at feature
negotiation, so it now reports where the host is brought up.

**Verified on both arms, differing in exactly one variable** — a negative
assertion alone would have been satisfied by a broken fixture:

| Host | Negotiation | Posture |
|---|---|---|
| dev loop, `virtio-gpu-pci` | `virgl=0` | `composed path = CPU (virgl=0)` |
| thyla-pi, `virtio-gpu-gl-pci` | `virgl=1 capsets=2` | `compositor ctx 256 up` → `composed path = GPU` |

Getting the positive arm took one correction of its own: the `capset` verb
filters its output at the capset markers, so the Pi run *looked* like it lacked
the line when it had simply not been shown it — `boot-probe.sh` keeps the full
log on the host, and the line was there. A truncated capture and a missing
feature are the same reading until you check which one you have.

### C-2b — the 3D screen, landed gated and HONESTLY UNPROVEN on its own arm

The screen becomes a host-side 3D resource attached to the compositor context
where GL exists, falling back to the 2D resource everywhere else. Guest backing
stays on both paths, because at C-2b the screen is still CPU-filled — only its
host-side representation changes. `screen_push` grows a 3D arm, and there the
sync transfer moves the whole surface rather than the damage rect: a deliberate
trade, since C-3 deletes the CPU fill outright and building a rect path for a
mechanism already scheduled for removal is waste.

**What is verified, and what is not — stated because the gap is the finding.**
The FALLBACK arm is verified: suite 1367/1367, and LS-CI 35/35 where the
`ls-gfx` scenarios assert exact pixels via screendump and therefore cannot pass
without a working composed screen. **The 3D arm has never executed.**
`alloc_screen` runs only under `Scanout::Composed`, and neither the dev-loop
boot nor the Pi's `capset` boot enters it, so `screen res N 3D (compositor ctx)`
printed on neither host. `prove` produced no new boot log to grep.

So this lands **gated off on every host I could exercise** — dead on the dev
loop by capability, unproven on the Pi by opportunity — and the commit says so
rather than calling a clean boot a verification. Booting green proves the gate
did not fire, which is exactly what an `if (false)` would also prove.

**Then I found why, and it is a tooling gap rather than a code problem.** The
Pi logs say `tapestryd: scanout direct 0 (1280x800)`: every existing Pi verb
drives a SINGLE display-sized GL client, and that takes the **Direct** path —
scanning out the client's own resource and bypassing the compositor screen
entirely. §4.5.1 spells out the condition: Direct demands one visible surface
AND one visible leaf AND an exactly display-sized surface. So composed scanout
needs two surfaces, or one smaller than the display, and **no verb in
`warp-host.sh` produces either.** `capset` and `smoke` both land in Direct;
`tri` and `prove` left no new boot log at all.

That is worth more than a failed check: it says the composed path — the entire
subject of the Warp-C arc — has no driver on the only host that can run its GPU
half. Building one (two surfaces, or a mode change that un-sizes a single one,
which is what `ls-gfx-mode` does locally) is the next task, and it gates C-2b,
C-3, and the arc's exit criterion alike.

### The driver — C-2b's 3D arm finally executes, and my own note was wrong

The task I left myself was "build a Pi driver that forces Composed scanout."
Before building anything I checked the claim under it, and **it was false**. The
section above says "no verb in `warp-host.sh` produces either" — but
`glq-virgl.exp`, which `quake` runs, opens GLQuake in a window and its very
first assertion is `-re {scanout composed \((\d+)x(\d+)\)}` with the label
"composed entry (two leaves)". `decomp` and `wedge` split the layout too. What
was actually true is narrower and duller: the verbs I had *read the boot logs
of* — `capset`, `smoke` — boot with no client at all, so aurora alone is
display-sized and lands in Direct. I generalised from the two logs I had to a
claim about all ten verbs, and wrote it into two documents.

Worth noting how cheap the catch was: one grep for `composed` across
`tools/warp/*.exp`, run because the note asserted a negative over a set I had
not enumerated. **The evidence that a thing is absent has to come from the whole
set, not from the members that happened to be in front of me** — and a note
written confidently at a compaction boundary is exactly where that error
survives, because the far side inherits it as established fact.

I still did not use `quake`. It drags in the pool's `tyr-glquake`, S3TC quirks
(#216), the #198 storm, and 900-second timeouts — a lot of machinery that can
fail for reasons having nothing to do with C-2b. `/bin/tapestry-battery` brings
up two surfaces, lives in the ramfs, and needs no GL of its own, so **the only
GL object in the experiment is the compositor's own screen**. That isolation is
the reason to pick it, not availability.

`tools/warp/composed-screen.exp` boots, takes the posture line between boot and
login (it prints at bringup, which is where a host property belongs — a lesson
this arc already paid for), runs the battery, and asserts the screen mint. **The
control is the device**, which is why the scenario takes one as a parameter
instead of hardcoding the GL model: two legs, one host, one variable, each
asserting the other's outcome is wrong.

```
virtio-gpu-gl-pci -> composed path = GPU -> screen res 67 3D (compositor ctx) (1280x800)
virtio-gpu-pci    -> composed path = CPU -> screen res 67 2D (1280x800)
```

**C-2b's 3D arm has now executed**, on real V3D silicon through virgl. The
second line is what makes the first mean something: a GL-only leg would pass
identically against a tapestryd that ignored the negotiated bit and always
minted 3D. Two legs that *disagree* are stronger evidence than two that both
pass — the control produced a different answer rather than merely staying quiet.
Both legs minting `res 67` is a small corroboration on the side: everything
upstream of the branch is identical, so the arm is the only thing that moved.

The gate keeps two claims separate rather than collapsing them — posture matches
the device, screen arm matches the posture — so a host that had silently lost
its GL could not satisfy the second by making both sides equally wrong. And
`tools/warp-host.sh composed` requires each leg's scenario-completion line as
well as its screen line, because a leg that died immediately after printing its
screen line would otherwise still show the gate everything it greps for. That
term is not hypothetical caution: the `reject` verb in this same file shipped
grepping `C0-REJECT` while its producer printed `C0-DETECT`, and exited 0 on the
exact failure it existed to catch.

### Then C-2d refuted itself before it wrote a line (§4.5.8a, OPEN)

With the driver landed I went to implement §4.5.8 — the per-slot host resources
the operator voted for — and read the present path first. The decision does not
survive it, for a reason nobody had in view at the vote.

Three facts, each one grep:

1. Every client rotates slots on every present: `cur_slot = (cur_slot + 1) %
   nslots`, `libtapestry/src/lib.rs:525`, unconditional, both scanout modes.
2. Nothing copies content from slot *N* to slot *N+1*. `pixels()` hands back
   the raw current slot; there is no carry-forward anywhere.
3. **The single per-generation host resource is therefore doing a job nobody
   wrote down: it is the accumulation buffer.** A damage-only present transfers
   only its rect, so the host resource keeps the rest of the previous frame and
   the stale guest slots never reach the host.

Give each slot its own host resource and that job has no owner. A damage-only
present would render a three-frames-stale background around each fresh rect —
in Direct immediately, and in Composed at C-3. And the client this lands on is
**aurora**: it repaints only rows `r0..r1` and presents that rect
(`aurora/src/main.rs:1027-1038`), and it is the default Direct client on every
boot. The very line I have been reading all session, `scanout direct 0
(1280x800)`, is that client.

What makes this worth recording is not the catch but where the load was.
§4.5.8's analysis compared 3× / 2× / 1× VRAM and serialization — a complete
comparison of the properties anyone had *named*. The single resource's real
function was invisible because nothing declared it; it was an emergent
consequence of "transfer only the damage rect", and it had been load-bearing
for the console for as long as the console has existed. **A design comparison
can be sound over every property you listed and still miss the one the code is
actually relying on.** Only reading the path surfaces those.

I recorded it as **§4.5.8a** with four options rather than picking one, because
the vote is the operator's and this changes the terms they voted on. The
recommendation is buffer age — `EGL_EXT_buffer_age` and Wayland's
`wl_surface.damage_buffer` exist for this exact problem, Android's BufferQueue
exposes the same, and it keeps the per-slot vote intact at no VRAM cost while
retiring the latent hazard instead of routing around it. C-2c and C-3 both wait
on the answer: every option changes what gets attached and what gets blitted.

### The vote, and C-2d-a (`0a0e0fbb`, `931bf15a`)

The operator picked buffer age. Implementing it immediately hit a constraint the
option sketch had assumed away: I had written "present CQE now carries: age",
and it cannot. A present is a 9P write over the Loom ring, so its CQE is
**kernel-owned** — `result` is the write's byte count, `flags` is `LOOM_CQE_*`,
and `struct loom_cqe` is `_Static_assert`-pinned at 16 bytes. Putting a
compositor payload there is a kernel ABI break for a compositor convenience.

The way out was to notice who already owns the information. `libtapestry` owns
the rotation — `cur_slot` advances only after a present's own CQE — so it knows
exactly when each slot was last presented and can derive the age itself. A
`TEV_AGE` event was rejected (async to the present, so it races the rotation) and
a control word in the weave was rejected (a client-visible layout change for
something the client can compute).

**The interesting part is what the derivation costs, because it is the same
trap again.** A derived age is correct only if the client hears about every
server-side invalidation — which is exactly the kind of undeclared dependency
that produced §4.5.8a two hours earlier. So it is written down as a named
invariant this time rather than left to be rediscovered: tapestryd must not skip
a transfer without the client subsequently getting a redraw request, and a
redraw invalidates **every** slot, so the client repaints full for `nslots`
presents, not one. Both arms are wired in `libtapestry`.

Then aurora handed back independent corroboration of §4.5.8a. `main.rs:988`
already routes any OSD pass through the full-frame branch, with the comment
*"a partial rect could transfer stale panel pixels from an older slot"*. The
symptom had been understood locally, for one widget, and worked around — the
general statement just never got made. That is what an emergent load-bearing
property looks like from the inside: not unknown, merely un-generalized.

I split the chunk, because the halves are not symmetric: per-slot resources
without age break every accumulator, but age without per-slot resources is inert
and harmless. So the client half went first — and **its honest gate is that
nothing changed.** `ls-gfx` PASS, `ls-gfx-panes` PASS (exact pane-centre
pixels), suite 1367/1367. Its actual effect is unobservable until C-2d-b removes
the accumulator, and the commit says so rather than dressing a green boot up as
verification.

**Then I got the prerequisite list wrong, in the commit message, within twenty
minutes of writing the lesson that prevents it.** I swept for clients that
present partial damage with `grep 'present(Some\|present_rects'` and reported
three. That greps **API shape**, not the property that matters — *damage
smaller than the full surface*. Checked properly, it is one:

- `tapestry-battery` needs **nothing**. Every present is `present(None)`, and
  its one `present_rects` tiles the whole surface with two rects after writing
  every pixel. Its own header says so: *"presents FULL-FRAME only."* I had
  called it "the one with teeth."
- `tapestry-demo` is the real one, and is the sharpest example in the tree: it
  paints the quadrant background **into slot 0 only**, at frame 0, then draws
  just the plasma box into *rotating* slots forever after. Slots 1 and 2 never
  receive the background at all — they hold alloc-time zeros. Under per-slot
  resources, two frames in three would show black around the plasma.

"A pattern that matches the wrong thing returns a confident wrong answer, never
an error" is pinned at the top of my own memory index. It still went into a
commit message, a scripture section and the handoff, because a grep that
*returns results* feels like a sweep that *finished*. Corrected in §4.5.8b and
the handoff; the commit body stands as written, with this as its correction.

### The stop hook guarded the wrong stop, and the guard was never needed (`b3632942`, `cd0b3390`, `b61ca929`)

The operator noticed the Stop hook fired once in the long run and then went
quiet at a second stop it should have caught, and asked aux and me to work out
why. It is the third instance this week of the same family, and the sharpest.

**The measurement.** Replaying the hook's own parser over the real 805 MB
transcript: the silent stop sat at **530k / 73 turns** — inside the window on
both axes. So "it was correctly silent above the checkpoint" is dead. Isolating
the logic with synthetic input showed it behaves exactly as written. The cause
was upstream, and the pattern repeats: every firing is followed by silence for
the rest of the continuation, re-arming only when the user speaks or a
compaction lands.

**What I got wrong, and it was not the code.** `stop_hook_active` means "this
hook already triggered a continuation" — per-continuation by definition. I
exited early on it, which made the hook a once-per-*run* nudge guarding the
first stop and nothing after, i.e. the stop most likely to be earned and none
of the ones that follow. I kept that early exit because I believed it was the
loop guard.

**It never was.** aux fetched the contract: the harness overrides a Stop hook
after **eight consecutive blocks** (`CLAUDE_CODE_STOP_HOOK_BLOCK_CAP`). The
belay already existed one level up. So I had built a guard against a loop
something else was already preventing, and paid for it with the exact behaviour
the hook exists to provide. That is a different failure from a bug: **the code
did what I meant; what I meant rested on a contract I had not read.** No amount
of testing my own intent would have found it — only reading someone else's.

**The instrument came before the fix, and earned it twice.** The hook had nine
silent exits, so "correctly silent", "suppressed", and "crashed" were one
observation and any diagnosis could only be a guess — the same shape that had
just cost the vault a stranded day. So a ledger row on every path landed first.
Then it caught two things I would not have:

- Its own blind spot: the `stop_hook_active` parser printed `"1"` on exception,
  so a malformed stdin logged as `silent-stop-hook-active`. **The instrument
  built to separate those two causes could not separate those two causes.** The
  malformed-stdin test leg printed the wrong row, which is the only reason I
  looked.
- On its first *real* output: three rows in 24 seconds with incoherent context
  jumps, because the ledger is shared by main/aux/vault and I had dropped the
  session field from aux's spec. An interleaved log with no writer is worse
  than no log — it invites a confident reconstruction of one impossible session
  out of three real ones.

**And the fix validated itself in production before I finished writing it up:**
the reworded stem ("fires once per stop") came back in a live firing that
re-armed mid-continuation after real work — something the old version could not
do — with the ledger row `588458ctx/44t/27b/flag1` showing exactly why.

### C-2d-b landed, and the sabotage that proved it unverified (`f86177b6`)

The server half went in as voted: each generation mints `WEAVE_SLOTS` host
resources instead of one, backed per-slot instead of whole-weave. The
consequences were all followed rather than found later — `res_stale` becomes
per-slot; Direct binds the presented slot's resource and therefore rebinds every
frame (a KMS page flip, carrying the #57 post-bind flush); transfer offsets lose
their slot base, which the compiler confirmed by reporting `slot_stride` newly
unused; retire and `release_gen` unref all three or leak two per surface in the
process that IS the console.

`Held::Direct(Rect)` was the one that needed design rather than editing, and it
is why I stopped the first attempt at it. A rect union is well-defined only
while every held present lands on one resource; presents rotate slots, so two
held presents sit on different resources and `release` must flush each against
its own. Now `[Rect; WEAVE_SLOTS]` — bounded by construction, since a client
cannot hold more presents than it has slots.

**Then the sabotage passed, and that is the result worth the whole chunk.** I
disabled aurora's age handling with per-slot resources live — `stale_slot =
false`, `back = 0`, exactly the pre-C-2d-a client against a non-accumulating
server — and **`ls-gfx` still reported PASS.**

So the two gates I had been treating as verification are not. `ls-gfx` asserts
the frame *looks like* a console and that dumps *differ* after a command;
neither notices a stale background around fresh rows. `ls-gfx-panes` drives the
battery, which presents full-frame only and never exercises the accumulator path
at all. Between them they cover everything about the compositor **except the
property C-2d changes.**

That is the same trap as C-2b at the start of this run — a green result that
proves the gate did not fire — except this time I was the one about to be
fooled by it, having written the C-2b version into scripture that morning. The
difference between the two is not insight, it is that I ran the sabotage. Had I
not, this would have landed as "green on both pixel gates", which is *true* and
means nothing.

C-2d is therefore **implemented, not verified**, and the commit says so. §4.5.8c
records what the missing gate has to do: paint a region, damage a *different*
region, rotate all slots, sample the first region. `ls-gfx-panes` already has
the sampling machinery, so it is a scenario to write, not an instrument. The
focused audit is owed too — `usr/tapestryd` is an I-40 trigger surface and this
is the live scanout path — and could not run here because agent spawning is off.

### The self-compaction slot had two keys that did not agree (`7061115a`)

aux found this by reading the ledger nobody reads, and it is the best kind of
find: the mechanism had been quietly half-broken since it was built, and the
evidence had been sitting in a file the whole time.

`~/.claude/thyla-selfcompact/log.tsv` has vault's `allow` at 2026-08-16
10:44:32Z with **no `consumed` and no `nudge`**, and its `.note.pending` still
in the slot dir a day later. Every `main` row is paired; only vault's is
orphaned. That session compacted itself and was never handed its own resume
note — it sat at a prompt for the rest of the day.

The cause is a key mismatch, and **the comment is the interesting part.**
`tools/thyla-selfcompact.sh` said, in as many words: *"Two independent
derivations of one key, no shared config to drift."* The producer keys on `git
rev-parse --show-toplevel`; the consumer on `basename(dirname(transcript))`,
which is where the session was **launched**. Those coincide for main and aux
and do not for vault, which is launched from the thylacine tree and works in
thylacine-vault. So the comment **named the hazard and then asserted it away**,
and that assertion is what kept it unexamined for the mechanism's whole life.
It is every "keep these in sync" note that has ever rotted, except this one had
the confidence of sounding like an argument.

The fix needed no new identity, because one was already there and unused: the
arming script has always stamped `pane=$TMUX_PANE` into the meta, and a hook is
a child of the same claude, so it reads the same value. Pane match first, path
key as fallback.

**But the half that mattered was the silence.** The old failure was not doing
the wrong thing — it did *nothing*, and left no evidence, so `allow` without
`consumed` was the only trace. There is now an `orphan-note` row whenever a
pending slot goes unmatched, plus a 30-minute staleness discard.

**Then the test caught a bug in the fix that was worse than the bug.** The
first age check used `time.mktime` on a UTC stamp — `mktime` reads a
`struct_time` as *local* — so a note stamped that same second measured as an
hour old and was **discarded**. In any non-UTC zone that breaks every
legitimate resume: the repair would have converted a vault-only silent miss
into a universal one. I saw it only because leg 1 of the test printed
`stale-discarded` on a note written a moment earlier. Four legs, with legs 3–4
as the controls that make leg 1 mean anything — same note, same path-key
mismatch, only the pane varies:

```
1 pane matches, fresh    -> INJECTED,     consumed
2 pane matches, 25h old  -> not injected, stale-discarded
3 CONTROL no TMUX_PANE   -> not injected, orphan-note
4 CONTROL wrong pane     -> not injected, orphan-note
```

aux also retracted something in the same message, which is worth recording
because the retraction is worth more than the claim was: the "fourth
unregistered session" cited in the yip lease rationale **was aux itself** —
`ps -o ppid` on its own tool shell resolved to the process it had been reading
as a stranger. A census needs a control, and the control was its own identity.
Same family as `ps` matching its own command line, from the other end.

### Found in passing: `docs/REFERENCE.md`'s snapshot block died in Phase 5

The doc-update step sent me to `docs/REFERENCE.md` to refresh its Snapshot
block, which `CLAUDE.md` calls non-negotiable per chunk. **The newest "Tip"
bullet in it is a Phase 5 chunk** (`P5-stratumd-stub-bringup` audit close), and
there are 101 bullets behind it. The file's last commit of any kind is
`418688cf`, 2026-08-01. It contains **zero** occurrences of "Warp", "Tapestry",
"Clade" or "PTY-" — three whole arcs and a subsystem that do not exist as far as
the as-built technical reference is concerned.

So a binding per-PR obligation has been quietly unmet across roughly two phases,
including by me, several times this week. It is the "*a status field whose flip
is nobody's step stays unflipped*" shape: every chunk's author is told to
refresh it, no chunk's work makes them, and nothing fails when they do not.

**I deliberately did NOT patch my own bullet onto the top.** A dead list with
one fresh entry reads as maintained, which is worse than one that visibly
stopped — the reader trusts it again. The real question is what that block is
*for* now that `docs/phaseN-status.md` carries per-chunk rows and this journal
carries the narrative; answering it is a scripture-shaped decision, not a doc
edit to slip into a tooling commit. Enqueued rather than fixed in passing, and
enqueued in memory because the tracker is down this session.

### The gate that sees C-2d, red under both sabotages — and the defect building it found (after the self-compaction at `a733402e`)

Resumed from my own note with one instruction: build the §4.5.8c gate on aurora
in Direct, and validate it by re-running the sabotage that had passed `ls-gfx`
and requiring red. That is what happened, with two things the note did not
anticipate.

**The gate** (`tools/interactive/ls-gfx-age.exp` + `gfx_region.py`). Fill three
times with `yes … | head -n 200` so every slot carries glyphs; a POSITIVE
control — the same region assert, four keystroke-rotated dumps, each must show
text (a negative with no positive twin is satisfied by a broken fixture); then
`clear`, which blanks every cell in one all-rows present into ONE slot; then
eight rounds of keystrokes + dump, region exactly Bonfire, every pixel read.
The region is in cells (rows 6..rows-3, cols 2..cols/2) off aurora's own
`console up` line, so a font change moves it rather than breaks it.

**What the note left to the author, and how it was decided.** The detector is
slot-phased: the screen shows the slot presented LAST, so one dump samples one
slot. I had written "probabilistic — require N consecutive dumps". Working it
through, the honest model is *driven*, not sampled: each keystroke is a
row-0-only redraw, i.e. one present into the next slot, so the rounds advance
the phase deterministically plus whatever blink presents fall in the round.
That reframing exposed the real trap: **a broken client can have ONE stale
slot, not two** — an off-by-one in the union (`back = age-2`) leaves exactly one
— and the 1,2,3,1,2,3,… key pattern I first sketched (meant to break any
phase-lock with the blink) visits residues 1,0,0,1,0,0,1,0 under `b=0`: it never
reaches residue 2 and would pass an off-by-one every time. A plain one key per
round does reach it (1,2,0,1,2,0…) but is the pattern a 60 Hz blink can
phase-lock. So the negative leg types 1,1,2,1,1,2,1,1 keys, which visits all
three residues for *any* constant blink count per round (checked for b=0,1,2 in
the header); the
independence bounds — 3^-8 for the no-age class, (2/3)^8 = 3.9% for the
one-stale-slot class — are the fallback if the blink rate varies mid-leg, and
the header says which claim is load-bearing.

**Measured** (HVF, 128×36 cells, region 368 280 px). Fixed build: positive
63 882/368 280 non-bg on 4/4 dumps (identical counts — every slot holds the
same fill, as a correct client guarantees), negative **0/368 280 on 8/8**,
43 s. **S1** — the §4.5.8c sabotage, `stale_slot = false` + `back = 0`: **red
3/3 attempts**, at rounds 2, 1, 2 (63 882 stale px, i.e. the pre-clear fill
verbatim). **S2** — `back` off by one: **red 3/3**, at rounds 2, 5, 2. The
five-round attempt is the 1,1,2 pattern paying for itself: four dumps landed on
the two good slots before the fifth reached the one stale one. Restore green.
Both sabotages applied and reverted with `Edit`, and `grep SABOTAGE` empty
before the restore build.

**The defect the gate found — in C-2d-a, not C-2d-b.** Reading aurora's damage
branch to predict the sabotage outcomes, I traced what `931bf15a` records into
`dmg_hist`: **the WIDENED range** ("this is what actually reached the slot, and
the next union reads it"). That reasoning conflates *repaint* with *damage*.
The union answers "what changed since slot X was last presented"; what changed
between two presents is the dirty span, and the widening only says how much of
it THIS slot had to catch up on. Recording the widened range makes any
full-rows entry — every scroll — re-enter every later union, so every present
after it repaints all rows, forever. Aurora has been repainting the whole grid
on every cursor blink since C-2d-a landed: correct pixels, dead damage path.
Fixed to record the dirty span (`dirty0, dirty1` captured before the widening);
a full entry now falls out of the window after `nslots` presents. Two things
follow that are worth having in writing: S2 is a sabotage only against the
*fixed* recording — under the widened one an off-by-one is masked, since any
`back ≥ 1` propagates the full-rows entry (the old code had slack precisely
because it had no damage path); and the tight recording is guarded by the gate
that was built in the same chunk, which is the right order.

**Wrong turns, caught:** the first run failed on my own Tcl (`gfx_dump` takes
two args and I passed one) — three attempts, ~30 s each, all on the harness
side, before a pixel was read. And the resume note's "the sampling machinery is
in `ls-gfx-panes`" was true and unhelpful: `ppm-sample.py` reads one pixel; the
gate needs a region census with a positive control, which is a 40-line tool.

**Owed, unchanged:** the focused audit on `usr/tapestryd` (I-40; agent spawning
still off). The vault-owned prose (`sub-aurora`, `sub-libtapestry`,
`sub-tapestryd`) for C-2d and the recording fix goes over yip; the local
reference carries the gate.

### The device's OK was never the renderer's verdict — C-2b's "3D" word re-earned

Found while designing C-2c's gate, and by the one move that keeps saving this
arc: reading the source of the thing making the claim before repeating the
claim. My C-2c draft was about to say, for the third time in a week, that a
`CTX_ATTACH_RESOURCE` answered OK "attests the host accepted it". Before
writing that I fetched QEMU v10.0.0 `hw/display/virtio-gpu-virgl.c` (thyla-pi
runs 10.0.11) and read the handlers. **They ignore the `virgl_renderer_*` return
value** — for `CTX_CREATE`, `RESOURCE_CREATE_2D/3D`, `CTX_ATTACH/DETACH`,
`TRANSFER_TO_HOST_3D`, `SUBMIT_3D`, `CTX_DESTROY`; `ATTACH_BACKING` checks it
only to clean up the iov. `RESP_OK_NODATA` means "QEMU parsed it": nonzero,
non-duplicate id, valid iov. Only `SET_SCANOUT` (`resource_get_info_ext`) and
`RESOURCE_UNREF` (QEMU-side existence) consult anything.

**So three of my own documents were false in the same sentence.** C-2b's gate
header, `149-warp.md` and (by reference) the status row said the screen's "3D"
word was "the conjunction of four response-checked round trips the host
answered OK — a claim about the host accepting the object". Those four are
exactly the ignored ones. And it was not only prose: `alloc_screen`'s "a 3D
failure is NOT fatal — it falls back to 2D" was dead for a renderer-side
refusal — `is3d` reduced to `comp_ctx`, "3D" printed, and the failure landed
later, silently, as `INVALID_RESOURCE_ID` at the composed `SET_SCANOUT`, whose
result the code dropped after printing "scanout composed" *before* the bind.
The display would have kept the previous scanout, and the C-2b gate would have
said VERIFIED. #240 had measured this exact shape for `SUBMIT_3D` four days
earlier; the finding was filed against one command and never checked against
its family — the same lesson as the C-2d gate pattern that morning, one level
up.

**The repair is #240's own technique**: make the producer prove it with pixels.
`alloc_screen` writes 16 sentinel pixels into the fresh screen's backing,
`TRANSFER_TO_HOST_3D`s them through the compositor context, clobbers the
backing, `TRANSFER_FROM_HOST_3D`s back, compares, restores the zeros. Only a
resource the renderer holds, has attached to `COMPOSITOR_CTX`, and moves pixels
through can pass; a refused create or attach makes both transfers renderer-side
no-ops and the clobber survives. A refusal now falls back to 2D for real, the
screen line says why, the composed line prints after the bind with its verdict,
and `composed-screen.exp` grew a fifth term (the bound resource IS the minted
screen; the verb requires it on both legs).

**Measured on thyla-pi** (KVM, real V3D, boot-ms ~212 000), one variable —
the format the renderer will accept — two runs. *Sabotage*, `VIRGL_FORMAT`
`0x7FFF` in the 3D create: GL leg `screen res 71 2D (1280x800) -- 3D refused:
renderer round trip`, then `scanout composed (1280x800) res 71 bound` — so
`CREATE_3D`, `CTX_ATTACH_RESOURCE` and `ATTACH_BACKING` all came back OK from
the device under a format the renderer cannot accept (the reason would have
named the step otherwise), the renderer refused, the fallback was real and the
display got a working screen; the scenario went RED on the arm and the verb
reported three GATE FAIL terms; the non-GL leg was unaffected. *Clean*: GL leg
**`screen res 71 3D (compositor ctx) (1280x800)`** + `res 71 bound`, non-GL
`2D` + `res 71 bound`, all five terms, rc 0. The half that says the OLD code
would have printed 3D under the sabotage is inferred from the measured OKs and
the old boolean (`comp_ctx && create.is_ok() && attach.is_ok()`), not itself
measured — I chose not to spend a third Pi cycle on a one-line inference and
say so here.

**What this changes downstream**: `CTX_ATTACH_RESOURCE`'s response witnesses
nothing, so C-2c cannot be verified by its attach at all — its gate is P1b's two
arms in-guest (attach + one blit + readback; no-attach control red), which means
C-2c lands WITH the first blit witness. The C-2c design draft
(compositor-side import on host, bounded by hosting, no client verb — every
compositor in the prior art does it that way) is written and waits on that
correction; it goes into GPU-DESIGN as §4.5.10 with the next chunk.

### C-2c — the compositor imports what it composes, and the import is witnessed (after the self-compaction at `8c20b1f8`)

Resumed from the second self-compaction of the run (`8c20b1f8`, all pushed;
the note said "next is C-2c WITH its blit witness", and that is what this is).

**What C-2c is, in one line:** at `alloc_weave` tapestryd now
`CTX_ATTACH_RESOURCE`s every slot resource of a generation into
`COMPOSITOR_CTX`, and at `present-to` it imports the GL adoption's consented
BO — the client handing its buffer to the compositor is the whole grant, no
client verb (§4.5.10) — and every import is revoked BEFORE the resource's
unref on every death path (`release_gen`, `retire`, `wbo_retire`, `present-to
off`/replace, the consented surface's retire).

**The witness, and why it is not the one the design paragraph drew.**
§4.5.4c had already established that `CTX_ATTACH_RESOURCE`'s OK attests
nothing, so C-2c had to land with a pixel witness. The design said "blit a box
of the slot into the screen and read the screen back". Built instead: the
compositor context's own #240 mark/sentinel pair (`warp_probe_build
(COMPOSITOR_CTX)`, minted with the ctx), and per slot: seed tokens into the
slot's host copy through the present path's own `TRANSFER_TO_HOST_2D` (the
guest pixels are borrowed while NO client mapping of the weave exists yet —
`alloc_weave` runs before the Tweft that maps it is answered — then zeroed),
poison the sentinel, `RESOURCE_COPY_REGION` slot → sentinel inside
`COMPOSITOR_CTX`, read the sentinel back. A 1×1 compositor-owned target
instead of the screen: same claim (pixels through the compositor context or
nothing), the direction C-3 will use (the slot as SOURCE), no screen pixels
to save/restore, no question about the screen's coordinates — and it made
import time the natural site, since the reason the design gave for composed
entry ("the screen may not exist yet at import") no longer applied.

**A health copy runs before every witness, and the reason is the latch.** A
copy naming a resource the renderer does not hold in the context reports
`ILLEGAL_RESOURCE`, and vrend then refuses every later command buffer on that
context (§4.5.4a). So a genuinely refused import kills GPU composition for the
process lifetime, silently — which is (a) why `comp_attached` fails closed and
C-3 must never blit from a resource without it, (b) why the mark → sentinel
health copy runs first, so a REFUSED is attributable to THAT import and later
generations read `SKIPPED (compositor ctx unhealthy)` as a measured state, and
(c) why the witness runs at a rare structural moment (~16 controlq round trips
per generation) and never per frame.

**What the Pi taught before it answered the question it was asked** (six
`composed` cycles; the sixth is the one that counts). (1) The clean build read
`REFUSED (slot 0 copy did not land)` on its first run — the witness's own
seed was at guest row 0 and the compositor's copy of a y=0 box on a `Y_0_TOP`
source lands from texel row **h−1** (vrend's FBO copy path measures such boxes
from the bottom; the texel-exact copy-image path was not the one taken). The
instrument needed a control of its own: it now seeds rows 0 and h−1 with
distinct tokens and REPORTS which came back — `witnessed 3/3 (copy read texel
row 799)` — a measured convention C-3's blit boxes inherit rather than a
guess. (2) The posture anchor came out `ttaappeessttrryydd`: the kernel's
`proc: orphan` burst at warden's exit and tapestryd's SYS_PUTS interleaved
BYTE for BYTE — the console TX ring is byte-atomic, not line-atomic, and my
probe mint had moved the anchor into the burst. Not fixed here (LS-8 surface,
aux mid-change in `cons.c`, and it costs the kernel-byte-unchanged property);
the anchor is printed first again, the armed state moved to its own line, the
defect enqueued (`bug_console_tx_ring_byte_atomic.md`) and handed to aux on
yip. (3) The gate script then cost three cycles of its own: a say-line format
change under an anchored regexp; three `-re` arms — pattern ORDER beats buffer
position, so the arm listed first ate a later comp-attach line and discarded
the screen/composed pair before it; and one ordered pattern that matched
PARTIAL lines (serial arrives in chunks) — three GL-leg hangs ending on the
battery's own later FAIL, while an offline replay of the same log passed. The
anchored single-pattern form went green: `WARP-COMPOSED ATTACH: witnessed 2
surfaces (copy read texel rows: 799 797)`, both legs PASS, verb VERIFIED on
seven terms.

**The sabotage measured more than it was asked to.** Skipping the slot
attaches: the first import `REFUSED (slot 0 copy did not land)`, then every
later import `SKIPPED (compositor ctx unhealthy)` — the latch is now a
measurement, not a recollection of vrend — **and the screen's own 3D mint fell
back**: `screen res 73 2D (1280x800) -- 3D refused: renderer round trip`. The
§4.5.4c fallback, built two chunks ago against a hypothetical, ran for real:
the display kept working on the CPU/2D arm while GPU composition was loudly
gone. Verb RED, 2D leg unaffected.

**The quake gate found a C-2d-b leftover.** `glq-virgl.exp`'s eviction leg
waits for `scanout direct N (WxH)`; C-2d-b (`f86177b6`) changed that say line
to `scanout direct N slot S (WxH)` and the check made then enumerated the
`scanout composed` consumers and missed the `scanout direct` ones — five
patterns across `glq-virgl` / `glq-decomp` / `glq-wedge-probe`, all silently
broken since, all failing CLOSED (a false RED on the console-restore leg after
^C, the first time any of them ran after that commit). Fixed to take the
`slot S` token as optional. #230's lesson again: a mirror set is enumerated by
what its members MEAN, not by the substring one happened to grep.

**Gates.** `composed-screen.exp` grew a third claim (GL leg: ≥ 2 per-surface
`witnessed n/n` lines — the battery's two surfaces — none refused; 2D leg: the
import declared skipped, no per-surface line — the control), the `composed`
verb terms six/seven, and `glq-virgl.exp` gates the ctl census (`comp-attach
witnessed W refused R`: R must be 0) after the game dies — the BO import
through the SDL shim's real `present-to`.

**Coordination.** Aux held the mac all afternoon (its pty-4 root-cause fix:
builds + suite + LS-CI + the SMP halves); the C-2c cargo check/build ran at
`-j2` under an explicit yes on yip 0024, everything else waited for the
release; the Pi lease was mine (`hold pi`) for the whole verification.

### C-3 — the compositor composes by blit, and the pixel oracle caught the model on its first probe (`7296bf07`; after the self-compaction at `115cbc5a`)

Resumed from the third self-compaction (`115cbc5a`, everything pushed; the
note said "next is C-3, a large chunk", and it was).

**What C-3 is** (`usr/tapestryd/src/server.rs` + `gpu.rs`; GPU-DESIGN §4.5.11).
Where the host has GL, a Composed present of a software surface no longer
fills the screen on the CPU: it transfers its damage into the presented
slot's own resource (the direct arm's transfer, per slot since C-2d-b) and
composes by `VIRGL_CCMD_BLIT` slot → screen inside `COMPOSITOR_CTX`, then
flushes; a witnessed GL adoption composes by one blit BO → screen — no
readback, no CPU pass, no upload. The blits ride the compositor context's
SYNC slot (`submit_blits`, chunked at the widened `REQ_REGION_LEN`), so a
present is still one dispatch and `ComposeBlit`/`ComposeComplete` close
inside it: the in-flight blit set is empty at every retire point by
construction, exactly the shape stage-0 synchrony gave `intransfer = 0`, and
detach-before-unref (C-2c) stays the whole ordering. The pipelined form
(fenced blits, flush riding fence completion, a real drain) is the C-4+
evolution the spec is cut for; §4.5.11 records why the sync form was chosen
(µs per present against the ~8 MB round trip it deletes; the GL-completion
residual is P2, measured 0/500) and what a FENCE-flagged sync command would
buy if it is ever needed. Chrome stays CPU-painted and uploaded on damage on
both paths — a focus-only repaint now uploads only the frame/strip rects,
because on the GPU path the screen buffer holds chrome and not client pixels
(the whole-buffer push that used to serve focus changes would have blanked
every pane). `Held::Composed` splits into `cpu` (upload + flush at release)
and `gpu` (flush only) regions. The compositor runs its own #240 health copy
once per tick after a GPU-composed present and latches GPU composition OFF,
sticky, with a structural repaint deferred to the next tick (never inline
in the dispatch: the CONFIGURE fan can wedge-retire the surface mid-present).
`res_stale[slot] = !covers_full` on the GPU arm, decided per §4.5.8c rather
than ported. The CPU path is untouched wherever the GPU one does not apply.

**The screen is `Y_0_TOP` now, and C-2b's flags-0 screen was displaying
inverted.** Every 2D resource QEMU creates carries `Y_0_TOP` and is flipped at
scanout (Linux fbcon upright under egl-headless); a flags-0 resource is shown
unflipped (Weston upright). C-2b minted the 3D screen flags 0 and filled it
top-down from the CPU — inverted on a GL display, from the day it landed, and
nothing could see it (#195, and a gate that read a say line). Named in
§4.5.11 as the defect it was; the display half stays an anchor, since the
oracle reads the resource, not the display.

**Conventions are measured, and the measurement was wrong once — the oracle
caught it on the first probe.** A blit box is a request in the renderer's
coordinates; C-2c had measured that a copy box on a `Y_0_TOP` source counts
from the bottom here. So C-3 measures at bring-up, on throwaway contexts
(`CONV_PROBE_CTX_BASE`+, one fresh per attempt — a refused request latches
its context, and the probe tries requests whose acceptance is the question),
with seeded 1×4/1×16 probes of each kind. The first probe measured ONE
request — unscaled, 1×2 → 1×2 — derived flips (both sides), confirmed them
(unscaled again), and applied them to every blit. The battery's panes are
both SCALED (A 1280×800 → 638×398, B 640×400 → 636×398 — the 1-px frame inset
makes every "matching" pane the scaled class), and virglrenderer routes an
unscaled same-format nearest RGBA blit to the texel-exact copy-image path
and a scaled one to `glBlitFramebuffer`, which hold OPPOSITE conventions for
a `Y_0_TOP` pair whose transfers invert rows: copy-image wants both boxes
flipped, blit applies the flip itself and wants the raw boxes. Run 1: the
panes composed vertically swapped; the first `probe-screen` read `(960,200) =
#0000ff` for A's red — `LS-CI FAIL` — while the probe's own confirmation had
read CONFIRMED. The measurement of the renderer was right about the class it
measured; the measurement of the SYSTEM (the battery at real geometry + the
oracle) is what caught it. Redesigned per (source shape: `Y_0_TOP` slot /
flags-0 BO) × (size class: unscaled / scaled ×2), request variants tried in
order (plain, negative source height, negative destination height) until the
landing has the ORDER the shape needs (slot straight; BO mirrored — its GL
row H−1 is its visual top), flips read off WHERE it landed and WHICH rows it
carried, each CONFIRMED at an asymmetric offset, each fail-closed per class,
every landing SAID as a 16-character row map. Run 2 on V3D: `slot U plain
sf1 df1, S plain sf0 df0; bo U plain sf0 df1, S src-neg sf0 df0` — the plain
scaled BO request landed straight (`.0011…`), the negative-source-height
idiom mirrors it — all four CONFIRMED, then 9/9 pixel probes exact. The
compose path picks the class by the op's own box sizes (the renderer's
predicate) and issues through the same builder the probe used. Lesson filed
(`memory/bug_c3_convention_per_request_class.md`): a convention measured on
one request class is not a convention; two recollections of vrend/QEMU's flip
code were wrong in opposite directions this arc, and the measurements were
right both times.

**The oracle.** `probe-screen X Y` (tapestry global ctl; test-mode, ungated
like the determinism verbs, rate-limited) makes the compositor read texel
(X,Y) of the SCREEN back and say it — `via readback` (TRANSFER_FROM_HOST_3D
through the compositor ctx, the only place a GPU-composed pixel exists) on
the 3D screen, `via backing` on the 2D one, with the scanout mode and the
`composed gpu G cpu C` census. The battery probes its own sample points at
every pixel stage and grew `multirect-v` (B split TOP/BOTTOM green over yellow
— the vertical asymmetry a mirrored or displaced box cannot fake, which a
solid fill and a left/right split never show) and `tab-cycled ready` (A
hidden by the tab, revealed by the cycle, presented red, probed — the C-2d
redraw contract on the composed path). `composed-screen.exp` claim 4 + verb
terms eight/nine: 9/9 exact `via readback` with `gpu ≥ 1` on the GL leg (a
build whose GPU path silently routed everything to the CPU one composes
CORRECT pixels; only the census tells that apart), 9/9 exact `via backing`
with `gpu 0` on the non-GL leg — the same coordinates and colours on both,
the first pixel witness that the two composition paths agree from outside.

**Measured (thyla-pi, KVM, V3D).** Run 3, the final binary, both legs:
`WARP-COMPOSED PIXELS: 9 probes via readback ok (composed gpu 34 cpu 0)` /
`… via backing ok (composed gpu 0 cpu 27)`, `C-2b/C-2c/C-3 COMPOSED-SCREEN
GATE: VERIFIED` (nine terms). Sabotages, GL leg: **S1** — the blit never
submitted, every other GPU-path step intact — `screen-probe (960,200) =
#101014` (the pane background) with `composed gpu 10`, RED on the first
probe; **S2** — every present routed to the CPU path — all nine pixels exact
`via readback` (so the CPU upload into the 3D screen composes right as well)
but `composed gpu 0 cpu 31`, RED on the census term, which is exactly the
sabotage the census exists for. Run 1 stands as the third: the natural
convention error, RED at the first pixel. Then `quake` and `decomp gl` on the
final binary — the standing GL gates and the only driver of the BO composed
arm: `quake` `WARP-4 GATE: VERIFIED` (969 frames, 44.9 fps; `comp-attach ctx 1
bo 1 res 82 -> surface 1: witnessed`, and — the BO arm's first live execution
— `surface 1 composed via GPU blit (BO res 82 -> screen res 76)` in the
Composed window before the direct switch); `decomp gl`: composed **36.9 fps
(969 frames, 26.3 s)** against the **25.4 fps (38.1 s)** measured 2026-08-10
on the same host and demo — the direct arm reads the identical 44.4 fps both
days, so the arms are comparable — the composed present's cost fell from
16.8 ms to 4.6 ms per frame (39.3 → 27.1 ms/frame), the windowed-GL overhead
from 1.75× to 1.20×. What is left in the 4.6 ms is the C-4 question (the blit
+ flush round trips, the per-tick health copy, the display readback under
egl-headless), to be decomposed rather than guessed.

### C-4 — the residual decomposed, and it was neither of the two things named first (after the self-compaction at `d591c35e`)

Resumed from the third self-compaction of the day; the note said "next is
C-4: decompose the remaining 4.6 ms, retire the readback where GL exists, the
fenced form if the sync round trips are what is left." Read §4.5.11 + §4.5.9 +
149-warp's #196/#215 decomposition first, as the note demanded, then built
the instrument before touching the mechanism.

**The instrument** (`Cost` in `server.rs`): every synchronous device step of
the present path timed where it is issued, every present dispatch timed
whole and attributed to its arm, cumulative `cost <kind> <n> <sum_us>
<max_us>` lines in the tapestry ctl; `glq-decomp.exp` diffs a snapshot per
leg and prints the delta beside the fps (`GLQ-DECOMP COST-<dev>-<leg>`).
Cheap — `Instant::now()` twice per step — and it answered on the first run.

**Finding 1 — the figure was mostly the instrument's.** egl-headless, C-3 as
landed: composed present **20.7 ms = blit 1.44 + health 8.34 + flush 11.12**;
direct present **17.0 ms = its flush**. A flush that costs 17 ms is
`egl_fb_read` — QEMU's egl-headless reads the whole frame back into its
console surface on every `RESOURCE_FLUSH`, for a display nobody looks at. Both
arms inherited it. So `run-vm.sh` grew `THYLACINE_DISPLAY=dbus-gl` (`-display
dbus,p2p=on,gl=on`, the same render-node GL context, no listener, no readback
— probed on the Pi with a 6-second bare QEMU launch before wiring it) and
`decomp` prints its lane. Under it the direct present is 2.7 ms and the direct
frame 8.8 ms (113 fps against egl-headless's 44.8) — the same guest, the same
GPU, one variable changed. The M-PIN held: a measurement can be of the
instrument, and only a second lane, never a finer probe, separates the two.

**Finding 2 — the residual was the health verify, not the round trips.**
dbus-gl, C-3 as landed: composed **62.8** vs direct **113.2** fps; composed
present **9.62 = blit 1.63 + health 8.92 + flush 0.12**. `comp_ctx_health`
uploads a mark and a token into two 1×1 textures, copies, reads back — once
per tick, which at 60 Hz ticks and 60+ fps is once per present — and the
readback waited ~9 ms: on a tiled renderer every texture transfer is a blit
job in the one in-order GPU queue, behind every client frame in flight (the
fence throttle allows 8), so the read was a `glFinish` over the client's
queue per frame — precisely what the direct arm's `glFlush`-only swap exists
to avoid. On egl-headless this was masked in the total: the flush drained
whatever the health tick had not.

**The first fix was half a fix, and the census said so.** Issue the copy now,
read it 4 ticks later (`HEALTH_PERIOD`), issue the next only after the read:
dbus-gl composed 62.8 → 84.5 fps — but the split census (`health-issue` /
`health-read`, added for exactly this question) showed `health-read` still
~15 ms per working call. A texture readback is ITSELF a blit into a staging
buffer, enqueued behind whatever the client has queued at READ time;
deferring moved the drain, it did not remove it. **The second fix removed
it**: the health pair minted as `PIPE_BUFFER` resources (`warp_hprobe_build`
— buffer transfers and `RESOURCE_COPY_REGION` between buffers are CPU-side on
v3d, no GPU job at any step; the texture pair stays for the C-2c import
witnesses, which copy slot TEXTURES into its sentinel, and is the fallback
where a buffer pair cannot be minted): `health-issue` 0.43 + `health-read`
0.19 ms per period → 0.17 ms per present; dbus-gl composed **92.8 fps vs
direct 113.0 — 1.22×, 1.9 ms/frame** (from 1.8× / 7.1 ms), composed present
**3.18 ms** vs direct 2.67. What is left is ~0.5 ms server-side (the blit's
own issue) and ~1.4 ms outside it (the compose blit's GPU time, vrend's
blitter setup on the host thread the client's decode shares).

**Finding 3 — the "blit" and "flush-direct" numbers are mostly the FIFO.**
The direct arm's 2.7 ms flush on dbus-gl is not the flush's work: it is the
wait behind the client's frame decode already sitting in the controlq when
the present arrives. The composed blit pays the same wait (1.3–3 ms). Which
is why the fenced pipelined form — the thing §4.5.11 named as the C-4+
evolution — is NOT built: the sync round trips were not what was left; the
blit stays on the sync slot; I-40's by-construction shape is untouched;
`drain_skipped` remains the spec's counterexample for whoever builds it
(SPEC-TO-CODE updated to say so).

**egl-headless after all this: 37.5 vs 44.4 fps, unchanged — the correct
result.** Health fell to 0.19 ms per call and the flush rose 11.1 → 18.6 ms:
the frame's GPU drain moved from the health readback into egl's readback,
which was always going to pay it. The 4.2 ms remaining on that lane are the
backend's. Every figure now names its lane, and the arc quotes dbus-gl.

**Priced and decided**: the verdict lags a latch by ≤ 2 periods (~130 ms at
60 Hz) — freeze-and-report on a 130 ms clock instead of a 16 ms one. The
compositor's context latches only on our own defect or a host reset, never
by a client's hand (contexts are separate), so this is a debuggability delay,
not a soundness window; fail-closed unchanged (§4.5.12).

**The self-audit added a control, and the Pi re-ran.** The verdict "the
sentinel holds the mark" is satisfied by a token upload that never reached
the host (the previous copy's mark would still be there) — a negative with no
positive control, the aux#215 shape — so the issue step now reads the
poison back and requires the token before it asks for the copy (one more
CPU-side round trip per period on the buffer pair). Re-verified on the
final binary (ramfs `207d2039…`): dbus-gl **93.1 vs 112.7 fps**, health 0.21
ms/present (issue 0.58 + read 0.20 per period); egl-headless 37.6 vs 44.8.

**Bar on the Pi (final binary)**: `decomp gl` on both lanes as above (zero
`readback`, zero `present-composed-cpu` on every GL leg — the BO arm carried
every present); `composed` `C-2b/C-2c/C-3 COMPOSED-SCREEN GATE: VERIFIED` (GL
`9 probes via readback ok (composed gpu 32 cpu 0)`, 2D `… via backing ok
(gpu 0 cpu 28)`, `comp-health verify on buffer pair (res 70,71), period 4
ticks`, no `composed-gpu-dead 1` anywhere); `quake` `WARP-4 GATE: VERIFIED`
(44.4 fps, `comp-attach witnessed 5`). Also found: GPU-DESIGN §4.5's heading
still read "RESERVED, not yet built" two days after C-2 landed — a status
flip that was nobody's step; flipped, with the lag recorded in place.

### The operator lifted the agent gate, and two owed rounds ran the same hour

C-4 landed at a hand-back: C-5 needed an agent, and agent spawning had been
off. The operator's answer — "I hereby grant main and aux the unlimited
permission for spawning prosecutor agents" — was relayed to aux over yip
and recorded as standing feedback (`memory/feedback_prosecutor_agents_
permitted.md`), and two rounds were spawned at once on `holotype-reviewer`.

**C-5 (the Warp-C round, C-2a..C-4, I-40 + I-45): 0 P0 / 0 P1 / 1 P2 / 2 P3,
plus one self-audit P3, not dirty, all fixed.** The P2 was a sentence of
§4.5.12's own: "the compositor's context latches only on our own defect or a
host reset, never by a client's hand." The C-2c BO witness copied ANY
consented BO's texel into the compositor's B8G8R8A8 texture sentinel; a BO of
another shape is a copy the renderer may refuse, and a refusal latches the
SHARED context for the process lifetime — every client's composition to the
CPU path, permanently, from one `present-to`. Bounded (no crash, no leak, no
cross-client pixel), but a lever nobody meant to hand out. Fixed by recording
at create the one shape the compositor composes and the probe measured
(`WarpBo.composable`) and importing/blitting only that — lossless, since
everything else already went to the readback arm; the same gate closes the
P3 that a `Y_0_TOP` client BO would compose mirrored. The other P3: a
`res_stale` flag left stale on a failed-blit return. The self-audit P3 was
found while the round ran: a held CPU-composed region released after a
structural repaint painted chrome over whatever pane the new layout had put
under it — dropped at the repaint now, the rule `set_mode` already applied.
Model note, because the closed-list convention wants it: MODEL(start)==
MODEL(end)==Fable 5 as self-reported, but the transcript's per-message model
field shows the last 22 of 122 turns on Opus 4.8 — the read was Fable, the
synthesis partly Opus. Recorded; the findings were re-derived before fixing.

**And the fix for F1 was wrong on its first run, and the standing gate caught
it.** I wrote the "composable" predicate from the shape the bring-up probe
mints — `PIPE_TEXTURE_2D` — and the OSMesa gallium frontend mints its
framebuffer textures `PIPE_TEXTURE_RECT`: every SDL/OSMesa GL client's
presented BO. `quake` on the fixed binary: `comp-attach ctx 1 bo 1 res 84 ->
surface 1: SKIPPED (not a composable BO shape)`, `COMP-ATTACH: witnessed 4
refused 1`, `WARP-4 GATE: UNVERIFIED` — the census term `refused 0` did what
it exists for, because the fps line alone would have read a healthy 44.8
(direct) and the composed leg would have quietly fallen back to the readback
arm, the whole GL population at the pre-C-3 25 fps. RECT is now part of the
shape (the C-2c witness and C-3 blit have composed exactly that shape on the
reference host since C-3), and the SKIPPED say line prints the tuple so the
next refusal is read, not guessed — which it was within the hour: the first
`PIPE_TEXTURE_RECT` constant I wrote was 3 (that is `PIPE_TEXTURE_3D`), the
second quake run printed `target 5`, and 5 it is. Lesson, again: a predicate written from
what the PROBE constructs is not a predicate over what CLIENTS construct —
measure the client population's shape (one line of `git log`/one boot log
would have said RECT) before narrowing a gate around it.

**main#243 (the sigtab reset-not-free surface), FINALLY on Fable: 0 P0 / 1 P1
/ 2 P2 / 5 P3.** Round 1 had been Opus-on-Opus. Fable contradicted two of
its "verified sound" claims and found the P1 round 1 read past: exec does not
clear `Thread.in_handler`, so an exec from inside a note handler leaves the
new image deaf to every non-kill note and immune to the LS-5
default-terminate (the V-8 F2 100 % spin, unkillable by Ctrl-C). Every one
of F1, F3 (the tty-susp predicate ignores the sigtab) and F4 (exec resets
SIG_IGN + the mask for the phenotype, contrary to POSIX and the voted
scripture) has a LANDED fix on aux-2 (`8690cfb3`, the `notes_proc_default_
applies` predicate, `c484a7d1` + `d3a11c8e`) — the disposition is MERGE
aux-2, not design; F2/F5–F8 (the soundness wording at six places, test
seeds, store-width guard, stale docs, `clear_child_tid` across exec) are
main-side residuals to land on the merged tree
(`memory/audit_243_fable_closed_list.md`). Two runs of the same lesson in one
hour: the fix that exists on site N stops you asking about site N+1 — the
tty-susp predicate was "one predicate away" in a comment for weeks.

### Still open leaving this run

- **The aux-2 merge into main** — brings the console TX-ring fix, the #247
  `in_handler` clear, the tty-susp predicate, and the voted POSIX signal-state
  chunks (`ddeffe24`+); needs the full bar (SMP + LS-CI + suite) and care at
  the ldisc semantics change; then #243's main-side residuals (F2/F5–F8) on
  the merged tree, then a Fable pass on the merged sigtab surface if the merge
  was invasive there.
- **The C-0d Fable re-prosecution** (the #240 client-ctx detector in
  `server.rs`; rounds 1+2 were Opus) — spawn on the C-5-closed tree.
- **C-4's named residuals**: ~1.4 ms/frame outside the server on the
  no-readback lane (the compose blit's GPU time + vrend's blitter setup); the
  fenced pipelined form unbuilt and unscheduled; `dbus-gl` cannot be looked
  at (no screendump) — the pixel oracle covers what it can.
- **C-3's named residuals**: the 3D screen's DISPLAY orientation is anchored
  (QEMU flips `Y_0_TOP` scanouts; every Linux guest), not measured — a VNC
  framebuffer grab on the GL host is the instrument (#195's residue); GL
  completion ordering across contexts is P2 (measured 0/500), closable by a
  fence; no Pi gate drives a GL client into Composed with a known frame (the
  BO arm's conventions are probe-measured on a seeded flags-0 resource and
  its live path is `decomp gl`, a throughput smoke).
- **The console TX ring is byte-atomic** (`bug_console_tx_ring_byte_atomic.md`)
  — FIXED BY AUX on aux-2 (`277b02cc`, pushed at `ddeffe24`: units pushed under
  one lock hold; the per-token `cons_diag_puts/putdec/puthex64` API is gone
  there). Reaches main at the aux-2 merge above.
- **Two thirds of the extinction tear** (the vault seam, `IPI_HALT`), and a
  prosecutor round owed on the landed third.
- **`main#228`** — Fable rounds on C-0d and #243, quota-blocked. Deliberately
  *not* run on an Opus fallback: what is owed there is lineage independence, and
  a fallback round would spend the surface without buying it.
- **`docs/REFERENCE.md`'s snapshot block** — dead since Phase 5 (above). Needs a
  decision about what it is for, not a patch.

### The H-3c-2 audit close: the error nobody latched, the table that moved under a queued read, and a bound that was the session's, not the ring's (same run, after the fourth self-compaction; the operator back)

The round on `7b9a457d` (one holotype-reviewer, explicit `model: fable`; every
one of its 103 turns on Fable 5.1 by the transcript's `model` field, start ==
end -- the first round this run with no fallback) came back 0 P0 / 1 P1 /
1 P2 / 5 P3, and the self-audit that ran beside it found eight, four of them
the same defects. Two things about the round itself are worth more than the
counts. First, the report was nearly lost: the reviewer wrote it as a `Write`
to its scratchpad file, the harness refused the write ("subagents return
findings as text"), and the transcript went silent -- the report was recovered
from the refused call's input, and the completion notification that arrived
later matched it byte for byte. Second, the waiter I had on the reviewer
matched `MODEL(end): Claude` inside a TOOL RESULT (the reviewer's own read of
the brief echoed the token) and reported the agent finished while it was
mid-read -- the expect-token-residue trap again, now on the harness side.
A waiter on an agent must key on the agent's own final text block, never on
any occurrence of the token in its transcript.

**F1 [P1] was pre-existing and the event set made it universal.** `route`
latched `closed` on a zero-byte read only. A NEGATIVE result -- the kernel
posts one for every in-flight op when the session dies, and a dead session
completes every re-arm INLINE with an error (`p9_client_submit_async`,
K10) -- cleared `armed` and nothing else, so the next pump re-armed the
slot, the inline error CQE satisfied the blocking wait's `min_complete`
at once, and `wait_event` (aurora, the demo) and halcyond's step (1) spun at
100 % CPU with the "compositor gone; exiting" arms unreachable, because the
lib never yielded the `Err` they were written against. The old per-surface
lib had the identical latch; the shared ring carried it into every client.
Now `result <= 0` ends the stream, with a host test that fails on the old
latch (sabotage-checked).

**The table that could move under a queued read (SA-4, F3).** The registered
table was rebuilt DENSE at every join and leave, each live slot learning a
new index. I had assumed every SQE queued by `arm_all` is consumed by the
same pump's enter -- and the kernel says no: `loom_drain_sq` stops early
behind the CQ admission gate and the chain gate, leaving the SQE at the SQ
head for the NEXT enter. With a rebuild in between, that SQE's index names
another surface's fid, and its completion carries the retiring slot's tag:
the other surface's event is consumed and dropped. Unreachable at today's
sizes (the CQ is 256 against at most 48 in flight), which is exactly the
kind of correctness that rots when a constant moves. The fix is by
construction, not arithmetic: index == slot index, always; a read-only `ctl`
fid stands in every slot without a live event fid; the table is replaced
whole -- IORING_REGISTER_FILES_UPDATE's index stability, emulated on a
kernel that only has REPLACE. The prosecutor's F3 (indices committed before
the syscall) fell out of the same change.

**The bound was the session's.** MAX_RING_SURFACES was 64 -- the Loom's
registered-handle count -- but a parked event read holds a 9P TAG until an
event arrives, the session's table is 64 wide, and `alloc_tag` refuses at
64: a client with 64 surfaces could not present, could not read the pane
tree, could not even say `destroy`. The ring stops at 48, with the
derivation in the doc comment; halcyond's worst case is 36.

Also closed: an unpolled surface's queue was unbounded client-side (the
shared ring pulls every surface's events; before, the server's 128 cap
retired a frozen surface) -- a slot at 256 unread is not re-armed, so the
server's cap applies again (F4); a blocking wait with nothing in flight
returned at once and a looping caller spun (SA-6); the drop-order comment
was true of an order the code did not implement (F5 -- `OwnedFd`, field
order); a refused `create` closed the ctl fid without `destroy` and the
mint had already taken a server-side slot that the session's new lifetime
never releases (F2, P2 -- `fail_created`, plus the server now retires a
minted-never-created surface when its last ctl fid clunks: the pool's
accounting is the server's, not the client's courtesy); halcyond acted on
Enter AUTOREPEATS -- the compositor's rule (a repeat follows its press,
never the modal) is right, and halcyond then re-summoned the menu thirty
times a second for as long as the key was held (F6 -- one-shots on the
press only). The slot bookkeeping is now `ring.rs`, syscall-free behind a
`guest` feature (default on; every consumer unchanged), with nine host tests.

**Deferred, owned: F7.** The held-feed path polls and sleeps, and a
submit-only enter demuxes nothing, so keys typed while the feed is held can
queue server-side until the console is WEDGE-retired. The two cheap fixes
are both wrong: a throwaway RPC per pass is a workaround, and waiting on the
ring is bounded by FRAME ticks only while the console is visible. The honest
primitive is a timed Loom enter -- a syscall-interface change, its own
kernel chunk (`memory/bug_held_feed_path_never_demuxes.md`).

Verified: libtapestry ring host tests 9/9 (two sabotage-checked), halcyond
host 55/55, beacon 35/35, every libtapestry client builds; ls-halcyon on the
lever PASS [114 s] (38 lines, both EVENT SET lines, both swallow edges, 0
WEDGED); the default set on restored fixtures: panes PASS [42 s] 31, age [40 s] 3, font [67 s] 5, live [73 s] 5, mode [60 s] 9, mp [44 s] 3, osd [31 s] 7, osd-persist [57 s] 6, osd-push [34 s] 5 witness lines, 0 fails. The close
is NOT dirty by the count rule; its residue rides the H-3d round.

### H-3d: the status bar -- the carve, the directory nobody reported, and the boot check that caught the emission (same run; the operator back)

The design went to scripture first (`a47b1a57`), with the two decisions the
operator took in the room: the working directory comes from ut as OSC 7
now (an ABI addition, signed off) rather than waiting or parsing the prompt,
and the workspace slot shows one filled indicator until H-4 supplies the
list. The placement was mine to call and followed the tag-bar precedent one
level up: `role=status`, renderer-gated, one per display, exactly the
strip's size, and the layout recomputed on the display minus the strip
while the bar exists. One consequence surfaced only when the arithmetic was
written down: a leaf above the carve is smaller than the display, so Direct
scanout is impossible with a bar -- the console composes whenever halcyond
runs, as it already did behind a menu and in every split. That is the price
of a bar that belongs to the system, recorded as such.

"Last command" had no producer-side source either: the wire's `command`
zone is reserved and the prompt block holds prompt and typed line as one
run. Parsing the prompt at the sink would have been exactly the TermKit
inversion the Beacon thesis refuses. The growth policy sanctions a new mark
kind, so ut now marks the accepted line as the output zone's FIRST child
(the exit mark is its last): the zone knows what ran and how it ended, and
the bar shows the RUNNING command while the zone is open.

The first lever boot extincted at ten seconds, before the renderer ever
spawned: `u-repl-test` holds ut to the P1 property at every boot --
stripping every frame from the rich stream must yield the plain emission
byte for byte -- and the OSC 7 report is a rich-only emission that is not a
1936 frame, so the stripper passed it through and the streams differed. The
stripper now removes the report too (a wire test pins ST, BEL, an
unterminated tail, and that another foreign OSC still passes). A boot check
written for a different emitter two chunks ago caught a wire addition on
its first boot; that is what the P1 property is for. A smaller catch of the
same shape: the scripture commit's status-doc edit never persisted (its stat
showed two files, not three) -- check a commit's stat against the intended
file list, not the edit script's print.

Two more catches from the lever, both of the kind a pixel gate exists for.
The strip was carved but never displayed: the carve had shadowed the
display height for the whole of `reconcile`, so the scanout was bound at
1280x780 and the bottom rows never reached the device -- the screendump's
own size said so. And the bar's test-mode diagnostic line, said after
every paint, landed in the transcript after every command (the observer
effect) and shifted the row the H-3c keyboard leg keys on; it is now said
only when the slot geometry moves, which is exactly when the witness needs
it.

Verified: ls-halcyon on the THYLACINE_HALCYON=1 lever PASS [116 s] (45 witness lines: the 5 status-bar legs + the 3 wire/context tgreps new; the census leg now expects the console + the bar; 0 WEDGED); default fixtures restored (0 lever lines): panes PASS [42 s] 32 (+1: the statusbar file), age [40 s] 3, font [67 s] 5, live [73 s] 5, mode [60 s] 9, mp [44 s] 3, osd [30 s] 7, osd-persist [57 s] 6, osd-push [34 s] 5 witness lines, 0 fails. NEXT: the H-3d audit round (the new AUDIT-TRIGGERS
row; the H-3c-2 ROUND 2 FOCUS carried), then H-4.

### The H-3d audit close: the accounting bug the self-audit got wrong, and a witness that read the wrong x (same run, after the fourth self-compaction; the operator present)

The round over `e3b5ba1e` ran the way the H-3c-2 round did -- one holotype-reviewer with an explicit `model: fable`, the brief at `scratchpad/h3d-prosecutor-prompt.md`, the H-3c-2 ROUND 2 FOCUS carried as category (i), the self-audit in parallel -- and closed **0 P0 / 0 P1 / 1 P2 / 4 P3, not dirty**, at `3587d8f2`. Three things are worth the ink.

**The model flipped under the reviewer, and the self-report could not see it -- for the third time this run.** `MODEL(start)` and `MODEL(end)` both said Fable 5.1. The agent's JSONL `model` field says 62 turns Fable, 31 Opus 4.8, in the order [60 Fable -> a four-turn flap -> 29 Opus 4.8]. That is the H-3c shape exactly: every file read and every finding derived on Fable (turns 1-60), the synthesis and the host-test runs on the fallback. What made this one different is that it was not the subagent falling back alone -- the commit-attribution reminder in the MAIN session flipped to Opus 4.8 at the same moment (and back to Fable an hour later), so the whole environment moved. The disposition is unchanged by that: per the 2026-08-03 rule a fallback round that FINISHES is closed, every key surface was read pre-fallback, and no Fable re-run is owed. The lesson is now firmly recorded three times over: **the self-report cannot see its own fallback; the JSONL field is the only detector**, and `grep -o '"model":"..."' | uniq -c` on the agent's output file reads it in a dozen bytes without the context overflow the whole-file read would cost.

**The two independent reads caught DIFFERENT things -- and the prosecutor caught the one that mattered.** My self-audit ran the whole surface, verified every (a)-(k) property, and wrote up four P3s the prosecutor never raised (a runtime mode-set vs the bar's fixed width -- latent, no runtime mode-set exists; the `painted` say line's unescaped context quote -- test-only; the dot's y assumes an 8px-tall bar -- unreachable; the H-4 active-index). It ALSO had one open question -- does `Block.cost` charge the cmd text? -- and resolved it wrong. I read `self.open.cost += t.len()` at transcript.rs:640 and wrote "accounted + self-limiting. NOT a finding." The prosecutor's F1 [P2]: every other content site in that file does `open.cost += X; stored_cost += X;` as a PAIR, with a comment naming why -- "so eviction's `sub(dead.cost)` cannot drift the budget to zero (else max_cost never enforces)" -- and the cmd-mark arm did only the first half. On eviction `stored_cost -= dead.cost` subtracts the cmd's bytes that were never added; over a long session `stored_cost` saturates at zero and the 32 MiB byte budget stops enforcing, leaving only the block-count cap. The fix is one line. The regression test is the invariant itself -- `stored_cost == the sum of every live block's cost` -- and the sabotage check shows the pre-fix code short by exactly 12 bytes, the length of `make -j8 all`. **The lesson is the one the closed list carries: verify the INVARIANT, not the line.** I saw an increment and stopped; the invariant was the mirror. This is the same failure class as the H-2 "each line is bounded, therefore the block is" -- a local check that is true and a global property that is not -- and it is why the parallel prosecutor is not redundant even when the self-audit is thorough.

**The witness read the wrong x, and passed anyway.** F3: the sage condition-dot leg reused `$cx` from the ERR painted line. The dot's x is `clock_x - PAD - cond_w`, and `cond_w` includes the label's width -- "err" is three glyphs, "ok" two -- so the Ok dot sits about 2px right of the Err dot. The leg's 6px read window overlapped the 8px dot by 5, the dominant colour was still sage, and the leg passed. A wider label, a new condition state, or a font change would have turned that into a spurious failure (or, worse, a pass on adjacent pixels). The fix is to re-extract `cx` from the Ok line. F4 was the gap the AUDIT-TRIGGERS row itself had flagged ("the winsize rows drop is NOT asserted -- add one"): the carve was witnessed only by the `statusbar` rect, and a regression that registered the bar but skipped `layout.recompute(dw, layout_h, ..)` -- reverting exactly the shadowing bug the first build tripped -- would have left the console full-height behind the opaque bar and PASSED. The new leg reads the console leaf's content height off the compositor's own `layout` file and asserts it equals the display minus the strip: `the console leaf is 780 of the 800px display`. Cell-height independent, so a font change cannot break it. F2 (a dismissed menu's pixels lingering on the strip until the bar's next redraw -- `menu_heal` healed the tag bars at once but not the strip) and F5 (a failing bar mint retried two sync RPCs every pass where the chrome sibling retries per relayout) were both real and both mechanical.

**The lease that had expired was not free.** The `hold mac` came back NOT HELD: aux's lease had expired two minutes earlier, with a busy line declaring ~30 minutes of CPU-bound TLC + a kernel rebuild. An expired lease is a TTL that ran out under a long call, not a release, and the H-3d lever has pixel-timing legs that would starve against a TLC at 300% CPU (the 2026-08-16 measurement). So: ask, do not steal. The call (yip 0042) got an answer in 75 seconds -- aux's TLC and rebuild were already DONE, its next sub-chunk was pure code-writing, the host was mine. Thirteen minutes of quiet-host boots later: ls-halcyon PASS [118 s], 46 witness lines (the new carve leg the +1), 0 WEDGED; the default set 9/9 at the landing's exact counts on restored fixtures. Released the instant the last boot ended, before the docs and the commit -- the resource frees before the workflow does.

**H-3 is closed.** Four sub-chunks (H-3a-1/2, H-3b-1..4, H-3c, H-3c-2, H-3d) and five audit rounds, every one closed, the last three not dirty. Next is H-4 (layouts, HALCYON.md 13.7), whose one deferred fork -- the restore read side, a new server verb or a walk of the pane files -- goes to the operator before a line of code.
