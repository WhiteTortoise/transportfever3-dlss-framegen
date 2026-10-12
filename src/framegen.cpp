// Our own GPU work, recorded and submitted just before each present:
//  - camera motion vectors from the game's depth buffer and camera matrices,
//  - a self-check that warps the previous frame with them and measures the
//    remaining difference (logged), which also picks the screen/depth
//    conventions that fit the game,
//  - optional debug view (Ctrl+Alt+M) drawn into the top-right corner.
#include "framegen.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "shaders/errstat.h"
#include "shaders/mvgen.h"
#include "shaders/mvvis.h"
#include "streamline.h"

namespace fg {
namespace {

#define FG_DEV_FNS(X)                                                                                              \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdPipelineBarrier) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch)            \
    X(CmdCopyImage) X(CmdBlitImage) X(CmdFillBuffer) X(CreateFence) X(DestroyFence) X(WaitForFences)                \
    X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) X(CreateDescriptorSetLayout)                              \
    X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateDescriptorPool)          \
    X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler) X(DestroySampler)   \
    X(CreateImageView) X(DestroyImageView) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements)             \
    X(BindImageMemory) X(AllocateMemory) X(FreeMemory) X(CreateBuffer) X(DestroyBuffer)                             \
    X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(CreateShaderModule) X(DestroyShaderModule)    \
    X(CreateComputePipelines) X(DestroyPipeline) X(QueueSubmit)

struct DevFns {
#define X(n) PFN_vk##n n = nullptr;
    FG_DEV_FNS(X)
#undef X
} vk;

struct PushConstants {
    float reproj[16];
    float size[2];
    float invSize[2];
    int32_t flipY;
    int32_t depthMode;
    int32_t step;
    int32_t pad;
};
static_assert(sizeof(PushConstants) == 96, "must match shaders/common.glsl");

struct OwnedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

constexpr int kSlots = 3;
constexpr int kStatStep = 4;

struct Slot {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer stats = VK_NULL_HANDLE;
    VkDeviceMemory statsMemory = VK_NULL_HANDLE;
    uint32_t* statsPtr = nullptr;
    VkDescriptorSet set = VK_NULL_HANDLE;
    bool submitted = false;
    bool statsPending = false;
};

std::mutex g_mutex;
VkDevice g_device = VK_NULL_HANDLE;
VkPhysicalDeviceMemoryProperties g_memProps{};
bool g_failed = false;
std::unordered_map<VkQueue, uint32_t> g_queueFamilies;

// Created once per device.
VkDescriptorSetLayout g_setLayout = VK_NULL_HANDLE;
VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
VkDescriptorPool g_descriptorPool = VK_NULL_HANDLE;
VkPipeline g_mvgenPipe = VK_NULL_HANDLE, g_errstatPipe = VK_NULL_HANDLE, g_mvvisPipe = VK_NULL_HANDLE;
VkSampler g_pointSampler = VK_NULL_HANDLE, g_linearSampler = VK_NULL_HANDLE;
VkCommandPool g_commandPool = VK_NULL_HANDLE;
Slot g_slots[kSlots];
uint32_t g_nextSlot = 0;
bool g_deviceObjectsReady = false;

// Recreated when the extent or the scene colour format changes.
VkExtent2D g_extent{};
VkFormat g_prevFormat = VK_FORMAT_UNDEFINED;
OwnedImage g_mvImage, g_prevImage, g_visImage;
bool g_sizedReady = false;
bool g_sizedInitialized = false;  // layouts moved from UNDEFINED to GENERAL

std::unordered_map<VkImage, VkImageView> g_gameViews;

// Signalled by our submit and waited on by the present. One per swapchain
// image: a semaphore used by a present may only be reused once that image
// has been acquired again, which is guaranteed when it is presented again.
std::unordered_map<VkImage, VkSemaphore> g_presentSemaphores;

