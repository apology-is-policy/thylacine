// The pasteboard behind `snarf`: the Mac's general pasteboard, or one in memory for a test.
use std::sync::Mutex;

use objc2::rc::autoreleasepool;
use objc2_app_kit::{NSPasteboard, NSPasteboardTypeString};
use objc2_foundation::NSString;

pub trait Pasteboard: Send + Sync {
    /// The pasteboard's plain text as UTF-8; empty when it holds no text.
    fn text(&self) -> Vec<u8>;
    /// Replace what the pasteboard holds with `text`. False when the pasteboard refused it.
    fn set_text(&self, text: &str) -> bool;
}

/// The Mac's general pasteboard. NSPasteboard may be used from any thread.
pub struct SystemPasteboard;

impl Pasteboard for SystemPasteboard {
    fn text(&self) -> Vec<u8> {
        autoreleasepool(|_| {
            let pb = NSPasteboard::generalPasteboard();
            // SAFETY: NSPasteboardTypeString is an immutable framework constant.
            let kind = unsafe { NSPasteboardTypeString };
            match pb.stringForType(kind) {
                Some(s) => s.to_string().into_bytes(),
                None => Vec::new(),
            }
        })
    }

    fn set_text(&self, text: &str) -> bool {
        autoreleasepool(|_| {
            let pb = NSPasteboard::generalPasteboard();
            // SAFETY: as above.
            let kind = unsafe { NSPasteboardTypeString };
            pb.clearContents();
            pb.setString_forType(&NSString::from_str(text), kind)
        })
    }
}

/// A pasteboard that is a string, for a test that must not touch the person's pasteboard.
pub struct MemoryPasteboard(Mutex<String>);

impl MemoryPasteboard {
    pub fn new(text: &str) -> MemoryPasteboard {
        MemoryPasteboard(Mutex::new(text.to_string()))
    }
}

impl Pasteboard for MemoryPasteboard {
    fn text(&self) -> Vec<u8> {
        self.0
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
            .into_bytes()
    }

    fn set_text(&self, text: &str) -> bool {
        *self.0.lock().unwrap_or_else(|e| e.into_inner()) = text.to_string();
        true
    }
}
