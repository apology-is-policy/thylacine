// The guest's screen as a view: a CAMetalLayer that shows the scanout, and the keys and pointer of
// whoever has the window. All of it runs on the main thread; the display thread only marks damage.
use std::cell::{Cell, OnceCell, RefCell};
use std::collections::HashSet;
use std::ptr::NonNull;
use std::sync::atomic::Ordering;
use std::time::Instant;

use objc2::rc::Retained;
use objc2::runtime::{AnyObject, ProtocolObject};
use objc2::{define_class, msg_send, sel, DefinedClass, MainThreadOnly};
use objc2_app_kit::{NSEvent, NSEventModifierFlags, NSEventType, NSResponder, NSView};
use objc2_core_foundation::CGSize;
use objc2_core_graphics::{kCGColorSpaceSRGB, CGColorSpace};
use objc2_foundation::{
    ns_string, MainThreadMarker, NSObject, NSObjectProtocol, NSPoint, NSRect, NSRunLoop,
    NSRunLoopCommonModes, NSTimer,
};
use objc2_metal::{
    MTLClearColor, MTLCommandBuffer, MTLCommandEncoder, MTLCommandQueue,
    MTLCreateSystemDefaultDevice, MTLDevice, MTLLibrary, MTLLoadAction, MTLOrigin, MTLPixelFormat,
    MTLPrimitiveType, MTLRegion, MTLRenderCommandEncoder, MTLRenderPassDescriptor,
    MTLRenderPipelineDescriptor, MTLRenderPipelineState, MTLSamplerDescriptor,
    MTLSamplerMinMagFilter, MTLSamplerState, MTLSize, MTLStoreAction, MTLTexture,
    MTLTextureDescriptor, MTLTextureUsage, MTLViewport,
};
use objc2_quartz_core::{CADisplayLink, CALayer, CAMetalDrawable, CAMetalLayer};

use crate::display::{Display, Input, Pixels, Rect};
use crate::keymap;

const SHADERS: &str = r#"
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; float2 uv; };
vertex V vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    V o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(t.sample(s, in.uv).rgb, 1.0);
}
"#;

struct Renderer {
    device: Retained<ProtocolObject<dyn MTLDevice>>,
    queue: Retained<ProtocolObject<dyn MTLCommandQueue>>,
    pipeline: Retained<ProtocolObject<dyn MTLRenderPipelineState>>,
    nearest: Retained<ProtocolObject<dyn MTLSamplerState>>,
    linear: Retained<ProtocolObject<dyn MTLSamplerState>>,
    layer: Retained<CAMetalLayer>,
    texture: Option<Retained<ProtocolObject<dyn MTLTexture>>>,
    serial: u64,
    tex_size: (u32, u32),
    drawable_size: (u32, u32),
    mapped: bool, // whether the last upload read QEMU's memory or a copy it sent
}

#[derive(Default)]
pub struct Stats {
    pub frames: u64,
    pub uploads: u64,
    pub upload_px: u64,
    pub upload_us: u64,
    pub upload_us_max: u64,
    pub frame_us: u64,
    pub frame_us_max: u64,
}
impl Stats {
    fn add(&mut self, o: &Stats) {
        self.frames += o.frames;
        self.uploads += o.uploads;
        self.upload_px += o.upload_px;
        self.upload_us += o.upload_us;
        self.upload_us_max = self.upload_us_max.max(o.upload_us_max);
        self.frame_us += o.frame_us;
        self.frame_us_max = self.frame_us_max.max(o.frame_us_max);
    }
    fn line(&self) -> String {
        format!(
            "{} frames, {} uploads, {} px, upload avg {} us max {} us, frame avg {} us max {} us",
            self.frames,
            self.uploads,
            self.upload_px,
            self.upload_us / self.uploads.max(1),
            self.upload_us_max,
            self.frame_us / self.frames.max(1),
            self.frame_us_max
        )
    }
}

// A trackpad reports a scroll in points and a wheel in notches; the guest's wheel only has notches.
const POINTS_PER_NOTCH: f64 = 10.0;
const NOTCHES_PER_EVENT: f64 = 8.0;

