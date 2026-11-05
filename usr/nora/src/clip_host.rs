//! The session clipboard and mode report over a private ring: the binary's half of
//! the shared clipboard, as `lsp_host` is of the language server. The engine raises
//! requests and takes text. What crosses the ring, and every deadline, is the client's
//! (ASYNC-SERVICE-LIFECYCLE, AS-4d), and nothing here waits on the server. An answer's line
//! to the console parks, as any console write does, for the length of a trusted episode.
use alloc::string::String;
use interaction_native::{parse_locator, Host};
use libhalcyon::interaction_client::{Event, Refusal, Why};
use libhalcyon::interaction_wire::Mode;
use libthyla_rs::{
    env, loom::ThylaKernel, t_close, t_open, t_putstr, T_OPATH, T_WALK_OPEN_FROM_ROOT,
};
use nora::editor::{ClipboardRequest, Editor, PasteAskers};
use parley::transport::Tag;

/// Poll tags 0 to 7 are lsp_host's and dap_host's.
pub const TAG_CLIP: Tag = 8;
const MS: u64 = 1_000_000;
const LABEL: &str = "Nora";

pub struct Clip {
    host: Host<ThylaKernel>,
    reported: Option<(Mode, bool)>,
    paste_askers: PasteAskers,
}

fn why(why: Why) -> &'static str {
    match why {
        Why::Refused(_) => "the session refused it",
        Why::Lost => "the connection went away",
        Why::TimedOut => "no answer in time",
        Why::Malformed => "a reply broke the protocol",
        Why::Cancelled => "cancelled",
    }
}

impl Clip {
    /// None without an endpoint or the private ring, which is no error: the editor
    /// carries on with its local registers. The locator is not authority.
    pub fn start() -> Option<Self> {
        let value = env::var("HALCYON_INTERACTION")?;
        let locator = parse_locator(value.as_bytes())?;
        let path = b"/srv";
        let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OPATH) };
        if srv < 0 {
            return None;
        }
        let host = Host::new(ThylaKernel, srv as i32, locator);
        unsafe { t_close(srv) };
        Some(Self {
            host: host.ok()?,
            reported: None,
            paste_askers: PasteAskers::default(),
        })
    }

    pub fn poll_fd(&self) -> Option<(i32, Tag)> {
        self.host.poll_fd().map(|fd| (fd, TAG_CLIP))
    }

    /// How long the host's poll may sleep, if the client owes anything.
    pub fn deadline_ms(&self) -> Option<u32> {
        let owed = self.host.next_deadline()?;
        let left = owed.saturating_sub(self.host.now()).div_ceil(MS);
        Some(left.min(u32::MAX as u64) as u32)
    }

    /// Mode reports follow the editing mode. The client coalesces them.
    pub fn report_mode(&mut self, ed: &Editor) {
        let mode = match ed.mode_str() {
            "INS" => Mode::Insert,
            "VIS" => Mode::Visual,
            "CMD" => Mode::Command,
            _ => Mode::Normal,
        };
        let now_reporting = Some((mode, ed.is_readonly()));
        if self.reported != now_reporting {
            self.reported = now_reporting;
            let now = self.host.now();
            // A constant label of plain text is never turned down.
            let _ = self.host.report_mode(mode, ed.is_readonly(), LABEL, now);
        }
    }

    pub fn request(&mut self, request: ClipboardRequest, ed: &mut Editor) {
        let now = self.host.now();
        let asked = match request {
            ClipboardRequest::Copy(text) => self.host.copy(text.into_bytes(), now),
            ClipboardRequest::Paste(into) => {
                let asked = self.host.paste(now);
                if asked.is_ok() {
                    self.paste_askers.asked(into);
                }
                asked
            }
        };
        let refusal = match asked {
            Ok(()) => return,
            Err(Refusal::Busy) => "clipboard: still busy with the last request",
            Err(Refusal::TooLarge) => "clipboard: too large to copy",
            Err(Refusal::NotText) => "clipboard: that selection is not plain text",
        };
        ed.set_status(String::from(refusal));
    }

    /// Run what the ring has ready if its descriptor woke the poll or a deadline has
    /// passed. Returns true when the editor should repaint.
    pub fn service(&mut self, woke: bool, ed: &mut Editor) -> bool {
        let now = self.host.now();
        if woke || self.host.next_deadline().is_some_and(|at| at <= now) {
            self.host.pump(now);
        }
        let mut told = false;
        while let Some(event) = self.host.take_event() {
            told = true;
            match event {
                Event::Copied { .. } => {
                    t_putstr("nora: clipboard copied\n");
                    ed.set_status(String::from("copied to the clipboard"))
                }
                Event::CopyUnknown => {
                    t_putstr("nora: clipboard copy unknown\n");
                    ed.set_status(String::from(
                        "clipboard: the copy may or may not have landed",
                    ))
                }
                Event::CopyFailed(reason) => {
                    t_putstr(&alloc::format!("nora: clipboard copy failed: {}\n", why(reason)));
                    ed.set_status(alloc::format!("clipboard: copy failed: {}", why(reason)))
                }
                Event::Pasted(bytes) => match (
                    core::str::from_utf8(&bytes),
                    self.paste_askers.answered(),
                ) {
                    (Ok(text), Some(into)) => {
                        t_putstr(if ed.paste_text(into, text) {
                            "nora: clipboard pasted\n"
                        } else {
                            "nora: clipboard paste not applied\n"
                        });
                    }
                    // The client reports only the paste it was asked for.
                    (Ok(_), None) => {
                        t_putstr("nora: clipboard paste not applied\n");
                    }
                    (Err(_), _) => {
                        t_putstr("nora: clipboard paste failed: not text\n");
                        ed.set_status(String::from("clipboard: that was not text"));
                    }
                },
                Event::PasteFailed(reason) => {
                    let _ = self.paste_askers.answered();
                    t_putstr(&alloc::format!("nora: clipboard paste failed: {}\n", why(reason)));
                    ed.set_status(alloc::format!("clipboard: paste failed: {}", why(reason)))
                }
                // A mode report grants nothing, and the editor is not owed a word about it.
                Event::ModeRefused(_) => {}
            }
        }
        told
    }
}
