# MAC-RUNNER -- Thylacine as a Mac app: the outer-shell runner

**Status: DESIGNED, UNBUILT (2026-11-10). Binding.** The operator set the goal and the
constraints (section 2) and voted eight of the forks (section 11); the other seven stand
as recommended and are the operator's to overturn. Nothing here is built. Of section 9's
measurements only the ones a finding calls "measured on the M2" have run, and a
measurement that contradicts a finding amends this document before code follows it.

**Who read what.** A research agent (Fable 5.1) wrote the findings from source reads on
2026-11-10; every claim it could not verify is marked UNVERIFIED with what settles it.
The main session re-read these in the cited source and they hold: QEMU v11.1.3 creating
Apple's in-kernel GIC (`target/arm/hvf/hvf.c:1357-1360`) with vmstate
(`hw/intc/arm_gicv3_hvf.c:785`); QEMU's `LICENSE:8`; the `Listener.Unix.Map` interface
(`ui/dbus-display1.xml:473-492`); virglrenderer's `src/venus/vkr_metal_helpers.m` and its
use of `VK_EXT_external_memory_metal`; qemu-display's MIT licence; and every `tree:`
citation in B5, E1 and H1 (two of which it corrected). Every other source citation is the
research agent's reading, not re-read.

Evidence conventions. `qemu-10.1:` = gitlab.com/qemu-project/qemu `stable-10.1` at
`29e39cbd` (v10.1.5, 2026-03-17). `qemu-11.1:` = tag `v11.1.3` at `005c61c0` (2026-10-28,
the newest release when read). `utm:` = github.com/utmapp/UTM main, cloned 2026-11-10.
`virgl:` = gitlab.freedesktop.org/virgl/virglrenderer main at `6fb4cbb7` (2026-11-09).
`qd:` = gitlab.com/marcandre.lureau/qemu-display main. `tree:` = this repository.
`SDK:` = the installed MacOSX.sdk (15.5). "measured on the M2, 2026-11-10" = a probe on
an Apple M2 MacBook Air, macOS 26.4: a C program, ad-hoc signed with
`com.apple.security.hypervisor`, calling Hypervisor.framework directly. The clones are
not in-tree; re-clone at the named commits to re-read.

---

## 1. Goal

Download a DMG, drag Thylacine to Applications, allow it once in System Settings, click
the Dock icon: Thylacine boots inside a native window that can go fullscreen, with the
Mac's clipboard, folders, display scale, audio and power state flowing across. Easy to
get, launch and work with -- the way Parallels makes Windows useful. A custom runner over
a maintained QEMU fork, never bare QEMU, with a guest-side driver for host interop.
Target every Apple-silicon Mac on macOS 15 or later (vote M3), and run best on the
operator's M2 Air (macOS 26.4).

Non-goals (v1): running other guests; USB passthrough; bridged networking; App Store or
notarized distribution; Intel Macs (vote M3).

## 2. Decisions (the operator's, 2026-11-09 and 2026-11-10)

**The goal, in the operator's words.** "I want Thylacine to be useful running on MacOS
under HVF -- Imagine Parallels running Windows on a Mac. In order to be useful, it needs
to be easy to get, easy to launch, and easy to work with -- you should just be able to
click a Thylacine icon in your dock to start it in an "outer shell" window with a
possibility of fullscreen." And: "Don't be bound by the limitations of my Mac M2 Air, we
want to target every Mac, even the newest ones. But it should of course run the best way
possible on mine."

**Constraints stated before the research.**
1. The app OWNS its window; QEMU runs headless underneath and is never visible (no QEMU
   window, Dock icon, menu or name): "I don't want the users to see QEMU."
2. Rust for the app (Swift not required).
3. An unsigned DMG; users allow it via System Settings > Privacy & Security > Open Anyway.
   No Developer ID, no notarization. Ad-hoc signing IS needed: see finding P1.
4. QEMU is patched (a maintained fork and patch queue) to widen hardware 3D on macOS.
5. gpu0 renders into a rect in our own window.

**Votes (2026-11-10; each names the fork of section 11 it settles).**
- **M1** (F1, F3): QEMU is a hidden helper. Our fork runs as an XPC service inside the
  `.app`, shipped as a separate GPLv2 program with a source offer; the app is GPLv3;
  nothing is linked across.
- **M2** (F4): 3D is virgl first (virgl -> ANGLE -> Metal), then Venus (MoltenVK;
  KosmicKrisp on macOS 26+), both in one build.
- **M3** (F9): Apple silicon, macOS 15 or later. No Intel build.
- **M4** (F13): closing the window suspends the VM to disk; Quit shuts it down.
- **M5** (F6): interop is 9P over a virtio-serial port, served by the app and mounted at
  `/mnt/mac`, with the shared folders inside that tree. No `-virtfs` shortcut.
- **M6** (F12): keys are taken while focused, and fullscreen blocks process switching,
  with no permission asked; an optional "full grab" setting asks for Input Monitoring.
- **M7** (F10): updates by Sparkle 2 with EdDSA, pending measurement M-7.
- **M8** (F2): the fork starts from QEMU 11.1.x stable.

The vault records these as `dec-2026-11-10-mac-runner`.

---

## 3. Findings that bound the design

Numbered so the architecture and the forks can cite them. Each is a verified fact unless
marked UNVERIFIED.

### 3.1 Process model and licensing (question A)

**A1. QEMU has an in-process hook, and UTM uses it.** `qemu-10.1:system/main.c:58-66`
declares `int (*qemu_main)(void)`; on Darwin it defaults to `os_darwin_cfrunloop_main`
(`CFRunLoopRun()` on the main thread) and `main()` runs `qemu_default_main` (the BQL main
loop) on a detached thread (`main.c:85-93`). `ui/cocoa.m:2164` sets `qemu_main = cocoa_main`.
The per-target emulator is built as `static_library('qemu-' + target, ...)`
(`qemu-10.1:meson.build:4413`); UTM packages it as a dylib and `dlopen`s it, resolving
`main` / `qemu_init` / `qemu_main_loop` / `qemu_cleanup` by `dlsym`
(`utm:QEMULauncher/Bootstrap.c:15-45`), then calls `qemu_init(argc, argv, envp)`.

**A2. UTM's macOS process model is three processes, not one.** `QEMUHelper` is an XPC
service (`utm:QEMUHelper/QEMUHelperProtocol.h:31` `startQemu:...`; `QEMUHelper.m:119-147`
launches an `NSTask`) that spawns `QEMULauncher`, a separate executable which dlopens QEMU
(`Bootstrap.c:113-120`, or `startQemuFork` at `:92-110` for the fork variant), and
`QEMURenderServer` (virglrenderer's Venus render server, `render-server-mode=process`,
`utm:scripts/build_dependencies.sh:796-800`). Entitlements: `QEMUHelper.entitlements` =
app-sandbox, application-groups, audio-input, allow-jit, files.bookmarks, **hypervisor**,
network.client/server, **com.apple.vm.networking** (signed variant only -- the `-unsigned`
variant drops vm.networking and adds `cs.disable-library-validation`);
`QEMULauncher.entitlements` = app-sandbox, allow-jit, `com.apple.security.inherit`,
hypervisor, vm.networking; `QEMURenderServer.entitlements` = app-sandbox + inherit.
On iOS (no fork/exec of foreign binaries) UTM links QEMU in-process; on macOS it does not.

**A3. QEMU's licence.** `qemu-10.1:LICENSE:8-9`: "The QEMU emulator as a whole is released
under the GNU General Public License, version 2." Lines 11-19: parts carry their own
GPLv2-compatible licences; files with no header are "GPLv2 or (at your option) any later
version"; since July 2013 GPLv2-ONLY contributions are accepted only for `bsd-user/`,
`linux-user/`, `hw/vfio/`, `hw/xen/xen_pt*` -- none of which is in a macOS aarch64
system-emulator build (vfio and xen are Linux-only) -- but older v2-only files may exist
elsewhere, so the *whole* must be treated as GPLv2-only. Consequence (my reading of the GPL
texts and the FSF FAQ, not legal advice): (a) **linking** QEMU (static `.a` or dylib, incl.
`dlopen` of a QEMU-derived dylib that the app calls into) into a GPLv3 app makes one
program whose combined terms must satisfy both licences, and GPLv2-only and GPLv3 are
incompatible -- not distributable; (b) a **separate helper process** talking over sockets /
XPC / shared memory is the "arm's length" case: QEMU stays GPLv2 (we ship its source and our
patches under QEMU's terms), the app stays GPLv3, the guest driver is in-tree GPLv3. The
licence alone forces the helper-process model. UTM's own licence is Apache-2.0
(`utm:LICENSE`), and UTM ships QEMU as separate binaries -- consistent with (b).

**A4. The hypervisor entitlement is required in the process that calls Hypervisor.**
Apple: `com.apple.security.hypervisor` -- "The entitlement is required to use the
Hypervisor APIs in any process" (developer.apple.com, bundleresources/entitlements, macOS
11+). Measured on the M2, 2026-11-10: an ad-hoc-signed C probe carrying that entitlement
got `hv_vm_create` rc=0. So the HELPER binary is the one that must be ad-hoc signed with
the entitlement; the app can carry it too but need not.

### 3.2 Display path (question B)

**B1. The D-Bus display interfaces** (`qemu-11.1:ui/dbus-display1.xml`; 10.1 is identical
but for `Chardev.VCEncoding`): `org.qemu.Display1.VM` (Name, UUID, ConsoleIDs,
Interfaces), `.Console` (`RegisterListener(h fd)`, `SetUIInfo(width_mm q, height_mm q,
xoff i, yoff i, width u, height u)` at xml:94-101, Label/Head/Type/Width/Height/
DeviceAddress), `.Keyboard` (Press/Release by qnum keycode, Modifiers), `.Mouse` (Press,
Release, `SetAbsPosition`, `RelMotion`, IsAbsolute), `.MultiTouch`, `.Listener` (`Scanout`
= pixels in-band, `Update` = damage rect pixels in-band, `ScanoutDMABUF`/`UpdateDMABUF`,
`Disable`, `MouseSet`, `CursorDefine`), `.Listener.Unix.Map` (`ScanoutMap(h fd, offset,
w, h, stride, format)`, `UpdateMap(x, y, w, h)`), `.Listener.Unix.ScanoutDMABUF2`
(multi-plane), `.Listener.Win32.Map` and `.Listener.Win32.D3d11` (`ScanoutTexture2d` with a
shared D3D11 texture handle, `UpdateTexture2d`), `.Clipboard` (Register, Unregister,
Grab(serial, mimes), Release, Request(mime) -> data), `.Audio` + `AudioOutListener`
(Init/Fini/SetEnabled/SetVolume/Write) + `AudioInListener` (Read), `.Chardev` (Register,
SendBreak). The Rust side mirrors this in `qd:qemu-display/src/*.rs` (console_listener,
keyboard, mouse, clipboard, audio, chardev; MIT; zbus 6.2 with the `p2p` feature,
`qd:Cargo.toml`).

**B2. Which scanout kinds exist on macOS.** Server side (`qemu-11.1:ui/dbus-listener.c`):
the Unix.Map shared-memory path is compiled for every non-Windows host (`:536-572`
`dbus_scanout_map` calls `..._unix_map_call_scanout_map_sync` with the surface's
`share_handle`; `:831-839` `UpdateMap`; `:1138` the proxy). DMABUF is `CONFIG_GBM` only
(`qemu-10.1:ui/dbus-listener.c:212-260, 291-405`), i.e. Linux. The D3D11 shared-texture
path is `WIN32 && CONFIG_OPENGL` (`:462-526`). **There is no GPU-texture scanout kind for
macOS**; that is the IOSurface patch (section 4.2). The shared memory behind Unix.Map comes
from `qemu_pixman_image_new_shareable` -> `qemu_memfd_alloc`, which on a host without
memfd falls back to `mkstemp` + `unlink` + `ftruncate` + `mmap(MAP_SHARED)` in `$TMPDIR`
(`qemu-10.1:util/memfd.c:108-131`), and every virtio-gpu 2D resource is allocated that way
(`qemu-10.1:hw/display/virtio-gpu.c:287-296`; the scanout surface inherits the handle at
`:672`). So on macOS the guest's scanout resource is an fd the app can `mmap` directly:
one GPU upload (or one memcpy into an IOSurface) per damage rect, no pixel bytes on the
bus.

**B3. p2p mode on macOS needs no bus daemon.** `-display dbus,p2p=on` makes QEMU wait for
`add_client` (`qemu-10.1:ui/dbus.c:206-208` "wait for dbus_display_add_client()"); the
client is attached by QMP `getfd` (an fd passed over the QMP Unix socket with SCM_RIGHTS)
+ `add_client protocol=@dbus-display fdname=...` (`qemu-11.1:qemu-options.hx:2311`:
"p2p=yes|no : Use peer-to-peer connection, accepted via QMP add_client"). QEMU then speaks
D-Bus as the authentication SERVER on that socket, anonymous auth allowed
(`qemu-10.1:ui/dbus.c:286-335`). The qemu-display crate does exactly this with a
`UnixStream::pair()` and `zbus::connection::Builder::unix_stream(p1).p2p()`
(`qd:qemu-display/src/display.rs:134-205`); its Unix branch leaves the SCM_RIGHTS send as a
FIXME, which is a few lines of `sendmsg`. `addr=` instead connects QEMU to a given bus
address; neither needs a session bus (`dbus.c:209-215`).

