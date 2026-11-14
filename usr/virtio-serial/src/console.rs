//! The virtio console's control protocol (VIRTIO 1.2 section 5.3.6.2) as this driver uses it:
//! take each port the device adds, learn its name, open the one named `org.thylacine.mac`,
//! and follow whether the Mac app is on it (PORT_OPEN from the device).
//!
//! Every control message is `id[4] event[2] value[2]`, little-endian; PORT_NAME's name
//! follows. The device is not trusted: a port id past the ports this driver set queues up
//! for is refused, and a message for a port it never added is ignored.

use alloc::vec;
use alloc::vec::Vec;

pub const DEVICE_READY: u16 = 0;
pub const DEVICE_ADD: u16 = 1;
pub const DEVICE_REMOVE: u16 = 2;
pub const PORT_READY: u16 = 3;
pub const CONSOLE_PORT: u16 = 4;
pub const RESIZE: u16 = 5;
pub const PORT_OPEN: u16 = 6;
pub const PORT_NAME: u16 = 7;

/// `id[4] event[2] value[2]`.
pub const LEN: usize = 8;

/// The port the Mac app is on (docs/MAC-RUNNER.md 4.4).
pub const MAC_PORT: &[u8] = b"org.thylacine.mac";

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Control {
    pub id: u32,
    pub event: u16,
    pub value: u16,
}

pub fn encode(c: Control) -> [u8; LEN] {
    let mut b = [0u8; LEN];
    b[0..4].copy_from_slice(&c.id.to_le_bytes());
    b[4..6].copy_from_slice(&c.event.to_le_bytes());
    b[6..8].copy_from_slice(&c.value.to_le_bytes());
    b
}

/// A control message from the device, and what follows its header; `None` when the buffer
/// is shorter than the header.
pub fn decode(buf: &[u8]) -> Option<(Control, &[u8])> {
    if buf.len() < LEN {
        return None;
    }
    let c = Control {
        id: u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]),
        event: u16::from_le_bytes([buf[4], buf[5]]),
        value: u16::from_le_bytes([buf[6], buf[7]]),
    };
    Some((c, &buf[LEN..]))
}

/// A PORT_NAME's name. QEMU appends a NUL that the specification does not
/// (hw/char/virtio-serial-bus.c), so trailing NULs are not part of it.
pub fn port_name(rest: &[u8]) -> &[u8] {
    let mut end = rest.len();
    while end > 0 && rest[end - 1] == 0 {
        end -= 1;
    }
    &rest[..end]
}

/// What the driver must do for a control message.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Act {
    /// Send this control message to the device.
    Send(Control),
    /// The port named `org.thylacine.mac` is this one: give its receive queue buffers.
    Target(u32),
    /// The Mac app connected to the port.
    HostOpen,
    /// The Mac app went away.
    HostClosed,
    /// The device removed the port.
    TargetGone,
}

#[derive(Copy, Clone, Default)]
struct Port {
    added: bool,
    named: bool,
    host: bool,
}

pub struct Console {
    ports: Vec<Port>,
    target: Option<u32>,
}

impl Console {
    /// `ports` is how many ports this driver set queues up for (ids 0 to `ports - 1`).
    pub fn new(ports: u32) -> Console {
        Console { ports: vec![Port::default(); ports as usize], target: None }
    }

    /// The first message: the driver is ready for the device's ports.
    pub fn ready() -> Control {
        Control { id: 0, event: DEVICE_READY, value: 1 }
    }

    pub fn target(&self) -> Option<u32> {
        self.target
    }

    /// Is the Mac app on the target port?
    pub fn host_open(&self) -> bool {
        self.target.map_or(false, |t| self.ports[t as usize].host)
    }

    /// Every port the device added has said its name. A port given no name never does, so
    /// the caller bounds the wait.
    pub fn settled(&self) -> bool {
        self.ports.iter().any(|p| p.added) && self.ports.iter().all(|p| !p.added || p.named)
    }