// The whole notches a scroll of `dy` completes, carrying what is left in `acc`. Positive is up.
fn notches(acc: &mut f64, dy: f64, precise: bool) -> i32 {
    if !precise {
        return if dy > 0.0 {
            1
        } else if dy < 0.0 {
            -1
        } else {
            0
        };
    }
    if *acc * dy < 0.0 {
        *acc = 0.0; // the fingers turned round: what was owed the other way is dropped
    }
    *acc += dy;
    let n = (*acc / POINTS_PER_NOTCH)
        .trunc()
        .clamp(-NOTCHES_PER_EVENT, NOTCHES_PER_EVENT);
    *acc -= n * POINTS_PER_NOTCH;
    if acc.abs() >= POINTS_PER_NOTCH {
        *acc = 0.0; // a fling past the bound is not replayed afterwards
    }
    n as i32
}

impl Renderer {
    fn new() -> Result<Renderer, String> {
        let device = MTLCreateSystemDefaultDevice().ok_or("no Metal device")?;
        let queue = device.newCommandQueue().ok_or("no command queue")?;
        let lib = device
            .newLibraryWithSource_options_error(ns_string!(SHADERS), None)
            .map_err(|e| e.to_string())?;
        let vs = lib
            .newFunctionWithName(ns_string!("vs"))
            .ok_or("no vertex function")?;
        let fs = lib
            .newFunctionWithName(ns_string!("fs"))
            .ok_or("no fragment function")?;
        let desc = MTLRenderPipelineDescriptor::new();
        desc.setVertexFunction(Some(&vs));
        desc.setFragmentFunction(Some(&fs));
        unsafe { desc.colorAttachments().objectAtIndexedSubscript(0) }
            .setPixelFormat(MTLPixelFormat::BGRA8Unorm);
        let pipeline = device
            .newRenderPipelineStateWithDescriptor_error(&desc)
            .map_err(|e| e.to_string())?;
        let sampler = |f: MTLSamplerMinMagFilter| {
            let d = MTLSamplerDescriptor::new();
            d.setMinFilter(f);
            d.setMagFilter(f);
            device.newSamplerStateWithDescriptor(&d).ok_or("no sampler")
        };
        let nearest = sampler(MTLSamplerMinMagFilter::Nearest)?;
        let linear = sampler(MTLSamplerMinMagFilter::Linear)?;
        let layer = CAMetalLayer::new();
        layer.setDevice(Some(&device));
        layer.setPixelFormat(MTLPixelFormat::BGRA8Unorm);
        layer.setFramebufferOnly(true);
        // The guest's pixels are sRGB; without a colour space the panel shows them in its own gamut.
        let srgb = CGColorSpace::with_name(Some(unsafe { kCGColorSpaceSRGB }));
        layer.setColorspace(srgb.as_deref());
        Ok(Renderer {
            device,
            queue,
            pipeline,
            nearest,
            linear,
            layer,
            texture: None,
            serial: 0,
            tex_size: (0, 0),
            drawable_size: (0, 0),
            mapped: false,
        })
    }

    // Bring the texture up to the scanout. Returns whether anything changed on screen.
    fn upload(&mut self, display: &Display, stats: &mut Stats) -> bool {
        let t0 = Instant::now();
        let mut s = display.shared.lock().unwrap();
        let damage = s.damage.take();
        let Some(sc) = &s.scanout else {
            let had = self.texture.take().is_some();
            self.serial = 0;
            return had;
        };
        let fresh = sc.serial != self.serial || self.texture.is_none();
        if fresh {
            if self.tex_size != (sc.w, sc.h) || self.texture.is_none() {
                let d = unsafe {
                    MTLTextureDescriptor::texture2DDescriptorWithPixelFormat_width_height_mipmapped(
                        MTLPixelFormat::BGRA8Unorm,
                        sc.w as usize,
                        sc.h as usize,
                        false,
                    )
                };
                self.texture = self.device.newTextureWithDescriptor(&d);
                self.tex_size = (sc.w, sc.h);
            }
            self.serial = sc.serial;
        }
        let Some(tex) = &self.texture else {
            return false;
        };
        let r = if fresh {
            Rect {
                x: 0,
                y: 0,
                w: sc.w,
                h: sc.h,
            }
        } else {
            match damage {
                Some(r) => r,
                None => return false,
            }
        };
        let (w, h, stride) = (sc.w, sc.h, sc.stride as usize);
        self.mapped = matches!(sc.pixels, Pixels::Mapped(_));
        let (ptr, len, keep) = match &sc.pixels {
            Pixels::Mapped(region) => (region.as_ptr(), region.len(), Some(region.clone())),
            Pixels::Owned(buf) => (buf.as_ptr(), buf.len(), None),
        };
        let copy = |ptr: *const u8| {
            if r.x + r.w > w || r.y + r.h > h {
                return;
            }
            let first = r.y as usize * stride + r.x as usize * 4;
            let last = (r.y + r.h - 1) as usize * stride + (r.x + r.w) as usize * 4;
            if last > len {
                return;
            }
            let region = MTLRegion {
                origin: MTLOrigin {
                    x: r.x as usize,
                    y: r.y as usize,
                    z: 0,
                },
                size: MTLSize {
                    width: r.w as usize,
                    height: r.h as usize,
                    depth: 1,
                },
            };
            unsafe {
                tex.replaceRegion_mipmapLevel_withBytes_bytesPerRow(
                    region,
                    0,
                    NonNull::new_unchecked(ptr.add(first) as *mut std::ffi::c_void),
                    stride,
                );
            }
        };
        match keep {
            // A mapping outlives the lock through its own reference, so the display thread (and with it
            // QEMU's main loop) is not held for the length of the copy.
            Some(region) => {
                drop(s);
                copy(region.as_ptr());
            }
            None => {
                copy(ptr);
                drop(s);
            }
        }
        let us = t0.elapsed().as_micros() as u64;
        stats.uploads += 1;
        stats.upload_px += r.w as u64 * r.h as u64;
        stats.upload_us += us;
        stats.upload_us_max = stats.upload_us_max.max(us);
        true
    }

