/* SDL_thylacine — the Vulkan window glue (Warp-WSI W-3e). Contracts and
 * the linking model in SDL_thylacinevulkan.c's header comment. */
#ifndef SDL_thylacinevulkan_h_
#define SDL_thylacinevulkan_h_

#include "../../SDL_internal.h"
#include "../SDL_sysvideo.h"

extern int THYLACINE_Vulkan_LoadLibrary(_THIS, const char *path);
extern void THYLACINE_Vulkan_UnloadLibrary(_THIS);
extern SDL_bool THYLACINE_Vulkan_GetInstanceExtensions(_THIS,
                                                       SDL_Window *window,
                                                       unsigned *count,
                                                       const char **names);
extern SDL_bool THYLACINE_Vulkan_CreateSurface(_THIS, SDL_Window *window,
                                               VkInstance instance,
                                               VkSurfaceKHR *surface);
extern void THYLACINE_Vulkan_GetDrawableSize(_THIS, SDL_Window *window,
                                             int *w, int *h);

#endif /* SDL_thylacinevulkan_h_ */