**B4. Resize, HiDPI and refresh rate ride SetUIInfo -> EDID.** virtio-gpu generates the
EDID from `req_state[scanout].{width_mm,height_mm,refresh_rate}`
(`qemu-11.1:hw/display/virtio-gpu-base.c:63-79`), which `SetUIInfo` fills (`:111-115`) and
then raises `VIRTIO_GPU_EVENT_DISPLAY` (`:124`). The guest derives its scale from the EDID
millimetres (`tree:docs/HALCYON-SCALE.md:51-57, 85-97`: QEMU's default EDID reads 100 DPI,
so the guest snaps to 100%). So the app sets `width_mm = width_px * 25.4 / (96 * scale)`
and the guest's own scale machinery does the rest. For a large mode the millimetres reach the
guest as whole centimetres only: told no rate, QEMU takes 75 Hz, and gives the EDID no detailed
timing when a side is 4096 pixels or more or the pixel clock would pass 655.36 MHz
(`qemu-11.1:hw/display/edid-generate.c:390, 417-420, 473-474, 500-505`), which is from 6.25
megapixels; the base block's centimetres are cut, not rounded, and the mode is in an extension
block. The guest's rule still gives the Mac's scale there (found by audit round 3 on the app,
2026-11-11; `macrunner/app`'s unit test runs the guest's own `scale.rs` over it). The D-Bus `SetUIInfo` has NO refresh_rate
argument (xml:94-101); UTM carries a QEMU patch adding a `refresh_rate` device property
(mHz) for displays that report none (`utm:patches/qemu-10.0.12-utm.patch:4464-4486`), the
precedent for telling the guest 120 Hz on ProMotion.

