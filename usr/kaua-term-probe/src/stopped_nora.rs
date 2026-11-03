//! A server stopped mid-copy does not stop typing (ASYNC-SERVICE-LIFECYCLE, AS-4d): a
//! real Nora against loom-fixture, driven by tools/interactive/ls-nora-stopped-copy.exp.
//! ut can neither set a child's environment nor make anything but a raw command's
//! terminal raw, so this hosts the editor on a pts of its own.
//!
//! The first Nora is the control, one variable away: the same keys against the fixture
//! that answers, so that a copy which never starts cannot pass for one that is held.
use ::alloc::{format, string::String, vec::Vec};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use interaction_native::{parse_locator, Host};
use libhalcyon::interaction_body::IDLE_MS;
use libthyla_rs::{
    env,
    fs::{File, OpenOptions},
    io::{Read, Write},
    loom::ThylaKernel,
    process::{pipe, Child, Command, Stdio},
    thread,
    time::{monotonic_ns, sleep, Duration},
    *,
};
use vt::Vt;

pub(crate) type Result<T = ()> = core::result::Result<T, &'static str>;

const EOPNOTSUPP: i32 = -95;
const MS: u64 = 1_000_000;
pub(crate) const DRAIN_STACK: u64 = 64 * 1024;
const RAW: &[u8] = b"-icanon -echo -isig -icrnl -onlcr";
const COLS: u16 = 80;
const ROWS: u16 = 24;
/// What the client's deadline would release. A wait on it is what this must catch, so
/// whatever must not wait for it gets half of it.
pub(crate) const PROMPTLY_MS: u64 = IDLE_MS as u64 / 2;
pub(crate) const ROOM_MS: u64 = IDLE_MS as u64 * 2;
/// Longer than the keyboard source's hold on a lone Escape.
const ESC_SETTLE_MS: u64 = 200;
pub(crate) const TICK_MS: u64 = 20;
const FILE_TICK_MS: u64 = 100;

// What the status row comes to show. A frame is painted by difference, so a word's bytes
// can have a cursor move anywhere inside them, and only the screen is sure to spell it.
const FIRST_PAINT: u32 = 0;
const COPIED: u32 = 1;
const PASTED: u32 = 2;
const NO_ANSWER: u32 = 3;
const WORDS: [&str; 4] = ["NOR", "copied", "pasted", "answer"];

static SEEN: AtomicU32 = AtomicU32::new(0);
static DRAINED: AtomicBool = AtomicBool::new(false);

/// A pts master does not poll, and an editor whose output has nowhere to go stops, so
/// its output has a thread of its own for the life of the editor.
extern "C" fn drain(arg: u64) {
    let master = arg as i64;
    let mut screen = Vt::new(COLS as usize, ROWS as usize);
    let mut buffer = [0u8; 2048];
    loop {
        let n = unsafe { t_read(master, buffer.as_mut_ptr(), buffer.len()) };
        if n <= 0 {
            break;
        }
        screen.feed(&buffer[..n as usize]);
        for (i, word) in WORDS.iter().enumerate() {
            if screen.row_holds(ROWS as usize - 1, word) {
                SEEN.fetch_or(1 << i, Ordering::SeqCst);
            }
        }
    }
    drop(screen);
    DRAINED.store(true, Ordering::SeqCst);
    unsafe { t_thread_exit() }
}

pub(crate) fn pause(ms: u64) -> Result {
    sleep(Duration::from_millis(ms)).map_err(|_| "sleep")
}

pub(crate) fn after(ms: u64) -> u64 {
    monotonic_ns().saturating_add(ms * MS)
}

/// Whole occurrences, none overlapping: `alphalpha` holds one.
fn holds(text: &[u8], word: &[u8]) -> usize {
    let mut count = 0;
    let mut at = 0;
    while at + word.len() <= text.len() {
        if text[at..].starts_with(word) {
            count += 1;
            at += word.len();
        } else {
            at += 1;
        }
    }
    count
}

pub(crate) struct Fixture {
    child: Child,
    control: File,
    notify: File,
    heard: Vec<u8>,
}

impl Fixture {
    pub(crate) fn start() -> Result<Self> {
        let (control_end, control) = pipe().map_err(|_| "the control pipe")?;
        let (notify, notify_end) = pipe().map_err(|_| "the notify pipe")?;
        let mut command = Command::new("/bin/loom-fixture");
        command
            .arg("--srv")
            .stdout(Stdio::File(control_end))
            .stderr(Stdio::File(notify_end));
        let child = command.spawn().map_err(|_| "spawn the fixture")?;
        let mut fixture = Self {
            child,
            control,
            notify,
            heard: Vec::new(),
        };
        fixture.wait(b'R', ROOM_MS, "the fixture never posted")?;
        Ok(fixture)
    }

