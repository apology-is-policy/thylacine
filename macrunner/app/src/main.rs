// Thylacine for Mac, first window: attach to a QEMU started with `-display dbus,p2p=on` and show the
// guest. usage: thylacine-mac --attach <qmp socket>; the other arguments are a test's (view::Options).
mod display;
mod keymap;
mod view;
// The guest's own rule for a scale, compiled into the tests from its source, so that what the
// tests say a guest makes of a size is what a guest makes of it.
#[cfg(test)]
#[rustfmt::skip]
#[allow(dead_code, clippy::all)]
#[path = "../../../usr/lib/libhalcyon/src/scale.rs"]
mod guest_scale;

use std::cell::OnceCell;

use objc2::rc::Retained;
use objc2::runtime::ProtocolObject;
use objc2::{define_class, msg_send, sel, ClassType, DefinedClass, MainThreadOnly};
use objc2_app_kit::{
    NSApplication, NSApplicationActivationPolicy, NSApplicationDelegate,
    NSApplicationTerminateReply, NSBackingStoreType, NSEvent, NSEventModifierFlags, NSEventType,
    NSMenu, NSMenuDidBeginTrackingNotification, NSMenuItem, NSResponder, NSWindow,
    NSWindowCollectionBehavior, NSWindowDelegate, NSWindowStyleMask,
};
use objc2_foundation::{
    MainThreadMarker, NSNotification, NSNotificationCenter, NSObject, NSObjectProtocol, NSPoint,
    NSRect, NSSize, NSString,
};

use view::{GuestView, Options};

define_class!(
    // SAFETY: NSApplication allows subclassing; Application has no ivars and no Drop.
    #[unsafe(super(NSApplication, NSResponder, NSObject))]
    #[thread_kind = MainThreadOnly]
    #[name = "ThylacineApplication"]
    struct Application;

    impl Application {
        // AppKit does not deliver a key-up while Command is held. The guest saw the key go down, so it
        // must see it come up, or the key stays held there.
        #[unsafe(method(sendEvent:))]
        fn send_event(&self, ev: &NSEvent) {
            if ev.r#type() == NSEventType::KeyUp && ev.modifierFlags().contains(NSEventModifierFlags::Command) {
                if let Some(window) = ev.window(self.mtm()) {
                    window.sendEvent(ev);
                    return;
                }
            }
            let _: () = unsafe { msg_send![super(self), sendEvent: ev] };
        }
    }
);

struct Ivars {
    window: OnceCell<Retained<NSWindow>>,
    view: OnceCell<Retained<GuestView>>,
    qmp: String,
    opts: Options,
    activate: bool,
}

