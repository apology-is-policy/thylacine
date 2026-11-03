//! The paste key in a real ut (ASYNC-SERVICE-LIFECYCLE, AS-4e), against loom-fixture,
//! driven by tools/interactive/ls-ut-paste.exp. The shell runs on a pts of its own, with
//! the fixture's locator in its environment, and takes its keys from this.
//!
//! A pasted line is echoed in lower case and `tr` prints it in upper case, so the two
//! words tell a line in the buffer from a line that ran. The answered legs are the
//! control for the stopped ones: a key that never fetches cannot pass for one held.
use crate::stopped_nora::{
    after, pause, Fixture, Result, DRAIN_STACK, PROMPTLY_MS, ROOM_MS, TICK_MS,
};
use ::alloc::{format, string::String, vec, vec::Vec};
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU8, Ordering};
use interaction_native::{parse_locator, Host};
use libhalcyon::interaction_client::Event;
use libthyla_rs::{
    fs::File, io::Write, loom::ThylaKernel, process::Child, thread, time::monotonic_ns, *,
};

const UP: u32 = 0;
const ONE_IN_BUFFER: u32 = 1;
const ONE_RAN: u32 = 2;
const TWO_RAN: u32 = 3;
const THREE_RAN: u32 = 4;
const FOUR_RAN: u32 = 5;
const FIVE_RAN: u32 = 6;
const SIX_RAN: u32 = 7;
const TOO_LONG: u32 = 8;
const UNPRINTABLE: u32 = 9;
const STILL_FETCHING: u32 = 10;
const NO_ANSWER: u32 = 11;
const THREE_IN_BUFFER: u32 = 12;
const SEVEN_RAN: u32 = 13;
const EIGHT_RAN: u32 = 14;
const NINE_RAN: u32 = 15;
const WORDS: [&[u8]; 16] = [
    b"UT-PASTE-UP",
    b"ut-paste-one",
    b"UT-PASTE-ONE",
    b"UT-PASTE-TWO",
    b"UT-PASTE-THREE",
    b"UT-PASTE-FOUR",
    b"UT-PASTE-FIVE",
    b"UT-PASTE-SIX",
    b"too long for this line",
    b"a tab or control character",
    b"still fetching the last paste",
    b"no answer in time",
    b"ut-paste-three",
    b"UT-PASTE-SEVEN",
    b"UT-PASTE-EIGHT",
    b"UT-PASTE-NINE",
];
const LONGEST_WORD: usize = 29;
const PASTE_KEY: &[u8] = b"\x1bp";
/// Long enough for a line that submitted itself to have run and printed.
const SETTLE_MS: u64 = 1000;
/// The line's limit is 64 KiB: this is past it, and well inside the clipboard's.
const TOO_LONG_BYTES: usize = 65 * 1024;

static SEEN: AtomicU32 = AtomicU32::new(0);
static DRAINED: AtomicBool = AtomicBool::new(false);
/// The terminal's last bytes, for a failure to show what the shell was doing.
const TAIL_BYTES: usize = 240;
static TAIL: [AtomicU8; TAIL_BYTES] = [const { AtomicU8::new(0) }; TAIL_BYTES];
static TAIL_NEXT: AtomicU32 = AtomicU32::new(0);

fn remember(bytes: &[u8]) {
    for &b in bytes {
        let at = TAIL_NEXT.fetch_add(1, Ordering::Relaxed) as usize % TAIL_BYTES;
        TAIL[at].store(b, Ordering::Relaxed);
    }
}

fn tail() -> String {
    let next = TAIL_NEXT.load(Ordering::Relaxed) as usize;
    let first = next.saturating_sub(TAIL_BYTES);
    (first..next)
        .map(|i| match TAIL[i % TAIL_BYTES].load(Ordering::Relaxed) {
            b @ 0x20..=0x7e => b as char,
            _ => '.',
        })
        .collect()
}

