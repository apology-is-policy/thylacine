// macOS virtual key codes (kVK_*, layout-independent positions) to QEMU "qnum": the XT set-1 make code,
// with 0x80 set for a key whose scancode carries the e0 prefix. org.qemu.Display1.Keyboard takes qnum.

pub const CAPS_LOCK: u16 = 0x39;

#[link(name = "Carbon", kind = "framework")]
extern "C" {
    fn LMGetKbdType() -> u8;
    fn KBGetLayoutType(keyboard_type: i16) -> u32;
}

// Whether the keyboard that was last typed on is an ISO one (the tall Enter, a key beside left Shift).
pub fn iso_keyboard() -> bool {
    const ISO: u32 = u32::from_be_bytes(*b"ISO ");
    // SAFETY: both read a value the system keeps; neither takes a pointer.
    unsafe { KBGetLayoutType(LMGetKbdType() as i16) == ISO }
}

// The code for where a key is. On an ISO keyboard macOS reports the key left of 1 and the key beside
// left Shift with each other's codes; everywhere else a code is already a place.
pub fn position(vk: u16, iso: bool) -> u16 {
    match (vk, iso) {
        (0x0a, true) => 0x32,
        (0x32, true) => 0x0a,
        _ => vk,
    }
}

pub fn qnum(vk: u16) -> Option<u32> {
    Some(match vk {
        0x00 => 0x1e,
        0x01 => 0x1f,
        0x02 => 0x20,
        0x03 => 0x21,
        0x04 => 0x23,
        0x05 => 0x22, // A S D F H G
        0x06 => 0x2c,
        0x07 => 0x2d,
        0x08 => 0x2e,
        0x09 => 0x2f,
        0x0a => 0x56,
        0x0b => 0x30, // Z X C V ISO B
        0x0c => 0x10,
        0x0d => 0x11,
        0x0e => 0x12,
        0x0f => 0x13,
        0x10 => 0x15,
        0x11 => 0x14, // Q W E R Y T
        0x12 => 0x02,
        0x13 => 0x03,
        0x14 => 0x04,
        0x15 => 0x05,
        0x16 => 0x07,
        0x17 => 0x06, // 1 2 3 4 6 5
        0x18 => 0x0d,
        0x19 => 0x0a,
        0x1a => 0x08,
        0x1b => 0x0c,
        0x1c => 0x09,
        0x1d => 0x0b, // = 9 7 - 8 0
        0x1e => 0x1b,
        0x1f => 0x18,
        0x20 => 0x16,
        0x21 => 0x1a,
        0x22 => 0x17,
        0x23 => 0x19, // ] O U [ I P
        0x24 => 0x1c,
        0x25 => 0x26,
        0x26 => 0x24,
        0x27 => 0x28,
        0x28 => 0x25,
        0x29 => 0x27, // Return L J ' K ;
        0x2a => 0x2b,
        0x2b => 0x33,
        0x2c => 0x35,
        0x2d => 0x31,
        0x2e => 0x32,
        0x2f => 0x34, // \ , / N M .
        0x30 => 0x0f,
        0x31 => 0x39,
        0x32 => 0x29,
        0x33 => 0x0e,
        0x35 => 0x01, // Tab Space ` Backspace Esc
        0x36 => 0xdc,
        0x37 => 0xdb, // right, left Command
        0x38 => 0x2a,
        0x39 => 0x3a,
        0x3a => 0x38,
        0x3b => 0x1d, // Shift Caps Option Control
        0x3c => 0x36,
        0x3d => 0xb8,
        0x3e => 0x9d, // right Shift, Option, Control
        0x41 => 0x53,
        0x43 => 0x37,
        0x45 => 0x4e,
        0x47 => 0x45,
        0x4b => 0xb5,
        0x4c => 0x9c, // KP . * + Clear / Enter
        0x4e => 0x4a, // KP -
        0x52 => 0x52,
        0x53 => 0x4f,
        0x54 => 0x50,
        0x55 => 0x51,
        0x56 => 0x4b,
        0x57 => 0x4c, // KP 0 1 2 3 4 5
        0x58 => 0x4d,
        0x59 => 0x47,
        0x5b => 0x48,
        0x5c => 0x49, // KP 6 7 8 9
        0x7a => 0x3b,
        0x78 => 0x3c,
        0x63 => 0x3d,
        0x76 => 0x3e,
        0x60 => 0x3f,
        0x61 => 0x40, // F1..F6
        0x62 => 0x41,
        0x64 => 0x42,
        0x65 => 0x43,
        0x6d => 0x44,
        0x67 => 0x57,
        0x6f => 0x58, // F7..F12
        // The key a PC keyboard labels Insert. QEMU's table calls it Help (0xf5), which no guest binds.
        0x72 => 0xd2,
        0x73 => 0xc7,
        0x74 => 0xc9,
        0x75 => 0xd3,
        0x77 => 0xcf,
        0x79 => 0xd1, // Insert Home PgUp Del End PgDn
        0x7b => 0xcb,
        0x7c => 0xcd,
        0x7d => 0xd0,
        0x7e => 0xc8, // Left Right Down Up
        0x69 => 0x5d,
        0x6b => 0x5e,
        0x71 => 0x5f,
        0x6a => 0x55,
        0x40 => 0x83,
        0x4f => 0xf7,
        0x50 => 0x84,
        0x5a => 0x5a, // F13..F20
        0x51 => 0x59,
        0x6e => 0xdd, // keypad =, Menu
        0x5d => 0x7d,
        0x5e => 0x73,
        0x5f => 0x5c,
        0x66 => 0x71,
        0x68 => 0x72, // JIS: Yen, Underscore, keypad comma, Eisu, Kana
        0x48 => 0xb0,
        0x49 => 0xae,
        0x4a => 0xa0, // Volume up, down, mute: only a keyboard that sends them as keys
        _ => return None,
    })
}

