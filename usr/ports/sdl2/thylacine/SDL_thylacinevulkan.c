/* SDL_thylacine — the Vulkan window glue (Warp-WSI W-3e).
 *
 * The Thylacine Vulkan surface class is stock VK_EXT_headless_surface: the
 * venus driver's W-3d WSI (vn_wsi_thylacine) sits behind the headless
 * platform slot and turns swapchain images into compositor presentables,
 * so this file's CreateSurface is one vkCreateHeadlessSurfaceEXT call plus
 * THE ARMING MOVE — the two-sided display consent:
 *
 *   surface half: thyla_tap_glsrc(tap, <warp ctx pub>) — this window's
 *     tapestry surface accepts the venus renderer's warp ctx as its
 *     display source;
 *   ctx half: vn_renderer_thylacine_set_surface(tap.id) — the renderer's
 *     present poke (W-3d, dormant until now) names this surface at every
 *     vkQueuePresentKHR.
 *
 * Same mutual-adoption shape as the GL direct path (gl_bind in
 * SDL_thylacineopengl.c); either half alone is inert server-side. The ctx
 * half is armed only after the surface half lands, so no present ever
 * pokes a surface that has not consented. The armed surface global
 * survives window destroy uncleaned: harmless, because a present can only
 * come from a swapchain, a swapchain needs a surface, and the next
 * CreateSurface re-arms before any such present exists (one window per
 * process, sequential).
 *
 * LINKING MODEL (why every venus symbol here is WEAK): the vtable wiring
 * in SDL_thylacinevideo.c pulls this .o into EVERY SDL program, and
 * GL-only programs must still link — a weak undefined resolves to NULL
 * instead of erroring, and LoadLibrary then fails cleanly at run time.
 * The flip side is an ld semantics trap: A WEAK REFERENCE DOES NOT
 * EXTRACT ARCHIVE MEMBERS. A program that reaches Vulkan only through
 * SDL_Vulkan_* (vkQuake's shape) references no venus symbol strongly, so
 * putting libvulkan_virtio.a on its link line pulls NOTHING and these
 * weaks stay NULL. Such a program must force the ICD member in:
 *
 *     -u vk_icdGetInstanceProcAddr
 *
 * on the link, which drags the whole driver closure via the ICD dispatch
 * table. A program that calls any vn_/vk_icd symbol itself needs nothing.
 *
 * No loader, no dlopen: vk_icdGetInstanceProcAddr is resolved BY SYMBOL
 * and handed to SDL core as the vkGetInstanceProcAddr. Instance-level
 * resolution through it is real dispatch; programs must fetch
 * device-level entrypoints through vkGetDeviceProcAddr (the loader-less
 * pattern — the trampoline caveat from the venus prove applies).
 */
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_THYLACINE

#include "../SDL_sysvideo.h"
#include "../SDL_vulkan_internal.h"

#include "SDL_thylacinevideo.h"
#include "SDL_thylacinevulkan.h"

/* The two venus-side consent calls (vn_renderer_thylacine.c). Signatures
 * mirror vn_renderer.h; weak per the linking model above. */
extern void vn_renderer_thylacine_set_surface(uint32_t surface_id)
    __attribute__((weak));
extern uint32_t vn_renderer_thylacine_warp_ctx_pub(void)
    __attribute__((weak));

/* The mesa ICD entry. The real return type is PFN_vkVoidFunction; declared
 * as the equivalent bare function pointer so this file needs no vulkan.h
 * (the vendored khronos tree is pruned from this port). */
typedef void (*ThyVkVoidFn)(void);
extern ThyVkVoidFn vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *name)
    __attribute__((weak));

typedef ThyVkVoidFn (*ThyVkGipa)(VkInstance instance, const char *name);

/* VkHeadlessSurfaceCreateInfoEXT, hand-declared (3 fields; sType 4 bytes +
 * pad, pNext at 8, flags at 16 on LP64 — the real struct's layout).
 * 1000256000 = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT. */
#define THY_VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT 1000256000
typedef struct ThyVkHeadlessSurfaceCreateInfoEXT
{
    int32_t sType;
    const void *pNext;
    uint32_t flags;
} ThyVkHeadlessSurfaceCreateInfoEXT;

typedef int32_t (*ThyVkCreateHeadlessSurfaceEXT)(
    VkInstance instance, const ThyVkHeadlessSurfaceCreateInfoEXT *createInfo,
    const void *allocator, VkSurfaceKHR *surface);

static const char *const thy_vk_instance_exts[] = {
    "VK_KHR_surface",
    "VK_EXT_headless_surface",
};
#define THY_VK_INSTANCE_EXT_COUNT \
    ((unsigned)SDL_arraysize(thy_vk_instance_exts))

