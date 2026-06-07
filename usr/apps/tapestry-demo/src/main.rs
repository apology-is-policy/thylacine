// tapestry-demo -- a software-rendered animation that weaves a Tapestry over a
// Loom ring. The native PROOF-OF-CONCEPT for Thylacine's graphics fast-path.
//
// It draws an animated XOR plasma (pure-integer, no FP/libm) into the back
// buffer and presents each frame, exercising the full client model: arm the
// multishot event stream, drain events, render, present + recycle-gate.
//
// COMPILE-ONLY (the auxiliary track never boots). Backed by `MockLoom`, which
// completes presents immediately + schedules a Close so the offline run
// terminates. On real hardware the SAME code, backed by `libthyla_rs::loom`
// (Loom-6) talking to `tapestryd`, displays the live plasma. See
// usr/apps/TAPESTRY-DESIGN.md + TEST-PLAN.md.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use tapestry::{Display, Event, MockLoom, Rect, Tapestry};

aux_rt::main!(run);

const WIDTH: u32 = 320;
const HEIGHT: u32 = 200;
const FRAMES: u32 = 120;

// Animated XOR plasma -- a classic integer texture. Writes 32bpp RGBA.
fn render(tap: &mut Tapestry, frame: u32) {
    let w = tap.width();
    let h = tap.height();
    let stride = tap.stride();
    let buf = tap.back_buffer_mut();
    for y in 0..h {
        let row = (y * stride) as usize;
        for x in 0..w {
            let v = ((x ^ y).wrapping_add(frame)) as u8;
            let off = row + (x * 4) as usize;
            buf[off] = v; // R
            buf[off + 1] = v.wrapping_mul(3); // G
            buf[off + 2] = v.wrapping_mul(7); // B
            buf[off + 3] = 0xff; // A
        }
    }
}

fn run(_args: aux_rt::Args) -> i64 {
    let loom = MockLoom::new(FRAMES);
    let mut dpy = match Display::new(loom) {
        Ok(d) => d,
        Err(e) => {
            aux_rt::eprintln!("tapestry-demo: open display: {}", e);
            return 1;
        }
    };
    let mut tap = match dpy.new_tapestry(WIDTH, HEIGHT) {
        Ok(t) => t,
        Err(e) => {
            aux_rt::eprintln!("tapestry-demo: new tapestry: {}", e);
            return 1;
        }
    };

    let mut frame: u32 = 0;
    let mut vsyncs: u32 = 0;
    // A running checksum over presented frames -- proves pixels actually flow
    // through the model (and keeps the renderer from being optimized away).
    let mut checksum: u64 = 0;

    'running: loop {
        while let Some(ev) = dpy.poll_event(&mut tap) {
            match ev {
                Event::Close => break 'running,
                Event::Vsync => vsyncs += 1,
                Event::Key { code, pressed } => {
                    let _ = (code, pressed); // a real app would act on input
                }
                Event::Pointer { .. } | Event::Resize { .. } => {}
            }
        }

        render(&mut tap, frame);
        checksum = checksum.wrapping_add(tap.back_buffer_mut()[0] as u64);

        if let Err(e) = dpy.present(&mut tap, Rect::full(WIDTH, HEIGHT)) {
            aux_rt::eprintln!("tapestry-demo: present: {}", e);
            return 1;
        }
        frame += 1;
    }

    aux_rt::println!(
        "tapestry-demo: wove {} frames ({} vsyncs), checksum {}",
        frame,
        vsyncs,
        checksum
    );
    0
}
