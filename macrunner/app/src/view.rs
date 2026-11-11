// The guest's screen as a view: a CAMetalLayer that shows the scanout, and the keys and pointer of
// whoever has the window. All of it runs on the main thread; the display thread only marks damage.
use std::cell::{Cell, OnceCell, RefCell};
use std::collections::HashSet;
use std::ptr::NonNull;
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::{Duration, Instant};

use objc2::rc::Retained;
use objc2::runtime::{AnyObject, ProtocolObject};
use objc2::{define_class, msg_send, sel, AnyThread, DefinedClass, MainThreadOnly};
use objc2_app_kit::{
    NSApplication, NSBitmapFormat, NSBitmapImageRep, NSCursor, NSDeviceRGBColorSpace, NSEvent,
    NSEventModifierFlags, NSEventType, NSGraphicsContext, NSImage,
    NSMenuDidBeginTrackingNotification, NSResponder, NSScreen, NSView,
    NSWindowDidResignKeyNotification,
};
use objc2_core_foundation::{CGPoint, CGSize};
use objc2_core_graphics::{kCGColorSpaceSRGB, CGColorSpace, CGEvent, CGScrollEventUnit};
use objc2_foundation::{
    ns_string, MainThreadMarker, NSNotificationCenter, NSObject, NSObjectProtocol, NSPoint, NSRect,
    NSRunLoop, NSRunLoopCommonModes, NSSize, NSTimer,
};
use objc2_metal::{
    MTLClearColor, MTLCommandBuffer, MTLCommandEncoder, MTLCommandQueue,
    MTLCreateSystemDefaultDevice, MTLDevice, MTLLibrary, MTLLoadAction, MTLOrigin, MTLPixelFormat,
    MTLPrimitiveType, MTLRegion, MTLRenderCommandEncoder, MTLRenderPassDescriptor,
    MTLRenderPipelineDescriptor, MTLRenderPipelineState, MTLSamplerDescriptor,
    MTLSamplerMinMagFilter, MTLSamplerState, MTLSize, MTLStoreAction, MTLTexture,
    MTLTextureDescriptor, MTLTextureSwizzle, MTLTextureSwizzleChannels, MTLTextureUsage,
    MTLViewport,
};
use objc2_quartz_core::{CADisplayLink, CALayer, CAMetalDrawable, CAMetalLayer};

use crate::display::{Display, Input, Order, Pixels, Pointer, Rect};
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
    tex_order: Order,
    drawable_size: (u32, u32),
    mapped: bool, // whether the last upload read QEMU's memory or a copy it sent
}

type Texture = Retained<ProtocolObject<dyn MTLTexture>>;