// Is this virtual key a modifier, and if so which NSEventModifierFlags bit reports it?
pub fn modifier_flag(vk: u16) -> Option<usize> {
    Some(match vk {
        0x38 | 0x3c => 1 << 17, // Shift
        0x3b | 0x3e => 1 << 18, // Control
        0x3a | 0x3d => 1 << 19, // Option
        0x36 | 0x37 => 1 << 20, // Command
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn an_iso_keyboard_has_two_keys_the_other_way_round() {
        for vk in 0u16..0x80 {
            assert_eq!(position(vk, false), vk);
            if vk != 0x0a && vk != 0x32 {
                assert_eq!(position(vk, true), vk);
            }
        }
        // The key left of 1 is the grave key's place, the key beside left Shift is the 102nd key's.
        assert_eq!(qnum(position(0x0a, true)), Some(0x29));
        assert_eq!(qnum(position(0x32, true)), Some(0x56));
        assert_eq!(qnum(position(0x32, false)), Some(0x29));
    }
    #[test]
    fn no_two_keys_share_a_code() {
        let mut seen = std::collections::HashMap::new();
        for vk in 0u16..0x80 {
            if let Some(q) = qnum(vk) {
                if let Some(other) = seen.insert(q, vk) {
                    panic!("qnum {q:#x} from vk {other:#x} and {vk:#x}");
                }
            }
        }
        assert_eq!(seen.len(), 119);
    }
    #[test]
    fn the_keys_with_a_prefix_carry_the_high_bit() {
        // Up, left Command, Insert, Menu, right Control, keypad Enter: e0-prefixed in set 1.
        for (vk, q) in [
            (0x7e, 0xc8),
            (0x37, 0xdb),
            (0x72, 0xd2),
            (0x6e, 0xdd),
            (0x3e, 0x9d),
            (0x4c, 0x9c),
        ] {
            assert_eq!(qnum(vk), Some(q), "vk {vk:#x}");
        }
        // And two that are not: F13, keypad =.
        assert_eq!(qnum(0x69), Some(0x5d));
        assert_eq!(qnum(0x51), Some(0x59));
    }
    #[test]
    fn the_letters_land_on_their_rows() {
        // QWERTY's top row is 0x10.., the home row 0x1e.., the bottom row 0x2c..
        for (i, vk) in [0x0c, 0x0d, 0x0e, 0x0f, 0x11, 0x10, 0x20, 0x22, 0x1f, 0x23]
            .iter()
            .enumerate()
        {
            assert_eq!(qnum(*vk), Some(0x10 + i as u32));
        }
        for (i, vk) in [0x00, 0x01, 0x02, 0x03, 0x05, 0x04, 0x26, 0x28, 0x25]
            .iter()
            .enumerate()
        {
            assert_eq!(qnum(*vk), Some(0x1e + i as u32));
        }
        for (i, vk) in [0x06, 0x07, 0x08, 0x09, 0x0b, 0x2d, 0x2e]
            .iter()
            .enumerate()
        {
            assert_eq!(qnum(*vk), Some(0x2c + i as u32));
        }
        for (i, vk) in [0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1a, 0x1c, 0x19, 0x1d]
            .iter()
            .enumerate()
        {
            assert_eq!(qnum(*vk), Some(0x02 + i as u32));
        }
    }
}
