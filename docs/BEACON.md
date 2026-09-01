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
  terminal we can find (iTerm2 owns 1337, urxvt 777, ConEmu 9). *Proposal of
  record; confirm-at-first-implementation.*
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
| `zone` | `prompt \| command \| output \| exit=<code>` | The transcript structure (the OSC 133 analog): where a prompt, an entered command, and its output begin/end, and the exit status. Emitted by the *shell* (ut), not by programs. |
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
2. **Emit its own zones**: `zone prompt` / `zone command` / `zone output` /
   `zone exit=<code>` around its REPL cycle. This is the entire data model the
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