/// A pts master does not poll, so the shell's output has a thread of its own.
extern "C" fn drain(arg: u64) {
    let master = arg as i64;
    let mut buffer = [0u8; 2048];
    let mut kept = 0;
    loop {
        let n = unsafe { t_read(master, buffer[kept..].as_mut_ptr(), buffer.len() - kept) };
        if n <= 0 {
            break;
        }
        let end = kept + n as usize;
        remember(&buffer[kept..end]);
        for (i, word) in WORDS.iter().enumerate() {
            if buffer[..end].windows(word.len()).any(|w| w == *word) {
                SEEN.fetch_or(1 << i, Ordering::SeqCst);
            }
        }
        kept = end.min(LONGEST_WORD - 1);
        buffer.copy_within(end - kept..end, 0);
    }
    DRAINED.store(true, Ordering::SeqCst);
    unsafe { t_thread_exit() }
}

struct Ut {
    child: Child,
    master: i64,
}

impl Ut {
    fn start(service: &str) -> Result<Self> {
        // The spawn copies this environment, and ut reads it at each paste.
        let locator = format!("/srv/{}/dir/interaction", service);
        File::create("/env/HALCYON_INTERACTION")
            .and_then(|mut f| f.write_all(locator.as_bytes()))
            .map_err(|_| "the locator")?;
        let master = ptyhold::Master::mint().map_err(|_| "a pts")?;
        master.seed_winsize(80, 24);
        SEEN.store(0, Ordering::SeqCst);
        DRAINED.store(false, Ordering::SeqCst);
        let child = master
            .spawn_on_slave(&[String::from("/bin/ut")])
            .map_err(|_| "spawn the shell")?;
        let stack = unsafe { t_burrow_attach(DRAIN_STACK) };
        if stack < 0 {
            return Err("a stack to drain on");
        }
        let top = stack as u64 + DRAIN_STACK;
        unsafe { thread::spawn_raw(drain as *const () as u64, top, master.mfd as u64, 0) }
            .map_err(|_| "a thread to drain on")?;
        let ut = Self {
            child,
            master: master.mfd,
        };
        ut.keys(b"echo ut-paste-up | tr a-z A-Z\r")?;
        ut.sees(UP, "the shell never ran a typed line")?;
        Ok(ut)
    }

    fn saw(word: u32) -> bool {
        SEEN.load(Ordering::SeqCst) & (1 << word) != 0
    }

    fn sees(&self, word: u32, why: &'static str) -> Result {
        let limit = after(PROMPTLY_MS);
        while !Self::saw(word) {
            if monotonic_ns() >= limit {
                return Err(why);
            }
            pause(TICK_MS)?;
        }
        Ok(())
    }

    fn keys(&self, mut bytes: &[u8]) -> Result {
        while !bytes.is_empty() {
            let n = unsafe { t_write(self.master, bytes.as_ptr(), bytes.len()) };
            if n < 0 {
                return Err("a key did not reach the terminal");
            }
            if n == 0 {
                pause(TICK_MS)?;
            }
            bytes = &bytes[n as usize..];
        }
        Ok(())
    }

    /// `exit`, and it is gone without waiting for anything a server owes.
    fn leaves(mut self) -> Result {
        self.keys(b"exit\r")?;
        let limit = after(PROMPTLY_MS);
        loop {
            match self.child.try_wait() {
                Ok(Some(_)) => break,
                Ok(None) if monotonic_ns() >= limit => return Err("the shell did not leave"),
                Ok(None) => pause(TICK_MS)?,
                Err(_) => return Err("wait for the shell"),
            }
        }
        while !DRAINED.load(Ordering::SeqCst) {
            if monotonic_ns() >= limit {
                return Err("the terminal did not end with the shell");
            }
            pause(TICK_MS)?;
        }
        let _ = unsafe { t_close(self.master) };
        Ok(())
    }
}

fn host() -> Result<Host<ThylaKernel>> {
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err("the registry");
    }
    let locator = parse_locator(b"/srv/fixture/dir/interaction").ok_or("locator shape")?;
    let ring = Host::new(ThylaKernel, srv as i32, locator);
    let _ = unsafe { t_close(srv) };
    ring.map_err(|_| "the private ring")
}

