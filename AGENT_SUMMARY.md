# DLSS Frame Generation for Transport Fever 3: implementation summary

A working recipe for adding NVIDIA DLSS Frame Generation (DLSS-G) to Transport Fever 3 (TF3). TF3 has no native DLSS, TAA or motion vectors. Give this to a coding agent as context.

Result on an RTX 4050 Laptop GPU at 1080p Very High: about 40 FPS rendered without the mod, about 30 rendered + 30 generated (60 shown) with it. UI stays clean.

## Requirements
- RTX 40/50 GPU (DLSS-G does not run on RTX 30, AMD or Intel), Windows 10 20H1+ with hardware-accelerated GPU scheduling.
- Build tools: Visual Studio Build Tools (C++, CMake, MASM), Vulkan SDK (headers + glslangValidator), NVIDIA Streamline SDK (github.com/NVIDIA-RTX/Streamline releases; v2.14.1 used). Users download Streamline themselves (see Licensing).

## TF3 renderer facts (found by recording frames)
- **Vulkan only.** Instance API 1.1, one graphics queue (family 0). FSR 1 is the only upscaler.
- **How Vulkan is loaded.** The exe has **no static import of vulkan-1.dll**. It uses `LoadLibrary` + `GetProcAddress` and then `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`. The window surface is created by **SDL2** (`SDL_Vulkan_CreateSurface`), which resolves Vulkan through its own `GetProcAddress` import.
- **Camera.** The block is `View` at descriptor binding 10, std140, 464 bytes (`base/content/rendering/programs/shaders/pbr/view.h.glsl`). Float offsets: `projView[0..15]`, `projViewInverse[16..31]`, `proj[32..47]`, `projInverse[48..63]`, `view[64..79]`, `viewInverse[80..95]`, `camPos[96..98]`, `near[104]`, `far[105]`, `width(int)[107]`, `height(int)[108]`, `fov[114]`. A new host-visible, persistently mapped buffer is used each frame, so it can be read at submit time.
  - **Main camera:** perspective (`proj[11] == -1`, `proj[15] == 0`) and width/height equal to the swapchain. Shadow cascades are orthographic; window previews (vehicle/station windows) are smaller.
- **Conventions.** GL style: column vectors, clip z in [-1,1], depth = (z_ndc+1)/2, framebuffer row 0 = NDC y +1.
- **Depth.** MSAA 2x scene (R11G11B10F colour, D24S8 depth). Depth is resolved to a single-sample full-frame D24S8 by a `vkCreateRenderPass2` pass with `VkSubpassDescriptionDepthStencilResolve`, ending in `SHADER_READ_ONLY_OPTIMAL`. Use that resolve attachment.
- **UI-free (hudless) colour.** The last full-frame colour `vkCmdResolveImage` target (R11G11B10F, already tonemapped) before the first render pass that targets a swapchain image. The game copies it to the swapchain and then draws all UI, **including in-world labels**, on top.
- **VSync.** FIFO, 2 swapchain images.
- **In-world icons.** The first render pass onto the swapchain after that resolve draws the scene copy plus all in-world labels and icons (city names, station signs, vehicle icons). Windows and menus come in later passes (offscreen layer + composite). Copying the swapchain image at the end of that first pass and tagging the copy as hudless makes DLSS-G interpolate the icons with the scene, which removes icon jitter while panning (v0.2.0).

## Architecture
1. **Proxy `winhttp.dll` in the game folder.** The exe imports WINHTTP; forward all of its exports to System32 with generated MASM jump stubs. Do **not** proxy `vulkan-1.dll`: Streamline's interposer calls `LoadLibraryA("vulkan-1.dll")` and would load the proxy.
2. **In DllMain, IAT-patch `GetProcAddress`** in **both** `TransportFever3.exe` and `SDL2.dll`. When the module is vulkan-1.dll and the name starts with `vk`, return your hooks; hook `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` so everything resolved later goes through you.
3. **Record per command buffer** (begin/end render pass with framebuffer views, pipeline barriers with new layouts, resolves/copies with dst layouts, bind descriptor sets). Track descriptor writes to binding 10 (`vkUpdateDescriptorSets` and update templates) plus buffer→memory→mapped pointer.
4. **At `vkQueueSubmit`,** replay the recorded commands in submission order to track image layouts and find this frame's depth, hudless and camera.
5. **At `vkQueuePresentKHR`,** record and submit your own command buffer on the same queue. It waits on the present's semaphores and signals a semaphore **per swapchain image** (reusing one present semaphore across images trips "semaphore still in use by swapchain"). It does three things:
   - A compute pass writes motion vectors to an RGBA16F image (RG16F storage needs an optional feature).
   - Barriers return the game's images to their original layouts.
   - `slSetTagForFrame` while the command buffer is still recording.
   - Then present via Streamline's proxy with your semaphore.
6. **Command buffers allocated through device-level function pointers** need the loader dispatch pointer: `*(void**)cb = *(void**)device`.

