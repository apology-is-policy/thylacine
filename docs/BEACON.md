# BEACON.md — the semantic output markup

**Binding scripture.** Adopted 2026-09-01 (the Halcyon kickoff design conversation;
user-ratified in-session — the three-source UX vision [i3 / acme / Symbolics
Genera], all forks resolved, name user-chosen). Beacon is the annotation language
by which Thylacine programs communicate the *meaning* of their textual output;
renderers realize that meaning at whatever fidelity they possess. Halcyon renders
it richly (proportional type, live presentations); Aurora and a serial host
terminal let it pass harmlessly. Cross-refs: `docs/HALCYON.md` (the first rich
consumer), `docs/AURORA.md` (the cells-tier sibling),
`docs/COREUTILS-THYLACINE-DESIGN.md` (the existing visual language Beacon
subsumes), `docs/SYS-FD-DEVCLASS-SPEC.md` (the emission gate's kernel
dependency), `docs/TAPESTRY.md` §16 (the agentic payoff), `docs/UTOPIA-VISUAL.md`
(Bonfire, the palette — a *sibling*, not a component: Bonfire is color, Beacon is
meaning).

---

## 1. Thesis — annotate meaning at the producer, render at the sink

A program knows *what* its output is — this run is a path, this block is a table,
this region is one command's output — and today it throws that knowledge away at
`write()`, leaving every consumer to guess from flat bytes. Beacon keeps it: a
small, closed vocabulary of in-band annotations that travel *with* the text
through every pipe, PTY, and console in the system, and are interpreted only at
the final renderer.

The design is a deliberate fusion of three heritages, each contributing the part
it got right:

- **Symbolics Genera** (presentations): output is *typed*; the screen remembers
  that a region presents an object, and the commands applicable to it follow from
  its type. Genera could hold live object references because everything was one
  Lisp image. We cannot — so:
- **Plan 9** (the namespace, and the plumber): in Thylacine the object reference
  *is a 9P path* — the one universal, cross-process, already-secured name for
  anything. The plumber's rules-file pattern supplies the verb table.
- **The modern terminal ecosystem** (OSC): in-band escape-framed annotation is
  proven at scale — OSC 8 hyperlinks and the OSC 133 semantic zones
  (prompt/command/output marks adopted by iTerm2, WezTerm, kitty, Windows
  Terminal) demonstrate both the mechanism and its graceful degradation, because
  every conforming VT parser already discards OSC sequences it does not
  understand.

