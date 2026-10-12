#pragma once

#include "common.h"

namespace fg {

// NVIDIA Streamline (DLSS Frame Generation + Reflex), loaded at runtime from
// <game>\framegen\ in "manual hooking" mode: the game's Vulkan calls that
// Streamline needs to see are routed to its proxies by vk_hooks.cpp.

// Loads sl.interposer.dll and calls slInit. Must run before vkCreateInstance.
bool SlLoadAndInit();
bool SlActive();

// Streamline's proxy for a Vulkan function (nullptr if Streamline is inactive).
PFN_vkVoidFunction SlInstanceProc(const char* name);
PFN_vkVoidFunction SlDeviceProc(VkDevice device, const char* name);

void SlOnDeviceCreated(VkPhysicalDevice physicalDevice, VkDevice device);
void SlShutdown();
// Stops all per-frame Streamline calls (used when it cannot attach to the swapchain).
void SlDisableFrameGeneration();

// True if Frame Generation needs the swapchain without VSync.
bool SlWantsVsyncOff();

struct SlFrameInputs {
    VkCommandBuffer cb;  // our command buffer, still recording
    VkExtent2D extent;
    VkImage depth;
    VkImageView depthView;
    VkFormat depthFormat;
    VkImageLayout depthLayout;
    VkImageUsageFlags depthUsage;
    VkImage mvec;
    VkImageView mvecView;
    VkDeviceMemory mvecMemory;
    VkFormat mvecFormat;
    VkImageLayout mvecLayout;
    VkImageUsageFlags mvecUsage;
    VkImage hudless;
    VkImageView hudlessView;
    VkFormat hudlessFormat;
    VkImageLayout hudlessLayout;
    VkImageUsageFlags hudlessUsage;
    const float* camera;      // View block of this frame (framegen.h kViewFloats)
    const float* prevCamera;  // previous frame, or nullptr after a cut
};

// Per-frame calls, all on the presenting thread.
void SlOnSubmit();                              // first submit of a frame
void SlProvideFrame(const SlFrameInputs* in);   // nullptr: no game frame (menus, loading)
void SlPresentStart();
void SlPresentEnd();

}  // namespace fg
