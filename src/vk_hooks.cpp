// Phase 1: frame recorder.
//
// Tracks Vulkan objects (images, render passes, pipelines, buffers ...) and
// records what every command buffer does. On a capture request (Ctrl+Alt+C)
// the next full frame is written to
// tf3_framegen.log: every submit, render pass, attachment, pipeline (with the
// shader names found in its SPIR-V) and the camera ("View") uniform buffer.
// Nothing about rendering is changed.
#include "common.h"
#include "framegen.h"
#include "streamline.h"

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fg {
namespace {

// ---------------------------------------------------------------------------
// Real entry points
// ---------------------------------------------------------------------------

#define FG_HOOKS(X)                                                                                         \
    X(GetInstanceProcAddr) X(GetDeviceProcAddr) X(CreateInstance) X(CreateDevice) X(DestroyDevice)          \
    X(CreateSwapchainKHR) X(GetSwapchainImagesKHR) X(QueuePresentKHR) X(QueueSubmit) X(QueueSubmit2)        \
    X(CreateImage) X(DestroyImage) X(CreateImageView) X(CreateFramebuffer) X(CreateRenderPass)               \
    X(CreateRenderPass2) X(CreateShaderModule) X(CreateGraphicsPipelines) X(CreateComputePipelines)         \
    X(CreateBuffer) X(DestroyBuffer) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory)                    \
    X(BindBufferMemory2) X(MapMemory) X(UnmapMemory) X(UpdateDescriptorSets)                                \
    X(CreateDescriptorUpdateTemplate) X(UpdateDescriptorSetWithTemplate) X(BeginCommandBuffer)              \
    X(CmdBeginRenderPass) X(CmdBeginRenderPass2) X(CmdEndRenderPass) X(CmdEndRenderPass2) X(CmdNextSubpass) \
    X(CmdNextSubpass2) X(CmdBeginRendering) X(CmdEndRendering) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
    X(CmdPushDescriptorSetKHR) X(CmdDraw) X(CmdDrawIndexed) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect)   \
    X(CmdDrawIndirectCount) X(CmdDrawIndexedIndirectCount) X(CmdDispatch) X(CmdDispatchIndirect)           \
    X(CmdCopyImage) X(CmdCopyImage2) X(CmdBlitImage) X(CmdBlitImage2) X(CmdResolveImage) X(CmdCopyBuffer)  \
    X(CmdUpdateBuffer) X(CmdExecuteCommands) X(WaitForFences) X(AcquireNextImageKHR) X(QueueWaitIdle)          \
    X(DeviceWaitIdle) X(CmdPipelineBarrier) X(CmdPipelineBarrier2) X(GetDeviceQueue) X(EnumeratePhysicalDevices)    \
    X(DestroyInstance) X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR) X(DestroySwapchainKHR)

struct RealFns {
#define X(n) PFN_vk##n n = nullptr;
    FG_HOOKS(X)
#undef X
};
RealFns real;

// Streamline's proxies for the calls it must see (manual hooking). Null when
// Streamline is not active, in which case the real functions are used.
#define FG_SL_INSTANCE_PROXIES(X) \
    X(CreateInstance) X(EnumeratePhysicalDevices) X(CreateDevice) X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR)
#define FG_SL_DEVICE_PROXIES(X)                                                                              \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) \
    X(DeviceWaitIdle)

struct SlProxies {
#define X(n) PFN_vk##n n = nullptr;
    FG_SL_INSTANCE_PROXIES(X)
    FG_SL_DEVICE_PROXIES(X)
#undef X
};
SlProxies slp;

// Streamline's proxy if it is active, the real function otherwise.
#define VIA_SL(n) (slp.n ? slp.n : real.n)

PFN_vkGetPhysicalDeviceSurfacePresentModesKHR real_GetPhysicalDeviceSurfacePresentModesKHR = nullptr;

PFN_vkGetPhysicalDeviceMemoryProperties real_GetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties real_GetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties real_GetPhysicalDeviceQueueFamilyProperties = nullptr;

VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkPhysicalDeviceMemoryProperties g_memProps{};

// The camera uniform block of the game's shaders (pbr/view.h.glsl), std140.
constexpr uint32_t kViewBinding = 10;
constexpr VkDeviceSize kViewBlockSize = 464;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void Appendf(std::string& out, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) {
        out.append(buf, std::min<size_t>(n, sizeof(buf) - 1));
    }
}

const void* FindInChain(const void* pNext, VkStructureType type) {
    for (auto s = static_cast<const VkBaseInStructure*>(pNext); s; s = s->pNext) {
        if (s->sType == type) {
            return s;
        }
    }
    return nullptr;
}

const char* FormatName(VkFormat f) {
    switch (f) {
        case VK_FORMAT_UNDEFINED: return "UNDEF";
        case VK_FORMAT_R8_UNORM: return "R8";
        case VK_FORMAT_R8G8_UNORM: return "RG8";
        case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case VK_FORMAT_B8G8R8A8_SRGB: return "BGRA8_SRGB";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10";
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10";
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "R11G11B10F";
        case VK_FORMAT_R16_SFLOAT: return "R16F";
        case VK_FORMAT_R16G16_SFLOAT: return "RG16F";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
        case VK_FORMAT_R16G16B16A16_UNORM: return "RGBA16";
        case VK_FORMAT_R32_SFLOAT: return "R32F";
        case VK_FORMAT_R32G32_SFLOAT: return "RG32F";
        case VK_FORMAT_R32G32B32A32_SFLOAT: return "RGBA32F";
        case VK_FORMAT_R32_UINT: return "R32UI";
        case VK_FORMAT_D16_UNORM: return "D16";
        case VK_FORMAT_D32_SFLOAT: return "D32F";
        case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32FS8";
        case VK_FORMAT_X8_D24_UNORM_PACK32: return "X8D24";
        default: {
            static thread_local char buf[16];
            snprintf(buf, sizeof(buf), "fmt%d", static_cast<int>(f));
            return buf;
        }
    }
}

const char* LoadOpName(VkAttachmentLoadOp op) {
    switch (op) {
        case VK_ATTACHMENT_LOAD_OP_LOAD: return "load";
        case VK_ATTACHMENT_LOAD_OP_CLEAR: return "clear";
        case VK_ATTACHMENT_LOAD_OP_DONT_CARE: return "dc";
        default: return "?";
    }
}

const char* StoreOpName(VkAttachmentStoreOp op) {
    switch (op) {
        case VK_ATTACHMENT_STORE_OP_STORE: return "store";
        case VK_ATTACHMENT_STORE_OP_DONT_CARE: return "dc";
        default: return "none";
    }
}

// ---------------------------------------------------------------------------
// Stutter profiler: per-frame time spent in expensive Vulkan calls, plus the
// CPU time of every game thread. Frames over kSpikeMs are logged in detail.
// ---------------------------------------------------------------------------

constexpr double kSpikeMs = 70.0;

double g_qpcToMs = 0.0;

int64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double QpcMs(int64_t ticks) {
    if (g_qpcToMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcToMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    return ticks * g_qpcToMs;
}

enum Stat {
    kStatFenceWait, kStatAcquire, kStatPresent, kStatSubmit, kStatIdleWait, kStatPipelines, kStatShaderModules,
    kStatAlloc, kStatCount
};
const char* kStatNames[kStatCount] = {"wait-GPU", "acquire", "present", "submit", "wait-idle", "pipeline-compile",
                                      "shader-modules", "mem-alloc"};

struct StatSlot {
    std::atomic<int64_t> ticks{0};
    std::atomic<uint32_t> calls{0};
};
StatSlot g_frameStats[kStatCount];
std::atomic<uint64_t> g_frameAllocBytes{0};

struct ScopedStat {
    Stat stat;
    int64_t start;
    explicit ScopedStat(Stat s) : stat(s), start(Qpc()) {}
    ~ScopedStat() {
        g_frameStats[stat].ticks += Qpc() - start;
        g_frameStats[stat].calls++;
    }
};

struct ThreadSample {
    HANDLE handle = nullptr;
    uint64_t lastCpu = 0;  // 100 ns units
    std::string name;
};
std::unordered_map<DWORD, ThreadSample> g_threads;
DWORD g_threadsRefreshTick = 0;

uint64_t ThreadCpu(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return (static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
           (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
}

void RefreshThreadList() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || g_threads.count(te.th32ThreadID)) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        ThreadSample& ts = g_threads[te.th32ThreadID];
        ts.handle = h;
        ts.lastCpu = ThreadCpu(h);
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
            char buf[64];
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, sizeof(buf), nullptr, nullptr);
            ts.name = buf;
            LocalFree(desc);
        }
    }
    CloseHandle(snap);
}

struct ThreadDelta {
    DWORD tid;
    double ms;
    const std::string* name;
};

// Per-thread CPU ms since the previous call, busiest first.
std::vector<ThreadDelta> SampleThreads() {
    DWORD now = GetTickCount();
    if (now - g_threadsRefreshTick > 2000) {
        g_threadsRefreshTick = now;
        RefreshThreadList();
    }
    std::vector<ThreadDelta> out;
    for (auto& [tid, ts] : g_threads) {
        uint64_t cpu = ThreadCpu(ts.handle);
        if (cpu > ts.lastCpu) out.push_back({tid, (cpu - ts.lastCpu) / 10000.0, &ts.name});
        ts.lastCpu = cpu;
    }
    std::sort(out.begin(), out.end(), [](const ThreadDelta& a, const ThreadDelta& b) { return a.ms > b.ms; });
    return out;
}

