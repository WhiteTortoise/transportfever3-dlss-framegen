// NVIDIA Streamline integration: DLSS Frame Generation with Reflex.
//
// Frame flow (all on the presenting thread):
//   present end -> new frame token -> Reflex sleep -> simulation start
//   first queue submit -> simulation end, render submit start
//   present -> render submit end, tags + camera constants + options, present start/end
#include "streamline.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include "framegen.h"

namespace fg {
namespace {

HMODULE g_module = nullptr;
bool g_attempted = false;
bool g_active = false;
bool g_deviceReady = false;
bool g_vsyncOffRequired = false;
std::wstring g_dir;

PFun_slInit* g_slInit = nullptr;
PFun_slShutdown* g_slShutdown = nullptr;
PFun_slIsFeatureSupported* g_slIsFeatureSupported = nullptr;
PFun_slGetFeatureRequirements* g_slGetFeatureRequirements = nullptr;
PFun_slSetTagForFrame* g_slSetTagForFrame = nullptr;
PFun_slSetConstants* g_slSetConstants = nullptr;
PFun_slGetNewFrameToken* g_slGetNewFrameToken = nullptr;
PFun_slGetFeatureFunction* g_slGetFeatureFunction = nullptr;
PFun_slDLSSGSetOptions* g_slDLSSGSetOptions = nullptr;
PFun_slDLSSGGetState* g_slDLSSGGetState = nullptr;
PFun_slReflexSetOptions* g_slReflexSetOptions = nullptr;
PFun_slReflexSleep* g_slReflexSleep = nullptr;
PFun_slPCLSetMarker* g_slPCLSetMarker = nullptr;
PFN_vkGetInstanceProcAddr g_slGIPA = nullptr;
PFN_vkGetDeviceProcAddr g_slGDPA = nullptr;

const sl::ViewportHandle kViewport{0};
sl::FrameToken* g_token = nullptr;
std::atomic<bool> g_submitStarted{false};

bool g_userEnabled = true;  // Ctrl+Alt+<hotkey>, initial value from tf3_framegen.ini
bool g_modeKnown = false;
sl::DLSSGMode g_mode = sl::DLSSGMode::eOff;
DWORD g_lastStateLog = 0;
uint64_t g_framesSinceLog = 0;

void OnSlLog(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eInfo) return;  // full log goes to framegen\sl.log
    std::string m(msg ? msg : "");
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    FG_LOG("Streamline %s: %s", type == sl::LogType::eError ? "ERROR" : "warning", m.c_str());
}

template <typename T>
bool Resolve(T*& out, const char* name) {
    out = reinterpret_cast<T*>(GetProcAddress(g_module, name));
    if (!out) FG_LOG("Streamline: missing export %s", name);
    return out != nullptr;
}

template <typename T>
bool FeatureFn(sl::Feature feature, T*& out, const char* name) {
    void* fn = nullptr;
    if (g_slGetFeatureFunction(feature, name, fn) != sl::Result::eOk || !fn) {
        FG_LOG("Streamline: feature function %s unavailable", name);
        return false;
    }
    out = reinterpret_cast<T*>(fn);
    return true;
}

void Marker(sl::PCLMarker m) {
    if (g_slPCLSetMarker && g_token) g_slPCLSetMarker(m, *g_token);
}

void BeginFrame() {
    if (!g_deviceReady) return;
    if (g_slGetNewFrameToken(g_token, nullptr) != sl::Result::eOk) {
        g_token = nullptr;
        return;
    }
    if (g_slReflexSleep) g_slReflexSleep(*g_token);
    Marker(sl::PCLMarker::eSimulationStart);
    g_submitStarted = false;
}

// --- camera constants -------------------------------------------------------
// The game uses GL conventions (column vectors, clip z in [-1,1], depth =
// (z+1)/2). Streamline wants row vectors in row-major storage, which is the
// same memory layout as a column-major column-vector matrix, and a clip space
// whose z equals the depth buffer value, so z is remapped with T.

void Mul(const double* a, const double* b, double* o) {  // column-major o = a * b
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            o[c * 4 + r] = s;
        }
}

const double kT[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.5, 0, 0, 0, 0.5, 1};
const double kTInv[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, -1, 1};
const double kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

void Load(const float* src, double* dst) {
    for (int i = 0; i < 16; ++i) dst[i] = src[i];
}

