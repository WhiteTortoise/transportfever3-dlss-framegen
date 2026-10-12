// Renders frames shaped like Transport Fever 3's (MSAA scene with depth
// resolve, colour resolve, camera block at set binding 10, GUI pass to the
// swapchain) through our winhttp.dll, under the Khronos validation layer.
// Exit code = number of validation errors (0 = clean).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK(x)                                                    \
    do {                                                            \
        VkResult r_ = (x);                                          \
        if (r_ != VK_SUCCESS) {                                     \
            printf("FAILED %s -> %d (line %d)\n", #x, r_, __LINE__); \
            return 100;                                             \
        }                                                           \
    } while (0)

static int g_validationErrors = 0;

static VKAPI_ATTR VkBool32 VKAPI_CALL OnDebug(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                              VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* d,
                                              void*) {
    // The test itself does not clean up its own objects; ignore that report.
    if (d->pMessageIdName && strstr(d->pMessageIdName, "vkDestroyDevice-device-05137")) return VK_FALSE;
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (g_validationErrors++ < 20) printf("VALIDATION ERROR: %s\n\n", d->pMessage);
    } else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        printf("validation warning: %.300s\n\n", d->pMessage);
    }
    return VK_FALSE;
}

// Column-major 4x4 helpers (GL conventions, like the game).
static void Mul(const double* a, const double* b, double* o) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            o[c * 4 + r] = s;
        }
}