struct Window {
    std::vector<double> frameMs;
    uint32_t spikes = 0, spikesLogged = 0;
    double statMs[kStatCount] = {};
    uint32_t statCalls[kStatCount] = {};
} g_window;

void ProfileFrame(uint64_t frame, DWORD presentThread) {
    static int64_t lastPresent = 0;
    int64_t now = Qpc();
    double frameMs = lastPresent ? QpcMs(now - lastPresent) : 0.0;
    lastPresent = now;

    double ms[kStatCount];
    uint32_t calls[kStatCount];
    for (int i = 0; i < kStatCount; ++i) {
        ms[i] = QpcMs(g_frameStats[i].ticks.exchange(0));
        calls[i] = g_frameStats[i].calls.exchange(0);
        g_window.statMs[i] += ms[i];
        g_window.statCalls[i] += calls[i];
    }
    uint64_t allocBytes = g_frameAllocBytes.exchange(0);
    std::vector<ThreadDelta> threads = SampleThreads();
    if (frameMs <= 0.0) return;
    g_window.frameMs.push_back(frameMs);

    if (frameMs > kSpikeMs) {
        g_window.spikes++;
        if (g_window.spikesLogged++ < 15) {
            std::string s;
            Appendf(s, "SPIKE frame %llu: %.0f ms |", frame, frameMs);
            for (int i = 0; i < kStatCount; ++i) {
                if (calls[i] && ms[i] >= 0.5) Appendf(s, " %s %.0f ms (%u)", kStatNames[i], ms[i], calls[i]);
            }
            if (allocBytes) Appendf(s, " allocated %.1f MB", allocBytes / 1048576.0);
            s += " | busiest threads:";
            for (size_t i = 0; i < threads.size() && i < 5; ++i) {
                Appendf(s, " %s[%lu]%s %.0f ms", threads[i].name->empty() ? "?" : threads[i].name->c_str(),
                        threads[i].tid, threads[i].tid == presentThread ? "(render)" : "", threads[i].ms);
            }
            FG_LOG("%s", s.c_str());
        }
    }
}

void ProfileSummary(uint64_t frame) {
    std::vector<double>& f = g_window.frameMs;
    if (f.empty()) return;
    std::vector<double> sorted = f;
    std::sort(sorted.begin(), sorted.end());
    double total = 0;
    for (double v : f) total += v;
    std::string s;
    Appendf(s, "frame %llu: %.1f fps | frame ms median %.1f p95 %.1f max %.0f | spikes>%.0fms: %u |", frame,
            f.size() * 1000.0 / total, sorted[sorted.size() / 2], sorted[sorted.size() * 95 / 100], sorted.back(),
            kSpikeMs, g_window.spikes);
    for (int i = 0; i < kStatCount; ++i) {
        if (g_window.statCalls[i] && g_window.statMs[i] >= 1.0) {
            Appendf(s, " %s %.0f ms/%u", kStatNames[i], g_window.statMs[i], g_window.statCalls[i]);
        }
    }
    FG_LOG("%s", s.c_str());
    g_window = Window{};
}

// ---------------------------------------------------------------------------
// Object tracking
// ---------------------------------------------------------------------------

struct ImageInfo {
    uint32_t id = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    uint32_t mips = 1, layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = 0;
    bool swapchain = false;
};

struct ViewInfo {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

struct AttachmentDesc {
    VkFormat format;
    VkAttachmentLoadOp load;
    VkAttachmentStoreOp store;
    VkImageLayout initial, final;
};

struct RenderPassInfo {
    uint32_t id = 0;
    std::vector<AttachmentDesc> attachments;
    uint32_t subpasses = 0;
    int depthResolveAttachment = -1;  // MSAA depth is resolved into this attachment
};

struct FramebufferInfo {
    uint32_t id = 0;
    std::vector<VkImageView> views;
    uint32_t width = 0, height = 0;
};

struct ShaderInfo {
    uint32_t id = 0;
    uint32_t words = 0;
    uint64_t hash = 0;
    std::vector<std::string> names;  // OpName / OpMemberName strings
    bool usesView = false;
};

struct PipelineInfo {
    uint32_t id = 0;
    bool compute = false;
    std::vector<uint32_t> shaders;
};

struct MemoryInfo {
    VkDeviceSize size = 0;
    VkMemoryPropertyFlags flags = 0;
    uint8_t* mapped = nullptr;  // pointer corresponding to mapOffset
    VkDeviceSize mapOffset = 0;
};

struct BufferInfo {
    uint32_t id = 0;
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memoryOffset = 0;
};

struct UboRef {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, range = 0;
    bool dynamic = false;
};

struct TemplateInfo {
    bool hasView = false;
    size_t offset = 0;
    bool dynamic = false;
};

std::shared_mutex g_objMutex;
std::unordered_map<VkImage, ImageInfo> g_images;
std::unordered_map<VkImageView, ViewInfo> g_views;
std::unordered_map<VkRenderPass, RenderPassInfo> g_renderPasses;
std::unordered_map<VkFramebuffer, FramebufferInfo> g_framebuffers;
std::vector<ShaderInfo> g_shaderList;  // indexed by id, never shrinks
std::unordered_map<VkShaderModule, uint32_t> g_shaderModules;
std::unordered_map<VkPipeline, PipelineInfo> g_pipelines;
std::unordered_map<VkDeviceMemory, MemoryInfo> g_memory;
std::unordered_map<VkBuffer, BufferInfo> g_buffers;
std::unordered_map<VkDescriptorSet, UboRef> g_viewSets;
std::unordered_map<VkDescriptorUpdateTemplate, TemplateInfo> g_templates;
uint32_t g_nextImageId = 1, g_nextRpId = 1, g_nextFbId = 1, g_nextPipeId = 1, g_nextBufId = 1;

ShaderInfo ScanSpirv(const uint32_t* code, size_t bytes) {
    ShaderInfo si;
    size_t words = bytes / 4;
    si.words = static_cast<uint32_t>(words);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < words; ++i) {
        h = (h ^ code[i]) * 1099511628211ull;
    }
    si.hash = h;
    if (words < 5 || code[0] != 0x07230203) {
        return si;
    }
    std::set<std::string> seen;
    for (size_t i = 5; i < words;) {
        uint32_t op = code[i] & 0xFFFF, wc = code[i] >> 16;
        if (wc == 0 || i + wc > words) {
            break;
        }
        const char* str = nullptr;
        size_t maxLen = 0;
        if (op == 5 && wc > 2) {  // OpName
            str = reinterpret_cast<const char*>(&code[i + 2]);
            maxLen = (wc - 2) * 4;
        } else if (op == 6 && wc > 3) {  // OpMemberName
            str = reinterpret_cast<const char*>(&code[i + 3]);
            maxLen = (wc - 3) * 4;
        }
        if (str) {
            std::string s(str, strnlen(str, maxLen));
            if (!s.empty() && seen.insert(s).second) {
                si.names.push_back(s);
            }
            if (s == "projViewInverse") {
                si.usesView = true;
            }
        }
        i += wc;
    }
    return si;
}

uint32_t RegisterShader(VkShaderModule module, const uint32_t* code, size_t bytes) {
    ShaderInfo si = ScanSpirv(code, bytes);
    std::unique_lock lock(g_objMutex);
    si.id = static_cast<uint32_t>(g_shaderList.size());
    g_shaderList.push_back(std::move(si));
    if (module) {
        g_shaderModules[module] = g_shaderList.back().id;
    }
    return g_shaderList.back().id;
}

// ---------------------------------------------------------------------------
// Command buffer recording
// ---------------------------------------------------------------------------

enum class Ev : uint8_t {
    BeginRP, EndRP, NextSubpass, BeginRendering, EndRendering, BindPipeline, BindSets, PushView,
    Draw, Dispatch, CopyImage, BlitImage, ResolveImage, CopyBuffer, UpdateBuffer, Exec,
    Barrier,  // a = image, b = new layout
};

struct Event {
    Ev type;
    uint32_t count = 0;      // merged draws / dispatches
    uint32_t extra = 0;      // index into CmdLog::extra (variable data)
    uint32_t extraCount = 0;
    uint64_t a = 0, b = 0, c = 0, d = 0;
};

struct CmdLog {
    std::vector<Event> events;
    std::vector<uint64_t> extra;
    void Clear() {
        events.clear();
        extra.clear();
    }
    uint32_t PushExtra(const uint64_t* v, uint32_t n) {
        uint32_t at = static_cast<uint32_t>(extra.size());
        extra.insert(extra.end(), v, v + n);
        return at;
    }
};

std::mutex g_cbMutex;
std::unordered_map<VkCommandBuffer, std::unique_ptr<CmdLog>> g_cbLogs;

CmdLog* GetLog(VkCommandBuffer cb) {
    thread_local VkCommandBuffer lastCb = VK_NULL_HANDLE;
    thread_local CmdLog* lastLog = nullptr;
    if (cb == lastCb) {
        return lastLog;
    }
    std::lock_guard<std::mutex> lock(g_cbMutex);
    auto& slot = g_cbLogs[cb];
    if (!slot) {
        slot = std::make_unique<CmdLog>();
    }
    lastCb = cb;
    lastLog = slot.get();
    return lastLog;
}

template <typename T>
uint64_t H(T handle) {
    return reinterpret_cast<uint64_t>(handle);
}

