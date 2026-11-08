#pragma once

// Initializes the Vulkan-Hpp dynamic dispatch loader (used by NVRHI's static
// backend) for an already-created VkInstance/VkDevice. Defined in
// vulkan_dispatch.cpp (graphics builds only).
void ignis_init_vulkan_dispatch(void* vk_instance, void* vk_device);