**What Beacon is not.** It is not structure-in-the-pipe (PowerShell/Nushell —
and TermKit's grave): the pipeline carries plain bytes, always; Beacon frames
appear only on interactive sinks (§4). It is not typography: a program never
says "italic," it says "emphasis" (§3). It is not a pixel channel: images,
video, and every graphical surface ride Tapestry (`docs/TAPESTRY.md`), never
Beacon — Beacon annotates *text*.

## 2. The wire — OSC-framed, text-as-payload

Beacon frames are OSC escape sequences on the ordinary output stream:

```
ESC ] 1936 ; v1 ; <op> [; <arg>...] ST        — open  (ST = ESC \)
ESC ] 1936 ; v1 ; /<op> ST                    — close
```

- **OSC 1936** is the Beacon number — the thylacine's year, and unclaimed by any
  terminal we can find (iTerm2 owns 1337, urxvt 777, ConEmu 9). **ADOPTED
  2026-09-01** (the 12.10-1 confirmation ran at the H-1 close: collision greps
  over `third_party/` + `usr/ports/` + the tree clean — reviewer + author
  independently).
- **`v1`** is the vocabulary version. A renderer ignores frames whose version it
  does not speak (payload text still renders).
- **The bracketing rule is load-bearing: the plain text is always the payload in
  the stream; frames only wrap it.** Stripping every Beacon frame from a stream
  MUST yield exactly the bytes the program would emit at the `none` tier. Text is
  never encoded *inside* a frame (the OSC 8 discipline). This single rule is what
  makes degradation free: a renderer (or host terminal) that discards unknown
  OSC renders the plain output correctly without knowing Beacon exists.
- Frames terminate with ST; parsers tolerate BEL per VT convention. Frames nest
  (a zone contains a table contains object runs) to a bounded depth (8;
  deeper frames are discarded as malformed). Unknown `<op>`: skip the frame pair,
  keep the payload. Malformed frames (unclosed at stream pressure boundaries,
  oversized args) are abandoned — a renderer never buffers unboundedly waiting
  for a close.

## 3. The vocabulary — semantic, closed, versioned

**The stance that decides whether this rots: programs emit MEANING; renderers own
STYLE.** No Beacon frame ever names a font, a size, or a color. The renderer maps
semantics to typography through its stylesheet — Halcyon to proportional
DejaVu / mono Cornucopia / the light Bonfire complement; a future consumer to
whatever it likes. The moment the vocabulary drifts toward HTML-general, we have
rebuilt TermKit's grave; v1 is deliberately about a dozen forms, and growth
requires a version bump plus scripture amendment here.

**v1 — structure:**

| Op | Args | Meaning |
|---|---|---|
| `zone` + `mark` | `zone k=prompt \| command \| output`; `mark k=exit;code=<i64>` | The transcript structure (the OSC 133 analog): where a prompt, an entered command, and its output begin/end, and the exit-status completion mark (§12.2 normative forms). Emitted by the *shell* (ut), not by programs. |
| `table` | column spec: per-column `l\|r\|c` alignment, optional `hdr` first-row flag | A table. Cells... |
| `row`, `cell` | — | ...delimited by `row`/`cell` frames wrapping the plain cell text. The plain-stream realization (the payload between frames) is the aligned, whitespace-separated form. |
| `hdr` | `level=1..3` | A heading. |
| `rule` | — | A separator (self-closing). |

**v1 — inline:**

| Op | Args | Meaning |
|---|---|---|
| `em` | `class=emph \| strong \| dim \| code` | Emphasis by class, never by face. |
| `obj` | `type=path \| pid \| url \| commit \| user`, `ref=<canonical>` | **A presentation**: this run of text presents an object of `type`, canonically named by `ref`. For `type=path`, `ref` is the cleaned absolute 9P path. The run becomes mouse-sensitive in a rich renderer (§7). |

The `obj` op is the Genera payoff and the reason Beacon exists; everything else
is furniture around it.

## 4. Tiers, negotiation, and the emission gate

Three capability tiers, **advertised by the renderer, never sniffed by
programs**:

| Tier | Advertised by | Programs emit |
|---|---|---|
| `none` | serial host terminal (mode 1 / mode 3 deployments) | plain bytes + today's Bonfire SGR where the color discipline allows |
| `cells` | Aurora | the existing visual language *unchanged* — Bonfire SGR + box-drawing furniture (`COREUTILS-THYLACINE-DESIGN.md`); a later Aurora may realize `table` frames as box-drawing itself (§9) |
| `rich` | Halcyon | Beacon frames; no SGR inside Beacon-structured output (one source of truth — the stylesheet) |

Mechanics:

- **Advertisement** rides the console machinery the way winsize does (ARCH
  §23.5.3 precedent): the renderer declares its tier; the shell reads it and
  exports it to children as an environment variable (name provisional:
  `BEACON=none|cells|rich`), exactly as TERM propagates today.
- **The gate is two-condition**: emit above `none` iff (a) the fd is an
  interactive console/pts — **which requires `SYS_FD_DEVCLASS`**
  (`docs/SYS-FD-DEVCLASS-SPEC.md`), the owed kernel fix that also unparks the
  coreutils' `--color=auto`; the Beacon arc **pulls it forward** as a real
  dependency (the chunk-completeness rule) — and (b) the advertised tier says
  the sink renders it. One kernel mechanism serves both gates.
- **The color discipline is inherited wholesale** and extended by one clause:
  the payload/presentation/diagnostics split of
  `COREUTILS-THYLACINE-DESIGN.md` governs Beacon identically — **filter and
  data-payload tools (cat, sort, cut, wc, hexdump, ...) emit no Beacon at any
  tier, ever.** A semantic frame in a data plane is the same corruption as a
  color code in one.

## 5. Transport — proven, not hoped

Beacon adds **zero transport machinery**. The frames ride the byte stream, and
every hop is already certified transparent:

- **PTY**: I-20's byte-conservation invariant (the PTY-1 kernel seam + ptyfs)
  guarantees frames cross master/slave unaltered.
- **Console**: the cons drain/feed renderer backend (LS-8 / Tapestry G-4) is
  byte-transparent by construction.
- **Degradation**: Aurora's VT parser already consumes and discards unknown OSC;
  so do iTerm2/WezTerm/etc. in console mode. The `none`/`cells` behavior ships
  in software that already exists.
- **Known caveat**: terminal multiplexers (tmux under a pts) filter unknown OSC
  toward their own host terminal — in mode-3 (headless/SSH) deployments the tier
  is `none`/`cells` anyway, so nothing is lost; native session multiplexing on
  Aurora/Halcyon is pane-level and does not re-serialize the stream.

## 6. ut's obligations — a relay, not a transformer

The shell's role is deliberately minimal; **ut never parses, transforms, or
re-emits child output**:

1. **Propagate** the tier to children (the environment variable, §4).
2. **Emit its own zones**: `zone k=prompt` / `zone k=output` (k=command
   RESERVED) plus the `mark k=exit;code=<i64>` completion point (the 12.2
   registry's normative forms) around its REPL cycle. This is the entire data model the
   Halcyon transcript needs — command blocks, "select a past command, tweak,
   resubmit," and per-entry exit badges all read off the zone stream. (Free side
   effect: OSC-133-speaking host terminals get native block marks from a
   Thylacine console session.)
3. **Pass through untouched** — the REPL echo path must not mangle frames mid-
   sequence.

## 7. Presentations and verbs — the executable-text unification

A rich renderer makes every `obj` run mouse-sensitive:

- **The verb table** is a plumber-style rules file (provisional:
  `/lib/beacon/verbs` system tier + `$home/lib/beacon/verbs` session tier — the
  aurora-config two-tier precedent) mapping `type` → the verbs offered (for
  `path`: open, edit-in-nora, stat, cd-here, ...). A context menu on an `obj`
  run *is* the verb list for its type (`HALCYON.md` §6 owns the menu UX).
- **One action mechanism, three heritages**: acme executes *selected* text by
  inference; Genera executes *presented* objects by annotation; the tag line
  (`TAPESTRY.md` §14) executes *typed* text. In Thylacine all three converge on
  the same dispatch — text + (inferred or annotated) type → verb — so the
  transcript, the tag bar, and the context menu share one rules engine.

**The security clause (binding, and the whole of it):** a Beacon frame can only
ever change how bytes *look* and what a *user click offers* — **never execute,
never elevate, never alter input routing, never trigger any action without a
user gesture.** Three corollaries, each closing a real attack:

- **Anti-clickjack**: a verb menu always displays the *resolved* `ref` it will
  act on — text says one thing, `ref` says another, the user sees the ref.
- **Untrusted zones**: any program (or a `cat`-ed hostile file) can emit `zone`
  frames; zones are rendering *hints* with no authority — the transcript may
  look odd, nothing more. The trusted path (I-27, SAK) is entirely outside
  Beacon's reach.
- **Bounded parse**: frame args are length-capped; the nesting bound and the
  abandon-on-malformed rule (§2) hold the parser to O(1) state per frame.

## 8. The emission library and the migration

- **`libthyla-rs` gains a `beacon` module**: programs describe output once (the
  structural model — tables, runs, objects) and the library realizes it per tier
  — `rich` → frames, `cells` → the box+SGR emission the coreutils do *today*
  (that code path relocates into the library; the visual language is preserved
  verbatim), `none`/`--color=never` → plain bytes.
- **First emitters** (the value-ordered sweep, not a big bang): `ut` (zones),
  `ls`, `grep` (match runs + `obj path` on filenames), `ps` (`obj pid`),
  `stat`. The remaining coreutils convert as touched.
- Pouch/phenotype/foreign programs emit whatever they emit — plain VT renders
  plain, exactly as today. Beacon is native-first by construction.

## 9. The agentic payoff

A Beacon stream is machine-readable ground truth: the agent that today OCRs a
screendump can read *what the output meant* from the frames — the
structure-vs-pixels oracle pairing of `TAPESTRY.md` §16, extended from layout to
content. A dev/test-gated per-pane Beacon dump (the #880 strip-for-production
class, like the capture files) is the designed hook; it lands when the rich
renderer does.

## 10. Deferred / rejected

- **Deferred**: Aurora's box-drawing realization of `table` frames (Aurora phase
  1 ignores Beacon; the cells tier keeps today's emission); degrade-to-256/16
  color (already a named seam in the coreutils doc); additional `obj` types
  (each is a vocabulary amendment here).
- **Rejected**: structure-in-the-pipe (breaks POSIX composability; TermKit);
  out-of-band side channels (fragile association, dies at every existing hop);
  typography in the vocabulary (stylesheet inversion); Beacon-carried pixels
  (Tapestry owns pixels).

## 11. Naming rationale (locked) + status

**Beacon** = the signal fire: structured light that *carries meaning*, relayed
unchanged through every station, read only at the destination — which is
precisely the wire contract (§5). It completes the light family: **Aurora** the
dawn, **Halcyon** the day, **Bonfire** the palette, **Beacon** the signal.
(User-chosen 2026-09-01 from the candidate set; "Ember" was disqualified — it is
already a Bonfire palette role.)

- **2026-09-01**: scripture adopted (this doc; born in the Halcyon kickoff
  design conversation, `docs/HALCYON.md` §1). No code. Implementation order
  lives in HALCYON.md §11: the kernel gate (`SYS_FD_DEVCLASS`) + the `beacon`
  library + ut zones + the first emitters form the opening chunk family, and are
  useful to Aurora/serial users immediately (the `--color=auto` unparking rides
  along).
- **2026-09-01 (same day)**: §12 added — the concretization design pass
  (ground-truthed against the tree; implementation-grade for H-1).

---

## 12. The concretization design pass (2026-09-01; implementation-grade)

> Added the same day as adoption, by the operator-directed design pass whose
> explicit bar is: **a session on a lesser model must be able to build H-1
> from this section without re-deriving any decision.** Every "exists today"
> claim below was verified against the tree on 2026-09-01 (file:line cited),
> not recalled. Where a decision is deliberately left to the implementer, it
> says so and bounds the choice.

### 12.0 Ground truth (verified 2026-09-01)

| Fact | Where | Consequence |
|---|---|---|
| Aurora's VT parser swallows unknown OSC today: states `Osc`/`OscEsc`, a 256-byte `osc_buf`, oversize sets `osc_over` and the frame is discarded at the terminator; ST (`ESC \`) and BEL both terminate; non-`7770` OSC is swallowed as termination-detect-only | `usr/lib/vt/src/lib.rs:153-154, 214-226, 342-362` (the shared crate H-2a extracted from `usr/aurora/src/vt.rs`) | The `cells`/`none` degradation ships in software that already exists; ZERO Aurora work in H-1. |
| **OSC 7770 is already Thylacine-private** — aurora's config-push channel (`OSC 7770;aurora;<key>;<value>`) | `usr/lib/vt/src/lib.rs:214` | In-tree precedent for a private OSC number; Beacon takes a distinct one (1936). Never reuse 7770. |
| The consctl verb pattern: `winsize <cols> <rows>` is parsed with staged values + atomic whole-write reject-on-malformed; readback appends `winsize <c> <r>\n` to the ctl render line; winsize resets to unset on renderer detach | `kernel/cons.c:96-97, 1412, 2054-2081, 2161-2177` | The `beacon <tier>` verb is a sibling: same staging, same atomicity, same readback extension, same reset-on-detach. |
| `SYS_FD_DEVCLASS` is spec'd implementation-ready: syscall **80**, returns `struct Dev.dc` as a positive byte, `-T_E_BADF` otherwise; handler sketch + dc table + test plan in the spec | `docs/SYS-FD-DEVCLASS-SPEC.md` (whole) | H-1's kernel half implements that spec as written. One open decision is bound below (§12.4). |
| The coreutils presentation layer is already a shared **pure `no_std + alloc`, libthyla-rs-free, host-tested** crate: `boxd.rs` (162 ln), `color.rs` (92), `palette.rs` (41), plus backend-gated `meta`/`ui`/`usage` | `usr/coreutils/src/lib.rs` (the crate doc states the pattern + the color discipline) | The Beacon crate follows the identical pattern, and the cells realization RELOCATES these modules (§12.5). |
| ut's REPL zone points: `Repl::draw_prompt` emits the prompt (`usr/utopia/libutopia/src/repl.rs:381`); the evaluator owns `$status` (`repl.rs:346`; a non-zero status does not end the session, scripture 8.9) | `usr/utopia/libutopia/src/repl.rs` | The four zone hooks land in exactly two places: around `draw_prompt`, and around the accept→eval arm (§12.6). |
| No Rust-std-on-pouch lane exists (no musl Rust target anywhere in `tools/build.sh`) | grep, 2026-09-01 | Beacon's parser/emitter must be `no_std + alloc` — and is (§12.5). |

### 12.1 The wire grammar (v1; normative)

```
frame      = OSC "1936" SEP version SEP op *( SEP arg ) ST
close      = OSC "1936" SEP version SEP "/" op ST
OSC        = ESC "]"                ; 0x1B 0x5D
SEP        = ";"                    ; 0x3B
ST         = ESC "\"                ; 0x1B 0x5C   (parsers also accept BEL 0x07)
version    = "v1"
op         = "zone" / "table" / "row" / "cell" / "hdr" / "em" / "obj"   ; paired
           / "mark" / "rule"                                            ; point (no close)
arg        = key "=" value
key        = 1*( %x61-7A )          ; lowercase a-z
value      = *( %x20-3A / %x3C-7E / pct )   ; printable ASCII minus ";", plus escapes
pct        = "%" HEXDIG HEXDIG      ; percent-escape
```

Normative rules, each load-bearing:

1. **Payload bracketing**: for paired ops, the annotated text lies BETWEEN the
   open and close frames, as ordinary stream bytes. A point op (`mark`,
   `rule`) wraps nothing. **Stripping every frame from a stream MUST yield
   byte-exactly the `none`-tier emission** — this is the testable property
   (§12.8 P1), not a slogan.
2. **Escaping**: inside a `value`, the bytes `%` `;` and anything outside
   0x20–0x7E are percent-escaped (`%25`, `%3B`, `%XX`). Values are ASCII on
   the wire; UTF-8 content percent-escapes its high bytes. Keys never escape.
3. **Caps** (parser-enforced, discard-don't-buffer): whole frame ≤ 2048
   bytes; one value ≤ 1024 (a path); ≤ 8 args per frame; nesting depth ≤ 8.
   A frame exceeding a cap is consumed to its terminator and dropped
   (aurora's `osc_over` idiom, `vt.rs:341`). An unclosed paired op at
   end-of-stream or at a zone boundary is auto-closed by the renderer —
   annotations never buffer text indefinitely.
4. **Unknown handling**: unknown `version` → drop every `1936` frame, keep
   payload. Unknown `op` → drop that frame pair (a lone unknown close is
   dropped silently). Unknown `key` → ignore the arg, keep the frame. This is
   the forward-compat contract; v2 may add ops/keys without breaking v1
   renderers.
5. **Nesting legality** (renderer may flatten illegal nesting, never error):
   `zone` ⊃ anything; `table` ⊃ `row` ⊃ `cell`; `cell`/`hdr` ⊃ inline
   (`em`/`obj`); inline ops nest nothing. `table` direct children other than
   `row` are illegal.

### 12.2 The v1 op registry (normative arguments)

| Op | Kind | Args | Notes |
|---|---|---|---|
| `zone` | paired | `k=prompt \| command \| output` | Emitted by the SHELL only (§12.6). `prompt` wraps prompt + line-editing echo (the OSC 133 A..C region); `output` wraps the command's whole output. `command` is RESERVED in v1 (ut echoes into the prompt zone; a shell that re-echoes the accepted line may wrap it later). |
| `mark` | point | `k=exit;code=<i64>` | The command-completion mark (OSC 133 `D;exit` analog). Emitted as the LAST child of the `output` zone, immediately before its close (amended at H-1c-2, §12.5 deviation 8: containment beats a floating between-zones mark — the renderer needs no backward association). |
| `table` | paired | `cols=<spec>;hdr=0\|1` | `<spec>` = one char per column, `l`/`r`/`c` alignment (e.g. `cols=lrrl`). `hdr=1` ⇒ the first `row` is a header row. |
| `row` | paired | — | Direct child of `table`. The row's payload newline is ordinary stream bytes after the close. |
| `cell` | paired | — | Direct child of `row`. The plain-stream realization between cells (spaces/padding) is payload OUTSIDE the cell frames — so stripping yields the aligned plain table. |
| `hdr` | paired | `level=1\|2\|3` | A heading. |
| `rule` | point | — | A separator. Plain realization: the emitter's own rule line (payload). |
| `em` | paired | `class=emph \| strong \| dim \| code` | Emphasis by class. `code` implies monospace in every rich stylesheet. |
| `obj` | paired | `type=path \| pid \| url \| commit \| user; ref=<canonical>` | The presentation. `type=path` ⇒ `ref` is the cleaned ABSOLUTE 9P path (the emitter resolves relative names before emitting; a ref the emitter cannot canonicalize ⇒ emit no frame, plain text only). `pid` ⇒ `ref` is the decimal pid. `url`/`commit`/`user` ⇒ ref is the literal. |

**Vocabulary growth policy**: any new op or key is an amendment to this table
plus a version note; renderers already tolerate it (rule 4). Growth toward
layout/typography ops is REFUSED on sight — that was the TermKit failure.

### 12.3 The tier mechanism (consctl verb + environment)

- **The verb**: `beacon none|cells|rich` on `/dev/consctl` — implemented in
  `kernel/cons.c` beside the `winsize` verb, copying its discipline exactly:
  parsed in the staged pass (`kernel/cons.c:2054-2081` shape), atomic
  whole-write reject on a malformed token, readback appended to
  `cons_render_mode`'s line as ` beacon <tier>` (parser parity with the
  `winsize` token, `cons.c:2161-2177`), and **reset to `none` at the
  renderer drain's teardown** (`cons_drain_close` — corrected at
  implementation: the `cons.c:1412` site this bullet first cited is
  `cons_test_reset`, not a detach path; winsize never reset on detach, and
  the tier resets at BOTH the drain close and the test reset). Mint gate: the same
  attached-OR-renderer widening winsize uses (ARCH §23.5.3) — only the
  console renderer (aurora / Halcyon) or the attached owner may set it.
- **The `/dev/beacon` leaf** (REVISED at the H-1 close — this bullet
  originally bound "no new leaf; the consctl readback + the environment
  suffice; revisit only with a named consumer," and the revisit condition
  was met by the round-1 P1): **the consctl readback is structurally
  unreachable for the session shell.** ut's consctl fd is ONE Spoor threaded
  joey → login → ut whose offset every non-positioned mode write advances
  past the ≤67-byte line; devdev is non-seekable (pread/lseek gate on
  `dev->seekable`, the RW-4 R2-F2 narrowing), and a fresh consctl open
  fails the I-27 attach gate (attach never propagates — that is why the fd
  is inherited at all, #94-B). So the tier readback gets the winsize-leaf
  precedent exactly: `/dev/beacon`, ungated read-only, serving
  `beacon <tier>\n` (`cons_render_beacon`; a renderer self-description is
  not a secret). The consctl RENDER-LINE token stays (the renderer's own
  readback + parser parity); the leaf is the consumer-side surface. ut
  fresh-opens it per session (and, at H-2, per prompt — the F10 re-read).
  **F10 AS-BUILT (H-2d-4, `usr/utopia/shell/src/main.rs`)**: the re-read is
  keyed on `Repl::prompt_cycles` (bumped at the accept arm), so it fires
  once per accepted prompt, not per keystroke; on a CHANGED tier it
  re-exports and prints the `ut: beacon <tier> re-exported (renderer
  change)` canary — an unchanged tier is silent (the ls-halcyon E2E pins
  the silence; the H-1 ls-3a leg pins the session-start export).
- **The environment**: at session start (and after `tty:winch`-class renderer
  changes — cheap to re-read), ut reads the consctl render line, parses the
  `beacon` token, and exports **`BEACON=none|cells|rich`** to children
  (name = proposal-of-record; confirm at implementation). Absent token or
  absent variable ⇒ `none`. A pts session under tmux/ssh never sees the verb
  ⇒ `none`/inherited — correct by construction.
- **Aurora sets `cells`** (one line in its consctl bring-up, beside its
  winsize write); the serial/UART backend sets nothing ⇒ `none`; Halcyon sets
  `rich` when it exists. Programs never see a renderer name — only the tier.

### 12.4 The emission gate + `SYS_FD_DEVCLASS` (the kernel half of H-1)

- Implement `docs/SYS-FD-DEVCLASS-SPEC.md` **as written** (syscall 80; the
  handler sketch, the `-T_E_BADF` arm, the ref-transfer `spoor_clunk`
  discipline). Its one open decision is **bound now**: **normalize the
  `/dev/cons` namespace leaf to report `'c'`** (both the `SYS_CONSOLE_OPEN`
  fd and a walked `/dev/cons` fd answer `'c'`), so `is_terminal ≡ (dc=='c')`
  is exact with no special cases. If `devpipe` currently lacks a distinct
  `dc`, give it one (any unclaimed char; document it in the spec's table) —
  the test plan's `pipe != 'c'` assertion must hold structurally, not by
  accident.
- The library gate (pure function of two inputs, trivially testable):

```
effective_tier(env_tier, dc_of_stdout, flag) =
    flag == never              -> None
    flag == always             -> env_tier                 // explicit user override
                                  // (amended at the H-1 close, audit F5: the earlier
                                  //  "floor Cells" clause is DROPPED -- an absent
                                  //  advertisement means no renderer reads frames, so
                                  //  always trusts the advertised tier, never invents one)
    else (auto, the default)   -> if dc_of_stdout == 'c' then env_tier else None
```

- Per-tool flag: `--beacon=auto|always|never`, mirroring `--color=WHEN`
  (COREUTILS-THYLACINE-DESIGN.md). **`--color` keeps governing color; at
  `cells` tier the two gates compose** (color off ⇒ the cells realization
  drops SGR but may keep the box; the existing `never` semantics — drop
  color AND box — are unchanged). The long-parked `--color=auto` flips to a
  real gate in the same commit (its stub is `ls::stdout_is_console()`, a
  `true` stub today — the spec's consumer #1 names the exact swap point).

### 12.5 The crate: `usr/lib/beacon` (the relocation)

- **Pattern**: the proven coreutils-crate shape (`usr/coreutils/src/lib.rs`
  header): pure `no_std + alloc`, ZERO libthyla-rs dependency, host-tested
  (`cargo test -p beacon --target aarch64-apple-darwin`). The syscall-touching
  gate input (`dc_of_stdout`) is passed IN by the caller; libthyla-rs gains
  the thin `fs::fd_devclass(fd)` + `io::stdout().is_terminal()` wrappers per
  the spec.
- **The relocation**: `boxd.rs`, `color.rs`, `palette.rs` MOVE from
  `usr/coreutils/src/` into the beacon crate (they are already pure; the
  crate doc says so) as the cells realization's engine; `usr/coreutils`
  depends on `beacon` and re-exports them so the 51 bins keep compiling with
  a one-line `use` change. `meta`/`ui`/`usage`/`netpump` stay in coreutils
  (backend-gated, tool-specific).
- **API sketch** (binding in shape, not in identifier):

```rust
pub enum Tier { None, Cells, Rich }
pub enum Em   { Emph, Strong, Dim, Code }
pub enum Obj<'a> { Path(&'a str), Pid(u64), Url(&'a str), Commit(&'a str), User(&'a str) }

pub struct Sink<'a> { out: &'a mut dyn Out, tier: Tier }   // Out = the no_std write shim
impl Sink {
    pub fn text(&mut self, s: &str);                        // payload, always
    pub fn em(&mut self, class: Em, s: &str);
    pub fn obj(&mut self, o: Obj, text: &str);              // ref canonical, text as shown
    pub fn hdr(&mut self, level: u8, s: &str);
    pub fn rule(&mut self);
    pub fn zone(&mut self, z: Zone) -> ZoneGuard;           // shell-only ops
    pub fn mark_exit(&mut self, code: i64);
}

pub struct Table { /* cols: alignment spec, hdr flag, rows of cells (text + optional Em/Obj) */ }
impl Table { pub fn realize(&self, s: &mut Sink); }
// realize(Rich)  -> table/row/cell frames wrapping the SAME aligned plain text
// realize(Cells) -> boxd + palette SGR (byte-identical to today's coreutils output)
// realize(None)  -> the aligned plain text alone
```

- **The invariant the API enforces by construction**: every emitting method
  writes the plain payload on every tier and adds frames only at `Rich` —
  so property P1 (§12.8) holds for any program using the API, not just
  well-behaved ones.
- **AS-BUILT deviations (H-1b, within the "binding in shape" latitude),
  recorded**: (1) zones are explicit `zone_open`/`zone_close`, not a scope
  guard — the shell's zones are NOT lexically scoped (the prompt zone opens
  in `draw_prompt` and closes in the accept arm, a different call); (2)
  `em`/`obj` at the **Cells** tier are payload-only — the cells look is the
  bins' existing box+SGR language used directly (object identity is a Rich
  concept), which is also what keeps `realize(Cells)` byte-identical to
  today; (3) an `obj` whose ref exceeds `VALUE_MAX` emits no frame at all
  (plain text only) — a truncated ref would be a wrong ref for the verb
  menu to act on.
- **AS-BUILT deviations (H-1c-2, the emitters)**: (1) the rich arms are
  **ADDITIVE** — the cells/never realizations keep their existing code
  byte-identical, and Rich branches beside them (§12.7's "Table::realize
  replaces its direct boxd calls" was revised: the box IS the cells
  realization and stays); (2) at Rich, SGR is forced OFF in the emitting
  bin (`on = !rich && …`) so beacon-structured output never carries SGR —
  for the annotation-shaped emitters (ls short, grep, stat) that makes
  `strip(rich)` equal the tool's own plain output byte-exactly, asserted
  in-guest on real spawns (coreutil-smoke); (3) **stat's `table` op is
  deferred** — its GNU-shaped block is deliberately not tabular and a
  table realization would break the strip identity; stat rich = the block
  with `obj type=path` on the subject; (4) **`ps` did not exist and was
  built as part of this chunk** (the §12.7 list assumed it): one atomic
  `/ctl/procs` read; verbatim pass-through when unstyled; boxed at cells;
  a beacon table with `obj type=pid` at Rich; parse-failure degrades to
  the verbatim text; (5) `ls -l` at Rich drops the classify suffix (the
  REALM column classifies) and emits the table WITH its header row — the
  rich payload is the aligned header+rows form, not the header-less
  `--color=never` row dump; (6) the `--color=auto` flip swept **all 15
  remaining `stdout_is_console` stubs** (ns/qid/con/pelt/realm/netstat/
  the net tools/weft-bench/httpd/nettest), not just ls — every stub's own
  comment promised exactly this swap — AND the DEFAULTS unified to `Auto`
  across every color-bearing tool (17 flips: the Always presentation tools
  + grep's Never), per COREUTILS-THYLACINE-DESIGN's ordained end-state
  ("both unify to `auto` once SYS_FD_DEVCLASS makes TTY detection real —
  the default is simply color iff a terminal"); (7) obj-ref
  canonicalization is `coreutils::path::abs` (cwd-anchored lexical clean,
  shared with realpath) — a `None` (unreadable cwd) emits no frame; (8)
  **the exit mark is the LAST CHILD of the `output` zone**, not a
  between-zones point (§12.2's `mark` row said "between the output close
  and the next prompt open") — containment beats the OSC-133-style
  floating mark because the renderer needs no backward association to know
  which output block the mark terminates, and the u-repl-test pins the
  as-built order (`mark` → `/zone` → `zone prompt`). The §12.2 row is
  amended to say so.

### 12.6 ut integration (the shell half of H-1)

Two hook sites, both in `usr/utopia/libutopia/src/repl.rs`:

1. **`Repl::draw_prompt` (repl.rs:381)**: open `zone k=prompt` before the
   prompt render, ONCE per prompt display (redraws inside line editing stay
   inside the open zone — do not re-open per keystroke; the completion-strip
   save/restore drawing at repl.rs:395-399 is inside the zone and needs no
   change).
2. **The accept→eval arm** (the path that consumes the accepted line and
   calls the evaluator): close the prompt zone, open `zone k=output`, run the
   command; on completion emit `mark k=exit code=$status` (the evaluator's
   `$status`, repl.rs:346) and close the output zone. The next `draw_prompt`
   opens the next prompt zone.

Rules: ut emits zones **only when its stdout tier gate passes** (§12.4 — a
scripted/piped ut emits nothing); ut NEVER inspects or rewrites child bytes
(children write the console directly; the zone frames land around them from
ut's own writes, which is exactly the OSC 133 shell-integration model); the
`BEACON` export (§12.3) happens at session start in ut's environment setup.

### 12.7 The first emitters (H-1's sweep, in order)

1. **ut** — zones + the export (§12.6). The transcript's entire data model.
2. **ls** — `Table::realize` replaces its direct boxd calls; filename cells
   carry `obj type=path ref=<abs>` (ls already resolves the directory; join +
   clean is available in the coreutils meta layer); the `--color=auto` flip
   rides (§12.4).
3. **grep** — per-match line: `obj path` on the filename prefix, `em strong`
   on the match span. (grep's match highlight is named a presentation surface
   in COREUTILS-THYLACINE-DESIGN's discipline table already.)
4. **ps** — `obj type=pid` on the pid column; `table` for the listing.
5. **stat** — `table` + `obj path` on the subject.

Everything else converts as touched. **The filter tools are excluded by
doctrine** (the color discipline): cat/head/tail/sort/uniq/tr/cut/tee/wc/
hexdump/seq/echo never link the emitting half at any tier.

### 12.8 Test plan (H-1's gates)

- **P1, the strip property (host, per-emitter, cargo)**: for every emitter
  and every fixture: `strip_frames(realize(Rich)) == realize(None)`
  byte-identical, and `realize(Cells)` byte-identical to the pre-relocation
  output (golden fixtures captured BEFORE the relocation commit — the
  behavior-preserving proof).
- **P2, grammar round-trip (host)**: parse(emit(x)) == x over the op
  registry, including escaping edge cases (`;`, `%`, UTF-8 payload refs) and
  every cap boundary (2047/2048/2049-byte frames; depth 8/9).
- **P3, the parser-robustness corpus (host)**: truncated frames, unknown
  ops/keys/versions, illegal nesting, BEL vs ST termination, interleaved
  SGR — parser never panics, never buffers past a cap, always yields the
  payload. (This corpus is reused by Halcyon's renderer tests at H-3.)
- **Kernel (in-guest)**: the spec's own test plan verbatim —
  `fd_devclass(console)=='c'`, `pipe != 'c'`, `dev9p file == '9'`,
  `bad fd < 0` — plus the normalization case (walked `/dev/cons` == `'c'`).
- **E2E (expect, the LS-CI family)**: interactive `ls` boxed+colored;
  `ls | cat` byte-clean (the auto flip's proof); `ut` under a `rich`-forced
  consctl shows zone frames in the captured stream (assert on the raw
  serial bytes — the host terminal swallowing them visually is itself the
  degradation demo); aurora boot unchanged (its parser swallows 1936 —
  ZERO-diff screendump against a pre-Beacon baseline).

### 12.9 Audit + scripture-sync obligations (owed at H-1's close, not now)

- `SYS_FD_DEVCLASS` = a NEW syscall surface → its own `docs/AUDIT-TRIGGERS.md`
  row + CLAUDE.md index line + a focused round (small scope: the handler +
  the dc normalization; prosecute the ref-transfer discipline and the
  no-authority claim).
- The consctl `beacon` verb rides the existing LS-8 cons row (an addendum to
  that row, same round).
- The beacon crate itself is NOT kernel-audit-bearing (pure userspace
  formatting) — its rigor is P1–P3 + the fuzz corpus; Halcyon's PARSER is the
  audit-bearing consumer later (H-arc audit, format-fuzz class like image
  decode).
- `docs/SYS-FD-DEVCLASS-SPEC.md` gains the as-built addendum (number, dc
  table deltas, the normalization decision) in the same commit as the
  syscall.

### 12.10 Open items (each named, none blocking H-1's start)

1. DONE at the H-1 close (see §2: ADOPTED 2026-09-01). Was: OSC **1936** — confirm no collision at first implementation (one grep of
   the vendored terminal-adjacent sources + the SDL/mesa tree; then pin it in
   §2 as adopted).
2. The env name **`BEACON`** — confirm (vs `TH_BEACON`) at implementation;
   the parser accepts only the chosen one.
3. `devpipe`'s dc char, if new — record in the spec's table.
4. Whether `grep`'s obj-path ref uses cwd-relative display text with an
   absolute ref (recommended: yes — text as shown, ref canonical; §12.2's
   obj row already requires the canonical ref).