    // One pass: clear to black, then the scanout letterboxed into a target of `size` pixels.
    fn draw(
        &self,
        cmd: &ProtocolObject<dyn MTLCommandBuffer>,
        target: &ProtocolObject<dyn MTLTexture>,
        size: (u32, u32),
    ) {
        let pass = MTLRenderPassDescriptor::renderPassDescriptor();
        let att = unsafe { pass.colorAttachments().objectAtIndexedSubscript(0) };
        att.setTexture(Some(target));
        att.setLoadAction(MTLLoadAction::Clear);
        att.setStoreAction(MTLStoreAction::Store);
        att.setClearColor(MTLClearColor {
            red: 0.0,
            green: 0.0,
            blue: 0.0,
            alpha: 1.0,
        });
        let Some(enc) = cmd.renderCommandEncoderWithDescriptor(&pass) else {
            return;
        };
        if let Some(tex) = &self.texture {
            let (dw, dh) = (size.0 as f64, size.1 as f64);
            let (tw, th) = (self.tex_size.0 as f64, self.tex_size.1 as f64);
            if dw > 0.0 && dh > 0.0 && tw > 0.0 && th > 0.0 {
                let scale = (dw / tw).min(dh / th);
                let (vw, vh) = ((tw * scale).round(), (th * scale).round());
                enc.setViewport(MTLViewport {
                    originX: ((dw - vw) / 2.0).floor(),
                    originY: ((dh - vh) / 2.0).floor(),
                    width: vw,
                    height: vh,
                    znear: 0.0,
                    zfar: 1.0,
                });
                // A whole-number scale keeps the guest's pixels square and sharp; anything else is filtered.
                let whole = (scale - scale.round()).abs() < 1e-6 && scale >= 1.0;
                enc.setRenderPipelineState(&self.pipeline);
                unsafe {
                    enc.setFragmentTexture_atIndex(Some(tex), 0);
                    enc.setFragmentSamplerState_atIndex(
                        Some(if whole { &self.nearest } else { &self.linear }),
                        0,
                    );
                    enc.drawPrimitives_vertexStart_vertexCount(MTLPrimitiveType::Triangle, 0, 3);
                }
            }
        }
        enc.endEncoding();
    }

    // False if the layer had no drawable to give: the frame is still owed.
    fn present(&mut self) -> bool {
        let Some(drawable) = self.layer.nextDrawable() else {
            return false;
        };
        let Some(cmd) = self.queue.commandBuffer() else {
            return false;
        };
        self.draw(&cmd, &drawable.texture(), self.drawable_size);
        cmd.presentDrawable(ProtocolObject::from_ref(&*drawable));
        cmd.commit();
        true
    }

