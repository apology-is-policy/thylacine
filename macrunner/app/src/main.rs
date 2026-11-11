// Thylacine for Mac, first window: attach to a QEMU started with `-display dbus,p2p=on` and show the
// guest. usage: thylacine-mac --attach <qmp socket>; the other arguments are a test's (view::Options).
mod display;
mod keymap;
mod view;

use std::cell::OnceCell;

use objc2::rc::Retained;
use objc2::runtime::ProtocolObject;
use objc2::{define_class, msg_send, sel, ClassType, DefinedClass, MainThreadOnly};
use objc2_app_kit::{
    NSApplication, NSApplicationActivationPolicy, NSApplicationDelegate, NSBackingStoreType,
    NSEvent, NSEventModifierFlags, NSEventType, NSMenu, NSMenuItem, NSResponder, NSWindow,
    NSWindowCollectionBehavior, NSWindowDelegate, NSWindowStyleMask,
};
use objc2_foundation::{
    ns_string, MainThreadMarker, NSNotification, NSObject, NSObjectProtocol, NSPoint, NSRect,
    NSSize, NSString,
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
            window.setTitle(ns_string!("Thylacine"));
            window.setCollectionBehavior(NSWindowCollectionBehavior::FullScreenPrimary);
            window.setAcceptsMouseMovedEvents(true);
            window.setContentView(Some(&view));
            window.makeFirstResponder(Some(&view));
            window.setDelegate(Some(ProtocolObject::from_ref(self)));
            window.center();
            window.makeKeyAndOrderFront(None);
            app.setMainMenu(Some(&menu(mtm)));
            app.setActivationPolicy(NSApplicationActivationPolicy::Regular);
            if self.ivars().activate {
                #[allow(deprecated)]
                app.activateIgnoringOtherApps(true);
            }
            self.ivars().window.set(window).ok();
            self.ivars().view.set(view).ok();
        }
    }

    unsafe impl NSWindowDelegate for Delegate {
        #[unsafe(method(windowDidResignKey:))]
        fn did_resign_key(&self, _n: &NSNotification) {
            if let Some(v) = self.ivars().view.get() {
                v.release_keys();
            }
        }

        #[unsafe(method(windowWillClose:))]
        fn will_close(&self, _n: &NSNotification) {
            NSApplication::sharedApplication(self.mtm()).terminate(None);
        }
    }
);

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
    add(
        "View",
        &[item(
            mtm,
            "Enter Full Screen",
            sel!(toggleFullScreen:),
            "f",
            NSEventModifierFlags::Command | NSEventModifierFlags::Control,
        )],
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
            other => {
                eprintln!("thylacine-mac: unknown argument {other}");
                std::process::exit(2);
            }
        }
        i += 1;
    }
    let Some(qmp) = qmp else {
        eprintln!("usage: thylacine-mac --attach <qmp socket> [--stats] [--no-activate] [--exit-after <s>] [--dump <ppm>] [--scripted-input]");
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
