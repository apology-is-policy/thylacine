// /coreutil-smoke -- U-6e-pre-b runtime verification driver for the adopted
// native coreutils. joey spawns this; it spawns each coreutil via
// libthyla_rs::process::Command with a piped stdin (fed a known input) +
// piped stdout (captured), asserts the output bytes + exit status, and
// reports per-check markers to the UART (t_putstr). Exit 0 iff every check
// passed; joey reaps the status and gates the boot.
//
// This is the FIRST runtime execution of the coreutils (the auxiliary track
// only compiled them) AND a second exercise of the native-argv path -- each
// tool reads its own argv through env::args(). The kernel collapses any
// non-zero child exit to 1 (sys_exits_handler), so status checks distinguish
// success (0) from failure (non-zero), not literal codes.
//
// REAP-BEFORE-READ (Thylacine deadlock-avoidance): a Proc's fds close at
// REAP (proc_free / wait_pid), NOT at exit (the #844 handle-lifetime design;
// kernel/proc.c). So the child's stdout WRITE-end stays open until we reap
// it -- the std "read child stdout to EOF, then wait" order DEADLOCKS here
// (we would block reading for an EOF that only the reap produces). Instead we
// write stdin, then wait() (the child writes its output into the 4096-byte
// pipe buffer and exits; the reap closes the write-end), THEN drain stdout
// (buffered data survives the write-end close; the read returns it + EOF).
// This is correct for outputs <= PIPE_BUF_SIZE (4096); every test here is
// < 100 bytes. Larger streaming output would need a concurrent reader thread
// (a libthyla_rs::process ergonomic gap tracked for v1.x).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::vec::Vec;

use libthyla_rs::io::{Read, Write};
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::t_putstr;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

// Run `name args...`, feeding `input` to stdin, capturing stdout. Returns
// (exit_code, stdout_bytes), or None on spawn/wait failure.
fn run_tool(name: &str, args: &[&str], input: &[u8]) -> Option<(i32, Vec<u8>)> {
    let mut cmd = Command::new(name);
    for a in args {
        cmd.arg(*a);
    }
    cmd.stdin(Stdio::Piped).stdout(Stdio::Piped).stderr(Stdio::Piped);
    let mut child = cmd.spawn().ok()?;

    // Feed stdin, then drop the write-end so the child's stdin reaches EOF
    // (the data persists in the 4096-byte pipe buffer for the child to read).
    if let Some(mut si) = child.stdin.take() {
        let _ = si.write_all(input);
        drop(si);
    }

    // Drop the stderr read-end up front -- unused, and dropping it early
    // keeps the parent from pinning it.
    drop(child.stderr.take());

    // REAP FIRST (see the module header): the child runs, writes its output
    // into the pipe buffer, and exits; wait() reaps it, which closes the
    // stdout write-end. Only THEN does the read see EOF.
    let st = child.wait().ok()?;

    // Drain the buffered stdout (the write-end is now closed -> the read
    // returns the buffered bytes followed by EOF).
    let mut out = Vec::new();
    if let Some(mut so) = child.stdout.take() {
        so.read_to_end(&mut out).ok()?;
    }
    Some((st.raw(), out))
}

struct Checker {
    fails: usize,
    checks: usize,
}

impl Checker {
    fn pass(&mut self, label: &str) {
        self.checks += 1;
        t_putstr(&format!("coreutil-smoke: {} ok\n", label));
    }

    fn fail(&mut self, label: &str, detail: &str) {
        self.checks += 1;
        self.fails += 1;
        t_putstr(&format!("coreutil-smoke: {} FAILED -- {}\n", label, detail));
    }

    // Assert exact stdout + exit code.
    fn expect(&mut self, label: &str, name: &str, args: &[&str], input: &[u8], want: &[u8], want_code: i32) {
        match run_tool(name, args, input) {
            Some((code, out)) if out.as_slice() == want && code == want_code => self.pass(label),
            Some((code, out)) => self.fail(
                label,
                &format!("got code={} out_len={} (want code={} out_len={})", code, out.len(), want_code, want.len()),
            ),
            None => self.fail(label, "spawn/wait failed"),
        }
    }