    pub(crate) fn ask(&mut self, byte: u8) -> Result {
        self.control
            .write_all(&[byte])
            .map_err(|_| "ask the fixture")
    }

    /// Takes whatever the notify pipe holds, waiting up to `timeout_ms` for the first.
    fn listen(&mut self, timeout_ms: i32) -> Result {
        let mut p = TPollFd {
            fd: self.notify.as_raw_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        if unsafe { t_poll(&mut p, 1, timeout_ms) } <= 0 {
            return Ok(());
        }
        let mut byte = [0u8; 1];
        match self.notify.read(&mut byte) {
            Ok(1) => {
                self.heard.push(byte[0]);
                Ok(())
            }
            _ => Err("the fixture hung up"),
        }
    }

    fn take(&mut self, byte: u8) -> bool {
        match self.heard.iter().position(|b| *b == byte) {
            Some(i) => {
                self.heard.remove(i);
                true
            }
            None => false,
        }
    }

    pub(crate) fn wait(&mut self, byte: u8, patience_ms: u64, why: &'static str) -> Result {
        let limit = after(patience_ms);
        while !self.take(byte) {
            if monotonic_ns() >= limit {
                return Err(why);
            }
            self.listen(TICK_MS as i32)?;
        }
        Ok(())
    }

    /// Whether the fixture has already said so, without waiting and without taking it.
    pub(crate) fn said(&mut self, byte: u8) -> Result<bool> {
        self.listen(0)?;
        Ok(self.heard.contains(&byte))
    }

    /// No session left, with whoever had one gone.
    pub(crate) fn no_sessions(&mut self) -> Result {
        self.ask(b'Q')?;
        self.wait(b'Z', PROMPTLY_MS, "a session outlived its editor")
    }

    /// With no session, the fixture answers 76.
    pub(crate) fn finish_unused(self) -> Result {
        let Self {
            mut child, control, ..
        } = self;
        drop(control);
        let _ = child.wait();
        Ok(())
    }

    /// 77 is every session ended and no violation.
    pub(crate) fn finish(self) -> Result {
        let Self {
            mut child, control, ..
        } = self;
        drop(control);
        let status = child.wait().map_err(|_| "wait for the fixture")?;
        let code = status.code().unwrap_or(-1);
        if code != 77 {
            t_putstr(&format!("stopped-nora: the fixture exited {}\n", code));
            return Err("the fixture found a violation");
        }
        Ok(())
    }
}

struct Nora {
    child: Child,
    master: i64,
    path: String,
}

impl Nora {
    fn start(service: &str, name: &str) -> Result<Self> {
        let home = env::var("HOME").ok_or("no home")?;
        let path = format!("{}/{}", home, name);
        File::create(&path).map_err(|_| "the file to edit")?;
        // The spawn copies this environment.
        let locator = format!("/srv/{}/dir/interaction", service);
        File::create("/env/HALCYON_INTERACTION")
            .and_then(|mut f| f.write_all(locator.as_bytes()))
            .map_err(|_| "the locator")?;
        let master = ptyhold::Master::mint().map_err(|_| "a pts")?;
        OpenOptions::new()
            .write(true)
            .open(format!("/dev/pts/{}ctl", master.n))
            .and_then(|mut ctl| ctl.write_all(RAW))
            .map_err(|_| "a raw pts")?;
        master.seed_winsize(COLS, ROWS);
        SEEN.store(0, Ordering::SeqCst);
        DRAINED.store(false, Ordering::SeqCst);
        let child = master
            .spawn_on_slave(&[String::from("/bin/nora"), path.clone()])
            .map_err(|_| "spawn the editor")?;
        let stack = unsafe { t_burrow_attach(DRAIN_STACK) };
        if stack < 0 {
            return Err("a stack to drain on");
        }
        let top = stack as u64 + DRAIN_STACK;
        unsafe { thread::spawn_raw(drain as *const () as u64, top, master.mfd as u64, 0) }
            .map_err(|_| "a thread to drain on")?;
        let nora = Self {
            child,
            master: master.mfd,
            path,
        };
        nora.sees(FIRST_PAINT, "the editor never painted")?;
        Ok(nora)
    }