    pub fn event(&mut self, c: Control, rest: &[u8]) -> Vec<Act> {
        let mut acts = Vec::new();
        let i = c.id as usize;
        match c.event {
            DEVICE_ADD => {
                if i >= self.ports.len() {
                    acts.push(Act::Send(Control { id: c.id, event: PORT_READY, value: 0 }));
                } else {
                    // A second add of a port it holds changes nothing it learned.
                    self.ports[i].added = true;
                    acts.push(Act::Send(Control { id: c.id, event: PORT_READY, value: 1 }));
                }
            }
            PORT_NAME => {
                if i < self.ports.len() && self.ports[i].added && !self.ports[i].named {
                    self.ports[i].named = true;
                    if self.target.is_none() && port_name(rest) == MAC_PORT {
                        self.target = Some(c.id);
                        acts.push(Act::Target(c.id));
                        acts.push(Act::Send(Control { id: c.id, event: PORT_OPEN, value: 1 }));
                        if self.ports[i].host {
                            acts.push(Act::HostOpen);
                        }
                    }
                }
            }
            PORT_OPEN => {
                if i < self.ports.len() && self.ports[i].added {
                    let open = c.value != 0;
                    let was = self.ports[i].host;
                    self.ports[i].host = open;
                    if self.target == Some(c.id) {
                        // An open while open is a new connection whose close was lost.
                        if was {
                            acts.push(Act::HostClosed);
                        }
                        if open {
                            acts.push(Act::HostOpen);
                        }
                    }
                }
            }
            DEVICE_REMOVE => {
                if i < self.ports.len() && self.ports[i].added {
                    let was = self.ports[i].host;
                    self.ports[i] = Port::default();
                    if self.target == Some(c.id) {
                        self.target = None;
                        if was {
                            acts.push(Act::HostClosed);
                        }
                        acts.push(Act::TargetGone);
                    }
                }
            }
            // CONSOLE_PORT and RESIZE belong to console ports; this driver opens none.
            _ => {}
        }
        acts
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn c(id: u32, event: u16, value: u16) -> Control {
        Control { id, event, value }
    }

    fn named(con: &mut Console, id: u32, name: &[u8]) -> Vec<Act> {
        con.event(c(id, PORT_NAME, 1), name)
    }

    #[test]
    fn a_message_is_id_event_value_little_endian() {
        let b = encode(c(0x0403_0201, PORT_OPEN, 1));
        assert_eq!(b, [1, 2, 3, 4, 6, 0, 1, 0]);
        let mut with_name = b.to_vec();
        with_name.extend_from_slice(b"org.thylacine.mac\0");
        let (m, rest) = decode(&with_name).unwrap();
        assert_eq!(m, c(0x0403_0201, PORT_OPEN, 1));
        assert_eq!(port_name(rest), MAC_PORT);
        assert_eq!(decode(&b[..7]), None);
        assert_eq!(port_name(b"\0\0"), b"");
        assert_eq!(Console::ready(), c(0, DEVICE_READY, 1));
    }

    #[test]
    fn each_port_added_is_taken_and_one_past_the_queues_is_refused() {
        let mut con = Console::new(2);
        assert_eq!(con.event(c(1, DEVICE_ADD, 0), &[]), [Act::Send(c(1, PORT_READY, 1))]);
        assert_eq!(con.event(c(2, DEVICE_ADD, 0), &[]), [Act::Send(c(2, PORT_READY, 0))]);
        assert!(!con.settled());
        assert_eq!(named(&mut con, 2, MAC_PORT), [], "a port it refused cannot be the target");
        assert_eq!(con.target(), None);
    }

    #[test]
    fn the_mac_port_is_opened_once_by_its_name() {
        let mut con = Console::new(4);
        con.event(c(1, DEVICE_ADD, 0), &[]);
        con.event(c(3, DEVICE_ADD, 0), &[]);
        assert_eq!(named(&mut con, 1, b"org.qemu.guest_agent.0\0"), []);
        assert!(!con.settled());
        assert_eq!(named(&mut con, 3, b"org.thylacine.mac\0"), [Act::Target(3), Act::Send(c(3, PORT_OPEN, 1))]);
        assert!(con.settled());
        assert_eq!(named(&mut con, 3, MAC_PORT), [], "one name per port");
        assert_eq!(named(&mut con, 2, MAC_PORT), [], "a port never added has no name");
        assert_eq!(con.target(), Some(3));
        assert_eq!(con.event(c(3, DEVICE_ADD, 0), &[]), [Act::Send(c(3, PORT_READY, 1))]);
        assert_eq!(con.target(), Some(3), "a second add forgets nothing");
        assert!(con.settled());
    }

    #[test]
    fn a_name_that_only_starts_like_the_mac_port_is_not_it() {
        let mut con = Console::new(2);
        con.event(c(1, DEVICE_ADD, 0), &[]);
        assert_eq!(named(&mut con, 1, b"org.thylacine.mac2"), []);
        assert_eq!(con.target(), None);
    }

    #[test]
    fn the_app_coming_and_going_is_told_only_for_the_target() {
        let mut con = Console::new(3);
        con.event(c(1, DEVICE_ADD, 0), &[]);
        con.event(c(2, DEVICE_ADD, 0), &[]);
        named(&mut con, 1, b"other");
        named(&mut con, 2, MAC_PORT);
        assert_eq!(con.event(c(1, PORT_OPEN, 1), &[]), []);
        assert_eq!(con.event(c(2, PORT_OPEN, 1), &[]), [Act::HostOpen]);
        assert!(con.host_open());
        assert_eq!(con.event(c(2, PORT_OPEN, 0), &[]), [Act::HostClosed]);
        assert_eq!(con.event(c(2, PORT_OPEN, 0), &[]), []);
        assert!(!con.host_open());
    }

    #[test]
    fn an_open_while_open_is_a_new_connection() {
        let mut con = Console::new(2);
        con.event(c(1, DEVICE_ADD, 0), &[]);
        named(&mut con, 1, MAC_PORT);
        con.event(c(1, PORT_OPEN, 1), &[]);
        assert_eq!(con.event(c(1, PORT_OPEN, 1), &[]), [Act::HostClosed, Act::HostOpen]);
    }

    #[test]
    fn an_app_already_there_when_the_name_comes_is_told() {
        let mut con = Console::new(2);
        con.event(c(1, DEVICE_ADD, 0), &[]);
        assert_eq!(con.event(c(1, PORT_OPEN, 1), &[]), []);
        assert_eq!(named(&mut con, 1, MAC_PORT), [Act::Target(1), Act::Send(c(1, PORT_OPEN, 1)), Act::HostOpen]);
    }

    #[test]
    fn the_target_removed_closes_the_line() {
        let mut con = Console::new(2);
        con.event(c(1, DEVICE_ADD, 0), &[]);
        named(&mut con, 1, MAC_PORT);
        con.event(c(1, PORT_OPEN, 1), &[]);
        assert_eq!(con.event(c(1, DEVICE_REMOVE, 0), &[]), [Act::HostClosed, Act::TargetGone]);
        assert_eq!(con.target(), None);
        assert_eq!(con.event(c(1, PORT_OPEN, 1), &[]), [], "a removed port is not there");
        // Added again, it is found again.
        con.event(c(1, DEVICE_ADD, 0), &[]);
        assert_eq!(named(&mut con, 1, MAC_PORT)[0], Act::Target(1));
    }

    #[test]
    fn messages_for_ports_never_added_or_past_the_queues_are_ignored() {
        let mut con = Console::new(2);
        assert_eq!(con.event(c(1, PORT_NAME, 0), MAC_PORT), [], "a name for a port never added");
        assert_eq!(con.event(c(1, PORT_OPEN, 1), &[]), []);
        assert_eq!(con.event(c(9, PORT_OPEN, 1), &[]), []);
        assert_eq!(con.event(c(9, DEVICE_REMOVE, 0), &[]), []);
        assert_eq!(con.event(c(u32::MAX, PORT_NAME, 0), MAC_PORT), []);
        assert_eq!(con.event(c(1, RESIZE, 0), &[]), []);
        assert_eq!(con.event(c(1, CONSOLE_PORT, 1), &[]), []);
        assert!(!con.settled(), "no port added: nothing has settled");
        // An open said before the port was added was ignored, so naming it says no app.
        con.event(c(1, DEVICE_ADD, 0), &[]);
        assert_eq!(named(&mut con, 1, MAC_PORT), [Act::Target(1), Act::Send(c(1, PORT_OPEN, 1))]);
        assert!(!con.host_open());
    }
}