## Motion vectors (camera only)
```glsl
reproj = prevProjView * curProjViewInverse;          // computed on the CPU in double
ndc = vec4(2*uv.x-1, 1-2*uv.y, 2*depth-1, 1);
p   = reproj * ndc;  prevUv = vec2(0.5*p.x/p.w+0.5, 0.5-0.5*p.y/p.w);
mv  = prevUv - uv;                                   // UV units, so mvecScale = {1,1}
```
These were verified with a CPU ray-traced ground-truth scene and in game: with them, the frame-to-frame difference is about 2%, versus about 10% without. Objects that move on their own (trains) are not covered; their motion vectors read as static and only DLSS-G's own optical flow catches them. In practice no smearing was visible on moving trains (tested in game).

## Streamline (manual hooking)
- **Load and init.** `LoadLibrary` `sl.interposer.dll` from a subfolder and call `SetDllDirectoryW(subfolder)` (plugins load NGX/Reflex DLLs by name). Call `slInit` **inside your `vkCreateInstance` hook, before creating the instance**:
  - flags: `eUseManualHooking | eDisableCLStateTracking | eUseFrameBasedResourceTagging` (tagging is rejected without the last one)
  - features: `kFeatureDLSS_G`, `kFeatureReflex`, `kFeaturePCL`
  - `renderAPI = eVulkan`, `pathsToPlugins` = the subfolder
  - `engine = eCustom` + `projectId`; `applicationId` 0 uses a temporary ID that works
- **Route through Streamline's proxies** (from the interposer's exported `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`): `vkCreateInstance`, **`vkEnumeratePhysicalDevices`** (its device-creation proxy needs the physical-device→instance map built here), `vkCreateDevice`, **`vkCreateWin32SurfaceKHR`** (it records the HWND; without it you get "Could not find a window corresponding to this application", the swapchain fails and the game crashes), `vkDestroySurfaceKHR`, `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`, `vkQueuePresentKHR`, `vkDeviceWaitIdle`. The proxies add the queues, extensions and features they need to the game's device.
- **VSync.** DLSS-G on Vulkan requires VSync off (`eVSyncOffRequired`); switch FIFO → IMMEDIATE in your create-swapchain hook.
- **Constants** (`slSetConstants`, once per frame).
  - **Matrices.** GL column-major arrays copied as-is equal Streamline's row-major, row-vector layout. Remap clip z to the depth-buffer range with `T = [1,0,0,0, 0,1,0,0, 0,0,.5,0, 0,0,.5,1]` (column-major):
    - `cameraViewToClip = T·proj`
    - `clipToCameraView = projInv·T⁻¹`
    - `clipToPrevClip = T·prevPV·curPVinv·T⁻¹`
    - `prevClipToClip = T·curPV·prevPVinv·T⁻¹`
  - **Flags.** `depthInverted false`, `cameraMotionIncluded true`, `mvecScale {1,1}`, `jitter 0`.
  - **Camera vectors** come from `viewInverse` columns (right, up, -forward).
  - **`reset = true`** after a camera cut.
- **Tags** (`eValidUntilPresent`): depth, motion vectors, hudless, with Vulkan image, view, layout at present time, size, format and usage. In menus and loading screens, set null tags and `DLSSGMode::eOff`; use `eRetainResourcesWhenOff`.
- **Reflex/PCL markers, all with one frame token per frame:**
  1. After present: new token → `slReflexSleep` → SimulationStart
  2. First queue submit: SimulationEnd + RenderSubmitStart
  3. Before present: RenderSubmitEnd + PresentStart
  4. After present: PresentEnd
- **Shutdown.** Call `slShutdown` before the game's `vkDestroyDevice`.
- **Window focus.** DLSS-G pauses when the window is not focused ("window not focused" in `sl.log`).
- **Checking it works.** `slDLSSGGetState().numFramesActuallyPresented == 2` means frames are being generated.
- **Expected noise.** These validation errors come from Streamline itself and can be ignored: pacer "fake-swapchain-buffer" layout, low_latency2 without present_id, privateData. Warnings about unsupported CmdBindPipeline/BeginCommandBuffer hooks are also expected in manual mode.

## Testing tips
- Write a test program that loads Vulkan the way the game does (LoadLibrary + GetProcAddress, surface through the game's own SDL2.dll) and run it under `VK_LAYER_KHRONOS_validation`.
- A CPU-generated ground-truth scene (known depth and camera) checks the motion-vector convention without the game.
- Make the test DPI-aware and handle `VK_ERROR_OUT_OF_DATE_KHR` by recreating the swapchain.

## Limitations
- Only works at 100% Resolution Scale with MSAA on; the frame structure differs with FSR or MSAA off.
- Motion vectors cover camera movement only (no visible artifacts on trains in testing, but fast-moving objects could still show some).
- Shown frame rate is capped by display refresh while generating.
- Game updates may change the render pipeline.

## Licensing
Streamline source is MIT. The NVIDIA runtime DLLs (`sl.*.dll`, `nvngx_dlssg.dll`, `NvLowLatencyVk.dll`) are under NVIDIA's RTX SDK license, which only allows redistribution as part of an application, with attribution. Have users download the Streamline release themselves rather than bundling the DLLs.
