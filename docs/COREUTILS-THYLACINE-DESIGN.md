# Coreutils: the Thylacine visual language

Design record for the coreutils "make it exotic" arc (user-directed 2026-06-16).
Aux-authored (`usr/apps/`); the main agent owns folding the binding parts into
`docs/` (the scripture reconciliation is owed -- see "Scripture change" below).

## The decision

The old scripture line -- *"coreutils have disciplined non-colored output"* -- is
**retired**. Thylacine coreutils now speak a shared visual language: the Bonfire
palette (the same RGB nora + ut use), box-drawing furniture for listings, and
Thylacine-specific columns that foreground what is exotic about the system (the
9P qid, the namespace realm). The goal, in the user's words: *"make it
immediately look that we're not in unix, but something far more exotic."*

The discipline is not gone -- it is **relocated**. See "The color discipline".

## The color discipline (load-bearing)

Color belongs on **presentation** and **diagnostics**, NEVER on a data
**payload** a pipe consumes.

- **Presentation tools** (ls, stat, id, whoami, ns, which, env, date, uname,
  grep's match highlight) -- colored, boxed, columned. The interactive surface.
- **Diagnostics** (every tool's `eprintln!` errors) -- may use color (e.g. the
  program name in ember) since stderr is not a pipe payload.
- **Filters / data payloads** (cat, head, tail, sort, uniq, tr, cut, tee, wc's
  numbers, hexdump, seq, echo) -- stay **byte-clean**. A colored payload corrupts
  `tool | tool`. These tools do NOT pull in the palette for their output.

This is why the discipline was never really about "no color" -- it was about not
corrupting the data plane. Coloring the *presentation* plane honors that while
making the system exotic.

## The palette (Bonfire, as truecolor SGR)

RGB matches `nora::theme` (docs/UTOPIA-VISUAL.md U-2) so the editor, the shell,
and the tools share one identity. Rendered as `\x1b[38;2;R;G;Bm` (the QEMU serial
host TTY + the future Aurora framebuffer render truecolor; degrade-to-256/16 is a
seam).

| Role        | Bonfire | Used for                                   |
|-------------|---------|--------------------------------------------|
| `FG`        | e4ddd8  | body text                                  |
| `DIM`       | 9a8f8a  | furniture: box rules, column headers       |
| `EMBER`     | e07840  | accent: titles, counts, the program name   |
| `SLATE`     | 8a9ac8  | **directories**                            |
| `GREEN`     | b8d098  | **executables** (any `x` bit)              |
| `VIOLET`    | a898c8  | **grafts** (live kernel namespaces)        |
| `GOLD`      | c8a882  | **devices** (char devices)                 |

## The color gate

**Reliable cooked-mode TTY detection is IMPOSSIBLE on the current kernel surface**
(confirmed by reading the kernel + a boot, 2026-06-16). A console fd
(`SYS_CONSOLE_OPEN`) and a pipe fd are byte-for-byte indistinguishable from
userspace:
- Both are **`KOBJ_SPOOR`** handles (`sys_console_open_for_proc` installs a
  KOBJ_SPOOR R|W; `devpipe_attach` returns a Spoor).
- **Neither implements `stat_native`** (devcons + devpipe are both absent from the
  stat_native set), so `fstat(1)` **fails on both**.
- **Neither stamps a namespace `Path`** (no walk), so `fd2path(1)` returns **0 on
  both** (an empty-path success).
- `io::stdout_is_live()` is a liveness probe -- true for a pipe too.
- Kaua's CPR handshake (`kaua::query::terminal_size`) needs **raw mode +
  console-attach** (a TUI prerogative); a cooked-mode coreutil can't use it, and a
  per-invocation cursor-park/CPR round-trip is far too heavy for `ls`. (And a
  reply only proves *a* terminal somewhere, not that fd 1 is it.)

Ground truth (a boot): `la | cat` keeps the color+box because the default is
`always` AND nothing can detect the pipe.

So: `--color=WHEN` with `WHEN` in `always | never | auto`. **Default = color on**
(the exotic look the user wants). `never` is the clean escape hatch (drops color
AND the box -> a parseable pipe payload). `auto` is **parked == on** until the
kernel surface lands; it is the wiring point.

### The owed kernel fix: `SYS_FD_DEVCLASS` (main-track)

The clean, permanent fix is a tiny read-only introspection syscall the kernel
should expose -- aux specs it; the main agent adds it (kernel is off-limits to
aux). **User-chosen 2026-06-16: `SYS_FD_DEVCLASS`** (return the fd's Dev class
char) over a minimal boolean `SYS_ISATTY` -- the richer call also sharpens the ls
REALM column + powers the proposed `realm`/`qid` tools. Full implementation-ready
spec: **`usr/apps/SYS-FD-DEVCLASS-SPEC.md`** (ABI, the dc-char table, the kernel
sketch, the libthyla-rs wrapper, the consumers).

When it lands, `ls::stdout_is_console()` becomes `io::stdout().is_terminal()` and
the ls default flips `Always -> Auto`. Until then, the interactive look is
gorgeous-by-default and `--color=never` gives a clean pipe.

## ls -- the centerpiece (boxed; realm + qid)

User-chosen: **boxed header**, **both** the realm and qid columns.

- **Plain `ls`**: one name per line, color-coded by kind, with a classify suffix
  (`/` dir, `*` exec). The immediate "not unix" hit.
- **`ls -l` (= ll / la)**: a box framed by the directory path + an item count
  (top border) and a graft legend (bottom border, only when grafts are present),
  with columns `MODE OWNER SIZE REALM QID NAME`.
- **REALM** (the Thylacine column): the namespace *nature* of each entry --
  `fs` (a real filesystem object), `dev` (a char device), `graft` (a live
  kernel-served namespace mount). Derivation is honest and reliable: a graft is
  exactly an entry whose `fstat` **fails** (the synthetic Dev has no
  `stat_native`) while `readdir` reported it -- so the old ugly `??????` row
  becomes a first-class, explained `graft`.
- **QID** (the exotic identity): the 9P qid the kernel knows the object by --
  `{t}:0x{path}` where `t` is `d`/`f`/`c`. Grafts show `-` (fstat doesn't cross
  the mount, so there is no qid to report). This is Plan-9 made visible: unix
  hides the inode behind `-i`; Thylacine foregrounds the qid.
- **OWNER**: `system` for `T_PRINCIPAL_SYSTEM`, else the numeric uid. There is no
  uid->name service yet (LS-K seam; even `whoami` prints numeric) -- the column
  auto-upgrades to names when it lands.
- **SIZE**: bytes (files); `-` for dirs/devs/grafts (size is not meaningful);
  `-h` for human-readable (1024-based).

## Other tools (the same language, applied with discipline)

- **grep** (DONE): highlight the matched substring (bold ember) + slate filename
  / moss line-number / dim `:` separators. The matched LINE is a payload, so grep
  **defaults to color-OFF** (byte-clean -- the INVERSE of ls): `--color` opts in
  interactively, and it flips to `auto` when `SYS_FD_DEVCLASS` lands. Only the
  match span + furniture are wrapped, and only when on.

**The default-by-nature rule**: a **presentation** tool (ls, stat, id, ...)
defaults color-ON (you look at it; piping is rare); a **payload** tool (grep, and
any future filter that gains optional color) defaults color-OFF (it is usually
piped; `--color` opts in). Both unify to `auto` once `SYS_FD_DEVCLASS` makes TTY
detection real -- then the default is simply "color iff a terminal."
- **stat / id / ns / which / env / date / uname**: labeled, lightly colored
  presentation (the label dim, the value in the kind color); a boxed form where a
  multi-field record warrants it.
- **The filters stay clean** (see the discipline).

## Thylacine-specific tool ideas (the user invited these)

- **`realm <path>...`** -- print each path's realm (fs/dev/graft) + which Dev
  serves it. Folds the ls REALM column into a standalone query.
- **`qid <path>...`** -- print the 9P qid of each path (type:vers:path). The
  Plan-9 identity, standalone.
- **`pelt`** (BUILT 2026-06-16) -- a `tree`-like recursive lister that colors by
  kind and STOPS at graft boundaries (a graft is shown + marked `(graft)` but
  never descended, so it never walks a live kernel namespace as if it were
  disk). Flags: `-a` dotfiles, `-d` dirs-only, `-L N` max depth, `--color`.
  Built native on `coreutils::{meta,color,palette}`; the striped │/├/└ rails
  are the thylacine's pelt. A `tree`-style count footer (N directories, M
  files, K grafts). Added to `tools/build.sh` usr_rs_bins.