// Copy of the screen after the in-world labels/icons are drawn (before the
// windows/menus). Used as DLSS's UI-free input when icons should move with
// the camera in generated frames.
OwnedImage g_iconsImage;
VkFormat g_iconsFormat = VK_FORMAT_UNDEFINED;
VkExtent2D g_iconsExtent{};
std::vector<OwnedImage> g_retiredIconsImages;  // replaced while possibly in use; freed with the device
bool g_iconsCaptured = false;                   // a copy was recorded since the last present

// Camera of the previous frame.
bool g_havePrevCamera = false;
double g_prevProjView[16];
float g_prevCamera[kViewFloats];

// Self-check results.
struct MotionCheck {
    uint64_t sum[5] = {};
    uint64_t count[5] = {};
    uint32_t frames = 0;
} g_check;
DWORD g_lastCheckLog = 0;
int g_flipY = 1, g_depthMode = 0;
bool g_configLocked = false;
bool g_debugView = false;

#define VK_CHECK(expr)                                              \
    do {                                                            \
        VkResult r_ = (expr);                                       \
        if (r_ != VK_SUCCESS) {                                     \
            FG_LOG("framegen: %s failed (%d), disabled", #expr, r_); \
            g_failed = true;                                        \
            return false;                                           \
        }                                                           \
    } while (0)

uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < g_memProps.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (g_memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool CreateOwnedImage(OwnedImage& out, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VK_CHECK(vk.CreateImage(g_device, &ci, nullptr, &out.image));
    VkMemoryRequirements req;
    vk.GetImageMemoryRequirements(g_device, out.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vk.AllocateMemory(g_device, &ai, nullptr, &out.memory));
    VK_CHECK(vk.BindImageMemory(g_device, out.image, out.memory, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vk.CreateImageView(g_device, &vi, nullptr, &out.view));
    return true;
}

void DestroyOwnedImage(OwnedImage& img) {
    if (img.view) vk.DestroyImageView(g_device, img.view, nullptr);
    if (img.image) vk.DestroyImage(g_device, img.image, nullptr);
    if (img.memory) vk.FreeMemory(g_device, img.memory, nullptr);
    img = OwnedImage{};
}

bool CreatePipeline(const uint32_t* code, size_t bytes, VkPipeline& out) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule module;
    VK_CHECK(vk.CreateShaderModule(g_device, &mi, nullptr, &module));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = g_pipelineLayout;
    VkResult r = vk.CreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &ci, nullptr, &out);
    vk.DestroyShaderModule(g_device, module, nullptr);
    VK_CHECK(r);
    return true;
}

bool CreateDeviceObjects(uint32_t queueFamily) {
    VkDescriptorSetLayoutBinding b[6] = {};
    const VkDescriptorType types[6] = {
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    for (uint32_t i = 0; i < 6; ++i) {
        b[i].binding = i;
        b[i].descriptorType = types[i];
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6;
    li.pBindings = b;
    VK_CHECK(vk.CreateDescriptorSetLayout(g_device, &li, nullptr, &g_setLayout));

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK(vk.CreatePipelineLayout(g_device, &pli, nullptr, &g_pipelineLayout));

    if (!CreatePipeline(kMvgenSpv, sizeof(kMvgenSpv), g_mvgenPipe)) return false;
    if (!CreatePipeline(kErrstatSpv, sizeof(kErrstatSpv), g_errstatPipe)) return false;
    if (!CreatePipeline(kMvvisSpv, sizeof(kMvvisSpv), g_mvvisPipe)) return false;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vk.CreateSampler(g_device, &si, nullptr, &g_pointSampler));
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    VK_CHECK(vk.CreateSampler(g_device, &si, nullptr, &g_linearSampler));

    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kSlots},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kSlots},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kSlots;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    VK_CHECK(vk.CreateDescriptorPool(g_device, &dpi, nullptr, &g_descriptorPool));

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = queueFamily;
    VK_CHECK(vk.CreateCommandPool(g_device, &cpi, nullptr, &g_commandPool));

    for (Slot& s : g_slots) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = g_commandPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vk.AllocateCommandBuffers(g_device, &cai, &s.cb));
        // Command buffers allocated outside the loader need the loader's dispatch pointer.
        *reinterpret_cast<void**>(s.cb) = *reinterpret_cast<void**>(g_device);

        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vk.CreateFence(g_device, &fi, nullptr, &s.fence));

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 64;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vk.CreateBuffer(g_device, &bi, nullptr, &s.stats));
        VkMemoryRequirements req;
        vk.GetBufferMemoryRequirements(g_device, s.stats, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vk.AllocateMemory(g_device, &ai, nullptr, &s.statsMemory));
        VK_CHECK(vk.BindBufferMemory(g_device, s.stats, s.statsMemory, 0));
        VK_CHECK(vk.MapMemory(g_device, s.statsMemory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&s.statsPtr)));

        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = g_descriptorPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &g_setLayout;
        VK_CHECK(vk.AllocateDescriptorSets(g_device, &dai, &s.set));
    }
    g_deviceObjectsReady = true;
    FG_LOG("framegen: device objects created (queue family %u)", queueFamily);
    return true;
}