    // Assert stdout CONTAINS `needle` + exit code (for content we don't pin
    // byte-for-byte).
    fn expect_contains(&mut self, label: &str, name: &str, args: &[&str], input: &[u8], needle: &[u8], want_code: i32) {
        match run_tool(name, args, input) {
            Some((code, out)) if code == want_code && window_contains(&out, needle) => self.pass(label),
            Some((code, out)) => self.fail(
                label,
                &format!("got code={} out_len={} (want code={}, contains {} bytes)", code, out.len(), want_code, needle.len()),
            ),
            None => self.fail(label, "spawn/wait failed"),
        }
    }
}

fn window_contains(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    needle.len() <= hay.len() && hay.windows(needle.len()).any(|w| w == needle)
}

// --- H-1c-2 Beacon helpers ---

/// Assert `name args...` exits 0 with output free of ESC (0x1b) -- no SGR
/// and no OSC frames. The piped-stdout auto-gate proof.
fn beacon_clean(c: &mut Checker, label: &str, name: &str, args: &[&str]) {
    match run_tool(name, args, b"") {
        Some((0, out)) if !out.contains(&0x1b) => c.pass(label),
        Some((0, _)) => c.fail(label, "ESC bytes in piped output"),
        Some((code, _)) => c.fail(label, &format!("exit {}", code)),
        None => c.fail(label, "spawn/wait failed"),
    }
}

/// Write our own /env/BEACON; children inherit it via env_clone_into.
fn write_env_beacon(value: &[u8]) -> bool {
    match libthyla_rs::fs::File::create("/env/BEACON") {
        Ok(mut f) => f.write_all(value).is_ok(),
        Err(_) => false,
    }
}