**B5. The guest side as it was read on 2026-11-10 (the next paragraph is what was built
from it; line numbers are that day's).** Lictor owns virtio-gpu
(`tree:usr/lictor/src/backend/gpu.rs`): `read_display_info` at boot (~2840),
`query_display_info` only on `mode auto` (~2941, from `backend/device.rs:257`),
`parse_edid_mm` (~2930). A host resize raised the config-change event, which Lictor treated
as stale-wake noise ("an interrupt-ish train from host window resizes", `gpu.rs:448-462`)
and did not re-probe; nothing in `kernel/` or `usr/` read `events_read` for that bit.
halcyond already relayouts live from the compositor's display info
(`usr/halcyond/src/main.rs:584-606`). Live resize was therefore Lictor treating
`VIRTIO_GPU_EVENT_DISPLAY` like `mode auto` (re-probe + EDID re-read + `events_clear`) and
propagating the geometry. Lictor was co-owned by Astra via `docs/handoffs/`, so at that
reading it was a coordination item, not a unilateral edit.

**B5, as built (2026-11-11; the operator, with Astra away: agents may change Lictor, and
leave her a note, `docs/handoffs/050`).** Lictor takes the device's events and counts the
display ones: on the interrupt line the ISR byte's configuration bit says when to look; on
MSI-X the configuration interrupt shares the queues' one vector, so `events_read` is read
once for each interrupt ticket. Each is cleared as it is read. The count goes out on every
reply the broker makes (`Stats.display_events`), so the compositor, which asks the broker
something on every pass, needs no new wake. tapestryd follows a change of the count once it
has stood 150 ms: it asks for the display's size and takes the mode fitted to it, then the
EDID, then the scale (`docs/AURORA-CONFIG.md` 3.4, amended the same day). The fitted mode is
the host's size, or that size over or times a whole number, the first that is within the
mode's bounds and whose screen and full-screen surface can be allocated; the same rule
gives the first mode at boot. An event whose asking finds the device stalled waits for it
to answer; an asking that was made and failed is made again, six times at most; and after a
scanout command that the device did not answer, the compositor's pass binds the screen again
(or has the one client shown draw, whose present binds) for as long as the device is live,
and where the display is composed repaints and pushes everything again. Lictor's own trusted screen is made once, at boot, by the same rule against its
own limits (800x720 at least, 4096x2160 pixels at most), and no longer fails for a host of
another size. An explicit `mode W H` pins the mode until a `mode auto` that is accepted.

**B6. Frame pacing API.** `NSView.displayLink(target:selector:)` returns a `CADisplayLink`
"in-sync with the display the view is on", macOS 14+ (Apple docs JSON, fetched 2026-11-10).
`CVDisplayLink` is deprecated: `SDK:CoreVideo.framework/Headers/CVDisplayLink.h:51`
`API_DEPRECATED_BEGIN("use NSView.displayLink(target:selector:), NSWindow.displayLink...,
or NSScreen.displayLink... ", macos(10.4, 15.0))`. CADisplayLink follows the view's screen,
so a window dragged to a 120 Hz display re-paces itself; the guest's EDID rate is a
separate, static claim (B4).

**B7. IOSurface crossing processes.** `SDK:IOSurface.framework/Headers/IOSurfaceAPI.h:27,30`:
`IOSurfaceCreateXPCObject` / `IOSurfaceLookupFromXPCObject`, macOS 10.7+;
`IOSurfaceRef.h:340-346`: `IOSurfaceCreateMachPort` / `IOSurfaceLookupFromMachPort`.
`IOSurfaceRef.h:101`: `kIOSurfaceIsGlobal` is `API_DEPRECATED("Global surfaces are
insecure", macos(10.6,10.11))`. UTM nevertheless uses the deprecated global path: its QEMU
patch creates the surface with `kIOSurfaceIsGlobal = true`
(`utm:patches/qemu-10.0.12-utm.patch:369`), sends `IOSurfaceGetID` (`:423`) through the
SPICE scanout fd, and CocoaSpice does `IOSurfaceLookup(id)`
(`cocoaspice:Sources/CocoaSpice/CSDisplay.m:474-480`). It works in shipping UTM 5.0.8
(2026-10-17) but is deprecated and any process can look up a global surface. D-Bus carries
fds, not Mach ports, so the IOSurface must cross on an XPC/Mach channel (section 4.1) or
by global ID.

### 3.3 3D on macOS (question C)

**C1. Stock QEMU cannot do GL on macOS.** `egl_init()` has three branches: `WIN32` (ANGLE
D3D), `CONFIG_GBM` (Linux), else `"egl: not available on this platform"`
(`qemu-10.1:ui/egl-helpers.c:702-730`; unchanged in 11.1, `:729`). `virtio-gpu-gl*` is
compiled only `if virgl.found() and opengl.found()` (`hw/display/meson.build:79-82, 102-105`)
and `opengl` means epoxy with `epoxy/egl.h` (`meson.build:1792-1799`). `-display
dbus,gl=on` goes through the same `egl_init`. Measured on the M2: Homebrew QEMU 10.0.2
offers `virtio-gpu-pci`/`-device` only, no `-gl`, displays `none curses cocoa dbus`.
`cocoa.m` has no `display_opengl`/GL path at all (grep in 11.1: none).

**C2. UTM's virgl recipe (what ships today).** Dependencies (`utm:patches/sources`): QEMU
`v10.0.12-utm` (utmapp/qemu release tarball), ANGLE built from utmapp/WebKit
`Source/ThirdParty/ANGLE` with `xcodebuild archive -scheme ANGLE`
(`utm:scripts/build_dependencies.sh:712-729`), libepoxy fork `bf98587` built `-Degl=yes
-Dglx=no` (`:790`), virglrenderer fork `b6c4d36e` built `-Dvenus=true -Dneptune=true
-Dvulkan-dload=false -Drender-server-mode=process|thread` (`:796-800`), MoltenVK fork
`b5680e3`, Vulkan-Loader fork. UTM's QEMU patch (171 KB, 17 subjects) touches
`ui/spice-display-metal.m` (IOSurface-backed scanout textures through
`EGL_IOSURFACE_PLANE_ANGLE` / `EGL_TEXTURE_INTERNAL_FORMAT_ANGLE`, `:311-326`),
`hw/display/virtio-gpu-{gl,virgl,base,rutabaga}.c` (async fencing incl. on the render
server; "don't stall the control queue behind a blob"; deferred unref; refresh_rate),
`target/arm/hvf/hvf.c` (in-kernel GICv3 pre-upstream, a full PMUv3, PMCCNTR without BQL,
**"fix lost interrupt delivery during WFI"** -- a `cpus_kick_thread` / `hvf_wait_for_ipi`
race that "stalls guest interrupts indefinitely", `:124-150`), `hw/arm/virt.c` ("shrink the
high PCIe MMIO window to fit small PA spaces" -- on a 36-bit IPA host the 512 GiB high
window did not fit and large 64-bit BARs such as virtio-gpu hostmem were unusable,
`:552-575`), `ui/cocoa.m`, `hw/display/apple-gfx.m` (macOS 27 SDK). Release notes: v5.0.0
(2026-01-11) added Venus (Vulkan 1.3 for Linux guests) and an "Apple Core OpenGL" backend
giving OpenGL 4.1 on macOS only (no Vulkan); v5.0.2: KosmicKrisp WIP, MoltenVK recommended;
v5.0.6: minimum macOS 12, KosmicKrisp requires macOS 26; v5.0.8 (2026-10-17): updated
virglrenderer/MoltenVK/DXMT. So UTM has THREE host GL/Vulkan routes: ANGLE-EGL (GLES on
Metal), Apple CoreGL (desktop GL 4.1, deprecated API), and Venus on a Vulkan-on-Metal ICD.

**C3. Upstream virglrenderer now supports macOS hosts, including Venus.**
`virgl:meson.build:58` `with_host_darwin`; `:311-313` EGL accepted on darwin/windows
without GBM; `:371-376, 400-407` Venus on darwin adds Objective-C,
`-DVK_USE_PLATFORM_METAL_EXT -DVK_ENABLE_BETA_EXTENSIONS` and the Metal + Foundation
frameworks; `src/meson.build:234` `venus/vkr_metal_helpers.m` (MIT, 2026);
`src/venus/vkr_device_memory.c:355-366`: when the ICD has `VK_EXT_external_memory_metal`
and no `VK_KHR_external_memory_fd`, host-visible memory is allocated as shm and imported as
an `MTLBuffer` (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT`) -- which is how blob /
hostmem mapping works without dma-buf; `server/render_socket.c` has `__APPLE__` branches
(fd passing to the render server). `virgl:COPYING`: MIT. The `neptune` option (D3D11/D3D12
virtualization) exists only in UTM's fork (`meson_options.txt` of `b6c4d36e`, fetched).

**C4. The Vulkan-on-Metal ICDs.** MoltenVK (docs fetched 2026-11-10): "a layered
implementation of Vulkan 1.4", runtime macOS 12.0+, extensions include
`VK_KHR_external_memory`, `VK_KHR_external_semaphore`, `VK_KHR_external_fence`,
`VK_EXT_external_memory_host`, `VK_EXT_external_memory_metal`; **`VK_KHR_external_memory_fd`
does not appear** (hence C3's MTLBuffer path). KosmicKrisp (Mesa docs,
docs.mesa3d.org/drivers/kosmickrisp.html): "a Vulkan conformant implementation for macOS on
Apple Silicon hardware", "implemented on top of Metal 4, which requires macOS 26 and up",
built with `-Dplatforms=macos -Dvulkan-drivers=kosmickrisp`; LunarG: Vulkan 1.3 CTS
conformance (2025), developed for Google "to accelerate the Android emulator on macOS";
upstream in Mesa 26.0; Phoronix reports Vulkan 1.4 conformance (XDC 2026) and MoltenVK
feature parity work (Mesa 26.1-devel). UTM 5.0.6 ships both, KosmicKrisp gated to
macOS 26.

**C5. gfxstream / rutabaga.** QEMU's `virtio-gpu-rutabaga` device depends only on
`rutabaga_gfx_ffi` with no host-OS gate (`qemu-10.1:meson.build:1408-1412`,
`hw/display/meson.build:86-90, 108-112`). gfxstream is the Android Emulator's host renderer
(GLES via ANGLE, Vulkan via a Vulkan-on-Metal ICD -- KosmicKrisp was built for exactly
that, C4); AOSP/Fuchsia commits show a macOS surface path and MoltenVK bring-up.
**UNVERIFIED: rutabaga_gfx + gfxstream building and running under QEMU on a macOS host**
(the crosvm/Mesa READMEs were unreachable: 404 / Anubis). What settles it: build
`rutabaga_gfx_ffi` with the gfxstream feature on the M2 and realise
`-device virtio-gpu-rutabaga` (M-5 in section 9). It would also require a gfxstream GUEST
driver (Mesa's gfxstream Vulkan ICD), which Thylacine's native GPU stack does not have --
the guest speaks virgl (and Venus by design, `tree:docs/GPU-DESIGN.md` 4.5.5).

**C6. Thylacine's guest GPU stack is virgl GL today, Venus tomorrow.** The Warp-C arc
composes on the compositor's own virgl context (`GPU-DESIGN.md` 4.5.11-4.5.13, measured on
thyla-pi; `tree:docs/GPU-DESIGN.md:1396-1480`: the CPU path is PERMANENT and the composed
path is gated on the negotiated `VIRTIO_GPU_F_VIRGL`). Venus is designed (4.5.5) and its
host side verified on two Linux hosts (`GPU-DESIGN.md` 9.1, capset id=4). So a macOS host
that offers virgl GL accelerates the guest NOW; Venus needs the guest client first.

**C7. Scripture position.** `tree:docs/ARCHITECTURE.md:5793` (I-45): guest-exposure half
ENFORCED; host half on virgl/Venus "RESERVED-NOT-ENFORCED ... the host is documented as
TRUSTED, see GPU-DESIGN 9.2". `GPU-DESIGN.md:3002-3023` (9.2): virgl's GL path runs
in-process in QEMU, unsandboxed, ~60 kLOC of guest-facing C with a TGSI text parser, no
fuzzing, CVE history; acceptable because "we are the guest *and* we control the host, on a
development machine. It becomes load-bearing the moment Thylacine is shipped as an untrusted
guest, or runs untrusted guest code with GL access -- at which point this decision must be
revisited." Shipping a Mac app that runs the USER's own Thylacine does not make the guest
untrusted to its owner; it does make the host-half components (ANGLE, MoltenVK/KosmicKrisp,
the render server, our patches) part of the trusted half, and it fires the "untrusted guest
code with GL access" clause the day the guest browser gets GL. See section 7.

### 3.4 HVF across Mac generations (question D)

**D1. The in-kernel GICv3 is MERGED and the hardware works on the M2.** QEMU v11.1.3 has
`hw/intc/arm_gicv3_hvf.c` (`qemu-11.1:hw/intc/meson.build`: `specific_ss.add(when:
['CONFIG_HVF', 'CONFIG_ARM_GICV3'], if_true: files('arm_gicv3_hvf.c'))`), creates the
Apple vGIC with `hv_gic_config_set_distributor_base(cfg, 0x08000000)` /
`..._redistributor_base(cfg, 0x080A0000)` / `hv_gic_create` under `__builtin_available(macOS
15.0)` (`target/arm/hvf/hvf.c:1350-1360`), and the vGIC has full vmstate save/restore
(`arm_gicv3_hvf.c:785-811`). `hw/arm/virt.c:2717-2724`: under HVF, GICv2 is offered only
WITHOUT the in-kernel irqchip and GICv3 WITH it; `:2800-2802` "ITS not supported on HVF
when using the hardware vGIC" (MSIs come from GICv2m, `:2771` -- with the default `its=auto`;
measured: `its=off` removes the MSI controller altogether and PCI falls back to INTx); `:4111-4118`
+ `:4449-4457`: machine types `virt-11.0` and older default to NO in-kernel irqchip, `virt`
(= `virt-11.1`) defaults to it. Measured on the M2, 2026-11-10: `hv_gic_create` with those
bases returned rc=0 and `GICD_TYPER = 0x0478001f` (ITLinesNumber 31 -> 1024 INTIDs, IDbits
16). Homebrew's 10.0.2 imports no `hv_gic_*` symbol (measured).

**D1a. "HVF cannot do GICv3 here" was a guest bug (measured 2026-11-10, M-1; this replaces
the draft's reading that the limit was QEMU's emulated distributor).** With QEMU 11.1.3
and Apple's vGIC the guest still died at GIC bring-up on `assert(isv)`
(`target/arm/hvf/hvf.c:2447`), three boots of three. QEMU's `hvf_data_abort` trace shows
exactly one abort without a syndrome in such a boot: a write at IPA `0x08000c0c`,
`GICD_ICFGR`, on 11.1.3 and on stock 10.0.2 alike. The kernel's `dist_init` loop had
compiled to a post-indexed store (`str wzr, [x10], #0x8`), and ARM gives a hypervisor a
valid syndrome only for a load or store of one general register with no writeback. The
kernel's device accesses now all take the fixed form of `arch/arm64/mmio.h`. With that
fix: stock QEMU 10.0.2 under HVF runs the suite green with GICv3 and the emulated ITS
(2442/0), so the fleet's "HVF means GICv2" is no longer forced; QEMU 11.1.3 refuses GICv2
under HVF unless `kernel-irqchip=off` (`virt.c:2717-2724`; with it off, the suite is green
there too), and with the in-kernel GICv3 the kernel boots and a whole kernel-suite boot
makes 318,344 trapped device accesses, none without a syndrome. Apple's vGIC implements
every INTID (1020), which one kernel test had assumed no controller does. Final build
(1abdae4194c9c021, with that test corrected): QEMU 11.1.3 with the in-kernel GICv3 runs the
WHOLE suite green, 2442/0, four boots of four (three with `its=off`, one with `its=auto`); one of those boots with the trace on made 1,350,398 trapped device
accesses, kernel and userspace drivers together, none without a syndrome. **The machine
line is `-machine virt,gic-version=3` with `its` left at `auto`:** the guest then finds a
GICv2m frame (`gic-msi: v2m first=80 count=64`) and MSIs work; with `its=off` it finds no
MSI controller and every PCI function shares INTx lines. The patch series (Mohamed Mediouni, v1 2025-07 to v20
2026-03-16) carried a known macOS 15 build trap: `HV_SYS_REG_CNTHCTL_EL2` needs the
`macOS 15.0` guard when the deployment target is lower.

**D2. Nested virtualisation.** `hv_vm_config_get_el2_supported` /
`hv_vm_config_set_el2_enabled` are `API_AVAILABLE(macos(15.0))`
(`SDK:Hypervisor.framework/Headers/hv_vm_config.h`); QEMU 11.1 enables it via
`hvf_nested_virt_enable` (`virt.c:3297`, `hvf.c:1311-1345`) and then sets
`ARM_FEATURE_EL2` (`hvf.c:1168-1169`). Measured on the M2: `el2_supported = false`. The
series author: M3 and later only; the macOS 26.3 implementation is nVHE-only. Not needed by
Thylacine (no nested guests); listed for the matrix.

**D3. SME.** `hv_sme_config_get_max_svl_bytes` is `macos(15.2)` (`SDK:hv_sme_config.h`);
QEMU 11.1 reads `ID_AA64SMFR0`/`ID_AA64ZFR0` when `hvf_arm_sme2_supported()` under
`macOS 15.2` (`hvf.c:1177-1190`) and saves/restores ZT0 (`:626, 701`), and masks SME under
nested virt (`:1228-1230`). 10.1 masked SME entirely with the comment "on the M4 there is
SME but not SVE" (`qemu-10.1:target/arm/hvf/hvf.c:908-919`). Measured on the M2: no SME
(the config call fails). A guest must not assume SVE from SME, nor SME at all.

**D4. What `-cpu host` exposes.** QEMU reads `ID_AA64PFR0/1, DFR0/1, ISAR0/1, MMFR0/1/2`
and `MIDR` from a scratch vCPU (`qemu-10.1:hvf.c:863-903`; "Add ID_AA64ISAR2_EL1 here when
HVF supports it"), clamps `PARange` to the VM's IPA size, sets `reset_sctlr =
0x30100180`. PMU: with `kernel-irqchip=on` QEMU forces `ID_AA64DFR0.PMUVer = 1` and the OS
provides a cycles-counter-only PMU; with it off, PMUVer = 0 and QEMU "emulates" a
time-derived cycle counter (`qemu-11.1:hvf.c:1210-1227`). MTE: refused -- "mach-virt: hvf
does not support providing MTE to the guest CPU" (`virt.c:3017-3020`). Measured on the M2
(macOS 26.4, `aux-gate/macrunner/probe`): `MIDR 0x610f0000`, `ID_AA64PFR0 0x1101000011110011`,
**`ID_AA64PFR1 0x0`**, `ISAR0 0x0221100110212120`, `ISAR1 0x0000111110211402`, `MMFR0
0x000010000f100001`, `MMFR1 0x11212000`, `MMFR2 0x1001001100001011`, `DFR0 0x10305006`. So
a guest under HVF on the M2 has LSE and PAC and **no BTI** (PFR1.BT = 0; the boot banner
reads `features: PAC,LSE,CRC32`), no MTE, no RNDR (the kernel seeds from virtio-rng). The
tree's earlier "Apple cores: LSE+PAC+BTI" (`docs/TOOLING.md`) was wrong and is corrected.
**UNVERIFIED per generation** (M1 vs M3 vs M4 vs M5 feature words): the probe prints them;
the public sources I found give ISA levels only (M1 ARMv8.5-A; M4 ARMv9.2-A with SME2, no
SVE, "flagged as 8.7-A by LLVM").

**D5. IPA size and memory map.** `hv_vm_config_get_max_ipa_size` /
`set_ipa_size` are `macos(13.0)`. Measured on the M2: max IPA 36 bits (64 GiB), default
36; `hv_vm_get_max_vcpu_count` = 64. The patch-series discussion mentions a 40-bit maximum
on some macOS 15 systems -- chip-dependent, so measure per Mac. QEMU sizes the virt memmap
from `hvf_get_physical_address_range()` (`qemu-11.1:virt.c:2898-2901`) and disables each
high region that does not fit (`virt_set_high_memmap`, `:2493-2516`): on a 36-bit host the
512 GiB high PCIe window is dropped, so a virtio-gpu `hostmem` BAR of any size has no home
-- the exact problem UTM's "shrink the high PCIe MMIO window" patch fixes (C2). Blob /
hostmem resources on the M2 therefore need that patch (or `hostmem=0` -- no mapped blobs).
The guest reads the same limit: `mmu_pa_bits()` is `ID_AA64MMFR0_EL1.PARange` capped at 40,
and a device claim or a user leaf past it is refused. Before that bound the kernel assumed
40 bits, and the first touch of a leaf above 64 GiB was an address size fault (measured,
`aux-gate/macrunner/m7`: ESR `0x96000003`). A BAR the fork places must stay under it.

**D6. Host pages are 16 KiB.** `qemu-10.1:accel/hvf/hvf-all.c:99-115`: a RAM section not
aligned to `qemu_real_host_page_size()` "can not map as RAM" (it becomes MMIO-trapped).
Stage-2 is 16 KiB-granular; the GUEST's stage-1 granule is its own choice, and Thylacine
has run its 4 KiB kernel under HVF as the default dev loop since 2026-06
(`tree:docs/TOOLING.md:72`). Guest-visible consequences: RAM base/size must stay 16 KiB
aligned (QEMU does this), and any memory the guest shares with the host at page
granularity (blobs, the D-Bus map) is 16 KiB-granular on the host side.

**D7. Timer.** HVF virtualises CNTV; QEMU's HVF code includes WFI handling with a
timed `pselect` and the IPI kick path, where UTM found and fixed a lost-kick race
("fix lost interrupt delivery during WFI", C2). Host sleep stops the vCPUs; what the guest
counter does across a Mac sleep is UNVERIFIED (M-10).

**D8. Intel Macs.** HVF on x86-64 cannot run an ARM64 guest; only TCG can. Apple said at
WWDC 2025 that macOS 26 Tahoe is the last major macOS for Intel, supporting four models
(2019 Mac Pro, 2019 16" MBP, 2020 4-port 13" MBP, 2020 27" iMac). A universal build would
run the guest under TCG at a fraction of HVF speed. QEMU's platform policy: "the most recent
major version at all times for up to five years ... previous major version ... dropped 2
years after the new major version" (`qemu-10.1:docs/about/build-platforms.rst`).

**D9. Minimum macOS.** The in-kernel GIC needs macOS 15.0; SME needs 15.2; QEMU 11.1
guards both with `__builtin_available` so a 12+ deployment target still builds and falls
back to the emulated GICv2 (D1). UTM's floor is macOS 12 (v5.0.6 notes). The Sequoia
Gatekeeper flow the operator chose (P2) exists from macOS 15. KosmicKrisp needs macOS 26.

### 3.5 Host integration (question E)

**E1. Transports available on a macOS QEMU.** virtio-9p local export: supported on
macOS (`qemu-10.1:meson.build:2394-2400` requires Linux or macOS + `pthread_fchdir_np`;
`hw/9pfs/9p-util-darwin.c`), and the dev loop already uses it (`tree:tools/run-vm.sh:474-476`
`-virtfs local,...,mount_tag=host0,security_model=none`). virtio-serial / virtconsole /
virtserialport: present (measured on the M2 device list). vhost-user (hence virtiofs /
vhost-user-fs) and vhost-kernel: Linux only (`meson.build:230-238`); vsock needs vhost ->
**no vsock on a macOS host** (measured: no vhost-* devices). passt: Linux only
(`meson.build:1289`). The guest has NO virtio-serial driver today (grep of `tree:kernel`
and `tree:usr` for `VIRTIO_ID_CONSOLE|virtio-serial|virtio-console`: none). The guest
already speaks 9P over a byte stream it holds: `SYS_ATTACH_9P` runs Tversion and Tattach
over a PAIR OF PIPES (`tree:kernel/syscall.c:2648-2651`,
`sys_attach_9p_ends_are_pipes`: both ends must be `devpipe`) and returns a root Spoor,
which `SYS_MOUNT` grafts into the namespace (`tree:kernel/syscall.c:9127-9146`;
`tree:usr/lib/libthyla-rs/src/territory.rs:177` is the wrapper). So a driver that owns
the port and relays its bytes to a pipe pair gives the session a 9P tree with no new
kernel interface. Drivers are userspace Procs under a warden allowance
(`tree:docs/ROADMAP.md` 3.5; I-34); only the VirtIO transport core is in the kernel
(`tree:kernel/virtio.c`, `virtio_pci.c`).

**E2. Networking.** `-netdev user` (libslirp) works unprivileged; `vmnet-host/-shared/
-bridged` exist on darwin (`qemu-10.1:net/vmnet-*.m`) but `com.apple.vm.networking` is
"restricted to developers of virtualization software. To request this entitlement, contact
your Apple representative" and is "required to use the vmnet APIs" without root (Apple docs
JSON, macOS 10.10+). UTM's unsigned variant drops it (A2). So: slirp, with `hostfwd` for
inbound ports.

**E3. Audio.** Measured on the M2: QEMU audio drivers `none coreaudio dbus wav`; the guest
drives virtio-sound. Either the helper plays through `-audiodev coreaudio` directly, or
`-audiodev dbus` streams to the app over `org.qemu.Display1.AudioOutListener` (B1) and the
app plays via CoreAudio.

**E4. Suspend / snapshot.** The HVF vGIC has vmstate (D1), so `migrate "exec:..."` to a
file and `-incoming` restore (UTM's suspend model) is viable; the disk must be quiesced or
the state paired with the disk image. UNVERIFIED end to end (M-10).

**E5. Disk image.** The pool image is sparse (5 GB apparent, 906 MB real) and must be
copied with `cp -c` (`tree:docs/GPU-HOST-SETUP.md:43-47`); `clonefile(2)` on APFS gives an
instant, space-sharing per-user copy of the shipped template.

**E6. Clipboard ownership.** `tree:docs/HALCYON-INTERACTION.md:25-27` lists "host/guest
clipboard synchronization" outside that tranche; halcyond holds the session clipboard
(`:56`) and the corona arc owns it. The host
bridge must therefore surface the Mac pasteboard as a FILE the session clipboard broker
chooses to read/write (a `/mnt/mac/snarf` file + a change counter), never as a second
clipboard. QEMU's own `org.qemu.Display1.Clipboard` (B1) is a third option that bypasses the
guest's broker and is NOT recommended.

### 3.6 Packaging (question E, continued)

**P1. Ad-hoc signing.** On Apple silicon every native executable must be signed (ad-hoc
suffices), and the entitlement must be in the signature of the process calling
Hypervisor (A4, measured). `codesign --force --sign - --entitlements helper.entitlements`
on the helper; `--sign -` on the app; no hardened runtime (an ad-hoc app with Library
Validation may refuse to load differently-signed frameworks -- Sparkle's docs warn exactly
that for ad-hoc signatures).

**P2. Gatekeeper on macOS 15+.** Apple (2024-08-06): "In macOS Sequoia, users will no
longer be able to Control-click to override Gatekeeper ... They'll need to visit System
Settings > Privacy & Security". Apple support 102445: open the app once, then Privacy &
Security > "Open Anyway", confirm; the app is then saved as an exception. The assessment is
triggered by the `com.apple.quarantine` xattr the browser puts on the DMG and its contents.

**P3. Updates.** Sparkle 2 (sparkle-project.org/documentation): Developer ID is "if
possible", not required; update archives are signed with an EdDSA (ed25519) key
(`SUPublicEDKey` in Info.plist); signed feeds since 2.9. `Sparkle/SUFileManager.m`
(`2.x` branch) implements `releaseItemFromQuarantineAtRootURL:` which removes the
`com.apple.quarantine` xattr from every file of the installed update, so an update does not
re-trigger the P2 flow. **UNVERIFIED:** whether macOS 13+'s "App Management" privacy
prompt fires when an ad-hoc-signed app replaces its own bundle (M-7). Alternative: a
20-line in-app updater (download DMG, EdDSA-verify, swap bundle, strip quarantine).

**P4. Size.** Homebrew's `qemu-system-aarch64` 10.0.2 is 28.8 MB; its Homebrew dylibs
(glib, gio, gobject, gmodule, pixman, gnutls, libslirp, libpng, jpeg-turbo, zstd, snappy,
lzo, capstone, fdt, ncurses, libusb, libssh, vde) total 14.5 MiB (`otool -L` + `stat`);
`share/qemu` is 315 MB of firmware/ROMs that a `-kernel` direct boot does not need. A
single-target fork with only the backends we use (no gnutls/ssh/usb/vde/capstone) lands
around 40-60 MB of code plus ANGLE (~10 MB) plus MoltenVK/KosmicKrisp (~10-30 MB), plus the
guest images (kernel ELF, ramfs, a ~906 MB real / 5 GB sparse pool). DMG with APFS/
compression: budget ~1 GB; UNVERIFIED until built (M-12).

### 3.7 The Rust app (question F)

**R1. Crates (crates.io, fetched 2026-11-10).** `objc2-app-kit` 0.3.2, `objc2-metal` 0.3.2,
`objc2-io-surface` 0.3.2 (all 2025-10-04; the objc2 family also covers Foundation incl.
`NSXPCConnection` behind a feature, QuartzCore/`CAMetalLayer`, CoreVideo); `zbus` 6.2.1
(2026-11-01, `p2p` feature, fds as `zvariant::Fd`); `winit` 0.30.13 stable / 0.31.0-beta.4
(2026-10-24); `qemu-display` 0.2 (MIT, zbus 6.2, in the workspace at `qd:`; its GTK widget
`qemu-rdw` is parked pending a zbus 6 port). Host toolchain measured: rustc/cargo 1.97.1
with `aarch64-apple-darwin` + `aarch64-unknown-none`. XPC: no maintained high-level crate
(`xpc-connection` 2020, `xpc-sys` 0.4.2 2023); the libxpc C API (`xpc_connection_create`,
`xpc_fd_create`, `IOSurfaceLookupFromXPCObject`) is a few `extern "C"` declarations, or
`NSXPCConnection` via objc2-foundation.

**R2. Keyboard capture without Accessibility.** An active app's `NSEvent.addLocalMonitorForEvents`
sees every key event the system does not reserve; UTM uses it (`utm:Platform/macOS/Display/
VMDisplayQemuMetalWindowController.swift:142`, `handleCaptureKeys` `:645-667`: when the
pointer is captured all keys go to the guest; a Cmd key-up is forwarded because "macOS
doesn't like to send Cmd+KeyUp") and `CGAssociateMouseAndMouseCursorPosition(0)` for pointer
capture (`VMMetalView.swift:343`). System-reserved chords (Cmd-Tab, Cmd-H, Cmd-Shift-Q,
Force Quit) are disabled, without any permission, by `NSApplication.presentationOptions`:
`disableProcessSwitching` = "The process switching user interface (Command + Tab to cycle
through apps) is disabled" (macOS 10.6+, Apple docs JSON), plus `disableHideApplication`,
`disableForceQuit`, `disableSessionTermination`; these are kiosk options that work in
fullscreen (UTM toggles `presentationOptions` around its fullscreen space,
`VMDisplayWindow.swift:180-184`). QEMU's cocoa `full-grab=on` is a `CGEventTapCreate(
kCGHIDEventTap, ...)` (`qemu-10.1:ui/cocoa.m:744`) and needs Input Monitoring/Accessibility
-- the tree records "macOS asks once for Accessibility permission for QEMU"
(`tree:tools/run-vm.sh:417-428`). Spotlight (Cmd-Space) and screenshot chords stay with the
system unless the user grants that permission (optional "full grab" setting).

### 3.8 The HVF-only user-facing bugs in the tree

**H1. freeze-172 / flood-174** (`tree:tools/interactive/freeze-172.exp:1-12`,
`flood-174.exp:1-10`): both were GUEST bugs on the serial path (PL011 ICR cleared after the
FIFO drain; the 256-byte cons ring dropping under a flood), both FIXED; the scenarios are
pinned to TCG because TCG's serialised vCPU makes the burst deterministic
(`tree:docs/TOOLING.md:709-710`). The product's keyboard is virtio-input, not PL011, so
the live twin is the open bug "keys are lost behind a client's GPU queue" (OPEN-BUGS
2026-11-05, corona's): Lictor's one thread drains the input eventq once per serve pass, so
a client with GPU work queued delays the drain, QEMU drops a whole report when the queue
is full and tells nobody, and a lost release leaves a key held and parks the seat. A
first fix (eventq 16 -> 64 plus releasing every held key when the queue runs low) did NOT
end it: measured on thyla-pi 2026-11-05, 6 of 9 command lines arrived whole behind a
60-deep chain and the ring read 0 free of 64 three times. It is open, and a Mac app makes
it a daily bug: every keystroke crosses this path. Route: guest (corona and Lictor),
gated by M-9.

**H2. nora-demo** (`tree:tools/interactive/nora-demo.exp:15-16`): "nora's launch CPR
handshake is reliable on TCG and breaks under HVF (bug_nora_hvf_cpr_handshake)". A guest
race exposed by HVF's real parallelism. Route: guest (owned under that memory file; a race
to be hunted per CLAUDE.md, not avoided by TCG).

**H3. A QEMU-side HVF hang exists and UTM fixed it**: the WFI lost-kick race (C2) "stalls
guest interrupts indefinitely and hangs pause_all_vcpus() on guest reset". Route: carry
UTM's patch in our queue until upstream, and re-run every "HVF wedges" repro with it.

**H4. A guest access QEMU cannot decode kills the VM with no message** (found by M-1, D1a).
`hvf_handle_exception` has `assert(isv)` under a comment "TODO: ISV will be 0 for SIMD or
SVE accesses. Inject the exception into the guest" (`qemu-11.1:target/arm/hvf/hvf.c:2443-2447`),
and `assert(!s1ptw)` beside it. To the user of the app that is a window that vanishes. The
guest is fixed (D1a) and gated (`tools/check-mmio-forms.py`), but a product does not abort
on guest input. Route: a fork patch that reports the PC, the IPA and the instruction to
the app over QMP and injects an external data abort into the guest, so the guest names
its own faulting instruction (a `snare:` for userspace, an extinction for the kernel).

---

## 4. Architecture (each piece cites the finding that forces it)

### 4.1 Process model: app + helper + render server

```
 Thylacine.app (Rust, GPLv3)                      Contents/XPCServices/
 +------------------------------+   XPC/Mach      +---------------------------+
 | AppKit window, Metal layer   |<--------------->| thylacine-vmm.xpc         |
 | menus, fullscreen, input     |  socketpair fd  |  = our QEMU fork, headless|
 | 9P server for /mnt/mac       |<--------------->|  -display dbus,p2p=on     |
 | D-Bus p2p client (zbus)      |  D-Bus p2p      |  -accel hvf, in-kernel GIC|
 | CoreAudio (optional)         |  QMP            |  hypervisor entitlement   |
 +------------------------------+  virtio-serial  +------------+--------------+
                                    (9P bytes)                 | fd/socket
                                                   +-----------v--------------+
                                                   | virgl render server      |
                                                   | (Venus; process mode)    |
                                                   +--------------------------+
```

- **One VM per helper process**, QEMU as a separate program (A3 forces it; A2 is the
  precedent). The helper is an XPC service bundled in the app so launchd owns its lifetime,
  Mach-capable messages flow natively (B7: `IOSurfaceCreateXPCObject`), and the entitlement
  sits on the helper binary (A4, P1). The app opens the connection, sends the VM config and
  a `socketpair` end (`xpc_fd_create`) that becomes the D-Bus p2p socket; the helper runs
  `qemu_init` with `-display dbus,p2p=on` and attaches that fd through the in-process
  equivalent of `add_client` (B3) -- a 10-line patch (`dbus_display_add_client(fd)` is
  already the function QMP calls) so no QMP round trip is needed; QMP stays on a second
  socketpair for `system_powerdown`, `screendump`, `migrate`.
- **Crash isolation and restart**: QEMU calls `exit()` and `abort()` freely
  (`qemu-10.1:system/main.c:55` `exit(status)`; `g_assert_not_reached`). In-process that
  kills the window; as a helper it is an XPC invalidation the app turns into a "Thylacine
  stopped (reason)" sheet with Restart. The render server crashing kills only 3D (UTM's
  QEMURenderServer precedent).
- **The guest side** (in-tree, GPLv3) speaks 9P to the app over a virtio-serial port
  (E1; section 4.4). No code is linked across the GPL boundary.
- Rejected: in-process dlopen (UTM-iOS style): GPLv2/GPLv3 incompatibility (A3) and no
  crash isolation. Plain `posix_spawn` child without XPC: needs the deprecated global
  IOSurface ID (B7) or `bootstrap_register` to cross a Mach port; vote M1 chose the XPC
  service from the start.

### 4.2 Display path

**Alpha (stock QEMU behaviour): D-Bus Unix.Map.** The helper runs `-display dbus,p2p=on`
(no `gl`). The app registers a `Listener` whose `ScanoutMap(fd, offset, w, h, stride,
format)` mmaps the guest's 2D scanout resource (B2) and `UpdateMap(x,y,w,h)` marks damage;
on each CADisplayLink tick (B6) with pending damage the app uploads the dirty rects into
an IOSurface-backed `MTLTexture` and presents through a `CAMetalLayer` scaled to the view.
Keys go back as `Keyboard.Press/Release` (qnum codes) on the display connection. The
pointer does NOT go back on it (amended 2026-11-12 under the standing 2026-09-01
authorization; this paragraph first said `Mouse.SetAbsPosition`, and `RelMotion` when the
pointer is captured). QEMU gives a console one pointer mode: that of the pointer device
bound to the console, and with none bound that of the one whose driver came up last
(`ui/input.c`, `qemu_input_is_absolute`). `SetAbsPosition` is refused in the one mode and
`RelMotion` in the other. A guest of ours brings its mouse up after its tablet, so with the
devices as our launchers declared them every position the app sent was refused ("Mouse is
not absolute": seen with the app itself against a QEMU with no guest, 10.0.2 and 11.1.3).
Binding the tablet to the display makes the console absolute for good, which cures that
and rules `RelMotion` out for good. So the app keeps the monitor it attached through and
sends the pointer as QMP `input-send-event`, which takes a position and relative motion
alike: addressed to `gpu0` head 0; a press or a notch in one command with its place, which
the guest is handed as one report; the axis value the app's own, `ceil(x * 32767 / (W - 1))`,
the least value that the compositor's `ax * (W - 1) / 32767` brings back to x (QEMU's own
`x * 0x7fff / W`, which every frontend of its uses, comes back a pixel short). The launcher
binds the tablet to `gpu0` (`display=gpu0,head=0`, the tablet declared after the GPU,
because a bind to a display that does not exist yet fails without a word), so that an
event reaches one device whole; with the tablet bound to nothing QEMU hands the place to
the tablet and the button to the mouse. Relative motion is the captured pointer's, below.
UTM changes the mode with HMP `mouse_set` at each capture instead: a race against a guest
that brings a device up again, on an interface QEMU does not keep stable. What it costs:
QEMU's monitor
serves one client at a time, so the socket the app attached through is the app's while it
runs, and the monitor refuses input for a machine that is stopped. Resize: the app
calls `SetUIInfo(width_mm, height_mm, 0, 0, w_px, h_px)` on live-resize end (B4); the guest
must act on `VIRTIO_GPU_EVENT_DISPLAY` (B5 -- the Lictor change, coordinated with Astra).
HiDPI: the scanout is sized in backing pixels (`backingScaleFactor` x points) and the
millimetres encode the scale, so the guest renders at 200% into a 1:1 texture
(`tree:tools/run-vm.sh:366-376` already does this by hand with `THYLACINE_HIDPI`).
A picture of another size than the view is shown whole, centred, at one scale. A guest that
could not make a mode of the view's size made one of that size over a whole number k and
dropped the remainder (B5), so the view is k times its picture and under k pixels more: the
app draws that picture at k, each guest pixel on k by k of the view's, and the remainder is a
bar. The rule (decided 2026-11-12, under the standing 2026-09-01 authorization): the whole
scale below the filling one is kept when filling the view would make the picture at most one
of its own pixels larger; any other picture is filtered to fill. The rule goes by that
distance alone, so a mode pinned in the guest is drawn whole too where the view comes that
close to a multiple of it. A view of whole points at two pixels to a point is even both ways
and a halved mode divides it exactly, so there the rule changes nothing; what it changes is a
view with an odd side in pixels, a mode over three, and a picture the view has just moved a
little away from. A filled picture takes a rectangle of whole pixels too: the side that bounds
the scale has all of the view's, the other the nearest whole number. The pointer is mapped
through the same rectangle, found in the drawable's pixels, each way by the rectangle's extent
over the scanout's.
Cursor: Thylacine's pointer is NOT in the scanout. Lictor sends it down the virtio cursor
queue (`usr/lictor/src/backend/gpu/cursor.rs`, a 64x64 image), and under `-display dbus`
QEMU forwards it to the listener as `CursorDefine` (image and hot spot) and `MouseSet`
(position, and whether the guest shows one); whoever shows the screen has to draw it, as
QEMU's own cocoa window does. The app makes the Mac's pointer wear the guest's image while
it is over the picture (an `NSCursor`, sized so that an image pixel covers a guest pixel)
and shows none when the guest hides its own; the guest's position is not drawn from, because
the guest only repeats the absolute position it was sent, a round trip late. (The first
window, 4f02edd44, ignored both calls on a misreading of this paragraph's earlier text,
"Thylacine draws its own": it showed the Mac's arrow and no guest pointer.)

The captured pointer (decided 2026-11-12, under the standing 2026-09-01 authorization).
What it is for: the guest hands every motion to the surface that has the keys as deltas
(`TEV_PTR_REL`, `docs/TAPESTRY.md`, "PTR_REL is the mouse-look stream"), exact from its
mouse and made up from consecutive positions of its tablet, and the made-up ones stop
where the picture ends. Captured, the Mac's pointer is put over the middle of the picture,
held there (`CGAssociateMouseAndMouseCursorPosition(false)`, as `utm:Platform/macOS/Display/
VMMetalView.swift:343` and `qemu-11.1:ui/cocoa.m:1065` hold theirs) and hidden. Putting it
there first is this app's own: QEMU's window holds the pointer where it is
(`ui/cocoa.m:1056-1067`), and a pointer held over a menu or over another window would hand
the next click to that. A capture that cannot hold the pointer is not made. Each move goes
to the guest's mouse: `rel` events in a command addressed to no display. A button or a notch goes
the same way, in a command of its own. Measured with no guest on QEMU 10.0.2 and 11.1.3, the tablet bound and
not: such a command is one report on the mouse; a notch is the wheel's button going down
(its coming up is a stray key there too); QEMU passes any value on, and a zero as an event;
and with the tablet bound a position addressed to no display is refused. What is sent is
the event's own motion (`deltaX`, `deltaY`: points, with the Mac's acceleration in them)
times the guest pixels a point covers in the picture, the fraction kept for the next
event, so the guest's pointer goes as far on the glass as the Mac's would have. A button
comes up on the device it went down on, whichever way the pointer is sent by then:
capturing lets go of nothing. The guest's pointer is not the Mac's any longer, so the app
draws it: the image of `CursorDefine` with its hot spot on the place of `MouseSet` (Lictor
reports the hot spot's place; QEMU's cocoa window puts the image's corner there), in the
pass that draws the picture. On release the Mac's pointer is put where the guest's is
(`CGWarpMouseCursorPosition`) and let go. An event the Mac made before the pointer changed
hands, and the app takes after, is not read as one made after: a move is dropped, and a
press or a notch is sent with no place. The guest has one thing to change for this: it
makes a delta of the first position after the mouse's motion, from the last position
before it, which is the whole captured motion a second time; a position that follows
motion from the mouse is to seed and no more (`usr/tapestryd`, and the sentence of
`docs/TAPESTRY.md` that describes the made-up deltas).

Who captures: the person, with View > Capture Pointer, Control-Command-G (beside
Control-Command-F for full screen), and the same lets go. The app lets go by itself when
its window stops being key, when a menu opens and when it is left for another app; while
captured the window's title says how to let go. It also lets go when its window moves,
changes size, changes screen, or enters or leaves full screen (decided 2026-11-12 after
audit round 3, under the same authorization): the pointer is held at a point of the screen,
a window that has moved may no longer be under that point, and the next click would then be
another program's. QEMU's own window lets go on leaving full screen
(`qemu-11.1:ui/cocoa.m:1258-1262`). Weighed and not taken: holding the pointer again over
the picture's new place, which would keep a capture through a resize but has to follow a
window through an animation, and which no test here can try. Plan 9 has no capture, and
no relative pointer for a program at all: one that wants motion past the edge writes the pointer back
to the middle (`mouse(3)`: "Writing the mouse file, in the same format, causes the mouse
cursor to move to the position specified"; 9front's `games/doom/i_video.c` does so
whenever the pointer leaves the inner part of its window), and drawterm on a Mac obeys
with `CGWarpMouseCursorPosition` (`gui-cocoa/screen.m`, `mouseset`). There the program
decides, and that is what this is to grow into: a surface says it wants the pointer (a
word on its `ctl`, as `intent` is one; the SDL port's relative mode is told today and
tells nobody), the compositor passes that on, and the app captures while that surface has
the keys. It needs a way from the guest to the app, which is the interop service's (4.4),
and a word in Tapestry's protocol, which is its owners' to add. The person's chord stays
then as the way out. Rejected: capturing on a click in the window or on entering full
screen (QEMU's cocoa window when the guest's pointer is relative; two of UTM's settings,
`VMDisplayQemuMetalWindowController.swift:442-465`): a guest of ours takes positions, so
most of the time nothing wants deltas, and a capture only takes the Mac's pointer away. A
chord of modifiers alone (UTM's Control-Option, taken the moment both are down,
`VMMetalView.swift:188-201`: it fires inside any chord meant for the guest with those two
in it). Motion straight from the device (`GCMouse`): the guest adds no acceleration, so
its pointer would move a pixel a count; it is the right source for a surface that asks
for the pointer and for raw motion with it, and belongs to that trigger.

**Product: an IOSurface scanout.** For `virtio-gpu-gl` (virgl/Venus) the scanout is a GPU
texture and the DMABUF kinds do not exist on macOS (B2). The fork adds
`org.qemu.Display1.Listener.Darwin.IOSurface` with `ScanoutIOSurface(surface_id u,
width u, height u, format u)` / `UpdateIOSurface(x,y,w,h)` -- the surface itself crossing
as an XPC object over the helper's XPC channel keyed by that id (B7), never as a global
surface. QEMU-side, virgl's scanout texture is bound to an IOSurface through ANGLE's
`EGL_ANGLE_iosurface_client_buffer` (UTM's `spice-display-metal.m` hunks are the recipe,
C2) and Venus scanouts through `VK_EXT_external_memory_metal` / an `MTLTexture` on the
IOSurface. The app wraps the IOSurface as an `MTLTexture` and samples it: zero copies, the
compositor's frame lands on glass one blit later. The 2D path should also move to an
IOSurface (QEMU writes the resource into IOSurface memory) so there is one presentation
path; Unix.Map stays as the no-patch fallback.

**Pacing.** `NSView.displayLink` (B6) at the display's rate (60 Hz on the M2 Air panel;
120 Hz on ProMotion externals): present only on damage, never block the D-Bus thread; the
guest's EDID claims the refresh rate the app passes (B4; the fork adds `refresh_rate` to
the D-Bus `SetUIInfo` or uses UTM's device property). Multiple displays: one QEMU console
per head is the D-Bus model (`ConsoleIDs`); v1 ships one head, and a window moving between
displays just re-paces (B6) and re-sends `SetUIInfo` if the scale changes.

### 4.3 3D route (ranked)

| rank | route | host pieces (all TRUSTED under I-45's host half) | guest needs | M2 Air (macOS 26.4) | every Mac |
|---|---|---|---|---|---|
| 1 | **virgl GL over ANGLE (GLES on Metal)** | upstream virglrenderer (C3) + libepoxy(EGL) + ANGLE from WebKit (C2) + our `egl_init` Darwin branch + IOSurface scanout | nothing new: tapestryd's virgl composition (C6) | works today in UTM; GLES 3.x under virgl | Apple silicon; Intel via ANGLE-Metal on Intel GPUs (UNVERIFIED here) |
| 2 | **Venus over a Vulkan-on-Metal ICD** | virglrenderer `-Dvenus=true` (C3) + render server (process mode) + MoltenVK (macOS 12+) or KosmicKrisp (macOS 26+, conformant 1.3/1.4, Google-backed) (C4) | the Venus guest client (designed, unbuilt; C6) | best ceiling; KosmicKrisp is the modern driver | MoltenVK on all; KosmicKrisp on 26+ |
| 3 | virgl GL over Apple CoreGL | UTM's "Apple Core OpenGL" backend (desktop GL 4.1) | same as 1 | deprecated API, Apple-silicon GL 4.1 | all Macs until Apple removes GL |
| 4 | gfxstream via rutabaga | rutabaga_gfx_ffi + gfxstream + ANGLE + a VoM ICD (C5) | a gfxstream guest ICD Thylacine does not have | UNVERIFIED on macOS QEMU | -- |

Recommendation: build 1 and 2 into the same QEMU (one `virtio-gpu-gl-pci` with
`venus=true` offers both capsets); ship 1 at beta, 2 when the guest Venus client lands;
keep 3 as a measured alternative if ANGLE's GLES profile loses something tapestryd's virgl
path needs; drop 4. Blob/hostmem on the M2 needs UTM's PCIe-window patch (D5). The CPU
composition path stays the universal fallback (`GPU-DESIGN.md` 4.5.9) -- the alpha ships
without 3D at all and must be fully usable.

### 4.4 Interop: `/mnt/mac`, drawterm turned around

The app serves ONE 9P tree the guest mounts at `/mnt/mac`. The one new guest piece is a
virtio-serial driver: a userspace Proc under a warden allowance that relays the port's
bytes to a pipe pair, on which the session attaches 9P and mounts the root (E1). The
kernel gains no interface. Everything host-side is a file:

```
/mnt/mac/
  snarf            the Mac pasteboard (UTF-8 text; read = current, write = set);
                   snarfseq: NSPasteboard.changeCount, poll-free via 9P read-blocking
  display/         size (w h px), scale (pct), refresh (mHz), name -- what SetUIInfo sent
  open             write "<path|url>" -> NSWorkspace.open on the host (policy-gated)
  power            read: battery/ac state; write "sleep-ahead"/"resume" events arrive here
  time             host wall clock (ns) + tz, for a resync after resume
  folders/<name>/  the shared folders, served by the app from the chosen host directories
  session          write "shutdown"/"reboot" requests; read = app state
```

Policy lives in the app (which folders, whether `open` is allowed, clipboard direction),
exactly the way a drawterm server decides what it exports. The guest's clipboard broker
(halcyond, corona's arc) decides when `snarf` is read or written (E6): the file is an
offer, not a second clipboard. Shared folders are served by the app's own 9P server, not
QEMU's `-virtfs local`: one transport and one policy point (vote M5). `-virtfs` stays what
it is today, the dev loop's host share (E1).

**This tree is ABI.** The file names and formats above are a draft; the interop station
fixes them in this section before the first byte of either side is written, and from
then a change to them is a format break.

Transport detail: `-device virtio-serial-pci -device virtserialport,chardev=mac,
name=org.thylacine.mac -chardev socket,id=mac,...` is QEMU's side; the app side is a Unix
socket or an inherited fd; the guest driver relays the port to the pipe pair of E1.
9P over a byte stream is exactly what Plan 9 does over `cpu`/`drawterm` lines.

### 4.5 App structure (Rust)

Crates: `objc2-app-kit` (NSApplication, NSWindow, NSMenu, NSEvent monitors, fullscreen via
`toggleFullScreen`, `presentationOptions`), `objc2-quartz-core` (`CAMetalLayer`),
`objc2-metal` (device, command queue, blit), `objc2-io-surface`, `objc2-foundation`
(`NSXPCConnection` or raw libxpc via `extern "C"`), `zbus` 6 p2p (+ the `qemu-display`
crate as the typed proxies; MIT), a 9P server crate (there is none in the tree's userspace
that targets std; write a small one -- the message set is 13 verbs), `clonefile` via libc.
No winit, no wgpu: the window needs native menus, kiosk presentation options, IME and
exact key routing (R2), and the drawing is one textured quad (F11). Threads: main (AppKit
+ display link), D-Bus (one thread that owns both connections and never leaves `block_on`:
zbus 6 has no executor thread of its own and QEMU calls the listener synchronously, M-3),
input (so that the window's thread never waits for QEMU), 9P server, XPC. Rendering with raw Metal keeps the
IOSurface path a two-liner (`newTextureWithDescriptor:iosurface:plane:`).

### 4.6 The QEMU fork

Base **v11.1.3** (vote M8): in-kernel GICv3 + vGIC save/restore + nested + SME are upstream
(D1-D3), so the queue starts small. Patch queue (git branch `thylacine/11.1`, one patch per
file-topic, `git format-patch` into `macrunner/qemu-patches/`, rebased per stable point
release):

1. `ui/egl-helpers.c`: a Darwin branch of `egl_init()` using ANGLE's
   `eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, ..., EGL_PLATFORM_ANGLE_TYPE_METAL)`
   -- upstreamable (mirrors the existing WIN32/ANGLE branch, C1).
2. `ui/dbus-listener.c` + `dbus-display1.xml`: `Listener.Darwin.IOSurface` -- upstreamable
   (mirrors `Win32.D3d11`).
3. `hw/display/virtio-gpu-virgl.c`: IOSurface-backed scanout textures (ANGLE iosurface
   client buffers) -- upstreamable with 1-2.
4. `hw/arm/virt.c`: UTM's "shrink the high PCIe MMIO window to fit small PA spaces"
   (D5) -- upstreamable; check whether UTM has sent it.
5. `target/arm/hvf/hvf.c`: UTM's WFI lost-kick fix (H3) -- upstream candidate; verify
   against 11.1's current kick path first.
6. `ui/dbus.c`: accept the p2p client fd from the command line / inherited fd (4.1) --
   upstreamable as `-display dbus,p2p=on,fd=N`.
7. `SetUIInfo` refresh rate (B4) -- upstreamable as a new interface version.
8. `target/arm/hvf/hvf.c`: an undecodable data abort (ISV=0, or a stage-1 walk fault) is
   reported and injected into the guest as an external abort instead of `assert` (H4) --
   upstream candidate (it is their TODO).
9. Build: single target `aarch64-softmmu`, arm64 host only (vote M3),
   `--disable-*` everything unused (no gnutls, libssh, libusb, vde, capstone, curses,
   cocoa, sdl, gtk, vnc?, spice), `--enable-hvf --enable-dbus-display --enable-virtfs
   --enable-coreaudio --enable-virglrenderer --enable-opengl --enable-slirp`.

Dependencies and bundling: glib/gio/gobject/gmodule, pixman, libslirp, zstd (optional),
libfdt, libepoxy (EGL only), ANGLE (`libEGL.dylib`, `libGLESv2.dylib`), virglrenderer (+
`virgl_render_server`), MoltenVK (`libMoltenVK.dylib`) and/or KosmicKrisp
(`libvulkan_kosmickrisp.dylib` + ICD JSON) with the Vulkan loader. All relinked to
`@rpath` / `@executable_path/../Frameworks` with `install_name_tool` (or built with
`-Wl,-rpath`), every dylib ad-hoc signed. Licences: QEMU GPLv2 (source offer), ANGLE BSD,
virglrenderer MIT (C3), libepoxy MIT, MoltenVK Apache-2.0, KosmicKrisp/Mesa MIT, glib
LGPL-2.1+, pixman MIT, libslirp BSD-3, libfdt BSD/GPL dual -- all compatible with
redistribution alongside a GPLv3 app; glib's LGPL needs the dylib (not static) form.
Size: section 3.6 P4.

### 4.7 Packaging, signing, first run, updates

- Bundle: `Thylacine.app/Contents/{MacOS/Thylacine, XPCServices/thylacine-vmm.xpc,
  Frameworks/*.dylib, Resources/{guest/kernel.elf, guest/ramfs.cpio, guest/pool-template.img,
  keymaps}}`. Per-user state in `~/Library/Application Support/Thylacine/{pool.img,
  disk.img, state/}`; first run `clonefile`s the template (E5) and writes a config.
- Signing: `codesign -s - --deep` is NOT enough for entitlements; sign inner binaries first,
  the helper with `--entitlements` (hypervisor only; no vm.networking, E2), then the app
  (P1). DMG: plain `hdiutil create -format ULMO` (lzma) with an Applications symlink.
- First launch: Gatekeeper refuses (quarantined, unsigned by an identity); the README and
  the DMG background say: open once, then System Settings > Privacy & Security > Open
  Anyway (P2). The app itself, once running, strips nothing.
- Updates: Sparkle 2 with EdDSA signatures and a signed appcast; Sparkle removes
  quarantine from the installed update (P3); measure the App Management prompt (M-7). If
  it bites, the in-app updater alternative (P3).

### 4.8 Networking, audio, time, power, suspend

- Network: slirp `-netdev user` with `hostfwd` rules editable in the app (E2). No vmnet.
- Audio: alpha `-audiodev coreaudio` in the helper (E3); product `-audiodev dbus` -> the
  app's CoreAudio output unit so volume/device selection is the app's (F8).
- Time: `-rtc base=utc,clock=host`; after a host resume the app writes `/mnt/mac/time` and
  raises an event the guest uses to step its clock (D7, M-10).
- Power: app menu Shut Down -> QMP `system_powerdown` (the guest's PSCI path); guest-
  initiated shutdown -> QEMU exits -> XPC invalidation -> the window closes cleanly. Sleep
  ahead: `NSWorkspace.willSleepNotification` -> `/mnt/mac/power` event -> guest flushes;
  resume -> time resync.
- Suspend to disk (vote M4: Close = suspend, like Parallels): `migrate "exec:zstd > state"` with
  the disk images untouched, restore with `-incoming` (E4) -- the vGIC vmstate exists (D1).

---

## 5. Per-Mac-generation support matrix

| | Intel (unsupported, vote M3) | M1 (2020) | M2 (operator) | M3 | M4 | M5 (2025) |
|---|---|---|---|---|---|---|
| accel | TCG only (D8) | HVF | HVF (measured) | HVF | HVF | HVF |
| in-kernel GICv3 (macOS 15+) | n/a | API chip-agnostic; UNVERIFIED on M1 | **works** (GICD_TYPER 0x0478001f, measured) | UNVERIFIED | UNVERIFIED | UNVERIFIED |
| nested virt (M3+, macOS 15+) | no | no | **no** (measured) | yes per series author | yes | UNVERIFIED |
| SME (QEMU 11.1, macOS 15.2+) | no | no | **no** (measured) | no | yes (SME2, no SVE) | UNVERIFIED |
| MTE to guest | no | no (HVF refuses, D4) | no | no | no | no |
| PAC / BTI / LSE | TCG `max` | PAC+LSE; BTI UNVERIFIED | PAC+LSE, **no BTI** (measured: `ID_AA64PFR1 = 0`) | UNVERIFIED | UNVERIFIED | UNVERIFIED |
| max IPA | n/a | UNVERIFIED | **36 bits** (measured) | UNVERIFIED | UNVERIFIED | UNVERIFIED |
| max vCPUs | host cores | UNVERIFIED | **64** (measured) | | | |
| 3D route 1 (ANGLE virgl) | yes (slow guest) | yes | yes | yes | yes | yes |
| 3D route 2 ICD | MoltenVK | MoltenVK | MoltenVK, KosmicKrisp (26.4) | both | both | both |
| min macOS for the product | -- | 15 (vote M3) | 15 | 15 | 15 | 26 ships with it |

The probe that fills the UNVERIFIED cells is M-1's C program (the M2 probe, extended
to print the ID registers from a scratch vCPU); it costs seconds per machine.

## 6. The HVF-only bugs: fix routes (from 3.8)

| symptom in the tree | nature | route | gate |
|---|---|---|---|
| "HVF wedges on a mere held key" (freeze-172/flood-174 pinned to TCG) | fixed guest PL011/cons bugs; the live twin is keys lost behind a client's GPU queue (H1), OPEN | guest: corona's and Lictor's (a deeper eventq did not end it); also run both .exp forced-HVF under the in-kernel GIC | M-9 |
| nora-demo breaks under HVF (CPR handshake) | guest race exposed by real SMP | guest: hunt it (elusive-bug-hunt; `bug_nora_hvf_cpr_handshake`) | M-2 |
| GICv2 only under HVF | a GUEST bug: a post-indexed store to `GICD_ICFGR` that no hypervisor can emulate (D1a) | fixed in the kernel (`arch/arm64/mmio.h` everywhere, gated); GICv3 now runs under HVF on QEMU 10.0.2 (emulated, ITS) and 11.1.3 (in-kernel, GICv2m with `its=auto`) | M-1, done; owed: `run-vm.sh` declares `thylacine.msi` for GICv3 under HVF so the MSI tests stop skipping there |
| QEMU aborts on an access it cannot decode | `assert(isv)` in the fork's base (H4) | fork patch: report it and inject an external abort into the guest | M-2 |
| an unexplained HVF hang, if one recurs | UTM's WFI lost-kick race (H3) | carry UTM's hvf.c patch; re-run every wedge repro with it | M-2 |

## 7. Scripture and coordination owed (before the code each item governs)

1. **This document**: the process model, the `/mnt/mac` protocol (ABI-bearing: file names
   and formats), the fork's patch queue policy, the packaging contract.
2. **ARCHITECTURE.md I-45 (line 5793)**: the host half's trusted set grows from
   "virglrenderer/Venus" to "virglrenderer + ANGLE + libepoxy + MoltenVK/KosmicKrisp + the
   render server + our QEMU patches"; keep RESERVED-NOT-ENFORCED, name the pieces, and add
   the rule that the render server runs out of process (the only isolation gain available).
3. **GPU-DESIGN.md 9.1/9.2 and 2.6**: 9.1's "nothing can be run locally" and 2.6's
   "macOS has neither" become historical once the fork exists; 9.2's threat-model revisit
   clause is triggered in one direction only (section 3.3 C7): record that the user's own
   guest is trusted by its owner, and that GL/Vulkan access for UNTRUSTED guest code (the
   browser) stays closed until virgl is sandboxed or the render server covers it.
4. **TOOLING.md 60-100 / 690-720 and DISPLAY-MODES.md**: a new display mode `app`
   (`-display dbus,p2p=on`), the GICv3-under-HVF flip, `run-vm.sh` growing the fork as an
   optional `THYLACINE_QEMU=` binary. (Done 2026-11-11: `THYLACINE_DISPLAY=app`, and `=dbus`
   with serial left live for tests; both drop `gpu-mmio0`, because a client is handed
   console 0. `THYLACINE_QEMU=` is still owed.)
5. **Lictor (Astra, `docs/handoffs/`)**: handle `VIRTIO_GPU_EVENT_DISPLAY` as `mode auto`
   (B5). **Corona**: the `snarf` file contract (E6). **Nocturne**: nothing changes for
   `-audiodev coreaudio`; the dbus audio route is a host-side swap.
6. **The new guest driver**: virtio-serial (device id 3) joins the warden's allowance
   table (I-34) and the launcher's `thylacine.virtio-pci-serial` token
   (`run-vm.sh:840-890` derives tokens from `-device`). It is an audit-trigger surface:
   its row lands with it.
7. **`docs/ROADMAP.md`**: the arc's entry, and its bound: it takes no Phase 7 ship risk.
8. **`docs/TAPESTRY.md` and `usr/tapestryd` (main and corona)**: a position that follows
   motion from the mouse seeds the made-up deltas and makes none (4.2, the captured
   pointer). Later, with 4.4: the word by which a surface asks for the pointer.

## 8. Phasing

- **Alpha (window, 2D, input, resize, clipboard, one shared folder).** A1: fork v11.1.3,
  headless build with hvf + dbus-display + virtfs + coreaudio + slirp; the in-kernel GIC
  boot (M-1). A2: the Rust app -- AppKit window, Metal layer, Unix.Map listener, keyboard/
  tablet input, fullscreen, menus; the helper an XPC service from the start (vote M1). A3: resize + HiDPI via SetUIInfo (A3a, the app tells the size; A3b, the guest takes a mode for any size told and follows a change: B5 as built). A4: the
  virtio-serial guest driver + mount-on-fd + the app's 9P server with `snarf`, `display`,
  `power`, `session`, and one shared folder under `folders/` (vote M5). A5: packaging -- ad-hoc signed
  bundle, DMG, the Open Anyway walkthrough; clonefile first run. Exit: a stranger installs
  it from the DMG and uses Halcyon for an hour, retina-crisp, copy/paste both ways.
- **Beta (3D, audio in the app, suspend, updates).** B1: ANGLE + libepoxy + virglrenderer
  built and bundled; `egl_init` Darwin patch; `virtio-gpu-gl-pci`; IOSurface scanout
  interface; tapestryd's composed path measured against thyla-pi's numbers (M-4). B2:
  Sparkle updates (M-7). B3: suspend/resume + sleep/resume time sync (M-10). B4: `-audiodev
  dbus` -> app. B5: the HVF bug routes (section 6) closed with evidence.
- **Product.** C1: Venus (render server, MoltenVK/KosmicKrisp) when the guest client
  lands (M-5). C2: ProMotion pacing + refresh-rate EDID (M-6). C3: multiple shared
  folders over the app's 9P, `open`, policy UI. C4: per-generation matrix filled (M-1 on
  every Mac the team can reach).

## 9. Measurement plan for the M2 Air (each names its verdict)

M-1 **In-kernel GICv3 boot.** Build QEMU v11.1.3 (`--enable-hvf --enable-dbus-display
--enable-virtfs --enable-coreaudio --enable-slirp --target-list=aarch64-softmmu`, Homebrew
deps). Boot the ci image with `-machine virt,gic-version=3,its=off,accel=hvf
-cpu host` and `kernel-irqchip=on` (default for `virt-11.1`). Verdict: login reached; the
boot banner's GIC line says v3; `thylacine.msi` behaviour with GICv2m (no ITS) -- the MSI
tests pass or the launcher's `its` token logic needs the `3:*:hvf` case; full suite +
`ci-smp-gate` green under it; time-to-login vs emulated GICv2 (two runs each). Extend the
M2 probe to dump `ID_AA64{PFR0,PFR1,ISAR0,ISAR1,MMFR0,MMFR1,MMFR2,DFR0}` from a
scratch vCPU and record the M2's feature words for section 5.
**Ran 2026-11-10** (`aux-gate/macrunner/m1`, `m2`, `m3`; finding D1a). Login-level verdict:
the whole suite, 2442/0, four boots of four under the in-kernel GICv3 on the final build.
The banner's GIC line says v3. MSI: GICv2m with `its=auto`, none with `its=off`; the
launcher declares `thylacine.msi` for GICv3 under HVF with `its=auto` (f48fc214c), and the
four MSI tests run there. Boot to the marker 71.6 to
84.4 s against 72.5 to 79.0 s on the emulated GICv2: no wall-time difference at this
grain. The ID registers are in D4. Not run: `ci-smp-gate` under it (the full gate is held).

M-2 **The HVF wedge class under the new GIC.** Run `freeze-172.exp`, `flood-174.exp`,
`nora-demo.exp` with `THYLACINE_ACCEL=hvf` on the M-1 build, 10 boots each; then apply
UTM's WFI-kick patch and repeat. Verdict per scenario: pass count, and for nora a
reproduction log of the CPR handshake race (a hunt, not a count).

M-3 **D-Bus p2p + Unix.Map on macOS.** `-display dbus,p2p=on` + a 150-line zbus client
(socketpair, `getfd`/`add_client` over QMP with SCM_RIGHTS, `RegisterListener`,
`ScanoutMap`). Verdict: the fd mmaps (the mkstemp fallback) and pixels match a QMP
screendump; per-frame cost of `UpdateMap` -> Metal upload at 2560x1600 on a 60 Hz link
(ms/frame, %CPU of app and helper); input round trip (keystroke to echo) in ms.

**Ran 2026-11-10 and 2026-11-11, the part that needs no guest** (`macrunner/app/test/headless.sh`;
QEMU 10.0.2 and 11.1.3, unpatched, started `-S -nodefaults -display dbus,p2p=on -device
virtio-gpu-pci`, so no vCPU runs and the console shows QEMU's placeholder). The transport works
on macOS as built: the private D-Bus connection comes up over a socket pair handed in by QMP
`getfd` + `add_client` with EXTERNAL authentication (0.8 ms), `RegisterListener` is accepted, and
`Listener.Unix.Map` delivers the scanout as a file descriptor that maps (640x480, stride 2560,
pixman `0x20020888`). The mapped frame, and the app's own output read back from Metal at 1:1, are
each byte-identical to QMP `screendump`; so is the app's output when it does not advertise
`Unix.Map` and QEMU sends the pixels in `Listener.Scanout` instead. A deliberately flipped shader
turns the comparison red. Not shown by a grey-on-black placeholder: channel order and colour
management, which need a guest frame with colour. Owed under a lease: a real guest, damage rates,
key-to-damage time, the per-frame upload cost at 2560x1600.

Four things that run measured and the design did not say:

1. **QEMU's main loop calls the listener synchronously** (`ui/dbus-listener.c`: the proxies are
   made with `*_proxy_new_sync`, and `ScanoutMap` is `call_scanout_map_sync` with
   `DBUS_DEFAULT_TIMEOUT`). zbus 6 runs a connection on the thread that is inside `block_on` and
   has no thread of its own, so a listener whose thread sleeps stalls the VM: 5019 ms to the
   first frame with the probe's thread asleep, 1.3 ms with both connections on a thread parked in
   `block_on`. The app keeps one thread for D-Bus that does nothing else, and sends input from
   another so that the window's thread never waits for QEMU.
2. **A display link does not tick while the display sleeps** (0 ticks in 4 s with
   `CGDisplayIsAsleep` true; about 57 a second awake). Only presentation may hang off
   `NSView.displayLink`; anything that must happen regardless needs its own source.
3. **AppKit does not deliver a key-up while Command is held.** Reproduced through
   `NSApplication.sendEvent`: Command down, D down, D up, Command up reached QEMU as press 219,
   press 32, release 219, so the guest keeps D held. The app's `NSApplication` subclass hands that
   key-up to the event's window; the same script then gives press 219, press 32, release 32,
   release 219.
4. **The click that activates the window does not reach the guest** (`acceptsFirstMouse:` is NO
   by default; with YES the click arrives). Kept as AppKit has it.

And one thing reading the app's source found, which no well-behaved QEMU shows: **macOS grants a
shared mapping past the end of a file** (1 MiB of an 8192-byte file, measured) and faults on the
read, so `ScanoutMap`'s geometry is checked against the file's length before the map.

**Ran 2026-11-11 with a guest** (five boots in 16 minutes under one lease; the app at `c045b8f24`,
the Halcyon session image baked from `f003526d8`, QEMU 11.1.3, HVF, GICv3, an M2 Air on macOS
27.2.1). The guest's scanout is 2560x1600, stride 10240, pixman `0x20028888`, and maps. The app's
output read back from Metal is byte-identical to QMP `screendump` of the stopped guest in all four
comparisons: mapped, sent in the messages, in the product posture (`THYLACINE_DISPLAY=app`), and in
the boot the app itself sized (M-8). The frame has colour (an amber accent: red over blue in 0.3%
of the pixels sampled, blue over red in none, the same in both pictures), so the channel order is
settled against QEMU's own picture. Colour management is not: the panel was not measured. The
Mac's pointer wears the guest's (`pointer 64x64 hot 4,4 shown` in every leg, and what AppKit draws
of it was looked at). Upload cost at 2560x1600 on the mapped path in a settled session: 1.2 ms
average and 3.6 ms at worst over 87 frames (a frame 1.4 ms); sent in the messages, 1.0 ms over 37
smaller rectangles; in the product posture 1.4 ms, 12.5 ms at worst, over 32. Across a boot (the
app attached before the guest started) the average was 9.2 ms and one upload took 120 ms; what
that one waited for was not isolated. While the probe typed 18 keys the guest damaged about 6
rectangles and 10.6 megapixels a second. From a key sent over the display connection to the
guest's next damage: 7.5 to 42.2 ms, median 11.7. The connection is up in 2.4 ms and `ScanoutMap`
arrives 0.9 ms after the listener registers. Not measured: the CPU the app and QEMU use, a stream
of full frames at the display's rate, the making of a texture.

M-4 **virgl GL over ANGLE.** Build libepoxy (EGL only), ANGLE (from WebKit, UTM's
script), upstream virglrenderer; patch `egl_init` for Darwin; `-device
virtio-gpu-gl-pci -display dbus,p2p=on,gl=on`. Verdict: `tapestryd: gpu up ... virgl=1
capsets=N` in the boot log (the 4.5.9 control line); `tools/warp-host.sh composed` both
legs on the M2 (composed/direct fps vs thyla-pi's 93.1/112.7); `ls-gfx*` byte-identical.

M-5 **Venus on the M2.** virglrenderer `-Dvenus=true` with the render server in process
mode against (a) MoltenVK, (b) KosmicKrisp (macOS 26.4). Verdict: capset id 4 offered;
the Warp-6 V-0 feature word (`lo=0x3000001b` on Linux hosts) reproduced or explained;
blob mapping works only with the PCIe-window patch (`hostmem=256M`) -- record the
`virt_set_high_memmap` warning without it. Optional: build `rutabaga_gfx_ffi` with
gfxstream and try `-device virtio-gpu-rutabaga` -- a pass/fail that settles C5.

M-6 **IOSurface over XPC + pacing.** A helper XPC service creates an IOSurface, sends
`IOSurfaceCreateXPCObject`; the app looks it up and samples it under `NSView.displayLink`.
Verdict: zero-copy confirmed (no memcpy in the profile); latency from helper write to
glass on the 60 Hz panel and on a 120 Hz external display if one is available; the
link's rate follows the display the window is on.

M-7 **Signing, Gatekeeper, updates.** Ad-hoc sign an app bundle whose XPC helper carries
the hypervisor entitlement; `hv_vm_create` from the service succeeds; put the DMG through a
browser download (quarantine set) and record the exact Sequoia/Tahoe "Open Anyway" flow with
screenshots; install a Sparkle update signed with EdDSA and record whether an App
Management prompt appears and whether the updated app launches without a second Open Anyway.

M-8 **HiDPI chain.** After Lictor's re-probe lands: send `SetUIInfo(338, 211, 0, 0, 2560,
1600)` (200% at 2560x1600) and read the guest's `edid ... mm` boot line and the resulting
scale; resize the window and measure guest relayout latency (ms from `SetUIInfo` to the
first frame at the new size).

Measured so far, with no guest (2026-11-11; QEMU 10.0.2 and 11.1.3; `macrunner/app/test/as_guest.py`
reads the device over QEMU's qtest socket as a driver would): after `SetUIInfo(338, 211, 0, 0,
2560, 1600)` a device made 1024x768 answers `GET_DISPLAY_INFO` with 2560x1600, the detailed
timing of its EDID says 338x211 mm, and `events_read` has the display bit. The same holds on a
QEMU started with `-S` (status `prelaunch`): the timer that hands the size to the device is on the
real-time clock (`ui/console.c`, `qemu_console_set_ui_info`), not the machine's. So B4's QEMU half
is as read. The app now tells the size itself, as it attaches and after a resize (A3's app half;
339x212 mm for 1280x800 points, rounded where the probe's figures were cut off). A mode too large
for a detailed timing, 4200x700 points told by the app on this screen (8400x1400), comes back with
no preferred timing and `111x18 cm` for the 1111x185 mm told, on both QEMUs (the size leg since
round 3). What is left of
M-8 needs a guest: that Lictor boots at the size, the scale the guest takes from the
millimetres, and the latency of a follow, which waits for Lictor to follow the event.

**Measured 2026-11-11 with a guest** (the run under M-3). A size told before the guest starts is
the size it starts at. With the device made 1280x800 and no scale on the kernel command line, the
probe's `SetUIInfo(338, 211, 0, 0, 2560, 1600)` on a paused QEMU gives `tapestryd: edid 338x211
mm for 2560x1600 px`, `scale 200 (edid)` and `halcyond: session up 2560x1600`. With the app
attached to the paused QEMU in the probe's place (its window 1280x800 points on this screen) the
lines are `edid 339x212 mm`, `scale 200 (edid)` and `session up 2560x1600`, and the app's picture
of that session is QEMU's byte for byte. Left of M-8 then: the latency of a follow (measured
since; the paragraph after the next).

**And a defect, measured in the same run.** With the device made 3456x2160 (full screen on a
16-inch MacBook Pro), Lictor and tapestryd come up at that size (`scale 100`, from QEMU's own
EDID), the session's first full-screen surface cannot be allocated (`tapestryd:
t_dma_create_weave(89579520) failed -1`, 225 times in 18 s: three buffers against the kernel's 64
MiB a weave), `halcyond: FAIL session connect/create`, and QEMU shows "Display output is not
active". tapestryd's `mode` verb has refused such a size since 2026-07-23; the boot has no rule
for one. Read, not run: under 800x720 pixels, or over 4096x2160 in all, Lictor's own probe fails
(`Screen::new`), and Lictor is the keyboard and the pointer too. The app can reach both: a small
window on a screen with one pixel to a point, full screen on a larger panel. So can QEMU's own
windows and `xres`/`yres`. The fix is the guest's and belongs to this station's guest half: a mode
for any host size (the host's size over or times a whole number, the first whose surfaces can be
made).

**The guest's half, measured 2026-11-11 and 12** (A3b; QEMU 11.1.3, HVF, an M2 Air; the bare image
of 99cd8d55f and of 4dd462eb8). On the image from before it, a device of 640x400 and one of
5120x2880 each came up with no compositor (`gpu up` four times, never `serving`): what was read
above is measured. With it: 640x400 gives Lictor's trusted screen 1280x800 and a session at
640x400; 3456x2160 a session at 1728x1080 with no allocation refused; 5120x2880 the trusted screen
and the session at 2560x1440. The latency of a follow: with the probe telling `SetUIInfo(338, 211,
0, 0, 2560, 1600)` to a session running at 1280x800, the scanout is 2560x1600 214.3 ms later on the
first image and 282.2 ms on the second, on the probe's clock, one sample each; 150 ms of either is
the time the compositor waits after the last event (`QUIET_MS`). That is to the scanout of the new
size, not to a client's first frame at it, and two samples are not a distribution. The guest says
`mode 1280x800 -> 2560x1600` and `scale 100 -> 200 (mode)`. With the app in the probe's place its
window is followed as it attaches, and again after its content is set to 1000x640 points
(2000x1280), and the app's frame after that is QEMU's dump byte for byte; the same with the GPU's
driver on the interrupt line (`gpu PCI IRQ mode=Intx vector=65535`, by a local change that makes
`auto` the line; that branch is in no gate). What a fitted mode costs: full screen on a 14- or
16-inch MacBook Pro is a session of half as many pixels a side, because three buffers of a
full-screen surface pass the kernel's 64 MiB a weave at 5.59 megapixels (3024x1890 needs 65.4 MiB,
3456x2160 85.4). Whether to raise that bound is the kernel's question and is not decided.

M-9 **Input under load.** 120 Hz host autorepeat of a held key into a guest with a 0.6 s
GPU-busy client (H1's repro) on whatever fix corona's arc has by then. Verdict: lost
keys and lost releases (counts), seat parked or not. Also Cmd-Tab/Cmd-H/Cmd-Q routing in fullscreen with the
presentation options, with and without the optional event tap.

M-10 **Suspend/resume and sleep.** `migrate "exec:cat > state"` under the in-kernel GIC
(vGIC vmstate), `-incoming` restore; the guest's monotonic and wall clocks before/after a
5-minute Mac sleep; a `date`-equivalent in the guest after resume with and without the
`/mnt/mac/time` resync.

M-11 **IPA / BAR fit.** `-device virtio-gpu-gl-pci,blob=on,hostmem=1G` on the 36-bit M2:
the `virt_set_high_memmap` outcome with and without UTM's PCIe-window patch.

M-12 **Size and startup.** The stripped fork + dylibs + ANGLE + ICD sizes; `hdiutil` DMG
size; `clonefile` time for the 5 GB sparse pool; click-to-login time (cold and warm).

M-13 **Networking.** slirp throughput (iperf-equivalent through `hostfwd`), DNS, and
whether the guest's Weft stack needs anything slirp does not do (no ICMP, no inbound
without hostfwd).

M-14 **Audio.** `-audiodev coreaudio` with virtio-sound: output latency and dropouts
(nocturne's own gate), then the same over `-audiodev dbus` -> CoreAudio in the app.

## 10. Risks

1. **The fork's maintenance load.** Every patch not upstreamed is rebased per release;
   the IOSurface/ANGLE patches cut across `ui/` and `hw/display/`, UTM's history shows
   ~17 patches held across years. Mitigation: upstream 1, 2, 4, 6, 7 of section 4.6 early;
   keep the queue in-repo with a rebase gate.
2. **ANGLE's build.** Built from WebKit's copy via xcodebuild (C2) -- a large, moving
   dependency with its own macOS-version churn (Firefox hit IOSurface and multi-device
   issues packaging it). Mitigation: pin the WebKit commit as UTM does; budget the first
   build as a chunk.
3. **36-bit IPA on the M2** (D5): without the PCIe-window patch, blob resources are dead.
4. **Guest changes on co-owned code** (Lictor's re-probe, corona's snarf policy, a new
   kernel driver) are coordination items with their own audit rounds.
5. **The trusted host half grows** (C7): virgl's unhardened GL path now sits behind a
   user-facing app. Not a user-vs-user threat (the guest is the user's), but the browser's
   GL access must stay closed.
6. **Gatekeeper UX**: an unsigned app is a two-step install and Apple keeps tightening it;
   a future macOS could remove Open Anyway. The decision is the operator's; the mitigation
   is the in-app updater keeping users on a bundle they already allowed.
7. **Sparkle on an ad-hoc app** (P3 UNVERIFIED).
8. **No Intel build** (vote M3): the four Intel models macOS 26 still supports get nothing.
9. **HVF races** (H2, H3): the product runs HVF always; every TCG-pinned scenario is a
   coverage hole the product cannot have.
10. **virglrenderer upstream's Darwin support is weeks old** (vkr_metal_helpers.m 2026) --
    expect churn; UTM's fork is the proven variant.

## 11. Decision record (the forks, as decided)

Voted forks carry their vote; the others stand as recommended, and the operator may
overturn any of them.

| fork | decided | by | rejected |
|---|---|---|---|
| F1 process model | QEMU fork as a bundled XPC service helper; D-Bus p2p over a socketpair passed by XPC; IOSurfaces as XPC objects | vote M1 | a plain `posix_spawn` child with deprecated global IOSurface IDs (UTM's way); QEMU linked in-process (GPLv2-only against GPLv3, no isolation, one VM per run) |
| F2 fork base | QEMU v11.1.x stable | vote M8 | UTM's `10.0.12-utm` tarball (older base, SPICE-shaped display code); master (a moving target) |
| F3 licensing | QEMU and our patches ship as a separate GPLv2 program with a source offer; app and guest driver GPLv3; no linking | vote M1 | a lawyer's opinion on linking a v2-or-later subset |
| F4 3D route | virgl GL over ANGLE first, Venus (MoltenVK; KosmicKrisp on 26+) second, one build | vote M2 | Venus only; Apple CoreGL in place of ANGLE (kept as a measured alternative, 4.3); gfxstream |
| F5 display transport | Unix.Map shared memory at alpha, an IOSurface scanout (new D-Bus interface, XPC object) at beta | recommended | IOSurface from day one; in-band `Update` pixels |
| F6 interop transport | 9P over a virtio-serial port, served by the app, mounted at `/mnt/mac`, shared folders inside it | vote M5 | folders via `-virtfs local`; 9P over TCP through slirp |
| F7 clipboard | `/mnt/mac/snarf` and `snarfseq`, consumed by halcyond's broker under corona's policy | recommended | QEMU's D-Bus Clipboard interface (it bypasses the guest's broker) |
| F8 audio | `-audiodev coreaudio` in the helper at alpha; `-audiodev dbus` to the app at product | recommended | dbus from day one; coreaudio for good |
| F9 floor | Apple silicon, macOS 15+; no Intel build | vote M3 | macOS 12+ with the emulated GICv2 fallback; a universal binary with a TCG Intel slice |
| F10 updates | Sparkle 2 with EdDSA, pending M-7 | vote M7 | an in-app updater (the fallback if M-7 fails); none |
| F11 windowing | objc2 AppKit, raw Metal, `CAMetalLayer` | recommended | winit and wgpu |
| F12 key capture | a local monitor, plus presentation options in fullscreen; an optional full-grab event tap behind Input Monitoring | vote M6 | always require the permission; never capture system chords |
| F13 close | Close suspends to disk (migrate to file); Quit shuts down | vote M4 | Close asks to shut down; Close keeps the VM running hidden |
| F14 HVF bug fixes | in the guest for H1 and H2, in the fork queue for H3, all gated by M-2 and M-9 before beta | recommended | a beta that ships around them |
| F15 scripture | this document, plus the I-45, GPU-DESIGN and TOOLING amendments of section 7, each before the code it governs | recommended | folding it into GPU-DESIGN and TOOLING |
| F16 the pointer's path | QMP `input-send-event` on the monitor the app attached through; the launcher binds the tablet to `gpu0` (4.2) | aux, 2026-11-12, under the 2026-09-01 authorization | the display connection's `SetAbsPosition` and `RelMotion` (a console has one mode); binding the tablet and nothing else; HMP `mouse_set` at each capture (UTM) |
| F17 capturing the pointer | the person's chord, Control-Command-G, now; a surface's own request once the guest can reach the app (4.2, 4.4) | aux, 2026-11-12, under the 2026-09-01 authorization | capture on a click or on full screen; a chord of modifiers alone; motion straight from the device |

## 12. What was not read, and what is UNVERIFIED

Not read: QEMU's `ui/spice-*` (we do not use SPICE); UTM's Swift renderer beyond the files
cited; Mesa's gfxstream and crosvm's rutabaga READMEs (Anubis / 404); the QEMU 11.0/11.1
wiki changelogs (Anubis) -- the source tree stood in; Apple's HTML docs where the JSON
endpoint was empty (hv_gic_create page -- the SDK header stood in); Thylacine's kernel
virtio transport code beyond `sys_attach_9p` (E1); `docs/AUDIT-TRIGGERS.md` beyond
grep. UNVERIFIED and how to settle: rutabaga/gfxstream on macOS QEMU (M-5 optional);
per-generation ID registers and IPA sizes on M1/M3/M4/M5 (M-1's probe on those Macs);
`kIOSurfaceIsGlobal` still functional on macOS 26 (irrelevant if F1(a)); the App Management
prompt for a self-updating ad-hoc app (M-7); the guest counter across host sleep (M-10);
whether `-display dbus,gl=on` has any further macOS-specific trap after `egl_init` (M-4);
QEMU's exact deployment-target floor for v11.1 (`__builtin_available` guards suggest 12+;
the policy doc gives no number); app size (M-12); whether UTM has sent its PCIe-window and
WFI patches upstream (check qemu-devel before carrying them); that holding the Mac's
pointer and putting it back ask no permission on the macOS this runs on (a person
capturing once settles it: no test does it, because it would take the pointer from
whoever is at the Mac).

## 13. Evidence index (short form; full citations inline above)

- QEMU: `LICENSE:8-19`; `system/main.c:58-93`; `meson.build:230-238,1289,1408-1412,
  1792-1805,2394-2400,4413`; `hw/display/meson.build:79-112`; `ui/dbus-display1.xml`;
  `ui/dbus.c:206-215,286-335`; `ui/dbus-listener.c` (11.1) `:536-572,831-839,1138`;
  `ui/egl-helpers.c:702-730`; `ui/cocoa.m:744`; `util/memfd.c:108-131`;
  `hw/display/virtio-gpu.c:287-296,672`; `hw/display/virtio-gpu-base.c` (11.1) `:63-79,
  111-124`; `accel/hvf/hvf-all.c:99-115`; `target/arm/hvf/hvf.c` (10.1) `:863-919`, (11.1)
  `:626,701,1168-1230,1311-1360`; `hw/arm/virt.c` (11.1) `:2717-2724,2771-2802,2898-2901,
  2493-2516,3017-3020,4111-4118,4449-4457`; `hw/intc/arm_gicv3_hvf.c:785-811`;
  `qemu-options.hx` (11.1) `:2300-2314`; `docs/about/build-platforms.rst`.
- UTM: `QEMUHelper/*`, `QEMULauncher/Bootstrap.c:15-45,92-120`, `*.entitlements`,
  `scripts/build_dependencies.sh:712-800`, `patches/sources`,
  `patches/qemu-10.0.12-utm.patch` (subjects; `:124-150,311-326,369,423,552-575,
  4464-4486`), `Platform/macOS/Display/VMDisplayQemuMetalWindowController.swift:142,645-667`,
  `VMMetalView.swift:343`, `VMDisplayWindow.swift:180-184`; CocoaSpice `CSDisplay.m:474-480`;
  GitHub releases v5.0.0, v5.0.2, v5.0.6, v5.0.8.
- virglrenderer: `meson.build:58,311-313,371-407`, `src/meson.build:234`,
  `src/venus/vkr_device_memory.c:355-366`, `vkr_physical_device.c:316-364`,
  `server/render_socket.c`, `COPYING`; UTM fork `meson_options.txt` (neptune).
- qemu-display: `Cargo.toml`, `qemu-display/src/display.rs:134-205`,
  `console_listener.rs:40-85`.
- Apple: SDK headers `hv_gic.h`, `hv_vm_config.h`, `hv_sme_config.h`, `IOSurfaceAPI.h:27-30`,
  `IOSurfaceRef.h:101,340-346`, `CVDisplayLink.h:51`; developer.apple.com JSON for
  `com.apple.security.hypervisor`, `com.apple.vm.networking`, `NSView.displayLink`,
  `PresentationOptions.disableProcessSwitching`; news 2024-08-06 (Sequoia Gatekeeper);
  support 102445.
- Others: MoltenVK Runtime User Guide; docs.mesa3d.org KosmicKrisp; LunarG/Khronos/
  Phoronix KosmicKrisp announcements; Sparkle documentation + `SUFileManager.m`;
  patchew.org HVF vGIC series v1-v20; crates.io API for objc2-*, zbus, winit; WWDC 2025
  Intel-last coverage.
- Tree: `docs/TOOLING.md:72,95,699-720`; `docs/GPU-HOST-SETUP.md:1-47`;
  `docs/GPU-DESIGN.md:176-196,1396-1480,2975-3023`; `docs/ARCHITECTURE.md:5793`;
  `docs/HALCYON-SCALE.md:51-97`; `docs/HALCYON-INTERACTION.md:20-27,56`;
  `tools/run-vm.sh:326-440,474-476,536-612,840-890`; `tools/interactive/{freeze-172,
  flood-174,nora-demo}.exp` headers; `usr/lictor/src/backend/gpu.rs:444-464`;
  `usr/lib/libthyla-rs/src/territory.rs:168-181`; `kernel/syscall.c:2648-2651,9127-9146`;
  `docs/ROADMAP.md` 3.5; OPEN-BUGS 2026-11-05 (the bug queue).
- Measured on the M2, 2026-11-10: hv_gic works, IPA 36, 64 vCPUs, no EL2,
  no SME; Homebrew QEMU 10.0.2's devices/displays/accels/audio; rustc 1.97.1.