void AddDraw(VkCommandBuffer cb, bool indirect) {
    CmdLog* log = GetLog(cb);
    if (!log->events.empty() && log->events.back().type == Ev::Draw) {
        log->events.back().count++;
        log->events.back().a += indirect ? 1 : 0;
        return;
    }
    Event e{Ev::Draw};
    e.count = 1;
    e.a = indirect ? 1 : 0;
    log->events.push_back(e);
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

std::atomic<uint64_t> g_frame{0};
std::atomic<bool> g_capturing{false};
bool g_captureRequested = false;
int g_captureIndex = 0;
std::mutex g_captureMutex;
std::string g_capture;
std::set<uint32_t> g_capturePipelines;
std::set<std::pair<VkBuffer, VkDeviceSize>> g_captureViewUbos;
uint32_t g_captureSubmit = 0;

std::string DescribeImageView(VkImageView view) {
    std::string s;
    auto v = g_views.find(view);
    if (v == g_views.end()) {
        Appendf(s, "view?%llx", H(view));
        return s;
    }
    auto img = g_images.find(v->second.image);
    if (img == g_images.end()) {
        Appendf(s, "img?%llx", H(v->second.image));
        return s;
    }
    const ImageInfo& ii = img->second;
    Appendf(s, "img#%u %s %ux%u", ii.id, FormatName(v->second.format), ii.extent.width, ii.extent.height);
    if (ii.layers > 1) Appendf(s, "x%u", ii.layers);
    if (ii.mips > 1) Appendf(s, " mips%u", ii.mips);
    if (ii.samples > 1) Appendf(s, " msaa%u", ii.samples);
    if (ii.swapchain) s += " SWAPCHAIN";
    return s;
}

std::string DescribeImage(VkImage image) {
    std::string s;
    auto img = g_images.find(image);
    if (img == g_images.end()) {
        Appendf(s, "img?%llx", H(image));
    } else {
        const ImageInfo& ii = img->second;
        Appendf(s, "img#%u %s %ux%u%s", ii.id, FormatName(ii.format), ii.extent.width, ii.extent.height,
                ii.swapchain ? " SWAPCHAIN" : "");
    }
    return s;
}

std::string DescribeBuffer(VkBuffer buffer) {
    std::string s;
    auto b = g_buffers.find(buffer);
    if (b == g_buffers.end()) {
        Appendf(s, "buf?%llx", H(buffer));
    } else {
        Appendf(s, "buf#%u(%llu)", b->second.id, static_cast<unsigned long long>(b->second.size));
    }
    return s;
}

void DumpLog(const CmdLog* log, std::string& out, int depth) {
    std::string pad(depth * 2 + 4, ' ');
    uint32_t lastPipe = UINT32_MAX;
    UboRef lastView{};
    for (const Event& e : log->events) {
        switch (e.type) {
            case Ev::BeginRP: {
                auto rpIt = g_renderPasses.find(reinterpret_cast<VkRenderPass>(e.a));
                auto fbIt = g_framebuffers.find(reinterpret_cast<VkFramebuffer>(e.b));
                Appendf(out, "%sRENDERPASS rp#%u fb#%u area %ux%u\n", pad.c_str(),
                        rpIt != g_renderPasses.end() ? rpIt->second.id : 0,
                        fbIt != g_framebuffers.end() ? fbIt->second.id : 0, static_cast<uint32_t>(e.c),
                        static_cast<uint32_t>(e.d));
                std::vector<VkImageView> views;
                if (e.extraCount) {
                    for (uint32_t i = 0; i < e.extraCount; ++i) {
                        views.push_back(reinterpret_cast<VkImageView>(log->extra[e.extra + i]));
                    }
                } else if (fbIt != g_framebuffers.end()) {
                    views = fbIt->second.views;
                }
                for (size_t i = 0; i < views.size(); ++i) {
                    Appendf(out, "%s  att%zu: %s", pad.c_str(), i, DescribeImageView(views[i]).c_str());
                    if (rpIt != g_renderPasses.end() && i < rpIt->second.attachments.size()) {
                        const AttachmentDesc& ad = rpIt->second.attachments[i];
                        Appendf(out, " [%s/%s layout %d->%d]", LoadOpName(ad.load), StoreOpName(ad.store),
                                ad.initial, ad.final);
                    }
                    out += "\n";
                }
                lastPipe = UINT32_MAX;
                break;
            }
            case Ev::BeginRendering: {
                Appendf(out, "%sRENDERING area %ux%u\n", pad.c_str(), static_cast<uint32_t>(e.c),
                        static_cast<uint32_t>(e.d));
                // extra: color views..., then depth view (marked by e.a = color count)
                for (uint32_t i = 0; i < e.extraCount; ++i) {
                    VkImageView v = reinterpret_cast<VkImageView>(log->extra[e.extra + i]);
                    if (!v) continue;
                    Appendf(out, "%s  %s%u: %s\n", pad.c_str(), i < e.a ? "color" : "depth", i,
                            DescribeImageView(v).c_str());
                }
                lastPipe = UINT32_MAX;
                break;
            }
            case Ev::EndRP:
            case Ev::EndRendering:
                Appendf(out, "%sEND\n", pad.c_str());
                break;
            case Ev::NextSubpass:
                Appendf(out, "%sNEXT SUBPASS\n", pad.c_str());
                break;
            case Ev::BindPipeline: {
                auto p = g_pipelines.find(reinterpret_cast<VkPipeline>(e.a));
                uint32_t id = p != g_pipelines.end() ? p->second.id : 0;
                if (id != lastPipe) {
                    Appendf(out, "%spipe#%u%s\n", pad.c_str(), id, e.b ? " (compute)" : "");
                    lastPipe = id;
                    g_capturePipelines.insert(id);
                }
                break;
            }
            case Ev::BindSets: {
                // extra: sets..., then dynamic offsets (e.b = set count)
                for (uint32_t i = 0; i < e.b; ++i) {
                    auto vs = g_viewSets.find(reinterpret_cast<VkDescriptorSet>(log->extra[e.extra + i]));
                    if (vs == g_viewSets.end()) continue;
                    if (vs->second.buffer == lastView.buffer && vs->second.offset == lastView.offset) continue;
                    lastView = vs->second;
                    Appendf(out, "%sView UBO set=%llu: %s +%llu range %llu%s", pad.c_str(), e.a + i,
                            DescribeBuffer(vs->second.buffer).c_str(), vs->second.offset, vs->second.range,
                            vs->second.dynamic ? " DYNAMIC" : "");
                    if (e.extraCount > e.b) {
                        out += " dynOffsets:";
                        for (uint32_t k = e.b; k < e.extraCount; ++k) Appendf(out, " %llu", log->extra[e.extra + k]);
                    }
                    out += "\n";
                    g_captureViewUbos.insert({vs->second.buffer, vs->second.offset});
                }
                break;
            }
            case Ev::PushView: {
                Appendf(out, "%sView UBO (push descriptor): %s +%llu range %llu\n", pad.c_str(),
                        DescribeBuffer(reinterpret_cast<VkBuffer>(e.a)).c_str(), e.b, e.c);
                g_captureViewUbos.insert({reinterpret_cast<VkBuffer>(e.a), e.b});
                break;
            }
            case Ev::Draw:
                Appendf(out, "%sdraws x%u%s\n", pad.c_str(), e.count, e.a ? " (some indirect)" : "");
                break;
            case Ev::Dispatch:
                Appendf(out, "%sdispatch x%u\n", pad.c_str(), e.count);
                break;
            case Ev::CopyImage:
            case Ev::BlitImage:
            case Ev::ResolveImage: {
                const char* what = e.type == Ev::CopyImage ? "COPY" : e.type == Ev::BlitImage ? "BLIT" : "RESOLVE";
                Appendf(out, "%s%s %s -> %s\n", pad.c_str(), what,
                        DescribeImage(reinterpret_cast<VkImage>(e.a)).c_str(),
                        DescribeImage(reinterpret_cast<VkImage>(e.b)).c_str());
                break;
            }
            case Ev::CopyBuffer:
                Appendf(out, "%sCOPYBUF %s -> %s (%llu regions, first dst +%llu size %llu)\n", pad.c_str(),
                        DescribeBuffer(reinterpret_cast<VkBuffer>(e.a)).c_str(),
                        DescribeBuffer(reinterpret_cast<VkBuffer>(e.b)).c_str(), static_cast<unsigned long long>(e.count),
                        e.c, e.d);
                break;
            case Ev::UpdateBuffer:
                Appendf(out, "%sUPDATEBUF %s +%llu size %llu\n", pad.c_str(),
                        DescribeBuffer(reinterpret_cast<VkBuffer>(e.a)).c_str(), e.b, e.c);
                break;
            case Ev::Barrier:
                break;
            case Ev::Exec:
                for (uint32_t i = 0; i < e.extraCount; ++i) {
                    VkCommandBuffer sec = reinterpret_cast<VkCommandBuffer>(log->extra[e.extra + i]);
                    Appendf(out, "%sEXECUTE secondary cb %llx\n", pad.c_str(), H(sec));
                    CmdLog* secLog = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(g_cbMutex);
                        auto it = g_cbLogs.find(sec);
                        if (it != g_cbLogs.end()) secLog = it->second.get();
                    }
                    if (secLog && depth < 3) DumpLog(secLog, out, depth + 1);
                }
                break;
        }
    }
}

void CaptureSubmit(VkQueue queue, const VkCommandBuffer* cbs, uint32_t count) {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    std::shared_lock objLock(g_objMutex);
    Appendf(g_capture, "  SUBMIT #%u queue %llx (%u command buffers)\n", g_captureSubmit++, H(queue), count);
    for (uint32_t i = 0; i < count; ++i) {
        CmdLog* log = nullptr;
        {
            std::lock_guard<std::mutex> cbLock(g_cbMutex);
            auto it = g_cbLogs.find(cbs[i]);
            if (it != g_cbLogs.end()) log = it->second.get();
        }
        Appendf(g_capture, "   cb %llx: %zu events\n", H(cbs[i]), log ? log->events.size() : 0);
        if (log) DumpLog(log, g_capture, 0);
    }
}