void WaitAllSlots() {
    for (Slot& s : g_slots) {
        if (s.submitted) {
            vk.WaitForFences(g_device, 1, &s.fence, VK_TRUE, 2000000000ull);
        }
    }
}

void DestroySized() {
    DestroyOwnedImage(g_mvImage);
    DestroyOwnedImage(g_prevImage);
    DestroyOwnedImage(g_visImage);
    g_sizedReady = false;
    g_sizedInitialized = false;
}

bool CreateSized(VkExtent2D extent, VkFormat sceneFormat) {
    const VkImageUsageFlags kStorage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    // RGBA16F rather than RG16F: storage use of RG16F needs an optional device feature.
    if (!CreateOwnedImage(g_mvImage, VK_FORMAT_R16G16B16A16_SFLOAT, extent,
                          kStorage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    if (!CreateOwnedImage(g_prevImage, sceneFormat, extent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        return false;
    }
    VkExtent2D half{(extent.width + 1) / 2, (extent.height + 1) / 2};
    if (!CreateOwnedImage(g_visImage, VK_FORMAT_R8G8B8A8_UNORM, half, kStorage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    g_extent = extent;
    g_prevFormat = sceneFormat;
    g_sizedReady = true;
    g_sizedInitialized = false;
    g_havePrevCamera = false;
    FG_LOG("framegen: frame resources created for %ux%u (scene format %d)", extent.width, extent.height, sceneFormat);
    return true;
}

bool IsDepthStencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

VkImageView GameView(VkImage image, VkFormat format, bool depth) {
    auto it = g_gameViews.find(image);
    if (it != g_gameViews.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {static_cast<VkImageAspectFlags>(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                           0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vk.CreateImageView(g_device, &vi, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_gameViews[image] = view;
    return view;
}

void Barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
             VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
             VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vk.CmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void MemoryBarrier(VkCommandBuffer cb, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags srcStage,
                   VkPipelineStageFlags dstStage) {
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m.srcAccessMask = src;
    m.dstAccessMask = dst;
    vk.CmdPipelineBarrier(cb, srcStage, dstStage, 0, 1, &m, 0, nullptr, 0, nullptr);
}

// Column-major 4x4: out = a * b.
void Mul(const double* a, const double* b, double* out) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
    }
}

void ReadSlotStats(Slot& s) {
    if (!s.statsPending) return;
    s.statsPending = false;
    for (int i = 0; i < 5; ++i) {
        g_check.sum[i] += s.statsPtr[i];
        g_check.count[i] += s.statsPtr[5 + i];
    }
    g_check.frames++;
}

void LogMotionCheck() {
    DWORD now = GetTickCount();
    if (now - g_lastCheckLog < 3000 || g_check.frames == 0) return;
    g_lastCheckLog = now;
    double avg[5];
    for (int i = 0; i < 5; ++i) {
        avg[i] = g_check.count[i] ? 100.0 * g_check.sum[i] / 1000.0 / g_check.count[i] : -1.0;
    }
    FG_LOG("motion check (%u moving frames): avg difference without motion vectors %.2f%% | flipY0/depth2d-1 "
           "%.2f%% | flipY1/depth2d-1 %.2f%% | flipY0/depth=z %.2f%% | flipY1/depth=z %.2f%% | using flipY%d depthMode%d",
           g_check.frames, avg[0], avg[1], avg[2], avg[3], avg[4], g_flipY, g_depthMode);
    if (!g_configLocked && g_check.frames >= 30) {
        int best = -1;
        for (int c = 0; c < 4; ++c) {
            if (avg[c + 1] >= 0 && (best < 0 || avg[c + 1] < avg[best + 1])) best = c;
        }
        if (best >= 0) {
            g_flipY = best & 1;
            g_depthMode = best >> 1;
            g_configLocked = true;
            FG_LOG("motion check: selected flipY%d depthMode%d (%.2f%% vs %.2f%% without motion vectors)", g_flipY,
                   g_depthMode, avg[best + 1], avg[0]);
        }
    }
    g_check = MotionCheck{};
}

}  // namespace

void FgOnDeviceCreated(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, const VkPhysicalDeviceMemoryProperties& memProps) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_device = device;
    g_memProps = memProps;
    g_failed = false;
    char env[8] = {};
    if (GetEnvironmentVariableA("TF3FG_DEBUG_VIEW", env, sizeof(env)) && env[0] == '1') g_debugView = true;
#define X(n)                                                                     \
    vk.n = reinterpret_cast<PFN_vk##n>(gdpa(device, "vk" #n));                   \
    if (!vk.n) {                                                                 \
        FG_LOG("framegen: missing vk" #n ", disabled");                          \
        g_failed = true;                                                         \
    }
    FG_DEV_FNS(X)
#undef X
}

void FgOnDeviceDestroyed() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_device || !g_deviceObjectsReady) {
        g_device = VK_NULL_HANDLE;
        return;
    }
    WaitAllSlots();
    DestroySized();
    DestroyOwnedImage(g_iconsImage);
    for (OwnedImage& img : g_retiredIconsImages) DestroyOwnedImage(img);
    g_retiredIconsImages.clear();
    for (auto& [image, view] : g_gameViews) vk.DestroyImageView(g_device, view, nullptr);
    g_gameViews.clear();
    for (auto& [image, sem] : g_presentSemaphores) vk.DestroySemaphore(g_device, sem, nullptr);
    g_presentSemaphores.clear();
    for (Slot& s : g_slots) {
        vk.DestroyFence(g_device, s.fence, nullptr);
        vk.DestroyBuffer(g_device, s.stats, nullptr);
        vk.FreeMemory(g_device, s.statsMemory, nullptr);
        s = Slot{};
    }
    vk.DestroyCommandPool(g_device, g_commandPool, nullptr);
    vk.DestroyDescriptorPool(g_device, g_descriptorPool, nullptr);
    vk.DestroyPipeline(g_device, g_mvgenPipe, nullptr);
    vk.DestroyPipeline(g_device, g_errstatPipe, nullptr);
    vk.DestroyPipeline(g_device, g_mvvisPipe, nullptr);
    vk.DestroyPipelineLayout(g_device, g_pipelineLayout, nullptr);
    vk.DestroyDescriptorSetLayout(g_device, g_setLayout, nullptr);
    vk.DestroySampler(g_device, g_pointSampler, nullptr);
    vk.DestroySampler(g_device, g_linearSampler, nullptr);
    g_deviceObjectsReady = false;
    g_device = VK_NULL_HANDLE;
}

void FgOnImageDestroyed(VkImage image) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_gameViews.find(image);
    if (it == g_gameViews.end()) return;
    WaitAllSlots();
    vk.DestroyImageView(g_device, it->second, nullptr);
    g_gameViews.erase(it);
}

void FgRecordWorldIconsCopy(VkCommandBuffer cb, VkImage swapImage, VkImageLayout swapLayout, VkFormat format,
                            VkExtent2D extent) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_failed || !g_device || !swapImage || !extent.width) return;
    if (!g_iconsImage.image || g_iconsFormat != format || g_iconsExtent.width != extent.width ||
        g_iconsExtent.height != extent.height) {
        if (g_iconsImage.image) g_retiredIconsImages.push_back(g_iconsImage);
        g_iconsImage = OwnedImage{};
        if (!CreateOwnedImage(g_iconsImage, format, extent,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            return;
        }
        g_iconsFormat = format;
        g_iconsExtent = extent;
        FG_LOG("framegen: world icons capture image created (%ux%u)", extent.width, extent.height);
    }
    const VkPipelineStageFlags kAll = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkPipelineStageFlags kTransfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkImageLayout src = swapLayout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL
                                                              : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Barrier(cb, swapImage, VK_IMAGE_ASPECT_COLOR_BIT, swapLayout, src, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, kTransfer);
    Barrier(cb, g_iconsImage.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, kAll, kTransfer);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {extent.width, extent.height, 1};
    vk.CmdCopyImage(cb, swapImage, src, g_iconsImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Barrier(cb, g_iconsImage.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            kTransfer, kAll);
    Barrier(cb, swapImage, VK_IMAGE_ASPECT_COLOR_BIT, src, swapLayout, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, kTransfer, kAll);
    g_iconsCaptured = true;
}

void FgSetQueueFamily(VkQueue queue, uint32_t family) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queueFamilies[queue] = family;
}

const VkPresentInfoKHR* FgOnPresent(VkQueue queue, const VkPresentInfoKHR* info, const PresentInputs* in,
                                    VkPresentInfoKHR& storage, bool* providedToSl) {
    std::lock_guard<std::mutex> lock(g_mutex);
    *providedToSl = false;

    static bool comboWasDown = false;
    bool comboDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                     (GetAsyncKeyState('M') & 0x8000);
    bool iconsReady = g_iconsCaptured && g_iconsImage.image;
    g_iconsCaptured = false;

    if (comboDown && !comboWasDown) {
        g_debugView = !g_debugView;
        FG_LOG("framegen: motion vector debug view %s", g_debugView ? "on" : "off");
    }
    comboWasDown = comboDown;

    if (!in || g_failed || !g_device) {
        g_havePrevCamera = false;
        return info;
    }
    uint32_t family = 0;
    auto qf = g_queueFamilies.find(queue);
    if (qf != g_queueFamilies.end()) family = qf->second;
    if (!g_deviceObjectsReady && !CreateDeviceObjects(family)) return info;
    if (!g_sizedReady || g_extent.width != in->extent.width || g_extent.height != in->extent.height ||
        g_prevFormat != in->hudlessFormat) {
        WaitAllSlots();
        DestroySized();
        if (!CreateSized(in->extent, in->hudlessFormat)) return info;
    }

    VkImageView depthView = GameView(in->depth, in->depthFormat, true);
    VkImageView hudlessView = GameView(in->hudless, in->hudlessFormat, false);
    if (!depthView || !hudlessView) {
        FG_LOG("framegen: could not create views on game images, disabled");
        g_failed = true;
        return info;
    }

    Slot& s = g_slots[g_nextSlot];
    g_nextSlot = (g_nextSlot + 1) % kSlots;
    if (s.submitted && vk.WaitForFences(g_device, 1, &s.fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) {
        FG_LOG("framegen: timed out waiting for our previous work, disabled");
        g_failed = true;
        return info;
    }
    ReadSlotStats(s);
    LogMotionCheck();
    vk.ResetFences(g_device, 1, &s.fence);

    // Camera reprojection: current clip space -> previous clip space.
    double projView[16], projViewInv[16];
    for (int i = 0; i < 16; ++i) {
        projView[i] = in->camera[i];
        projViewInv[i] = in->camera[16 + i];
    }
    PushConstants pc{};
    bool moved = false;
    double reproj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (g_havePrevCamera) {
        Mul(g_prevProjView, projViewInv, reproj);
        for (int i = 0; i < 16; ++i) {
            if (std::fabs(g_prevProjView[i] - projView[i]) > 1e-6 * (1.0 + std::fabs(projView[i]))) moved = true;
        }
    }
    for (int i = 0; i < 16; ++i) pc.reproj[i] = static_cast<float>(reproj[i]);
    pc.size[0] = static_cast<float>(in->extent.width);
    pc.size[1] = static_cast<float>(in->extent.height);
    pc.invSize[0] = 1.0f / pc.size[0];
    pc.invSize[1] = 1.0f / pc.size[1];
    pc.flipY = g_flipY;
    pc.depthMode = g_depthMode;
    pc.step = kStatStep;
    const bool selfCheck = GetConfig().selfCheck;
    bool runCheck = selfCheck && g_havePrevCamera && moved && g_sizedInitialized;

    // Descriptors.
    const VkImageLayout kRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo depthInfo{g_pointSampler, depthView, kRead};
    VkDescriptorImageInfo curInfo{g_linearSampler, hudlessView, kRead};
    VkDescriptorImageInfo prevInfo{g_linearSampler, g_prevImage.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mvInfo{VK_NULL_HANDLE, g_mvImage.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo statsInfo{s.stats, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo visInfo{VK_NULL_HANDLE, g_visImage.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[6] = {};
    for (uint32_t i = 0; i < 6; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = s.set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = w[1].descriptorType = w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &depthInfo;
    w[1].pImageInfo = &curInfo;
    w[2].pImageInfo = &prevInfo;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[3].pImageInfo = &mvInfo;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[4].pBufferInfo = &statsInfo;
    w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[5].pImageInfo = &visInfo;
    vk.UpdateDescriptorSets(g_device, 6, w, 0, nullptr);

    // Record.
    VkCommandBuffer cb = s.cb;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.BeginCommandBuffer(cb, &bi);

    const VkPipelineStageFlags kAll = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const VkPipelineStageFlags kTransfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkAccessFlags kAnyWrite = VK_ACCESS_MEMORY_WRITE_BIT;
    const VkAccessFlags kAnyAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkImageAspectFlags depthAspect =
        VK_IMAGE_ASPECT_DEPTH_BIT | (IsDepthStencil(in->depthFormat) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);

    if (!g_sizedInitialized) {
        Barrier(cb, g_mvImage.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                kAnyAccess, kAll, kAll);
        Barrier(cb, g_prevImage.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                kAnyAccess, kAll, kAll);
        Barrier(cb, g_visImage.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                kAnyAccess, kAll, kAll);
    }
    Barrier(cb, in->depth, depthAspect, in->depthLayout, kRead, kAnyWrite, VK_ACCESS_SHADER_READ_BIT, kAll, kCompute);
    Barrier(cb, in->hudless, VK_IMAGE_ASPECT_COLOR_BIT, in->hudlessLayout, kRead, kAnyWrite, VK_ACCESS_SHADER_READ_BIT,
            kAll, kCompute);
    vk.CmdFillBuffer(cb, s.stats, 0, VK_WHOLE_SIZE, 0);
    MemoryBarrier(cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, kTransfer,
                  kCompute);

    vk.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipelineLayout, 0, 1, &s.set, 0, nullptr);
    vk.CmdPushConstants(cb, g_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vk.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mvgenPipe);
    vk.CmdDispatch(cb, (in->extent.width + 7) / 8, (in->extent.height + 7) / 8, 1);
    if (runCheck) {
        vk.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_errstatPipe);
        uint32_t w4 = (in->extent.width + kStatStep - 1) / kStatStep, h4 = (in->extent.height + kStatStep - 1) / kStatStep;
        vk.CmdDispatch(cb, (w4 + 7) / 8, (h4 + 7) / 8, 1);
    }
    MemoryBarrier(cb, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT, kCompute,
                  kCompute | VK_PIPELINE_STAGE_HOST_BIT);

    if (g_debugView) {
        vk.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mvvisPipe);
        uint32_t hw = (in->extent.width + 1) / 2, hh = (in->extent.height + 1) / 2;
        vk.CmdDispatch(cb, (hw + 7) / 8, (hh + 7) / 8, 1);
        Barrier(cb, g_visImage.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, kCompute,
                kTransfer);
        Barrier(cb, in->swapImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAnyWrite, VK_ACCESS_TRANSFER_WRITE_BIT, kAll, kTransfer);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(hw), static_cast<int32_t>(hh), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {static_cast<int32_t>(in->extent.width - hw), 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(in->extent.width), static_cast<int32_t>(hh), 1};
        vk.CmdBlitImage(cb, g_visImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, in->swapImage,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        Barrier(cb, in->swapImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, kTransfer, kAll);
        Barrier(cb, g_visImage.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL, 0, 0, kTransfer, kAll);
    }

    VkImageLayout hudlessNow = kRead;
    if (selfCheck) {
        // Keep this frame's scene colour for next frame's self-check.
        Barrier(cb, in->hudless, VK_IMAGE_ASPECT_COLOR_BIT, kRead, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, kCompute, kTransfer);
        MemoryBarrier(cb, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute, kTransfer);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {in->extent.width, in->extent.height, 1};
        vk.CmdCopyImage(cb, in->hudless, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_prevImage.image,
                        VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        hudlessNow = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    // Hand the game's images back in the layouts it left them in.
    Barrier(cb, in->hudless, VK_IMAGE_ASPECT_COLOR_BIT, hudlessNow, in->hudlessLayout,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT, kAnyAccess, kTransfer | kCompute, kAll);
    Barrier(cb, in->depth, depthAspect, kRead, in->depthLayout, VK_ACCESS_SHADER_READ_BIT, kAnyAccess, kCompute, kAll);
    MemoryBarrier(cb, kAnyWrite, kAnyAccess, kAll, kAll);

    // Hand this frame to DLSS Frame Generation. Layouts are the ones the
    // images will be in at present time.
    if (SlActive()) {
        SlFrameInputs fi{};
        fi.cb = cb;
        fi.extent = in->extent;
        fi.depth = in->depth;
        fi.depthView = depthView;
        fi.depthFormat = in->depthFormat;
        fi.depthLayout = in->depthLayout;
        fi.depthUsage = in->depthUsage;
        fi.mvec = g_mvImage.image;
        fi.mvecView = g_mvImage.view;
        fi.mvecMemory = g_mvImage.memory;
        fi.mvecFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        fi.mvecLayout = VK_IMAGE_LAYOUT_GENERAL;
        fi.mvecUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        fi.hudless = in->hudless;
        fi.hudlessView = hudlessView;
        fi.hudlessFormat = in->hudlessFormat;
        fi.hudlessLayout = in->hudlessLayout;
        fi.hudlessUsage = in->hudlessUsage;
        if (iconsReady && g_iconsExtent.width == in->extent.width && g_iconsExtent.height == in->extent.height) {
            fi.hudless = g_iconsImage.image;
            fi.hudlessView = g_iconsImage.view;
            fi.hudlessFormat = g_iconsFormat;
            fi.hudlessLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fi.hudlessUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        }
        fi.camera = in->camera;
        fi.prevCamera = g_havePrevCamera ? g_prevCamera : nullptr;
        SlProvideFrame(&fi);
        *providedToSl = true;
    }
    vk.EndCommandBuffer(cb);

    VkSemaphore& done = g_presentSemaphores[in->swapImage];
    if (!done) {
        VkSemaphoreCreateInfo smi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vk.CreateSemaphore(g_device, &smi, nullptr, &done) != VK_SUCCESS) {
            FG_LOG("framegen: could not create semaphore, disabled");
            g_failed = true;
            return info;
        }
    }
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, kAll);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &done;
    VkResult r = vk.QueueSubmit(queue, 1, &si, s.fence);
    if (r != VK_SUCCESS) {
        FG_LOG("framegen: submit failed (%d), disabled", r);
        g_failed = true;
        return info;
    }
    s.submitted = true;
    s.statsPending = runCheck;
    g_sizedInitialized = true;
    for (int i = 0; i < 16; ++i) g_prevProjView[i] = projView[i];
    memcpy(g_prevCamera, in->camera, sizeof(g_prevCamera));
    g_havePrevCamera = true;

    storage = *info;
    storage.waitSemaphoreCount = 1;
    storage.pWaitSemaphores = &done;
    return &storage;
}

}  // namespace fg