// A BGRA texture takes a pixel's four bytes as blue, green, red and alpha, in that order. This is
// which of them each colour is read from when the frame's bytes are in another order.
fn swizzle(order: Order) -> MTLTextureSwizzleChannels {
    use MTLTextureSwizzle as From;
    let (red, green, blue) = match order {
        Order::Bgrx => (From::Red, From::Green, From::Blue),
        Order::Rgbx => (From::Blue, From::Green, From::Red),
        Order::Xrgb => (From::Green, From::Red, From::Alpha),
        Order::Xbgr => (From::Alpha, From::Red, From::Green),
    };
    MTLTextureSwizzleChannels {
        red,
        green,
        blue,
        alpha: From::One,
    }
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

// Where a tw x th picture goes in a w x h area that shows all of it, centred, at one scale:
// (x, y, width, height, scale). The drawing and the pointer both take the picture's place from here.
fn fit(tw: f64, th: f64, w: f64, h: f64) -> Option<(f64, f64, f64, f64, f64)> {
    if tw <= 0.0 || th <= 0.0 || w <= 0.0 || h <= 0.0 {
        return None;
    }
    let scale = (w / tw).min(h / th);
    let (vw, vh) = (tw * scale, th * scale);
    Some(((w - vw) / 2.0, (h - vh) / 2.0, vw, vh, scale))
}

// QEMU's number for a button AppKit numbers `number` (left 0, right 1, middle 2, then the side
// buttons). QEMU's 3 and 4 are the wheel.
fn button_code(number: isize) -> Option<u32> {
    Some(match number {
        0 => 0,
        1 => 2,
        2 => 1,
        3 => 5,
        4 => 6,
        _ => return None,
    })
}

// Copy rectangle `r` of a frame whose rows are `stride` bytes apart into the same place in `tex`,
// from `len` readable bytes at `src`. False, with nothing copied, if the rectangle's last pixel is not
// among them. Metal is asked for r.w pixels of each row and reads no further (the test below puts an
// unreadable page after the last one).
// SAFETY: `src` is valid for reads of `len` bytes for the whole call, and `r` lies inside `tex`.
unsafe fn replace(
    tex: &ProtocolObject<dyn MTLTexture>,
    src: *const u8,
    len: usize,
    stride: usize,
    r: Rect,
) -> bool {
    if r.w == 0 || r.h == 0 {
        return false;
    }
    let first = r.y as usize * stride + r.x as usize * 4;
    let last = (r.y + r.h - 1) as usize * stride + (r.x + r.w) as usize * 4;
    if last > len {
        return false;
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
    tex.replaceRegion_mipmapLevel_withBytes_bytesPerRow(
        region,
        0,
        NonNull::new_unchecked(src.add(first) as *mut std::ffi::c_void),
        stride,
    );
    true
}

// The Mac's pointer wearing the guest's image while it is over the picture. The guest moves its own
// pointer to the positions it is sent, so the Mac's is already where the guest's is, and has no lag.
#[derive(Default)]
struct Worn {
    image: Option<Arc<Pointer>>,
    on: bool,
    scale: f64, // view points to a guest pixel when `cursor` was made
    cursor: Option<Retained<NSCursor>>, // None: the Mac's arrow stays
}

// The view points an image pixel of the pointer gets: a guest pixel's worth, but no more than keeps
// the pointer within MAX_POINTER_POINTS a side. A guest may set a screen so small that its picture
// is magnified many times over, and its pointer would be with it.
const MAX_POINTER_POINTS: f64 = 256.0;
fn pointer_scale(w: u32, h: u32, scale: f64) -> f64 {
    scale.min(MAX_POINTER_POINTS / w.max(h).max(1) as f64)
}

// A bitmap AppKit owns, `w` x `h` pixels of R, G, B, A.
fn bitmap(w: u32, h: u32, format: NSBitmapFormat) -> Option<Retained<NSBitmapImageRep>> {
    let rep = unsafe {
        NSBitmapImageRep::initWithBitmapDataPlanes_pixelsWide_pixelsHigh_bitsPerSample_samplesPerPixel_hasAlpha_isPlanar_colorSpaceName_bitmapFormat_bytesPerRow_bitsPerPixel(
            NSBitmapImageRep::alloc(),
            std::ptr::null_mut(),
            w as isize,
            h as isize,
            8,
            4,
            true,
            false,
            NSDeviceRGBColorSpace,
            format,
            w as isize * 4,
            32,
        )
    }?;
    (!rep.bitmapData().is_null() && rep.bytesPerRow() == w as isize * 4).then_some(rep)
}

// The guest's pointer as an NSCursor, `scale` view points to an image pixel.
fn ns_cursor(p: &Pointer, scale: f64) -> Option<Retained<NSCursor>> {
    // No flag: alpha last and multiplied in, which is what the guest's image is.
    let rep = bitmap(p.w, p.h, NSBitmapFormat(0))?;
    let rgba = p.rgba();
    // SAFETY: `bitmap` made room for w * h * 4 bytes, which is what a Pointer holds.
    unsafe { std::ptr::copy_nonoverlapping(rgba.as_ptr(), rep.bitmapData(), rgba.len()) };
    let size = NSSize::new(p.w as f64 * scale, p.h as f64 * scale);
    rep.setSize(size);
    let image = NSImage::initWithSize(NSImage::alloc(), size);
    image.addRepresentation(&rep);
    Some(NSCursor::initWithImage_hotSpot(
        NSCursor::alloc(),
        &image,
        NSPoint::new(p.hot_x as f64 * scale, p.hot_y as f64 * scale),
    ))
}

// What AppKit draws of a cursor's image over mid-grey, `w` x `h` pixels as RGB rows: for a test to
// compare, and for a person to look at.
fn over_grey(cursor: &NSCursor, w: u32, h: u32) -> Option<Vec<u8>> {
    let out = bitmap(w, h, NSBitmapFormat(0))?;
    let len = w as usize * h as usize * 4;
    // SAFETY: `bitmap` made room for `len` bytes, and nothing else holds them yet.
    for px in unsafe { std::slice::from_raw_parts_mut(out.bitmapData(), len) }.chunks_exact_mut(4) {
        px.copy_from_slice(&[0x80, 0x80, 0x80, 0xff]);
    }
    let ctx = NSGraphicsContext::graphicsContextWithBitmapImageRep(&out)?;
    NSGraphicsContext::saveGraphicsState_class();
    NSGraphicsContext::setCurrentContext(Some(&ctx));
    cursor.image().drawInRect(NSRect::new(
        NSPoint::new(0.0, 0.0),
        NSSize::new(w as f64, h as f64),
    ));
    ctx.flushGraphics();
    NSGraphicsContext::restoreGraphicsState_class();
    // SAFETY: as above; the drawing is done.
    let px = unsafe { std::slice::from_raw_parts(out.bitmapData(), len) };
    Some(
        px.chunks_exact(4)
            .flat_map(|p| [p[0], p[1], p[2]])
            .collect(),
    )
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
            tex_order: Order::Bgrx,
            drawable_size: (0, 0),
            mapped: false,
        })
    }

    fn make_texture(&self, w: u32, h: u32, order: Order) -> Option<Texture> {
        let d = unsafe {
            MTLTextureDescriptor::texture2DDescriptorWithPixelFormat_width_height_mipmapped(
                MTLPixelFormat::BGRA8Unorm,
                w as usize,
                h as usize,
                false,
            )
        };
        d.setSwizzle(swizzle(order));
        self.device.newTextureWithDescriptor(&d)
    }

    // Bring the texture up to the scanout. Returns whether anything changed on screen.
    fn upload(&mut self, display: &Display, stats: &mut Stats) -> bool {
        self.upload_with(display, stats, Self::make_texture)
    }

    // `make` is the making of a texture, which a test replaces in order to act while it is made.
    fn upload_with(
        &mut self,
        display: &Display,
        stats: &mut Stats,
        make: impl FnOnce(&Renderer, u32, u32, Order) -> Option<Texture>,
    ) -> bool {
        let t0 = Instant::now();
        let mut s = display.shared.lock().unwrap();
        let Some(sc) = &s.scanout else {
            s.damage = None;
            self.serial = 0;
            self.tex_size = (0, 0);
            return self.texture.take().is_some();
        };
        let fresh = sc.serial != self.serial || self.texture.is_none();
        let fits = self.tex_size == (sc.w, sc.h) && self.tex_order == sc.order;
        if fresh && (!fits || self.texture.is_none()) {
            // A texture is made with the lock released: QEMU waits on the display thread, for a
            // second at most, while it hands over a scanout, and gives up sharing its memory for
            // good if it is not answered. What the making costs is not added to that wait.
            let (serial, w, h, order) = (sc.serial, sc.w, sc.h, sc.order);
            drop(s);
            let made = make(self, w, h, order);
            s = display.shared.lock().unwrap();
            if s.scanout.as_ref().map(|sc| sc.serial) != Some(serial) {
                // The guest set another scanout meanwhile: that one is the next frame's. What is on
                // the glass, and the size the pointer is placed by, stay as they are until then.
                display.dirty.store(true, Ordering::Release);
                return false;
            }
            let had = self.texture.is_some();
            self.tex_size = if made.is_some() { (w, h) } else { (0, 0) };
            self.tex_order = order;
            self.texture = made;
            if self.texture.is_none() {
                return had;
            }
        }
        let Some(tex) = &self.texture else {
            return false;
        };
        let damage = s.damage.take();
        let Some(sc) = &s.scanout else {
            return false;
        };
        if fresh {
            self.serial = sc.serial;
        }
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
        if r.x + r.w > sc.w || r.y + r.h > sc.h {
            return false;
        }
        let stride = sc.stride as usize;
        self.mapped = matches!(sc.pixels, Pixels::Mapped(_));
        let copied = match &sc.pixels {
            // A mapping outlives the lock through its own reference, so the display thread (and with it
            // QEMU's main loop) is not held for the length of the copy.
            Pixels::Mapped(region) => {
                let region = region.clone();
                drop(s);
                // SAFETY: the mapping is `len` bytes and lives until `region` is dropped below.
                unsafe { replace(tex, region.as_ptr(), region.len(), stride, r) }
            }
            // SAFETY: the buffer cannot change or move while the lock is held.
            Pixels::Owned(buf) => unsafe { replace(tex, buf.as_ptr(), buf.len(), stride, r) },
        };
        if !copied {
            return false;
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
            let (tw, th) = (self.tex_size.0 as f64, self.tex_size.1 as f64);
            if let Some((x, y, vw, vh, scale)) = fit(tw, th, size.0 as f64, size.1 as f64) {
                // Whole pixels, so that a whole-number scale stays one.
                enc.setViewport(MTLViewport {
                    originX: x.floor(),
                    originY: y.floor(),
                    width: vw.round(),
                    height: vh.round(),
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
    held: RefCell<keymap::Held>,
    pressed: RefCell<HashSet<u32>>, // buttons the guest holds, by QEMU's number
    stats: RefCell<Stats>,
    total: RefCell<Stats>,
    stats_since: Cell<Option<Instant>>,
    opts: Options,
    presented: Cell<u64>,
    owed: Cell<bool>, // the texture changed and no frame has shown it yet
    ticks: Cell<u64>,
    scroll: Cell<f64>,
    worn: RefCell<Worn>,
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
            self.wear_pointer();
        }

        #[unsafe(method(resetCursorRects))]
        fn reset_cursor_rects(&self) {
            let Ok(worn) = self.ivars().worn.try_borrow() else { return };
            if let (Some(cursor), Some((rect, _))) = (&worn.cursor, self.picture()) {
                self.addCursorRect_cursor(rect, cursor);
            }
        }

        // A display link is silent while the display sleeps, so a test's deadline has a timer of its own.
        #[unsafe(method(deadline:))]
        fn deadline(&self, _timer: &NSTimer) { self.finish(); }

        #[unsafe(method(scripted:))]
        fn scripted(&self, _timer: &NSTimer) { self.scripted_input(); }

        #[unsafe(method(keyDown:))]
        fn key_down(&self, ev: &NSEvent) {
            self.settle_modifiers(ev);
            self.key(ev.keyCode(), true);
        }

        #[unsafe(method(keyUp:))]
        fn key_up(&self, ev: &NSEvent) {
            self.settle_modifiers(ev);
            self.key(ev.keyCode(), false);
        }

        #[unsafe(method(flagsChanged:))]
        fn flags_changed(&self, ev: &NSEvent) {
            self.settle_modifiers(ev);
            let vk = ev.keyCode();
            if vk == keymap::CAPS_LOCK {
                // macOS reports the lock's state, not the key: one event per toggle.
                self.key(vk, true);
                self.key(vk, false);
                return;
            }
            let was_held = self.ivars().held.borrow().holds(vk);
            let Some(down) = keymap::modifier_is_down(vk, ev.modifierFlags().0, was_held) else { return };
            if down != was_held { self.key(vk, down); }
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
        fn mouse_down(&self, ev: &NSEvent) { self.button(ev, button_code(0), true); }
        #[unsafe(method(mouseUp:))]
        fn mouse_up(&self, ev: &NSEvent) { self.button(ev, button_code(0), false); }
        #[unsafe(method(rightMouseDown:))]
        fn right_mouse_down(&self, ev: &NSEvent) { self.button(ev, button_code(1), true); }
        #[unsafe(method(rightMouseUp:))]
        fn right_mouse_up(&self, ev: &NSEvent) { self.button(ev, button_code(1), false); }
        // Every button past the first two arrives here, with its number in the event.
        #[unsafe(method(otherMouseDown:))]
        fn other_mouse_down(&self, ev: &NSEvent) {
            self.button(ev, button_code(ev.buttonNumber()).filter(|&b| b != 0 && b != 2), true);
        }
        #[unsafe(method(otherMouseUp:))]
        fn other_mouse_up(&self, ev: &NSEvent) {
            self.button(ev, button_code(ev.buttonNumber()).filter(|&b| b != 0 && b != 2), false);
        }

        #[unsafe(method(scrollWheel:))]
        fn scroll_wheel(&self, ev: &NSEvent) {
            self.settle_modifiers(ev);
            // A scroll over the bars is not the guest's: it would land wherever its pointer last was.
            if self.guest_point(ev).is_none() {
                return;
            }
            let mut acc = self.ivars().scroll.get();
            let n = notches(&mut acc, ev.scrollingDeltaY(), ev.hasPreciseScrollingDeltas());
            self.ivars().scroll.set(acc);
            if n == 0 {
                return;
            }
            // A window that is not active is still scrolled, and is sent no moves: the guest's
            // pointer may be anywhere, so the notches go with the place they were made over.
            self.pointer(ev);
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
            held: RefCell::new(keymap::Held::default()),
            pressed: RefCell::new(HashSet::new()),
            stats: RefCell::new(Stats::default()),
            total: RefCell::new(Stats::default()),
            stats_since: Cell::new(None),
            opts,
            presented: Cell::new(0),
            owed: Cell::new(false),
            ticks: Cell::new(0),
            scroll: Cell::new(0.0),
            worn: RefCell::new(Worn::default()),
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

    // Release everything the guest believes is held, keys and buttons: the window stopped being where
    // they go, and whatever is let go of from here on is let go of somewhere else.
    pub fn release_keys(&self) {
        let held = self.ivars().held.borrow_mut().drain();
        for qnum in held {
            self.send(Input::Key { qnum, down: false });
        }
        let mut pressed: Vec<u32> = self.ivars().pressed.borrow_mut().drain().collect();
        pressed.sort_unstable();
        for button in pressed {
            self.send(Input::Button {
                button,
                down: false,
            });
        }
    }

    // Quit: what is held is let go of, and the releases are given a moment to reach QEMU. It lifts
    // the keys itself when the app goes away, and not the buttons.
    pub fn part(&self) {
        self.release_keys();
        self.ivars().display.drain(Duration::from_millis(500));
    }

    // Every event, a click and a scroll as much as a key, says which modifiers are down. One the
    // guest holds that the event says is up was let go of where the view could not see it (a menu
    // that was open, a shortcut the system took), so the guest is told now, before the event's own
    // key or button.
    fn settle_modifiers(&self, ev: &NSEvent) {
        let gone = self
            .ivars()
            .held
            .borrow()
            .modifiers_gone(ev.modifierFlags().0);
        for code in gone {
            self.key(code, false);
        }
    }

    fn send(&self, ev: Input) {
        self.ivars().display.input.send(ev).ok();
    }

    // `code` is the event's key code. Where that key is depends on the keyboard, and is settled
    // when it goes down.
    fn key(&self, code: u16, down: bool) {
        let sent = {
            let mut held = self.ivars().held.borrow_mut();
            if down {
                held.press(code, keymap::iso_keyboard())
            } else {
                held.release(code)
            }
        };
        if let Some(qnum) = sent {
            self.send(Input::Key { qnum, down });
        }
    }

    // Where the scanout is shown in the view, and how many view points a guest pixel takes.
    // None while there is no scanout, or while a frame is being drawn.
    fn picture(&self) -> Option<(NSRect, f64)> {
        let b = self.bounds();
        let (tw, th) = self.ivars().renderer.try_borrow().ok()?.as_ref()?.tex_size;
        let (x, y, vw, vh, scale) = fit(tw as f64, th as f64, b.size.width, b.size.height)?;
        Some((NSRect::new(NSPoint::new(x, y), NSSize::new(vw, vh)), scale))
    }

    // The scanout pixel under a window event, if it is over the picture.
    fn guest_point(&self, ev: &NSEvent) -> Option<(u32, u32)> {
        let p = self.convertPoint_fromView(ev.locationInWindow(), None);
        let (r, scale) = self.picture()?;
        let x = (p.x - r.origin.x) / scale;
        // AppKit's origin is the bottom left; the guest's is the top left.
        let y = (r.origin.y + r.size.height - p.y) / scale;
        if x < 0.0 || y < 0.0 || x >= r.size.width / scale || y >= r.size.height / scale {
            return None;
        }
        Some((x as u32, y as u32))
    }

    // Bring the Mac's pointer up to the guest's: its image, whether it shows one, and the size a
    // guest pixel has in the view now.
    fn wear_pointer(&self) {
        let iv = self.ivars();
        let picture = self.picture();
        let scale = picture.map_or(0.0, |(_, s)| s);
        let cursor = {
            let mut worn = iv.worn.borrow_mut();
            let news = iv.display.pointer_dirty.swap(false, Ordering::AcqRel);
            if !news && scale == worn.scale {
                return;
            }
            if news {
                let s = iv.display.shared.lock().unwrap();
                worn.image = s.pointer.clone();
                worn.on = s.pointer_on;
            }
            worn.scale = scale;
            worn.cursor = match &worn.image {
                // A guest that never defined a pointer has none to wear: the Mac's arrow stays.
                None => None,
                Some(_) if scale <= 0.0 => None,
                Some(p) if worn.on => ns_cursor(p, pointer_scale(p.w, p.h, scale)),
                // A guest that hides its pointer is shown none.
                Some(_) => Pointer::new(1, 1, 0, 0, vec![0; 4]).and_then(|p| ns_cursor(&p, 1.0)),
            };
            worn.cursor.clone()
        };
        let Some(window) = self.window() else { return };
        // Also when the picture has gone: the rectangle that was registered for it goes with it.
        window.invalidateCursorRectsForView(self);
        // The rectangles are only looked at when the pointer next moves; one that is resting on the
        // picture would keep its old image until then.
        if let (Some(cursor), Some((rect, _)), true) = (cursor, picture, window.isKeyWindow()) {
            let p = self.convertPoint_fromView(window.mouseLocationOutsideOfEventStream(), None);
            let inside = p.x >= rect.origin.x
                && p.y >= rect.origin.y
                && p.x < rect.origin.x + rect.size.width
                && p.y < rect.origin.y + rect.size.height;
            if inside {
                cursor.set();
            }
        }
    }

    fn pointer(&self, ev: &NSEvent) {
        self.settle_modifiers(ev);
        if let Some((x, y)) = self.guest_point(ev) {
            self.send(Input::Abs { x, y });
        }
    }

    // A press counts only over the picture: over the bars it would land wherever the guest's
    // pointer last was. A release is sent exactly when its press was, wherever it happens.
    fn button(&self, ev: &NSEvent, button: Option<u32>, down: bool) {
        self.settle_modifiers(ev);
        let Some(button) = button else { return };
        if down {
            if self.guest_point(ev).is_none() || !self.ivars().pressed.borrow_mut().insert(button) {
                return;
            }
        } else if !self.ivars().pressed.borrow_mut().remove(&button) {
            return;
        }
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
        // A pointer as the guest would define one: three colours, white, a clear pixel whose colour
        // must not show, and a colour at half strength with its alpha multiplied in, which comes
        // out one way if it is taken so and another if the alpha is applied to it again. B, G, R, A.
        iv.display.define_pointer(
            3,
            2,
            1,
            1,
            vec![
                0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, // red, green, blue
                255, 255, 255, 255, 30, 20, 10, 0, 20, 50, 100, 128, // white, clear, half
            ],
        );
        iv.display.show_pointer(true);
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
        // What AppKit posts when the window stops being where keys go, and when a menu opens. Posted
        // here as AppKit posts them, so that what answers is the app's own wiring.
        let center = NSNotificationCenter::defaultCenter();
        let lose_keys = || unsafe {
            center.postNotificationName_object(NSWindowDidResignKeyNotification, Some(&window))
        };
        // S down, the window loses the keys, another key is typed, and S up arrives late.
        key(NSEventType::KeyDown, 0x01, none, false);
        lose_keys();
        key(NSEventType::KeyDown, 0x04, none, false); // H
        key(NSEventType::KeyUp, 0x04, none, false);
        key(NSEventType::KeyUp, 0x01, none, false);
        // Option goes down and its release never arrives (a menu took it): the next key says no
        // modifier is down, so Option is let go of before that key is sent.
        key(
            NSEventType::FlagsChanged,
            0x3a,
            NSEventModifierFlags::Option,
            false,
        );
        key(NSEventType::KeyDown, 0x0b, none, false); // B
        key(NSEventType::KeyUp, 0x0b, none, false);
        key(NSEventType::FlagsChanged, 0x3a, none, false);
        // E down, a menu opens, another key is typed, and E up arrives late.
        key(NSEventType::KeyDown, 0x0e, none, false);
        unsafe { center.postNotificationName_object(NSMenuDidBeginTrackingNotification, None) };
        key(NSEventType::KeyDown, 0x05, none, false); // G
        key(NSEventType::KeyUp, 0x05, none, false);
        key(NSEventType::KeyUp, 0x0e, none, false);
        // The two Shifts as a keyboard reports them, each with its own bit beside the one they
        // share. Left goes down, the window loses the keys, right goes down, and left comes up:
        // a key the guest no longer holds, which the shared bit alone would take for a press.
        let shift = |own: usize| NSEventModifierFlags(NSEventModifierFlags::Shift.0 | own);
        key(NSEventType::FlagsChanged, 0x38, shift(0x02), false);
        lose_keys();
        key(NSEventType::FlagsChanged, 0x3c, shift(0x06), false);
        key(NSEventType::FlagsChanged, 0x38, shift(0x04), false);
        key(NSEventType::FlagsChanged, 0x3c, none, false);
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
        // Control goes down and its release is never seen. The next thing is a click whose flags
        // say no modifier is down: Control is let go of before the button goes down.
        key(
            NSEventType::FlagsChanged,
            0x3b,
            NSEventModifierFlags::Control,
            false,
        );
        mouse(NSEventType::RightMouseDown, corner);
        mouse(NSEventType::RightMouseUp, corner);
        key(NSEventType::FlagsChanged, 0x3b, none, false);
        // A wheel notch over the same pixel. AppKit makes no scroll events, so Core Graphics does;
        // the event belongs to no window, and its place is read as it is given. Core Graphics
        // counts down from the top of the first screen, AppKit up from its bottom.
        let top = NSScreen::screens(self.mtm())
            .firstObject()
            .map_or(0.0, |s| s.frame().size.height);
        let notch = CGEvent::new_scroll_wheel_event2(None, CGScrollEventUnit::Line, 1, 1, 0, 0);
        if let Some(cg) = &notch {
            CGEvent::set_location(Some(cg), CGPoint::new(corner.x, top - corner.y));
        }
        match notch.and_then(|cg| NSEvent::eventWithCGEvent(&cg)) {
            Some(ev) => unsafe { msg_send![self, scrollWheel: &*ev] },
            None => eprintln!("scripted: no scroll event"),
        }
        // The left button, handed to the view itself: a window that is not active keeps a left
        // click for its own activation, so the window's routing of one is not what is tried here.
        let left = |down: bool, p: NSPoint| {
            let ty = if down {
                NSEventType::LeftMouseDown
            } else {
                NSEventType::LeftMouseUp
            };
            let ev = NSEvent::mouseEventWithType_location_modifierFlags_timestamp_windowNumber_context_eventNumber_clickCount_pressure(
                ty, p, none, 0.0, wn, None, 0, 1, 1.0);
            match (ev, down) {
                (Some(ev), true) => unsafe { msg_send![self, mouseDown: &*ev] },
                (Some(ev), false) => unsafe { msg_send![self, mouseUp: &*ev] },
                (None, _) => eprintln!("scripted: no mouse event"),
            }
        };
        left(true, mid);
        left(false, mid);
        // The letterbox bar, outside the picture. A click there is nobody's in the guest.
        let bar = self.convertPoint_toView(NSPoint::new(1.0, b.size.height / 2.0), None);
        mouse(NSEventType::RightMouseDown, bar);
        mouse(NSEventType::RightMouseUp, bar);
        // A press over the picture let go of over the bar: the release is sent, without a position.
        mouse(NSEventType::RightMouseDown, corner);
        mouse(NSEventType::RightMouseUp, bar);
        // A button held when the window loses the keys is let go of then, and its own release is late.
        left(true, mid);
        lose_keys();
        left(false, mid);
        // A move over the bar reaches the guest as nothing.
        mouse(NSEventType::MouseMoved, bar);
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
          // The left button goes down and the run ends with it down: it is let go of on the way out.
        left(true, mid);
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
        }
        // A display that sleeps gives no ticks, so the pointer is brought up to date here as well.
        self.wear_pointer();
        if let Some(r) = iv.renderer.borrow().as_ref() {
            let worn = iv.worn.borrow();
            let pointer = match (&worn.image, &worn.cursor) {
                (None, _) => "none".to_string(),
                (Some(p), cursor) => format!(
                    "{}x{} hot {},{} {}",
                    p.w,
                    p.h,
                    p.hot_x,
                    p.hot_y,
                    match (cursor.is_some(), worn.on) {
                        (false, _) => "NOT WORN",
                        (true, true) => "shown",
                        (true, false) => "hidden",
                    }
                ),
            };
            eprintln!(
                "finish: {} display-link ticks, {} presented frames, texture {}x{}, pixels {}, pointer {}",
                iv.ticks.get(),
                iv.presented.get(),
                r.tex_size.0,
                r.tex_size.1,
                if r.mapped {
                    "mapped"
                } else {
                    "in the messages"
                },
                pointer
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
                // The pointer the Mac wears, beside the picture: <name>-pointer.ppm.
                if let (Some(p), Some(cursor), true) = (&worn.image, &worn.cursor, worn.on) {
                    let beside = format!("{}-pointer.ppm", path.trim_end_matches(".ppm"));
                    match over_grey(cursor, p.w, p.h) {
                        Some(rgb) => {
                            let mut out = format!("P6\n{} {}\n255\n", p.w, p.h).into_bytes();
                            out.extend_from_slice(&rgb);
                            match std::fs::write(&beside, out) {
                                Ok(()) => eprintln!("dump: pointer {}x{} to {beside}", p.w, p.h),
                                Err(e) => eprintln!("dump: {beside}: {e}"),
                            }
                        }
                        None => eprintln!("dump: the pointer could not be drawn"),
                    }
                }
            }
        }
        // Out through the application, as Quit goes, so that what is held is let go of on the way.
        NSApplication::sharedApplication(self.mtm()).terminate(None);
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
    use super::{button_code, fit, notches, pointer_scale, replace, Renderer, Stats};
    use crate::display::{Display, Order, Rect};
    use objc2::runtime::ProtocolObject;
    use objc2_metal::{
        MTLCommandBuffer, MTLCommandQueue, MTLCreateSystemDefaultDevice, MTLDevice, MTLOrigin,
        MTLPixelFormat, MTLRegion, MTLSize, MTLTexture, MTLTextureDescriptor, MTLTextureUsage,
    };
    use std::ptr::NonNull;
    use std::sync::atomic::Ordering;

    fn texture(
        device: &ProtocolObject<dyn MTLDevice>,
        w: u32,
        h: u32,
        target: bool,
    ) -> objc2::rc::Retained<ProtocolObject<dyn MTLTexture>> {
        let d = unsafe {
            MTLTextureDescriptor::texture2DDescriptorWithPixelFormat_width_height_mipmapped(
                MTLPixelFormat::BGRA8Unorm,
                w as usize,
                h as usize,
                false,
            )
        };
        if target {
            d.setUsage(MTLTextureUsage::RenderTarget | MTLTextureUsage::ShaderRead);
        }
        device.newTextureWithDescriptor(&d).unwrap()
    }

    // The texture's pixels, four bytes each, rows w * 4 apart.
    fn read(tex: &ProtocolObject<dyn MTLTexture>, w: u32, h: u32) -> Vec<u8> {
        let mut out = vec![0u8; w as usize * h as usize * 4];
        let region = MTLRegion {
            origin: MTLOrigin { x: 0, y: 0, z: 0 },
            size: MTLSize {
                width: w as usize,
                height: h as usize,
                depth: 1,
            },
        };
        unsafe {
            tex.getBytes_bytesPerRow_fromRegion_mipmapLevel(
                NonNull::new_unchecked(out.as_mut_ptr() as *mut std::ffi::c_void),
                w as usize * 4,
                region,
                0,
            );
        }
        out
    }

    #[test]
    fn a_picture_is_fitted_whole_and_centred() {
        assert_eq!(
            fit(4.0, 2.0, 400.0, 300.0),
            Some((0.0, 50.0, 400.0, 200.0, 100.0))
        );
        assert_eq!(
            fit(2.0, 4.0, 400.0, 300.0),
            Some((125.0, 0.0, 150.0, 300.0, 75.0))
        );
        assert_eq!(
            fit(4.0, 2.0, 400.0, 200.0),
            Some((0.0, 0.0, 400.0, 200.0, 100.0))
        );
        assert_eq!(
            fit(640.0, 480.0, 320.0, 240.0),
            Some((0.0, 0.0, 320.0, 240.0, 0.5))
        );
        for (tw, th, w, h) in [
            (0.0, 2.0, 4.0, 4.0),
            (2.0, 0.0, 4.0, 4.0),
            (2.0, 2.0, 0.0, 4.0),
            (2.0, 2.0, 4.0, -1.0),
        ] {
            assert_eq!(fit(tw, th, w, h), None);
        }
    }

    #[test]
    fn a_button_keeps_its_meaning() {
        // AppKit: left, right, middle, then the side buttons. QEMU: left 0, middle 1, right 2, side 5, extra 6.
        assert_eq!(
            [0, 1, 2, 3, 4].map(button_code),
            [Some(0), Some(2), Some(1), Some(5), Some(6)]
        );
        assert_eq!(button_code(5), None);
        assert_eq!(button_code(-1), None);
    }

    #[test]
    fn an_upload_reads_the_rectangle_and_no_byte_past_it() {
        let Some(device) = MTLCreateSystemDefaultDevice() else {
            eprintln!("no Metal device: not tried");
            return;
        };
        // A frame 8 x 4 whose rows are 48 bytes apart, placed so that its last pixel is the last
        // readable byte: the page after it faults on any read.
        let (w, h, stride) = (8u32, 4u32, 48usize);
        let need = (h as usize - 1) * stride + w as usize * 4;
        let page = unsafe { libc::sysconf(libc::_SC_PAGESIZE) } as usize;
        let base = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                2 * page,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_ANON | libc::MAP_PRIVATE,
                -1,
                0,
            )
        };
        assert_ne!(base, libc::MAP_FAILED);
        assert_eq!(
            unsafe { libc::mprotect(base.add(page), page, libc::PROT_NONE) },
            0
        );
        let frame = unsafe { (base as *mut u8).add(page - need) };
        // That a read of the very next byte does fault, shown by a child that makes one and dies of it.
        let child = unsafe { libc::fork() };
        assert!(child >= 0);
        if child == 0 {
            let byte = unsafe { std::ptr::read_volatile(frame.add(need)) };
            unsafe { libc::_exit(byte as i32 & 1) };
        }
        let mut status = 0;
        assert_eq!(unsafe { libc::waitpid(child, &mut status, 0) }, child);
        assert!(
            libc::WIFSIGNALED(status),
            "the byte past the frame was readable"
        );
        let paint = |add: u8| {
            let bytes = unsafe { std::slice::from_raw_parts_mut(frame, need) };
            bytes.fill(0xee); // what lies between the rows
            for y in 0..h as usize {
                for x in 0..w as usize {
                    let px = [x as u8 + add, y as u8 + add, (x ^ y) as u8, 255];
                    bytes[y * stride + x * 4..][..4].copy_from_slice(&px);
                }
            }
        };
        let want = |x: usize, y: usize, add: u8| [x as u8 + add, y as u8 + add, (x ^ y) as u8, 255];
        let tex = texture(&device, w, h, false);
        let all = Rect { x: 0, y: 0, w, h };
        paint(0);
        assert!(unsafe { replace(&tex, frame, need, stride, all) });
        let got = read(&tex, w, h);
        for y in 0..h as usize {
            for x in 0..w as usize {
                assert_eq!(
                    got[(y * w as usize + x) * 4..][..4],
                    want(x, y, 0),
                    "({x}, {y})"
                );
            }
        }
        // A rectangle that ends on the frame's last pixel: only it changes.
        let part = Rect {
            x: 5,
            y: 2,
            w: 3,
            h: 2,
        };
        paint(100);
        assert!(unsafe { replace(&tex, frame, need, stride, part) });
        let got = read(&tex, w, h);
        for y in 0..h as usize {
            for x in 0..w as usize {
                let add = if x >= 5 && y >= 2 { 100 } else { 0 };
                assert_eq!(
                    got[(y * w as usize + x) * 4..][..4],
                    want(x, y, add),
                    "({x}, {y})"
                );
            }
        }
        // A byte short of the last pixel, or no pixels at all: refused, and nothing is read.
        assert!(!unsafe { replace(&tex, frame, need - 1, stride, all) });
        assert!(!unsafe { replace(&tex, frame, need - 1, stride, part) });
        assert!(!unsafe {
            replace(
                &tex,
                frame,
                need,
                stride,
                Rect {
                    x: 0,
                    y: 0,
                    w: 0,
                    h: 1,
                },
            )
        });
        assert!(unsafe {
            replace(
                &tex,
                frame,
                need - 1,
                stride,
                Rect {
                    x: 0,
                    y: 0,
                    w: 7,
                    h: 4,
                },
            )
        });
        unsafe { libc::munmap(base, 2 * page) };
    }

    #[test]
    fn the_picture_is_drawn_where_the_pointer_is_mapped() {
        let Ok(mut r) = Renderer::new() else {
            eprintln!("no Metal device: not tried");
            return;
        };
        // A scanout 4 x 2, every pixel its own colour (B, G, R, A).
        let (tw, th) = (4u32, 2u32);
        let colour = |x: u32, y: u32| {
            [
                10 + 60 * x as u8,
                20 + 100 * y as u8,
                200 - 40 * x as u8,
                255u8,
            ]
        };
        let mut src = Vec::new();
        for y in 0..th {
            for x in 0..tw {
                src.extend_from_slice(&colour(x, y));
            }
        }
        let tex = texture(&r.device, tw, th, false);
        let all = Rect {
            x: 0,
            y: 0,
            w: tw,
            h: th,
        };
        assert!(unsafe { replace(&tex, src.as_ptr(), src.len(), tw as usize * 4, all) });
        r.texture = Some(tex);
        r.tex_size = (tw, th);
        // Bars above and below, bars at the sides, no bars; then a size nothing divides.
        for (dw, dh, exact) in [
            (400u32, 300u32, true),
            (300, 400, true),
            (400, 200, true),
            (37, 91, false),
        ] {
            let target = texture(&r.device, dw, dh, true);
            let cmd = r.queue.commandBuffer().unwrap();
            r.draw(&cmd, &target, (dw, dh));
            cmd.commit();
            cmd.waitUntilCompleted();
            let got = read(&target, dw, dh);
            let (x0, y0, vw, vh, scale) = fit(tw as f64, th as f64, dw as f64, dh as f64).unwrap();
            let mut inside = 0;
            for py in 0..dh {
                for px in 0..dw {
                    // The guest pixel under the middle of this one, as the pointer is mapped.
                    let (gx, gy) = (
                        (px as f64 + 0.5 - x0) / scale,
                        (py as f64 + 0.5 - y0) / scale,
                    );
                    let at = &got[(py as usize * dw as usize + px as usize) * 4..][..4];
                    let over = gx >= 0.0 && gy >= 0.0 && gx < tw as f64 && gy < th as f64;
                    // Away from an edge by more than the one device pixel the drawing rounds to.
                    let clear = |g: f64, n: u32| g < -1.0 / scale || g > n as f64 + 1.0 / scale;
                    if exact && over {
                        inside += 1;
                        assert_eq!(
                            at,
                            colour(gx as u32, gy as u32),
                            "{dw}x{dh} at ({px}, {py})"
                        );
                    } else if exact || clear(gx, tw) || clear(gy, th) {
                        assert_eq!(at, [0, 0, 0, 255], "{dw}x{dh} bar at ({px}, {py})");
                    }
                }
            }
            if exact {
                assert_eq!(inside, (vw * vh) as u32, "{dw}x{dh}");
            } else {
                // The middle of each guest pixel shows that pixel, near enough.
                for y in 0..th {
                    for x in 0..tw {
                        let px = (x0 + (x as f64 + 0.5) * scale) as usize;
                        let py = (y0 + (y as f64 + 0.5) * scale) as usize;
                        let at = &got[(py * dw as usize + px) * 4..][..4];
                        for (a, b) in at.iter().zip(colour(x, y)) {
                            assert!(
                                (*a as i32 - b as i32).abs() <= 40,
                                "{dw}x{dh} guest ({x}, {y}): {at:?}"
                            );
                        }
                    }
                }
            }
        }
    }

    #[test]
    fn the_pointer_is_a_guest_pixel_to_a_pixel_up_to_a_size() {
        assert_eq!(pointer_scale(64, 64, 0.5), 0.5);
        assert_eq!(pointer_scale(64, 64, 4.0), 4.0);
        // A 16 x 16 guest screen filling the window: fifty points to a pixel, and not for the pointer.
        assert_eq!(pointer_scale(64, 64, 50.0), 4.0);
        assert_eq!(pointer_scale(256, 16, 3.0), 1.0);
        assert_eq!(pointer_scale(16, 2, 1000.0), 16.0);
    }

    #[test]
    fn the_colours_are_read_in_the_order_the_frame_has_them() {
        let Ok(mut r) = Renderer::new() else {
            eprintln!("no Metal device: not tried");
            return;
        };
        let colours = [(200u8, 100u8, 40u8), (10, 250, 60)];
        let junk = 0x77; // the byte that is alpha or nothing
        for order in [Order::Bgrx, Order::Rgbx, Order::Xrgb, Order::Xbgr] {
            let mut src = Vec::new();
            for (red, green, blue) in colours {
                src.extend_from_slice(&match order {
                    Order::Bgrx => [blue, green, red, junk],
                    Order::Rgbx => [red, green, blue, junk],
                    Order::Xrgb => [junk, red, green, blue],
                    Order::Xbgr => [junk, blue, green, red],
                });
            }
            let tex = r.make_texture(2, 1, order).unwrap();
            let all = Rect {
                x: 0,
                y: 0,
                w: 2,
                h: 1,
            };
            assert!(unsafe { replace(&tex, src.as_ptr(), src.len(), 8, all) });
            r.texture = Some(tex);
            r.tex_size = (2, 1);
            let target = texture(&r.device, 2, 1, true);
            let cmd = r.queue.commandBuffer().unwrap();
            r.draw(&cmd, &target, (2, 1));
            cmd.commit();
            cmd.waitUntilCompleted();
            let got = read(&target, 2, 1);
            for (i, (red, green, blue)) in colours.into_iter().enumerate() {
                assert_eq!(
                    got[i * 4..][..4],
                    [blue, green, red, 255],
                    "{order:?}, pixel {i}"
                );
            }
        }
    }

    #[test]
    fn a_scanout_set_while_a_texture_is_made_leaves_the_glass_alone() {
        let Ok(mut r) = Renderer::new() else {
            eprintln!("no Metal device: not tried");
            return;
        };
        let (display, _sent) = Display::detached();
        let mut stats = Stats::default();
        let frame = |w: u32, h: u32, v: u8| vec![v; (w * h * 4) as usize];
        display.set_owned(4, 2, Order::Bgrx, frame(4, 2, 0x11));
        assert!(r.upload(&display, &mut stats));
        assert_eq!((r.tex_size, r.serial), ((4, 2), 1));
        // A larger scanout arrives, and while its texture is made the guest sets a third.
        display.set_owned(8, 4, Order::Bgrx, frame(8, 4, 0x22));
        let changed = r.upload_with(&display, &mut stats, |r, w, h, order| {
            assert_eq!((w, h), (8, 4));
            display.set_owned(2, 2, Order::Bgrx, frame(2, 2, 0x33));
            display.dirty.store(false, Ordering::Release);
            r.make_texture(w, h, order)
        });
        assert!(!changed);
        assert!(
            display.dirty.load(Ordering::Acquire),
            "the next frame must look again"
        );
        // What is on the glass is still the first, and so is the size the pointer is placed by.
        assert_eq!((r.tex_size, r.serial), ((4, 2), 1));
        assert_eq!(read(r.texture.as_ref().unwrap(), 4, 2), frame(4, 2, 0x11));
        // The next look shows the third.
        assert!(r.upload(&display, &mut stats));
        assert_eq!((r.tex_size, r.serial), ((2, 2), 3));
        assert_eq!(read(r.texture.as_ref().unwrap(), 2, 2), frame(2, 2, 0x33));
        // A texture that cannot be made takes the picture away, once.
        display.set_owned(8, 4, Order::Bgrx, frame(8, 4, 0x44));
        assert!(r.upload_with(&display, &mut stats, |_, _, _, _| None));
        assert!(r.texture.is_none());
        assert_eq!(r.tex_size, (0, 0));
        assert!(!r.upload_with(&display, &mut stats, |_, _, _, _| None));
        assert!(r.upload(&display, &mut stats));
        assert_eq!((r.tex_size, r.serial), ((8, 4), 4));
    }

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