static bool Invert(const double* m, double* inv) {
    double t[16];
    t[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    t[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    t[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    t[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    t[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    t[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    t[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    t[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    t[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    t[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    t[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    t[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    t[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    t[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    t[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    t[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
    if (det == 0) return false;
    for (int i = 0; i < 16; ++i) inv[i] = t[i] / det;
    return true;
}

static void Normalize(double* v) {
    double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
}

// Camera flying over a ground plane, GL conventions like the game.
static void FillViewBlock(float* f, int frame, uint32_t w, uint32_t h, double* pv, double* pvi) {
    const double n = 4, fa = 8163, fov = 1.047, a = double(w) / h, ff = 1.0 / tan(fov / 2);
    double proj[16] = {ff / a, 0, 0, 0, 0, ff, 0, 0, 0, 0, (fa + n) / (n - fa), -1, 0, 0, 2 * fa * n / (n - fa), 0};
    double eye[3] = {frame * 0.8, 60.0, 0.0};
    double fwd[3] = {0.3 + 0.004 * frame, -0.8, -1.0};
    Normalize(fwd);
    double up[3] = {0, 1, 0};
    double s[3] = {fwd[1] * up[2] - fwd[2] * up[1], fwd[2] * up[0] - fwd[0] * up[2], fwd[0] * up[1] - fwd[1] * up[0]};
    Normalize(s);
    double u[3] = {s[1] * fwd[2] - s[2] * fwd[1], s[2] * fwd[0] - s[0] * fwd[2], s[0] * fwd[1] - s[1] * fwd[0]};
    double view[16] = {s[0], u[0], -fwd[0], 0, s[1], u[1], -fwd[1], 0, s[2], u[2], -fwd[2], 0,
                       -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]),
                       -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                       fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2], 1};
    Mul(proj, view, pv);
    Invert(pv, pvi);
    memset(f, 0, 464);
    for (int i = 0; i < 16; ++i) {
        f[i] = float(pv[i]);
        f[16 + i] = float(pvi[i]);
        f[32 + i] = float(proj[i]);
    }
    f[104] = float(n);
    f[105] = float(fa);
    reinterpret_cast<int32_t*>(f)[107] = int32_t(w);
    reinterpret_cast<int32_t*>(f)[108] = int32_t(h);
    f[114] = float(fov);
}

static void Transform(const double* m, const double* v, double* o) {
    for (int r = 0; r < 4; ++r) o[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
}

// Ray-traces the ground plane: checkerboard colour and D24 depth with the
// game's conventions (row 0 = NDC y +1, depth = (z_ndc + 1) / 2).
static void RenderScene(const double* pv, const double* pvi, uint32_t w, uint32_t h, uint32_t* color, uint32_t* depth) {
    for (uint32_t py = 0; py < h; ++py) {
        for (uint32_t px = 0; px < w; ++px) {
            double x = 2.0 * (px + 0.5) / w - 1.0, y = 1.0 - 2.0 * (py + 0.5) / h;
            double nearC[4] = {x, y, -1, 1}, farC[4] = {x, y, 1, 1}, nw[4], fw[4];
            Transform(pvi, nearC, nw);
            Transform(pvi, farC, fw);
            for (int i = 0; i < 3; ++i) {
                nw[i] /= nw[3];
                fw[i] /= fw[3];
            }
            double dir[3] = {fw[0] - nw[0], fw[1] - nw[1], fw[2] - nw[2]};
            size_t i = size_t(py) * w + px;
            double t = dir[1] < 0 ? -nw[1] / dir[1] : -1;
            if (t < 0 || t > 1) {
                color[i] = 0xFFE69664;  // sky (ABGR)
                depth[i] = 0xFFFFFF;
                continue;
            }
            double p[4] = {nw[0] + t * dir[0], 0.0, nw[2] + t * dir[2], 1.0};
            int checker = (int(floor(p[0] / 8)) + int(floor(p[2] / 8))) & 1;
            uint32_t r = checker ? 230 : 40;
            uint32_t g = uint32_t(120 + 100 * sin(p[2] * 0.05));
            uint32_t b = uint32_t(120 + 100 * sin(p[0] * 0.07));
            color[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
            double clip[4];
            Transform(pv, p, clip);
            double d = (clip[2] / clip[3] + 1.0) * 0.5;
            depth[i] = uint32_t(d * 16777215.0) & 0xFFFFFF;
        }
    }
}

static double Luma(uint32_t c) {
    return (0.299 * (c & 0xFF) + 0.587 * ((c >> 8) & 0xFF) + 0.114 * ((c >> 16) & 0xFF)) / 255.0;
}

static double SampleLuma(const uint32_t* img, uint32_t w, uint32_t h, double u, double v) {
    double x = u * w - 0.5, y = v * h - 0.5;
    int x0 = int(floor(x)), y0 = int(floor(y));
    double fx = x - x0, fy = y - y0;
    auto at = [&](int xi, int yi) {
        xi = xi < 0 ? 0 : (xi >= int(w) ? int(w) - 1 : xi);
        yi = yi < 0 ? 0 : (yi >= int(h) ? int(h) - 1 : yi);
        return Luma(img[size_t(yi) * w + xi]);
    };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

// Same metric as the mod's errstat.comp, but warping with exact motion: the
// best score any motion vectors could reach on this scene.
static void IdealResidual(const double* pvi, const double* prevPv, const uint32_t* cur, const uint32_t* curDepth,
                          const uint32_t* prev, uint32_t w, uint32_t h, double& sumStill, double& sumIdeal,
                          uint64_t& count) {
    for (uint32_t py = 0; py < h; py += 4) {
        for (uint32_t px = 0; px < w; px += 4) {
            size_t i = size_t(py) * w + px;
            double d = curDepth[i] / 16777215.0;
            if (d >= 0.99999) continue;
            double u = (px + 0.5) / w, v = (py + 0.5) / h;
            double clip[4] = {2 * u - 1, 1 - 2 * v, 2 * d - 1, 1}, world[4], pc[4];
            Transform(pvi, clip, world);
            for (int k = 0; k < 4; ++k) world[k] /= world[3];
            Transform(prevPv, world, pc);
            double pu = 0.5 * pc[0] / pc[3] + 0.5, pvv = 0.5 - 0.5 * pc[1] / pc[3];
            if (pu < 0 || pu > 1 || pvv < 0 || pvv > 1) continue;
            double l = Luma(cur[i]);
            sumStill += fabs(l - SampleLuma(prev, w, h, u, v));
            sumIdeal += fabs(l - SampleLuma(prev, w, h, pu, pvv));
            count++;
        }
    }
}

int main() {
    // winhttp import keeps our proxy DLL loaded exactly as in the game.
    HINTERNET session = WinHttpOpen(L"tf3fg-frame-test", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    WinHttpCloseHandle(session);

    const uint32_t W = 800, H = 450;
    // Physical pixels, so the window really is W x H with display scaling on.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"tf3fg_test";
    RegisterClassW(&wc);
    RECT rc{0, 0, LONG(W), LONG(H)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"tf3fg_test", L"tf3_framegen test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50,
                              rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    // Frame generation only runs while its window has focus.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    printf("window has focus: %s\n", GetForegroundWindow() == hwnd ? "yes" : "no");

    HMODULE vkmod = LoadLibraryA("vulkan-1.dll");
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vkmod, "vkGetInstanceProcAddr"));
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));

    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char* iext[] = {"VK_KHR_surface", "VK_KHR_win32_surface", "VK_EXT_debug_utils"};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "frame_test";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
    ici.enabledExtensionCount = 3;
    ici.ppEnabledExtensionNames = iext;
    VkInstance inst;
    CHECK(createInstance(&ici, nullptr, &inst));

#define IFN(n) auto n = reinterpret_cast<PFN_vk##n>(gipa(inst, "vk" #n))
    IFN(CreateDebugUtilsMessengerEXT);
    IFN(EnumeratePhysicalDevices);
    IFN(GetPhysicalDeviceProperties);
    IFN(GetPhysicalDeviceMemoryProperties);
    IFN(CreateDevice);
    IFN(GetDeviceProcAddr);
    IFN(CreateWin32SurfaceKHR);
    IFN(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    IFN(GetPhysicalDeviceSurfaceSupportKHR);

    VkDebugUtilsMessengerCreateInfoEXT dmi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dmi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    dmi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    dmi.pfnUserCallback = OnDebug;
    VkDebugUtilsMessengerEXT messenger;
    CHECK(CreateDebugUtilsMessengerEXT(inst, &dmi, nullptr, &messenger));

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = wc.hInstance;
    sci.hwnd = hwnd;
    VkSurfaceKHR surface;
    CHECK(CreateWin32SurfaceKHR(inst, &sci, nullptr, &surface));

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
    VkBool32 canPresent = VK_FALSE;
    GetPhysicalDeviceSurfaceSupportKHR(pd, 0, surface, &canPresent);
    if (!canPresent) {
        printf("queue family 0 cannot present\n");
        return 100;
    }
    VkPhysicalDeviceMemoryProperties memProps;
    GetPhysicalDeviceMemoryProperties(pd, &memProps);

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
    DFN(QueuePresentKHR); DFN(CreateImage); DFN(GetImageMemoryRequirements); DFN(AllocateMemory);
    DFN(BindImageMemory); DFN(CreateImageView); DFN(CreateRenderPass2); DFN(CreateRenderPass); DFN(CreateFramebuffer);
    DFN(CreateBuffer); DFN(GetBufferMemoryRequirements); DFN(BindBufferMemory); DFN(MapMemory);
    DFN(CreateDescriptorSetLayout); DFN(CreatePipelineLayout); DFN(CreateDescriptorPool); DFN(AllocateDescriptorSets);
    DFN(UpdateDescriptorSets); DFN(CreateCommandPool); DFN(AllocateCommandBuffers); DFN(BeginCommandBuffer);
    DFN(EndCommandBuffer); DFN(CmdBeginRenderPass2); DFN(CmdEndRenderPass2); DFN(CmdBeginRenderPass);
    DFN(CmdEndRenderPass); DFN(CmdBindDescriptorSets); DFN(CmdPipelineBarrier); DFN(CmdResolveImage);
    DFN(CmdCopyBufferToImage); DFN(DestroySwapchainKHR); DFN(DestroyFramebuffer); DFN(DestroyImageView);
    DFN(CreateFence); DFN(WaitForFences); DFN(ResetFences); DFN(CreateSemaphore); DFN(QueueSubmit);
    DFN(DeviceWaitIdle); DFN(DestroyDevice);

    VkQueue queue;
    GetDeviceQueue(dev, 0, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    printf("surface extent %ux%u (swapchain %ux%u)\n", caps.currentExtent.width, caps.currentExtent.height, W, H);
    VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = surface;
    swci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
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
    std::vector<VkImage> swapImages(nimg);
    GetSwapchainImagesKHR(dev, swapchain, &nimg, swapImages.data());

    auto memType = [&](uint32_t bits, VkMemoryPropertyFlags f) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & f) == f) return i;
        return 0u;
    };
    auto makeImage = [&](VkFormat fmt, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         VkImage& img, VkImageView& view) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = {W, H, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = samples;
        ci.usage = usage;
        CreateImage(dev, &ci, nullptr, &img);
        VkMemoryRequirements req;
        GetImageMemoryRequirements(dev, img, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory mem;
        AllocateMemory(dev, &ai, nullptr, &mem);
        BindImageMemory(dev, img, mem, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        CreateImageView(dev, &vi, nullptr, &view);
    };
    // RGBA8 instead of the game's R11G11B10F so the CPU-rendered scene uploads directly.
    const VkFormat kColor = VK_FORMAT_R8G8B8A8_UNORM, kDepth = VK_FORMAT_D24_UNORM_S8_UINT;
    VkImage msColor, msDepth, depth, hudless;
    VkImageView msColorV, msDepthV, depthV, hudlessV;
    makeImage(kColor, VK_SAMPLE_COUNT_2_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT, msColor, msColorV);
    makeImage(kDepth, VK_SAMPLE_COUNT_2_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
              VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, msDepth, msDepthV);
    makeImage(kDepth, VK_SAMPLE_COUNT_1_BIT,
              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
              VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, depth, depthV);
    makeImage(kColor, VK_SAMPLE_COUNT_1_BIT,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT, hudless, hudlessV);

    // Scene pass: MSAA colour + depth, depth resolved like the game's rp#25.
    VkAttachmentDescription2 att[3] = {};
    for (auto& a : att) {
        a.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    att[0].format = kColor;
    att[0].samples = VK_SAMPLE_COUNT_2_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    att[1].format = kDepth;
    att[1].samples = VK_SAMPLE_COUNT_2_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[2].format = kDepth;
    att[2].samples = VK_SAMPLE_COUNT_1_BIT;
    att[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference2 colorRef{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT};
    VkAttachmentReference2 depthRef{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1,
                                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
    VkAttachmentReference2 resolveRef{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 2,
                                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
    VkSubpassDescriptionDepthStencilResolve dsr{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
    dsr.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    dsr.stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    dsr.pDepthStencilResolveAttachment = &resolveRef;
    VkSubpassDescription2 sub{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    sub.pNext = &dsr;
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo2 rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    rpi.attachmentCount = 3;
    rpi.pAttachments = att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    VkRenderPass scenePass;
    CHECK(CreateRenderPass2(dev, &rpi, nullptr, &scenePass));
    VkImageView sceneViews[3] = {msColorV, msDepthV, depthV};
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass = scenePass;
    fbi.attachmentCount = 3;
    fbi.pAttachments = sceneViews;
    fbi.width = W;
    fbi.height = H;
    fbi.layers = 1;
    VkFramebuffer sceneFb;
    CHECK(CreateFramebuffer(dev, &fbi, nullptr, &sceneFb));

    // GUI pass straight onto the swapchain image.
    VkAttachmentDescription ga{};
    ga.format = swci.imageFormat;
    ga.samples = VK_SAMPLE_COUNT_1_BIT;
    ga.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ga.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ga.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ga.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ga.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference gref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription gsub{};
    gsub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    gsub.colorAttachmentCount = 1;
    gsub.pColorAttachments = &gref;
    VkRenderPassCreateInfo grpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    grpi.attachmentCount = 1;
    grpi.pAttachments = &ga;
    grpi.subpassCount = 1;
    grpi.pSubpasses = &gsub;
    VkRenderPass guiPass;
    CHECK(CreateRenderPass(dev, &grpi, nullptr, &guiPass));
    std::vector<VkFramebuffer> guiFbs;
    std::vector<VkImageView> guiViews;
    auto makeGuiFramebuffers = [&]() {
        guiFbs.assign(nimg, VK_NULL_HANDLE);
        guiViews.assign(nimg, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < nimg; ++i) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = swapImages[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = swci.imageFormat;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            CreateImageView(dev, &vi, nullptr, &guiViews[i]);
            VkFramebufferCreateInfo gfi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            gfi.renderPass = guiPass;
            gfi.attachmentCount = 1;
            gfi.pAttachments = &guiViews[i];
            gfi.width = W;
            gfi.height = H;
            gfi.layers = 1;
            CreateFramebuffer(dev, &gfi, nullptr, &guiFbs[i]);
        }
    };
    makeGuiFramebuffers();
    int recreations = 0;
    // What a game does on VK_ERROR_OUT_OF_DATE_KHR (alt-tab, resize ...).
    auto recreateSwapchain = [&]() {
        DeviceWaitIdle(dev);
        for (uint32_t i = 0; i < nimg; ++i) {
            DestroyFramebuffer(dev, guiFbs[i], nullptr);
            DestroyImageView(dev, guiViews[i], nullptr);
        }
        DestroySwapchainKHR(dev, swapchain, nullptr);
        if (CreateSwapchainKHR(dev, &swci, nullptr, &swapchain) != VK_SUCCESS) return false;
        GetSwapchainImagesKHR(dev, swapchain, &nimg, nullptr);
        swapImages.resize(nimg);
        GetSwapchainImagesKHR(dev, swapchain, &nimg, swapImages.data());
        makeGuiFramebuffers();
        recreations++;
        return true;
    };

    // Camera block at binding 10, one buffer per frame in flight.
    VkDescriptorSetLayoutBinding vb{10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings = &vb;
    VkDescriptorSetLayout dsl;
    CHECK(CreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    VkPipelineLayout pl;
    CHECK(CreatePipelineLayout(dev, &pli, nullptr, &pl));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 3;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VkDescriptorPool dp;
    CHECK(CreateDescriptorPool(dev, &dpi, nullptr, &dp));

    const int kFrames = 3;
    VkBuffer ubo[kFrames];
    float* uboPtr[kFrames];
    VkBuffer colorStage[kFrames], depthStage[kFrames];
    uint32_t* colorPtr[kFrames];
    uint32_t* depthPtr[kFrames];
    VkDescriptorSet sets[kFrames];
    VkCommandBuffer cbs[kFrames];
    VkFence fences[kFrames];
    VkSemaphore acquired[kFrames], rendered[kFrames];
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(CreateCommandPool(dev, &cpi, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.commandBufferCount = kFrames;
    CHECK(AllocateCommandBuffers(dev, &cai, cbs));
    for (int i = 0; i < kFrames; ++i) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 464;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        CreateBuffer(dev, &bi, nullptr, &ubo[i]);
        VkMemoryRequirements req;
        GetBufferMemoryRequirements(dev, ubo[i], &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory mem;
        AllocateMemory(dev, &ai, nullptr, &mem);
        BindBufferMemory(dev, ubo[i], mem, 0);
        MapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&uboPtr[i]));
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dp;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &dsl;
        AllocateDescriptorSets(dev, &dai, &sets[i]);
        VkDescriptorBufferInfo dbi{ubo[i], 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[i];
        w.dstBinding = 10;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &dbi;
        UpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        for (int k = 0; k < 2; ++k) {
            VkBuffer& sb = k ? depthStage[i] : colorStage[i];
            VkBufferCreateInfo sbi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            sbi.size = VkDeviceSize(W) * H * 4;
            sbi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            CreateBuffer(dev, &sbi, nullptr, &sb);
            VkMemoryRequirements sreq;
            GetBufferMemoryRequirements(dev, sb, &sreq);
            VkMemoryAllocateInfo sai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            sai.allocationSize = sreq.size;
            sai.memoryTypeIndex =
                memType(sreq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkDeviceMemory smem;
            AllocateMemory(dev, &sai, nullptr, &smem);
            BindBufferMemory(dev, sb, smem, 0);
            MapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(k ? &depthPtr[i] : &colorPtr[i]));
        }
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CreateFence(dev, &fi, nullptr, &fences[i]);
        VkSemaphoreCreateInfo smi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CreateSemaphore(dev, &smi, nullptr, &acquired[i]);
        CreateSemaphore(dev, &smi, nullptr, &rendered[i]);
    }

    double prevPv[16] = {}, sumStill = 0, sumIdeal = 0;
    uint64_t idealCount = 0;
    std::vector<uint32_t> prevColor(size_t(W) * H);
    for (int frame = 0; frame < 400; ++frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        int f = frame % kFrames;
        WaitForFences(dev, 1, &fences[f], VK_TRUE, UINT64_MAX);
        ResetFences(dev, 1, &fences[f]);
        uint32_t idx;
        VkResult ar = AcquireNextImageKHR(dev, swapchain, UINT64_MAX, acquired[f], VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
            // The fence was reset but nothing will signal it; recreate it signalled.
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            CreateFence(dev, &fi, nullptr, &fences[f]);
            if (recreations > 5 || !recreateSwapchain()) {
                printf("swapchain recreation failed or repeated, stopping at frame %d\n", frame);
                break;
            }
            continue;
        }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            printf("acquire returned %d at frame %d, stopping\n", ar, frame);
            break;
        }
        double pv[16], pvi[16];
        FillViewBlock(uboPtr[f], frame, W, H, pv, pvi);
        RenderScene(pv, pvi, W, H, colorPtr[f], depthPtr[f]);
        if (frame > 0) {
            IdealResidual(pvi, prevPv, colorPtr[f], depthPtr[f], prevColor.data(), W, H, sumStill, sumIdeal, idealCount);
        }
        memcpy(prevPv, pv, sizeof(pv));
        memcpy(prevColor.data(), colorPtr[f], prevColor.size() * 4);

        VkCommandBuffer cb = cbs[f];
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        BeginCommandBuffer(cb, &bi);
        VkClearValue clears[3] = {};
        clears[0].color = {{0.2f + 0.002f * frame, 0.4f, 0.6f, 1}};
        clears[1].depthStencil = {0.9995f, 0};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = scenePass;
        rbi.framebuffer = sceneFb;
        rbi.renderArea = {{0, 0}, {W, H}};
        rbi.clearValueCount = 3;
        rbi.pClearValues = clears;
        VkSubpassBeginInfo sbi{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};
        sbi.contents = VK_SUBPASS_CONTENTS_INLINE;
        VkSubpassEndInfo sei{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
        CmdBeginRenderPass2(cb, &rbi, &sbi);
        CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &sets[f], 0, nullptr);
        CmdEndRenderPass2(cb, &sei);

        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = hudless;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
        VkImageResolve res{};
        res.srcSubresource = res.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        res.extent = {W, H, 1};
        CmdResolveImage(cb, msColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hudless, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &res);
        // Replace the cleared scene with the ray-traced one (colour and depth).
        VkBufferImageCopy up{};
        up.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        up.imageExtent = {W, H, 1};
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
                           nullptr);
        CmdCopyBufferToImage(cb, colorStage[f], hudless, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
        VkImageMemoryBarrier db{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = depth;
        db.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        db.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        db.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        db.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        db.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &db);
        up.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        CmdCopyBufferToImage(cb, depthStage[f], depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
        db.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        db.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        db.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        db.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &db);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);

        VkClearValue gclear{};
        VkRenderPassBeginInfo gbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        gbi.renderPass = guiPass;
        gbi.framebuffer = guiFbs[idx];
        gbi.renderArea = {{0, 0}, {W, H}};
        gbi.clearValueCount = 1;
        gbi.pClearValues = &gclear;
        CmdBeginRenderPass(cb, &gbi, VK_SUBPASS_CONTENTS_INLINE);
        CmdEndRenderPass(cb);
        EndCommandBuffer(cb);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired[f];
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered[f];
        CHECK(QueueSubmit(queue, 1, &si, fences[f]));
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered[f];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &idx;
        VkResult pr = QueuePresentKHR(queue, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
            printf("present returned %d at frame %d (continuing)\n", pr, frame);
        }
    }
    DeviceWaitIdle(dev);
    DestroyDevice(dev, nullptr);
    printf("swapchain recreations: %d\n", recreations);
    printf("reference (CPU, exact motion): without motion vectors %.2f%%, with exact motion %.2f%%\n",
           100.0 * sumStill / idealCount, 100.0 * sumIdeal / idealCount);
    printf("frames done, validation errors: %d\n", g_validationErrors);
    return g_validationErrors;
}
