# UT + Nora ergonomics arc — "from works to feels-pro"

**Status: DESIGN (intent), user-approved 2026-06-14. Phase 1 LANDED 2026-06-14; Phase 2 in progress (#115a tab-completion LANDED); Phase 3 pending.** It references + extends the canonical scripture: `docs/UTOPIA-SHELL-DESIGN.md` (the shell), `docs/KAUA.md` + `docs/reference/112-kaua.md` (the console-TUI substrate), `docs/reference/113-nora.md` (the editor), `docs/LIFE-SUPPORT.md` (LS-4 per-Proc cwd, LS-8 termios/consctl).

## Execution status

- **Phase 1 — LANDED (2026-06-14):** #113 (`cd`→`$home` + ut-starts-in-home via login `--home`), #114 (nora open-missing→new buffer, via a `fs::exists` precheck), #116 (the `Env` alias table + `la`/`ll`, folded into `evaluate_argv` for every command position), #118 (the `~`-abbreviated prompt, reusing `path::abbreviate_home`). Two design-doc assumptions were already true in the tree (the prompt was cwd-aware; nora had the `NotFound`→new-buffer arm but the kernel's flat `-1` never triggered it — the precheck is what makes it real). Verified: device userspace build clean, kernel suite 880/880, boot OK, login→ut(`--home`) non-interactive E2E clean, eval-path probes (u-builtin/glob/6/redir-test) all status=0. Pure userspace (kernel byte-identical → no SMP gate / formal audit, the U-6g/Kaua precedent). `tools/interactive/ergo-1.exp` is the standing LS-CI regression (blocked today by the pre-existing #105 HVF-PTY-exit-before-login, proven environmental by the test.sh contrast). ut → `v0.9-dev`.
- **Phase 2 — in progress (#115).** Ground-truth (2026-06-14) revised the scope: the "keystone" the design imagined building (a raw-mode `LineEditor`) was ALREADY built and wired — `usr/utopia/libutopia/src/line_editor.rs` (U-4a/b/c/d) carries the editor engine, the **completion ENGINE** (`CompletionSource`/LCP/`ShowCompletions`), **history nav (Ctrl-P/N), Ctrl-R search**, and multi-line; `repl.rs` drives the prompt in **raw mode** (`console_apply_default`→`PROMPT_MODE`). The audit-bearing I-27 surface (raw-mode editor + consctl dance) was already **discharged at the Kaua T-4 audit (#101)**. So #115's remainder is pure userspace data/logic (design doc §6: unit-tested, not audit-gated), split:
  - **#115a — LANDED (2026-06-14):** namespace-driven Tab completion. A real `completion::ShellCompletionSource` (the engine had only the test-only `StaticCompletionSource`, so Tab was inert) — token-0 completes the command index (builtins + aliases + funcs + the cached `/bin` scan, rebuilt per accepted line via `Repl::install_completion`/`refresh_command_index`); arg tokens path-complete via live `fs::read_dir` (`cd`→dirs only); candidates carry a `/` (dir) or space (file/cmd) terminator. Pure classify/split unit-tested; the live `read_dir` path proven in-QEMU by the `u-repl-test` probe. Verified: device build clean, 880/880, boot OK, `u-repl-test: all OK`, login→ut session clean.
  - **#115b — pending:** `~/.ut_history` persistence (history nav already works in-memory; this adds load-at-start + append).
  - **#115c — pending:** fish-style command-line coloring (reuses the #115a command index via `LineEditor::set_known_commands`).
- **Phase 3 — pending:** #117 (ANSI size handshake), #119 (nora runtime soft-wrap), #120 (nora multi-buffer).

## 0. Why

`ut` + `nora` are functionally proven — the LS-7/Kaua T-4 path carries real keystrokes from Ghostty through cons → Kaua → nora → Stratum to durable bytes on disk, and back through `cat`. This arc is **pure ergonomics**: make the daily-driver shell and editor feel professional. Every item is **userspace-only** (no kernel privilege/ABI surface) — it rides the now-solid foundation (cons/consctl, Kaua, `SYS_CHDIR`, the per-user encrypted home). User-surfaced while testing nora live; all items below were explicitly approved.

## 1. Prior art (research-first, per CLAUDE.md)

- **Plan 9 `rc`** (our heritage): minimalist — no completion, no history, no aliases. We extend tastefully, keeping rc's *namespace-as-truth* ethos: the filesystem IS the completion source.
- **zsh**: the two-tab completion (TAB → complete the longest common prefix; second TAB → a candidate menu) + `LISTMAX` + the navigable menu. The model the user cited.
- **fish**: live syntax coloring of the command line; history-based autosuggestions.
- **helix**: the `[Space]` command-palette menu + in-palette completion navigation. The model the user cited for completion **and** for nora's soft-wrap toggle — which Helix only exposes via *static config*; **nora will do it at runtime** (the user's specific ask).
- **readline**: history + Ctrl-R reverse-search + line-editing primitives (word motion, kill/yank).
- **Thylacine fit**: completion is namespace-driven (Plan-9-idiomatic) — token-0 reads `/bin` (the #58 exec namespace) for commands; argument tokens read the cwd via `SYS_READDIR`. History persists to `~/.ut_history` in the per-user **encrypted** home (A-5b) — private by construction.

## 2. The keystone insight

Most "pro shell" features — tab completion, history recall, Ctrl-R, live coloring, bracketed paste — share **one** foundation: `ut` must own the keystrokes, i.e. a **raw-mode line editor** (a readline) at the prompt, replacing today's cooked-mode whole-line read (the kernel LS-8b cons cooks lines today; `repl.rs` blocks in `read()` for a full line). `ut` already holds the consctl fd (#94-B-b `Env.consctl_fd`) and the raw-mode dance (LS-7). The clean home is a **reusable Kaua `LineEditor` widget** (Kaua already has the cell-buffer, the O(1)-state VT input parser, and the diff-render). Build the `LineEditor` once; the whole cluster lights up on it. **This is why the arc is keyed on Phase 2.**

## 3. The features (all approved 2026-06-14)

### Phase 1 — quick wins (small, low-risk, high-delight; no LineEditor needed)

**Q1. `cd` → `$HOME` + ut starts in home (#113).**
- ut `cd` with no arg → `chdir($HOME)` (`SYS_CHDIR`, LS-4). The `cd` builtin lives in `usr/utopia/libutopia/src/eval/builtin.rs` (dispatch at `stmt.rs:1074`).
- login starts the session in the user's home: `chdir(/home/<user>)` (or pass the spawn cwd) + export `$HOME=/home/<user>` before spawning ut (`usr/login/src/main.rs` already binds `/home/<user>`).
- **VERIFY**: ut's env-var read path for `$HOME` (the `$path`=`/bin` precedent exists; confirm general `$VAR` expansion). If absent, add it, or pass HOME via the login→ut argv/env.

**Q2. Hardwired aliases `la` / `ll` (NEW).**
- `la` = `ls -la`; `ll` = `ls -l` (confirmed 2026-06-14). Both flags are already supported (`ls [-la1]`) — no `ls` extension needed.
- Where: a small **hardwired alias table** in ut, expanded *before* the `fn → builtin → /bin` resolution. Plan 9 rc has no aliases; ut adds a baked set now and (later) user-defined aliases (an `alias` builtin + a `~/.utrc`). Build it as a real alias mechanism (not a magic special-case) so user aliases compose later.

**Q3. nora: open-missing → NEW buffer (#114).**
- Today `nora missing.txt` → `nora: missing.txt: I/O error` and refuses (you must `touch` first). Every editor opens a non-existent path as a new empty buffer and creates it on save.
- Fix: **NotFound** → empty buffer + a `[New]` status flag; create on `:w`. A real **Io** (permission/fault) → keep the error.
- Clean signal = **#102** (open-of-missing → a `NotFound` errno instead of generic `Io`; gated on the **#20** `-T_E_*` re-vote). Interim without #102: nora `stat`/`metadata`-prechecks existence. Land #102 here if #20 clears.

### Phase 2 — the Kaua `LineEditor` + the cluster it unlocks (#115)

**P1. Kaua `LineEditor` widget (the keystone).** A reusable raw-mode line editor in `usr/lib/kaua`: cursor, insert/delete, word-motion, kill/yank, rendered via Kaua's diff-render, fed by the VT input parser. ut's prompt switches to raw (consctl) for input and restores cooked for child stdin (the LS-7 dance, generalized from foreground children to the prompt itself). **I-27-safe**: pure fd0/fd1, never console-attached, never confers consctl — same as Kaua's term backend + nora. The `EventSource` trait is also the Loom-input seam (KAUA §4.4).

**P2. Tab completion** (the headline): the zsh/helix two-tab model.
- **TAB**: candidates for the current token. 1 → insert it fully + trailing ` ` (command) or `/` (dir). >1 → insert the longest common prefix; if the LCP didn't advance → open the menu.
- **Menu**: a popup below the line (Kaua renders it). TAB cycles forward / Shift-TAB back / arrows move; Enter accepts + closes; Esc cancels. Capped when long (`LISTMAX`-ish: show N + "… M more").
- **Sources (context-aware)**: token-0 → builtins + executables in `$path`(=`/bin`); token>0 → files/dirs under the typed prefix (resolve `dir/par` → `readdir dir/` filtered by `par`); `cd` completes dirs only; `$VAR` + `~` expansion are nice-to-have.

**P3. History + Ctrl-R**: up/down recall + Ctrl-R incremental reverse-search; persisted to `~/.ut_history` in the encrypted home (append per command, bounded length).

**P4. fish-style line coloring**: tokenize-as-you-type; a known command (resolves in `$path`/builtins) renders green, an unknown one red; optional arg/flag/string hues. Cheap on the `LineEditor`'s redraw.

**P5. Bracketed paste**: enable `\e[?2004h`; wrap pasted input (`\e[200~` … `\e[201~`) as **literal** so a multi-line paste isn't run line-by-line (and a paste into nora arrives whole). A real "pro terminal" touch.

### Phase 3 — terminal-size + nora polish

**T1. ANSI terminal-size handshake (Kaua) — APPROVED.** At launch, Kaua queries the real size: park the cursor at (999,999), Cursor-Position-Report `\e[6n` → the terminal replies `\e[<rows>;<cols>R` on fd0 (or `\e[18t` → `\e[8;<rows>;<cols>t`). Parse + set the Kaua viewport; **fallback 80×24 on timeout** (dumb terminal / the non-interactive test harness — a few-ms read deadline). Makes nora fill Ghostty. Live *resize* mid-edit is harder (no window-change signal over UART) — a re-query on a keybind or a periodic poll is the v1.x answer; query-at-launch covers the main case.

**T2. nora RUNTIME soft-wrap (the user's key ask).** A `[Space]` command-palette menu (helix-style) with a "toggle soft-wrap" entry that **reflows live** — Helix only does this via static config; nora does it at runtime. Needs nora's view layer to support a wrap mode (vs today's truncate/h-scroll): wrap long lines at the viewport width, adjust the cursor↔screen mapping. Pairs with T1 (wrap needs the real width).

**T3. nora multiple buffers (helix-style).** Open N files; switch via the space menu / a buffer list; per-buffer state. A nora arc on its own (bigger); the editor core already ports Stratum's `editor.rs` ~1:1, so multi-buffer is a buffer list + the switch UI.

## 4. cwd-aware prompt (small, anytime)

`~/cora ⊢` — show the cwd, `~` for `$HOME`, abbreviate deep paths. The `⊢` glyph stays (it's lovely; give it context). ut reads `SYS_GETCWD` (LS-4). Independent of the `LineEditor` (a prompt-string change) — can land in Phase 1 or alongside Phase 2.

## 5. Sequencing (recommended for the executing session(s))

1. **Phase 1** (Q1 cd/home + Q2 aliases + Q3 nora-new-file + §4 cwd prompt) — one short chunk; high delight, low risk; Q3 pairs with finally landing #102/#20.
2. **Phase 2 P1** (the Kaua `LineEditor`) — a design-pinned build; it changes the REPL input model and is **audit-bearing** as a new EL0 console-input path (the Kaua-backend / I-27 discipline applies — an Opus-4.8-max focused round on the `LineEditor` + the raw-mode prompt dance, like the T-4 audit). Then P2 completion → P3 history/Ctrl-R → P4 coloring → P5 bracketed paste, incrementally on it.
3. **Phase 3** T1 size-handshake (Kaua) → T2 nora soft-wrap (deps T1) → T3 nora multi-buffer (nora arc).

## 6. Invariants / discipline

- All **userspace**; **no new ARCH §28 invariant**. The `LineEditor` + the size-handshake are the I-27-adjacent surfaces (new EL0 console-input handling) → the Kaua-backend audit discipline applies (focused Opus-4.8-max round). The rest (completion logic, history, coloring, aliases, prompt, nora view) are pure widget/data layers → unit-tested, not audit-gated.
- History lives in the **encrypted** home (A-5b) — private by construction; never world-readable.
- Pure-userspace, kernel-byte-identical chunks need no SMP gate (the U-6g / Kaua precedent); the audit-bearing `LineEditor` still gets its focused round.

## 7. Tasks

#113 (cd/home) · #114 (nora new-file) · #115 (the `LineEditor` arc: completion + history + Ctrl-R + coloring + bracketed paste) · #116 (aliases) · #117 (ANSI size-handshake) · #118 (cwd prompt) · #119 (nora runtime soft-wrap) · #120 (nora multi-buffer). Q3 pairs with #102/#20 (the errno re-vote).