void Store(const double* m, sl::float4x4& out) {
    float* p = &out[0].x;
    for (int i = 0; i < 16; ++i) p[i] = static_cast<float>(m[i]);
}

// T * a * b * TInv
void Remapped(const double* a, const double* b, double* out) {
    double t0[16], t1[16];
    Mul(kT, a, t0);
    Mul(t0, b, t1);
    Mul(t1, kTInv, out);
}

void FillConstants(sl::Constants& c, const float* cam, const float* prev, VkExtent2D extent) {
    double proj[16], projInv[16], pv[16], pvInv[16], tmp[16];
    Load(cam + 32, proj);
    Load(cam + 48, projInv);
    Load(cam + 0, pv);
    Load(cam + 16, pvInv);
    Mul(kT, proj, tmp);
    Store(tmp, c.cameraViewToClip);
    Mul(projInv, kTInv, tmp);
    Store(tmp, c.clipToCameraView);
    Store(kIdentity, c.clipToLensClip);
    if (prev) {
        double prevPv[16], prevPvInv[16];
        Load(prev + 0, prevPv);
        Load(prev + 16, prevPvInv);
        Remapped(prevPv, pvInv, tmp);
        Store(tmp, c.clipToPrevClip);
        Remapped(pv, prevPvInv, tmp);
        Store(tmp, c.prevClipToClip);
    } else {
        Store(kIdentity, c.clipToPrevClip);
        Store(kIdentity, c.prevClipToClip);
    }
    c.jitterOffset = {0.0f, 0.0f};
    c.mvecScale = {1.0f, 1.0f};  // motion vectors are already in UV units
    c.cameraPinholeOffset = {0.0f, 0.0f};
    // viewInverse columns: right, up, back; camPos after it.
    const float* vi = cam + 80;
    c.cameraRight = {vi[0], vi[1], vi[2]};
    c.cameraUp = {vi[4], vi[5], vi[6]};
    c.cameraFwd = {-vi[8], -vi[9], -vi[10]};
    c.cameraPos = {cam[96], cam[97], cam[98]};
    c.cameraNear = cam[104];
    c.cameraFar = cam[105];
    c.cameraFOV = cam[114];
    c.cameraAspectRatio = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = prev ? sl::Boolean::eFalse : sl::Boolean::eTrue;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
}

void FillIdleConstants(sl::Constants& c, VkExtent2D extent) {
    Store(kIdentity, c.cameraViewToClip);
    Store(kIdentity, c.clipToCameraView);
    Store(kIdentity, c.clipToLensClip);
    Store(kIdentity, c.clipToPrevClip);
    Store(kIdentity, c.prevClipToClip);
    c.jitterOffset = {0.0f, 0.0f};
    c.mvecScale = {1.0f, 1.0f};
    c.cameraPinholeOffset = {0.0f, 0.0f};
    c.cameraPos = {0.0f, 0.0f, 0.0f};
    c.cameraUp = {0.0f, 1.0f, 0.0f};
    c.cameraRight = {1.0f, 0.0f, 0.0f};
    c.cameraFwd = {0.0f, 0.0f, -1.0f};
    c.cameraNear = 1.0f;
    c.cameraFar = 1000.0f;
    c.cameraFOV = 1.0f;
    c.cameraAspectRatio = extent.height ? static_cast<float>(extent.width) / extent.height : 1.0f;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = sl::Boolean::eTrue;
}

sl::Resource MakeResource(VkImage image, VkImageView view, VkDeviceMemory memory, VkExtent2D extent, VkFormat format,
                          VkImageLayout layout, VkImageUsageFlags usage) {
    sl::Resource r(sl::ResourceType::eTex2d, reinterpret_cast<void*>(image), reinterpret_cast<void*>(memory),
                   reinterpret_cast<void*>(view), static_cast<uint32_t>(layout));
    r.width = extent.width;
    r.height = extent.height;
    r.nativeFormat = static_cast<uint32_t>(format);
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.flags = 0;
    r.usage = usage;
    return r;
}

const char* ModeName(sl::DLSSGMode m) {
    return m == sl::DLSSGMode::eOn ? "on" : m == sl::DLSSGMode::eOff ? "off" : "other";
}

void SetMode(sl::DLSSGMode mode) {
    if (!g_slDLSSGSetOptions || (g_modeKnown && mode == g_mode)) return;
    sl::DLSSGOptions o{};
    o.mode = mode;
    o.numFramesToGenerate = 1;
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    sl::Result r = g_slDLSSGSetOptions(kViewport, o);
    FG_LOG("DLSS Frame Generation %s (result %d)", ModeName(mode), static_cast<int>(r));
    g_mode = mode;
    g_modeKnown = true;
}