    fn sees(&self, word: u32, why: &'static str) -> Result {
        let limit = after(PROMPTLY_MS);
        while SEEN.load(Ordering::SeqCst) & (1 << word) == 0 {
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

    /// Text in at the end of the line, back to Normal, and write the file. A copy leaves
    /// the cursor inside what it copied, and `i` would split that word.
    fn insert(&self, text: &[u8]) -> Result {
        self.keys(b"A")?;
        self.keys(text)?;
        self.keys(b"\x1b")?;
        pause(ESC_SETTLE_MS)?;
        self.keys(b":w\r")
    }

    /// Five letters from the start of the line, to the shared clipboard.
    fn copy_word(&self) -> Result {
        self.keys(b"0vllll y")
    }

    fn saved(&self) -> Result<Vec<u8>> {
        let mut file = File::open(&self.path).map_err(|_| "open the file")?;
        let mut text = Vec::new();
        let mut chunk = [0u8; 512];
        loop {
            match file.read(&mut chunk) {
                Ok(0) => return Ok(text),
                Ok(n) => text.extend_from_slice(&chunk[..n]),
                Err(_) => return Err("read the file"),
            }
        }
    }

    /// `:wq`, and it is gone without waiting for anything a server owes.
    fn leaves(mut self) -> Result<Vec<u8>> {
        self.keys(b":wq\r")?;
        let limit = after(PROMPTLY_MS);
        loop {
            match self.child.try_wait() {
                Ok(Some(status)) if status.success() => break,
                Ok(Some(_)) => return Err("the editor failed"),
                Ok(None) if monotonic_ns() >= limit => return Err("the editor did not leave"),
                Ok(None) => pause(TICK_MS)?,
                Err(_) => return Err("wait for the editor"),
            }
        }
        while !DRAINED.load(Ordering::SeqCst) {
            if monotonic_ns() >= limit {
                return Err("the terminal did not end with the editor");
            }
            pause(TICK_MS)?;
        }
        let _ = unsafe { t_close(self.master) };
        self.saved()
    }
}

/// The same keys, answered: the copy lands, the paste brings it back.
fn control(fixture: &mut Fixture) -> Result {
    let nora = Nora::start("fixture", "stopped-nora-control.txt")?;
    nora.insert(b"alpha")?;
    nora.copy_word()?;
    nora.sees(COPIED, "the answered copy did not land")?;
    // Past the end of the line, so that the file holds two whole words.
    nora.keys(b"A\x1b")?;
    pause(ESC_SETTLE_MS)?;
    nora.keys(b" p")?;
    nora.sees(PASTED, "the answered paste did not land")?;
    let text = nora.leaves()?;
    if holds(&text, b"alpha") != 2 {
        return Err("the answered paste is not in the file");
    }
    fixture.no_sessions()
}

fn stopped(fixture: &mut Fixture) -> Result {
    fixture.ask(b'H')?;
    fixture.ask(b'E')?;
    let nora = Nora::start("hold-copy", "stopped-nora.txt")?;
    nora.insert(b"bravo")?;
    nora.copy_word()?;
    fixture.wait(b'h', ROOM_MS, "the copy never reached the stopped server")?;

    // The copy is parked for as long as the fixture has not seen the session end, so
    // this needs no clock: typing that waited for the deadline lands after that.
    nora.insert(b"charlie")?;
    let limit = after(ROOM_MS);
    while holds(&nora.saved()?, b"charlie") == 0 {
        if fixture.said(b'e')? {
            return Err("nothing typed landed until the copy's deadline");
        }
        if monotonic_ns() >= limit {
            return Err("nothing typed landed");
        }
        pause(FILE_TICK_MS)?;
    }
    if fixture.said(b'e')? {
        return Err("nothing typed landed until the copy's deadline");
    }
    // The client gives up at its deadline, and the editor says so.
    fixture.wait(
        b'e',
        ROOM_MS,
        "the client never gave up on the stopped copy",
    )?;
    nora.sees(NO_ANSWER, "the failed copy was never reported")?;

    nora.insert(b"delta")?;
    let limit = after(PROMPTLY_MS);
    while holds(&nora.saved()?, b"delta") == 0 {
        if monotonic_ns() >= limit {
            return Err("typing did not survive the failed copy");
        }
        pause(FILE_TICK_MS)?;
    }

    // A second copy over a new connection, and leaving while the server sits on it.
    fixture.ask(b'H')?;
    nora.copy_word()?;
    fixture.wait(b'h', ROOM_MS, "no new copy after the failed one")?;
    let text = nora.leaves()?;
    for word in [&b"bravo"[..], b"charlie", b"delta"] {
        if holds(&text, word) != 1 {
            return Err("the file does not hold what was typed");
        }
    }
    fixture.no_sessions()
}

fn test() -> Result {
    let mut fixture = Fixture::start()?;
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
            t_putstr("stopped-nora: UNAVAILABLE\n");
            return fixture.finish_unused();
        }
        Err(_) => return Err("the private ring"),
    }
    control(&mut fixture)?;
    stopped(&mut fixture)?;
    fixture.finish()?;
    t_putstr("stopped-nora: PASS -- typing went on with a copy parked at a stopped server\n");
    Ok(())
}

pub fn run() -> i64 {
    match test() {
        Ok(()) => 0,
        Err(why) => {
            t_putstr(&format!("stopped-nora: FAIL -- {}\n", why));
            1
        }
    }
}