    // What the pipeline makes of the scanout at 1:1, read back as RGB rows: the picture the window shows,
    // minus the scaling. A test compares it with QEMU's own dump of the same frame.
    fn snapshot(&self) -> Option<(u32, u32, Vec<u8>)> {
        let (w, h) = self.tex_size;
        self.texture.as_ref()?;
        let d = unsafe {
            MTLTextureDescriptor::texture2DDescriptorWithPixelFormat_width_height_mipmapped(
                MTLPixelFormat::BGRA8Unorm,
                w as usize,
                h as usize,
                false,
            )
        };
        d.setUsage(MTLTextureUsage::RenderTarget | MTLTextureUsage::ShaderRead);
        let target = self.device.newTextureWithDescriptor(&d)?;
        let cmd = self.queue.commandBuffer()?;
        self.draw(&cmd, &target, (w, h));
        cmd.commit();
        cmd.waitUntilCompleted();
        let mut bgra = vec![0u8; w as usize * h as usize * 4];
        let region = MTLRegion {
            origin: MTLOrigin { x: 0, y: 0, z: 0 },
            size: MTLSize {
                width: w as usize,
                height: h as usize,
                depth: 1,
            },
        };
        unsafe {
            target.getBytes_bytesPerRow_fromRegion_mipmapLevel(
                NonNull::new_unchecked(bgra.as_mut_ptr() as *mut std::ffi::c_void),
                w as usize * 4,
                region,
                0,
            );
        }
        let mut rgb = Vec::with_capacity(w as usize * h as usize * 3);
        for px in bgra.chunks_exact(4) {
            rgb.extend_from_slice(&[px[2], px[1], px[0]]);
        }
        Some((w, h, rgb))
    }
}

pub struct Ivars {
    display: Display,
    renderer: RefCell<Option<Renderer>>,
    link: OnceCell<Retained<CADisplayLink>>,
    held: RefCell<HashSet<u16>>,
    stats: RefCell<Stats>,
    total: RefCell<Stats>,
    stats_since: Cell<Option<Instant>>,
    opts: Options,
    presented: Cell<u64>,
    owed: Cell<bool>, // the texture changed and no frame has shown it yet
    ticks: Cell<u64>,
    scroll: Cell<f64>,
}

// What a test asks of the window; a user's run leaves all of it off.
#[derive(Clone, Default)]
pub struct Options {
    pub print_stats: bool,
    pub exit_after: Option<f64>, // seconds
    pub dump: Option<String>,    // a PPM of the pipeline's output, written at exit_after
    pub scripted_input: bool, // one second in, a fixed run of key and pointer events through the window
}

