# HOLOTYPE RW-9 -- the Utopia stack (ut + libutopia + coreutils sample)

STANDARD tier. Reviewers: 4x Fable-max `holotype-reviewer` (R1 lexer+parser,
R2 evaluator+jobs+notes+glob+env+builtins, R3 line-editor+REPL+main, R4
coreutils sample; all `claude-fable-5`, MODEL start==end on all four -- no
fallback) + an Opus seam self-audit. Closed list:
`memory/audit_holotype_rw9_closed_list.md`.

**Status: IN PROGRESS (dirty close).** 1 P1 + 11 P2 + ~22 P3 -- by far the most
findings of any RW round. The two highest-value groups are FIXED + verified +
committed; the remaining P2s + P3s + the mandatory dirty-close round-2 are
owed (see "Remaining work").

## The soundness model for this surface

`ut` and the native coreutils are `no_std` Rust over a kernel that validates
everything -> process-local blast radius. But the build ships
`overflow-checks = true` + `panic = "abort"` (`usr/Cargo.toml`), so any
input-driven overflow / index / slice / `unwrap` **aborts the Proc** -- and the
session shell dying is a **logout** (login treats the session-`ut` exit as
logout). The EL0 user stack is a 256 KiB VMA with a one-page guard VMA below
it (`exec.c:161`), so a stack overflow faults cleanly (snare:segv ->
`proc_fault_terminate`) -- Proc death, not corruption. Scripture 8.1 mandates
malformed / pathological input yield a graceful `$status`/`$errstr`, never a
crash. So for a *shell*, a panic / unbounded-recursion / hang where the design
says "graceful error" is a real P2 (availability), and the notes/death/console
seam (the recurrent HOLOTYPE theme) is the highest-stakes interaction.

## Findings + dispositions

