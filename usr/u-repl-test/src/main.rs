// /u-repl-test -- U-6g REPL-loop boot probe.
//
// Drives the read-parse-eval main loop (libutopia::repl::Repl) through its
// public `feed` surface with scripted in-memory byte streams + a Vec sink.
// The interactive keystroke path (fd 0 = /dev/cons) cannot be driven non-
// interactively in the harness (the A-4c constraint: QEMU offers no UART-RX
// injection without disturbing the boot-banner ABI), but `feed` is fd-
// agnostic -- a pipe/cons delivers the SAME bytes the editor consumes, so
// this exercises the full loop deterministically. Covers:
//
//   1. Accept -> parse -> eval -> assignment state
//   2. A line split across two reads accumulates before the newline submits
//   3. `exit N` terminates the session with code N
//   4. Ctrl-D (0x04) on an empty buffer ends the session
//   5. Ctrl-C (0x03) discards the partial edit; the editor recovers
//   6. A printable keystroke renders the prompt + buffer to the sink (Redraw)
//   7. A parse error does NOT end the interactive session (scripture 8.9)
//
// joey gates the boot on this binary's status==0.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::t_putstr;
use libutopia::repl::Repl;

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // 1. A complete line submits on '\n', parses, evaluates, mutates Env.
    {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        if repl.feed(b"let greeting = hello\n", &mut sink).is_some() {
            return fail("a plain line unexpectedly ended the session");
        }
        if repl.env().get("greeting").as_scalar() != "hello" {
            return fail("`let greeting = hello` did not assign");
        }
    }

    // 2. A line split across two reads (the realistic chunked / byte-at-a-time
    //    arrival) accumulates in the editor; only the trailing '\n' submits.
    {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        if repl.feed(b"let part", &mut sink).is_some() {
            return fail("a partial line unexpectedly ended the session");
        }
        if repl.feed(b" = abc\n", &mut sink).is_some() {
            return fail("the completing read unexpectedly ended the session");
        }
        if repl.env().get("part").as_scalar() != "abc" {
            return fail("split-across-reads line did not assemble");
        }
    }

    // 3. `exit N` ends the session and the loop returns N.
    {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        match repl.feed(b"exit 5\n", &mut sink) {
            Some(5) => {}
            Some(_) => return fail("exit returned the wrong code"),
            None => return fail("exit did not end the session"),
        }
    }

    // 4. Ctrl-D (0x04) on an empty buffer ends the session (scripture 10.4).
    {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        match repl.feed(b"\x04", &mut sink) {
            Some(0) => {}
            Some(_) => return fail("Ctrl-D ended with a non-zero status"),
            None => return fail("Ctrl-D on an empty buffer did not end the session"),
        }
    }

    // 5. Ctrl-C (0x03) discards the in-progress edit; a fresh line then
    //    evaluates -- proving the editor recovered, not the session ended.
    {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        if repl.feed(b"junk text", &mut sink).is_some() {
            return fail("typing unexpectedly ended the session");
        }
        if repl.feed(b"\x03", &mut sink).is_some() {
            return fail("Ctrl-C unexpectedly ended the session");
        }
        if repl.feed(b"let after = ok\n", &mut sink).is_some() {
            return fail("the post-cancel line unexpectedly ended the session");
        }
        if repl.env().get("after").as_scalar() != "ok" {
            return fail("editor did not recover after Ctrl-C");
        }
    }

    // 6. A printable keystroke produces a Redraw; the loop renders to the sink.
    {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        let _ = repl.feed(b"a", &mut sink);
        if sink.is_empty() {
            return fail("a keystroke produced no rendering");
        }
    }

    // 7. A malformed line surfaces a diagnostic but does NOT end the
    //    interactive session (scripture 8.9: non-zero $status / errors at the
    //    prompt draw a fresh prompt rather than terminating). A later good
    //    line still evaluates.
    {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        if repl.feed(b")\n", &mut sink).is_some() {
            return fail("a parse error unexpectedly ended the session");
        }
        if repl.feed(b"let recovered = yes\n", &mut sink).is_some() {
            return fail("the post-error line unexpectedly ended the session");
        }
        if repl.env().get("recovered").as_scalar() != "yes" {
            return fail("the session did not recover after a parse error");
        }
    }

    // 8. #115a: the namespace-driven Tab completion source. Command-position
    //    completion is pure (filters the index); argument-position completion
    //    reads the LIVE filesystem -- the in-QEMU proof of the read_dir path
    //    that the host unit tests (libutopia cannot host-test) cannot exercise.
    {
        use alloc::string::String;
        use libutopia::completion::ShellCompletionSource;
        use libutopia::line_editor::CompletionSource;
        let cmds: Vec<String> = ["cat", "la", "ls"].iter().map(|s| String::from(*s)).collect();
        let src = ShellCompletionSource::new(cmds);
        // Command position: "l" -> the index entries starting "l", each
        // terminated with a trailing space, in sorted order.
        let c = src.complete("l", 1);
        if c.candidates != [String::from("la "), String::from("ls ")] {
            return fail("command-position completion returned the wrong candidates");
        }
        // Argument position over the live root: `ls /<TAB>` must read_dir "/"
        // and return its entries (bin / srv / proc / ... -- never empty), with
        // the directory entries terminated by '/'.
        let c = src.complete("ls /", 4);
        if c.candidates.is_empty() {
            return fail("path completion of the root returned no entries");
        }
        if !c.candidates.iter().any(|e| e.ends_with('/')) {
            return fail("path completion did not mark any root entry as a directory");
        }
    }

    // 9. #115c: command-line validity coloring. Pure logic (no syscall), but
    //    exercised in-guest as belt-and-suspenders over the host #[cfg(test)]
    //    contract. A known command renders the Bonfire `fen` SGR
    //    (#6a9a6a = 106,154,106); an unknown one `cinnabar` (#c06050 =
    //    192,96,80); an empty index renders the buffer verbatim.
    {
        use alloc::string::String;
        use libutopia::line_editor::LineEditor;
        let mut le = LineEditor::new();
        le.set_known_commands(["cat", "ls"].iter().map(|s| String::from(*s)).collect());
        let _ = le.feed_bytes(b"ls -la");
        let s = le.render("> ");
        if !s.contains("38;2;106;154;106") {
            return fail("a known command did not render the fen colour");
        }
        // A fresh editor with no index colours nothing.
        let mut plain = LineEditor::new();
        let _ = plain.feed_bytes(b"ls");
        let s = plain.render("> ");
        if s.contains("38;2;106;154;106") || s.contains("38;2;192;96;80") {
            return fail("coloring should be disabled with an empty command index");
        }
    }

    // 10. D4: zsh-style menu completion -- cycle + finalize + dismiss, driven
    //     on the live LineEditor in-guest (the host #[cfg(test)] tab_menu_*
    //     contract). The terminal strip rendering (render_menu_strip) is host-
    //     tested; here we prove the editor STATE MACHINE in QEMU.
    {
        use alloc::boxed::Box;
        use alloc::string::String;
        use alloc::vec;
        use libutopia::line_editor::{EditorAction, LineEditor, StaticCompletionSource};

        let mut le = LineEditor::new();
        le.set_completion_source(Box::new(StaticCompletionSource::new(vec![
            String::from("apple"),
            String::from("application"),
            String::from("apparatus"),
        ])));
        let _ = le.feed_bytes(b"app");
        // Tab: the shared prefix "app" is already typed -> enter the menu and
        // apply candidate[0].
        match le.feed_byte(0x09) {
            EditorAction::MenuShow { selected: 0, .. } => {}
            _ => return fail("D4: first Tab did not enter the menu at candidate 0"),
        }
        if le.buffer() != "apple" {
            return fail("D4: menu did not apply candidate[0]");
        }
        // Tab again: cycle to candidate[1].
        match le.feed_byte(0x09) {
            EditorAction::MenuShow { selected: 1, .. } => {}
            _ => return fail("D4: second Tab did not cycle to candidate 1"),
        }
        if le.buffer() != "application" {
            return fail("D4: cycle did not apply candidate[1]");
        }
        // Enter: finalize -- keep the selection, do NOT submit (Redraw).
        if le.feed_byte(b'\r') != EditorAction::Redraw {
            return fail("D4: Enter in the menu did not finalize as Redraw");
        }
        if le.buffer() != "application" {
            return fail("D4: finalize lost the applied selection");
        }
        // A subsequent Enter submits (Normal mode).
        match le.feed_byte(b'\r') {
            EditorAction::Accept(line) if line == "application" => {}
            _ => return fail("D4: Enter after finalize did not submit the line"),
        }

        // Dismiss-and-append: Tab into the menu, then a char dismisses + appends.
        let mut le2 = LineEditor::new();
        le2.set_completion_source(Box::new(StaticCompletionSource::new(vec![
            String::from("apple"),
            String::from("apricot"),
        ])));
        let _ = le2.feed_bytes(b"ap");
        let _ = le2.feed_byte(0x09); // -> apple
        if le2.buffer() != "apple" {
            return fail("D4: le2 menu did not apply apple");
        }
        let _ = le2.feed_byte(b'X');
        if le2.buffer() != "appleX" {
            return fail("D4: typing did not dismiss the menu + append");
        }
        t_putstr("u-repl-test: D4 menu completion OK\n");
    }

    // 8. AND-OR list (scripture 8.6): short-circuit + final-status. The status
    //    is the discriminator against "ran the RHS anyway": if `&&` did NOT
    //    short-circuit, `false && true` would run `true` and leave 0; if `||`
    //    did not, `true || false` would run `false` and leave 1.
    {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        repl.feed(b"false && true\n", &mut sink);
        if repl.env().status() == 0 {
            return fail("`false && true` ran the RHS -- && did not short-circuit");
        }
    }
    {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        repl.feed(b"true || false\n", &mut sink);
        if repl.env().status() != 0 {
            return fail("`true || false` ran the RHS -- || did not short-circuit");
        }
    }
    {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        // `||` runs the RHS when the LHS failed; the final status is the RHS's 0.
        repl.feed(b"false || true\n", &mut sink);
        if repl.env().status() != 0 {
            return fail("`false || true` did not run the RHS to success");
        }
    }
    {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        // Left-associative: the chain reaches the final `true`.
        repl.feed(b"false || false || true\n", &mut sink);
        if repl.env().status() != 0 {
            return fail("`false || false || true` did not reach the final true");
        }
    }
    t_putstr("u-repl-test: AND-OR (&& / ||) short-circuit OK\n");

    // 9. `=` in a command ARGUMENT is a literal (was UnexpectedEqualInCommand):
    //    `-std=c++20` / `--foo=bar` glue into single argv words. Test the parser
    //    directly -- a parse error here is the exact pre-fix failure -- and
    //    confirm a statement-start assignment (a different path) still parses.
    {
        use libutopia::parser::parse;
        if parse("clang -std=c++20 -O2 main.cpp -o main.o").is_err() {
            return fail("`clang -std=c++20 ...` still fails to parse a =-bearing arg");
        }
        if parse("let x = 5").is_err() {
            return fail("`let x = 5` assignment regressed");
        }
    }
    t_putstr("u-repl-test: = in argument position parses OK\n");

    // 10. winsize / line-wrap: the CPR width handshake + the visual-wrapped-row
    //     render. The bug: a command that wraps past the terminal edge, when
    //     the cursor is moved, re-clears only ONE physical row and re-emits ->
    //     the line duplicates on every keystroke. The fix needs (a) the editor
    //     to learn the width and (b) render to move up to the block TOP before
    //     clearing. Both are pure logic -- driven directly on the LineEditor.
    {
        use libutopia::line_editor::LineEditor;

        // (a) A CPR reply ESC[<rows>;<cols>R sets the width (cols = 2nd param).
        let mut le = LineEditor::new();
        if le.cols().is_some() {
            return fail("winsize: a fresh editor already claims a width");
        }
        let _ = le.feed_bytes(b"\x1b[24;80R");
        if le.cols() != Some(80) {
            return fail("winsize: a CPR reply did not set cols to 80");
        }
        // A CPR reply must NOT surface as a keystroke (buffer stays empty).
        if !le.buffer().is_empty() {
            return fail("winsize: the CPR reply leaked into the buffer as keys");
        }

        // (b) A reply dribbled across reads (the HVF serial split) reassembles
        //     -- the byte-at-a-time CSI parser handles chunking for free.
        let mut split = LineEditor::new();
        let _ = split.feed_bytes(b"\x1b[40");
        let _ = split.feed_bytes(b";132R");
        if split.cols() != Some(132) {
            return fail("winsize: a split CPR reply did not reassemble to 132");
        }

        // A non-CPR CSI final (one param) and a zero-size report leave cols
        // unset -- never a spurious width.
        let mut nope = LineEditor::new();
        let _ = nope.feed_bytes(b"\x1b[80R"); // one param -> not a CPR
        let _ = nope.feed_bytes(b"\x1b[0;0R"); // zero size -> rejected
        if nope.cols().is_some() {
            return fail("winsize: a non-CPR / zero-size report set a width");
        }

        // Width UNKNOWN: render is byte-preserved (the pre-fix newline-only
        // path). It starts with the single-line clear "\r\x1b[K".
        let mut unknown = LineEditor::new();
        let _ = unknown.feed_bytes(b"hello");
        let s = unknown.render("> ");
        if !s.starts_with("\r\x1b[K") {
            return fail("winsize: cols=None render regressed the byte-preserved fallback");
        }

        // Width known, short line (no wrap): the wrapped path erases to end of
        // screen ("\r\x1b[J") and shows the buffer -- a single physical row.
        let mut wide = LineEditor::new();
        wide.set_cols(80);
        let _ = wide.feed_bytes(b"hi");
        let s = wide.render("> ");
        if !s.starts_with("\r\x1b[J") || !s.contains("> hi") {
            return fail("winsize: cols=Some single-row render has the wrong shape");
        }

        // THE DISCRIMINATOR. cols=20, prompt "> " (width 2) + 30 chars = 32
        // cells -> 2 physical rows; the cursor ends on row 1. A first render
        // records that row; after a cursor move, the SECOND render must move
        // UP to the block top ("\x1b[1A") before clearing. The buggy
        // newline-only render treats the wrapped line as ONE line and emits NO
        // up-move -- so this assertion fails without the fix.
        let mut wrap = LineEditor::new();
        wrap.set_cols(20);
        let _ = wrap.feed_bytes(&[b'a'; 30]);
        let _ = wrap.render("> "); // establishes prev_cursor_row = 1
        let _ = wrap.feed_byte(0x02); // Ctrl-B: cursor left, still on row 1
        let s = wrap.render("> ");
        if !s.starts_with("\x1b[1A\r\x1b[J") {
            return fail("winsize: wrapped re-render did not move up to the block top (the dup bug)");
        }

        // (c) DISPLAY-MODES.md 3.4: parse_winsize -- the ONE parser that reads
        //     both the console `/dev/winsize` line and the pts ldisc ctl line
        //     (they share the "winsize C R" token by design). This is what the
        //     shell's width source feeds set_cols; a wrong parse would silently
        //     mis-wrap, so prove it total: both formats, and rejection of every
        //     malformed shape rather than a guessed width.
        use libutopia::repl::parse_winsize;
        if parse_winsize(b"winsize 80 24\n") != Some((80, 24)) {
            return fail("winsize: parse_winsize missed the console line");
        }
        if parse_winsize(b"+icanon +echo +isig +icrnl +onlcr winsize 132 43\n") != Some((132, 43)) {
            return fail("winsize: parse_winsize missed the pts ctl line");
        }
        if parse_winsize(b"winsize 0 0\n") != Some((0, 0)) {
            return fail("winsize: parse_winsize mishandled the serial 0 0 posture");
        }
        // Malformed -> None (never a guessed width): no token, non-digit,
        // empty field, u32 overflow.
        if parse_winsize(b"+icanon +echo\n").is_some()
            || parse_winsize(b"winsize 80 x\n").is_some()
            || parse_winsize(b"winsize  24\n").is_some()
            || parse_winsize(b"winsize 99999999999 24\n").is_some()
        {
            return fail("winsize: parse_winsize guessed a width on malformed input");
        }
    }
    t_putstr("u-repl-test: winsize / line-wrap OK\n");

    t_putstr("u-repl-test: all OK\n");
    0
}

fn fail(tag: &str) -> i64 {
    t_putstr("u-repl-test: FAILED -- ");
    t_putstr(tag);
    t_putstr("\n");
    1
}