define_class!(
    // SAFETY: NSView allows subclassing; GuestView has no Drop.
    #[unsafe(super(NSView, NSResponder, NSObject))]
    #[thread_kind = MainThreadOnly]
    #[name = "ThylacineGuestView"]
    #[ivars = Ivars]
    pub struct GuestView;

    unsafe impl NSObjectProtocol for GuestView {}

    impl GuestView {
        #[unsafe(method(acceptsFirstResponder))]
        fn accepts_first_responder(&self) -> bool { true }

        #[unsafe(method(wantsUpdateLayer))]
        fn wants_update_layer(&self) -> bool { true }

        #[unsafe(method_id(makeBackingLayer))]
        fn make_backing_layer(&self) -> Retained<CALayer> {
            match self.ivars().renderer.borrow().as_ref() {
                Some(r) => Retained::into_super(r.layer.clone()),
                None => CALayer::new(),
            }
        }

        #[unsafe(method(tick:))]
        fn tick(&self, _link: &CADisplayLink) {
            self.ivars().ticks.set(self.ivars().ticks.get() + 1);
            self.frame();
        }

        // A display link is silent while the display sleeps, so a test's deadline has a timer of its own.
        #[unsafe(method(deadline:))]
        fn deadline(&self, _timer: &NSTimer) { self.finish(); }

        #[unsafe(method(scripted:))]
        fn scripted(&self, _timer: &NSTimer) { self.scripted_input(); }

        #[unsafe(method(keyDown:))]
        fn key_down(&self, ev: &NSEvent) { self.key(ev.keyCode(), true); }

        #[unsafe(method(keyUp:))]
        fn key_up(&self, ev: &NSEvent) { self.key(ev.keyCode(), false); }

        #[unsafe(method(flagsChanged:))]
        fn flags_changed(&self, ev: &NSEvent) {
            let vk = ev.keyCode();
            if vk == keymap::CAPS_LOCK {
                // macOS reports the lock's state, not the key: one event per toggle.
                self.key(vk, true);
                self.key(vk, false);
                return;
            }
            let Some(bit) = keymap::modifier_flag(vk) else { return };
            let flag_set = ev.modifierFlags().0 & bit != 0;
            let was_held = self.ivars().held.borrow().contains(&vk);
            // The flag stays set while the key's twin on the other side is still down.
            let down = flag_set && !was_held;
            if down || was_held { self.key(vk, down); }
        }

        #[unsafe(method(mouseMoved:))]
        fn mouse_moved(&self, ev: &NSEvent) { self.pointer(ev); }
        #[unsafe(method(mouseDragged:))]
        fn mouse_dragged(&self, ev: &NSEvent) { self.pointer(ev); }
        #[unsafe(method(rightMouseDragged:))]
        fn right_mouse_dragged(&self, ev: &NSEvent) { self.pointer(ev); }
        #[unsafe(method(otherMouseDragged:))]
        fn other_mouse_dragged(&self, ev: &NSEvent) { self.pointer(ev); }

        #[unsafe(method(mouseDown:))]
        fn mouse_down(&self, ev: &NSEvent) { self.button(ev, 0, true); }
        #[unsafe(method(mouseUp:))]
        fn mouse_up(&self, ev: &NSEvent) { self.button(ev, 0, false); }
        #[unsafe(method(rightMouseDown:))]
        fn right_mouse_down(&self, ev: &NSEvent) { self.button(ev, 2, true); }
        #[unsafe(method(rightMouseUp:))]
        fn right_mouse_up(&self, ev: &NSEvent) { self.button(ev, 2, false); }
        #[unsafe(method(otherMouseDown:))]
        fn other_mouse_down(&self, ev: &NSEvent) { self.button(ev, 1, true); }
        #[unsafe(method(otherMouseUp:))]
        fn other_mouse_up(&self, ev: &NSEvent) { self.button(ev, 1, false); }

        #[unsafe(method(scrollWheel:))]
        fn scroll_wheel(&self, ev: &NSEvent) {
            let mut acc = self.ivars().scroll.get();
            let n = notches(&mut acc, ev.scrollingDeltaY(), ev.hasPreciseScrollingDeltas());
            self.ivars().scroll.set(acc);
            for _ in 0..n.abs() { self.send(Input::Wheel { up: n > 0 }); }
        }
    }
);

impl GuestView {
    pub fn new(
        mtm: MainThreadMarker,
        frame: NSRect,
        display: Display,
        opts: Options,
    ) -> Result<Retained<Self>, String> {
        let renderer = Renderer::new()?;
        let this = Self::alloc(mtm).set_ivars(Ivars {
            display,
            renderer: RefCell::new(Some(renderer)),
            link: OnceCell::new(),
            held: RefCell::new(HashSet::new()),
            stats: RefCell::new(Stats::default()),
            total: RefCell::new(Stats::default()),
            stats_since: Cell::new(None),
            opts,
            presented: Cell::new(0),
            owed: Cell::new(false),
            ticks: Cell::new(0),
            scroll: Cell::new(0.0),
        });
        let this: Retained<Self> = unsafe { msg_send![super(this), initWithFrame: frame] };
        this.setWantsLayer(true);
        // The link follows the display the view is on, and its rate.
        let link = unsafe {
            this.displayLinkWithTarget_selector(
                &*(Retained::as_ptr(&this) as *const AnyObject),
                sel!(tick:),
            )
        };
        unsafe { link.addToRunLoop_forMode(&NSRunLoop::mainRunLoop(), NSRunLoopCommonModes) };
        this.ivars().link.set(link).ok();
        if let Some(secs) = this.ivars().opts.exit_after {
            unsafe {
                NSTimer::scheduledTimerWithTimeInterval_target_selector_userInfo_repeats(
                    secs,
                    &*(Retained::as_ptr(&this) as *const AnyObject),
                    sel!(deadline:),
                    None,
                    false,
                );
            }
        }
        if this.ivars().opts.scripted_input {
            unsafe {
                NSTimer::scheduledTimerWithTimeInterval_target_selector_userInfo_repeats(
                    1.0,
                    &*(Retained::as_ptr(&this) as *const AnyObject),
                    sel!(scripted:),
                    None,
                    false,
                );
            }
        }
        Ok(this)
    }

    // Release everything the guest believes is held: the window stopped being where keys go.
    pub fn release_keys(&self) {
        let held: Vec<u16> = self.ivars().held.borrow_mut().drain().collect();
        for vk in held {
            if let Some(q) = keymap::qnum(vk) {
                self.send(Input::Key {
                    qnum: q,
                    down: false,
                });
            }
        }
    }

