# BROWSER -- Boosty, a web browser for Thylacine

**Status**: **RATIFIED 2026-09-21** by the operator's vote on section 11
(`vault/record/decisions/dec-2026-09-21-browser-engine-order.md`; proposed at
`c09141da`). **WebKit first, then Servo. No stage 0 -- neither NetSurf nor
`webfs` is built now. The Rust `std` port starts in parallel, owned by the aux
track (`docs/handoffs/041-rust-std-track-to-aux.md`). Effort stays at `xhigh`
for the whole arc, noted in each audit-bearing commit.** Two kernel designs
remain open by intent and come back for a signature in their own scripture
commits: O-1 (guard regions against I-12's wording) and O-3 (shared memory for
unprivileged Procs). The arc's status rows live in `docs/browser-status.md`.

**Audience**: the operator; every Claude session; Astra (Codex), who continues
this arc when Claude credits run out and cannot read `~/.claude` memory --
which is why the research is reproduced here rather than pointed at.

---

## 1. The request

The operator, 2026-09-21, verbatim:

> I will now have another, important arc for you -- the web browser. I believe
> that we now have everything that is needed to have a working web browser to
> some extent (we'll see about how our JIT capability can map to a JS engine).
> I had two options in mind: 1. Ladybird, the spiritual friend, as it also
> comes from an OS that was built from scratch, and is already in Rust. It is
> pre-alpha, but to us it doesn't really matter. 2. Gecko/spidermonkey.
> 3. Perhaps your research will surface another option. One thing I definitely
> do not want is Chrome or anything tied to either Google or Microsoft.

Clarified the same day, mid-research:

- "Tied to Google" means **exactly Blink, V8 and Chromium** -- "not every
  library they touched." Skia, ANGLE, brotli, woff2 and libwebp are acceptable.
- "WebKit, that's a cool option, I am a fan of WebKit myself."
- "Servo is also superbly interesting."

So the field is **Ladybird, WebKit, Servo, Gecko**, plus whatever small engine
makes sense as a first step.

## 2. The short version

1. **Every serious engine needs the same platform work first.** The engines
   differ less in what they demand of Thylacine than in what they give back.
   Section 6 lists that work as an engine-neutral *platform tranche*. Building
   it once makes the engine choice largely reversible.
2. **WebKit is the best-fitting full engine today** (section 4.2), for four
   measured reasons: an upstream GLib-free port exists whose embedding API
   paints into a caller-owned CPU buffer (the shape of a Tapestry surface);
   running with no JIT at all is an upstream-supported AArch64 configuration
   that *keeps WebAssembly*; JavaScriptCore already contains a dual-mapped JIT
   design ("separated WX heap") that is our I-42 model almost line for line;
   and it needs no Rust `std` port.
3. **Servo is the best strategic fit and the natural second engine**
   (section 4.3). It is Rust, embeddable by design, openly governed, and
   accepts new-OS ports upstream. Its gate is a Rust `std` port for Thylacine
   -- large, but worth having for reasons that have nothing to do with
   browsers.
4. **Ladybird is the engine to revisit, not the engine to start with**
   (section 4.1). The operator's premise is substantially right -- it is now
   one third Rust -- but that is exactly the problem today: it needs *both*
   the Rust `std` port *and* the multi-process IPC work, its upstream stopped
   accepting outside code in June 2026, and it is mid-rewrite. Once the
   tranche exists for WebKit and Servo, a Ladybird port is mostly a chrome and
   a dependency list.
5. **Gecko is not a candidate** (section 4.4): no embedding API off Android,
   no single-process mode, and the only recent small-OS port works by
   supplying GTK3 and Wayland, which we do not have and do not want.
6. **The JIT question has a good answer.** Thylacine's dual-map JIT
   (`CAP_JIT` + `SYS_JIT_CREATE`, I-42) maps cleanly onto JavaScriptCore,
   acceptably onto Ladybird's one WebAssembly JIT function, and badly onto
   SpiderMonkey (section 7).
7. **The novel part is confinement by construction** (section 8): a content
   process whose namespace contains no network stack at all. Every other OS
   subtracts authority from a browser process; Thylacine never grants it.

Recommendation: **WebKit first, Rust `std` and Servo as the second track,
Ladybird re-evaluated at its beta.** The vote is section 11.

---

## 3. What the tree offers a ported engine today

Measured at `99e19194` by grep, not recalled. This is the honest starting line.

| Area | Present | Absent |
|---|---|---|
| Toolchain | clang 22 / LLVM `llvmorg-22.1.8` fork with a real `aarch64-thylacine` triple; static libunwind + libc++abi + libc++; C++20 proven on-device (EH, RTTI, threads, TLS dtors, iostreams, `std::filesystem`) | C++23 is untested here (libc++ 22 supports it); no `dlopen` (static-only, #115); no tzdb |
| Rust | native userland is `no_std` on the built-in `aarch64-unknown-none` target over `libthyla-rs`; 144 vendored crates | **no Rust `std` port** |
| Memory (Pouch `mmap`) | anonymous, demand-zero, RW, kernel-chosen address (`SYS_BURROW_ATTACH_LAZY` 83); whole-mapping `munmap`; native `SYS_BURROW_DECOMMIT` 84 | `MAP_FIXED`, `mprotect`, `madvise`, `mremap`, partial `munmap`, file-backed `mmap` -- all `ENOSYS`. No guard pages. |
| Executable memory | `SYS_JIT_CREATE` 101 (dual map, `CAP_JIT`, elevation-only), `SYS_JIT_DESTROY` 102, `SYS_ICACHE_SYNC` 103; `usr/lib/thylajit/thyla_jit.h`; two consumers in tree (DOSBox-X dynrec, llvmpipe's ORC `DualMapMemoryMapper`) | any way to change a page's permissions (I-12: by design) |
| Processes | `fork`, `execve`, pthreads, `poll`, pipes, pty | `socketpair` |
| Local IPC | `AF_UNIX SOCK_STREAM` over `/srv` byte mode, with `SO_PEERCRED` | **`SCM_RIGHTS` fd passing; `shm_open`; `memfd`; `MAP_SHARED`.** Native Weft sharing exists (`SYS_WEFT_SHARE` 81 / `SYS_WEFT_MAP` 82) but is gated to the `CAP_HW_CREATE` driver tier (`kernel/syscall.c:7179`, Weft-7 F1) |
| Network | `AF_INET` through netd (smoltcp), non-blocking, `/net/dns`, `/net/cs` | -- |
| TLS / HTTP | native Rust: `usr/lib/tls` (rustls, `no_std`, RustCrypto provider), `usr/curl`, `usr/https`, `usr/httpd` | **no C TLS library, no libcurl** in the Pouch sysroot |
| C libraries | zlib 1.3.1, libsodium, SDL2 (video, audio, events, GL, Vulkan on Tapestry), Mesa 26.1.6 | ICU, FreeType, HarfBuzz, fontconfig, sqlite, libxml2, libpng/libjpeg/libwebp as sysroot libraries |
| Graphics | OSMesa frontend over llvmpipe (software, via `CAP_JIT`) and over virgl (GPU); Vulkan through Venus; Tapestry C client (`thyla_tap.h`): pixels, damage-rect present, GL source, key/pointer/scroll/configure/focus/close events | **no EGL**; no clipboard; no text-input method |
| Fonts | IBM Plex, Cornucopia, DejaVu vendored; `skrifa` + `zeno` rasteriser inside halcyond (Rust, not a C library) | a system font directory convention |
| Limits (I-32) | 256 MiB default page budget, 4 GiB hard ceiling by spawn-time budget; 256 threads; 256 children; 65536 VMAs; 1024 handles; 128 MiB shared-map pages | -- |

Two of these rows decide most of what follows. **The memory row**: every large
engine's allocator assumes aligned reservations, guard pages, decommit and
partial unmap. **The IPC row**: every multi-process engine assumes fd passing
and shared memory. Neither is a porting detail; both are kernel design.

`docs/NOVEL.md` already names the second one. **Mycelium** -- a native
channel with handle passing, the positive path of I-4 -- was parked on
2026-08-31 because "neither Mycelium nor AF_UNIX has a forcing near-term
driver." A multi-process browser is that driver.

---

## 4. The candidates

Each subsection: what the engine *is* in September 2026 (things changed; see
4.1), precedent on small systems, fit, and cost. Sources are in section 13.
"Verified" means read in the upstream source this week, by a research agent or
by me; "unverified" is marked.

### 4.1 Ladybird

**What it is now.** The premise "already in Rust" was closer to the truth than
my own prior. Swift was announced in August 2024 and abandoned; on 2026-02-23
the project published "Ladybird adopts Rust." Upstream `master` at `ee1487d`
(2026-09-21) has zero Swift files and 460 Rust files: roughly **442K lines of
Rust against 886K of C++**, excluding tests. In Rust today: the HTML parser,
URL, CSS parsing, style, layout, display-list painting, regex, and the LibJS
front end. Still C++: LibCore, LibIPC, LibWebView, LibMedia, the service
processes, the UI. Every Rust crate uses `std`; the toolchain is pinned to
1.98.0.

- **JavaScript**: LibJS has no JIT (removed February 2024; "currently
  JIT-less"). Its interpreter is *assembly generated at build time* from a DSL
  (x86-64 and aarch64 only; CMake hard-errors elsewhere). That is ordinary
  static text: clean under I-12 with no patch. LibRegex is a bytecode VM.
- **WebAssembly**: a Cranelift JIT since May 2026, on by default. One
  function (`Libraries/LibWasm/CraneliftBridge.cpp:317-342`) maps RW, relocates,
  then `mprotect`s to RX. `-DENABLE_CRANELIFT_JIT=OFF` selects the
  interpreter. It is the only executable-memory site outside the seccomp filter.
- **Memory**: a 4 TiB `PROT_NONE` cage for ArrayBuffer and Wasm memory,
  `MAP_FIXED` commit-over-reservation, `mprotect(PROT_READ)`, `madvise` in
  LibGC, mimalloc.
- **Processes**: WebContent per tab, RequestServer, ImageDecoder, WebWorker,
  Compositor (owns the GPU), WasmCompiler, WebDriver, ProcessReaper. **No
  single-process mode.** IPC is `socketpair(AF_LOCAL)` with `SCM_RIGHTS`
  attachments and `memfd`/`shm_open` shared memory. macOS uses a Mach-port
  transport instead, which is the precedent for a fourth, Thylacine transport.
- **Chrome**: Qt, AppKit or Android only. GTK was dropped in July 2026. There
  is a headless view; a new chrome subclasses `WebView::ViewImplementation` and
  `WebView::Application`. The porting document ends at a "TODO".
- **Dependencies** (required): Skia, ANGLE, curl (HTTP/2, HTTP/3, websockets),
  OpenSSL, ICU (exact version), HarfBuzz, fontconfig, FFmpeg, libavif,
  libjpeg-turbo, libpng, libwebp, tiff, wuffs, woff2, brotli, zlib, zstd,
  libxml2, sqlite, simdutf, simdjson, fmt, libtommath, libpsl, mimalloc, SDL3.
  CPU painting is supported (`--force-cpu-painting`).
- **Maturity**: pre-alpha; alpha "still the plan" for 2026 on Linux and macOS,
  beta 2027, stable 2028. 2,088,677 WPT subtests passing (August 2026).
  Twitch, YouTube, ChatGPT, VS Code web and Outlook are reported working.
  JavaScript is slow (Speedometer 3 about 3.9).
- **Governance**: a 501(c)(3); sponsors FUTO, Shopify, Cloudflare, HRF, Proton;
  no Google or Microsoft money; BSD-2-Clause.
- **Upstream is closed.** Since 2026-06-05: "We will no longer accept public
  pull requests" (`CONTRIBUTING.md:4`, verified). SerenityOS itself was dropped
  as a target on 2024-06-03.

**Fit.** The kinship is real and the JavaScript engine is the friendliest of
all to W^X. But a port needs *everything on both lists*: the Rust `std` port
(section 6, P4), the IPC and shared-memory work (P2), the memory work (P1) at
its most demanding (the 4 TiB cage), the longest dependency list of any
candidate, and a new chrome -- and then it lives forever as an out-of-tree
fork of a code base that is being rewritten from C++ to Rust underneath it,
with no route to send anything upstream.

**Verdict.** Not first. Re-evaluate at Ladybird's beta (2027). By then P1, P2
and P4 exist on Thylacine for other reasons, the rewrite has settled, and the
port is a chrome plus libraries.

### 4.2 WebKit

**The ports.** WebKit is several ports over one core. Read from
`Source/cmake/Options*.cmake` on `main`:

| Port | Maintainer | GLib | Network | 2D | Multi-process |
|---|---|---|---|---|---|
| GTK, WPE | Igalia | required (>= 2.70) | libsoup3 | Skia only (Cairo removed in 2.54, 2026-09-16) | yes |
| **PlayStation** | Sony | **none** | **curl + OpenSSL** | Cairo or Skia | yes, Unix sockets |
| Win | Sony-led | none | curl + OpenSSL | Skia | yes |
| JSCOnly | community | optional | -- | -- | -- |

The PlayStation port is the template: no GLib, no GStreamer, no toolkit, and
an embedding C API (`UIProcess/API/C/playstation/`) whose centre is

```
WKViewCreate(...)
WKViewClient.setViewNeedsDisplay(rect)
WKPagePaint(page, unsigned char* ARGB32, size, rect)   /* caller-owned buffer */
WKPageHandleKeyboardEvent / MouseEvent / WheelEvent
```

That is a Tapestry surface: a pixel buffer, damage rectangles, and an event
stream. Its required libraries are ICU (>= 70.1), curl, OpenSSL, FreeType,
HarfBuzz, fontconfig, PNG, JPEG, WebP, libxml2, sqlite, zlib, libpsl, plus EGL
and GLES2. It ships with `ENABLE_JIT`, `ENABLE_DFG_JIT` and `ENABLE_FTL_JIT`
all OFF (verified, `OptionsPlayStation.cmake:176-178`).

**JavaScriptCore without a JIT** is an upstream-supported AArch64
configuration, not a hack: with `!ENABLE(JIT)` the `ExecutableAllocator` is a
stub and no executable memory exists at runtime. The interpreter is LLInt,
assembled ahead of time by `offlineasm`. WebAssembly still runs, on the
in-place interpreter (IPInt; JIT-less calls fixed January 2025, full SIMD in
JIT-less configurations March 2026). Measured costs from the literature: asm
LLInt is about 2x CLoop; JIT-less loses roughly two thirds on Speedometer
against the full tiers.

**JavaScriptCore with a JIT, under strict W^X**: section 7. In one sentence:
the mechanism we need is already in the tree, Darwin-only, and porting it is a
three-site patch.

**Precedent.** Haiku (one maintainer, monthly merges, still on the
single-process WebKitLegacy downstream; its WebKit2 effort stalled on IPC,
shared memory and EGL -- *our* P2 and P5, named by someone else), MorphOS
Wayfarer (two developers, tracks a WebKitGTK tree), RISC OS Iris, Managarm
(WebKitGTK, tripped on `/proc/self/maps` and netlink). Upstream already
carries OS guards for Fuchsia, Haiku, Hurd and QNX.

**Costs.**
- Size: about 200 MB of C++ (order 5-6 M lines); 1.5-2 GB of RAM per unified
  build job. It cross-compiles first-class; the build runs on the host.
- C++23, libc++ >= 19, and Perl, Python, Ruby and gperf at build time
  (host-side only).
- **No Rust.** A forward risk instead: Swift is entering the tree
  (`ENABLE_BACK_FORWARD_LIST_SWIFT`, default OFF when cross-compiling).
- A new `Platform/IPC` and `SharedMemory` backend (P2).
- EGL (P5).
- LGPLv2 on WebCore and JavaScriptCore. Thylacine links statically, so LGPL
  section 6 applies: ship relinkable objects or complete source. Thylacine is
  open and builds from source, which satisfies it.
- Governance: Apple controls review; Igalia (11% of 2025 commits) and Sony
  are real stakeholders. No Google or Microsoft ownership. Only Apple and
  Igalia publish security advisories -- see open question O-2.
- Compatibility: Interop 2025 score 99. The best available outside Blink.

**Verdict.** The strongest first engine.

### 4.3 Servo

**What it is now.** A Linux Foundation Europe project maintained by Igalia
since 2023 (17 TSC members, 25 maintainers; 146 contributors and 3,183 merged
PRs in 2025; a EUR 545,400 Sovereign Tech Fund grant). It became a real
embeddable library this year: `servo` 0.1.0 reached crates.io on 2026-04-13,
releases are monthly (0.5.0 on 2026-08-31) with a half-yearly LTS track and
breaking changes each month. WPT score 48.2% -> 61.6% across 2025 (93.4% of
subtests).

- **Embedding**: `ServoBuilder`, `WebViewBuilder`, `WebViewDelegate`, and a
  `RenderingContext` trait the embedder may implement itself
  (`make_current`, `present`, `resize`, `size`, `read_to_image`, the GL API
  handles). `retsurf` runs Servo on SDL2 + GLES 3 with its own media backend
  -- the closest existing shape to ours.
- **Rendering**: WebRender draws through GL only (GLES 3.0 or GL 3.2). No
  Vulkan path. The "software" context is Mesa llvmpipe underneath, not a
  separate rasteriser. Our OSMesa frontend (llvmpipe or virgl) can back an
  embedder-owned `RenderingContext` without EGL; whether virgl's GLES 3 is
  adequate for WebRender's shaders is unverified and is an early probe.
- **Processes**: multi-process is an optional feature, and its sandbox crate
  is compiled out on aarch64. In practice on ARM64: **one process, no
  sandbox.** That removes P2 from the critical path, and it means the OS must
  be the containment (section 8).
- **JavaScript**: SpiderMonkey through `mozjs`, hard-wired by the DOM
  bindings generator. Section 7 has the detail: a no-JIT build is supported
  and tested in Servo's CI, at the price of **no WebAssembly**.
- **Dependencies**: `std`; hyper + rustls + aws-lc-rs (a C/asm/CMake build;
  rustls + ring is the fallback); harfbuzz and freetype (C); icu4x (Rust);
  rusqlite bundled; the pure-Rust `image` crate; media optional.
- **New-OS precedent**: FreeBSD became buildable in March 2026 with a
  59-line PR; OpenHarmony took about 2.5K lines across servoshell and surfman
  -- but OHOS is `target_os = "linux"`, so it needed neither a `std` port nor
  a new entry in Mozilla's closed OS list. **Redox is the true precedent**:
  Servo has run there since October 2025 (crashes on a second page load, no
  keyboard yet).

**The gate is Rust `std`** (section 6, P4). With a musl-like libc the cheap
route is a `target_family = "unix"` target reusing `sys/pal/unix`: the Hurd
port was +626 lines in `rust-lang/rust` and +3,297 in the `libc` crate; QNX
+603; a bespoke platform layer costs about four times that (Motor OS +2,297,
Xous +2,484) and pays worse, because most crates gate on `cfg(unix)`.

**Verdict.** The second engine, and the reason to do the `std` port. Not
first, because its compatibility and JavaScript story are materially behind
WebKit's and because its arrival date is set by toolchain work rather than by
browser work.

### 4.4 Gecko and SpiderMonkey

- Gecko has **no supported embedding** outside Android (GeckoView). Widget
  backends: android, cocoa, gtk, headless, uikit, windows. A new one is on
  the scale of `widget/gtk` (about 2.1 MB of source).
- The one recent small-OS port, Haiku's Firefox 153, **did not write a widget
  backend**: it runs GTK3 over an X/Wayland compatibility layer. Build: 5-6
  hours, 16 GB.
- Single-process has been unsupported since Firefox 124. IPC is Unix sockets
  with `SCM_RIGHTS` and `memfd_create`.
- All of Gecko's Rust links as one archive; `std` is mandatory. It is mandatory
  even for SpiderMonkey alone (`jsrust` depends on the `wast` crate).
- The licence (MPL-2.0) has no Google tie. The steward's income does: search
  royalties, overwhelmingly Google's, have been roughly 75-88% of Mozilla's
  revenue, and the September 2025 remedies ruling preserved them.

**Verdict.** Not a candidate. SpiderMonkey matters only as Servo's engine.

### 4.5 NetSurf, as an optional stage 0

C99, its own small libraries (libcss, libdom, hubbub, libnsfb), GPLv2. Release
3.11 (December 2023, added flexbox); commits through February 2026. Its own
porting guide sizes a new frontend: **about 1K lines for a proof of concept,
7K for basic use, 15K complete**, with five of twelve operation tables
mandatory, and recommends bringing up the headless `monkey` frontend first.
`libnsfb` abstracts a linear pixel buffer. JavaScript is Duktape (ES5.1), off
by default in practice; documents render, web applications do not.

It is what small systems ship first: RISC OS (its origin), Haiku, Redox,
ToaruOS, KolibriOS -- and **9front, whose port requires `webfs` to be running**,
because its fetcher is the Plan 9 HTTP file server rather than curl. That last
fact is the interesting one for us (section 8.1).

**Verdict.** Worth doing only if the operator wants a browser on the device
in weeks while the tranche is built. It is not on the path to the real engine
except through the libraries it shares (FreeType, libpng, libjpeg) and the
`webfs` service it would cause us to build.

### 4.6 Considered and set aside

- **Dillo** (3.2.0, FLTK, no JavaScript): weaker than NetSurf and drags FLTK in.
- **Text browsers**: w3m 0.5.6 and ELinks 0.19.1 are near-free on the TUI;
  chawan (Nim) is the most capable but brings a toolchain. None is "a web
  browser" in the sense asked for. Worth a day as a convenience, not an arc.
- **Blitz** (Rust: Stylo + Taffy + Vello, no JavaScript): a renderer, not a
  browser; needs `std`.
- **Gosub**: its scripting crates are V8-based. Excluded by the constraint.
- **Flow, Ultralight, Sciter**: proprietary.
- **Standalone JS engines** for any small-engine work: QuickJS-NG (ES2023,
  interpreter only, MIT) is the pick; a NetSurf fork has already swapped
  Duktape for it.

---

## 5. Side by side

| | WebKit | Servo | Ladybird | Gecko | NetSurf |
|---|---|---|---|---|---|
| Language | C++23 | Rust + C++ (mozjs) | C++23 + Rust (1/3) | C++ + Rust | C99 |
| Needs Rust `std` (P4) | **no** | yes | yes | yes | no |
| Needs fd passing + shm (P2) | yes | **no** (single process) | yes | yes | no |
| Needs the memory work (P1) | yes | yes | yes, most demanding | yes | little |
| JS with no executable memory | asm LLInt, supported upstream | interpreter or PBL, tested in CI | asm interpreter, the only mode | PBL | Duktape |
| WebAssembly with no JIT | **yes** (IPInt) | **no** | yes (interpreter) | no | none |
| JIT under strict W^X | **design exists in tree** (3 sites) | invasive patch, est. 0.5-2K lines, per-ESR rebase | one function | as Servo | n/a |
| Toolkit-free embedding | **yes** (paints to a caller buffer) | **yes** (`RenderingContext`) | no chrome for us; write one | no | yes (`libnsfb`) |
| Process isolation available | per tab + network process | none on aarch64 | per tab + services | per site | none |
| Web compatibility | Interop 99 | WPT 61.6% | pre-alpha, climbing fast | Interop parity | documents only |
| Upstream accepts our port | OS guards accepted before; policy unverified | **yes** (FreeBSD, OHOS, tier-3 Rust) | **no** (closed June 2026) | no | yes |
| Rebase burden | monthly on `main`, or a stable branch | monthly breaking API; LTS track | fast-moving, mid-rewrite | very high | low |
| Licence | LGPLv2 + BSD | MPL-2.0 | BSD-2 | MPL-2.0 | GPLv2 |
| Controlled by | Apple, with Igalia and Sony | LF Europe / Igalia | a non-profit | Mozilla (Google-funded) | volunteers |
| Size | ~5-6 M lines | ~Ladybird-sized + mozjs | 0.9 M C++ + 0.44 M Rust | ~21 M | small |

---

## 6. The platform tranche (engine-neutral)

Work that Thylacine needs before any full engine runs, with who needs it. This
is the real content of the arc's first months, whichever engine wins.

**P1. The anonymous-memory surface.** Needed by: all. What allocators and
garbage collectors assume, and we lack: (a) reserve address space without
commit, at a chosen alignment; (b) commit and decommit inside a reservation;
(c) release part of a mapping; (d) an inaccessible guard region; (e) an
RW -> R transition (Ladybird). We already have demand-zero lazy attach and
`SYS_BURROW_DECOMMIT`, so (a) and (b) are largely a Pouch wiring job
(`madvise(MADV_DONTNEED)` -> decommit) plus an alignment argument or a
partial-detach. (d) and (e) are the design question: **I-12 says a page is
writable XOR executable and that *no* permission-mutation syscall exists.**
Transitions among {none, R, RW} on never-executable anonymous memory do not
touch the W^X property, but they do contradict the second sentence as written.
That is an `ARCHITECTURE.md` section 28 amendment and needs the operator's
signature before a line of kernel code -- it is not decided here. The
alternative that needs no amendment is to give engines what they ask for
differently: guard regions as *never-committed reservation holes* (a lazy
region that refuses to fault in), which covers (d) without any mutation.
Audit-bearing (page fault + W^X row, overcommit row, `burrow_attach` row).

**P2. A channel that can carry memory.** Needed by: WebKit, Ladybird, Gecko.
Not by single-process Servo or NetSurf. Engines want two things: a
bidirectional message channel between related processes (we have `/srv` byte
streams and pipes; `socketpair` is a small addition) and **the ability to hand
a block of shared memory to the peer**. The kernel mechanism half-exists:
Weft already shares an anonymous Burrow across Procs through a consume-once
share id delivered over a 9P session -- which is precisely the I-4 positive
path -- but `SYS_WEFT_SHARE` is gated to the driver tier. The design question
is whether the browser's need is served by (i) generalising that gate under
the existing `PROC_SHARED_MAP_MAX_PAGES` bound, or (ii) building Mycelium
proper. Either way it is its own design document, read against the Weft-7
audit that added the gate, and it is scripture. Both engines isolate the
platform half cleanly (`Platform/IPC` + `SharedMemory` in WebKit; a fourth
`Transport` beside the Mach one in Ladybird), so we implement *our* primitive
and not an emulation of `SCM_RIGHTS`.

**P3. The C libraries.** ICU, FreeType, HarfBuzz, sqlite, libxml2, libpng,
libjpeg-turbo, libwebp, libpsl, and for WebKit and Ladybird **curl and
OpenSSL**. Routine Pouch ports; ICU's data is linked statically (no file
`mmap`). fontconfig is required by WebKit's PlayStation port and by Ladybird;
either a port or a small static-configuration shim (open question O-4). Each
new POSIX surface a port touches gets a Pouch patch and the Pouch audit
discipline.

**P4. Rust `std` for Thylacine.** Needed by: Servo, Ladybird, Gecko,
SpiderMonkey. A `target_family = "unix"` target over the Pouch libc: a
`rustc_target` spec, a `libc` crate module (the larger half), `std::os::thylacine`,
small arms in `sys/pal/unix`, the unwinder hookup. Out of tree first (a pinned
toolchain and a forked `libc`), tier 3 upstream once stable. Then the crate
tail: mio (has a `poll(2)` selector; one line), socket2, nix, rustix,
getrandom, ring or aws-lc. **This has value far beyond the browser**: it opens
crates.io to Thylacine. It also raises a question this document only flags:
ARCHITECTURE section 3.5 splits userspace into native `no_std` on
`libthyla-rs` and ported POSIX code on Pouch. A `std` target *on Pouch* is a
third thing -- Rust programs that are POSIX-shaped. Whether that is only for
ports, or becomes a sanctioned way to write new Thylacine programs, is the
operator's call (open question O-5).

**P5. A GL entry point engines recognise.** We have OSMesa over llvmpipe and
over virgl, and no EGL. WebKit's GLib-free ports require EGL + GLES2 at
configure time; Servo can be handed an embedder-owned context. Options: a
small EGL-over-OSMesa shim (surfaceless and pbuffer only), or patching the
engine's compositing to a CPU path. Probe before designing.

**P6. The desktop contract.** Clipboard, text input beyond raw keys, a
system font directory, a certificate store, a downloads location, and a page
budget for browser processes well above the 256 MiB default (the spawn-time
budget mechanism exists; the policy does not).

---

## 7. How the JIT capability maps onto each engine

Thylacine's model (I-42, I-12): there is no way to change a page's
permissions. Executable code is published through `SYS_JIT_CREATE`, which
returns **two views of the same memory** -- a writable, never-executable one
and an executable, never-writable one -- to a holder of `CAP_JIT`, and
`SYS_ICACHE_SYNC` makes new instructions visible.

**JavaScriptCore: the same design, already written.** `ExecutableAllocator.cpp`
contains `initializeSeparatedWXHeaps`: reserve the pool, `mach_vm_remap` it to
a second random address, make the original R+X and the alias R+W, and route
every write through one function, `performJITMemcpy(dst, src, n)`, which in
this mode calls a thunk with the *offset into the pool*. On Apple hardware the
thunk is execute-only with the writable base burned in as an immediate, so the
writable address is never readable -- a hardening we could adopt. The mode is
compiled only for `OS(DARWIN) && HAVE(REMAP_JIT)` on ARM64 (verified,
lines 189-334), and used only when fast permission switching is unavailable.
Everywhere else JavaScriptCore maps its pool permanently RWX, which Thylacine
cannot do and would not want. Porting the mode is three sites: replace the
`mach_vm_remap` + `vm_protect` sequence with `SYS_JIT_CREATE`; stop the
initial reservation asking for RWX; give `ARM64Assembler::cacheFlush` a
Thylacine arm calling `SYS_ICACHE_SYNC` (it is a hard `#error` today).
Unverified, and the first thing the JIT chunk must establish: that *every*
writer goes through the chokepoint (the Apple fast-permission mode imposes the
same discipline, which is good evidence and not proof). The YARR regex JIT,
the CSS selector JIT and the Wasm tiers share the one allocator.

**Ladybird: nothing to do for JavaScript; one function for WebAssembly.**
LibJS and LibRegex never generate code. The Cranelift bridge's generic path is
map-RW, fill, `mprotect`-to-RX; on a dual map it becomes fill-through-the-
writable-view, sync. Or switch it off.

**SpiderMonkey (Servo, Gecko): no dual mapping exists, and adding one is
invasive.** `ProcessExecutableMemory.cpp` has three modes: `mprotect` flips
(the engine default), permanent RWX (what Firefox actually ships in content
processes, except on OpenBSD), and Apple's `MAP_JIT`. The permission *scopes*
are few (`AutoWritableJitCode`, `AutoMarkJitCodeWritableForThread`), but those
guards change permissions in place while a dual map changes the **address**:
the linker copy-out, every `PatchWrite_*` and `patchJump` in the ARM64
assembler, GC relocation tracing and Wasm static linking must all learn a
constant RW-RX delta. Estimated at 0.5-2K lines across 15-25 files, rebased
every ESR; nobody upstream wants it. Without it, the supported mode is
`--disable-jit` or the Portable Baseline Interpreter (`--enable-portable-
baseline-interp`; 1.26x the C++ interpreter on Octane; needs no runtime code
generation) -- and **no WebAssembly**, because SpiderMonkey has no Wasm
interpreter.

**Who holds `CAP_JIT`.** It is elevation-only (I-2): never inherited at fork,
granted through the `cap` device. Under WebKit only WebContent processes would
ask for it, which makes "JIT on or off, per site" a *capability decision* the
system makes, not an engine preference -- a browser-level Lockdown Mode for
free. The first ship is JIT-less everywhere; the JIT arrives as its own
audited chunk.

---

## 8. Heritage, the state of the art, and the Thylacine shape

### 8.1 Heritage

Plan 9 put HTTP behind a file server in 1995. `webfs(4)` mounts at `/mnt/web`:
open `clone`, get a connection directory `n/` holding `url`, `ctl`, `body`,
`contenttype`, `postbody` and `parsed/`; *opening `body` issues the request*.
`webcookies(4)` is a separate server. `mothra` and `abaco` are thin renderers
over it, `hget` is a shell script over it, and 9front's NetSurf port fetches
through it. The renderer never owns a socket.

### 8.2 State of the art

- **SerenityOS -> Ladybird**: WebContent per tab, RequestServer, ImageDecoder
  with a fresh process per image, each confined by `pledge` and `unveil`. When
  Ladybird left SerenityOS it kept the process split and **lost the
  confinement**, and ran without a real sandbox until seccomp/Landlock and
  Seatbelt landed in June 2026. The architecture is portable; the confinement
  is the operating system's contribution.
- **Fuchsia** (`fuchsia.web`): a `Context` owns state for a set of `Frame`s and
  is created with a `service_directory` (the capabilities the engine may use)
  and an isolated `data_directory`. The engine reaches only what was routed
  to it.
- **Genode**: the browser is an ordinary component holding routed GUI, NIC and
  file-system sessions; a second GUI-server instance is interposed as a video
  bridge so that the capability graph, not the application, mediates the
  webcam. Its authors also say the uncomfortable part aloud: tabs inside one
  browser do not feel as secure as separate components.
- **Everyone's escape hatch** is a Linux VM with a mainstream browser (9front
  `vmx`, Sculpt's VirtualBox), and everyone describes it as unsatisfying.

### 8.3 The Thylacine shape: confinement by construction

`pledge`, `unveil`, seccomp, Landlock and Seatbelt are **subtractive**: a
process is born with the machine's ambient surface and hands pieces back.
What remains is still a reachable network stack, a reachable filesystem root
and a syscall table to probe. Thylacine is **constructive**: a Proc can name
only what was bound into its namespace (I-1, I-28), holds only the
capabilities it was spawned with (I-2), and cannot receive a handle except
over a session someone gave it (I-4).

So the browser is a set of Procs whose namespaces are built, not filtered:

| Proc | Namespace contains | Capabilities |
|---|---|---|
| **chrome** (one; the UI process) | its Tapestry surfaces and input; the profile directory; `/srv` names of its own children | none beyond a user program's |
| **fetch** (one per profile; WebKit's network process, or `webfs`) | `/net`; the certificate store (read-only); the cookie and cache directories | none |
| **content** (one per tab, later per site) | one channel to chrome, one to fetch; fonts (read-only); **no `/net`, no `/srv`, no `/proc`, no `/dev`, no home** | `CAP_JIT` only if policy grants it |
| **decode** (short-lived, per image or media item) | one channel; nothing else | none |

Properties that fall out, and that subtraction cannot express:

1. **No network stack to reach.** A compromised content Proc does not face a
   filtered socket API; `/net` does not exist for it. Its only route outward is
   a request to the fetch Proc, which applies policy with full knowledge of who
   is asking (`SO_PEERCRED`-style peer identity is already in the tree).
2. **Revocation is closing a session.** Tearing down the channel ends the
   authority completely and at once.
3. **Decode in a throwaway Proc is already our pattern.** I-47 put image
   decoding in the short-lived `view` Proc and crossed the raster to halcyond
   as a bounded write. The browser's decoders are the same design.
4. **The JIT is a granted capability, not a build option** (section 7).
5. **The Genode bridge trick applies**: camera, microphone and audio go
   through Nocturne and Tapestry sessions the chrome chooses to route, never
   through devices the content Proc can name.

This is a `docs/NOVEL.md` candidate -- *the browser as a capability graph:
site isolation enforced by the namespace, not by the engine* -- and it is
engine-independent. With WebKit it costs almost nothing, because WebKit
already separates UI, network and web processes; we only decline to give the
web process a namespace worth attacking. With single-process Servo it
degrades to confining the whole browser, which is weaker and is an honest
argument for WebKit first.

An invariant is owed and not yet allocated: *a content Proc's authority is
exactly its constructed namespace and its granted capabilities; it never holds
a network, service-registry, process or device name.* Proposed as the next
free section-28 number at ratification, with a deny-path probe as its witness
(BOOT OK does not prove a gate is wired; only a refused attempt does).

### 8.4 `webfs`

> Not built now (D-2, 2026-09-21). Kept as the record of the heritage shape and
> of why it is not WebKit's network layer.

Independent of the engine, a native `webfs` is worth having: `/mnt/web` for
`rc`-style scripts, `hget`, the manual reader, NetSurf if stage 0 is chosen.
It is a small native Rust 9P server over the rustls we already ship. It is
**not** proposed as WebKit's network layer: WebKit's curl backend is about
fifteen thousand lines of cookies, cache, authentication, HTTP/2 and
WebSockets, and rewriting that over 9P would be a second browser project.
WebKit's own network process, confined as in 8.3, *is* the heritage shape.

---

## 9. The proposed arc

Assuming the recommendation is accepted. Each phase closes on a witness; each
kernel phase is audit-bearing and preceded by its own scripture commit.

| Phase | What | Exit |
|---|---|---|
| **B-0** | **JavaScriptCore alone** (`JSCOnly` port, no JIT) cross-built for Thylacine. This is WebKit's own recommended first step for a new OS, and it answers the operator's JIT question early and cheaply. | `jsc` runs test262 samples and a benchmark on the device; the list of P1 gaps it actually hit, measured |
| **B-1** | P1, scoped by what B-0 measured. Scripture first if I-12's wording moves. | the allocators run unmodified or with a Thylacine arm; SMP gate; audit |
| **B-2** | The JIT: separated WX heap on `SYS_JIT_CREATE`; `CAP_JIT` clearance for `jsc`. | same benchmark with JIT tiers; a deny-path probe (no `CAP_JIT` -> interpreter, never RWX); audit |
| **B-3** | P3: the libraries, ICU first. | each library's own tests under Pouch |
| **B-4** | P2: design document, then the primitive, then WebKit's `Platform/IPC` + `SharedMemory` backend. | two-process message + shared-bitmap witness; audit |
| **B-5** | WebCore + WebKit2 bring-up, headless: `PORT=Thylacine`, modelled on PlayStation. P5 resolved here. | `WKPagePaint` renders a local page to a PNG on the device |
| **B-6** | The chrome on Tapestry; P6; the constructed namespaces of 8.3 with their deny-path probes. | a page loads over TLS from the network in a tile; content Proc proven unable to open `/net` |
| **B-7** | Hardening, fuzz posture, the invariant's ENFORCED flip, the Operator's Manual section. | arc close |
| **R** (parallel, **owned by aux**, started 2026-09-21) | P4: Rust `std`, then the crate tail, then Servo with PBL in a confined single Proc. | a `std` hello-world built by cargo for a Thylacine Rust target and run on the device; later, servoshell-equivalent pixels |
| ~~S-0~~ | ~~`webfs` + NetSurf on Tapestry.~~ **Not built: the operator voted "neither" on D-2.** | -- |

B-0 is deliberately small and deliberately first: it costs days, it tests the
toolchain (C++23, libc++, WTF), it measures P1 instead of guessing at it, and
it produces a JavaScript engine Thylacine can use whatever happens next.

---

## 10. Risks

1. **The fork tax.** Every option is a permanent downstream. Haiku's WebKit is
   one person merging monthly. Mitigation: keep the port in platform
   directories, change shared code only behind a Thylacine guard, and offer
   upstream what upstream will take.
2. **Swift in WebKit.** Optional today and off when cross-compiling. If it
   becomes mandatory, the choices are a Swift toolchain port or freezing on a
   branch. Watch it; do not bet against it silently.
3. **Security maintenance.** A browser is the most attacked program on any
   system and only Apple and Igalia publish WebKit advisories. Section 8.3 is
   the structural answer -- assume the content Proc falls, and make that
   worth little -- but it does not remove the duty to track fixes (O-2).
4. **P1 touches I-12's wording** and **P2 touches I-4's positive path.** Both
   are real kernel design with real audit cost. They are also things
   Thylacine needs anyway; the browser is the forcing function, not the cause.
5. **Performance without a JIT** is a two-thirds Speedometer loss. B-2 exists
   to end that, and is scheduled early on purpose.
6. **Build weight.** WebKit wants tens of gigabytes of host RAM for a
   parallel build. Host-side only; thyla-pi cannot build it.
7. **Scope.** This is a multi-month arc with two kernel designs inside it. The
   fallback structure of ROADMAP section 11 applies: nothing here may put the
   v1.0 release candidate at risk.

---

## 11. Decisions for the operator

> **VOTED 2026-09-21.** D-1: **(a) WebKit, then Servo.** D-2: **neither** --
> no NetSurf and no `webfs` now (the operator cut the recommended `webfs`).
> D-3: **in parallel, now, owned by aux** ("I will launch Aux to deliver the
> Rust STD"). Effort: **xhigh throughout.** The options are kept below as the
> record of what was weighed.

**D-1. Which engine first?**
- **(a) WebKit, then Servo** -- recommended. Best fit today; the only engine
  with a W^X-clean JIT design in its tree; process isolation that matches 8.3;
  no `std` prerequisite. Servo follows on track R.
- **(b) Servo first.** Rust-native and upstreamable. Slower to a usable
  browser (the `std` port and the mozjs cross-build come first), weaker
  compatibility, no WebAssembly without a patch nobody upstream wants, no
  process isolation on ARM64. The `std` port is valuable regardless.
- **(c) Ladybird first.** The kinship choice. Needs P1, P2, P3 at their
  largest and P4, on a closed upstream, mid-rewrite, before its alpha.
- **(d) Platform tranche first, engine later.** Build P1, P2 and P4 as
  Thylacine features in their own right and choose in 2027. Defensible, but
  B-0 is so cheap and so informative that deciding blind seems worse.

**D-2. Stage 0?** NetSurf + `webfs` for a browser on the device in weeks
(GPLv2 frontend, documents only), or skip it and keep every hour on the
tranche. Recommended: **build `webfs` regardless; NetSurf only if an early
browser matters to the operator** -- it is cheap, and it is a side quest.

**D-3. Track R now or later?** Start the Rust `std` port in parallel
(it is independent of the browser and Astra or aux could own it), or sequence
it after B-5.

### Open questions this document does not settle

- **O-1** P1's shape: reservation holes versus an I-12 wording amendment.
  Decided in B-1's scripture commit, informed by B-0's measurements.
- **O-2** Which WebKit line to track: `main` (where the PlayStation port
  lives) or Igalia's stable branches (which carry the security backports but
  are only tested for GTK and WPE). To be measured at B-5.
- **O-3** P2: generalise the Weft share gate, or build Mycelium. Its own
  design document.
- **O-4** fontconfig: port, or a static-configuration shim.
- **O-5** Whether Rust-`std`-on-Pouch is for ports only or a sanctioned third
  way to write Thylacine programs (ARCHITECTURE 3.5).
- **O-6** The name -- **RESOLVED 2026-09-21: the browser is named Boosty**,
  after the operator's cat ("We need a name for the browser. I want to name it
  'Boosty' after my cat."). The operator's name outranks the thematic
  candidates this document had held (Sighting, Range, Benjamin), which are
  withdrawn. The engine keeps its own name (WebKit); `webfs` keeps its Plan 9
  name. Program name `boosty`; the chrome lives at `usr/boosty/` when B-6
  builds it.

---

## 12. What this document deliberately does not do

It does not allocate an invariant number, amend I-12, design P2, choose a
name, or add an audit-trigger row. Each of those happens in the scripture
commit of the phase that needs it, with the operator's signature where
CLAUDE.md requires one.

---

## 13. Sources

Research was done on 2026-09-21 by six parallel agents reading primary
sources, then spot-checked. Items I verified myself are marked (v).

**Ladybird** -- upstream clone at `ee1487d` (v: zero `.swift`, 460 `.rs`,
`CONTRIBUTING.md:4`, `rust-toolchain.toml` 1.98.0, `Libraries/LibJS/CMakeLists.txt:337`,
`Libraries/LibWasm/CraneliftBridge.cpp:317-342`, no `no_std` crate);
https://ladybird.org/posts/adopting-rust/ ;
https://ladybird.org/posts/changing-how-we-develop-ladybird/ ;
https://ladybird.org/newsletter/2026-05-31/ , `/2026-06-30/` , `/2026-08-31/` ;
`Documentation/Porting.md`, `Documentation/FAQ.md`, `vcpkg.json`.

**WebKit** -- `Source/JavaScriptCore/jit/ExecutableAllocator.{cpp,h}` (v: lines
189-334 and the `performJITMemcpy` chokepoint), `Source/cmake/OptionsPlayStation.cmake`
(v: dependency list, JIT OFF at 176-178), `Source/cmake/WebKitFeatures.cmake`,
`Source/WTF/wtf/PlatformEnableCocoa.h`, `Source/WebKit/Platform/IPC/unix/ConnectionUnix.cpp`,
`Source/WebKit/UIProcess/API/C/playstation/`;
https://docs.webkit.org/Ports/Introduction.html ;
https://webkitgtk.org/2026/09/16/webkitgtk-2.54-highlights.html ;
https://bugs.webkit.org/show_bug.cgi?id=286390 ; https://bugs.webkit.org/show_bug.cgi?id=35154 ;
https://webkit.org/blog/17899/introducing-the-jetstream-3-benchmark-suite/ ;
https://webkit.org/blog/17808/interop-2025-review/ ;
https://trac.webkit.org/wiki/SuccessfulPortHowTo ;
https://www.haiku-os.org/blog/zardshard/2024-08-16_gsoc_2024_porting_webkit2_final_report ;
https://discuss.haiku-os.org/t/webkit2-status/16901 ; https://webkit.org/licensing-webkit/ .

**Servo and Rust `std`** -- https://servo.org/blog/2026/04/13/servo-0.1.0-release/ ;
https://servo.org/blog/2026/08/31/july-in-servo/ ; https://blogs.igalia.com/mrego/servo-2025-stats/ ;
https://github.com/servo/servo/pull/37972 ; https://github.com/servo/servo/issues/30541 ;
https://github.com/servo/mozjs ; https://github.com/mxmgorin/retsurf ;
https://www.redox-os.org/news/this-month-251031/ ;
https://github.com/rust-lang/rust/pull/115230 ; https://github.com/rust-lang/libc/pull/3325 ;
https://doc.rust-lang.org/rustc/target-tier-policy.html .

**Gecko and SpiderMonkey** -- `js/src/jit/ProcessExecutableMemory.cpp`,
`js/src/jit/JitOptions.cpp`, `js/src/wasm/WasmFeatures.cpp`, `js/moz.configure`,
`modules/libpref/init/StaticPrefList.yaml` on `mozilla-firefox/firefox@main`;
https://cfallin.org/blog/2023/10/11/spidermonkey-pbl/ ;
https://bugzilla.mozilla.org/show_bug.cgi?id=1855321 ;
https://github.com/mozilla-spidermonkey/spidermonkey-embedding-examples ;
https://discuss.haiku-os.org/t/progress-on-porting-firefox/13493 ;
https://www.npr.org/2025/09/02/nx-s1-5478625/google-chrome-doj-antitrust-ruling .

**NetSurf and the small engines** --
https://ci.netsurf-browser.org/jenkins/job/docs-netsurf/doxygen/md_docs_implementing_new_frontend.html ;
https://www.netsurf-browser.org/about/news ; https://github.com/netsurf-plan9/nsport ;
https://www.phoronix.com/news/Redox-OS-April-2025 ; https://bellard.org/quickjs/ .

**Precedent** -- https://man.9front.org/4/webfs ; https://man.9front.org/1/mothra ;
http://fqa.9front.org/fqa8.html ;
https://github.com/SerenityOS/serenity/blob/master/Documentation/Browser/ProcessArchitecture.md ;
https://fuchsia.dev/reference/fidl/fuchsia.web ;
https://genodians.org/nfeske/2022-01-27-browser-odyssey ;
https://managarm.org/2023/12/31/end-of-year-update.html ;
https://www.openbsd.org/papers/eurobsdcon2022-landry-taming_the_fox.md .

**Unverified and flagged in the text**: that every JavaScriptCore writer uses
the `performJITMemcpy` chokepoint; virgl's GLES 3 adequacy for WebRender; the
size estimate for a SpiderMonkey dual-map patch; WebKit's current policy on
accepting a new port; Ladybird's test262 figure.