void DumpViewUbo(std::string& out, VkBuffer buffer, VkDeviceSize offset) {
    auto b = g_buffers.find(buffer);
    if (b == g_buffers.end()) return;
    auto m = g_memory.find(b->second.memory);
    Appendf(out, "  View UBO %s +%llu: memory flags 0x%x", DescribeBuffer(buffer).c_str(), offset,
            m != g_memory.end() ? m->second.flags : 0);
    if (m == g_memory.end() || !m->second.mapped) {
        out += " (not host-mapped)\n";
        return;
    }
    VkDeviceSize at = b->second.memoryOffset + offset;
    if (at < m->second.mapOffset || at + kViewBlockSize > m->second.mapOffset + m->second.size) {
        out += " (outside mapped range)\n";
        return;
    }
    const uint8_t* p = m->second.mapped + (at - m->second.mapOffset);
    const float* f = reinterpret_cast<const float*>(p);
    out += " host-mapped\n";
    const char* names[] = {"projView", "projViewInverse", "proj", "projInverse", "view", "viewInverse"};
    for (int k = 0; k < 6; ++k) {
        Appendf(out, "    %-16s", names[k]);
        for (int j = 0; j < 16; ++j) Appendf(out, " %.4g", f[k * 16 + j]);
        out += "\n";
    }
    const int32_t* iv = reinterpret_cast<const int32_t*>(p);
    Appendf(out, "    camPos %.2f %.2f %.2f  near %.4g far %.4g time %.3f size %dx%d fov %.4g mode %d\n", f[96], f[97],
            f[98], f[104], f[105], f[106], iv[107], iv[108], f[114], iv[115]);
}

void FinishCapture() {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    std::shared_lock objLock(g_objMutex);
    std::string& out = g_capture;
    out += "  --- pipelines used this frame ---\n";
    for (uint32_t pid : g_capturePipelines) {
        for (const auto& [handle, pi] : g_pipelines) {
            if (pi.id != pid) continue;
            Appendf(out, "  pipe#%u%s shaders:", pi.id, pi.compute ? " compute" : "");
            for (uint32_t sid : pi.shaders) Appendf(out, " s%u", sid);
            out += "\n";
            for (uint32_t sid : pi.shaders) {
                if (sid >= g_shaderList.size()) continue;
                const ShaderInfo& si = g_shaderList[sid];
                Appendf(out, "    s%u (%u words%s):", sid, si.words, si.usesView ? ", View" : "");
                size_t len = 0;
                for (const std::string& n : si.names) {
                    if (len > 600) {
                        out += " ...";
                        break;
                    }
                    out += ' ';
                    out += n;
                    len += n.size() + 1;
                }
                out += "\n";
            }
            break;
        }
    }
    out += "  --- camera data ---\n";
    for (const auto& [buffer, offset] : g_captureViewUbos) {
        DumpViewUbo(out, buffer, offset);
    }
    Appendf(out, "=== END CAPTURE %d ===\n", g_captureIndex);
    LogRaw(out);
    out.clear();
    g_capturePipelines.clear();
    g_captureViewUbos.clear();
}

