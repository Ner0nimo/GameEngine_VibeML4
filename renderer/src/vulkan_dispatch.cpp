// NVRHI static builds expect exactly one TU in the program to emit the
// Vulkan-Hpp dynamic dispatch storage and to initialize it once the raw
// VkInstance/VkDevice exist. We use the directly linked libvulkan's entry
// points (declared manually because VK_NO_PROTOTYPES suppresses the header
// prototypes) - same approach as NVRHI's own tests.
#ifdef IGNIS_RENDERER_VULKAN
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <sys/wait.h>
#include <unistd.h>

extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* pName);
extern "C" PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char* pName);

void ignis_init_vulkan_dispatch(void* vk_instance, void* vk_device) {
    vk::detail::defaultDispatchLoaderDynamic.init(
        static_cast<VkInstance>(vk_instance), &vkGetInstanceProcAddr,
        static_cast<VkDevice>(vk_device), &vkGetDeviceProcAddr);
}

int ignis_dispatch_probe(void* vk_instance, void* vk_physical_device) {
    auto& d = vk::detail::defaultDispatchLoaderDynamic;
    if (!d.vkGetPhysicalDeviceProperties2) return 1;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    d.vkGetPhysicalDeviceProperties2(static_cast<VkPhysicalDevice>(vk_physical_device), &props);
    return props.properties.deviceName[0] ? 0 : 2;
}
#endif
