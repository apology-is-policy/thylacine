// The app's end of the line (4.4): connect to QEMU's socket, take the driver's HELLO and answer
// with this end's, then run one 9P2000.L session per channel, each on its own thread so that one
// channel's slow host call never stalls another. When QEMU's end goes (the VM stopped, or the
// connection broke), every session ends with it, and the app connects again.
use std::collections::HashMap;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use macline::{End, Event, Kind, Line, Parser};

use super::log::Log;
use super::snarf::Pasteboard;
use super::tree::{Session, Shared};
use super::Config;

/// The largest frame this end accepts.
const MAXFRAME: u32 = 65536;
/// A channel whose queue toward its session passes this many bytes is closed (the driver's rule
/// for its own queues, 4.4: one channel never stalls another).
const QUEUE_MAX: usize = 16 << 20;
const RETRY: Duration = Duration::from_millis(200);

/// Serve the line until the process ends (or, with `once`, until the first connection ends).
pub fn serve(cfg: Config, pasteboard: Arc<dyn Pasteboard>, log: Arc<Log>) -> Result<(), String> {
    let shared = Arc::new(Shared::new(cfg.shares, pasteboard, log.clone())?);
    log.line(&format!(
        "start pid={} socket={}",
        std::process::id(),
        cfg.socket.display()
    ));
    loop {
        let stream = match UnixStream::connect(&cfg.socket) {
            Ok(s) => s,
            Err(_) => {
                thread::sleep(RETRY);
                continue;
            }
        };
        log.line("connected");
        match connection(stream, &shared) {
            Ok(()) => log.line("line closed by QEMU"),
            Err(why) => log.line(&why),
        }
        if cfg.once {
            return Ok(());
        }
    }
}

/// The state the reader and the sessions share: the line's rules, and whether the connection
/// still stands. The stream's writing half has its own lock, taken first.
struct Wire {
    line: Line,
    live: bool,
}

struct Ends {
    out: Mutex<UnixStream>,
    wire: Mutex<Wire>,
    log: Arc<Log>,
}

impl Ends {
    fn lock_wire(&self) -> std::sync::MutexGuard<'_, Wire> {
        self.wire.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// A channel's bytes as DATA frames, while the channel may still carry them.
    fn send_data(&self, chan: u32, bytes: &[u8]) {
        let mut out = self.out.lock().unwrap_or_else(|e| e.into_inner());
        let peer_max = {
            let w = self.lock_wire();
            if !w.live || !w.line.sendable(chan) {
                return;
            }
            w.line
                .peer_max()
                .expect("a channel opens only after the HELLO")
        };
        let mut frames = Vec::with_capacity(bytes.len() + 64);
        macline::data_frames(&mut frames, chan, bytes, peer_max);
        // A failed write is the connection's end, which the reader sees.
        let _ = out.write_all(&frames);
    }

    /// This end's CLOSE for `chan`, when one is owed.
    fn send_close(&self, chan: u32) {
        let mut out = self.out.lock().unwrap_or_else(|e| e.into_inner());
        let owed = {
            let mut w = self.lock_wire();
            w.live && w.line.close(chan)
        };
        if owed {
            let mut f = Vec::with_capacity(macline::HEADER);
            macline::put(&mut f, Kind::Close, chan, &[]);
            let _ = out.write_all(&f);
        }
    }
}

struct Channel {
    tx: mpsc::Sender<Vec<u8>>,
    queued: Arc<AtomicUsize>,
}