    fn send(&self, ev: Input) {
        self.ivars().display.input.send(ev).ok();
    }

    fn key(&self, vk: u16, down: bool) {
        let Some(q) = keymap::qnum(vk) else { return };
        {
            let mut held = self.ivars().held.borrow_mut();
            if down {
                held.insert(vk);
            } else if !held.remove(&vk) && vk != keymap::CAPS_LOCK {
                return;
            }
        }
        self.send(Input::Key { qnum: q, down });
    }

    // The scanout pixel under a window event, if it is over the picture.
    fn guest_point(&self, ev: &NSEvent) -> Option<(u32, u32)> {
        let p = self.convertPoint_fromView(ev.locationInWindow(), None);
        let b = self.bounds();
        let r = self.ivars().renderer.borrow();
        let (tw, th) = r.as_ref()?.tex_size;
        if tw == 0 || th == 0 || b.size.width <= 0.0 || b.size.height <= 0.0 {
            return None;
        }
        let scale = (b.size.width / tw as f64).min(b.size.height / th as f64);
        let (vw, vh) = (tw as f64 * scale, th as f64 * scale);
        let x = (p.x - (b.size.width - vw) / 2.0) / scale;
        // AppKit's origin is the bottom left; the guest's is the top left.
        let y = ((b.size.height - p.y) - (b.size.height - vh) / 2.0) / scale;
        if x < 0.0 || y < 0.0 || x >= tw as f64 || y >= th as f64 {
            return None;
        }
        Some((x as u32, y as u32))
    }

    fn pointer(&self, ev: &NSEvent) {
        if let Some((x, y)) = self.guest_point(ev) {
            self.send(Input::Abs { x, y });
        }
    }

    fn button(&self, ev: &NSEvent, button: u32, down: bool) {
        self.pointer(ev);
        self.send(Input::Button { button, down });
    }