void LogState() {
    g_framesSinceLog++;
    DWORD now = GetTickCount();
    if (!g_slDLSSGGetState || now - g_lastStateLog < 5000) return;
    sl::DLSSGState s{};
    if (g_slDLSSGGetState(kViewport, s, nullptr) == sl::Result::eOk) {
        FG_LOG("DLSS-G state: status 0x%x, %u frames presented for %llu rendered in %.1f s, max generated %u",
               static_cast<uint32_t>(s.status), s.numFramesActuallyPresented,
               static_cast<unsigned long long>(g_framesSinceLog), (now - g_lastStateLog) / 1000.0, s.numFramesToGenerateMax);
    }
    g_lastStateLog = now;
    g_framesSinceLog = 0;
}

}  // namespace

bool SlLoadAndInit() {
    if (g_attempted) return g_active;
    g_attempted = true;
    g_dir = ModuleDir() + L"framegen\\";
    if (GetFileAttributesW((ModuleDir() + L"tf3_framegen.nofg").c_str()) != INVALID_FILE_ATTRIBUTES) {
        FG_LOG("Streamline: disabled by tf3_framegen.nofg");
        return false;
    }
    std::wstring interposer = g_dir + L"sl.interposer.dll";
    if (GetFileAttributesW(interposer.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FG_LOG("Streamline: %ls not found, frame generation unavailable", interposer.c_str());
        return false;
    }
    // Streamline and its plugins load further DLLs (NGX, Reflex) by name.
    SetDllDirectoryW(g_dir.c_str());
    g_module = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_module) {
        FG_LOG("Streamline: could not load sl.interposer.dll (error %lu)", GetLastError());
        return false;
    }
    bool ok = Resolve(g_slInit, "slInit") && Resolve(g_slShutdown, "slShutdown") &&
              Resolve(g_slIsFeatureSupported, "slIsFeatureSupported") &&
              Resolve(g_slGetFeatureRequirements, "slGetFeatureRequirements") &&
              Resolve(g_slSetTagForFrame, "slSetTagForFrame") && Resolve(g_slSetConstants, "slSetConstants") &&
              Resolve(g_slGetNewFrameToken, "slGetNewFrameToken") &&
              Resolve(g_slGetFeatureFunction, "slGetFeatureFunction");
    g_slGIPA = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g_module, "vkGetInstanceProcAddr"));
    g_slGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(g_module, "vkGetDeviceProcAddr"));
    if (!ok || !g_slGIPA || !g_slGDPA) return false;

    static const sl::Feature kFeatures[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    const wchar_t* paths[] = {g_dir.c_str()};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = g_dir.c_str();
    pref.logMessageCallback = &OnSlLog;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = kFeatures;
    pref.numFeaturesToLoad = 3;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = "4b0e6a5e-7c1f-4c43-9d8e-3f6f2e1a9b70";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = g_slInit(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        FG_LOG("Streamline: slInit failed (%d), frame generation unavailable", static_cast<int>(r));
        return false;
    }
    sl::FeatureRequirements req{};
    if (g_slGetFeatureRequirements(sl::kFeatureDLSS_G, req) == sl::Result::eOk) {
        g_vsyncOffRequired = (static_cast<uint32_t>(req.flags) &
                              static_cast<uint32_t>(sl::FeatureRequirementFlags::eVSyncOffRequired)) != 0;
        FG_LOG("Streamline: DLSS-G requirements flags 0x%x (vsync off required: %s)", static_cast<uint32_t>(req.flags),
               g_vsyncOffRequired ? "yes" : "no");
    }
    g_active = true;
    FG_LOG("Streamline initialized (manual hooking, plugins in %ls)", g_dir.c_str());
    return true;
}

bool SlActive() {
    return g_active;
}

bool SlWantsVsyncOff() {
    return g_active && g_deviceReady && g_vsyncOffRequired;
}

PFN_vkVoidFunction SlInstanceProc(const char* name) {
    return g_active ? g_slGIPA(VK_NULL_HANDLE, name) : nullptr;
}

PFN_vkVoidFunction SlDeviceProc(VkDevice device, const char* name) {
    return g_active ? g_slGDPA(device, name) : nullptr;
}