/// Puts text on the fixture's clipboard over a connection of this probe's own.
fn seed(fixture: &mut Fixture, text: Vec<u8>) -> Result {
    let mut client = host()?;
    let now = client.now();
    client
        .copy(text, now)
        .map_err(|_| "the seed copy was refused")?;
    let limit = after(ROOM_MS);
    loop {
        let now = client.now();
        client.pump(now);
        while let Some(event) = client.take_event() {
            match event {
                Event::Copied { .. } => {
                    drop(client);
                    return fixture.no_sessions();
                }
                Event::CopyUnknown | Event::CopyFailed(_) => return Err("the seed copy failed"),
                _ => {}
            }
        }
        if monotonic_ns() >= limit {
            return Err("the seed copy did not finish");
        }
        pause(TICK_MS)?;
    }
}

fn answered(fixture: &mut Fixture) -> Result {
    seed(fixture, b"echo ut-paste-one | tr a-z A-Z".to_vec())?;
    let ut = Ut::start("fixture")?;

    // In the buffer, not run, and Enter is what runs it.
    ut.keys(PASTE_KEY)?;
    ut.sees(ONE_IN_BUFFER, "the paste never reached the line")?;
    pause(SETTLE_MS)?;
    if Ut::saw(ONE_RAN) {
        return Err("a paste submitted itself");
    }
    ut.keys(b"\r")?;
    ut.sees(ONE_RAN, "the pasted line did not run")?;

    // Two lines stay one edit, ending newline included.
    seed(
        fixture,
        b"echo ut-paste-two | tr a-z A-Z\necho ut-paste-three | tr a-z A-Z\n".to_vec(),
    )?;
    ut.keys(PASTE_KEY)?;
    ut.sees(THREE_IN_BUFFER, "the two lines never reached the buffer")?;
    pause(SETTLE_MS)?;
    if Ut::saw(TWO_RAN) || Ut::saw(THREE_RAN) {
        return Err("a pasted line feed submitted");
    }
    ut.keys(b"\r")?;
    ut.sees(TWO_RAN, "the first pasted line did not run")?;
    ut.sees(THREE_RAN, "the second pasted line did not run")?;

    // Refused whole, said so, and the line is still the shell's.
    seed(fixture, vec![b'x'; TOO_LONG_BYTES])?;
    ut.keys(PASTE_KEY)?;
    ut.sees(
        TOO_LONG,
        "a paste past the line's limit was not refused aloud",
    )?;
    seed(fixture, b"echo a\tb".to_vec())?;
    ut.keys(PASTE_KEY)?;
    ut.sees(UNPRINTABLE, "a tab was not refused aloud")?;
    ut.keys(b"echo ut-paste-four | tr a-z A-Z\r")?;
    ut.sees(FOUR_RAN, "a refused paste left something in the line")?;

    ut.leaves()?;
    fixture.no_sessions()
}

