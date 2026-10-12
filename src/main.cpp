// Entry point. We are loaded as winhttp.dll from the game folder:
//  1. forward every winhttp export to the real system DLL (generated stubs),
//  2. patch the game's import of GetProcAddress so that when it resolves
//     Vulkan functions from vulkan-1.dll it receives our hooks instead.
#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <mutex>

namespace {

struct ExportEntry {
    int ordinal;
    const char* name;
};

const ExportEntry kExports[] = {
#include "proxy/winhttp_exports.inc"
};

}  // namespace

// Jump table used by proxy/winhttp_stubs.asm.
extern "C" void* g_winhttpReal[sizeof(kExports) / sizeof(kExports[0])] = {};

namespace fg {
namespace {

std::wstring g_moduleDir;
Config g_config;
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
DWORD g_startTick = 0;

HMODULE g_vulkanModule = nullptr;
using PFN_GetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
PFN_GetProcAddress g_realGetProcAddress = nullptr;

void LoadRealWinhttp() {
    wchar_t sysDir[MAX_PATH];
    UINT n = GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring path(sysDir, n);
    path += L"\\winhttp.dll";
    HMODULE real = LoadLibraryW(path.c_str());
    if (!real) {
        return;
    }
    for (size_t i = 0; i < sizeof(kExports) / sizeof(kExports[0]); ++i) {
        const ExportEntry& e = kExports[i];
        g_winhttpReal[i] = reinterpret_cast<void*>(
            GetProcAddress(real, e.name ? e.name : MAKEINTRESOURCEA(e.ordinal)));
    }
}

const wchar_t kDefaultIni[] =
    L"; tf3_framegen settings. hotkey and self_check apply within 2 seconds, start_enabled at the next game start.\r\n"
    L"[framegen]\r\n"
    L"; on = frame generation starts enabled when a map loads, off = starts disabled\r\n"
    L"start_enabled=on\r\n"
    L"; Ctrl+Alt+<key> toggles frame generation in game: a letter, a digit, a symbol key such as \\, or F1-F12\r\n"
    L"hotkey=\\\r\n"
    L"; on = log how accurate the motion vectors are (diagnostics, costs a little GPU time)\r\n"
    L"self_check=off\r\n";

bool IniBool(const std::wstring& ini, const wchar_t* key, bool def) {
    wchar_t buf[16];
    GetPrivateProfileStringW(L"framegen", key, def ? L"on" : L"off", buf, 16, ini.c_str());
    return _wcsicmp(buf, L"on") == 0 || _wcsicmp(buf, L"true") == 0 || _wcsicmp(buf, L"1") == 0 ||
           _wcsicmp(buf, L"yes") == 0;
}

int ParseHotkey(const wchar_t* s) {
    if (s[0] && !s[1] && iswalnum(s[0])) return towupper(s[0]);
    if (s[0] && !s[1]) {
        // Punctuation such as \ ; ' / -> the key that types it on the current keyboard layout.
        SHORT vk = VkKeyScanW(s[0]);
        if (vk != -1) return vk & 0xFF;
    }
    if ((s[0] == L'F' || s[0] == L'f') && s[1]) {
        int n = _wtoi(s + 1);
        if (n >= 1 && n <= 12) return VK_F1 + n - 1;
    }
    return 0;
}

void LoadConfig() {
    std::wstring ini = g_moduleDir + L"tf3_framegen.ini";
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* exeName = wcsrchr(exe, L'\\');
    bool inGame = exeName && _wcsicmp(exeName + 1, L"TransportFever3.exe") == 0;
    // Only the game gets a default file (the test programs must not leave copies around).
    if (inGame && GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, ini.c_str(), L"wb") == 0 && f) {
            // UTF-16 LE so GetPrivateProfileString reads it as Unicode.
            const unsigned char bom[2] = {0xFF, 0xFE};
            fwrite(bom, 1, 2, f);
            fwrite(kDefaultIni, sizeof(wchar_t), wcslen(kDefaultIni), f);
            fclose(f);
        }
    }
    g_config.startEnabled = IniBool(ini, L"start_enabled", true);
    g_config.selfCheck = IniBool(ini, L"self_check", false);
    wchar_t key[16];
    GetPrivateProfileStringW(L"framegen", L"hotkey", L"\\", key, 16, ini.c_str());
    int vk = ParseHotkey(key);
    if (!vk) vk = ParseHotkey(L"\\");
    g_config.hotkey = vk ? vk : VK_OEM_5;
}

bool IsVulkanLoader(HMODULE mod) {
    if (!mod) {
        return false;
    }
    if (mod == g_vulkanModule) {
        return true;
    }
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(mod, path, MAX_PATH);
    if (len < 12) {
        return false;
    }
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (_wcsicmp(base, L"vulkan-1.dll") != 0) {
        return false;
    }
    if (!g_vulkanModule) {
        g_vulkanModule = mod;
        Log("vulkan loader: %ls", path);
    }
    return true;
}

