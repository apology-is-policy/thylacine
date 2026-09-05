/* thy_vkloader.c -- the loader-less Vulkan resolution filter (Warp W-4).
 *
 * Thylacine links the venus ICD statically; there is no Vulkan loader and
 * no dlopen. vk_icdGetInstanceProcAddr returns mesa's vk_tramp_* dispatch
 * trampolines for device- and physical-device-parented names; the
 * object-creating vkCreateDevice trampoline tail-branches through a
 * dispatch slot only a real loader populates, so via gipa it branches to
 * NULL (measured at W-1; mesa thylacine_prove.c documents it). Physical-
 * device QUERY trampolines have populated slots and resolve fine.
 *
 * The filter returns the driver's own entrypoints directly for the two
 * names that must never be trampolines -- vkCreateDevice (the measured
 * breakage) and vkGetDeviceProcAddr (every device-level global is loaded
 * through it, so it must itself be real) -- and defers every other name
 * to the ICD. volk is initialized with this filter, so volkLoadDevice()
 * resolves ALL device-level globals through the real vn_GetDeviceProcAddr:
 * dispatch entries, never loader-layout trampolines.
 */

#include "volk.h"
#include <string.h>

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

VKAPI_ATTR VkResult VKAPI_CALL
vn_CreateDevice(VkPhysicalDevice physical_device,
                const VkDeviceCreateInfo *create_info,
                const VkAllocationCallbacks *allocator, VkDevice *device);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vn_GetDeviceProcAddr(VkDevice device, const char *name);

PFN_vkVoidFunction VKAPI_CALL
THY_VkGetProcAddr(VkInstance instance, const char *name)
{
	if (strcmp(name, "vkCreateDevice") == 0)
		return (PFN_vkVoidFunction)vn_CreateDevice;
	if (strcmp(name, "vkGetDeviceProcAddr") == 0)
		return (PFN_vkVoidFunction)vn_GetDeviceProcAddr;
	return vk_icdGetInstanceProcAddr(instance, name);
}

void THY_InitVulkanLoader(void)
{
	volkInitializeCustom(THY_VkGetProcAddr);
}
