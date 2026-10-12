// Smoke test: imports winhttp.dll (our proxy, copied next to the exe) and
// loads Vulkan exactly like the game: LoadLibrary + GetProcAddress.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdio>
#include <vector>

#define CHECK(x)                                                    \
    do {                                                            \
        VkResult r_ = (x);                                          \
        if (r_ != VK_SUCCESS) {                                     \
            printf("FAILED %s -> %d (line %d)\n", #x, r_, __LINE__); \
            return 1;                                               \
        }                                                           \
    } while (0)

int main() {
    // 1. winhttp pass-through
    HINTERNET session = WinHttpOpen(L"tf3fg-smoke", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    printf("WinHttpOpen -> %p\n", session);
    if (!session) return 1;
    WinHttpCloseHandle(session);

    // 2. Vulkan through the hooked GetProcAddress
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk, "vkGetInstanceProcAddr"));
    printf("vkGetInstanceProcAddr %p (vulkan-1.dll at %p)\n", reinterpret_cast<void*>(gipa), vk);

    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "smoke";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    CHECK(createInstance(&ici, nullptr, &instance));

#define IFN(n) auto n = reinterpret_cast<PFN_vk##n>(gipa(instance, "vk" #n))
    IFN(EnumeratePhysicalDevices);
    IFN(GetPhysicalDeviceProperties);
    IFN(GetPhysicalDeviceQueueFamilyProperties);
    IFN(CreateDevice);
    IFN(GetDeviceProcAddr);

    uint32_t n = 0;
    EnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    EnumeratePhysicalDevices(instance, &n, pds.data());
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (auto p : pds) {
        VkPhysicalDeviceProperties props;
        GetPhysicalDeviceProperties(p, &props);
        printf("GPU: %s\n", props.deviceName);
        if (props.vendorID == 0x10DE) pd = p;
    }
    if (!pd) pd = pds[0];

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device;
    CHECK(CreateDevice(pd, &dci, nullptr, &device));

#define DFN(n) auto n = reinterpret_cast<PFN_vk##n>(GetDeviceProcAddr(device, "vk" #n))
    DFN(GetDeviceQueue);
    DFN(CreateCommandPool);
    DFN(AllocateCommandBuffers);
    DFN(BeginCommandBuffer);
    DFN(EndCommandBuffer);
    DFN(QueueSubmit);
    DFN(QueueWaitIdle);
    DFN(CreateImage);
    DFN(DestroyImage);
    DFN(CmdDraw);
    DFN(DestroyCommandPool);
    DFN(DestroyDevice);
    auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));

    VkImageCreateInfo imci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imci.imageType = VK_IMAGE_TYPE_2D;
    imci.format = VK_FORMAT_D32_SFLOAT;
    imci.extent = {1920, 1080, 1};
    imci.mipLevels = 1;
    imci.arrayLayers = 1;
    imci.samples = VK_SAMPLE_COUNT_1_BIT;
    imci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image;
    CHECK(CreateImage(device, &imci, nullptr, &image));

    VkQueue queue;
    GetDeviceQueue(device, 0, 0, &queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool;
    CHECK(CreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    CHECK(AllocateCommandBuffers(device, &cai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(BeginCommandBuffer(cb, &bi));
    CHECK(EndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    CHECK(QueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(QueueWaitIdle(queue));

    DestroyImage(device, image, nullptr);
    DestroyCommandPool(device, pool, nullptr);
    DestroyDevice(device, nullptr);
    destroyInstance(instance, nullptr);
    printf("OK\n");
    return 0;
}