define_class!(
    // SAFETY: NSObject allows subclassing; Delegate has no Drop.
    #[unsafe(super = NSObject)]
    #[thread_kind = MainThreadOnly]
    #[name = "ThylacineDelegate"]
    #[ivars = Ivars]
    struct Delegate;

    unsafe impl NSObjectProtocol for Delegate {}

    unsafe impl NSApplicationDelegate for Delegate {
        #[unsafe(method(applicationDidFinishLaunching:))]
        fn did_finish_launching(&self, _n: &NSNotification) {
            let mtm = self.mtm();
            let app = NSApplication::sharedApplication(mtm);
            let display = match display::connect(&self.ivars().qmp) {
                Ok(d) => d,
                Err(e) => {
                    eprintln!("thylacine-mac: {e}");
                    std::process::exit(1);
                }
            };
            let frame = NSRect::new(NSPoint::new(0.0, 0.0), NSSize::new(1280.0, 800.0));
            let view = match GuestView::new(mtm, frame, display, self.ivars().opts.clone()) {
                Ok(v) => v,
                Err(e) => {
                    eprintln!("thylacine-mac: {e}");
                    std::process::exit(1);
                }
            };
            // SAFETY: released-when-closed is turned off below, as a window outside a controller needs.
            let window = unsafe {
                NSWindow::initWithContentRect_styleMask_backing_defer(
                    NSWindow::alloc(mtm),
                    frame,
                    NSWindowStyleMask::Titled
                        | NSWindowStyleMask::Closable
                        | NSWindowStyleMask::Miniaturizable
                        | NSWindowStyleMask::Resizable,
                    NSBackingStoreType::Buffered,
                    false,
                )
            };
            unsafe { window.setReleasedWhenClosed(false) };
            window.setTitle(&NSString::from_str(view::TITLE));
            window.setCollectionBehavior(NSWindowCollectionBehavior::FullScreenPrimary);
            window.setAcceptsMouseMovedEvents(true);
            window.setContentView(Some(&view));
            window.makeFirstResponder(Some(&view));
            window.setDelegate(Some(ProtocolObject::from_ref(self)));
            // The least the window can be dragged to. The guest takes a mode for any size told
            // (its modes go down to 320x200). Its trusted screen needs 800x720 pixels and is
            // sized once, when the guest starts: at the size told by then, or at the first
            // that fits of that size over 2, times 2, over 3, times 3, over 4, times 4. The
            // size told is in pixels, so this least size is 1280x800 on a display of scale 2
            // and fits as it is; at scale 1 it is 640x400 and the trusted screen is twice that.
            window.setContentMinSize(NSSize::new(640.0, 400.0));
            window.center();
            window.makeKeyAndOrderFront(None);
            // Before anything else is sent: a guest that has not started yet starts at this size.
            view.tell_size();
            app.setMainMenu(Some(&menu(mtm)));
            app.setActivationPolicy(NSApplicationActivationPolicy::Regular);
            if self.ivars().activate {
                #[allow(deprecated)]
                app.activateIgnoringOtherApps(true);
            }
            self.ivars().window.set(window).ok();
            self.ivars().view.set(view).ok();
            // SAFETY: the delegate lives as long as the application, and the selector is its own.
            unsafe {
                NSNotificationCenter::defaultCenter().addObserver_selector_name_object(
                    self,
                    sel!(menuDidBeginTracking:),
                    Some(NSMenuDidBeginTrackingNotification),
                    None,
                );
            }
        }

        #[unsafe(method(applicationDidResignActive:))]
        fn did_resign_active(&self, _n: &NSNotification) {
            self.let_go();
        }

        // Quit and a closed window both end here.
        #[unsafe(method(applicationShouldTerminate:))]
        fn should_terminate(&self, _app: &NSApplication) -> NSApplicationTerminateReply {
            if let Some(v) = self.ivars().view.get() {
                v.part();
            }
            NSApplicationTerminateReply::TerminateNow
        }
    }

    impl Delegate {
        // A menu that is open takes the key events, releases included, and the window stays key
        // throughout: what the guest holds is let go of as the menu opens.
        #[unsafe(method(menuDidBeginTracking:))]
        fn menu_did_begin_tracking(&self, _n: &NSNotification) {
            self.let_go();
        }
    }

    unsafe impl NSWindowDelegate for Delegate {
        #[unsafe(method(windowDidResignKey:))]
        fn did_resign_key(&self, _n: &NSNotification) {
            self.let_go();
        }

        // A captured pointer is held at a point of the screen, over the picture. A window that
        // moves, changes its size or its screen, or goes to or from full screen is not under
        // that point for certain any more, and a click of a hidden pointer would be whatever
        // window's is: the capture ends. What the guest holds is not let go of: the window is
        // still where its input goes.
        #[unsafe(method(windowDidMove:))]
        fn did_move(&self, _n: &NSNotification) {
            self.give_pointer_back();
        }

        #[unsafe(method(windowDidResize:))]
        fn did_resize(&self, _n: &NSNotification) {
            self.give_pointer_back();
        }

        #[unsafe(method(windowDidChangeScreen:))]
        fn did_change_screen(&self, _n: &NSNotification) {
            self.give_pointer_back();
        }

        #[unsafe(method(windowWillEnterFullScreen:))]
        fn will_enter_full_screen(&self, _n: &NSNotification) {
            self.give_pointer_back();
        }

        #[unsafe(method(windowWillExitFullScreen:))]
        fn will_exit_full_screen(&self, _n: &NSNotification) {
            self.give_pointer_back();
        }

        #[unsafe(method(windowWillClose:))]
        fn will_close(&self, _n: &NSNotification) {
            NSApplication::sharedApplication(self.mtm()).terminate(None);
        }
    }
);

impl Delegate {
    fn give_pointer_back(&self) {
        if let Some(v) = self.ivars().view.get() {
            v.capture(false);
        }
    }

    fn let_go(&self) {
        if let Some(v) = self.ivars().view.get() {
            v.release_keys();
        }
    }
}

fn item(
    mtm: MainThreadMarker,
    title: &str,
    action: objc2::runtime::Sel,
    key: &str,
    mods: NSEventModifierFlags,
) -> Retained<NSMenuItem> {
    let it = unsafe {
        NSMenuItem::initWithTitle_action_keyEquivalent(
            NSMenuItem::alloc(mtm),
            &NSString::from_str(title),
            Some(action),
            &NSString::from_str(key),
        )
    };
    it.setKeyEquivalentModifierMask(mods);
    it
}

