//! Kernel endpoint mirror; contains no raw pixels, user pointers or GPU commands.
pub const STATUS: u64 = 1;
pub const INPUT: u64 = 2;
pub const ACK: u64 = 3;
pub const FRAME: u64 = 4;
pub const KEY: u64 = 5;
pub const RESTORED: u64 = 6;
pub const FAIL: u64 = 7;
pub const QUERY: u64 = 8;
pub const VISIBLE: u64 = 9;
pub const MASK: u64 = 10;
pub const GRANT: u64 = 11;
pub const CLIENT: u64 = 12;
pub const RENEW: u64 = 13;
// QUERY's `code`: the console episode's medium. Other replies carry the mask
// length there.
pub const MEDIUM_NONE: u32 = 0;
pub const MEDIUM_SERIAL: u32 = 1;
pub const MEDIUM_GRAPHICAL: u32 = 2;
// ABI: seat.h pins the same three. Swapped, Corvus prompts on the wrong medium.
const _: () = assert!(MEDIUM_NONE == 0 && MEDIUM_SERIAL == 1 && MEDIUM_GRAPHICAL == 2);
/// Where the kernel says the console episode runs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EpisodeMedium {
    /// No episode is open: it ended before the question was asked.
    Gone,
    Serial,
    Graphical { generation: u64 },
}
#[repr(C)]
pub struct Message {
    pub generation: u64,
    pub sequence: u64,
    pub phase: u32,
    pub length: u32,
    pub code: u32,
    pub value: u32,
    pub data: [u8; 512],
}
impl Default for Message {
    fn default() -> Self {
        Self { generation: 0, sequence: 0, phase: 0, length: 0, code: 0, value: 0, data: [0; 512] }
    }
}
const _: () = assert!(core::mem::size_of::<Message>() == 544);
const _: () = assert!(core::mem::offset_of!(Message, data) == 32);
#[cfg(feature = "guest")]
pub fn call(op: u64, msg: &mut Message) -> Result<(), ()> {
    let mut result = op as i64;
    unsafe {
        core::arch::asm!("svc #0", inlateout("x0") result,
            in("x1") msg as *mut Message as u64,
            in("x8") libthyla_rs::T_SYS_TRUSTED_SEAT, options(nostack));
    }
    if result == 0 { Ok(()) } else { Err(()) }
}
impl Message {
    /// Read QUERY's reply. `None` is a reply the kernel never sends. The phase
    /// plays no part: a NORMAL seat may hold no episode, and a FAILED one a
    /// serial episode.
    pub fn episode_medium(&self) -> Option<EpisodeMedium> {
        match self.code {
            MEDIUM_NONE => Some(EpisodeMedium::Gone),
            MEDIUM_SERIAL => Some(EpisodeMedium::Serial),
            MEDIUM_GRAPHICAL if self.generation != 0 => Some(EpisodeMedium::Graphical { generation: self.generation }),
            _ => None,
        }
    }
    /// A QUERY reply: is the episode open now the graphical one at `generation`?
    pub fn owns_graphical_episode(&self, generation: u64) -> bool {
        self.episode_medium() == Some(EpisodeMedium::Graphical { generation })
    }
}
impl Drop for Message {
    fn drop(&mut self) {
        // INPUT/KEY can carry credentials. Scrub even on an error return.
        let p = self as *mut Self as *mut u8;
        for i in 0..core::mem::size_of::<Self>() {
            unsafe { core::ptr::write_volatile(p.add(i), 0); }
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;

    fn reply(phase: u32, code: u32, generation: u64) -> Message {
        let mut m = Message::default();
        m.phase = phase;
        m.code = code;
        m.generation = generation;
        m
    }

    #[test]
    fn a_normal_seat_with_no_episode_is_gone_not_serial() {
        assert_eq!(reply(0, MEDIUM_NONE, 7).episode_medium(), Some(EpisodeMedium::Gone));
    }

    #[test]
    fn the_code_decides_whatever_the_phase_says() {
        assert_eq!(reply(0, MEDIUM_SERIAL, 7).episode_medium(), Some(EpisodeMedium::Serial));
        assert_eq!(reply(4, MEDIUM_SERIAL, 7).episode_medium(), Some(EpisodeMedium::Serial));
        assert_eq!(reply(2, MEDIUM_NONE, 7).episode_medium(), Some(EpisodeMedium::Gone));
    }

    #[test]
    fn graphical_carries_its_generation_and_needs_one() {
        assert_eq!(
            reply(2, MEDIUM_GRAPHICAL, 7).episode_medium(),
            Some(EpisodeMedium::Graphical { generation: 7 })
        );
        assert_eq!(reply(2, MEDIUM_GRAPHICAL, 0).episode_medium(), None);
    }

    #[test]
    fn only_the_same_graphical_episode_is_owned() {
        assert!(reply(2, MEDIUM_GRAPHICAL, 7).owns_graphical_episode(7));
        assert!(!reply(2, MEDIUM_GRAPHICAL, 8).owns_graphical_episode(7), "a newer graphical episode");
        assert!(!reply(0, MEDIUM_SERIAL, 7).owns_graphical_episode(7), "a serial episode, same generation");
        assert!(!reply(0, MEDIUM_NONE, 7).owns_graphical_episode(7), "no episode");
    }

    #[test]
    fn a_code_the_kernel_never_sends_is_refused() {
        assert_eq!(reply(0, 3, 7).episode_medium(), None);
        assert_eq!(reply(2, u32::MAX, 7).episode_medium(), None);
    }
}