FARPROC WINAPI Hook_GetProcAddress(HMODULE mod, LPCSTR name) {
    FARPROC real = g_realGetProcAddress(mod, name);
    if (!real || IS_INTRESOURCE(name) || name[0] != 'v' || name[1] != 'k' || !IsVulkanLoader(mod)) {
        return real;
    }
    return reinterpret_cast<FARPROC>(InterceptVulkan(name, reinterpret_cast<PFN_vkVoidFunction>(real)));
}

// Replace one entry in the import address table of `mod`.
bool PatchImport(HMODULE mod, const char* dll, const char* func, void* hook, void** original) {
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return false;
    }
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) {
            continue;
        }
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), func) != 0) {
                continue;
            }
            DWORD oldProtect;
            if (!VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                return false;
            }
            *original = reinterpret_cast<void*>(addrs->u1.Function);
            addrs->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&addrs->u1.Function, sizeof(void*), oldProtect, &oldProtect);
            return true;
        }
    }
    return false;
}

void Init(HMODULE self) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    g_moduleDir.assign(path, len);
    g_moduleDir.resize(g_moduleDir.find_last_of(L'\\') + 1);

    LoadRealWinhttp();

    // Kill switch: leave only the winhttp pass-through active.
    if (GetFileAttributesW((g_moduleDir + L"tf3_framegen.disable").c_str()) != INVALID_FILE_ATTRIBUTES) {
        return;
    }

    g_startTick = GetTickCount();
    // Shared so the log can be read while the game is running.
    g_logFile = _wfsopen((g_moduleDir + L"tf3_framegen.log").c_str(), L"w", _SH_DENYNO);
    Log("tf3_framegen loaded (build %s %s)", __DATE__, __TIME__);
    LoadConfig();
    Log("settings: start_enabled=%s hotkey=Ctrl+Alt+0x%02X self_check=%s", g_config.startEnabled ? "on" : "off",
        g_config.hotkey, g_config.selfCheck ? "on" : "off");

    HMODULE exe = GetModuleHandleW(nullptr);
    if (PatchImport(exe, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&Hook_GetProcAddress),
                    reinterpret_cast<void**>(&g_realGetProcAddress))) {
        Log("hooked GetProcAddress import of the game executable");
    } else {
        Log("ERROR: could not hook GetProcAddress; Vulkan will not be intercepted");
    }
    // SDL creates the window surface (SDL_Vulkan_CreateSurface) with Vulkan
    // functions it resolves itself; Streamline must see that call to learn
    // which window the swapchain belongs to.
    if (HMODULE sdl = GetModuleHandleW(L"SDL2.dll")) {
        void* original = nullptr;
        if (PatchImport(sdl, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&Hook_GetProcAddress),
                        &original)) {
            if (!g_realGetProcAddress) g_realGetProcAddress = reinterpret_cast<PFN_GetProcAddress>(original);
            Log("hooked GetProcAddress import of SDL2.dll");
        }
    }
}

}  // namespace

void Log(const char* fmt, ...) {
    if (!g_logFile) {
        return;
    }
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(g_logMutex);
    DWORD ms = GetTickCount() - g_startTick;
    fprintf(g_logFile, "[%6lu.%03lu] %s\n", ms / 1000, ms % 1000, buf);
    fflush(g_logFile);
}

void LogRaw(const std::string& text) {
    if (!g_logFile) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_logMutex);
    fwrite(text.data(), 1, text.size(), g_logFile);
    fflush(g_logFile);
}

const std::wstring& ModuleDir() {
    return g_moduleDir;
}

const Config& GetConfig() {
    return g_config;
}

void ReloadConfigIfChanged() {
    static DWORD lastCheck = 0;
    static FILETIME lastWrite{};
    DWORD now = GetTickCount();
    if (now - lastCheck < 2000) return;
    lastCheck = now;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW((g_moduleDir + L"tf3_framegen.ini").c_str(), GetFileExInfoStandard, &attr)) return;
    bool first = lastWrite.dwLowDateTime == 0 && lastWrite.dwHighDateTime == 0;
    if (CompareFileTime(&attr.ftLastWriteTime, &lastWrite) == 0) return;
    lastWrite = attr.ftLastWriteTime;
    if (first) return;  // already loaded at startup
    LoadConfig();
    Log("settings reloaded: hotkey=Ctrl+Alt+0x%02X self_check=%s", g_config.hotkey, g_config.selfCheck ? "on" : "off");
}

PFN_vkVoidFunction LoaderProc(const char* name) {
    if (!g_vulkanModule || !g_realGetProcAddress) {
        return nullptr;
    }
    return reinterpret_cast<PFN_vkVoidFunction>(g_realGetProcAddress(g_vulkanModule, name));
}

}  // namespace fg

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        fg::Init(self);
    }
    return TRUE;
}