fn menu(mtm: MainThreadMarker) -> Retained<NSMenu> {
    let bar = NSMenu::new(mtm);
    let add = |title: &str, items: &[Retained<NSMenuItem>]| {
        let top = NSMenuItem::new(mtm);
        let sub = NSMenu::initWithTitle(NSMenu::alloc(mtm), &NSString::from_str(title));
        for it in items {
            sub.addItem(it);
        }
        top.setSubmenu(Some(&sub));
        bar.addItem(&top);
    };
    add(
        "Thylacine",
        &[item(
            mtm,
            "Quit Thylacine",
            sel!(terminate:),
            "q",
            NSEventModifierFlags::Command,
        )],
    );
    // The view answers the second: it is the window's first responder.
    add(
        "View",
        &[
            item(
                mtm,
                "Enter Full Screen",
                sel!(toggleFullScreen:),
                "f",
                NSEventModifierFlags::Command | NSEventModifierFlags::Control,
            ),
            item(
                mtm,
                "Capture Pointer",
                sel!(toggleCapture:),
                "g",
                NSEventModifierFlags::Command | NSEventModifierFlags::Control,
            ),
        ],
    );
    bar
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut qmp = None;
    let mut opts = Options::default();
    let mut activate = true;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--attach" => {
                qmp = args.get(i + 1).cloned();
                i += 1;
            }
            "--stats" => opts.print_stats = true,
            "--no-activate" => activate = false,
            "--exit-after" => {
                opts.exit_after = args.get(i + 1).and_then(|s| s.parse().ok());
                i += 1;
            }
            "--dump" => {
                opts.dump = args.get(i + 1).cloned();
                i += 1;
            }
            "--scripted-input" => opts.scripted_input = true,
            "--scripted-bar" => opts.scripted_bar = true,
            "--scripted-capture" => opts.scripted_capture = true,
            "--scripted-captured-click" => {
                opts.scripted_captured_click = args.get(i + 1).and_then(|s| {
                    let mut n = s.split(',');
                    let at = (n.next()?.parse().ok()?, n.next()?.parse().ok()?);
                    let by = (n.next()?.parse().ok()?, n.next()?.parse().ok()?);
                    n.next().is_none().then_some((at.0, at.1, by.0, by.1))
                });
                if opts.scripted_captured_click.is_none() {
                    eprintln!("thylacine-mac: --scripted-captured-click takes <x>,<y>,<dx>,<dy>: a pixel of the guest's screen, and the guest pixels to move from it");
                    std::process::exit(2);
                }
                i += 1;
            }
            "--scripted-click" => {
                opts.scripted_click = args.get(i + 1).and_then(|s| {
                    let (x, y) = s.split_once(',')?;
                    Some((x.parse().ok()?, y.parse().ok()?))
                });
                if opts.scripted_click.is_none() {
                    eprintln!("thylacine-mac: --scripted-click takes <x>,<y>, a pixel of the guest's screen");
                    std::process::exit(2);
                }
                i += 1;
            }
            "--resize-to" => {
                opts.resize_to = args.get(i + 1).and_then(|s| {
                    let (w, h) = s.split_once('x')?;
                    Some((w.parse().ok()?, h.parse().ok()?))
                });
                if opts.resize_to.is_none() {
                    eprintln!("thylacine-mac: --resize-to takes <points>x<points>");
                    std::process::exit(2);
                }
                i += 1;
            }
            other => {
                eprintln!("thylacine-mac: unknown argument {other}");
                std::process::exit(2);
            }
        }
        i += 1;
    }
    let Some(qmp) = qmp else {
        eprintln!("usage: thylacine-mac --attach <qmp socket> [--stats] [--no-activate] [--exit-after <s>] [--dump <ppm>] [--scripted-input] [--scripted-bar] [--scripted-capture] [--scripted-click <x>,<y>] [--scripted-captured-click <x>,<y>,<dx>,<dy>] [--resize-to <w>x<h>]");
        std::process::exit(2);
    };
    let mtm = MainThreadMarker::new().expect("the main thread");
    // The first +sharedApplication decides the class of the one application object.
    let app: Retained<NSApplication> =
        unsafe { msg_send![Application::class(), sharedApplication] };
    let delegate: Retained<Delegate> = {
        let this = Delegate::alloc(mtm).set_ivars(Ivars {
            window: OnceCell::new(),
            view: OnceCell::new(),
            qmp,
            opts,
            activate,
        });
        unsafe { msg_send![super(this), init] }
    };
    app.setDelegate(Some(ProtocolObject::from_ref(&*delegate)));
    app.run();
}