| ID | Area | Lens | Sev | Where | Finding | Disposition |
|---|---|---|---|---|---|---|
| HT09.R2-F1 | eval / command-sub | S | **P1** | `eval/stmt.rs::capture_external_pipeline` | A command substitution whose NON-FINAL element fails to spawn (`echo $(foo \| cat)`, foo missing) orphans the capture write end in the parent (the never-spawned last element never took it) -> `read_to_end` blocks forever. The shell is self-managing, so the queued `interrupt` does not terminate it: an UN-INTERRUPTIBLE hang, escapable only by external `kill` | **FIXED** `@07a27c9` -- drop the un-consumed stdin_files/stdout_files Vecs before the drain (no-op on success; closes cap_wr on failure). Regression `/u-subst-test` #10 (`$(no-such \| echo done)` returns empty+127, not a hang) |
| HT09.R1-F1 / SA-1 / R2-F2 | parser + eval | S | **P2** | parser: `parser/parse.rs` + `parser/expr.rs::parse_subscript`; eval: `eval/stmt.rs::eval_block` + `run_command_substitution_script` | Unbounded recursion (CONVERGED 3-way R1+R2+self). The recursive-descent parser AND the evaluator had NO depth cap, so deeply nested input (`((((...))))`, `{{{...}}}`, `if{if{...}}`, `$($($(...)))`) or runaway eval (`fn f { f }`, self-`source`, eval-bomb) overflows the 256 KiB EL0 stack -> guard-page snare:segv -> shell death/logout | **FIXED** `@07a27c9` -- parser: linear token-nesting pre-pass (PARSE_MAX_NESTING=64) + a process-global re-lex counter in parse_subscript (PARSE_MAX_RELEX=32); eval: a shared Env eval-depth Cell at eval_block + run_command_substitution_script (EVAL_MAX_DEPTH=64), leave-balanced. New ParseErrorKind/EvalErrorKind::RecursionLimit. Regressions `/u-subst-test` #11 (eval) + #12 (parser) |
| HT09.R4-F4 | coreutils | S | **P2** | `coreutils/src/bin/{ls,mkdir,pwd}.rs` | LS-4-stale (the recurrent substrate-moved theme, a 6th time): LS-4 landed per-Proc cwd, but `pwd` hardcoded "/", `ls` with no operand listed "/" not the cwd, and `mkdir -p a/b` (relative) silently failed (short-circuited to a single create_dir) | **FIXED** `@c1aea9d` -- all three use `env::current_dir()`; mkdir_p resolves a relative path against the cwd. Deep interactive verification (cd + ls/pwd) owed to the interactive E2E / round-2 |
| HT09.R4-F3 | coreutils / seq | S | **P2** | `coreutils/src/bin/seq.rs` | seq emitted via the error-swallowing `println!`, so `seq 1 N \| head -1` kept generating ~2^63 failing SYS_WRITEs after head closed the pipe (the EPIPE pipe-note never terminates a non-reading native Proc) -> a pegged-CPU runaway | **FIXED** `@c1aea9d` -- `io::stdout().write_all` + break-on-error (the yes.rs idiom) |
| HT09.R2-F3 | eval / loops | S | **P2** | `eval/stmt.rs::eval_while` / `eval_for` | A runaway loop is un-interruptible: the loop bodies never drain the note queue, and the LS-5 eager note-open makes `interrupt` deliver-disposition (queues, does not terminate). `while (1==1) { }` spins, un-Ctrl-C-able; only external `kill` stops it | **OWED** -- FIX: drain a terminate-intent `interrupt` between iterations + pin loop-interrupt semantics in scripture 10.2. Coupled with R3-F1 (the notes seam). Task #50 |
| HT09.R3-F1 | REPL / notes | S | **P2** | `repl.rs:143-167` Accept arm + `tools/interactive/ls-5.exp:36-47` | A stale idle-prompt Ctrl-C kills the user's NEXT foreground command: the Accept arm runs `run_line` BEFORE `deliver_notes`, so the next command's `wait_pids_interruptible` forwards the stale queued `interrupt` to the just-spawned child -> spurious kill. **AND ls-5.exp papers over it with a load-bearing `\r` -- a VERIFY-AROUND of a live defect** (a stewardship issue) | **OWED** -- FIX: drain notes at the TOP of the Accept arm (before run_line); delete the load-bearing `\r` from ls-5.exp so the E2E asserts the real behavior. Needs the interactive E2E to verify. Task #50 |
| HT09.R3-F2 | line editor | S | **P2** | `line_editor.rs:691-755` render | Multi-line render block-crawl: the renderer is stateless about its prev frame, so every keystroke on a continuation line shifts the block down a row + leaves stale duplicate rows (the unlanded U-6 `prev_render_lines`/`\x1b[J` promise). Section 11.4 multi-line editing is visually broken | **OWED** -- FIX: stateful renderer (record prev cursor_line+total; `\x1b[{prev}F` prologue + `\x1b[J` after last line) + a screen-state E2E. Invasive; assess fix-vs-register at round-2. Task #52 |
| HT09.R4-F1 | coreutils / cp | S | **P2** | `coreutils/src/bin/cp.rs` | cp has no same-file or into-itself guard: `cp f f` / `cp f .` truncates the source to 0 bytes (open-then-create-trunc on the same file), silent data loss exit 0; `cp -r src src/sub` is a runaway (create-before-readdir yields the new dir) | **OWED** -- FIX: refuse on resolved-identity-equal src/dst; for -r refuse a dst lexically prefixed by src. Task #51 |
| HT09.R4-F2 | coreutils | S | **P2** | tail/sort/cut/grep/uniq/wc/cmp `.rs` | Seven utils slurp whole inputs into the 4 MiB fixed heap -> silent OOM-abort (exit 1, no message) on real /sbin binaries; cmp's OOM exit-1 collides with its "differ" verdict (a silently-wrong result) | **OWED** -- FIX: stream the easy ones (wc/grep/cut/uniq/cmp); a metadata-size pre-check guard is the cheap interim for all seven. Task #51 |
| HT09.R4-F5 | coreutils | S | **P2** | cut/uniq/grep/wc/sort/hexdump/ls/stat/realpath/tee/tr `.rs` | Nine+ utils write their PAYLOAD through the error-swallowing `io::out`/`print!` (or explicit `let _ =`), so a mid-stream stdout failure (a 9P-backed sink erroring) silently truncates output and exits 0 -- the silent-truncation/data-loss class | **OWED** -- FIX: route payload writes through `write_all` with the head/tail status discipline (error -> eprintln + nonzero exit + stop). Task #51 |
| HT09.R4-F6 | coreutils / tr | S | **P2** | `coreutils/src/bin/tr.rs::expand` | tr treats `[:class:]` as literal bytes -> `tr -d '[:space:]'` silently strips the letters s/p/a/c/e + brackets, preserving whitespace -- silently-wrong data, exit 0 | **OWED** -- FIX (cheap): reject any set containing the `[:` shape (or a bare `[`) with an error, converting silent corruption into a visible failure. Task #51 |

