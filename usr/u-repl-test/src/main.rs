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

    t_putstr("u-repl-test: all OK\n");
    0
}

fn fail(tag: &str) -> i64 {
    t_putstr("u-repl-test: FAILED -- ");
    t_putstr(tag);
    t_putstr("\n");
    1
}
