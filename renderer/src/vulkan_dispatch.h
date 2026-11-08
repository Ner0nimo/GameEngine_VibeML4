#pragma once

// Initializes the Vulkan-Hpp dynamic dispatch loader (used by NVRHI's static
// backend) for an already-created VkInstance/VkDevice. Defined in
// vulkan_dispatch.cpp (graphics builds only).
void ignis_init_vulkan_dispatch(void* vk_instance, void* vk_device);

// Diagnostic: call vkGetPhysicalDeviceProperties2 through the dispatcher.
// Returns 0 on success, non-zero on failure (does not crash the caller if the
// dispatcher is broken - the crash happens in the callee, so callers should
// use this only in a forked probe).
int ignis_dispatch_probe(void* vk_instance, void* vk_physical_device);
