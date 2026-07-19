// /bin/tapestry-demo -- the first tapestryd client (Tapestry G-3; the
// section 18.9 gate: "tapestry-demo plasma LIVE via screendump"). Draws the
// P4-L 4-quadrant pattern (the tools/screendump.sh -v contract: quadrant
// centers exact) as a static background, with an animated XOR plasma block
// in the display center -- geometrically clear of the four -v sample points
// (the block spans (3w/8, 3h/8)..(5w/8, 5h/8); the samples sit at (w/4, h/4)
// etc.), so -v is animation-phase-independent while any two dumps a frame
// apart differ inside the block: the liveness witness.
//
// This binary IS the end-to-end proof of the whole G-2/G-3 path: a private
// /srv/tapestry session -> surface mint -> create -> the weave map
// (SYS_WEFT_MAP -> the kernel's Tweft -> tapestryd's share -> the
// cross-Proc zero-copy mapping -- the G-2-audit F2 round-trip E2E residue,
// discharged here) -> Loom presents (the CQE recycle gate) -> the FRAME
// event clock pacing the animation. joey spawns it post-pivot and leaves it
// running (the presented surface must outlive the probe ladder for the
// per-boot pattern gate).

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::time::{sleep, Duration};
use tapestry::{Event, Rect, Surface, TEV_CLOSE, TEV_FRAME, TEV_KEY};

macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

const COLOR_RED: u32 = 0xFFFF_0000;
const COLOR_GREEN: u32 = 0xFF00_FF00;
const COLOR_BLUE: u32 = 0xFF00_00FF;
const COLOR_WHITE: u32 = 0xFFFF_FFFF;

const CONNECT_TRIES: u32 = 25;
const CONNECT_DELAY_MS: u64 = 200;

fn draw_quadrants(px: &mut [u32], w: u32, h: u32) {
    let (hw, hh) = (w / 2, h / 2);
    for y in 0..h {
        for x in 0..w {
            let c = match (x < hw, y < hh) {
                (true, true) => COLOR_RED,
                (false, true) => COLOR_GREEN,
                (true, false) => COLOR_BLUE,
                (false, false) => COLOR_WHITE,
            };
            px[(y * w + x) as usize] = c;
        }
    }
}

/// The animated XOR plasma (pure-integer, the aux-POC renderer) into the
/// center block only.
fn draw_plasma(px: &mut [u32], w: u32, bx: u32, by: u32, bw: u32, bh: u32, phase: u32) {
    for y in 0..bh {
        for x in 0..bw {
            let v = ((x ^ y).wrapping_add(phase)) as u8;
            let r = v as u32;
            let g = v.wrapping_mul(3) as u32;
            let b = v.wrapping_mul(7) as u32;
            px[((by + y) * w + (bx + x)) as usize] = 0xFF00_0000 | (r << 16) | (g << 8) | b;
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // Bounded connect retry: tapestryd is warden-spawned long before this
    // post-pivot demo, but a slow bring-up must not flake the boot.
    let mut surf: Option<Surface> = None;
    for i in 0..CONNECT_TRIES {
        match Surface::fullscreen() {
            Ok(s) => {
                surf = Some(s);
                break;
            }
            Err(e) => {
                if i == CONNECT_TRIES - 1 {
                    say!("tapestry-demo: FAIL connect/create {:?}", e);
                    return 1;
                }
                let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
            }
        }
    }
    let mut surf = surf.unwrap();
    let (w, h) = (surf.w, surf.h);

    // The plasma block: display-center, clear of the -v quadrant sample
    // points by construction (samples at (w/4,h/4),(3w/4,h/4),(w/4,3h/4),
    // (3w/4,3h/4); the block spans (3w/8..5w/8, 3h/8..5h/8)).
    let (bx, by, bw, bh) = (3 * w / 8, 3 * h / 8, w / 4, h / 4);
    if bx <= w / 4 || bx + bw >= 3 * w / 4 || by <= h / 4 || by + bh >= 3 * h / 4 {
        say!("tapestry-demo: FAIL degenerate geometry {}x{}", w, h);
        return 1;
    }

    // Frame 0: the full quadrant background + the first plasma phase.
    {
        let px = surf.pixels();
        draw_quadrants(px, w, h);
        draw_plasma(px, w, bx, by, bw, bh, 0);
    }
    if let Err(e) = surf.present(None) {
        say!("tapestry-demo: FAIL first present {:?}", e);
        return 1;
    }
    say!("tapestry-demo: surface {} up {}x{} (plasma {}x{}@{},{})", surf.id, w, h, bw, bh, bx, by);

    // The animation loop: FRAME-paced partial presents of the plasma block.
    // Each present rotates slots, so every slot's plasma region is redrawn
    // on its turn (the quadrant background lives in the resource from frame
    // 0 -- partial rects never touch it).
    let mut phase: u32 = 0;
    let mut frames: u64 = 0;
    loop {
        let ev: Event = match surf.wait_event() {
            Ok(ev) => ev,
            Err(e) => {
                say!("tapestry-demo: event stream ended {:?}", e);
                return 1;
            }
        };
        match ev.kind {
            TEV_FRAME => {
                phase = phase.wrapping_add(3);
                {
                    let px = surf.pixels();
                    draw_plasma(px, w, bx, by, bw, bh, phase);
                }
                if let Err(e) = surf.present(Some(Rect {
                    x: bx,
                    y: by,
                    w: bw,
                    h: bh,
                })) {
                    say!("tapestry-demo: FAIL present {:?}", e);
                    return 1;
                }
                frames += 1;
                if frames == 60 {
                    say!("tapestry-demo: 60 frames woven");
                }
            }
            TEV_KEY => {
                say!(
                    "tapestry-demo: key code={} value={} rune={:#x} mods={:#x}",
                    ev.code,
                    ev.value,
                    ev.rune,
                    ev.mods
                );
            }
            TEV_CLOSE => {
                say!("tapestry-demo: CLOSE received; exiting");
                return 0;
            }
            _ => {}
        }
    }
}