Recorded as ideas; built as the arc proceeds, user-steered.

## Standards audit (unix users feel at home)

### Finding 1: `~` is a small, tractable `ut` fix (NOT a coreutil change) -- DONE (2026-06-16)

The user's seed (`cp ~/foo` fails). Definitively traced (2026-06-16):

- In unix the **shell** expands `~` -> the home before the tool sees it; `cp`
  correctly treats a literal `~/foo` as a path that does not exist. Making `cp`
  expand `~` would be non-standard (and would surprise a unix user the other way),
  so this is **NOT a coreutil change**.
- It is a **`ut` change, and a small one** -- and the earlier "blocked by no
  envp" note was WRONG. `ut` already HAS the home: login forwards
  `--home /home/<user>` (since there is no kernel envp), `ut` parses it into
  `$home`, and uses it for the `~`-abbreviated prompt + `cd`-with-no-arg
  (`shell/src/main.rs` `parse_home_arg`). What is missing is **`~`-expansion in
  command-argument word evaluation**: `ut`'s lexer emits a `Tilde` token but the
  parser only consumes it as arithmetic bitwise-NOT; a word-leading `~` / `~/...`
  is never rewritten to `$home`.
- **Fix (user-authorized 2026-06-16; implement as a CAREFUL focused pass with
  tests -- it touches the audited parser core, so not rushed)**:

  Traced precisely:
  - Lexer: `~` -> `TokenKind::Tilde` (lexer.rs:203), always its own 1-byte token;
    `~/foo` lexes as `Tilde` + span-adjacent `Word("/foo")`.
  - `Tilde` is NOT in `parse.rs::is_value_token` (1400), so a `~` in argv position
    today does not parse as a word at all (the seed's true cause -- a parse-level
    miss, not "cp gets a literal ~"). Arith uses a SEPARATE
    `expr.rs::is_value_token_kind` (1318), so the command-context change does not
    touch arith's bitwise-NOT `~`.
  - Words: `ast.rs::Word` = `Single(Token)` | `Concat(Vec<Token>)`; `Concat` is
    today only the `^` operator (parse_word, parse.rs:581). Eval: `eval_word` /
    `eval_value_token` (eval/stmt.rs:1542) turn a Word into a Value. `$home` lives
    in `Env` (parsed from login's `--home`).

  Plan:
  1. Add `TokenKind::Tilde` to `parse.rs::is_value_token` (command context only).
  2. In `parse_word`, a LEADING `Tilde` (at a word START -- i.e. NOT span-adjacent
     to a preceding value token, so `foo~bar` stays mid-word/literal) consumes its
     span-adjacent following value tokens into a `Concat([Tilde, suffix...])`; a
     bare `~` is `Single(Tilde)`. Reuse `Concat` -- no new AST variant.
  3. In eval, `eval_value_token(Tilde)` -> `$home`; the `Concat` branch then
     naturally yields `$home` + suffix (`~/foo` -> `<home>/foo`).
  4. Tests: `~`, `~/foo`, `cd ~`, `echo ~/x`; quoted `"~"` stays literal (a Str
     token, never `Tilde` -- already safe); mid-word `foo~bar` unchanged (rare);
     arith `~x` unaffected. `~user` is a v1.x item (needs the name service).

  Bounded + self-contained, but real (parser + eval + tests across 3 files) ->
  done as its own careful pass, not at the end of a long coreutils session.

  **As-built (2026-06-16).** Four files, all in the main-track `usr/utopia`
  workspace (scoped authorization):
  - `parser/parse.rs`: `Tilde` added to `is_value_token` (command context
    only -- arith's `is_value_token_kind` untouched, so `(( ~x ))` stays
    bitwise-not). `parse_word` gains a maximal span-adjacent absorb: once a
    `~` appears in a word, it glues ALL span-adjacent value tokens into one
    `Concat`, so `~/foo`, `~/$dir`, and the literal `a~b` are each ONE argv
    element. Plain adjacency without a `~`/`^` (`$a$b`) still does NOT join
    (no regression).
  - `parser/ast.rs`: `Word::Concat` doc notes `~` as a second word-fuser.
  - `eval/stmt.rs`: `tilde_home` (-> `$home`, or literal `~` when `$home`
    unset so `~/foo` never collapses to `/foo`, and never zero-element so a
    Concat can't annihilate). `tilde_leads_home` decides expansion by
    POSITION + shape: a leading `~` expands ONLY in the bare `~` and `~/...`
    forms; `~name` / `~$x` (the POSIX username form) stay LITERAL (no name
    service at v1.0 -- so `cat ~backup` and a file named `~tmp` are not
    silently rewritten). A non-leading `~` (`a~b`) is always literal.
  - `eval/stmt.rs` + `parser/parse.rs`: 13 `#[cfg(test)]` unit tests (parse
    shape + eval value). NOTE: libutopia unit tests are documentation-grade
    -- they do NOT run in CI (`no_std`; `docs/reference/94-utopia-eval.md`
    confirms "validation happens at boot via /u-test"). So the LIVE
    regression is `usr/u-6-test::flow_tilde_expand` (in-VM), which captures
    `$(echo ~)` / `$(echo ~/foo)` / `$(echo a~b)` / `$(echo ~user)` /
    `$(echo ~/$sub)` stdout and asserts the expansions.

  Verified by `cargo build` (the aux ceiling) across libutopia + utopia-shell
  + u-6-test for `aarch64-unknown-none`; zero new clippy warnings. The in-VM
  `u-6-test` run is owed to the main agent's harness (aux cannot boot QEMU).

  **v1.x seams (documented, not bugs):** `~/foo*` does not glob (the `~`
  makes it a `Concat`, and a `Concat` is rc-literal -- the pre-existing
  scripture rule, `usr/utopia/libutopia/src/eval/stmt.rs` `glob_candidate`);
  `~user` needs the name service; `~+`/`~-` (PWD/OLDPWD) unsupported;
  `~"/q"` (quoted suffix right after a leading `~`) stays literal.

  **Owed doc (main agent):** `docs/reference/94-utopia-eval.md` should gain a
  "tilde expansion" subsection (the command-word `~`/`~/`/`~name` rules
  above). Recorded here + in DOC-GAP-REPORT.md; aux does not edit
  `docs/reference/*`.

### Finding 2: flag coverage (the coreutil-side, in-scope work)

Audited the short-flag coverage vs what a unix user reaches for. Notable gaps:

| Tool   | Has              | Common missing (priority)                          |
|--------|------------------|----------------------------------------------------|
| cp     | -r/-R            | **-v -n** (high), -i -f -p -a                       |
| mv     | (none)           | **-v -n** (high), -i -f                             |
| rm     | -r/-R -f         | -v -i (med), -d                                     |
| mkdir  | -p               | -v -m (med)                                         |
| cat    | (none)           | -n (number lines, med), -A -E -T -s -b              |
| head   | -n/-N            | -c (bytes), -q -v                                   |
| tail   | -n/-N            | -c, -q -v (-f follow = hard, defer)                 |
| sort   | -r -n -u         | -k -t -f -b (field/key sort)                        |
| uniq   | -c               | -d -u -i                                            |
| tr     | -d               | -s (squeeze) -c (complement)                        |
| grep   | -i -v -n -c      | -r (recursive) -l -o -w -E/-F                       |
| ls     | -l -a -h -F -1 --color | good                                         |

Combined short flags already parse where a tool has a char-flag loop (`cp -rv`
just needs `v` to be a known flag). `-` as stdin is honored by the filter tools
(cat/grep/...) already.

**Fix order** (each a small batch): cp + mv `-v`/`-n` first (the most-reached-for,
easy + safe); then rm/mkdir `-v`, cat `-n`; then the richer ones (sort `-k`, tr
`-s`/`-c`, grep `-r`) as separate chunks. `-i` (interactive prompt) is deferred --
it needs a stdin y/n read and is lower interactive value than `-v`/`-n`.

**Done so far:**
- cp + mv `-v` / `-n` / `-f` (DONE, prior session).
- rm `-v` (`removed '<path>'` / `removed directory '<path>'`, GNU format, in
  tree order); mkdir `-v` (`mkdir: created directory '<path>'`, only the
  components actually created under `-p`); cat `-n` (number all lines) + `-b`
  (number nonblank, overrides `-n`) -- the GNU `%6d<TAB>` gutter, continuous
  across operands, streamed byte-clean via `BufRead::read_until`. (2026-06-16.)
- Minor intentional delta: `mkdir -pv` reports the resolved ABSOLUTE component
  path (mkdir_p resolves relative inputs against cwd for create correctness),
  where GNU echoes the as-typed relative form -- unambiguous, not a home-feel
  blocker. Verified `cargo build` + clippy; in-VM exercise owed to the harness.
- **Batch 2a (text filters, 2026-06-16):**
  - cat `-E` (show ends `$`), `-T` (tabs `^I`), `-v` (nonprinting `^X`/`M-`),
    `-A`=`-vET`, `-e`=`-vE`, `-t`=`-vT`, `-s` (squeeze blank runs), `-u`
    (no-op). The line-oriented path now applies squeeze -> number -> content
    transform; the no-flag path stays the byte-clean io::copy stream.
  - tr `-s` (squeeze; SET2 when translating/deleting, else SET1), `-c`
    (complement SET1), AND backslash escapes in SETs (`\n \t \r \a \b \f \v
    \\ \NNN`) -- needed for `-s`/`-c` to be useful (`tr '\n' ' '`,
    `tr -cs 'a-zA-Z' '\n'`). The full mode matrix (translate / delete /
    squeeze-only / delete+squeeze / translate+squeeze, each x complement).
  - uniq `-d` (duplicated only), `-u` (unique only), `-i` (case-insensitive
    compare; first line of a group emitted as-is). `-d`+`-u` selects nothing.
- **Batch 2b (sort keys, 2026-06-16):** sort `-t SEP` (single-byte field
  separator; default = runs of blanks, awk-style fields), `-k F[.C][opts]
  [,F[.C][opts]]` (field/char key, per-key opts n/r/f/b, multiple -k =
  primary/secondary with a whole-line last resort), `-f` (fold case), `-b`
  (ignore leading blanks in keys). Handles `sort -t: -k3 -n`, `sort -k2`,
  `sort -k2,2`, `sort -k1.2`, `sort -k2n`. Per-key opts OR onto the global
  (GNU's "global applies unless the key overrides"). `-u` now dedupes by the
  active key equality (so `sort -nu` collapses numeric-equal lines, GNU-correct;
  plain `sort -u` is unchanged = exact-line). Deferred (documented): `-g`/`-h`/
  `-V`/`-M`/`-R` key types, `-s`/`-c`/`-o`/`-z`.
- **Batch 2c (grep power, 2026-06-16):** grep `-w` (whole-word match: a match's
  neighbours must be non-word `[A-Za-z0-9_]`), `-o` (print only the matched
  substrings, one per line; prefix + line number honored), `-l` (print only the
  names of files that match; "(standard input)" for stdin), `-r`/`-R` (recurse
  into directory operands, path-prefixing every match). Filename prefix shows
  when >1 source OR -r. PATTERN stays a literal substring (no regex engine;
  `-E`/`-F` moot/deferred). `contains` -> early-returning `has_match`;
  `find_spans` gained the -w word filter.
- **Batch 2d (head/tail bytes, 2026-06-16):** head/tail `-c N` (first/last N
  bytes) alongside `-n N`; both accept separate (`-c N`) and attached (`-cN`)
  forms, plus the legacy bare `-N` (lines). head -c streams (stops after N
  bytes); tail -c slices the slurped tail (saturating). Suffix multipliers
  (1K/1M) and the `+N`/`-N` from-offset forms are deferred.
- **Finding 2 flag-gaps sweep COMPLETE** (cp/mv/rm/mkdir/cat/tr/uniq/sort/grep/
  head/tail). Remaining Finding-2-table items are deliberate deferrals (the `-i`
  interactive prompt needs a stdin y/n read; sort `-g`/`-h`; the regex `-E`).

## Usage / --help enrichment (beginner-friendly; user-directed 2026-06-16)

Every coreutil's `--help` now has a one-line description and worked Examples
(`cmd ...   # what it does`), not just a usage line. The 23 tools that were on
a bare `usage: ... \n --help` one-liner (basename, chmod, clear, cmp, cut,
date, dirname, echo, env, hexdump, id, pwd, realpath, rmdir, seq, sleep, tee,
touch, uname, wc, which, whoami, yes) gained a description + per-flag lines +
examples. The help is HONEST about v1.0 limits: env (no environment surface),
which (no PATH -> only `/`-paths resolve), touch (no mtime), uname (static
fields), id/whoami (numeric, no name service) say so rather than implying GNU
behavior. The block-USAGE tools (the Finding-2 rewrites + ls/stat/ns/realm/qid)
get an Examples section in the same pass.

## meta dedup (DONE 2026-06-16)

ls + stat each carried their own copies of the Kind / classify / perms / owner
/ qid / realm presentation logic; both now route through `coreutils::meta`
(which realm + qid already used). Added `meta::perms_string` (the String form
both want); deleted ls's local `Kind`/`classify`/`perms_string`/`owner_str`/
`qid_str` and stat's `kind_color`/`realm`/`owner`/`perms_string`/`qid_type`
(~176 net lines gone). Behavior preserved (stat additionally gains the
symlink `l` perms char meta already emitted -- dormant pre-symlinks). All four
of ls/stat/realm/qid are now one source of truth for the exotic columns.

## Scripture change (owed to the main agent)

The binding line *"coreutils have disciplined non-colored output"* (wherever it
lives in `docs/` -- UTOPIA-VISUAL / a coreutils manual section) should be
updated to the **color discipline** above. Aux records the design here; the main
agent folds the binding text into `docs/` and refreshes the user manual. Until
then, this doc is the reference for the arc.
