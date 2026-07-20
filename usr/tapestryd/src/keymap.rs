// The compositor-owned keymap (TAPESTRY.md section 18.4): every KEY tevent
// carries BOTH the raw evdev code (games read it) and the resolved UTF-32
// rune (shells read it) -- the anti-Wayland-xkb decision. No keymap hand-off
// protocol exists; layouts are a tapestryd config concern. Stage 0 bakes US
// QWERTY; the layout table is the single point a config file replaces later.
//
// Modifier tracking lives here too: the compositor folds mod-key press/
// release into a running bitmask stamped on every event.

pub const MOD_SHIFT: u16 = 1 << 0;
pub const MOD_CTRL: u16 = 1 << 1;
pub const MOD_ALT: u16 = 1 << 2;
pub const MOD_SUPER: u16 = 1 << 3;

// evdev key codes (linux/input-event-codes.h; QEMU's virtio-keyboard uses
// these verbatim).
const KEY_LEFTCTRL: u16 = 29;
const KEY_LEFTSHIFT: u16 = 42;
const KEY_RIGHTSHIFT: u16 = 54;
const KEY_LEFTALT: u16 = 56;
const KEY_RIGHTCTRL: u16 = 97;
const KEY_RIGHTALT: u16 = 100;
const KEY_LEFTMETA: u16 = 125;
const KEY_RIGHTMETA: u16 = 126;

/// The running modifier state.
#[derive(Default)]
pub struct Mods {
    shift_l: bool,
    shift_r: bool,
    ctrl_l: bool,
    ctrl_r: bool,
    alt_l: bool,
    alt_r: bool,
    super_l: bool,
    super_r: bool,
}

impl Mods {
    /// Fold a key transition into the state; returns true if the code was a
    /// modifier key (still delivered as a KEY event -- clients see mods too).
    pub fn update(&mut self, code: u16, pressed: bool) -> bool {
        match code {
            KEY_LEFTSHIFT => self.shift_l = pressed,
            KEY_RIGHTSHIFT => self.shift_r = pressed,
            KEY_LEFTCTRL => self.ctrl_l = pressed,
            KEY_RIGHTCTRL => self.ctrl_r = pressed,
            KEY_LEFTALT => self.alt_l = pressed,
            KEY_RIGHTALT => self.alt_r = pressed,
            KEY_LEFTMETA => self.super_l = pressed,
            KEY_RIGHTMETA => self.super_r = pressed,
            _ => return false,
        }
        true
    }

    pub fn mask(&self) -> u16 {
        let mut m = 0;
        if self.shift_l || self.shift_r {
            m |= MOD_SHIFT;
        }
        if self.ctrl_l || self.ctrl_r {
            m |= MOD_CTRL;
        }
        if self.alt_l || self.alt_r {
            m |= MOD_ALT;
        }
        if self.super_l || self.super_r {
            m |= MOD_SUPER;
        }
        m
    }
}

/// Resolve an evdev keycode + shift state to a UTF-32 rune (0 = none).
/// US QWERTY. Ctrl folds a letter to its control rune (the terminal
/// convention -- Kaua/Aurora consume it directly).
pub fn resolve(code: u16, mods: u16) -> u32 {
    let shift = mods & MOD_SHIFT != 0;
    let base: u32 = match code {
        // Row: `1234567890-=
        41 => sel(shift, b'`', b'~'),
        2 => sel(shift, b'1', b'!'),
        3 => sel(shift, b'2', b'@'),
        4 => sel(shift, b'3', b'#'),
        5 => sel(shift, b'4', b'$'),
        6 => sel(shift, b'5', b'%'),
        7 => sel(shift, b'6', b'^'),
        8 => sel(shift, b'7', b'&'),
        9 => sel(shift, b'8', b'*'),
        10 => sel(shift, b'9', b'('),
        11 => sel(shift, b'0', b')'),
        12 => sel(shift, b'-', b'_'),
        13 => sel(shift, b'=', b'+'),
        // qwertyuiop[]\
        16 => alpha(shift, b'q'),
        17 => alpha(shift, b'w'),
        18 => alpha(shift, b'e'),
        19 => alpha(shift, b'r'),
        20 => alpha(shift, b't'),
        21 => alpha(shift, b'y'),
        22 => alpha(shift, b'u'),
        23 => alpha(shift, b'i'),
        24 => alpha(shift, b'o'),
        25 => alpha(shift, b'p'),
        26 => sel(shift, b'[', b'{'),
        27 => sel(shift, b']', b'}'),
        43 => sel(shift, b'\\', b'|'),
        // asdfghjkl;'
        30 => alpha(shift, b'a'),
        31 => alpha(shift, b's'),
        32 => alpha(shift, b'd'),
        33 => alpha(shift, b'f'),
        34 => alpha(shift, b'g'),
        35 => alpha(shift, b'h'),
        36 => alpha(shift, b'j'),
        37 => alpha(shift, b'k'),
        38 => alpha(shift, b'l'),
        39 => sel(shift, b';', b':'),
        40 => sel(shift, b'\'', b'"'),
        // zxcvbnm,./
        44 => alpha(shift, b'z'),
        45 => alpha(shift, b'x'),
        46 => alpha(shift, b'c'),
        47 => alpha(shift, b'v'),
        48 => alpha(shift, b'b'),
        49 => alpha(shift, b'n'),
        50 => alpha(shift, b'm'),
        51 => sel(shift, b',', b'<'),
        52 => sel(shift, b'.', b'>'),
        53 => sel(shift, b'/', b'?'),
        // Whitespace + control runes.
        57 => b' ' as u32,
        28 => b'\r' as u32,  // Enter
        15 => b'\t' as u32,  // Tab
        14 => 0x08,          // Backspace
        1 => 0x1B,           // Esc
        _ => 0,
    };
    if base == 0 {
        return 0;
    }
    // Ctrl folds letters (and a few punctuation runes) to control codes.
    if mods & MOD_CTRL != 0 {
        let lower = if (b'A' as u32..=b'Z' as u32).contains(&base) {
            base + 0x20
        } else {
            base
        };
        if (b'a' as u32..=b'z' as u32).contains(&lower) {
            return lower - (b'a' as u32) + 1;
        }
        return base;
    }
    base
}

#[inline]
fn sel(shift: bool, plain: u8, shifted: u8) -> u32 {
    (if shift { shifted } else { plain }) as u32
}

#[inline]
fn alpha(shift: bool, lower: u8) -> u32 {
    (if shift { lower - 0x20 } else { lower }) as u32
}
