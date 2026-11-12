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
    NSApplication, NSBitmapFormat, NSBitmapImageRep, NSControlStateValueOff, NSControlStateValueOn,
    NSCursor, NSDeviceRGBColorSpace, NSEvent, NSEventModifierFlags, NSEventType, NSGraphicsContext,
    NSImage, NSMenuDidBeginTrackingNotification, NSMenuItem, NSResponder, NSScreen, NSView,
    NSWindowDidResignKeyNotification,
};
use objc2_core_foundation::{CGPoint, CGSize};
use objc2_core_graphics::{
    kCGColorSpaceSRGB, CGAssociateMouseAndMouseCursorPosition, CGColorSpace, CGError, CGEvent,
    CGEventField, CGEventFlags, CGEventType, CGMouseButton, CGScrollEventUnit,
    CGWarpMouseCursorPosition,
};
use objc2_foundation::{
    ns_string, MainThreadMarker, NSNotificationCenter, NSObject, NSObjectProtocol, NSPoint,
    NSProcessInfo, NSRect, NSRunLoop, NSRunLoopCommonModes, NSSize, NSString, NSTimer,
};
use objc2_metal::{
    MTLBlendFactor, MTLClearColor, MTLCommandBuffer, MTLCommandEncoder, MTLCommandQueue,
    MTLCreateSystemDefaultDevice, MTLDevice, MTLLibrary, MTLLoadAction, MTLOrigin, MTLPixelFormat,
    MTLPrimitiveType, MTLRegion, MTLRenderCommandEncoder, MTLRenderPassDescriptor,
    MTLRenderPipelineDescriptor, MTLRenderPipelineState, MTLSamplerDescriptor,
    MTLSamplerMinMagFilter, MTLSamplerState, MTLScissorRect, MTLSize, MTLStoreAction, MTLTexture,
    MTLTextureDescriptor, MTLTextureSwizzle, MTLTextureSwizzleChannels, MTLTextureUsage,
    MTLViewport,
};
use objc2_quartz_core::{CADisplayLink, CALayer, CAMetalDrawable, CAMetalLayer};

use crate::display::{pixels, Display, Input, Order, Pixels, Place, Pointer, Rect, UiInfo};
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
fragment float4 fp(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, in.uv);
}
"#;

