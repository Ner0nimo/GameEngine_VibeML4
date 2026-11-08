// NVRHI static builds expect exactly one TU in the program to emit the
// Vulkan-Hpp dynamic dispatch storage and to initialize it once the raw
// VkInstance/VkDevice exist. We mirror NVRHI's own macro setup
// (vulkan-backend.h: VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1, linked prototypes)
// so the DispatchLoaderDynamic layout matches NVRHI's exactly.
#ifdef IGNIS_RENDERER_VULKAN
#include <vulkan/vulkan.h> // linked prototypes from libvulkan.so
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

void ignis_init_vulkan_dispatch(void* vk_instance, void* vk_device) {
    vk::detail::defaultDispatchLoaderDynamic.init(
        static_cast<VkInstance>(vk_instance), vkGetInstanceProcAddr,
        static_cast<VkDevice>(vk_device), vkGetDeviceProcAddr);
}
#endif