    // A fixed run of events, each made as AppKit makes them and handed to the window, so that what reaches
    // QEMU went through the same responder path a person's keys do. The caller reads QEMU's trace.
    fn scripted_input(&self) {
        let iv = self.ivars();
        if let Some(r) = iv.renderer.borrow_mut().as_mut() {
            r.upload(&iv.display, &mut iv.stats.borrow_mut());
        }
        let Some(window) = self.window() else {
            eprintln!("scripted: no window");
            return;
        };
        let wn = window.windowNumber();
        let none = NSEventModifierFlags::empty();
        let key = |ty: NSEventType, vk: u16, flags: NSEventModifierFlags, repeat: bool| {
            let ev = NSEvent::keyEventWithType_location_modifierFlags_timestamp_windowNumber_context_characters_charactersIgnoringModifiers_isARepeat_keyCode(
                ty, NSPoint::new(0.0, 0.0), flags, 0.0, wn, None, ns_string!(""), ns_string!(""), repeat, vk);
            match ev {
                Some(ev) => window.sendEvent(&ev),
                None => eprintln!("scripted: no key event for {vk:#x}"),
            }
        };
        let mouse = |ty: NSEventType, p: NSPoint| {
            let ev = NSEvent::mouseEventWithType_location_modifierFlags_timestamp_windowNumber_context_eventNumber_clickCount_pressure(
                ty, p, none, 0.0, wn, None, 0, 1, 1.0);
            match ev {
                Some(ev) => window.sendEvent(&ev),
                None => eprintln!("scripted: no mouse event"),
            }
        };
        key(NSEventType::KeyDown, 0x00, none, false); // A
        key(NSEventType::KeyUp, 0x00, none, false);
        key(NSEventType::KeyDown, 0x7e, none, false); // Up
        key(NSEventType::KeyUp, 0x7e, none, false);
        key(
            NSEventType::FlagsChanged,
            0x38,
            NSEventModifierFlags::Shift,
            false,
        ); // left Shift down, then up
        key(NSEventType::FlagsChanged, 0x38, none, false);
        key(NSEventType::KeyDown, 0x00, none, false); // A held: one repeat
        key(NSEventType::KeyDown, 0x00, none, true);
        key(NSEventType::KeyUp, 0x00, none, false);
        key(NSEventType::KeyDown, 0x01, none, false); // S down, the window loses the keys, S up arrives late
        self.release_keys();
        key(NSEventType::KeyUp, 0x01, none, false);
        // The pointer: the middle of the view, then the centre of guest pixel (10, 10).
        let b = self.bounds();
        let (tw, th) = iv
            .renderer
            .borrow()
            .as_ref()
            .map(|r| r.tex_size)
            .unwrap_or((0, 0));
        if tw == 0 || th == 0 {
            eprintln!("scripted: no scanout, pointer events skipped");
            return;
        }
        let scale = (b.size.width / tw as f64).min(b.size.height / th as f64);
        let at = |gx: f64, gy: f64| {
            let x = (b.size.width - tw as f64 * scale) / 2.0 + gx * scale;
            let y_top = (b.size.height - th as f64 * scale) / 2.0 + gy * scale;
            self.convertPoint_toView(NSPoint::new(x, b.size.height - y_top), None)
        };
        let mid = at(tw as f64 / 2.0, th as f64 / 2.0);
        mouse(NSEventType::MouseMoved, mid);
        mouse(NSEventType::LeftMouseDown, mid);
        mouse(NSEventType::LeftMouseUp, mid);
        // A key between the two positions: adjacent positions may be sent as one, and the test wants both.
        key(NSEventType::KeyDown, 0x03, none, false); // F
        key(NSEventType::KeyUp, 0x03, none, false);
        let corner = at(10.5, 10.5);
        mouse(NSEventType::RightMouseDown, corner);
        mouse(NSEventType::RightMouseUp, corner);
        // Outside the picture (the letterbox bar): a move there reaches the guest as nothing.
        mouse(
            NSEventType::MouseMoved,
            self.convertPoint_toView(NSPoint::new(1.0, b.size.height / 2.0), None),
        );
        // The same through NSApplication, as the event loop delivers them: a key-up while Command is held is
        // the one AppKit keeps to itself.
        let app = objc2_app_kit::NSApplication::sharedApplication(self.mtm());
        let akey = |ty: NSEventType, vk: u16, flags: NSEventModifierFlags| {
            let ev = NSEvent::keyEventWithType_location_modifierFlags_timestamp_windowNumber_context_characters_charactersIgnoringModifiers_isARepeat_keyCode(
                ty, NSPoint::new(0.0, 0.0), flags, 0.0, wn, None, ns_string!("d"), ns_string!("d"), false, vk);
            if let Some(ev) = ev {
                app.sendEvent(&ev);
            }
        };
        let cmd = NSEventModifierFlags::Command;
        akey(NSEventType::KeyDown, 0x02, none); // control: D down/up with no modifier -> 32, 32
        akey(NSEventType::KeyUp, 0x02, none);
        let fl = NSEvent::keyEventWithType_location_modifierFlags_timestamp_windowNumber_context_characters_charactersIgnoringModifiers_isARepeat_keyCode(
            NSEventType::FlagsChanged, NSPoint::new(0.0, 0.0), cmd, 0.0, wn, None, ns_string!(""), ns_string!(""), false, 0x37);
        if let Some(ev) = fl {
            app.sendEvent(&ev);
        } // Command down -> 219
        akey(NSEventType::KeyDown, 0x02, cmd); // D down with Command -> 32
        akey(NSEventType::KeyUp, 0x02, cmd); // D up with Command -> 32 release, IF AppKit delivers it
        let fl = NSEvent::keyEventWithType_location_modifierFlags_timestamp_windowNumber_context_characters_charactersIgnoringModifiers_isARepeat_keyCode(
            NSEventType::FlagsChanged, NSPoint::new(0.0, 0.0), none, 0.0, wn, None, ns_string!(""), ns_string!(""), false, 0x37);
        if let Some(ev) = fl {
            app.sendEvent(&ev);
        } // Command up -> 219 release
        eprintln!(
            "scripted: sent; view {}x{} points, scanout {tw}x{th}, scale {scale:.4}",
            b.size.width, b.size.height
        );
    }