struct Renderer {
    device: Retained<ProtocolObject<dyn MTLDevice>>,
    queue: Retained<ProtocolObject<dyn MTLCommandQueue>>,
    pipeline: Retained<ProtocolObject<dyn MTLRenderPipelineState>>,
    over: Retained<ProtocolObject<dyn MTLRenderPipelineState>>, // lays the guest's pointer on the picture
    pointer: Option<Drawn>,
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

// The guest's pointer as the picture's pass draws it, which it does while the Mac's is held: the
// guest moves its own then, and the Mac's shows nothing of where.
struct Drawn {
    image: Arc<Pointer>,
    texture: Texture,
    at: (i32, i32), // the guest pixel the hot spot is on
}

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

// How long the view's size must have stood before the guest is told it, in seconds. Each size told
// is a mode the guest sets.
const SETTLE: f64 = 0.25;

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

// Where a tw x th picture goes in an area of w x h pixels that shows all of it, centred:
// (x, y, width, height, scale), from the top left, a rectangle of whole pixels inside the area. The
// drawing and the pointer both take the picture's place from here, and stretch the picture over
// that rectangle; the scale is the one the picture was placed for.
//
// A guest that cannot make a mode of the view's size makes one of that size over a whole number and
// drops the remainder, so the view is that many times its picture and a few pixels more. Filling the
// view would filter every pixel of the picture to make it at most one of its own pixels larger. So
// where the whole scale below the filling one comes that close, the picture is drawn at it, on the
// area's pixels, and what is left over is a bar.
fn fit(tw: f64, th: f64, w: f64, h: f64) -> Option<(f64, f64, f64, f64, f64)> {
    if tw <= 0.0 || th <= 0.0 || w <= 0.0 || h <= 0.0 {
        return None;
    }
    let fill = (w / tw).min(h / th);
    let whole = fill.floor();
    // (fill - whole) * t <= whole, t the picture's longer side, as products: whole numbers of
    // pixels are compared exactly.
    let t = tw.max(th);
    let near = w * t <= whole * (t + 1.0) * tw || h * t <= whole * (t + 1.0) * th;
    let (vw, vh, scale) = if whole >= 1.0 && near {
        (tw * whole, th * whole, whole)
    } else if w * th <= h * tw {
        // Filled: the side that bounds the scale takes all of the area's, exactly, and the other
        // the nearest whole number of pixels, which is never more than the area has.
        (w, (th * w / tw).round().max(1.0), fill)
    } else {
        ((tw * h / th).round().max(1.0), h, fill)
    };
    let (x, y) = (((w - vw) / 2.0).floor(), ((h - vh) / 2.0).floor());
    Some((x, y, vw, vh, scale))
}

// The picture's place in a view of bw x bh points at `s` pixels to a point, as AppKit counts:
// (x, y, width, height) in points from the bottom left, and the points a guest pixel takes. It is
// found in the drawable's pixels, as the drawing finds it, so that a bar one pixel high is a bar to
// the pointer too.
fn place(tw: u32, th: u32, bw: f64, bh: f64, s: f64) -> Option<(f64, f64, f64, f64, f64)> {
    if s <= 0.0 {
        return None;
    }
    let (w, h) = (pixels(bw, s) as f64, pixels(bh, s) as f64);
    let (x, y, vw, vh, scale) = fit(tw as f64, th as f64, w, h)?;
    Some((x / s, (h - y - vh) / s, vw / s, vh / s, scale / s))
}

// The scanout pixel under (px, py) of a view, which counts from its bottom left, where the picture
// of a tw x th scanout has the rectangle `r` (`place`'s): None over a bar. The drawing stretches the
// picture over that rectangle, each way by its own amount, and so does this.
fn guest_pixel(px: f64, py: f64, r: (f64, f64, f64, f64), tw: u32, th: u32) -> Option<(u32, u32)> {
    let (x, y, w, h) = r;
    // The guest counts from the top left.
    let (dx, dy) = (px - x, y + h - py);
    if tw == 0 || th == 0 || !(dx >= 0.0 && dx < w && dy >= 0.0 && dy < h) {
        return None;
    }
    let at = |d: f64, side: f64, n: u32| ((d * n as f64 / side) as u32).min(n - 1);
    Some((at(dx, w, tw), at(dy, h, th)))
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

pub const TITLE: &str = "Thylacine";
// While the pointer is captured the window says how to get it back.
const CAPTURED_TITLE: &str = "Thylacine \u{2014} \u{2303}\u{2318}G gives the pointer back";

// The whole guest pixels in a move of `points`, where a guest pixel takes `scale` points of the
// view. What is left over is kept in `carry` for the next move, so that slow motion is not lost.
fn guest_motion(carry: &mut f64, points: f64, scale: f64) -> i32 {
    if scale <= 0.0 || !scale.is_finite() || !points.is_finite() {
        return 0;
    }
    let all = *carry + points / scale;
    let whole = all.trunc().clamp(i32::MIN as f64, i32::MAX as f64);
    // More than a number holds is not carried over.
    *carry = if whole == all.trunc() {
        all - whole
    } else {
        0.0
    };
    whole as i32
}
// Now, on the clock an event is stamped with: seconds since the Mac started.
fn uptime() -> f64 {
    NSProcessInfo::processInfo().systemUptime()
}
// How long before a change of hands an event can have been made and still be waiting to be taken.
const STALE_WITHIN: f64 = 5.0;
// Stamp an event Core Graphics made for a script. The system stamps an event when it is posted,
// and one that is only made says 0.
fn stamp(cg: &CGEvent) {
    CGEvent::set_timestamp(Some(cg), (uptime() * 1e9) as u64);
}
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
        let fp = lib
            .newFunctionWithName(ns_string!("fp"))
            .ok_or("no pointer function")?;
        let desc = MTLRenderPipelineDescriptor::new();
        desc.setVertexFunction(Some(&vs));
        desc.setFragmentFunction(Some(&fp));
        let att = unsafe { desc.colorAttachments().objectAtIndexedSubscript(0) };
        att.setPixelFormat(MTLPixelFormat::BGRA8Unorm);
        // The image's colours are already multiplied by its alpha.
        att.setBlendingEnabled(true);
        att.setSourceRGBBlendFactor(MTLBlendFactor::One);
        att.setSourceAlphaBlendFactor(MTLBlendFactor::One);
        att.setDestinationRGBBlendFactor(MTLBlendFactor::OneMinusSourceAlpha);
        att.setDestinationAlphaBlendFactor(MTLBlendFactor::OneMinusSourceAlpha);
        let over = device
            .newRenderPipelineStateWithDescriptor_error(&desc)
            .map_err(|e| e.to_string())?;
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
            over,
            pointer: None,
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

    // What the next frame draws of the guest's pointer: nothing, or `image` with its hot spot on
    // the guest pixel `at`. Returns whether that changes the frame.
    fn set_pointer(&mut self, want: Option<(&Arc<Pointer>, (i32, i32))>) -> bool {
        let Some((image, at)) = want else {
            return self.pointer.take().is_some();
        };
        if let Some(drawn) = &mut self.pointer {
            if Arc::ptr_eq(&drawn.image, image) {
                let moved = drawn.at != at;
                drawn.at = at;
                return moved;
            }
        }
        let d = unsafe {
            MTLTextureDescriptor::texture2DDescriptorWithPixelFormat_width_height_mipmapped(
                MTLPixelFormat::BGRA8Unorm,
                image.w as usize,
                image.h as usize,
                false,
            )
        };
        let bgra = image.cut();
        let all = Rect {
            x: 0,
            y: 0,
            w: image.w,
            h: image.h,
        };
        let texture = self.device.newTextureWithDescriptor(&d).filter(|t| {
            // SAFETY: a Pointer holds w * h * 4 bytes, rows w * 4 apart, and `all` is the texture.
            unsafe { replace(t, bgra.as_ptr(), bgra.len(), image.w as usize * 4, all) }
        });
        let had = self.pointer.take().is_some();
        self.pointer = texture.map(|texture| Drawn {
            image: image.clone(),
            texture,
            at,
        });
        had || self.pointer.is_some()
    }

    // One pass: clear to black, then the scanout letterboxed into a target of `size` pixels, and
    // the guest's pointer on it if it is drawn here.
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
                enc.setViewport(MTLViewport {
                    originX: x,
                    originY: y,
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
                if let Some(p) = &self.pointer {
                    // An image pixel on a guest pixel, the hot spot on the guest's place. What
                    // falls off the picture is not drawn: the bars are not the guest's.
                    let within = |v: f64, most: u32| v.round().clamp(0.0, most as f64) as usize;
                    let (left, top) = (within(x, size.0), within(y, size.1));
                    enc.setScissorRect(MTLScissorRect {
                        x: left,
                        y: top,
                        width: within(x + vw, size.0) - left,
                        height: within(y + vh, size.1) - top,
                    });
                    enc.setViewport(MTLViewport {
                        originX: x + (p.at.0 as f64 - p.image.hot_x as f64) * scale,
                        originY: y + (p.at.1 as f64 - p.image.hot_y as f64) * scale,
                        width: p.image.w as f64 * scale,
                        height: p.image.h as f64 * scale,
                        znear: 0.0,
                        zfar: 1.0,
                    });
                    enc.setRenderPipelineState(&self.over);
                    unsafe {
                        enc.setFragmentTexture_atIndex(Some(&p.texture), 0);
                        enc.setFragmentSamplerState_atIndex(
                            Some(if whole { &self.nearest } else { &self.linear }),
                            0,
                        );
                        enc.drawPrimitives_vertexStart_vertexCount(
                            MTLPrimitiveType::Triangle,
                            0,
                            3,
                        );
                    }
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
    captured: Cell<bool>, // the pointer is captured: its moves are the guest's mouse's, and the guest's pointer is drawn
    seized: Cell<bool>, // the Mac's pointer is held and hidden (a captured pointer's is, in a window that was key)
    skip: Cell<bool>,   // the next move has in it the jump the Mac's pointer was just made to take
    turned: Cell<f64>,  // when the pointer last changed hands, on the clock events are stamped with
    carry: Cell<(f64, f64)>, // the part of a guest pixel of a captured pointer's motion not sent yet
    script_step: Cell<u8>,   // how many steps of the paced capture script have run
    worn: RefCell<Worn>,
    told: Cell<Option<UiInfo>>, // the size the guest's display was last told
    tellings: Cell<u32>,
    settling: RefCell<Option<Retained<NSTimer>>>, // runs out SETTLE after the size last changed
}

// What a test asks of the window; a user's run leaves all of it off.
#[derive(Clone, Default)]
pub struct Options {
    pub print_stats: bool,
    pub exit_after: Option<f64>,                               // seconds
    pub dump: Option<String>, // a PPM of the pipeline's output, written at exit_after
    pub scripted_input: bool, // one second in, a fixed run of key and pointer events through the window
    pub resize_to: Option<(f64, f64)>, // one second in, the window's content set to this many points, and nudged a second later
    pub scripted_bar: bool, // one second in, the content set a point past the scanout each way; then clicks on the picture's corners and on the bars
    pub scripted_capture: bool, // one second in, the pointer captured (the Mac's is left alone: the window is not key), moved, clicked, scrolled and let go
    pub scripted_click: Option<(u32, u32)>, // one second in, the pointer to the middle of the picture; half a second on, a left click on this guest pixel; half a second on, the pointer to the last pixel
    pub scripted_captured_click: Option<(u32, u32, i32, i32)>, // one second in, the pointer to this guest pixel; half a second on, captured and moved this many guest pixels; half a second on, a left click
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
            self.wear_pointer();
            self.frame();
        }

        #[unsafe(method(resetCursorRects))]
        fn reset_cursor_rects(&self) {
            let Ok(worn) = self.ivars().worn.try_borrow() else { return };
            if let (Some(cursor), Some((rect, _, _))) = (&worn.cursor, self.picture()) {
                self.addCursorRect_cursor(rect, cursor);
            }
        }

        // A display link is silent while the display sleeps, so a test's deadline has a timer of its own.
        #[unsafe(method(deadline:))]
        fn deadline(&self, _timer: &NSTimer) { self.finish(); }

        #[unsafe(method(scripted:))]
        fn scripted(&self, _timer: &NSTimer) { self.scripted_input(); }

        #[unsafe(method(resize:))]
        fn resize(&self, _timer: &NSTimer) { self.scripted_resize(); }

        #[unsafe(method(resizeBack:))]
        fn resize_back(&self, _timer: &NSTimer) { self.scripted_resize_back(); }

        #[unsafe(method(barSize:))]
        fn bar_size(&self, _timer: &NSTimer) { self.scripted_bar_size(); }

        #[unsafe(method(barClicks:))]
        fn bar_clicks(&self, _timer: &NSTimer) { self.scripted_bar_clicks(); }

        #[unsafe(method(clickMiddle:))]
        fn click_middle(&self, _timer: &NSTimer) { self.scripted_click(0); }

        #[unsafe(method(clickPress:))]
        fn click_press(&self, _timer: &NSTimer) { self.scripted_click(1); }

        #[unsafe(method(clickLast:))]
        fn click_last(&self, _timer: &NSTimer) { self.scripted_click(2); }

        #[unsafe(method(captureScript:))]
        fn capture_script(&self, _timer: &NSTimer) { self.scripted_capture(); }
        #[unsafe(method(capturedClick:))]
        fn captured_click(&self, _timer: &NSTimer) { self.scripted_captured_click(); }

        // View > Capture Pointer, and its chord.
        #[unsafe(method(toggleCapture:))]
        fn toggle_capture(&self, _sender: Option<&AnyObject>) {
            self.capture(!self.ivars().captured.get());
        }

        #[unsafe(method(validateMenuItem:))]
        fn validate_menu_item(&self, item: &NSMenuItem) -> bool {
            if item.action() == Some(sel!(toggleCapture:)) {
                let on = self.ivars().captured.get();
                item.setState(if on { NSControlStateValueOn } else { NSControlStateValueOff });
            }
            true
        }

        // Every change of the view's size comes through here, a step at a time while the window's
        // edge is dragged.
        #[unsafe(method(setFrameSize:))]
        fn set_frame_size(&self, size: NSSize) {
            let _: () = unsafe { msg_send![super(self), setFrameSize: size] };
            self.size_changed();
        }

        // The window's pixels to a point may have changed: it is on another screen.
        #[unsafe(method(viewDidChangeBackingProperties))]
        fn backing_changed(&self) {
            let _: () = unsafe { msg_send![super(self), viewDidChangeBackingProperties] };
            self.size_changed();
        }

        #[unsafe(method(settled:))]
        fn settled(&self, _timer: &NSTimer) {
            self.ivars().settling.borrow_mut().take();
            self.tell_size();
        }

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
            // A captured pointer is the guest's own, and a notch lands where the guest has it.
            let at = if self.ivars().captured.get() || self.stale(ev) {
                None
            } else {
                let Some(at) = self.guest_point(ev) else { return };
                Some(at)
            };
            let mut acc = self.ivars().scroll.get();
            let n = notches(&mut acc, ev.scrollingDeltaY(), ev.hasPreciseScrollingDeltas());
            self.ivars().scroll.set(acc);
            // A window that is not active is still scrolled, and is sent no moves: the guest's
            // pointer may be anywhere, so each notch goes with the place it was made over.
            for _ in 0..n.abs() { self.send(Input::Wheel { up: n > 0, at }); }
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
            captured: Cell::new(false),
            seized: Cell::new(false),
            skip: Cell::new(false),
            turned: Cell::new(0.0),
            carry: Cell::new((0.0, 0.0)),
            script_step: Cell::new(0),
            worn: RefCell::new(Worn::default()),
            told: Cell::new(None),
            tellings: Cell::new(0),
            settling: RefCell::new(None),
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
        if this.ivars().opts.resize_to.is_some() {
            unsafe {
                NSTimer::scheduledTimerWithTimeInterval_target_selector_userInfo_repeats(
                    1.0,
                    &*(Retained::as_ptr(&this) as *const AnyObject),
                    sel!(resize:),
                    None,
                    false,
                );
            }
        }
        if this.ivars().opts.scripted_bar {
            unsafe {
                NSTimer::scheduledTimerWithTimeInterval_target_selector_userInfo_repeats(
                    1.0,
                    &*(Retained::as_ptr(&this) as *const AnyObject),
                    sel!(barSize:),
                    None,
                    false,
                );
            }
        }
        if this.ivars().opts.scripted_capture {
            unsafe {
                NSTimer::scheduledTimerWithTimeInterval_target_selector_userInfo_repeats(
                    1.0,
                    &*(Retained::as_ptr(&this) as *const AnyObject),
                    sel!(captureScript:),
                    None,
                    false,
                );
            }
        }
        if this.ivars().opts.scripted_captured_click.is_some() {
            for secs in [1.0, 1.5, 2.0] {
                unsafe {
                    NSTimer::scheduledTimerWithTimeInterval_target_selector_userInfo_repeats(
                        secs,
                        &*(Retained::as_ptr(&this) as *const AnyObject),
                        sel!(capturedClick:),
                        None,
                        false,
                    );
                }
            }
        }
        if this.ivars().opts.scripted_click.is_some() {
            for (secs, step) in [
                (1.0, sel!(clickMiddle:)),
                (1.5, sel!(clickPress:)),
                (2.0, sel!(clickLast:)),
            ] {
                unsafe {
                    NSTimer::scheduledTimerWithTimeInterval_target_selector_userInfo_repeats(
                        secs,
                        &*(Retained::as_ptr(&this) as *const AnyObject),
                        step,
                        None,
                        false,
                    );
                }
            }
        }
        Ok(this)
    }

    // The view's size changed, and may be about to again. The guest is told the size that is still
    // standing SETTLE later. The timer is one of the run loop's ordinary mode, which is not the
    // mode a drag of the window's edge runs in.
    fn size_changed(&self) {
        let iv = self.ivars();
        if let Some(waiting) = iv.settling.borrow_mut().take() {
            waiting.invalidate();
        }
        let timer = unsafe {
            NSTimer::scheduledTimerWithTimeInterval_target_selector_userInfo_repeats(
                SETTLE,
                &*(self as *const Self as *const AnyObject),
                sel!(settled:),
                None,
                false,
            )
        };
        *iv.settling.borrow_mut() = Some(timer);
    }

    // Tell the guest's display the view's size, unless that is what it was last told. QEMU answers
    // the guest's next question about its display with it; a guest that has not booted yet boots
    // at it.
    pub fn tell_size(&self) {
        let Some(window) = self.window() else { return };
        let b = self.bounds();
        let scale = window.backingScaleFactor();
        let Some(size) = UiInfo::of(b.size.width, b.size.height, scale) else {
            return;
        };
        let iv = self.ivars();
        if iv.told.get() == Some(size) {
            return;
        }
        iv.told.set(Some(size));
        iv.tellings.set(iv.tellings.get() + 1);
        self.send(Input::Size(size));
    }

    // The window's content set to three sizes in one turn of the run loop, the last the one asked
    // for: what a resize is to the view, without a hand on the window's edge.
    fn scripted_resize(&self) {
        let (Some(window), Some((w, h))) = (self.window(), self.ivars().opts.resize_to) else {
            return;
        };
        for (dw, dh) in [(120.0, 40.0), (-60.0, 20.0), (0.0, 0.0)] {
            window.setContentSize(NSSize::new(w + dw, h + dh));
        }
        let b = self.bounds();
        eprintln!(
            "scripted: content set to {w}x{h} points; the view is {}x{}",
            b.size.width, b.size.height
        );
        unsafe {
            NSTimer::scheduledTimerWithTimeInterval_target_selector_userInfo_repeats(
                1.0,
                &*(self as *const Self as *const AnyObject),
                sel!(resizeBack:),
                None,
                false,
            );
        }
    }

    // A second later, when that size has been told: the content a point wider and back in one
    // turn of the run loop. The view's size changed and is what it was, so nothing is told.
    fn scripted_resize_back(&self) {
        let (Some(window), Some((w, h))) = (self.window(), self.ivars().opts.resize_to) else {
            return;
        };
        let before = self.ivars().tellings.get();
        window.setContentSize(NSSize::new(w + 1.0, h));
        window.setContentSize(NSSize::new(w, h));
        eprintln!("scripted: content set a point wider and back, after {before} tellings");
    }

    // The window's content set one point larger each way than the scanout has pixels. With a whole
    // number of pixels to a point the view is then that many times the picture and one of the
    // picture's own pixels more: the whole-scale rule's bound, which is more than a guest's fitted
    // mode leaves (that is under one of its pixels).
    fn scripted_bar_size(&self) {
        let iv = self.ivars();
        if let Some(r) = iv.renderer.borrow_mut().as_mut() {
            r.upload(&iv.display, &mut iv.stats.borrow_mut());
        }
        let (tw, th) = iv
            .renderer
            .borrow()
            .as_ref()
            .map(|r| r.tex_size)
            .unwrap_or((0, 0));
        let Some(window) = self.window() else {
            eprintln!("scripted: no window");
            return;
        };
        if tw == 0 || th == 0 {
            eprintln!("scripted: no scanout, the bar is not tried");
            return;
        }
        window.setContentSize(NSSize::new(tw as f64 + 1.0, th as f64 + 1.0));
        unsafe {
            NSTimer::scheduledTimerWithTimeInterval_target_selector_userInfo_repeats(
                0.5,
                &*(self as *const Self as *const AnyObject),
                sel!(barClicks:),
                None,
                false,
            );
        }
    }

    // Right clicks on the bars and on the picture's first and last pixels beside them. The places
    // are worked out here from the rule alone (a guest pixel is a point; of the remainder, the half
    // rounded down is on the left and above), not through `place`: the caller reads QEMU's trace.
    fn scripted_bar_clicks(&self) {
        let Some(window) = self.window() else { return };
        let (tw, th) = self
            .ivars()
            .renderer
            .borrow()
            .as_ref()
            .map(|r| r.tex_size)
            .unwrap_or((0, 0));
        let s = window.backingScaleFactor();
        let b = self.bounds();
        let lead = (s / 2.0).floor() / s;
        let wn = window.windowNumber();
        let click = |x: f64, y_top: f64| {
            let p = self.convertPoint_toView(NSPoint::new(x, b.size.height - y_top), None);
            for ty in [NSEventType::RightMouseDown, NSEventType::RightMouseUp] {
                let ev = NSEvent::mouseEventWithType_location_modifierFlags_timestamp_windowNumber_context_eventNumber_clickCount_pressure(
                    ty, p, NSEventModifierFlags::empty(), 0.0, wn, None, 0, 1, 1.0);
                match ev {
                    Some(ev) => window.sendEvent(&ev),
                    None => eprintln!("scripted: no mouse event"),
                }
            }
        };
        click(b.size.width - 0.25, b.size.height / 2.0); // the bar on the right
        click(lead + 0.5, lead + (th / 2) as f64 + 0.5); // the first pixel of the middle row
        click(lead + tw as f64 - 0.5, lead + th as f64 - 0.5); // the last pixel of the last row
        click(b.size.width / 2.0, b.size.height - 0.25); // the bar below
        eprintln!(
            "scripted: bar: view {}x{} points at {s}, scanout {tw}x{th}",
            b.size.width, b.size.height
        );
    }

    // The pointer script for a guest that is running, a step at a time and half a second apart: a
    // compositor says where its pointer is once a frame, and the caller wants each place it was
    // sent to. The middle of the picture, a left click on the pixel asked for, the last pixel. The
    // events are handed to the view itself, because a window that is not active keeps a left click
    // for its own activation and is given no moves. A step aims at the middle of its pixel where
    // the view has the picture now, and is said as done only if the view sent that pixel: what a
    // step sent instead is said as that.
    fn scripted_click(&self, step: u8) {
        let iv = self.ivars();
        let (Some(window), Some((cx, cy))) = (self.window(), iv.opts.scripted_click) else {
            return;
        };
        // The script does not wait for a frame to bring the picture: with the Mac's display
        // asleep none comes.
        if let Some(r) = iv.renderer.borrow_mut().as_mut() {
            r.upload(&iv.display, &mut iv.stats.borrow_mut());
        }
        let Some((rect, _, (tw, th))) = self.picture() else {
            eprintln!("scripted: no scanout, the pointer is not tried");
            return;
        };
        let (what, gx, gy) = match step {
            0 => ("to the middle", tw / 2, th / 2),
            1 => ("a left click", cx.min(tw - 1), cy.min(th - 1)),
            _ => ("to the last pixel", tw - 1, th - 1),
        };
        let meant = Place {
            x: gx,
            y: gy,
            w: tw,
            h: th,
        };
        let x = rect.origin.x + (gx as f64 + 0.5) * rect.size.width / tw as f64;
        let y = rect.origin.y + rect.size.height - (gy as f64 + 0.5) * rect.size.height / th as f64;
        let p = self.convertPoint_toView(NSPoint::new(x, y), None);
        let wn = window.windowNumber();
        // What the view sent for one event: None if nothing, or else the place it gave it.
        let sent = |ty: NSEventType, button: Option<bool>| {
            let ev = NSEvent::mouseEventWithType_location_modifierFlags_timestamp_windowNumber_context_eventNumber_clickCount_pressure(
                ty, p, NSEventModifierFlags::empty(), 0.0, wn, None, 0, 1, 1.0)?;
            let at = self.guest_point(&ev);
            match button {
                Some(down) => self.button(&ev, button_code(0), down),
                None => self.pointer(&ev),
            }
            .then_some(at)
        };
        let all: Vec<Option<Option<Place>>> = if step == 1 {
            vec![
                sent(NSEventType::LeftMouseDown, Some(true)),
                sent(NSEventType::LeftMouseUp, Some(false)),
            ]
        } else {
            vec![sent(NSEventType::MouseMoved, None)]
        };
        match all.iter().find(|&&s| s != Some(Some(meant))) {
            None => eprintln!("scripted: pointer {what}, guest pixel {gx},{gy} of {tw}x{th}"),
            Some(other) => {
                let other = match other {
                    Some(Some(at)) => format!("{},{} of {}x{}", at.x, at.y, at.w, at.h),
                    Some(None) => "an event with no place".into(),
                    None => "nothing".into(),
                };
                eprintln!("scripted: pointer {what}: meant {gx},{gy} of {tw}x{th}, sent {other}");
            }
        }
    }

    // A move of the Mac's pointer as Core Graphics makes one, with how far the pointer went in
    // the event, stamped when it is made.
    fn made_move(&self, dx: i64, dy: i64) -> Option<Retained<NSEvent>> {
        let cg = CGEvent::new_mouse_event(
            None,
            CGEventType::MouseMoved,
            CGPoint::new(0.0, 0.0),
            CGMouseButton::Left,
        );
        if let Some(cg) = &cg {
            CGEvent::set_integer_value_field(Some(cg), CGEventField::MouseEventDeltaX, dx);
            CGEvent::set_integer_value_field(Some(cg), CGEventField::MouseEventDeltaY, dy);
            stamp(cg);
        }
        let ev = cg.and_then(|cg| NSEvent::eventWithCGEvent(&cg));
        if ev.is_none() {
            eprintln!("scripted: no motion event");
        }
        ev
    }

    // Such a move made now and handed to the view. Handed back: the motion the event says it
    // has, as AppKit reads it.
    fn scripted_move(&self, dx: i64, dy: i64) -> (f64, f64) {
        match self.made_move(dx, dy) {
            Some(ev) => {
                let _: () = unsafe { msg_send![self, mouseMoved: &*ev] };
                (ev.deltaX(), ev.deltaY())
            }
            None => (0.0, 0.0),
        }
    }

    // The captured pointer for a guest that is running, paced as the click script is and for
    // its reason. The pointer goes to a guest pixel, as the tablet's; half a second on it is
    // captured and moved a number of guest pixels; half a second on, a left click, which lands
    // wherever the guest then has its pointer. The window is not key, so the Mac's pointer is
    // left alone. The pointer stays captured: the picture a test takes at the end has the
    // guest's own pointer in it.
    fn scripted_captured_click(&self) {
        let iv = self.ivars();
        let step = iv.script_step.get();
        iv.script_step.set(step.saturating_add(1));
        let (Some(window), Some((gx, gy, dx, dy))) =
            (self.window(), iv.opts.scripted_captured_click)
        else {
            return;
        };
        if let Some(r) = iv.renderer.borrow_mut().as_mut() {
            r.upload(&iv.display, &mut iv.stats.borrow_mut());
        }
        let Some((rect, scale, (tw, th))) = self.picture() else {
            eprintln!("scripted: no scanout, the captured pointer is not tried");
            return;
        };
        let (gx, gy) = (gx.min(tw - 1), gy.min(th - 1));
        let x = rect.origin.x + (gx as f64 + 0.5) * rect.size.width / tw as f64;
        let y = rect.origin.y + rect.size.height - (gy as f64 + 0.5) * rect.size.height / th as f64;
        let p = self.convertPoint_toView(NSPoint::new(x, y), None);
        let wn = window.windowNumber();
        let event = |ty: NSEventType| {
            NSEvent::mouseEventWithType_location_modifierFlags_timestamp_windowNumber_context_eventNumber_clickCount_pressure(
                ty, p, NSEventModifierFlags::empty(), uptime(), wn, None, 0, 1, 1.0)
        };
        match step {
            0 => {
                let sent = event(NSEventType::MouseMoved).and_then(|ev| {
                    let at = self.guest_point(&ev);
                    self.pointer(&ev).then_some(at).flatten()
                });
                match sent {
                    Some(at) if (at.x, at.y) == (gx, gy) => eprintln!(
                        "scripted: captured click: the pointer to guest pixel {gx},{gy} of {tw}x{th}"
                    ),
                    Some(at) => eprintln!(
                        "scripted: captured click: meant {gx},{gy} of {tw}x{th}, sent {},{}",
                        at.x, at.y
                    ),
                    None => eprintln!(
                        "scripted: captured click: meant {gx},{gy} of {tw}x{th}, sent nothing"
                    ),
                }
            }
            1 => {
                self.capture(true);
                let held = iv.seized.get();
                // Whole points: an event's motion is a whole number of them.
                let by = self.scripted_move(
                    (dx as f64 * scale).round() as i64,
                    (dy as f64 * scale).round() as i64,
                );
                eprintln!(
                    "scripted: captured click: captured, the Mac's pointer held: {held}; a move of {},{} points at {scale} points to a guest pixel",
                    by.0, by.1
                );
            }
            _ => {
                let down = event(NSEventType::LeftMouseDown)
                    .is_some_and(|ev| self.button(&ev, button_code(0), true));
                let up = event(NSEventType::LeftMouseUp)
                    .is_some_and(|ev| self.button(&ev, button_code(0), false));
                eprintln!(
                    "scripted: captured click: a left click{}",
                    if down && up { "" } else { ", NOT SENT" }
                );
            }
        }
    }

    // The captured pointer as a person would use it, without taking the Mac's: the window is not
    // key, so the Mac's pointer is left alone. A move is made as Core Graphics makes one, with how
    // far the pointer went in the event, and handed to the view, as the buttons and the notch are.
    // A button between two moves keeps them two: adjacent moves may be sent as one.
    //
    // Three events are made on one side of a change of hands and handed over on the other, as a
    // queue does to an event made while the chord was on its way: a move made with the pointer
    // free, and a move and a press made with it captured. And the window is moved under a
    // captured pointer, which ends the capture.
    fn scripted_capture(&self) {
        let iv = self.ivars();
        // The script does not wait for a frame to bring the picture.
        if let Some(r) = iv.renderer.borrow_mut().as_mut() {
            r.upload(&iv.display, &mut iv.stats.borrow_mut());
        }
        let (Some(window), Some((rect, scale, (tw, th)))) = (self.window(), self.picture()) else {
            eprintln!("scripted: no scanout, the captured pointer is not tried");
            return;
        };
        // A pointer as the guest would define one and where it would have it: three colours,
        // white, a clear pixel, and a colour at half strength with its alpha multiplied in.
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
        iv.display.place_pointer(100, 60);
        let wn = window.windowNumber();
        let none = NSEventModifierFlags::empty();
        let middle = self.convertPoint_toView(
            NSPoint::new(
                rect.origin.x + rect.size.width / 2.0,
                rect.origin.y + rect.size.height / 2.0,
            ),
            None,
        );
        // An event over the middle of the picture, stamped when it is made.
        let made = |ty: NSEventType| {
            let ev = NSEvent::mouseEventWithType_location_modifierFlags_timestamp_windowNumber_context_eventNumber_clickCount_pressure(
                ty, middle, none, uptime(), wn, None, 0, 1, 1.0);
            if ev.is_none() {
                eprintln!("scripted: no mouse event");
            }
            ev
        };
        let hand = |ev: &NSEvent| {
            let _: () = match ev.r#type() {
                NSEventType::MouseMoved => unsafe { msg_send![self, mouseMoved: ev] },
                NSEventType::LeftMouseDown => unsafe { msg_send![self, mouseDown: ev] },
                NSEventType::LeftMouseUp => unsafe { msg_send![self, mouseUp: ev] },
                NSEventType::RightMouseDown => unsafe { msg_send![self, rightMouseDown: ev] },
                _ => unsafe { msg_send![self, rightMouseUp: ev] },
            };
        };
        let button = |ty: NSEventType| {
            if let Some(ev) = made(ty) {
                hand(&ev);
            }
        };
        let moved = |dx: i64, dy: i64| self.scripted_move(dx, dy);
        // A notch up, stamped when it is made. Where it says it is is wherever the Mac's pointer
        // happens to be, and nothing made here may depend on that.
        let made_notch = || {
            let cg = CGEvent::new_scroll_wheel_event2(None, CGScrollEventUnit::Line, 1, 1, 0, 0);
            if let Some(cg) = &cg {
                stamp(cg);
            }
            let ev = cg.and_then(|cg| NSEvent::eventWithCGEvent(&cg));
            if ev.is_none() {
                eprintln!("scripted: no scroll event");
            }
            ev
        };
        let scroll = |ev: &NSEvent| {
            let _: () = unsafe { msg_send![self, scrollWheel: ev] };
        };
        // Made with the pointer free and handed over captured: no motion of a captured pointer.
        let early = self.made_move(7, 7);
        self.capture(true);
        let held = iv.seized.get();
        if let Some(ev) = &early {
            hand(ev);
        }
        let first = moved(51, -26);
        button(NSEventType::LeftMouseDown);
        let second = moved(-20, 10);
        button(NSEventType::LeftMouseUp);
        if let Some(ev) = made_notch() {
            scroll(&ev);
        }
        // Right goes down captured and comes up after the pointer was given back, over the
        // picture: it comes up on the mouse.
        button(NSEventType::RightMouseDown);
        // Made captured and handed over after the give-back. The move is no place: it says where
        // the Mac's pointer was held. The press and the notch are sent, with no place for the
        // same reason.
        let late = (
            made(NSEventType::MouseMoved),
            made(NSEventType::LeftMouseDown),
            made_notch(),
        );
        self.capture(false);
        button(NSEventType::RightMouseUp);
        for ev in [&late.0, &late.1].into_iter().flatten() {
            hand(ev);
        }
        if let Some(ev) = &late.2 {
            scroll(ev);
        }
        button(NSEventType::LeftMouseUp);
        // Then a click that is the tablet's again.
        button(NSEventType::LeftMouseDown);
        button(NSEventType::LeftMouseUp);
        // The window moves under a captured pointer. The Mac's pointer would be held at a point
        // the picture is no longer under, so the capture ends, and a click is the tablet's.
        self.capture(true);
        let o = window.frame().origin;
        window.setFrameOrigin(NSPoint::new(o.x + 1.0, o.y));
        let let_go = !iv.captured.get();
        button(NSEventType::LeftMouseDown);
        button(NSEventType::LeftMouseUp);
        // Captured again and left so, for the picture a test takes at the end: the guest's
        // pointer is in it.
        self.capture(true);
        eprintln!(
            "scripted: captured pointer: the Mac's pointer held: {held}; moves of {},{} and {},{} points at {scale} points to a guest pixel; given back when the window moved: {let_go}; the guest's pointer drawn at 100,60 of {tw}x{th}",
            first.0, first.1, second.0, second.1
        );
    }

    // Release everything the guest believes is held, keys and buttons: the window stopped being where
    // they go, and whatever is let go of from here on is let go of somewhere else.
    pub fn release_keys(&self) {
        // The Mac's pointer goes back with them: a window that is not key has no claim on it.
        self.capture(false);
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
                at: None,
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

    // Where the scanout is shown in the view, how many view points a guest pixel takes, and the
    // scanout's size. None while there is no scanout, or while a frame is being drawn.
    fn picture(&self) -> Option<(NSRect, f64, (u32, u32))> {
        let b = self.bounds();
        let s = self.window().map(|w| w.backingScaleFactor()).unwrap_or(1.0);
        let (tw, th) = self.ivars().renderer.try_borrow().ok()?.as_ref()?.tex_size;
        let (x, y, vw, vh, scale) = place(tw, th, b.size.width, b.size.height, s)?;
        let rect = NSRect::new(NSPoint::new(x, y), NSSize::new(vw, vh));
        Some((rect, scale, (tw, th)))
    }

    // The scanout pixel under a window event, if it is over the picture, and the scanout's size.
    fn guest_point(&self, ev: &NSEvent) -> Option<Place> {
        let p = self.convertPoint_fromView(ev.locationInWindow(), None);
        let (r, _, (w, h)) = self.picture()?;
        let rect = (r.origin.x, r.origin.y, r.size.width, r.size.height);
        let (x, y) = guest_pixel(p.x, p.y, rect, w, h)?;
        Some(Place { x, y, w, h })
    }

    // Bring the Mac's pointer up to the guest's: its image, whether it shows one, and the size a
    // guest pixel has in the view now.
    fn wear_pointer(&self) {
        let iv = self.ivars();
        let picture = self.picture();
        let scale = picture.map_or(0.0, |(_, s, _)| s);
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
        if let (Some(cursor), Some((rect, _, _)), true) = (cursor, picture, window.isKeyWindow()) {
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

    // Capture the pointer, or give it back. Captured, the Mac's pointer is put over the middle of
    // the picture, held there and hidden, its moves go to the guest's mouse as motion, and the
    // guest's own pointer is drawn in the picture. Nothing the guest holds is let go of either
    // way: a button comes up on the device it went down on. A capture that cannot hold the Mac's
    // pointer is not made.
    pub fn capture(&self, on: bool) {
        let iv = self.ivars();
        if iv.captured.get() == on {
            return;
        }
        // Only the capture that has just put the Mac's pointer somewhere has a jump to drop.
        iv.skip.set(false);
        if on {
            if !self.seize() {
                return;
            }
        } else {
            self.give_back();
        }
        iv.captured.set(on);
        iv.carry.set((0.0, 0.0));
        // Read after the Mac's pointer was moved: see `stale`.
        iv.turned.set(uptime());
        self.send(Input::Capture(on));
        // The guest's pointer comes into the picture, or goes from it.
        iv.owed.set(true);
        if let Some(window) = self.window() {
            window.setTitle(&NSString::from_str(if on { CAPTURED_TITLE } else { TITLE }));
        }
    }

    // An event made before the pointer last changed hands and taken after it: a queue does that
    // to whatever was made while the chord was on its way. Where it says the pointer is, and how
    // far it says the pointer went, are the other state's. A move made while the pointer was held
    // says the held place, and sent as a place it would put the guest's pointer there; one made
    // while it was free has no jump in it to drop. So such a move is not sent, and such a press
    // or notch is sent with no place.
    //
    // Only an event of the last few seconds before the change is one: nothing waits in a queue
    // for longer. The bound is for the premise this stands on, that an event is stamped on the
    // clock `uptime` reads, which no test can be handed a real event to show. Were it false,
    // every event would be far older or newer than a change of hands and none would be stale,
    // which is how it was before this rule; without the bound, every move after a capture could
    // be taken for an old one.
    fn stale(&self, ev: &NSEvent) -> bool {
        let (made, turned) = (ev.timestamp(), self.ivars().turned.get());
        made < turned && made > turned - STALE_WITHIN
    }

    // The Mac's side of a capture: its pointer put over the middle of the picture, so that a
    // click is this view's whatever the pointer was over when the chord was pressed (a menu,
    // another window), then held there and hidden. Only a key window's: a click goes to the
    // window the Mac's pointer is over, and a window that is not key does not hold that pointer
    // over itself; its capture stands with the Mac's pointer left alone. Handed back: whether
    // the capture can stand. It cannot when the pointer could not be put there and held: one
    // that is held somewhere else, or hidden and free, clicks on whatever is under it.
    fn seize(&self) -> bool {
        let iv = self.ivars();
        if !self.window().is_some_and(|w| w.isKeyWindow()) {
            return true;
        }
        let b = self.bounds();
        let middle = match self.picture() {
            Some((r, _, _)) => NSPoint::new(
                r.origin.x + r.size.width / 2.0,
                r.origin.y + r.size.height / 2.0,
            ),
            None => NSPoint::new(b.size.width / 2.0, b.size.height / 2.0),
        };
        let Some(p) = self.on_screen(middle) else {
            eprintln!("pointer: not captured: the view is on no screen");
            return false;
        };
        let e = CGWarpMouseCursorPosition(p);
        if e != CGError::Success {
            eprintln!(
                "pointer: not captured: the Mac's pointer could not be put over the picture ({})",
                e.0
            );
            return false;
        }
        let e = CGAssociateMouseAndMouseCursorPosition(false);
        if e != CGError::Success {
            eprintln!(
                "pointer: not captured: the Mac's pointer could not be held ({})",
                e.0
            );
            return false;
        }
        NSCursor::hide();
        iv.seized.set(true);
        // The first move made after this has the jump in it.
        iv.skip.set(true);
        true
    }

    // Give the Mac's pointer back, if it is held. Nothing between taking the flag and letting go
    // can leave early: letting go needs no window, only the place the pointer is put back to
    // does. That place is where the guest's pointer has got to, so that the next place sent is
    // the one the guest already has it at.
    fn give_back(&self) {
        let iv = self.ivars();
        if !iv.seized.replace(false) {
            return;
        }
        let at = match iv.display.shared.lock() {
            Ok(s) => s.pointer_at,
            Err(e) => e.into_inner().pointer_at,
        };
        let there = self.picture().map(|(r, _, (tw, th))| {
            NSPoint::new(
                r.origin.x + (at.0 as f64 + 0.5) * r.size.width / tw as f64,
                r.origin.y + r.size.height - (at.1 as f64 + 0.5) * r.size.height / th as f64,
            )
        });
        let done = |what: &str, e: CGError| {
            if e != CGError::Success {
                eprintln!("pointer: {what} failed ({})", e.0);
            }
        };
        if let Some(p) = there.and_then(|p| self.on_screen(p)) {
            done(
                "putting the Mac's pointer where the guest's is",
                CGWarpMouseCursorPosition(p),
            );
        }
        // Straight after the move: it also ends the moment for which the system holds a pointer
        // still after one.
        done(
            "letting go of the Mac's pointer",
            CGAssociateMouseAndMouseCursorPosition(true),
        );
        NSCursor::unhide();
    }

    // A point of the view where the system counts the pointer's place from: the first screen's
    // top left corner, down.
    fn on_screen(&self, p: NSPoint) -> Option<CGPoint> {
        let window = self.window()?;
        let s = window.convertPointToScreen(self.convertPoint_toView(p, None));
        let top = NSScreen::screens(self.mtm())
            .firstObject()?
            .frame()
            .size
            .height;
        Some(CGPoint::new(s.x, top - s.y))
    }

    // A captured pointer's move. The event says how far the Mac's pointer would have gone, in
    // points; the guest's mouse is moved as many guest pixels as that is in the picture.
    fn motion(&self, ev: &NSEvent) -> bool {
        let iv = self.ivars();
        if iv.skip.replace(false) {
            return false;
        }
        let Some((_, scale, _)) = self.picture() else {
            return false;
        };
        let (mut cx, mut cy) = iv.carry.get();
        let dx = guest_motion(&mut cx, ev.deltaX(), scale);
        let dy = guest_motion(&mut cy, ev.deltaY(), scale);
        iv.carry.set((cx, cy));
        if (dx, dy) != (0, 0) {
            self.send(Input::Rel { dx, dy });
        }
        true
    }

    // Whether a place was sent for the event: none is over a bar. For a captured pointer, whether
    // its motion was taken.
    fn pointer(&self, ev: &NSEvent) -> bool {
        self.settle_modifiers(ev);
        if self.stale(ev) {
            return false;
        }
        if self.ivars().captured.get() {
            return self.motion(ev);
        }
        let at = self.guest_point(ev);
        if let Some(at) = at {
            self.send(Input::Abs(at));
        }
        at.is_some()
    }

    // A press counts only over the picture: over the bars it would land wherever the guest's
    // pointer last was. A release is sent exactly when its press was, wherever it happens: with
    // its place over the picture, and with none off it. Whether the event was sent is handed back.
    // A captured pointer's buttons have no place, and every press of one counts: the guest's
    // pointer is wherever its mouse has moved it.
    fn button(&self, ev: &NSEvent, button: Option<u32>, down: bool) -> bool {
        self.settle_modifiers(ev);
        let Some(button) = button else { return false };
        // No place: the guest's pointer is where its mouse moved it, and not where this event
        // says the Mac's is.
        let placeless = self.ivars().captured.get() || self.stale(ev);
        let at = if placeless {
            None
        } else {
            self.guest_point(ev)
        };
        if down {
            if (!placeless && at.is_none()) || !self.ivars().pressed.borrow_mut().insert(button) {
                return false;
            }
        } else if !self.ivars().pressed.borrow_mut().remove(&button) {
            return false;
        }
        self.send(Input::Button { button, down, at });
        true
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
        // The two Shifts from a source that gives neither a bit of its own. Both go down, the
        // window loses the keys, and the left one comes up while the right one holds the flag
        // they share: nothing tells that from a press, and it is sent as one, let go of when
        // the flag clears.
        key(
            NSEventType::FlagsChanged,
            0x38,
            NSEventModifierFlags::Shift,
            false,
        );
        key(
            NSEventType::FlagsChanged,
            0x3c,
            NSEventModifierFlags::Shift,
            false,
        );
        lose_keys();
        key(
            NSEventType::FlagsChanged,
            0x38,
            NSEventModifierFlags::Shift,
            false,
        );
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
        // A modifier whose release is never seen is let go of by whatever comes next, before
        // it: here by a move, with nothing else.
        key(
            NSEventType::FlagsChanged,
            0x3a,
            NSEventModifierFlags::Option,
            false,
        );
        mouse(NSEventType::MouseMoved, mid);
        key(NSEventType::FlagsChanged, 0x3a, none, false);
        // And by a click over the bar and a scroll over it, which send nothing of their own.
        // What reaches QEMU cannot show when the release was sent, the late one would look the
        // same, so the view says what the guest holds after each.
        let let_go = |what: &str, vk: u16| {
            let verdict = if iv.held.borrow().holds(vk) {
                "DID NOT let go of"
            } else {
                "let go of"
            };
            eprintln!("scripted: {what} over the bar {verdict} the modifier");
        };
        key(
            NSEventType::FlagsChanged,
            0x38,
            NSEventModifierFlags::Shift,
            false,
        );
        mouse(NSEventType::RightMouseDown, bar);
        let_go("a click", 0x38);
        mouse(NSEventType::RightMouseUp, bar);
        key(NSEventType::FlagsChanged, 0x38, none, false);
        key(
            NSEventType::FlagsChanged,
            0x3b,
            NSEventModifierFlags::Control,
            false,
        );
        // Core Graphics gives an event it makes the modifiers that are down on the Mac just
        // then; this one must say that none is.
        let over_bar = CGEvent::new_scroll_wheel_event2(None, CGScrollEventUnit::Line, 1, 1, 0, 0);
        if let Some(cg) = &over_bar {
            CGEvent::set_location(Some(cg), CGPoint::new(bar.x, top - bar.y));
            CGEvent::set_flags(Some(cg), CGEventFlags(0));
        }
        match over_bar.and_then(|cg| NSEvent::eventWithCGEvent(&cg)) {
            Some(ev) => unsafe { msg_send![self, scrollWheel: &*ev] },
            None => eprintln!("scripted: no scroll event"),
        }
        let_go("a scroll", 0x3b);
        key(NSEventType::FlagsChanged, 0x3b, none, false);
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
        // A display that sleeps gives no ticks, so the pointer is brought up to date here as well,
        // and with it what is drawn of the guest's own.
        self.wear_pointer();
        if let Some(r) = iv.renderer.borrow_mut().as_mut() {
            self.draw_pointer(r);
        }
        let b = self.bounds();
        let told = match iv.told.get() {
            Some(t) => format!(
                "told {}x{} px {}x{} mm, {} tellings",
                t.width,
                t.height,
                t.width_mm,
                t.height_mm,
                iv.tellings.get()
            ),
            None => "told nothing".to_string(),
        };
        eprintln!(
            "size: view {}x{} points at {}, {told}",
            b.size.width,
            b.size.height,
            self.window().map_or(0.0, |w| w.backingScaleFactor())
        );
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

    // What the pass draws of the guest's pointer from here on. A captured pointer is drawn: the
    // image the guest shows, where the guest has it. Whether that changes the frame.
    fn draw_pointer(&self, r: &mut Renderer) -> bool {
        let iv = self.ivars();
        let drawn = match (iv.captured.get(), iv.worn.try_borrow()) {
            (true, Ok(worn)) if worn.on => worn
                .image
                .clone()
                .map(|image| (image, iv.display.shared.lock().unwrap().pointer_at)),
            _ => None,
        };
        r.set_pointer(drawn.as_ref().map(|(image, at)| (image, *at)))
    }

    fn frame(&self) {
        let iv = self.ivars();
        let mut guard = iv.renderer.borrow_mut();
        let Some(r) = guard.as_mut() else { return };
        let t0 = Instant::now();
        let scale = self.window().map(|w| w.backingScaleFactor()).unwrap_or(1.0);
        let b = self.bounds();
        let want = (pixels(b.size.width, scale), pixels(b.size.height, scale));
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
        changed |= self.draw_pointer(r);
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
    use super::{
        button_code, fit, guest_motion, guest_pixel, notches, place, pointer_scale, replace, stamp,
        uptime, Renderer, Stats,
    };
    use crate::display::{pixels, Display, Order, Pointer, Rect};
    use objc2::runtime::ProtocolObject;
    use objc2_metal::{
        MTLCommandBuffer, MTLCommandQueue, MTLCreateSystemDefaultDevice, MTLDevice, MTLOrigin,
        MTLPixelFormat, MTLRegion, MTLSize, MTLTexture, MTLTextureDescriptor, MTLTextureUsage,
    };
    use std::ptr::NonNull;
    use std::sync::atomic::Ordering;
    use std::sync::Arc;

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
    fn a_picture_a_pixel_of_its_own_short_of_the_view_keeps_a_whole_scale() {
        // What a guest makes of 3025 x 1891 over two, and over four of 3027 x 1893: the remainder is
        // a bar, on the right and at the bottom when it is one pixel.
        assert_eq!(
            fit(1512.0, 945.0, 3025.0, 1891.0),
            Some((0.0, 0.0, 3024.0, 1890.0, 2.0))
        );
        assert_eq!(
            fit(756.0, 473.0, 3027.0, 1893.0),
            Some((1.0, 0.0, 3024.0, 1892.0, 4.0))
        );
        // A picture of another shape than the view: the side that limits it decides.
        assert_eq!(
            fit(1280.0, 800.0, 2561.0, 1700.0),
            Some((0.0, 50.0, 2560.0, 1600.0, 2.0))
        );
        // The same on its side, where the height limits.
        assert_eq!(
            fit(800.0, 1280.0, 1700.0, 2561.0),
            Some((50.0, 0.0, 1600.0, 2560.0, 2.0))
        );
        // A view one pixel larger than its picture each way shows it a pixel to a pixel.
        assert_eq!(
            fit(2560.0, 1600.0, 2561.0, 1601.0),
            Some((0.0, 0.0, 2560.0, 1600.0, 1.0))
        );
        // The bound is one of the picture's own pixels, counted: at it, and one view pixel past it.
        assert_eq!(fit(4.0, 2.0, 10.0, 91.0), Some((1.0, 43.0, 8.0, 4.0, 2.0)));
        assert_eq!(
            fit(4.0, 2.0, 11.0, 91.0),
            Some((0.0, 42.0, 11.0, 6.0, 2.75))
        );
        // Past it the view is filled, as any picture far from a whole scale is.
        assert_eq!(
            fit(1280.0, 800.0, 2800.0, 1800.0),
            Some((0.0, 25.0, 2800.0, 1750.0, 2.1875))
        );
        // A picture larger than the view is never at a whole scale: there is none below one.
        assert_eq!(
            fit(1280.0, 800.0, 640.0, 200.0),
            Some((160.0, 0.0, 320.0, 200.0, 0.25))
        );
    }

    #[test]
    fn a_filled_picture_has_whole_pixels_inside_the_view() {
        // The height bounds the scale and 480 times it is a hair over 1000 in floating point: the
        // picture takes the 1000 rows, from the first, and the nearest whole number of columns.
        assert_eq!(
            fit(640.0, 480.0, 1334.0, 1000.0),
            Some((0.0, 0.0, 1333.0, 1000.0, 1000.0 / 480.0))
        );
        // A picture so flat that its height rounds to nothing is a row high.
        assert_eq!(
            fit(16384.0, 1.0, 100.0, 100.0),
            Some((0.0, 49.0, 100.0, 1.0, 100.0 / 16384.0))
        );
        let mut filled = 0u32;
        for (tw, th) in [
            (640u32, 480u32),
            (1024, 768),
            (1280, 800),
            (800, 1280),
            (1512, 945),
            (37, 91),
            (16384, 1),
            (1, 16384),
        ] {
            for w in (1u32..4000).step_by(7).chain([1334, 16384, 65535]) {
                for h in [1u32, 2, 199, 480, 500, 767, 1000, 1117, 2160, 16384] {
                    let at = format!("{tw}x{th} in {w}x{h}");
                    let (x, y, vw, vh, scale) =
                        fit(tw as f64, th as f64, w as f64, h as f64).unwrap();
                    for v in [x, y, vw, vh] {
                        assert_eq!(v, v.floor(), "{at}");
                    }
                    assert!(x >= 0.0 && y >= 0.0 && vw >= 1.0 && vh >= 1.0, "{at}");
                    assert!(x + vw <= w as f64 && y + vh <= h as f64, "{at}");
                    if scale < 1.0 || scale != scale.floor() {
                        filled += 1;
                        assert!(vw == w as f64 || vh == h as f64, "{at}");
                        assert!(
                            x <= (w as f64 - vw) / 2.0 && y <= (h as f64 - vh) / 2.0,
                            "{at}"
                        );
                    } else {
                        assert_eq!((vw, vh), (tw as f64 * scale, th as f64 * scale), "{at}");
                    }
                }
            }
        }
        assert!(filled > 10000, "{filled}");
    }

    #[test]
    fn every_picture_a_guest_fits_to_a_view_is_drawn_whole() {
        // A view's size over k with the remainder dropped, as libhalcyon's fit_mode makes a mode.
        let mut n = 0u32;
        for long in 640u32..=8192 {
            for short in [
                200u32, 399, 400, 401, 480, 481, 720, 768, 945, 1117, 1964, 2234, 3384,
            ] {
                for (w, h) in [(long, short), (short, long)] {
                    for k in 2..=4u32 {
                        let (tw, th) = (w / k, h / k);
                        n += 1;
                        assert_eq!(
                            fit(tw as f64, th as f64, w as f64, h as f64),
                            Some((
                                ((w - tw * k) / 2) as f64,
                                ((h - th * k) / 2) as f64,
                                (tw * k) as f64,
                                (th * k) as f64,
                                k as f64
                            )),
                            "{w}x{h} over {k}"
                        );
                    }
                }
            }
        }
        assert!(n > 500000, "{n}");
    }

    #[test]
    fn a_pointer_over_the_picture_is_over_the_pixel_drawn_there() {
        // A whole scale with half a point of bar on the right and below (two pixels to a point).
        let (tw, th) = (1512, 945);
        let (x, y, w, h, _) = place(tw, th, 1512.5, 945.5, 2.0).unwrap();
        let r = (x, y, w, h);
        assert_eq!(guest_pixel(0.25, 945.25, r, tw, th), Some((0, 0)));
        assert_eq!(guest_pixel(1511.75, 0.75, r, tw, th), Some((1511, 944)));
        assert_eq!(guest_pixel(1512.25, 500.0, r, tw, th), None);
        assert_eq!(guest_pixel(100.0, 0.25, r, tw, th), None);
        // The top edge is the first row's and the bottom edge is nobody's; left and right likewise.
        assert_eq!(guest_pixel(0.0, 945.5, r, tw, th), Some((0, 0)));
        assert_eq!(guest_pixel(0.0, 0.5, r, tw, th), None);
        assert_eq!(guest_pixel(1512.0, 945.5, r, tw, th), None);
        assert_eq!(guest_pixel(-0.25, 500.0, r, tw, th), None);
        assert_eq!(guest_pixel(f64::NAN, 500.0, r, tw, th), None);
        // A filled picture is stretched each way by its own amount: 640 columns over 1333 pixels
        // and 480 rows over 1000. Its last column and its last row are the last ones drawn.
        let (tw, th) = (640, 480);
        let (x, y, w, h, _) = place(tw, th, 1334.0, 1000.0, 1.0).unwrap();
        let r = (x, y, w, h);
        assert_eq!(r, (0.0, 0.0, 1333.0, 1000.0));
        assert_eq!(guest_pixel(1332.5, 0.5, r, tw, th), Some((639, 479)));
        assert_eq!(guest_pixel(1333.5, 0.5, r, tw, th), None);
        assert_eq!(guest_pixel(0.5, 999.5, r, tw, th), Some((0, 0)));
        assert_eq!(guest_pixel(666.5, 500.5, r, tw, th), Some((320, 239)));
        assert_eq!(guest_pixel(1.0, 1.0, r, 0, th), None);
        // Three rows drawn as one, a hundred columns as ten: the two ways differ by thirty times.
        let (x, y, w, h, _) = place(100, 3, 10.0, 10.0, 1.0).unwrap();
        assert_eq!((x, y, w, h), (0.0, 5.0, 10.0, 1.0));
        assert_eq!(guest_pixel(5.5, 5.5, (x, y, w, h), 100, 3), Some((55, 1)));
        // Anywhere inside, to the last number below the far edges, is a pixel the scanout has.
        for w in 1..4000u32 {
            for n in [1u32, 3, 640, 1000, 1512, 16384] {
                let side = w as f64;
                let last = f64::from_bits(side.to_bits() - 1);
                let r = (0.0, 0.0, side, side);
                assert_eq!(
                    guest_pixel(last, side - last, r, n, n),
                    Some((n - 1, n - 1))
                );
                assert_eq!(guest_pixel(0.0, side, r, n, n), Some((0, 0)));
            }
        }
    }

    #[test]
    fn the_pointer_takes_the_place_the_drawing_takes() {
        // 1512.5 x 945.5 points at two pixels to a point are 3025 x 1891 pixels: the picture is drawn
        // from the top left corner and the odd row is a bar at the bottom, half a point high.
        assert_eq!(
            place(1512, 945, 1512.5, 945.5, 2.0),
            Some((0.0, 0.5, 1512.0, 945.0, 1.0))
        );
        // One pixel to a point: a bar of one row above and two below.
        assert_eq!(
            place(100, 50, 201.0, 103.0, 1.0),
            Some((0.0, 2.0, 200.0, 100.0, 2.0))
        );
        // Bars that are even stay even.
        assert_eq!(
            place(4, 2, 200.0, 150.0, 2.0),
            Some((0.0, 25.0, 200.0, 100.0, 50.0))
        );
        assert_eq!(place(4, 2, 200.0, 150.0, 0.0), None);
        assert_eq!(place(0, 2, 200.0, 150.0, 2.0), None);
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
        // Bars above and below, bars at the sides, no bars; a size a pixel past nine times the
        // picture, which is drawn at nine; then one that nothing divides, at a scale and a half.
        for (dw, dh, exact) in [
            (400u32, 300u32, true),
            (300, 400, true),
            (400, 200, true),
            (37, 91, true),
            (6, 91, false),
        ] {
            let target = texture(&r.device, dw, dh, true);
            let cmd = r.queue.commandBuffer().unwrap();
            r.draw(&cmd, &target, (dw, dh));
            cmd.commit();
            cmd.waitUntilCompleted();
            let got = read(&target, dw, dh);
            // The view at a pixel to a point, and each device pixel's middle as a pointer there.
            let (x0, y0, vw, vh, _) = place(tw, th, dw as f64, dh as f64, 1.0).unwrap();
            let rect = (x0, y0, vw, vh);
            let mut inside = 0;
            for py in 0..dh {
                for px in 0..dw {
                    let under = guest_pixel(px as f64 + 0.5, (dh - py) as f64 - 0.5, rect, tw, th);
                    let at = &got[(py as usize * dw as usize + px as usize) * 4..][..4];
                    match under {
                        Some((gx, gy)) if exact => {
                            inside += 1;
                            assert_eq!(at, colour(gx, gy), "{dw}x{dh} at ({px}, {py})");
                        }
                        // Filtered: some mixture of the picture's colours, none of which is black.
                        Some(_) => {
                            inside += 1;
                            assert_ne!(at[..3], [0, 0, 0], "{dw}x{dh} at ({px}, {py})");
                        }
                        None => assert_eq!(at, [0, 0, 0, 255], "{dw}x{dh} bar at ({px}, {py})"),
                    }
                }
            }
            assert_eq!(inside, (vw * vh) as u32, "{dw}x{dh}");
            if !exact {
                // The device pixel a guest pixel's middle falls in shows that pixel, near enough.
                for y in 0..th {
                    for x in 0..tw {
                        let px = (x0 + (x as f64 + 0.5) * vw / tw as f64) as u32;
                        let up = y0 + vh - (y as f64 + 0.5) * vh / th as f64;
                        let py = dh - 1 - up as u32;
                        assert_eq!(
                            guest_pixel(px as f64 + 0.5, (dh - py) as f64 - 0.5, rect, tw, th),
                            Some((x, y))
                        );
                        let at = &got[(py as usize * dw as usize + px as usize) * 4..][..4];
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

    // A tw x th scanout drawn into dw x dh device pixels and read back. Each of its pixels says
    // which it is (and is never black), or all of them are white.
    fn drawn(r: &mut Renderer, tw: u32, th: u32, dw: u32, dh: u32, white: bool) -> Vec<u8> {
        let mut src = Vec::with_capacity(tw as usize * th as usize * 4);
        for y in 0..th {
            for x in 0..tw {
                let own = [
                    x as u8,
                    y as u8,
                    0x80 | (x >> 8) as u8 | ((y >> 8) << 3) as u8,
                    255,
                ];
                src.extend_from_slice(if white { &[255; 4] } else { &own });
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
        let target = texture(&r.device, dw, dh, true);
        let cmd = r.queue.commandBuffer().unwrap();
        r.draw(&cmd, &target, (dw, dh));
        cmd.commit();
        cmd.waitUntilCompleted();
        read(&target, dw, dh)
    }

    // The scanout pixel a device pixel of `drawn` shows at a whole scale; None for a bar.
    fn shown(at: &[u8]) -> Option<(u32, u32)> {
        if at[..3] == [0, 0, 0] {
            return None;
        }
        let r = at[2] as u32;
        Some((
            at[0] as u32 | (r & 7) << 8,
            at[1] as u32 | (r >> 3 & 15) << 8,
        ))
    }

    #[test]
    fn at_a_whole_scale_every_device_pixel_shows_the_pixel_a_click_on_it_sends() {
        let Ok(mut r) = Renderer::new() else {
            eprintln!("no Metal device: not tried");
            return;
        };
        let mut views = 0;
        for (tw, th) in [(64u32, 48u32), (48, 64), (33, 17)] {
            for k in 1..=4u32 {
                // What is left over each way: nothing, one pixel, the most a fitted mode leaves,
                // and a whole guest pixel, which is the rule's bound.
                for (rw, rh) in [
                    (0, 0),
                    (1, 0),
                    (0, 1),
                    (1, 1),
                    (k - 1, k - 1),
                    (k, k),
                    (k, 0),
                    (1, k),
                ] {
                    let (dw, dh) = (tw * k + rw, th * k + rh);
                    let got = drawn(&mut r, tw, th, dw, dh, false);
                    for s in [1.0f64, 2.0] {
                        let (x, y, w, h, scale) =
                            place(tw, th, dw as f64 / s, dh as f64 / s, s).unwrap();
                        assert_eq!(scale * s, k as f64, "{tw}x{th} in {dw}x{dh}");
                        views += 1;
                        for py in 0..dh {
                            for px in 0..dw {
                                let at = &got[(py as usize * dw as usize + px as usize) * 4..][..4];
                                let click = ((px as f64 + 0.5) / s, ((dh - py) as f64 - 0.5) / s);
                                assert_eq!(
                                    shown(at),
                                    guest_pixel(click.0, click.1, (x, y, w, h), tw, th),
                                    "{tw}x{th} in {dw}x{dh} at {s}: device ({px}, {py})"
                                );
                            }
                        }
                    }
                }
            }
        }
        assert_eq!(views, 3 * 4 * 8 * 2);
    }

    #[test]
    fn a_filled_picture_covers_exactly_the_device_pixels_a_click_is_sent_from() {
        let Ok(mut r) = Renderer::new() else {
            eprintln!("no Metal device: not tried");
            return;
        };
        let mut views = 0;
        for (tw, th) in [(640u32, 480u32), (1024, 768), (1280, 800), (800, 1280)] {
            // Views in points with their pixels to a point. 1334 x 1000 is the one where 480 times
            // the scale is a hair over 1000; 1280 x 800 at one is the window as it opens.
            for (bw, bh, s) in [
                (1280.0f64, 800.0f64, 1.0f64),
                (1334.0, 1000.0, 1.0),
                (667.0, 500.0, 2.0),
                (1300.0, 850.0, 1.0),
                (1728.0, 1117.0, 1.0),
                (1000.5, 700.5, 2.0),
                (333.0, 777.0, 1.0),
            ] {
                let (dw, dh) = (pixels(bw, s), pixels(bh, s));
                let (x, y, w, h, scale) = place(tw, th, bw, bh, s).unwrap();
                if scale * s >= 1.0 && scale * s == (scale * s).floor() {
                    continue;
                }
                views += 1;
                let got = drawn(&mut r, tw, th, dw, dh, true);
                for py in 0..dh {
                    for px in 0..dw {
                        let at = &got[(py as usize * dw as usize + px as usize) * 4..][..4];
                        let click = ((px as f64 + 0.5) / s, ((dh - py) as f64 - 0.5) / s);
                        let sent = guest_pixel(click.0, click.1, (x, y, w, h), tw, th);
                        assert_eq!(
                            at[..3] == [255, 255, 255],
                            sent.is_some(),
                            "{tw}x{th} in {bw}x{bh} at {s}: device ({px}, {py}) is {at:?}, sent {sent:?}"
                        );
                        assert!(at[..3] == [255, 255, 255] || at[..3] == [0, 0, 0]);
                    }
                }
            }
        }
        assert!(views >= 24, "{views}");
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

    // What the scripts stand on when they make an event on one side of a change of hands: an
    // event Core Graphics only makes says 0, and one stamped with the clock's reading in
    // nanoseconds reads back as that many seconds. That a real event is stamped on that clock is
    // not shown here: no test can be handed one.
    #[test]
    fn an_event_a_script_stamps_reads_back_on_the_clock_a_change_of_hands_is_read_from() {
        use objc2_app_kit::NSEvent;
        use objc2_core_foundation::CGPoint;
        use objc2_core_graphics::{CGEvent, CGEventType, CGMouseButton};
        let made = || {
            CGEvent::new_mouse_event(
                None,
                CGEventType::MouseMoved,
                CGPoint::new(0.0, 0.0),
                CGMouseButton::Left,
            )
            .unwrap()
        };
        assert_eq!(NSEvent::eventWithCGEvent(&made()).unwrap().timestamp(), 0.0);
        let cg = made();
        let before = uptime();
        stamp(&cg);
        let after = uptime();
        let stamped = NSEvent::eventWithCGEvent(&cg).unwrap().timestamp();
        assert!(
            before - 1e-6 <= stamped && stamped <= after,
            "stamped between {before} and {after}, reads {stamped}"
        );
    }

    #[test]
    fn a_captured_pointers_motion_is_whole_guest_pixels_and_none_of_it_is_lost() {
        // Two points to a guest pixel: a point at a time is a pixel every other move.
        let mut carry = 0.0;
        let sent: Vec<i32> = (0..5).map(|_| guest_motion(&mut carry, 1.0, 2.0)).collect();
        assert_eq!(sent, [0, 1, 0, 1, 0]);
        // Back the other way, the half that was kept is used up first.
        let back: Vec<i32> = (0..3)
            .map(|_| guest_motion(&mut carry, -1.0, 2.0))
            .collect();
        assert_eq!(back, [0, 0, -1]);
        assert_eq!(carry, 0.0);
        // A picture shown at half its size: a point is two of its pixels.
        assert_eq!(guest_motion(&mut carry, 3.0, 0.5), 6);
        // Many small moves come to what one large move would.
        let mut carry = 0.0;
        let sum: i32 = (0..1000).map(|_| guest_motion(&mut carry, 0.3, 1.7)).sum();
        assert_eq!(sum, 176);
        assert!((sum as f64 + carry - 300.0 / 1.7).abs() < 1e-6);
        // No picture to scale by, and a move that is no number: nothing sent, nothing kept.
        let mut carry = 0.25;
        assert_eq!(guest_motion(&mut carry, 5.0, 0.0), 0);
        assert_eq!(guest_motion(&mut carry, f64::NAN, 1.0), 0);
        assert_eq!(guest_motion(&mut carry, f64::INFINITY, 1.0), 0);
        assert_eq!(guest_motion(&mut carry, 5.0, f64::NAN), 0);
        assert_eq!(carry, 0.25);
        // More than a number holds is the most it holds, and is not carried over.
        assert_eq!(guest_motion(&mut carry, -1e300, 1.0), i32::MIN);
        assert_eq!(carry, 0.0);
    }

    // The pointer a test defines: red, green, blue; white, a clear pixel, and a colour at half
    // strength with its alpha multiplied in. Its hot spot is the clear pixel. B, G, R, A.
    fn pointer() -> Arc<Pointer> {
        let bgra = vec![
            0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, // red, green, blue
            255, 255, 255, 255, 30, 20, 10, 0, 20, 50, 100, 128, // white, clear, half
        ];
        Arc::new(Pointer::new(3, 2, 1, 1, bgra).unwrap())
    }

    // The picture's own pixel at (x, y), as `drawn` makes it.
    fn own(x: u32, y: u32) -> [u8; 4] {
        [x as u8, y as u8, 0x80, 255]
    }

    // The half-strength pixel laid over `under`: its colour as it is, and what its alpha leaves of
    // the one below. A blend is allowed to round either way.
    fn half_over(got: &[u8], under: [u8; 4]) {
        for (c, (&g, &u)) in [20u8, 50, 100].iter().zip(got.iter().zip(&under)) {
            let want = *c as f64 + u as f64 * (1.0 - 128.0 / 255.0);
            assert!((g as f64 - want).abs() <= 1.0, "{got:?} over {under:?}");
        }
    }

    #[test]
    fn the_guests_pointer_is_drawn_with_its_hot_spot_on_its_place() {
        let mut r = Renderer::new().unwrap();
        let image = pointer();
        // Nothing is drawn of a pointer until it is asked for, and asking twice is no news.
        assert!(!r.set_pointer(None));
        assert!(r.set_pointer(Some((&image, (5, 4)))));
        assert!(!r.set_pointer(Some((&image, (5, 4)))));
        let px = drawn(&mut r, 16, 8, 16, 8, false);
        let at = |x: u32, y: u32| &px[(y * 16 + x) as usize * 4..][..4];
        assert_eq!(at(4, 3), [0, 0, 255, 255]);
        assert_eq!(at(5, 3), [0, 255, 0, 255]);
        assert_eq!(at(6, 3), [255, 0, 0, 255]);
        assert_eq!(at(4, 4), [255, 255, 255, 255]);
        // The clear pixel, which is the hot spot, shows the picture; so does everything around.
        assert_eq!(at(5, 4), own(5, 4));
        half_over(at(6, 4), own(6, 4));
        for (x, y) in [(3, 3), (7, 3), (3, 4), (7, 4), (4, 2), (5, 5), (6, 5)] {
            assert_eq!(at(x, y), own(x, y), "{x},{y}");
        }
        // Moved: news, and drawn there. At two device pixels to a guest pixel an image pixel
        // covers two by two.
        assert!(r.set_pointer(Some((&image, (9, 2)))));
        let px = drawn(&mut r, 16, 8, 32, 16, false);
        let at = |x: u32, y: u32| &px[(y * 32 + x) as usize * 4..][..4];
        for (x, y) in [(16, 2), (17, 2), (16, 3), (17, 3)] {
            assert_eq!(at(x, y), [0, 0, 255, 255], "{x},{y}");
        }
        assert_eq!(at(18, 2), [0, 255, 0, 255]);
        assert_eq!(at(15, 2), own(7, 1));
        assert_eq!(at(18, 4), own(9, 2));
        assert_eq!(at(16, 6), own(8, 3));
        // In the picture's corner the part of the image that is off the picture is not drawn,
        // and does not come back in on the other side.
        assert!(r.set_pointer(Some((&image, (0, 0)))));
        let px = drawn(&mut r, 16, 8, 16, 8, false);
        let at = |x: u32, y: u32| &px[(y * 16 + x) as usize * 4..][..4];
        assert_eq!(at(0, 0), own(0, 0));
        half_over(at(1, 0), own(1, 0));
        for (x, y) in [(2, 0), (0, 1), (1, 1), (15, 0), (15, 7), (0, 7), (14, 0)] {
            assert_eq!(at(x, y), own(x, y), "{x},{y}");
        }
        // A view taller than the picture has bars, and they are not the guest's to draw on: the
        // image's top row would be in the bar above.
        let px = drawn(&mut r, 16, 8, 16, 12, false);
        let at = |x: u32, y: u32| &px[(y * 16 + x) as usize * 4..][..4];
        for x in 0..16 {
            assert_eq!(at(x, 1)[..3], [0, 0, 0], "{x}");
        }
        assert_eq!(at(0, 2), own(0, 0));
        half_over(at(1, 2), own(1, 0));
        // Given back: news, and the picture is as the guest drew it.
        assert!(r.set_pointer(None));
        let px = drawn(&mut r, 16, 8, 16, 8, false);
        for y in 0..8 {
            for x in 0..16 {
                assert_eq!(px[(y * 16 + x) as usize * 4..][..4], own(x, y), "{x},{y}");
            }
        }
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
