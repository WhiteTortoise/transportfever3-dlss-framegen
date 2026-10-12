#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace fg {

// Logging (main.cpp). Every line is flushed so the log survives a crash.
void Log(const char* fmt, ...);
void LogRaw(const std::string& text);

// Directory of our DLL (= the game folder), with trailing backslash.
const std::wstring& ModuleDir();

// User settings from tf3_framegen.ini in the game folder.
struct Config {
    bool startEnabled = true;  // frame generation on when a map loads
    int hotkey = VK_OEM_5;     // Ctrl+Alt+<hotkey> toggles frame generation (default \, F tilts the camera)
    bool selfCheck = false;    // motion vector accuracy check (diagnostics)
};
const Config& GetConfig();
// Re-reads tf3_framegen.ini if it changed (checked at most every 2 s). Presenting thread only.
void ReloadConfigIfChanged();

// Real export of the Vulkan loader (vulkan-1.dll), bypassing our hooks.
PFN_vkVoidFunction LoaderProc(const char* name);

// vk_hooks.cpp: given a Vulkan function name and the real pointer, returns
// either our hook (remembering the real pointer) or the real pointer.
PFN_vkVoidFunction InterceptVulkan(const char* name, PFN_vkVoidFunction real);

}  // namespace fg

#define FG_LOG(...) ::fg::Log(__VA_ARGS__)