// ---------------------------------------------------------------------------
// Instance / device hooks
// ---------------------------------------------------------------------------

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Hook_GetInstanceProcAddr(VkInstance instance, const char* name) {
    return InterceptVulkan(name, real.GetInstanceProcAddr(instance, name));
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Hook_GetDeviceProcAddr(VkDevice device, const char* name) {
    return InterceptVulkan(name, real.GetDeviceProcAddr(device, name));
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                                   VkInstance* out) {
    const VkApplicationInfo* app = ci->pApplicationInfo;
    FG_LOG("vkCreateInstance app='%s' engine='%s' api=%u.%u.%u", app && app->pApplicationName ? app->pApplicationName : "",
           app && app->pEngineName ? app->pEngineName : "", app ? VK_API_VERSION_MAJOR(app->apiVersion) : 0,
           app ? VK_API_VERSION_MINOR(app->apiVersion) : 0, app ? VK_API_VERSION_PATCH(app->apiVersion) : 0);
    for (uint32_t i = 0; i < ci->enabledLayerCount; ++i) FG_LOG("  layer %s", ci->ppEnabledLayerNames[i]);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) FG_LOG("  instance ext %s", ci->ppEnabledExtensionNames[i]);
    if (!real.CreateInstance) real.CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(LoaderProc("vkCreateInstance"));
    if (SlLoadAndInit()) {
#define X(n) slp.n = reinterpret_cast<PFN_vk##n>(SlInstanceProc("vk" #n));
        FG_SL_INSTANCE_PROXIES(X)
#undef X
    }
    VkResult r = VIA_SL(CreateInstance)(ci, alloc, out);
    if (r != VK_SUCCESS && slp.CreateInstance) {
        FG_LOG("vkCreateInstance through Streamline failed (%d), retrying without it", r);
        SlShutdown();
        slp = SlProxies{};
        r = real.CreateInstance(ci, alloc, out);
    }
    FG_LOG("vkCreateInstance -> %d%s", r, slp.CreateInstance ? " (through Streamline)" : "");
    if (r == VK_SUCCESS) {
        g_instance = *out;
        PFN_vkGetInstanceProcAddr gipa = real.GetInstanceProcAddr
                                              ? real.GetInstanceProcAddr
                                              : reinterpret_cast<PFN_vkGetInstanceProcAddr>(LoaderProc("vkGetInstanceProcAddr"));
        if (!real.GetInstanceProcAddr) real.GetInstanceProcAddr = gipa;
        real_GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties"));
        real_GetPhysicalDeviceProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(g_instance, "vkGetPhysicalDeviceProperties"));
        real_GetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            gipa(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        real_GetPhysicalDeviceSurfacePresentModesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
            gipa(g_instance, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    FG_LOG("vkDestroyInstance");
    real.DestroyInstance(instance, alloc);
    if (instance == g_instance) g_instance = VK_NULL_HANDLE;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_EnumeratePhysicalDevices(VkInstance instance, uint32_t* count,
                                                             VkPhysicalDevice* devices) {
    return VIA_SL(EnumeratePhysicalDevices)(instance, count, devices);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* ci,
                                                          const VkAllocationCallbacks* alloc, VkSurfaceKHR* out) {
    return VIA_SL(CreateWin32SurfaceKHR)(instance, ci, alloc, out);
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                  const VkAllocationCallbacks* alloc) {
    VIA_SL(DestroySurfaceKHR)(instance, surface, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                                 const VkAllocationCallbacks* alloc, VkDevice* out) {
    if (real_GetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties props;
        real_GetPhysicalDeviceProperties(phys, &props);
        FG_LOG("vkCreateDevice on '%s' (vendor 0x%x, driver 0x%x, api %u.%u.%u)", props.deviceName, props.vendorID,
               props.driverVersion, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
               VK_API_VERSION_PATCH(props.apiVersion));
    }
    if (real_GetPhysicalDeviceQueueFamilyProperties) {
        uint32_t n = 0;
        real_GetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
        std::vector<VkQueueFamilyProperties> fams(n);
        real_GetPhysicalDeviceQueueFamilyProperties(phys, &n, fams.data());
        for (uint32_t i = 0; i < n; ++i) {
            FG_LOG("  queue family %u: flags 0x%x count %u", i, fams[i].queueFlags, fams[i].queueCount);
        }
    }
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
        FG_LOG("  requested queues: family %u x%u", ci->pQueueCreateInfos[i].queueFamilyIndex,
               ci->pQueueCreateInfos[i].queueCount);
    }
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) FG_LOG("  device ext %s", ci->ppEnabledExtensionNames[i]);
    for (auto s = static_cast<const VkBaseInStructure*>(ci->pNext); s; s = s->pNext) {
        FG_LOG("  device pNext sType %d", s->sType);
    }
    VkResult r = VIA_SL(CreateDevice)(phys, ci, alloc, out);
    FG_LOG("vkCreateDevice -> %d (device %llx)%s", r, H(r == VK_SUCCESS ? *out : VK_NULL_HANDLE),
           slp.CreateDevice ? " (through Streamline)" : "");
    if (r == VK_SUCCESS) {
        if (g_device) FG_LOG("WARNING: second device created; tracking the newest one");
        g_device = *out;
        g_physicalDevice = phys;
        if (real_GetPhysicalDeviceMemoryProperties) real_GetPhysicalDeviceMemoryProperties(phys, &g_memProps);
        if (!real.GetDeviceProcAddr && real.GetInstanceProcAddr) {
            real.GetDeviceProcAddr =
                reinterpret_cast<PFN_vkGetDeviceProcAddr>(real.GetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr"));
        }
        if (real.GetDeviceProcAddr) FgOnDeviceCreated(g_device, real.GetDeviceProcAddr, g_memProps);
        if (SlActive() && slp.CreateDevice) {
#define X(n) slp.n = reinterpret_cast<PFN_vk##n>(SlDeviceProc(g_device, "vk" #n));
            FG_SL_DEVICE_PROXIES(X)
#undef X
            SlOnDeviceCreated(phys, g_device);
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    FG_LOG("vkDestroyDevice %llx", H(device));
    if (device == g_device) {
        FgOnDeviceDestroyed();
        SlShutdown();
        slp = SlProxies{};
    }
    real.DestroyDevice(device, alloc);
    if (device == g_device) g_device = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Swapchain / submit / present
// ---------------------------------------------------------------------------

VkExtent2D g_swapExtent{};
VkFormat g_swapFormat = VK_FORMAT_UNDEFINED;
std::unordered_map<VkSwapchainKHR, std::vector<VkImage>> g_swapchainImages;

// What the current frame provides for frame generation, worked out at submit
// time by replaying the recorded commands in submission order.
struct FrameFindings {
    bool haveCamera = false;
    float camera[kViewFloats];
    VkImage depth = VK_NULL_HANDLE;             // full-frame depth produced by an MSAA depth resolve
    VkImage hudless = VK_NULL_HANDLE;           // scene colour copied to the screen before the GUI
    VkImage hudlessCandidate = VK_NULL_HANDLE;  // last full-frame colour resolve so far
};
std::mutex g_frameMutex;
FrameFindings g_findings;
std::unordered_map<VkImage, VkImageLayout> g_layouts;  // last layout seen, in submission order

bool IsDepthFormat(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

const ImageInfo* FindImage(VkImage image) {
    auto it = g_images.find(image);
    return it != g_images.end() ? &it->second : nullptr;
}

bool IsFullFrame(const ImageInfo* ii) {
    return ii && ii->extent.width == g_swapExtent.width && ii->extent.height == g_swapExtent.height;
}

const uint8_t* MappedBufferData(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
    auto b = g_buffers.find(buffer);
    if (b == g_buffers.end()) return nullptr;
    auto m = g_memory.find(b->second.memory);
    if (m == g_memory.end() || !m->second.mapped) return nullptr;
    VkDeviceSize at = b->second.memoryOffset + offset;
    if (at < m->second.mapOffset || at + size > m->second.mapOffset + m->second.size) return nullptr;
    return m->second.mapped + (at - m->second.mapOffset);
}

// The main camera is the perspective View block rendered at full frame size
// (shadow cascades are orthographic, window previews are smaller).
void InspectViewSet(VkDescriptorSet set) {
    if (g_findings.haveCamera) return;
    auto vs = g_viewSets.find(set);
    if (vs == g_viewSets.end() || vs->second.dynamic) return;
    auto b = g_buffers.find(vs->second.buffer);
    if (b == g_buffers.end() || b->second.size < vs->second.offset + kViewBlockSize) return;
    auto f = reinterpret_cast<const float*>(MappedBufferData(vs->second.buffer, vs->second.offset, kViewBlockSize));
    if (!f) return;
    auto iv = reinterpret_cast<const int32_t*>(f);
    if (f[32 + 11] != -1.0f || f[32 + 15] != 0.0f) return;
    if (iv[107] != static_cast<int32_t>(g_swapExtent.width) || iv[108] != static_cast<int32_t>(g_swapExtent.height)) {
        return;
    }
    memcpy(g_findings.camera, f, sizeof(g_findings.camera));
    g_findings.haveCamera = true;
}

void AnalyzeLog(const CmdLog* log, int depth) {
    const RenderPassInfo* rp = nullptr;
    VkImage rpImages[16];
    uint32_t rpCount = 0;
    for (const Event& e : log->events) {
        switch (e.type) {
            case Ev::BeginRP: {
                rp = nullptr;
                rpCount = 0;
                auto rpIt = g_renderPasses.find(reinterpret_cast<VkRenderPass>(e.a));
                if (rpIt != g_renderPasses.end()) rp = &rpIt->second;
                auto fbIt = g_framebuffers.find(reinterpret_cast<VkFramebuffer>(e.b));
                uint32_t n = e.extraCount ? e.extraCount
                                          : (fbIt != g_framebuffers.end() ? static_cast<uint32_t>(fbIt->second.views.size()) : 0);
                for (uint32_t i = 0; i < n && rpCount < 16; ++i) {
                    VkImageView v = e.extraCount ? reinterpret_cast<VkImageView>(log->extra[e.extra + i])
                                                 : fbIt->second.views[i];
                    auto vi = g_views.find(v);
                    rpImages[rpCount++] = vi != g_views.end() ? vi->second.image : VK_NULL_HANDLE;
                }
                bool toScreen = false;
                for (uint32_t i = 0; i < rpCount; ++i) {
                    const ImageInfo* ii = FindImage(rpImages[i]);
                    if (ii && ii->swapchain) toScreen = true;
                }
                if (toScreen && g_findings.hudlessCandidate && !g_findings.hudless) {
                    g_findings.hudless = g_findings.hudlessCandidate;
                }
                if (rp && rp->depthResolveAttachment >= 0 && static_cast<uint32_t>(rp->depthResolveAttachment) < rpCount) {
                    VkImage img = rpImages[rp->depthResolveAttachment];
                    if (IsFullFrame(FindImage(img))) g_findings.depth = img;
                }
                break;
            }
            case Ev::EndRP:
                if (rp) {
                    for (uint32_t i = 0; i < rpCount && i < rp->attachments.size(); ++i) {
                        if (rpImages[i]) g_layouts[rpImages[i]] = rp->attachments[i].final;
                    }
                }
                rp = nullptr;
                rpCount = 0;
                break;
            case Ev::Barrier:
                g_layouts[reinterpret_cast<VkImage>(e.a)] = static_cast<VkImageLayout>(e.b);
                break;
            case Ev::CopyImage:
            case Ev::BlitImage:
            case Ev::ResolveImage: {
                VkImage dst = reinterpret_cast<VkImage>(e.b);
                g_layouts[dst] = static_cast<VkImageLayout>(e.d);
                if (e.type == Ev::ResolveImage) {
                    const ImageInfo* ii = FindImage(dst);
                    if (IsFullFrame(ii) && !IsDepthFormat(ii->format) && !ii->swapchain) g_findings.hudlessCandidate = dst;
                }
                break;
            }
            case Ev::BindSets:
                for (uint32_t i = 0; i < e.b; ++i) {
                    InspectViewSet(reinterpret_cast<VkDescriptorSet>(log->extra[e.extra + i]));
                }
                break;
            case Ev::Exec:
                for (uint32_t i = 0; i < e.extraCount && depth < 2; ++i) {
                    CmdLog* sec = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(g_cbMutex);
                        auto it = g_cbLogs.find(reinterpret_cast<VkCommandBuffer>(log->extra[e.extra + i]));
                        if (it != g_cbLogs.end()) sec = it->second.get();
                    }
                    if (sec) AnalyzeLog(sec, depth + 1);
                }
                break;
            default:
                break;
        }
    }
}

void AnalyzeSubmit(const VkCommandBuffer* cbs, uint32_t count) {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    std::shared_lock objLock(g_objMutex);
    for (uint32_t i = 0; i < count; ++i) {
        CmdLog* log = nullptr;
        {
            std::lock_guard<std::mutex> cbLock(g_cbMutex);
            auto it = g_cbLogs.find(cbs[i]);
            if (it != g_cbLogs.end()) log = it->second.get();
        }
        if (log) AnalyzeLog(log, 0);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                       const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    FG_LOG("vkCreateSwapchainKHR %ux%u %s images>=%u presentMode %d usage 0x%x", ci->imageExtent.width,
           ci->imageExtent.height, FormatName(ci->imageFormat), ci->minImageCount, ci->presentMode, ci->imageUsage);
    g_swapExtent = ci->imageExtent;
    g_swapFormat = ci->imageFormat;
    VkSwapchainCreateInfoKHR patched = *ci;
    if (SlWantsVsyncOff() && ci->presentMode == VK_PRESENT_MODE_FIFO_KHR && real_GetPhysicalDeviceSurfacePresentModesKHR) {
        // Frame Generation paces frames itself and needs VSync off on Vulkan.
        uint32_t n = 0;
        real_GetPhysicalDeviceSurfacePresentModesKHR(g_physicalDevice, ci->surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        real_GetPhysicalDeviceSurfacePresentModesKHR(g_physicalDevice, ci->surface, &n, modes.data());
        for (VkPresentModeKHR want : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
            if (std::find(modes.begin(), modes.end(), want) != modes.end()) {
                patched.presentMode = want;
                FG_LOG("  present mode changed to %d for frame generation", want);
                break;
            }
        }
    }
    VkResult r = VIA_SL(CreateSwapchainKHR)(device, &patched, alloc, out);
    FG_LOG("vkCreateSwapchainKHR -> %d", r);
    if (r != VK_SUCCESS && slp.CreateSwapchainKHR) {
        // Keep the game running: plain swapchain, frame generation off for this session.
        FG_LOG("frame generation could not attach to the swapchain; continuing without it");
        SlDisableFrameGeneration();
#define X(n) slp.n = nullptr;
        FG_SL_DEVICE_PROXIES(X)
#undef X
        r = real.CreateSwapchainKHR(device, ci, alloc, out);
        FG_LOG("vkCreateSwapchainKHR (without frame generation) -> %d", r);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc,
                                                    const VkAllocationCallbacks* alloc) {
    {
        std::unique_lock lock(g_objMutex);
        g_swapchainImages.erase(sc);
    }
    VIA_SL(DestroySwapchainKHR)(device, sc, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR sc, uint32_t* count,
                                                          VkImage* images) {
    VkResult r = VIA_SL(GetSwapchainImagesKHR)(device, sc, count, images);
    if (images && (r == VK_SUCCESS || r == VK_INCOMPLETE)) {
        std::unique_lock lock(g_objMutex);
        g_swapchainImages[sc].assign(images, images + *count);
        for (uint32_t i = 0; i < *count; ++i) {
            ImageInfo& ii = g_images[images[i]];
            if (!ii.id) ii.id = g_nextImageId++;
            ii.format = g_swapFormat;
            ii.extent = {g_swapExtent.width, g_swapExtent.height, 1};
            ii.swapchain = true;
            FG_LOG("  swapchain image %u = img#%u", i, ii.id);
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits,
                                                VkFence fence) {
    SlOnSubmit();
    for (uint32_t i = 0; i < count; ++i) {
        AnalyzeSubmit(submits[i].pCommandBuffers, submits[i].commandBufferCount);
        if (g_capturing) CaptureSubmit(queue, submits[i].pCommandBuffers, submits[i].commandBufferCount);
    }
    ScopedStat stat(kStatSubmit);
    return real.QueueSubmit(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_WaitForFences(VkDevice device, uint32_t count, const VkFence* fences,
                                                  VkBool32 waitAll, uint64_t timeout) {
    ScopedStat stat(kStatFenceWait);
    return real.WaitForFences(device, count, fences, waitAll, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR sc, uint64_t timeout,
                                                        VkSemaphore sem, VkFence fence, uint32_t* index) {
    ScopedStat stat(kStatAcquire);
    return VIA_SL(AcquireNextImageKHR)(device, sc, timeout, sem, fence, index);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueWaitIdle(VkQueue queue) {
    ScopedStat stat(kStatIdleWait);
    return real.QueueWaitIdle(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_DeviceWaitIdle(VkDevice device) {
    ScopedStat stat(kStatIdleWait);
    return VIA_SL(DeviceWaitIdle)(device);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits,
                                                 VkFence fence) {
    for (uint32_t i = 0; i < count; ++i) {
        std::vector<VkCommandBuffer> cbs;
        for (uint32_t k = 0; k < submits[i].commandBufferInfoCount; ++k) {
            cbs.push_back(submits[i].pCommandBufferInfos[k].commandBuffer);
        }
        AnalyzeSubmit(cbs.data(), static_cast<uint32_t>(cbs.size()));
        if (g_capturing) CaptureSubmit(queue, cbs.data(), static_cast<uint32_t>(cbs.size()));
    }
    SlOnSubmit();
    ScopedStat stat(kStatSubmit);
    return real.QueueSubmit2(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    PresentInputs inputs;
    bool haveInputs = CollectPresentInputs(info, inputs);
    static bool hadInputs = false;
    if (haveInputs != hadInputs) {
        FG_LOG("frame generation inputs %s", haveInputs ? "found (camera, depth, scene colour)" : "not available");
        hadInputs = haveInputs;
    }
    VkPresentInfoKHR storage;
    bool providedToSl = false;
    const VkPresentInfoKHR* presentInfo =
        FgOnPresent(queue, info, haveInputs ? &inputs : nullptr, storage, &providedToSl);
    if (!providedToSl) SlProvideFrame(nullptr);
    SlPresentStart();
    VkResult r;
    {
        ScopedStat stat(kStatPresent);
        r = VIA_SL(QueuePresentKHR)(queue, presentInfo);
    }
    SlPresentEnd();
    uint64_t frame = ++g_frame;

    ProfileFrame(frame, GetCurrentThreadId());
    static DWORD lastTick = GetTickCount();
    DWORD now = GetTickCount();
    if (now - lastTick >= 10000) {
        ProfileSummary(frame);
        lastTick = now;
    }

    if (g_capturing) {
        g_capturing = false;
        FinishCapture();
    }
    // Ctrl+Alt+C (F10 is the game's quicksave).
    static bool comboWasDown = false;
    bool comboDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                     (GetAsyncKeyState('C') & 0x8000);
    bool hotkey = comboDown && !comboWasDown;
    comboWasDown = comboDown;
    if (hotkey) {
        g_captureRequested = true;
    }
    if (g_captureRequested) {
        g_captureRequested = false;
        g_captureIndex++;
        FG_LOG("=== CAPTURE %d (frame %llu) ===", g_captureIndex, frame);
        g_captureSubmit = 0;
        g_capturing = true;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Resource hooks
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateImage(VkDevice device, const VkImageCreateInfo* ci,
                                                const VkAllocationCallbacks* alloc, VkImage* out) {
    VkResult r = real.CreateImage(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_objMutex);
        ImageInfo& ii = g_images[*out];
        ii.id = g_nextImageId++;
        ii.format = ci->format;
        ii.extent = ci->extent;
        ii.mips = ci->mipLevels;
        ii.layers = ci->arrayLayers;
        ii.samples = ci->samples;
        ii.usage = ci->usage;
        constexpr VkImageUsageFlags kAttach =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        if ((ci->usage & kAttach) && ci->extent.width >= 256) {
            FG_LOG("render target img#%u %s %ux%ux%u layers %u mips %u usage 0x%x", ii.id, FormatName(ci->format),
                   ci->extent.width, ci->extent.height, ci->extent.depth, ci->arrayLayers, ci->mipLevels, ci->usage);
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* alloc) {
    FgOnImageDestroyed(image);
    {
        std::lock_guard<std::mutex> frameLock(g_frameMutex);
        g_layouts.erase(image);
    }
    {
        std::unique_lock lock(g_objMutex);
        g_images.erase(image);
    }
    real.DestroyImage(device, image, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateImageView(VkDevice device, const VkImageViewCreateInfo* ci,
                                                    const VkAllocationCallbacks* alloc, VkImageView* out) {
    VkResult r = real.CreateImageView(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_objMutex);
        g_views[*out] = ViewInfo{ci->image, ci->format};
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* ci,
                                                      const VkAllocationCallbacks* alloc, VkFramebuffer* out) {
    VkResult r = real.CreateFramebuffer(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_objMutex);
        FramebufferInfo& fb = g_framebuffers[*out];
        fb.id = g_nextFbId++;
        fb.width = ci->width;
        fb.height = ci->height;
        if (!(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && ci->pAttachments) {
            fb.views.assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* ci,
                                                     const VkAllocationCallbacks* alloc, VkRenderPass* out) {
    VkResult r = real.CreateRenderPass(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_objMutex);
        RenderPassInfo& rp = g_renderPasses[*out];
        rp.id = g_nextRpId++;
        rp.subpasses = ci->subpassCount;
        for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
            const VkAttachmentDescription& a = ci->pAttachments[i];
            rp.attachments.push_back({a.format, a.loadOp, a.storeOp, a.initialLayout, a.finalLayout});
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* ci,
                                                      const VkAllocationCallbacks* alloc, VkRenderPass* out) {
    VkResult r = real.CreateRenderPass2(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_objMutex);
        RenderPassInfo& rp = g_renderPasses[*out];
        rp.id = g_nextRpId++;
        rp.subpasses = ci->subpassCount;
        for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
            const VkAttachmentDescription2& a = ci->pAttachments[i];
            rp.attachments.push_back({a.format, a.loadOp, a.storeOp, a.initialLayout, a.finalLayout});
        }
        for (uint32_t i = 0; i < ci->subpassCount; ++i) {
            auto dsr = static_cast<const VkSubpassDescriptionDepthStencilResolve*>(FindInChain(
                ci->pSubpasses[i].pNext, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE));
            if (dsr && dsr->pDepthStencilResolveAttachment &&
                dsr->pDepthStencilResolveAttachment->attachment != VK_ATTACHMENT_UNUSED) {
                rp.depthResolveAttachment = static_cast<int>(dsr->pDepthStencilResolveAttachment->attachment);
            }
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* ci,
                                                       const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    VkResult r;
    {
        ScopedStat stat(kStatShaderModules);
        r = real.CreateShaderModule(device, ci, alloc, out);
    }
    if (r == VK_SUCCESS) RegisterShader(*out, ci->pCode, ci->codeSize);
    return r;
}

uint32_t ShaderIdForStage(const VkPipelineShaderStageCreateInfo& st) {
    if (st.module) {
        std::shared_lock lock(g_objMutex);
        auto it = g_shaderModules.find(st.module);
        return it != g_shaderModules.end() ? it->second : UINT32_MAX;
    }
    auto inl = static_cast<const VkShaderModuleCreateInfo*>(
        FindInChain(st.pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO));
    return inl ? RegisterShader(VK_NULL_HANDLE, inl->pCode, inl->codeSize) : UINT32_MAX;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                            const VkGraphicsPipelineCreateInfo* cis,
                                                            const VkAllocationCallbacks* alloc, VkPipeline* out) {
    VkResult r;
    {
        ScopedStat stat(kStatPipelines);
        r = real.CreateGraphicsPipelines(device, cache, count, cis, alloc, out);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!out[i]) continue;
        PipelineInfo pi;
        for (uint32_t s = 0; s < cis[i].stageCount; ++s) pi.shaders.push_back(ShaderIdForStage(cis[i].pStages[s]));
        std::unique_lock lock(g_objMutex);
        pi.id = g_nextPipeId++;
        g_pipelines[out[i]] = std::move(pi);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                           const VkComputePipelineCreateInfo* cis,
                                                           const VkAllocationCallbacks* alloc, VkPipeline* out) {
    VkResult r;
    {
        ScopedStat stat(kStatPipelines);
        r = real.CreateComputePipelines(device, cache, count, cis, alloc, out);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!out[i]) continue;
        PipelineInfo pi;
        pi.compute = true;
        pi.shaders.push_back(ShaderIdForStage(cis[i].stage));
        std::unique_lock lock(g_objMutex);
        pi.id = g_nextPipeId++;
        g_pipelines[out[i]] = std::move(pi);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateBuffer(VkDevice device, const VkBufferCreateInfo* ci,
                                                 const VkAllocationCallbacks* alloc, VkBuffer* out) {
    VkResult r = real.CreateBuffer(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_objMutex);
        BufferInfo& b = g_buffers[*out];
        b.id = g_nextBufId++;
        b.size = ci->size;
        b.usage = ci->usage;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* alloc) {
    {
        std::unique_lock lock(g_objMutex);
        g_buffers.erase(buffer);
    }
    real.DestroyBuffer(device, buffer, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* ai,
                                                   const VkAllocationCallbacks* alloc, VkDeviceMemory* out) {
    VkResult r;
    {
        ScopedStat stat(kStatAlloc);
        r = real.AllocateMemory(device, ai, alloc, out);
    }
    g_frameAllocBytes += ai->allocationSize;
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_objMutex);
        MemoryInfo& m = g_memory[*out];
        m.size = ai->allocationSize;
        m.flags = ai->memoryTypeIndex < g_memProps.memoryTypeCount
                      ? g_memProps.memoryTypes[ai->memoryTypeIndex].propertyFlags
                      : 0;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* alloc) {
    {
        std::unique_lock lock(g_objMutex);
        g_memory.erase(memory);
    }
    real.FreeMemory(device, memory, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_BindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                                     VkDeviceSize offset) {
    VkResult r = real.BindBufferMemory(device, buffer, memory, offset);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_objMutex);
        BufferInfo& b = g_buffers[buffer];
        b.memory = memory;
        b.memoryOffset = offset;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_BindBufferMemory2(VkDevice device, uint32_t count,
                                                      const VkBindBufferMemoryInfo* infos) {
    VkResult r = real.BindBufferMemory2(device, count, infos);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_objMutex);
        for (uint32_t i = 0; i < count; ++i) {
            BufferInfo& b = g_buffers[infos[i].buffer];
            b.memory = infos[i].memory;
            b.memoryOffset = infos[i].memoryOffset;
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_MapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                              VkDeviceSize size, VkMemoryMapFlags flags, void** data) {
    VkResult r = real.MapMemory(device, memory, offset, size, flags, data);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_objMutex);
        MemoryInfo& m = g_memory[memory];
        m.mapped = static_cast<uint8_t*>(*data);
        m.mapOffset = offset;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_UnmapMemory(VkDevice device, VkDeviceMemory memory) {
    {
        std::unique_lock lock(g_objMutex);
        auto it = g_memory.find(memory);
        if (it != g_memory.end()) it->second.mapped = nullptr;
    }
    real.UnmapMemory(device, memory);
}

bool IsUbo(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
}

VKAPI_ATTR void VKAPI_CALL Hook_UpdateDescriptorSets(VkDevice device, uint32_t writeCount,
                                                     const VkWriteDescriptorSet* writes, uint32_t copyCount,
                                                     const VkCopyDescriptorSet* copies) {
    for (uint32_t i = 0; i < writeCount; ++i) {
        const VkWriteDescriptorSet& w = writes[i];
        if (w.dstBinding == kViewBinding && w.dstArrayElement == 0 && IsUbo(w.descriptorType) && w.pBufferInfo &&
            (w.pBufferInfo[0].range == kViewBlockSize || w.pBufferInfo[0].range == VK_WHOLE_SIZE)) {
            std::unique_lock lock(g_objMutex);
            g_viewSets[w.dstSet] = UboRef{w.pBufferInfo[0].buffer, w.pBufferInfo[0].offset, w.pBufferInfo[0].range,
                                          w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC};
        }
    }
    real.UpdateDescriptorSets(device, writeCount, writes, copyCount, copies);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDescriptorUpdateTemplate(VkDevice device,
                                                                   const VkDescriptorUpdateTemplateCreateInfo* ci,
                                                                   const VkAllocationCallbacks* alloc,
                                                                   VkDescriptorUpdateTemplate* out) {
    VkResult r = real.CreateDescriptorUpdateTemplate(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        TemplateInfo ti;
        for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; ++i) {
            const VkDescriptorUpdateTemplateEntry& e = ci->pDescriptorUpdateEntries[i];
            if (e.dstBinding == kViewBinding && e.dstArrayElement == 0 && IsUbo(e.descriptorType)) {
                ti.hasView = true;
                ti.offset = e.offset;
                ti.dynamic = e.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            }
        }
        std::unique_lock lock(g_objMutex);
        g_templates[*out] = ti;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set,
                                                                VkDescriptorUpdateTemplate tmpl, const void* data) {
    {
        std::unique_lock lock(g_objMutex);
        auto it = g_templates.find(tmpl);
        if (it != g_templates.end() && it->second.hasView) {
            auto bi = reinterpret_cast<const VkDescriptorBufferInfo*>(static_cast<const uint8_t*>(data) + it->second.offset);
            if (bi->range == kViewBlockSize || bi->range == VK_WHOLE_SIZE) {
                g_viewSets[set] = UboRef{bi->buffer, bi->offset, bi->range, it->second.dynamic};
            }
        }
    }
    real.UpdateDescriptorSetWithTemplate(device, set, tmpl, data);
}

// ---------------------------------------------------------------------------
// Command recording hooks
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL Hook_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    GetLog(cb)->Clear();
    return real.BeginCommandBuffer(cb, bi);
}

void RecordBeginRP(VkCommandBuffer cb, const VkRenderPassBeginInfo* rp) {
    CmdLog* log = GetLog(cb);
    Event e{Ev::BeginRP};
    e.a = H(rp->renderPass);
    e.b = H(rp->framebuffer);
    e.c = rp->renderArea.extent.width;
    e.d = rp->renderArea.extent.height;
    auto att = static_cast<const VkRenderPassAttachmentBeginInfo*>(
        FindInChain(rp->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO));
    if (att && att->attachmentCount) {
        std::vector<uint64_t> v(att->attachmentCount);
        for (uint32_t i = 0; i < att->attachmentCount; ++i) v[i] = H(att->pAttachments[i]);
        e.extra = log->PushExtra(v.data(), att->attachmentCount);
        e.extraCount = att->attachmentCount;
    }
    log->events.push_back(e);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* rp,
                                                   VkSubpassContents contents) {
    RecordBeginRP(cb, rp);
    real.CmdBeginRenderPass(cb, rp, contents);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* rp,
                                                    const VkSubpassBeginInfo* sb) {
    RecordBeginRP(cb, rp);
    real.CmdBeginRenderPass2(cb, rp, sb);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdEndRenderPass(VkCommandBuffer cb) {
    GetLog(cb)->events.push_back(Event{Ev::EndRP});
    real.CmdEndRenderPass(cb);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo* se) {
    GetLog(cb)->events.push_back(Event{Ev::EndRP});
    real.CmdEndRenderPass2(cb, se);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdNextSubpass(VkCommandBuffer cb, VkSubpassContents contents) {
    GetLog(cb)->events.push_back(Event{Ev::NextSubpass});
    real.CmdNextSubpass(cb, contents);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdNextSubpass2(VkCommandBuffer cb, const VkSubpassBeginInfo* sb,
                                                const VkSubpassEndInfo* se) {
    GetLog(cb)->events.push_back(Event{Ev::NextSubpass});
    real.CmdNextSubpass2(cb, sb, se);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    CmdLog* log = GetLog(cb);
    Event e{Ev::BeginRendering};
    e.a = ri->colorAttachmentCount;
    e.c = ri->renderArea.extent.width;
    e.d = ri->renderArea.extent.height;
    std::vector<uint64_t> v;
    for (uint32_t i = 0; i < ri->colorAttachmentCount; ++i) v.push_back(H(ri->pColorAttachments[i].imageView));
    v.push_back(ri->pDepthAttachment ? H(ri->pDepthAttachment->imageView) : 0);
    e.extra = log->PushExtra(v.data(), static_cast<uint32_t>(v.size()));
    e.extraCount = static_cast<uint32_t>(v.size());
    log->events.push_back(e);
    real.CmdBeginRendering(cb, ri);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdEndRendering(VkCommandBuffer cb) {
    GetLog(cb)->events.push_back(Event{Ev::EndRendering});
    real.CmdEndRendering(cb);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    Event e{Ev::BindPipeline};
    e.a = H(p);
    e.b = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    GetLog(cb)->events.push_back(e);
    real.CmdBindPipeline(cb, bp, p);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp,
                                                      VkPipelineLayout layout, uint32_t first, uint32_t count,
                                                      const VkDescriptorSet* sets, uint32_t dynCount,
                                                      const uint32_t* dyn) {
    CmdLog* log = GetLog(cb);
    Event e{Ev::BindSets};
    e.a = first;
    e.b = count;
    std::vector<uint64_t> v;
    for (uint32_t i = 0; i < count; ++i) v.push_back(H(sets[i]));
    for (uint32_t i = 0; i < dynCount; ++i) v.push_back(dyn[i]);
    e.extra = log->PushExtra(v.data(), static_cast<uint32_t>(v.size()));
    e.extraCount = static_cast<uint32_t>(v.size());
    log->events.push_back(e);
    real.CmdBindDescriptorSets(cb, bp, layout, first, count, sets, dynCount, dyn);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint bp,
                                                        VkPipelineLayout layout, uint32_t set, uint32_t count,
                                                        const VkWriteDescriptorSet* writes) {
    for (uint32_t i = 0; i < count; ++i) {
        const VkWriteDescriptorSet& w = writes[i];
        if (w.dstBinding == kViewBinding && IsUbo(w.descriptorType) && w.pBufferInfo) {
            Event e{Ev::PushView};
            e.a = H(w.pBufferInfo[0].buffer);
            e.b = w.pBufferInfo[0].offset;
            e.c = w.pBufferInfo[0].range;
            GetLog(cb)->events.push_back(e);
        }
    }
    real.CmdPushDescriptorSetKHR(cb, bp, layout, set, count, writes);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    AddDraw(cb, false);
    real.CmdDraw(cb, vc, ic, fv, fi);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDrawIndexed(VkCommandBuffer cb, uint32_t idx, uint32_t ic, uint32_t fi, int32_t vo,
                                               uint32_t finst) {
    AddDraw(cb, false);
    real.CmdDrawIndexed(cb, idx, ic, fi, vo, finst);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    AddDraw(cb, true);
    real.CmdDrawIndirect(cb, b, o, n, s);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n,
                                                       uint32_t s) {
    AddDraw(cb, true);
    real.CmdDrawIndexedIndirect(cb, b, o, n, s);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf,
                                                     VkDeviceSize co, uint32_t n, uint32_t s) {
    AddDraw(cb, true);
    real.CmdDrawIndirectCount(cb, b, o, cbuf, co, n, s);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o,
                                                            VkBuffer cbuf, VkDeviceSize co, uint32_t n, uint32_t s) {
    AddDraw(cb, true);
    real.CmdDrawIndexedIndirectCount(cb, b, o, cbuf, co, n, s);
}

void AddDispatch(VkCommandBuffer cb) {
    CmdLog* log = GetLog(cb);
    if (!log->events.empty() && log->events.back().type == Ev::Dispatch) {
        log->events.back().count++;
        return;
    }
    Event e{Ev::Dispatch};
    e.count = 1;
    log->events.push_back(e);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    AddDispatch(cb);
    real.CmdDispatch(cb, x, y, z);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) {
    AddDispatch(cb);
    real.CmdDispatchIndirect(cb, b, o);
}

void AddImageOp(VkCommandBuffer cb, Ev type, VkImage src, VkImage dst, VkImageLayout srcLayout,
                VkImageLayout dstLayout) {
    Event e{type};
    e.a = H(src);
    e.b = H(dst);
    e.c = srcLayout;
    e.d = dstLayout;
    GetLog(cb)->events.push_back(e);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
                                             VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
    AddImageOp(cb, Ev::CopyImage, src, dst, sl, dl);
    real.CmdCopyImage(cb, src, sl, dst, dl, n, r);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyImage2(VkCommandBuffer cb, const VkCopyImageInfo2* info) {
    AddImageOp(cb, Ev::CopyImage, info->srcImage, info->dstImage, info->srcImageLayout, info->dstImageLayout);
    real.CmdCopyImage2(cb, info);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
                                             VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f) {
    AddImageOp(cb, Ev::BlitImage, src, dst, sl, dl);
    real.CmdBlitImage(cb, src, sl, dst, dl, n, r, f);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBlitImage2(VkCommandBuffer cb, const VkBlitImageInfo2* info) {
    AddImageOp(cb, Ev::BlitImage, info->srcImage, info->dstImage, info->srcImageLayout, info->dstImageLayout);
    real.CmdBlitImage2(cb, info);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
                                                VkImageLayout dl, uint32_t n, const VkImageResolve* r) {
    AddImageOp(cb, Ev::ResolveImage, src, dst, sl, dl);
    real.CmdResolveImage(cb, src, sl, dst, dl, n, r);
}

void AddBarrier(CmdLog* log, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    if (oldLayout == newLayout) return;
    Event e{Ev::Barrier};
    e.a = H(image);
    e.b = newLayout;
    log->events.push_back(e);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src,
                                                   VkPipelineStageFlags dst, VkDependencyFlags flags, uint32_t memCount,
                                                   const VkMemoryBarrier* mem, uint32_t bufCount,
                                                   const VkBufferMemoryBarrier* bufs, uint32_t imgCount,
                                                   const VkImageMemoryBarrier* imgs) {
    if (imgCount) {
        CmdLog* log = GetLog(cb);
        for (uint32_t i = 0; i < imgCount; ++i) AddBarrier(log, imgs[i].image, imgs[i].oldLayout, imgs[i].newLayout);
    }
    real.CmdPipelineBarrier(cb, src, dst, flags, memCount, mem, bufCount, bufs, imgCount, imgs);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* dep) {
    if (dep->imageMemoryBarrierCount) {
        CmdLog* log = GetLog(cb);
        for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; ++i) {
            const VkImageMemoryBarrier2& b = dep->pImageMemoryBarriers[i];
            AddBarrier(log, b.image, b.oldLayout, b.newLayout);
        }
    }
    real.CmdPipelineBarrier2(cb, dep);
}

VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    real.GetDeviceQueue(device, family, index, queue);
    if (*queue) FgSetQueueFamily(*queue, family);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n,
                                              const VkBufferCopy* r) {
    Event e{Ev::CopyBuffer};
    e.count = n;
    e.a = H(src);
    e.b = H(dst);
    e.c = n ? r[0].dstOffset : 0;
    e.d = n ? r[0].size : 0;
    GetLog(cb)->events.push_back(e);
    real.CmdCopyBuffer(cb, src, dst, n, r);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size,
                                                const void* data) {
    Event e{Ev::UpdateBuffer};
    e.a = H(dst);
    e.b = off;
    e.c = size;
    GetLog(cb)->events.push_back(e);
    real.CmdUpdateBuffer(cb, dst, off, size, data);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* cbs) {
    CmdLog* log = GetLog(cb);
    Event e{Ev::Exec};
    std::vector<uint64_t> v;
    for (uint32_t i = 0; i < n; ++i) v.push_back(H(cbs[i]));
    e.extra = log->PushExtra(v.data(), n);
    e.extraCount = n;
    log->events.push_back(e);
    real.CmdExecuteCommands(cb, n, cbs);
}

// ---------------------------------------------------------------------------
// Hook table
// ---------------------------------------------------------------------------

struct HookEntry {
    const char* name;
    void** slot;
    PFN_vkVoidFunction hook;
};

#define FG_ENTRY(n) {"vk" #n, reinterpret_cast<void**>(&real.n), reinterpret_cast<PFN_vkVoidFunction>(&Hook_##n)},
#define FG_ALIAS(alias, n) {alias, reinterpret_cast<void**>(&real.n), reinterpret_cast<PFN_vkVoidFunction>(&Hook_##n)},

const HookEntry kHooks[] = {
    FG_HOOKS(FG_ENTRY)
    FG_ALIAS("vkQueueSubmit2KHR", QueueSubmit2)
    FG_ALIAS("vkCreateRenderPass2KHR", CreateRenderPass2)
    FG_ALIAS("vkCmdBeginRenderPass2KHR", CmdBeginRenderPass2)
    FG_ALIAS("vkCmdEndRenderPass2KHR", CmdEndRenderPass2)
    FG_ALIAS("vkCmdNextSubpass2KHR", CmdNextSubpass2)
    FG_ALIAS("vkCmdBeginRenderingKHR", CmdBeginRendering)
    FG_ALIAS("vkCmdEndRenderingKHR", CmdEndRendering)
    FG_ALIAS("vkBindBufferMemory2KHR", BindBufferMemory2)
    FG_ALIAS("vkCreateDescriptorUpdateTemplateKHR", CreateDescriptorUpdateTemplate)
    FG_ALIAS("vkUpdateDescriptorSetWithTemplateKHR", UpdateDescriptorSetWithTemplate)
    FG_ALIAS("vkCmdDrawIndirectCountKHR", CmdDrawIndirectCount)
    FG_ALIAS("vkCmdDrawIndexedIndirectCountKHR", CmdDrawIndexedIndirectCount)
    FG_ALIAS("vkCmdCopyImage2KHR", CmdCopyImage2)
    FG_ALIAS("vkCmdBlitImage2KHR", CmdBlitImage2)
    FG_ALIAS("vkCmdPipelineBarrier2KHR", CmdPipelineBarrier2)
};

}  // namespace

PFN_vkVoidFunction InterceptVulkan(const char* name, PFN_vkVoidFunction realFn) {
    if (!realFn || !name) {
        return realFn;
    }
    for (const HookEntry& h : kHooks) {
        if (strcmp(h.name, name) == 0) {
            if (!*h.slot) {
                *h.slot = reinterpret_cast<void*>(realFn);
            }
            return h.hook;
        }
    }
    return realFn;
}

bool CollectPresentInputs(const VkPresentInfoKHR* info, PresentInputs& out) {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    FrameFindings f = g_findings;
    g_findings = FrameFindings{};
    if (!f.haveCamera || !f.depth || !f.hudless || info->swapchainCount != 1) return false;

    std::shared_lock objLock(g_objMutex);
    const ImageInfo* di = FindImage(f.depth);
    const ImageInfo* hi = FindImage(f.hudless);
    auto dl = g_layouts.find(f.depth);
    auto hl = g_layouts.find(f.hudless);
    auto sc = g_swapchainImages.find(info->pSwapchains[0]);
    if (!di || !hi || dl == g_layouts.end() || hl == g_layouts.end() || sc == g_swapchainImages.end() ||
        info->pImageIndices[0] >= sc->second.size()) {
        return false;
    }
    if (dl->second == VK_IMAGE_LAYOUT_UNDEFINED || hl->second == VK_IMAGE_LAYOUT_UNDEFINED) return false;
    if (!(di->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(hi->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !(hi->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    memcpy(out.camera, f.camera, sizeof(out.camera));
    out.extent = {di->extent.width, di->extent.height};
    out.depth = f.depth;
    out.depthFormat = di->format;
    out.depthLayout = dl->second;
    out.depthUsage = di->usage;
    out.hudless = f.hudless;
    out.hudlessFormat = hi->format;
    out.hudlessLayout = hl->second;
    out.hudlessUsage = hi->usage;
    out.swapImage = sc->second[info->pImageIndices[0]];
    out.swapFormat = g_swapFormat;
    return true;
}

}  // namespace fg
