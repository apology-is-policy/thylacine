# volk vendored source — prune manifest

Upstream: volk (Arseny Kapoulkine's meta-loader for Vulkan; MIT —
`LICENSE.md`), tag `vulkan-sdk-1.4.357.0`.
Tarball: `vulkan-sdk-1.4.357.0.tar.gz` from
`https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz`
sha256 `6400c7b23e24d17e4f04bac49b55b06c4e87677d33398e90344743ec73560ca6`.

Kept byte-pristine: `volk.h`, `volk.c`, `LICENSE.md`. Pruned: everything
else (CMake, CI, test). Every extension load in volk.c is guarded
`#if defined(VK_KHR_*)`, so it compiles against any header vintage —
here the mesa 26.1.6 `include/vulkan` set the venus driver itself builds
against (the `build/clade/venus/include` fetch).

## Why volk (W-4)

Thylacine has no Vulkan loader and no dlopen: the venus ICD is a static
archive whose public entry is `vk_icdGetInstanceProcAddr` (the `-u`
link). vkQuake calls ~130 core `vk*` symbols directly; volk supplies
every one as a global function pointer loaded through
gipa/vkGetDeviceProcAddr — with `volkLoadDevice()` resolving ALL
device-level entrypoints through `vkGetDeviceProcAddr`, which is exactly
the trampoline-caveat discipline (gipa-resolved device/physdev-parented
trampolines branch to NULL loader slots in a loader-less static link;
measured at W-1, `thylacine_prove.c`). The port's `thy_vkloader.c`
filter feeds volk direct `vn_*` symbols for the measured-broken names.