int THYLACINE_Vulkan_LoadLibrary(_THIS, const char *path)
{
    if (path) {
        /* No dlopen on Thylacine: the driver is static-linked, so an
         * explicit loader path cannot be honored (and would be a lie to
         * accept). NULL — SDL's and every app's default — is the ask. */
        return SDL_SetError("thylacine: no Vulkan loader library exists "
                            "(the venus ICD is static-linked; pass NULL)");
    }
    if (!vk_icdGetInstanceProcAddr) {
        return SDL_SetError("thylacine: no venus ICD in this program "
                            "(link libvulkan_virtio.a with "
                            "-u vk_icdGetInstanceProcAddr)");
    }
    _this->vulkan_config.vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)vk_icdGetInstanceProcAddr;
    return 0;
}

void THYLACINE_Vulkan_UnloadLibrary(_THIS)
{
    /* Statically linked: nothing to unload. */
    (void)_this;
}

SDL_bool THYLACINE_Vulkan_GetInstanceExtensions(_THIS, SDL_Window *window,
                                                unsigned *count,
                                                const char **names)
{
    /* Hand-rolled SDL_Vulkan_GetInstanceExtensions_Helper (that helper is
     * compiled only under SDL_VIDEO_VULKAN): query fills the count; copy
     * fills what fits and errors if the caller's array was short. */
    unsigned n, i;

    (void)_this;
    (void)window;
    if (!names) {
        *count = THY_VK_INSTANCE_EXT_COUNT;
        return SDL_TRUE;
    }
    n = (*count < THY_VK_INSTANCE_EXT_COUNT) ? *count
                                             : THY_VK_INSTANCE_EXT_COUNT;
    for (i = 0; i < n; i++) {
        names[i] = thy_vk_instance_exts[i];
    }
    *count = n;
    if (n < THY_VK_INSTANCE_EXT_COUNT) {
        SDL_SetError("Insufficient space for extension names");
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

SDL_bool THYLACINE_Vulkan_CreateSurface(_THIS, SDL_Window *window,
                                        VkInstance instance,
                                        VkSurfaceKHR *surface)
{
    SDL_WindowData *wd = (SDL_WindowData *)window->driverdata;
    ThyVkGipa gipa;
    ThyVkCreateHeadlessSurfaceEXT create_headless;
    ThyVkHeadlessSurfaceCreateInfoEXT ci;
    int32_t res;

    if (!wd) {
        SDL_SetError("thylacine: no surface for window");
        return SDL_FALSE;
    }
    gipa = (ThyVkGipa)_this->vulkan_config.vkGetInstanceProcAddr;
    if (!gipa) {
        SDL_SetError("thylacine: no vkGetInstanceProcAddr loaded");
        return SDL_FALSE;
    }
    create_headless = (ThyVkCreateHeadlessSurfaceEXT)gipa(
        instance, "vkCreateHeadlessSurfaceEXT");
    if (!create_headless) {
        SDL_SetError("thylacine: vkCreateHeadlessSurfaceEXT unresolved "
                     "(was VK_EXT_headless_surface enabled on the "
                     "instance?)");
        return SDL_FALSE;
    }
    SDL_zero(ci);
    ci.sType = THY_VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    res = create_headless(instance, &ci, NULL, surface);
    if (res != 0) {
        SDL_SetError("thylacine: vkCreateHeadlessSurfaceEXT failed (%d)",
                     (int)res);
        return SDL_FALSE;
    }

    /* The consent, surface half first; the ctx half arms only once the
     * surface has accepted, so the present poke can never name a surface
     * that has not named the ctx back. A skipped/failed consent is NOT a
     * surface-creation failure: the vk surface is real and every vk call
     * works — the presents just never reach the display (fullscreen
     * DIRECT is the only presentable display path until the composed arm
     * lands), which the warn line makes diagnosable. pub == 0 = the
     * renderer never connected (a venus-off host's stub instance). */
    if (vn_renderer_thylacine_warp_ctx_pub &&
        vn_renderer_thylacine_set_surface) {
        uint32_t pub = vn_renderer_thylacine_warp_ctx_pub();
        if (pub != 0 && thyla_tap_glsrc(&wd->tap, pub) == 0) {
            vn_renderer_thylacine_set_surface(wd->tap.id);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                        "thylacine: vulkan display consent incomplete "
                        "(warp ctx pub %u) -- presents will not reach "
                        "the display",
                        (unsigned)pub);
        }
    }
    return SDL_TRUE;
}

void THYLACINE_Vulkan_GetDrawableSize(_THIS, SDL_Window *window, int *w,
                                      int *h)
{
    /* One surface, no high-DPI scaling: the drawable IS the window (the
     * GL path's argument; wired anyway so the contract is explicit). */
    (void)_this;
    if (w) {
        *w = window->w;
    }
    if (h) {
        *h = window->h;
    }
}

#endif /* SDL_VIDEO_DRIVER_THYLACINE */
