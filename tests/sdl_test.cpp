// Creates the window and Vulkan surface through the game's SDL2.dll (as
// Transport Fever 3 does), then a swapchain and some presents through our
// winhttp.dll and Streamline. Exit code 0 = swapchain created and presented.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdio>
#include <vector>

extern "C" {
struct SDL_Window;
int SDL_Init(uint32_t flags);
void SDL_Quit();
const char* SDL_GetError();
SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, uint32_t flags);
void SDL_DestroyWindow(SDL_Window* window);
void SDL_PumpEvents();
void SDL_RaiseWindow(SDL_Window* window);
int SDL_Vulkan_GetInstanceExtensions(SDL_Window* window, unsigned int* count, const char** names);
int SDL_Vulkan_CreateSurface(SDL_Window* window, VkInstance instance, VkSurfaceKHR* surface);
}

#define CHECK(x)                                                    \
    do {                                                            \
        VkResult r_ = (x);                                          \
        if (r_ != VK_SUCCESS) {                                     \
            printf("FAILED %s -> %d (line %d)\n", #x, r_, __LINE__); \
            return 1;                                               \
        }                                                           \
    } while (0)

int main() {
    WinHttpCloseHandle(WinHttpOpen(L"tf3fg-sdl-test", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0));
    const uint32_t W = 800, H = 450;
    if (SDL_Init(0x20 /* SDL_INIT_VIDEO */) != 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("tf3_framegen SDL test", 0x2FFF0000, 0x2FFF0000, W, H,
                                          0x4 /* SHOWN */ | 0x10000000 /* VULKAN */);
    if (!window) {
        printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RaiseWindow(window);

    unsigned int extCount = 0;
    SDL_Vulkan_GetInstanceExtensions(window, &extCount, nullptr);
    std::vector<const char*> exts(extCount);
    SDL_Vulkan_GetInstanceExtensions(window, &extCount, exts.data());

    // The game loads Vulkan itself, like this.
    HMODULE vkmod = LoadLibraryA("vulkan-1.dll");
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vkmod, "vkGetInstanceProcAddr"));
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Transport Fever 3";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = extCount;
    ici.ppEnabledExtensionNames = exts.data();
    VkInstance inst;
    CHECK(createInstance(&ici, nullptr, &inst));

    VkSurfaceKHR surface;
    if (!SDL_Vulkan_CreateSurface(window, inst, &surface)) {
        printf("SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return 1;
    }

#define IFN(n) auto n = reinterpret_cast<PFN_vk##n>(gipa(inst, "vk" #n))
    IFN(EnumeratePhysicalDevices);
    IFN(GetPhysicalDeviceProperties);
    IFN(CreateDevice);
    IFN(GetDeviceProcAddr);
    uint32_t n = 0;
    EnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    EnumeratePhysicalDevices(inst, &n, pds.data());
    VkPhysicalDevice pd = pds[0];
    for (auto p : pds) {
        VkPhysicalDeviceProperties props;
        GetPhysicalDeviceProperties(p, &props);
        if (props.vendorID == 0x10DE) pd = p;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dext[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    VkDevice dev;
    CHECK(CreateDevice(pd, &dci, nullptr, &dev));

#define DFN(n) auto n = reinterpret_cast<PFN_vk##n>(GetDeviceProcAddr(dev, "vk" #n))
    DFN(GetDeviceQueue); DFN(CreateSwapchainKHR); DFN(GetSwapchainImagesKHR); DFN(AcquireNextImageKHR);
    DFN(QueuePresentKHR); DFN(CreateCommandPool); DFN(AllocateCommandBuffers); DFN(BeginCommandBuffer);
    DFN(EndCommandBuffer); DFN(CmdPipelineBarrier); DFN(QueueSubmit); DFN(CreateSemaphore); DFN(CreateFence);
    DFN(WaitForFences); DFN(ResetFences); DFN(DeviceWaitIdle); DFN(DestroySwapchainKHR); DFN(DestroyDevice);

    VkQueue queue;
    GetDeviceQueue(dev, 0, 0, &queue);
    VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = surface;
    swci.minImageCount = 2;
    swci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swci.imageExtent = {W, H};
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain;
    CHECK(CreateSwapchainKHR(dev, &swci, nullptr, &swapchain));
    uint32_t nimg = 0;
    GetSwapchainImagesKHR(dev, swapchain, &nimg, nullptr);
    std::vector<VkImage> images(nimg);
    GetSwapchainImagesKHR(dev, swapchain, &nimg, images.data());

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(CreateCommandPool(dev, &cpi, nullptr, &pool));
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.commandBufferCount = 1;
    CHECK(AllocateCommandBuffers(dev, &cai, &cb));
    VkSemaphoreCreateInfo smi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence;
    CHECK(CreateFence(dev, &fi, nullptr, &fence));
    std::vector<VkSemaphore> acquired(nimg + 1), rendered(nimg);
    for (auto& s : acquired) CreateSemaphore(dev, &smi, nullptr, &s);
    for (auto& s : rendered) CreateSemaphore(dev, &smi, nullptr, &s);

    int presented = 0;
    for (int frame = 0; frame < 120; ++frame) {
        SDL_PumpEvents();
        WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        uint32_t idx;
        VkSemaphore acq = acquired[frame % acquired.size()];
        VkResult ar = AcquireNextImageKHR(dev, swapchain, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            printf("acquire -> %d at frame %d\n", ar, frame);
            break;
        }
        ResetFences(dev, 1, &fence);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        BeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[idx];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &b);
        EndCommandBuffer(cb);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acq;
        si.pWaitDstStageMask = &ws;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered[idx];
        CHECK(QueueSubmit(queue, 1, &si, fence));
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &idx;
        VkResult pr = QueuePresentKHR(queue, &pi);
        if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) presented++;
    }
    DeviceWaitIdle(dev);
    DestroySwapchainKHR(dev, swapchain, nullptr);
    DestroyDevice(dev, nullptr);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("presented %d frames\n", presented);
    return presented > 100 ? 0 : 2;
}