void SlOnDeviceCreated(VkPhysicalDevice physicalDevice, VkDevice device) {
    if (!g_active) return;
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physicalDevice;
    sl::Result supported = g_slIsFeatureSupported(sl::kFeatureDLSS_G, adapter);
    FG_LOG("Streamline: DLSS Frame Generation %s on this GPU (%d)",
           supported == sl::Result::eOk ? "supported" : "NOT supported", static_cast<int>(supported));

    FeatureFn(sl::kFeatureDLSS_G, g_slDLSSGSetOptions, "slDLSSGSetOptions");
    FeatureFn(sl::kFeatureDLSS_G, g_slDLSSGGetState, "slDLSSGGetState");
    FeatureFn(sl::kFeatureReflex, g_slReflexSetOptions, "slReflexSetOptions");
    FeatureFn(sl::kFeatureReflex, g_slReflexSleep, "slReflexSleep");
    FeatureFn(sl::kFeaturePCL, g_slPCLSetMarker, "slPCLSetMarker");

    if (g_slReflexSetOptions) {
        sl::ReflexOptions ro{};
        ro.mode = sl::ReflexMode::eLowLatency;
        sl::Result rr = g_slReflexSetOptions(ro);
        FG_LOG("Streamline: Reflex low latency on (%d)", static_cast<int>(rr));
    }
    (void)device;
    g_userEnabled = GetConfig().startEnabled;
    g_deviceReady = true;
    g_modeKnown = false;
    BeginFrame();
}

void SlShutdown() {
    if (!g_active) return;
    g_deviceReady = false;
    g_token = nullptr;
    sl::Result r = g_slShutdown();
    FG_LOG("Streamline shut down (%d)", static_cast<int>(r));
    g_active = false;
}

void SlDisableFrameGeneration() {
    g_deviceReady = false;
    g_token = nullptr;
}

void SlOnSubmit() {
    if (!g_deviceReady || g_submitStarted.exchange(true)) return;
    Marker(sl::PCLMarker::eSimulationEnd);
    Marker(sl::PCLMarker::eRenderSubmitStart);
}

void SlProvideFrame(const SlFrameInputs* in) {
    if (!g_deviceReady || !g_token) return;

    ReloadConfigIfChanged();
    static bool comboWasDown = false;
    bool comboDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                     (GetAsyncKeyState(GetConfig().hotkey) & 0x8000);
    if (comboDown && !comboWasDown) {
        g_userEnabled = !g_userEnabled;
        FG_LOG("frame generation toggled %s by hotkey", g_userEnabled ? "on" : "off");
    }
    comboWasDown = comboDown;

    sl::Constants c{};
    if (in) {
        FillConstants(c, in->camera, in->prevCamera, in->extent);
    } else {
        FillIdleConstants(c, {1920, 1080});
    }
    g_slSetConstants(c, *g_token, kViewport);

    if (in) {
        sl::Resource depth = MakeResource(in->depth, in->depthView, VK_NULL_HANDLE, in->extent, in->depthFormat,
                                          in->depthLayout, in->depthUsage);
        sl::Resource mvec = MakeResource(in->mvec, in->mvecView, in->mvecMemory, in->extent, in->mvecFormat,
                                         in->mvecLayout, in->mvecUsage);
        sl::Resource hudless = MakeResource(in->hudless, in->hudlessView, VK_NULL_HANDLE, in->extent,
                                            in->hudlessFormat, in->hudlessLayout, in->hudlessUsage);
        sl::Extent ext{0, 0, in->extent.width, in->extent.height};
        sl::ResourceTag tags[3] = {
            sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ext),
            sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &ext),
            sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &ext),
        };
        g_slSetTagForFrame(*g_token, kViewport, tags, 3, reinterpret_cast<sl::CommandBuffer*>(in->cb));
    } else {
        sl::ResourceTag tags[3] = {
            sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
        };
        g_slSetTagForFrame(*g_token, kViewport, tags, 3, nullptr);
    }
    SetMode(in && g_userEnabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff);
    LogState();
}

void SlPresentStart() {
    if (!g_deviceReady || !g_token) return;
    if (g_submitStarted) Marker(sl::PCLMarker::eRenderSubmitEnd);
    Marker(sl::PCLMarker::ePresentStart);
}

void SlPresentEnd() {
    if (!g_deviceReady || !g_token) return;
    Marker(sl::PCLMarker::ePresentEnd);
    BeginFrame();
}

}  // namespace fg