fn stopped(fixture: &mut Fixture) -> Result {
    let ut = Ut::start("hold-paste")?;

    // Typing runs while the server sits on the paste, and Enter ends the fetch at once.
    fixture.ask(b'H')?;
    fixture.ask(b'E')?;
    ut.keys(PASTE_KEY)?;
    fixture.wait(b'h', ROOM_MS, "the paste never reached the stopped server")?;
    ut.keys(PASTE_KEY)?;
    ut.sees(STILL_FETCHING, "a second key did not say the first is out")?;
    ut.keys(b"echo ut-paste-five | tr a-z A-Z\r")?;
    ut.sees(FIVE_RAN, "nothing typed ran with a paste parked")?;
    fixture.wait(b'e', PROMPTLY_MS, "Enter did not end the fetch")?;

    // A command does not keep a fetch open: `head` sits on the pts until it is told to go
    // on, and the session ends while it does. The shell ends the fetch after making the
    // terminal the command's and before spawning it, so `go` is a line however far the
    // spawn has got: an Enter that stays a bare CR leaves `head` waiting here for good.
    fixture.ask(b'H')?;
    fixture.ask(b'E')?;
    ut.keys(PASTE_KEY)?;
    fixture.wait(b'h', ROOM_MS, "no new fetch before the command")?;
    ut.keys(b"head -n 1; echo ut-paste-seven | tr a-z A-Z\r")?;
    fixture.wait(
        b'e',
        PROMPTLY_MS,
        "a foreground command kept the fetch open",
    )?;
    if Ut::saw(SEVEN_RAN) {
        return Err("the command did not wait, so nothing was shown");
    }
    ut.keys(b"go\r")?;
    ut.sees(SEVEN_RAN, "the command did not run on")?;

    // A line typed for the command, in one write with the line that runs it: the shell
    // reads no further than its Enter. A line feed, since what is already in the ring
    // when the terminal turns cooked keeps its bare CR. The second `head` gets an Enter
    // typed once the first has gone: the gap between them stays cooked, so it is a line
    // feed wherever it lands.
    ut.keys(b"head -n 1 | tr a-z A-Z; head -n 1 | tr a-z A-Z\rut-paste-eight\n")?;
    ut.sees(EIGHT_RAN, "a line typed for the command went to the shell")?;
    ut.keys(b"ut-paste-nine\r")?;
    ut.sees(NINE_RAN, "the line's second command did not get its line")?;

    // Ctrl-C ends one too.
    fixture.ask(b'H')?;
    fixture.ask(b'E')?;
    ut.keys(PASTE_KEY)?;
    fixture.wait(b'h', ROOM_MS, "no new fetch after the last")?;
    ut.keys(b"\x03")?;
    fixture.wait(b'e', PROMPTLY_MS, "Ctrl-C did not end the fetch")?;

    // Left alone, the client gives up at its deadline, and the shell says so.
    fixture.ask(b'H')?;
    fixture.ask(b'E')?;
    ut.keys(PASTE_KEY)?;
    fixture.wait(b'h', ROOM_MS, "no new fetch after the Ctrl-C")?;
    fixture.wait(
        b'e',
        ROOM_MS,
        "the client never gave up on the stopped paste",
    )?;
    ut.sees(NO_ANSWER, "the failed paste was never reported")?;
    ut.keys(b"echo ut-paste-six | tr a-z A-Z\r")?;
    ut.sees(SIX_RAN, "typing did not survive the failed paste")?;

    // Leaving with one parked is not held up by it.
    fixture.ask(b'H')?;
    ut.keys(PASTE_KEY)?;
    fixture.wait(b'h', ROOM_MS, "no new fetch after the failed one")?;
    ut.leaves()?;
    fixture.no_sessions()
}

fn test() -> Result {
    const EOPNOTSUPP: i32 = -95;
    let mut fixture = Fixture::start("hold-paste")?;
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err("the registry");
    }
    let locator = parse_locator(b"/srv/fixture/dir/interaction").ok_or("locator shape")?;
    let ring = Host::new(ThylaKernel, srv as i32, locator);
    let _ = unsafe { t_close(srv) };
    match ring.map(drop) {
        Ok(()) => {}
        Err(EOPNOTSUPP) => {
            t_putstr("ut-paste: UNAVAILABLE\n");
            return fixture.finish_unused();
        }
        Err(_) => return Err("the private ring"),
    }
    answered(&mut fixture)?;
    stopped(&mut fixture)?;
    fixture.finish()?;
    t_putstr("ut-paste: PASS -- the key fetched, nothing submitted, and a stopped server stopped nothing\n");
    Ok(())
}

pub fn run() -> i64 {
    match test() {
        Ok(()) => 0,
        Err(why) => {
            t_putstr(&format!("ut-paste: terminal's last bytes: {}\n", tail()));
            t_putstr(&format!("ut-paste: FAIL -- {}\n", why));
            1
        }
    }
}