### P3 cluster (owed -- triage at round-2 / fix-cheap-or-register)

- **R1-F2** [P3]: `collect_value_tokens` swallows a subshell's `)` -> `(let x = 5)` mis-parses (break on RParen at depth 0, mirror RBrace). Task #52.
- **R1-F3** [P3]: arith `<<`/`>>` mis-lexes as heredoc start + unimplemented despite section 6.13 listing them (scripture-vs-impl).
- **R1-F4** [P3]: section 7.4 legacy `~` match op + braceless control bodies unparsed (scripture-vs-impl inconsistency).
- **R2-F4** [P3]: `fg`/`wait` use the non-interruptible blocking reap (route `bi_fg` through `wait_pids_interruptible`).
- **R2-F5** [P3]: `poll_notes_once` swallows poll errors -> a persistent note-fd error hot-spins the foreground wait (latent).
- **R2-F6** [P3]: `spec_pids`/`bi_kill` post to already-reaped pids -- sound only by the kernel's monotonic-never-recycle pid allocator; records the future-reuse seam.
- **R2-F7** [P3/C]: builtin completeness -- the section 9.2 namespace/cap builtins (bind/mount/unmount/pivot_root/rfork/cap/note) + section 9.1 read/set/export/alias/history/time/printf/test are ABSENT (the Thylacine-distinctive surface is missing from the shell).
- **R3-F3** [P3]: completion path char-boundary panic (the `StaticCompletionSource` `rfind(is_whitespace)+1` slice; latent -- no production completion source, but the reference idiom ships the bug + `apply_completion` trusts the source's range unvalidated).
- **R3-F4** [P3]: prompt control-byte injection -- a cwd with ESC/C0 bytes is emitted raw into the prompt + miscounted by `visible_width`.
- **R3-F5** [P3]: SS3 (`ESC O`) inserts stray letters; CSI private/intermediate bytes abort mid-sequence + leak the tail as typed text (no panic; buffer garbage from real-terminal sequences).
- **R3-F6** [P3]: history has an entry cap but no byte cap -> a large paste OOMs the shell (4 MiB heap) into logout.
- **R3-F7** [P3]: `[N]+ Done` job notifications bypass the output sink (`t_putstr` = UART, not fd 1) -> lost in fd-driven harnesses / a piped session.
- **R3-F8** [P3]: console-owner setup residuals (silent `open_notes` failure reverts to Ctrl-C=logout; eager-open happens AFTER the first prompt; fd-0-mint in a half-wired spawn).
- **R3-F9** [P3]: Ctrl-G documented as search-cancel but unwired.
- **R4-F7..F17** [P3]: head/tail `-N` overflow->10 + `-nN` form; `-` stdin claimed-not-implemented; rm `.`/`..`/`/` refusal + per-child error continuation; cp depth note + `cp -r ..`; cut `-d` empty/multibyte; tr empty-SET2/reversed-range; sort -n i64 + -nu; ls exit-status gaps; uniq 2nd operand silently ignored; syscall-per-fragment output. (See the R4 report for the full list.)

### Completeness / SOTA register (C/T -- register, not fixed in-arc)

- **chmod/chown coreutils MISSING** while the kernel rwx surface (A-2d/A-3) +
  `t_wstat` are live -- the perm system shipped without its admin tool
  (R4 highest-value register item).
- printf / sed / date / du / df / ln / test / find / xargs absent.
- Per-util flag gaps vs the GNU/BusyBox baseline (head/tail `-c`, `tail -f`,
  sort `-k/-t`, grep `-E/-q/-r`, cut `-s`, uniq `-d/-u/-i`, cp `-p/-i`, ...);
  none panic -- all reject unknown flags gracefully.
- Job model is faithful bash-minus-process-groups; the T-lens steer is the
  Plan 9 note-group model over POSIX pgids as U-PTY lands.
- Parser: no recursion-limit was the only structural gap; a sorted+bsearch
  fixup / arena are SOTA niceties.

## Verified SOUND (do not re-prosecute without new code)

- **The notes/death/job seam core** (the recurrent HOLOTYPE bug nest) is
  STRUCTURALLY SOUND: `wait_pids_interruptible` -- no pid-reuse window (the
  kernel allocator is strictly monotonic + never recycles, proc.c:382; the
  single-threaded shell is the sole reaper; a `!reaped` pid is alive-or-our-
  zombie, never reassigned), `remaining` cannot underflow, an interrupt after
  the final drain defers (not lost), the drain loop is queue-bounded; the
  by-pid reap is pid-precise (no bg-zombie steal); the command-sub
  drain-before-reap SUCCESS path is deadlock-free; the foreground-pipeline
  interrupt-forward gates on `reaped[i]`. (R2 + self, converged.)
- **Pipeline / redirection fd lifecycle**: pipe-alloc failure drops at scope
  exit; every spawn consumes its `Stdio::File` ends; `Command::spawn` drops
  both ends on success AND failure; a mid-pipeline spawn failure `break`s and
  the un-taken ends drop at return (no leak, no double-close) -- the ONE hole
  was the command-sub capture pipe (R2-F1, fixed).
- **Parser panic-freedom**: no reachable explicit panic (every `expect`/
  `unwrap`/`unreachable!` in non-test parser code is structurally guarded;
  lexer.rs has zero outside tests); UTF-8 char-boundary safety throughout
  (every `&src[a..b]` derives from `peek_char_len`/char-advanced positions;
  byte-scanners match ASCII-only markers); no parser-math overflow
  (`checked_*` int parse, monotonic pos); graceful handling of every classic
  hostile input (empty, NUL, lone `\`, `$`/`${`/`"`/`'`/`` ` ``/`$(`
  unterminated, unterminated heredoc, mismatched brackets, 100 KB token).
  The ONLY parser hole was the missing recursion cap (R1-F1, fixed).
- **Line-editor robustness** (R3 verified, held): the CSI parameter
  accumulator is saturating + slot-bounded (a 50-digit param cannot
  overflow/panic); the UTF-8 input state machine is bounded + rejects
  overlong/surrogate; all 13 cursor-mutation sites land on char boundaries;
  `render` arithmetic cannot underflow; history navigation + search indexing
  fully guarded; HT00.F1 (LS-5 eager note-open) intact. The render BEHAVIOR
  bug (R3-F2) is on-screen correctness, not a panic.
- **glob**: `match_bytes` OOB-free; `char_class_match` range access
  `is_some()`/`i+1<len`-guarded (`[a-]`, `[]`, reversed ranges safe); walk
  recursion bounded by segment count.
- **arithmetic (eval)**: Add/Sub/Mul/Div/Mod/Pow/Shl/Shr all `checked_*` ->
  Overflow/DivByZero/InvalidShift errors, never UB.
- **coreutils** (R4 verified): no reachable index/slice/unwrap panic on args
  or input across the suite; NO UTF-8 assumptions on byte streams (the
  stream utils are genuinely binary-safe -- the panics that exist are the
  F2 allocator OOM-aborts); seq arithmetic (checked_add + incr==0 reject) is
  sound (F3 is the sink); the FS-mutator SYSCALL returns are all checked
  (F5 is exclusively the stdout legs); head/tail/cat stream + propagate
  write errors to status correctly (the discipline the F5 utils didn't adopt).

## Remaining work (owed; this is a DIRTY close)

1. **P2 fixes**: R2-F3 + R3-F1 (notes/Ctrl-C, coupled, needs interactive E2E),
   R3-F2 (multi-line render, invasive), R4-F1 (cp data-loss), R4-F2 (slurp
   OOM), R4-F5 (swallowed payload writes), R4-F6 (tr classes -- cheap).
2. **P3 cluster**: fix the cheap/mechanical ones (R1-F2 subshell-`)`,
   R4-F6-adjacent), register the rest.
3. **Dirty-close round-2 re-audit** on ALL the fixes (mandatory: 1 P1 + 11 P2,
   invasive recursion-cap + notes-seam restructures). Focus the prosecutor on
   the recursion-cap leave-balance, the command-sub drop-ordering, and the
   notes-drain-reorder.
4. **Scripture**: pin loop-interrupt semantics in UTOPIA-SHELL-DESIGN section
   10.2 (R2-F3); reconcile the section 6.13/7.3/7.4 scripture-vs-impl deltas
   (R1-F3/F4).
5. Verified-sound interactive E2E for the LS-4 coreutils (cd + ls/pwd) and the
   notes/Ctrl-C fix.

## Reference

ARCH 3.5 (native/ported split); `docs/UTOPIA-SHELL-DESIGN.md` (sections 8/10/11
the soundness-bearing ones); `docs/HOLOTYPE.md` + `docs/holotype/00-register.md`
(HT09.*); closed lists 926_u6f + u7pre (the do-not-re-report SOUND set this
round built on).
