#pragma once

#include "common.h"

namespace fg {

// Floats in the game's camera uniform block (pbr/view.h.glsl, std140, 464 bytes).
constexpr size_t kViewFloats = 116;

// Everything gathered from the game's frame that our per-frame work needs.
struct PresentInputs {
    float camera[kViewFloats];  // main camera: [0..15] projView, [16..31] projViewInverse
    VkExtent2D extent;          // render extent (= swapchain extent for now)
    VkImage depth;              // single-sample depth after MSAA resolve
    VkFormat depthFormat;
    VkImageLayout depthLayout;
    VkImageUsageFlags depthUsage;
    VkImage hudless;            // final scene colour before any GUI is drawn
    VkFormat hudlessFormat;
    VkImageLayout hudlessLayout;
    VkImageUsageFlags hudlessUsage;
    VkImage swapImage;          // swapchain image being presented
    VkFormat swapFormat;
};

// vk_hooks.cpp: hands over (and resets) what was seen since the last present.
bool CollectPresentInputs(const VkPresentInfoKHR* info, PresentInputs& out);

// framegen.cpp
void FgOnDeviceCreated(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, const VkPhysicalDeviceMemoryProperties& memProps);
void FgOnDeviceDestroyed();
void FgOnImageDestroyed(VkImage image);
void FgSetQueueFamily(VkQueue queue, uint32_t family);
// Called while the game records its command buffer, right after the render
// pass that draws the scene plus the in-world labels/icons onto the swapchain
// image: copies that image so DLSS can treat the icons as part of the scene.
void FgRecordWorldIconsCopy(VkCommandBuffer cb, VkImage swapImage, VkImageLayout swapLayout, VkFormat format,
                            VkExtent2D extent);
// Records and submits our per-frame GPU work and hands the frame to
// Streamline (`*providedToSl` = true). Returns the present info to use
// (`storage` when the wait semaphores had to be replaced, otherwise `info`).
const VkPresentInfoKHR* FgOnPresent(VkQueue queue, const VkPresentInfoKHR* info, const PresentInputs* inputs,
                                    VkPresentInfoKHR& storage, bool* providedToSl);

}  // namespace fg