    // The end of a timed run: bring the texture up to date whether or not a frame was ever due, write
    // what the pipeline makes of it, and leave.
    fn finish(&self) {
        let iv = self.ivars();
        if let Some(r) = iv.renderer.borrow_mut().as_mut() {
            r.upload(&iv.display, &mut iv.stats.borrow_mut());
            eprintln!(
                "finish: {} display-link ticks, {} presented frames, texture {}x{}, pixels {}",
                iv.ticks.get(),
                iv.presented.get(),
                r.tex_size.0,
                r.tex_size.1,
                if r.mapped {
                    "mapped"
                } else {
                    "in the messages"
                }
            );
            if iv.opts.print_stats {
                let mut total = iv.total.borrow_mut();
                total.add(&iv.stats.borrow());
                eprintln!("stats in all: {}", total.line());
            }
            if let Some(path) = &iv.opts.dump {
                match r.snapshot() {
                    Some((w, h, rgb)) => {
                        let mut out = format!("P6\n{w} {h}\n255\n").into_bytes();
                        out.extend_from_slice(&rgb);
                        match std::fs::write(path, out) {
                            Ok(()) => eprintln!("dump: {w}x{h} to {path}"),
                            Err(e) => eprintln!("dump: {path}: {e}"),
                        }
                    }
                    None => eprintln!("dump: no scanout to show"),
                }
            }
        }
        std::process::exit(0);
    }

    fn frame(&self) {
        let iv = self.ivars();
        let mut guard = iv.renderer.borrow_mut();
        let Some(r) = guard.as_mut() else { return };
        let t0 = Instant::now();
        let scale = self.window().map(|w| w.backingScaleFactor()).unwrap_or(1.0);
        let b = self.bounds();
        let want = (
            (b.size.width * scale).round().max(1.0) as u32,
            (b.size.height * scale).round().max(1.0) as u32,
        );
        let mut changed = false;
        if want != r.drawable_size {
            r.layer.setContentsScale(scale);
            r.layer.setDrawableSize(CGSize {
                width: want.0 as f64,
                height: want.1 as f64,
            });
            r.drawable_size = want;
            changed = true;
        }
        let mut stats = iv.stats.borrow_mut();
        if iv.display.dirty.swap(false, Ordering::AcqRel) {
            changed |= r.upload(&iv.display, &mut stats);
        }
        if changed {
            iv.owed.set(true);
        }
        if !iv.owed.get() || !r.present() {
            return;
        }
        iv.owed.set(false);
        iv.presented.set(iv.presented.get() + 1);
        let us = t0.elapsed().as_micros() as u64;
        stats.frames += 1;
        stats.frame_us += us;
        stats.frame_us_max = stats.frame_us_max.max(us);
        if iv.opts.print_stats {
            let since = iv.stats_since.get().unwrap_or(t0);
            if iv.stats_since.get().is_none() {
                iv.stats_since.set(Some(t0));
            }
            if since.elapsed().as_secs_f64() >= 1.0 {
                let s = std::mem::take(&mut *stats);
                iv.total.borrow_mut().add(&s);
                eprintln!(
                    "stats: {}, texture {}x{}, drawable {}x{}",
                    s.line(),
                    r.tex_size.0,
                    r.tex_size.1,
                    r.drawable_size.0,
                    r.drawable_size.1
                );
                iv.stats_since.set(Some(Instant::now()));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::notches;

    #[test]
    fn a_wheel_scrolls_a_notch_an_event() {
        let mut acc = 0.0;
        assert_eq!(notches(&mut acc, 1.0, false), 1);
        assert_eq!(notches(&mut acc, 0.1, false), 1);
        assert_eq!(notches(&mut acc, -3.0, false), -1);
        assert_eq!(notches(&mut acc, 0.0, false), 0);
        assert_eq!(acc, 0.0);
    }

    #[test]
    fn a_trackpad_scrolls_a_notch_every_ten_points() {
        let mut acc = 0.0;
        assert_eq!(notches(&mut acc, 4.0, true), 0);
        assert_eq!(notches(&mut acc, 4.0, true), 0);
        assert_eq!(notches(&mut acc, 4.0, true), 1);
        assert_eq!(acc, 2.0);
        assert_eq!(notches(&mut acc, 25.0, true), 2);
        assert_eq!(acc, 7.0);
        // Turning round drops what was owed the other way.
        assert_eq!(notches(&mut acc, -4.0, true), 0);
        assert_eq!(acc, -4.0);
        assert_eq!(notches(&mut acc, -6.0, true), -1);
        assert_eq!(acc, 0.0);
        // A fling is bounded, and is not replayed afterwards.
        assert_eq!(notches(&mut acc, 1000.0, true), 8);
        assert_eq!(acc, 0.0);
        assert_eq!(notches(&mut acc, -1000.0, true), -8);
        assert_eq!(acc, 0.0);
    }
}