/// One connection, from its first byte to its end: Ok when QEMU closed it, Err with the record's
/// line when the line was malformed or the driver reset it.
fn connection(mut stream: UnixStream, shared: &Arc<Shared>) -> Result<(), String> {
    let out = stream.try_clone().map_err(|e| format!("line error: {e}"))?;
    let ends = Arc::new(Ends {
        out: Mutex::new(out),
        wire: Mutex::new(Wire {
            line: Line::new(End::App),
            live: true,
        }),
        log: shared.log.clone(),
    });
    let mut parser = Parser::new(MAXFRAME);
    let mut chans: HashMap<u32, Channel> = HashMap::new();
    let mut buf = vec![0u8; 65536];
    let result = 'conn: loop {
        let n = match stream.read(&mut buf) {
            Ok(0) => break Ok(()),
            Ok(n) => n,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => break Err(format!("line error: {e}")),
        };
        parser.push(&buf[..n]);
        loop {
            let (kind, chan, data) = match parser.next() {
                Ok(Some(f)) => f,
                Ok(None) => break,
                Err(m) => break 'conn Err(format!("line malformed: {m:?}")),
            };
            let event = ends.lock_wire().line.receive(kind, chan, data);
            match event {
                Err(m) => break 'conn Err(format!("line malformed: {m:?}")),
                Ok(Event::Hello(max)) => {
                    shared.log.line(&format!("hello maxframe={max}"));
                    let mut f = Vec::new();
                    macline::put_hello(&mut f, MAXFRAME);
                    let mut out = ends.out.lock().unwrap_or_else(|e| e.into_inner());
                    if let Err(e) = out.write_all(&f) {
                        break 'conn Err(format!("line error: {e}"));
                    }
                }
                Ok(Event::Discarded) => {
                    shared.log.line(&format!(
                        "discarded {} on chan {chan} before the driver's HELLO",
                        kind as u8
                    ));
                }
                Ok(Event::Opened(c)) => {
                    shared.log.line(&format!("open chan {c}"));
                    let (tx, rx) = mpsc::channel();
                    let queued = Arc::new(AtomicUsize::new(0));
                    chans.insert(
                        c,
                        Channel {
                            tx,
                            queued: queued.clone(),
                        },
                    );
                    let (ends, shared) = (ends.clone(), shared.clone());
                    thread::Builder::new()
                        .name(format!("mac line chan {c}"))
                        .spawn(move || session(c, rx, queued, ends, shared))
                        .map_err(|e| format!("line error: no thread for chan {c}: {e}"))?;
                }
                Ok(Event::Data(c, d)) => {
                    if let Some(ch) = chans.get(&c) {
                        let len = d.len();
                        if ch.queued.fetch_add(len, Ordering::AcqRel) + len > QUEUE_MAX {
                            shared.log.line(&format!(
                                "chan {c} closed: its queue passed {QUEUE_MAX} bytes"
                            ));
                            // Its session ends, and sends this end's CLOSE.
                            chans.remove(&c);
                        } else if ch.tx.send(d).is_err() {
                            // The session ended on its own (its stream was not 9P) and closed.
                            chans.remove(&c);
                        }
                    }
                }
                Ok(Event::Closed { chan: c, .. }) => {
                    // The session drains what it has, answers it, and sends this end's CLOSE.
                    chans.remove(&c);
                    shared.log.line(&format!("close chan {c}"));
                }
                Ok(Event::Reset) => {
                    shared.log.line("reset by the driver");
                    break 'conn Err("line malformed: reset".to_string());
                }
            }
        }
    };
    // Every session's writes from here fall on a dead connection, and its queue's end stops it.
    ends.lock_wire().live = false;
    drop(chans);
    let _ = stream.shutdown(std::net::Shutdown::Both);
    result
}

fn session(
    chan: u32,
    rx: mpsc::Receiver<Vec<u8>>,
    queued: Arc<AtomicUsize>,
    ends: Arc<Ends>,
    shared: Arc<Shared>,
) {
    let mut s = Session::new(chan, shared);
    let mut out = Vec::new();
    for data in rx {
        queued.fetch_sub(data.len(), Ordering::AcqRel);
        out.clear();
        let fed = s.feed(&data, &mut out);
        if !out.is_empty() {
            ends.send_data(chan, &out);
        }
        if let Err(why) = fed {
            ends.log.line(&format!("chan {chan} closed: {why}"));
            break;
        }
    }
    // The session's files and locks go before its CLOSE says it is gone.
    drop(s);
    ends.send_close(chan);
}