/// Run `name` twice -- the rich invocation and the plain one -- and assert
/// the P1 identity on real spawned output: the rich stream carries
/// `frame_needle`, and stripping every frame yields the plain run's bytes
/// EXACTLY. Both runs must agree on exit status too.
fn beacon_rich_vs_plain(
    c: &mut Checker,
    label: &str,
    name: &str,
    rich_args: &[&str],
    plain_args: &[&str],
    input: &[u8],
    frame_needle: &[u8],
) {
    let (rc, rout) = match run_tool(name, rich_args, input) {
        Some(v) => v,
        None => return c.fail(label, "rich spawn/wait failed"),
    };
    let (pc, pout) = match run_tool(name, plain_args, input) {
        Some(v) => v,
        None => return c.fail(label, "plain spawn/wait failed"),
    };
    if rc != pc {
        return c.fail(label, &format!("exit mismatch rich={} plain={}", rc, pc));
    }
    if !window_contains(&rout, frame_needle) {
        return c.fail(label, "expected frame missing from rich output");
    }
    if beacon::wire::strip(&rout) != pout {
        return c.fail(label, "strip(rich) != plain");
    }
    c.pass(label);
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut c = Checker { fails: 0, checks: 0 };

    // --- argv tools (no stdin) ---
    c.expect("echo", "echo", &["a", "b"], b"", b"a b\n", 0);
    c.expect("echo -n", "echo", &["-n", "x"], b"", b"x", 0);
    c.expect("seq N", "seq", &["3"], b"", b"1\n2\n3\n", 0);
    c.expect("seq F I L", "seq", &["2", "2", "6"], b"", b"2\n4\n6\n", 0);
    c.expect("true", "true", &[], b"", b"", 0);
    c.expect("false", "false", &[], b"", b"", 1);
    c.expect("basename", "basename", &["/a/b/c.txt"], b"", b"c.txt\n", 0);
    c.expect("basename suf", "basename", &["/a/b/c.txt", ".txt"], b"", b"c\n", 0);
    c.expect("dirname", "dirname", &["/a/b/c"], b"", b"/a/b\n", 0);
    c.expect("pwd", "pwd", &[], b"", b"/\n", 0);

    // --- stdin filters ---
    c.expect("cat stdin", "cat", &[], b"hello\n", b"hello\n", 0);
    c.expect("wc -c", "wc", &["-c"], b"hello\n", b"      6\n", 0);
    c.expect("wc -l", "wc", &["-l"], b"a\nb\nc\n", b"      3\n", 0);
    c.expect("wc default", "wc", &[], b"a b c\n", b"      1      3      6\n", 0);
    c.expect("head -n", "head", &["-n", "2"], b"1\n2\n3\n4\n", b"1\n2\n", 0);
    c.expect("tail -n", "tail", &["-n", "2"], b"1\n2\n3\n4\n", b"3\n4\n", 0);
    c.expect("sort", "sort", &[], b"banana\napple\ncherry\n", b"apple\nbanana\ncherry\n", 0);
    c.expect("sort -n", "sort", &["-n"], b"10\n2\n1\n", b"1\n2\n10\n", 0);
    c.expect("uniq", "uniq", &[], b"a\na\nb\nb\nb\nc\n", b"a\nb\nc\n", 0);
    c.expect("tr upper", "tr", &["a-z", "A-Z"], b"hello\n", b"HELLO\n", 0);
    c.expect("tr -d", "tr", &["-d", "l"], b"hello\n", b"heo\n", 0);
    // RW-9 R4-F6: a POSIX class is rejected, not silently mangled. Pre-fix,
    // `tr -d '[:space:]'` stripped the literal bytes [ : s p a c e ] and left
    // " b\n" at exit 0 (silent corruption); now it is empty + exit 1.
    c.expect("tr class reject", "tr", &["-d", "[:space:]"], b"a b\n", b"", 1);
    c.expect("cut -f", "cut", &["-d:", "-f2"], b"a:b:c\n", b"b\n", 0);
    c.expect("cut -c", "cut", &["-c1-3"], b"abcdef\n", b"abc\n", 0);
    c.expect("grep match", "grep", &["ba"], b"foo\nbar\nbaz\n", b"bar\nbaz\n", 0);
    c.expect("grep no-match", "grep", &["zzz"], b"foo\nbar\n", b"", 1);
    c.expect("grep -c", "grep", &["-c", "ba"], b"foo\nbar\nbaz\n", b"2\n", 0);

    // --- LS-3c misc coreutils ---
    // The kernel collapses any non-zero child exit to 1, so env's 125 and
    // cmp's 2 both arrive as 1. `yes` is intentionally NOT smoked here: it is
    // an unbounded producer, and the capture harness holds the read-end open
    // (no BrokenPipe), so its write never errors and wait() would deadlock --
    // `yes` is covered interactively (`yes | head`) in the LS-CI ls-3c scenario.
    c.expect("uname", "uname", &[], b"", b"Thylacine\n", 0);
    c.expect("uname -m", "uname", &["-m"], b"", b"aarch64\n", 0);
    c.expect("uname -a", "uname", &["-a"], b"", b"Thylacine (none) 1.0-dev #1-thylacine aarch64\n", 0);
    // The `env` coreutil is NOT bare-name smoked here: G15 reserves `/env` as
    // the per-Proc environment DEVICE (a directory; ARCH 9.7), so a bare-name
    // `env` spawn (cwd "/" -> "/env") now resolves to the device, not the
    // coreutil. The Plan 9 location for the env(1) command is `/bin/env` (the
    // /bin bind -> the cpio `env` binary), reachable post-pivot via the shell's
    // $path -- exercised there, not in this pre-pivot bare-name smoke.
    c.expect("realpath abs", "realpath", &["/a/b/../c"], b"", b"/a/c\n", 0);
    c.expect("realpath rel", "realpath", &["x/../y"], b"", b"/y\n", 0); // cwd "/" -> "/y"
    c.expect("sleep 0", "sleep", &["0"], b"", b"", 0);
    c.expect_contains("hexdump hex", "hexdump", &[], b"Hi", b"48 69", 0); // 'H'=0x48 'i'=0x69
    c.expect_contains("hexdump ascii", "hexdump", &[], b"Hi", b"|Hi|", 0);
    c.expect("which miss", "which", &["nope"], b"", b"", 1); // bare name, no PATH (G15)
    c.expect("which path", "which", &["/version"], b"", b"/version\n", 0);
    c.expect("cmp equal", "cmp", &["/version", "/version"], b"", b"", 0);
    c.expect_contains("cmp differ", "cmp", &["/version", "/welcome"], b"", b"differ", 1);

    // --- file read via File::open (devramfs /version is read-only, present) ---
    c.expect("cat FILE", "cat", &["/version"], b"", b"Thylacine v0.1-dev\n", 0);
    c.expect_contains("wc FILE", "wc", &["-l", "/version"], b"", b"/version", 0);

    // --- LS-K: identity + clock. This smoke runs as PRINCIPAL_SYSTEM
    // (joey-spawned), so uid == gid == 0xFFFFFFFE == 4294967294 -- exact-
    // matchable, proving the wrappers read the right field end-to-end. `date`
    // is time-varying; "UTC" proves it ran + formatted (the wall clock's 2020+
    // plausibility is the always-on kernel test clock.realtime_anchored). ---
    c.expect("whoami SYSTEM", "whoami", &[], b"", b"4294967294\n", 0);
    c.expect("id -u SYSTEM", "id", &["-u"], b"", b"4294967294\n", 0);
    c.expect("id SYSTEM", "id", &[], b"", b"uid=4294967294 gid=4294967294 groups=4294967294\n", 0);
    c.expect_contains("date UTC", "date", &[], b"", b"UTC", 0);

    // --- H-1c-2: the Beacon emitters + the --color=auto flip, end to end.
    // Every child here has a PIPED stdout, so SYS_FD_DEVCLASS answers '|'
    // (devpipe) in the child -- under Auto both gates resolve OFF. These are
    // the real spawns the crate's host tests cannot make.
    //
    // THE PIPE-BUDGET RULE (the module header's REAP-BEFORE-READ cap): every
    // leg's output MUST stay well under PIPE_BUF_SIZE (4096) or the child
    // blocks mid-write and wait() DEADLOCKS THE BOOT. So the ls subjects are
    // explicit FILE operands -- bounded by ARGUMENT COUNT forever (`ls /`
    // grows with every staged bin and `ls -l /` is ALREADY over budget; a
    // directory subject cannot work anyway: the boot ramfs is FLAT, so the
    // only listable dirs pre-pivot are root and the empty synth mounts, and
    // an empty listing would satisfy the clean check vacuously, the #215
    // broken-fixture shape). The ps rich leg first measures the raw snapshot
    // and SKIPS LOUDLY if the frame-multiplied estimate approaches the cap
    // (the proc table grows with boot services; a skip is visible in the
    // log, a hang would not be). ---

    // The flip's proof: `ls` (color now defaults to Auto) into a pipe is
    // BYTE-CLEAN -- no SGR, no frames. Fails on the pre-flip Always default
    // (file operands were SGR-wrapped too).
    beacon_clean(&mut c, "ls auto pipe clean", "ls", &["/version", "/welcome"]);
    beacon_clean(&mut c, "ls -l auto pipe clean", "ls", &["-l", "/version", "/welcome"]);

    // The ps legs' budget gate (audit H-1 F9): EVERY ps spawn pipes the full
    // /ctl/procs snapshot, so the pipe-budget hazard is in the spawn itself
    // -- a guard cannot measure by spawning. Measure by reading the SAME
    // source directly (no pipe), then gate all four legs on it. The proc
    // table grows with boot services; a skip is loud, a hang would not be.
    let ps_raw_len = {
        use libthyla_rs::io::Read as _;
        let mut sz = 0usize;
        if let Ok(mut f) = libthyla_rs::fs::File::open("/ctl/procs") {
            let mut b = [0u8; 512];
            while let Ok(n) = f.read(&mut b) {
                if n == 0 {
                    break;
                }
                sz += n;
            }
        }
        sz
    };
    if ps_raw_len > 0 && ps_raw_len < 3500 {
        beacon_clean(&mut c, "ps auto pipe clean", "ps", &[]);
        // `ps` piped = the verbatim /ctl/procs snapshot (parseable): the
        // kernel header + this very process family are in it.
        c.expect_contains("ps raw header", "ps", &[], b"", b"PID    PPID    NAME", 0);
        c.expect_contains("ps raw joey", "ps", &[], b"", b"joey", 0);
    } else {
        t_putstr(&format!(
            "coreutil-smoke: ps raw legs SKIPPED (/ctl/procs {}B nears PIPE_BUF)\n",
            ps_raw_len
        ));
    }

    // Force the rich tier down the REAL inheritance path: write our own
    // /env/BEACON (env_clone_into deep-copies it into every child we spawn),
    // then drive each emitter with --beacon=always (the flag trusts the
    // advertised tier; the pipe dc no longer gates). Asserts: the frames are
    // ON the wire, and stripping them yields the plain emission byte-exactly
    // (the P1 identity, in-guest).
    if write_env_beacon(b"rich") {
        c.pass("env BEACON=rich exported");
        beacon_rich_vs_plain(
            &mut c,
            "ls rich strips to plain",
            "ls",
            &["--beacon=always", "/version", "/welcome"],
            &["--beacon=never", "--color=never", "/version", "/welcome"],
            b"",
            b"\x1b]1936;v1;obj;type=path;ref=/version",
        );
        beacon_rich_vs_plain(
            &mut c,
            "grep rich strips to plain",
            "grep",
            &["--beacon=always", "ba"],
            &["--beacon=never", "ba"],
            b"foo\nbar\nbaz\n",
            b"\x1b]1936;v1;em;class=strong",
        );
        beacon_rich_vs_plain(
            &mut c,
            "stat rich strips to plain",
            "stat",
            &["--beacon=always", "/version"],
            &["--beacon=never", "--color=never", "/version"],
            b"",
            b"\x1b]1936;v1;obj;type=path;ref=/version",
        );
        // ps at rich: the beacon table with obj pid cells. Its plain payload
        // is the STYLED aligned table (not the raw snapshot), so assert the
        // frame + content, not equality with the pass-through. Budget-gated
        // on the direct /ctl/procs measurement above (frames multiply a raw
        // row ~6x): skip loudly rather than risk the pipe-full deadlock.
        if ps_raw_len > 0 && ps_raw_len * 7 < 3500 {
            match run_tool("ps", &["--beacon=always"], b"") {
                Some((0, out)) => {
                    if window_contains(&out, b"\x1b]1936;v1;obj;type=pid;ref=")
                        && window_contains(&out, b"\x1b]1936;v1;table;cols=rrllrrrr")
                        && window_contains(&beacon::wire::strip(&out), b"joey")
                    {
                        c.pass("ps rich table + obj pid");
                    } else {
                        c.fail("ps rich table + obj pid", "frames/payload missing");
                    }
                }
                Some((code, _)) => c.fail("ps rich table + obj pid", &format!("exit {}", code)),
                None => c.fail("ps rich table + obj pid", "spawn/wait failed"),
            }
        } else {
            // NOT a pass: the leg was not exercised. Loud, greppable, and
            // it should page whoever grew the boot's proc table this far.
            t_putstr(&format!(
                "coreutil-smoke: ps rich leg SKIPPED (/ctl/procs {}B x7 nears PIPE_BUF)\n",
                ps_raw_len
            ));
        }
        // Reset our env so the tail checks (and anything after) see none.
        let _ = write_env_beacon(b"none");
    } else {
        c.fail("env BEACON=rich exported", "/env/BEACON write failed");
    }

    if c.fails == 0 {
        t_putstr(&format!("coreutil-smoke: all OK ({} checks)\n", c.checks));
        0
    } else {
        t_putstr(&format!("coreutil-smoke: {} of {} checks FAILED\n", c.fails, c.checks));
        1
    }
}
