// ============================================================================
// Akai S5000 / S6000 / Z4 / Z8 / MPC4000 Windows 11 64-Bit WinUSB Shim & Scaler
// Version: v3.1.4 Production Release
// File: akS56USB_WinUSB_Shim.cpp -> Compiles to version.dll (x86)
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <winusb.h>
#include <usb.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <intrin.h>
#include <stdio.h>
#include <math.h>
#include <ole2.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shlwapi.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "oleaut32.lib")

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((ULONG_PTR)0x00000000L)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL ((ULONG_PTR)0xC0000001L)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING ((ULONG_PTR)0x00000103L)
#endif

// Standard Akai & WinUSB Interface GUIDs
DEFINE_GUID(GUID_DEVINTERFACE_AKAI_S56, 0x9D4B6529L, 0xD4A6, 0x11D3, 0x8E, 0xAE, 0x00, 0x50, 0xDA, 0x2E, 0x2D, 0x53);
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE_DEF, 0xA5DCBF10L, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB_DEF, 0x88BAE032L, 0x5A81, 0x49F0, 0xBC, 0x3D, 0xA4, 0xFF, 0x13, 0x82, 0x16, 0xD6);

static DWORD g_MainProcessId = 0;
static DWORD g_MainThreadId = 0;
static HMODULE g_hShimModule = NULL;

// Diagnostic builds only (cl /DAKSHIM_DIAG): window-sizing trace to %TEMP%\aksys_shim_diag.txt.
#ifdef AKSHIM_DIAG
static void DiagLog(const char* fmt, ...) {
    char line[1024];
    int n = snprintf(line, sizeof(line), "[%lu t%lu] ", GetTickCount(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof(line) - n - 3, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 3] = 0;
    strcat(line, "\r\n");
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "aksys_shim_diag.txt");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, line, (DWORD)strlen(line), &w, NULL);
        CloseHandle(h);
    }
}
static const char* DiagCallerName(void* retAddr) {
    static char name[MAX_PATH];
    HMODULE hMod = NULL;
    name[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)retAddr, &hMod) && hMod) {
        char full[MAX_PATH] = {0};
        GetModuleFileNameA(hMod, full, MAX_PATH - 1);
        const char* b = strrchr(full, '\\');
        strncpy(name, b ? b + 1 : full, MAX_PATH - 1);
    }
    return name;
}
#define DIAG(...) DiagLog(__VA_ARGS__)
static bool DiagIsSampleFile(const char* p) {
    if (!p) return false;
    char l[MAX_PATH] = {0};
    strncpy(l, p, sizeof(l) - 1);
    _strlwr(l);
    return strstr(l, ".wav") || strstr(l, "s56temp");
}
#define DIAG_FILE(path, ...) do { if (DiagIsSampleFile(path)) DiagLog(__VA_ARGS__); } while (0)
#else
#define DIAG(...) ((void)0)
#define DIAG_FILE(path, ...) ((void)0)
#endif

// ----------------------------------------------------------------------------
// Multi-File Drag-and-Drop & Memory Bank Directory Mirroring Engine
// ----------------------------------------------------------------------------
static bool g_InDragSession = false;
static bool g_DragTargetFolderActive = false;
static char g_DragTargetFolder[MAX_PATH] = {0};
static DWORD g_LastTransferTick = 0;

#define MAX_STAGED_FILES 128
struct StagedFileTracker {
    HANDLE hFile;
    char srcPath[MAX_PATH];
    char destPath[MAX_PATH];
    bool active;
};

#define MAX_PENDING_DROPS 128
struct PendingDropItem {
    char srcPath[MAX_PATH];
    char relPath[MAX_PATH];
};

static CRITICAL_SECTION g_StagedLock;
static StagedFileTracker g_StagedFiles[MAX_STAGED_FILES];
static PendingDropItem g_PendingDrops[MAX_PENDING_DROPS];
static int g_PendingDropCount = 0;

static bool CreateDirectoryRecursiveA(const char* path) {
    if (!path || !path[0]) return false;
    char temp[MAX_PATH];
    strncpy(temp, path, MAX_PATH - 1);
    temp[MAX_PATH - 1] = '\0';

    size_t len = strlen(temp);
    if (len > 0 && (temp[len - 1] == '\\' || temp[len - 1] == '/')) {
        temp[len - 1] = '\0';
    }

    DWORD attr = GetFileAttributesA(temp);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }

    char* p = strrchr(temp, '\\');
    char* p2 = strrchr(temp, '/');
    if (p2 > p) p = p2;
    if (p && p != temp) {
        *p = '\0';
        if (strlen(temp) > 2 || (strlen(temp) == 2 && temp[1] != ':')) {
            CreateDirectoryRecursiveA(temp);
        }
        *p = '\\';
    }

    return (CreateDirectoryA(temp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
}

static void CreateParentDirectoryA(const char* filePath) {
    if (!filePath || !filePath[0]) return;
    char dir[MAX_PATH];
    strncpy(dir, filePath, MAX_PATH - 1);
    dir[MAX_PATH - 1] = '\0';
    char* lastSlash = strrchr(dir, '\\');
    char* lastSlash2 = strrchr(dir, '/');
    if (lastSlash2 > lastSlash) lastSlash = lastSlash2;
    if (lastSlash && lastSlash != dir) {
        *lastSlash = '\0';
        CreateDirectoryRecursiveA(dir);
    }
}

// ak.Sys keeps working files ("UploadCache" for samples pulled from the sampler, "S56Temp") in
// folders next to AKSys.exe. Under Program Files a normal user can't write there, so Windows
// silently redirects ak.Sys into its VirtualStore, which external editors such as WaveLab can't
// see: "Edit Sample" opened the wrong file and the saved edit never came back. Every file API
// the shim hooks therefore rewrites "<AKSys.exe folder>\<working folder>..." to the per-user
// "%LOCALAPPDATA%\AKAIPRO\aksys\<working folder>...", shared by ak.Sys, the editor and the
// folder watch, with no permission changes needed.
static const char* const kAksysWorkFolders[] = { "UploadCache", "S56Temp" };

static bool GetAksysWorkDirs(const char** installDir, const char** userDir) {
    static char s_install[MAX_PATH] = {0};
    static char s_user[MAX_PATH] = {0};
    static volatile LONG s_state = 0;  // 0 = not yet, 1 = ready, 2 = unavailable
    if (s_state == 0) {
        char exe[MAX_PATH] = {0};
        char localAppData[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, exe, MAX_PATH - 1);
        char* slash = strrchr(exe, '\\');
        if (slash) *slash = 0;
        if (exe[0] && GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH) && localAppData[0]) {
            strncpy(s_install, exe, MAX_PATH - 1);
            snprintf(s_user, sizeof(s_user), "%s\\AKAIPRO\\aksys", localAppData);
            s_state = 1;
        } else {
            s_state = 2;
        }
    }
    if (s_state != 1) return false;
    *installDir = s_install;
    *userDir = s_user;
    return true;
}

// If 'at' points to "<install dir>\<work folder>" (followed by '\', '"' or end), returns the
// length of the install-dir part to replace; otherwise 0.
static size_t MatchAksysWorkPath(const char* at, const char* installDir) {
    size_t dirLen = strlen(installDir);
    if (_strnicmp(at, installDir, dirLen) != 0 || at[dirLen] != '\\') return 0;
    const char* rest = at + dirLen + 1;
    for (int i = 0; i < (int)(sizeof(kAksysWorkFolders) / sizeof(kAksysWorkFolders[0])); i++) {
        size_t n = strlen(kAksysWorkFolders[i]);
        if (_strnicmp(rest, kAksysWorkFolders[i], n) == 0 &&
            (rest[n] == '\\' || rest[n] == '/' || rest[n] == '"' || rest[n] == 0)) {
            return dirLen;
        }
    }
    return 0;
}

// Rewrites every ak.Sys working-folder path inside 'in' (a plain path or a command line).
// Returns true and fills 'out' if anything changed.
static bool RedirectAksysWorkPaths(const char* in, char* out, size_t outSize) {
    const char* installDir;
    const char* userDir;
    if (!in || !in[0] || !GetAksysWorkDirs(&installDir, &userDir)) return false;
    size_t userLen = strlen(userDir);
    size_t o = 0;
    bool changed = false;
    for (const char* p = in; *p; ) {
        size_t m = MatchAksysWorkPath(p, installDir);
        if (m) {
            if (o + userLen >= outSize) return false;
            memcpy(out + o, userDir, userLen);
            o += userLen;
            p += m;
            changed = true;
        } else {
            if (o + 1 >= outSize) return false;
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return changed;
}

// Convenience for hooks: returns the path to use (redirected into 'buf', or the original).
#define AKSYS_WORK_PATH(path, buf) (RedirectAksysWorkPaths((path), (buf), sizeof(buf)) ? (const char*)(buf) : (path))

static bool StripStagingCachePrefix(char* path) {
    if (!path || !path[0]) return false;

    while (path[0] == '\\' || path[0] == '/') {
        memmove(path, path + 1, strlen(path));
    }

    char lower[MAX_PATH] = {0};
    strncpy(lower, path, MAX_PATH - 1);
    _strlwr(lower);

    const char* prefixes[] = {
        "uploadcache",
        "upload_cache",
        "upload cache",
        "aksys_transfer_cache",
        "transfer_cache",
        "transfercache"
    };

    bool stripped = false;
    for (int i = 0; i < (int)(sizeof(prefixes) / sizeof(prefixes[0])); i++) {
        size_t pLen = strlen(prefixes[i]);
        if (strncmp(lower, prefixes[i], pLen) == 0) {
            char nextChar = path[pLen];
            if (nextChar == '\\' || nextChar == '/') {
                size_t skip = pLen + 1;
                while (path[skip] == '\\' || path[skip] == '/') skip++;
                memmove(path, path + skip, strlen(path + skip) + 1);
                stripped = true;
                strncpy(lower, path, MAX_PATH - 1);
                _strlwr(lower);
                i = -1;
            } else if (nextChar == '\0') {
                path[0] = '\0';
                return true;
            }
        }
    }
    return stripped;
}

static bool GetRelativePathFromAksysOrName(LPCSTR name, char* outRelPath, DWORD maxLen) {
    if (!name || !outRelPath || maxLen == 0) return false;
    outRelPath[0] = 0;

    char lower[MAX_PATH] = {0};
    strncpy(lower, name, MAX_PATH - 1);
    _strlwr(lower);

    // 1. Detect ak.Sys internal UploadCache staging path
    const char* pUpload = strstr(lower, "uploadcache");
    if (!pUpload) pUpload = strstr(lower, "upload_cache");
    if (!pUpload) pUpload = strstr(lower, "upload cache");
    if (pUpload) {
        const char* pOrig = name + (pUpload - lower);
        const char* pSlash = strchr(pOrig, '\\');
        if (!pSlash) pSlash = strchr(pOrig, '/');
        if (pSlash && pSlash[1]) {
            strncpy(outRelPath, pSlash + 1, maxLen - 1);
            outRelPath[maxLen - 1] = '\0';
            StripStagingCachePrefix(outRelPath);
            return true;
        } else {
            outRelPath[0] = '\0';
            return true;
        }
    }

    // 2. Check for AKSys_Transfer_Cache
    const char* pTrans = strstr(lower, "aksys_transfer_cache");
    if (pTrans) {
        const char* pOrig = name + (pTrans - lower);
        const char* pSlash = strchr(pOrig, '\\');
        if (!pSlash) pSlash = strchr(pOrig, '/');
        if (pSlash && pSlash[1]) {
            strncpy(outRelPath, pSlash + 1, maxLen - 1);
            outRelPath[maxLen - 1] = '\0';
            StripStagingCachePrefix(outRelPath);
            return true;
        } else {
            outRelPath[0] = '\0';
            return true;
        }
    }

    // 3. Check for standard aksys installation root
    const char* pAksys = strstr(lower, "aksys");
    if (pAksys) {
        const char* pOrig = name + (pAksys - lower);
        const char* pSlash = strchr(pOrig, '\\');
        if (!pSlash) pSlash = strchr(pOrig, '/');
        if (pSlash && pSlash[1]) {
            strncpy(outRelPath, pSlash + 1, maxLen - 1);
            outRelPath[maxLen - 1] = '\0';
            StripStagingCachePrefix(outRelPath);
            return true;
        }
    }

    // 4. Relative or bare file name
    if (strstr(lower, ":\\") == NULL && strstr(lower, ":/") == NULL) {
        strncpy(outRelPath, name, maxLen - 1);
        outRelPath[maxLen - 1] = '\0';
        StripStagingCachePrefix(outRelPath);
        return true;
    }

    const char* lastSlash = strrchr(name, '\\');
    if (!lastSlash) lastSlash = strrchr(name, '/');
    if (lastSlash && lastSlash[1]) {
        strncpy(outRelPath, lastSlash + 1, maxLen - 1);
        outRelPath[maxLen - 1] = '\0';
        StripStagingCachePrefix(outRelPath);
        return true;
    }

    strncpy(outRelPath, name, maxLen - 1);
    outRelPath[maxLen - 1] = '\0';
    StripStagingCachePrefix(outRelPath);
    return true;
}

static bool GetExplorerFolderPath(HWND hwndTarget, char* outPath, DWORD maxLen) {
    if (!hwndTarget || !outPath || maxLen == 0) return false;
    outPath[0] = 0;

    HWND hwndDesktop = GetShellWindow();
    HWND hwndProgman = FindWindowA("Progman", NULL);
    HWND hwndWorkerW = FindWindowA("WorkerW", NULL);
    HWND hRoot = GetAncestor(hwndTarget, GA_ROOT);

    if (hwndTarget == hwndDesktop || hRoot == hwndDesktop ||
        hwndTarget == hwndProgman || hRoot == hwndProgman ||
        hwndTarget == hwndWorkerW || hRoot == hwndWorkerW) {
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, outPath))) {
            return (outPath[0] != 0);
        }
    }

    IShellWindows* pShellWindows = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_ALL, IID_IShellWindows, (void**)&pShellWindows)) && pShellWindows) {
        long count = 0;
        pShellWindows->get_Count(&count);
        for (long i = 0; i < count; i++) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = i;
            IDispatch* pDisp = NULL;
            if (SUCCEEDED(pShellWindows->Item(v, &pDisp)) && pDisp) {
                IWebBrowserApp* pApp = NULL;
                if (SUCCEEDED(pDisp->QueryInterface(IID_IWebBrowserApp, (void**)&pApp)) && pApp) {
                    SHANDLE_PTR hExp = 0;
                    pApp->get_HWND(&hExp);
                    if ((HWND)hExp == hRoot || (HWND)hExp == hwndTarget) {
                        BSTR bstrUrl = NULL;
                        pApp->get_LocationURL(&bstrUrl);
                        if (bstrUrl) {
                            char urlStr[MAX_PATH] = {0};
                            WideCharToMultiByte(CP_ACP, 0, bstrUrl, -1, urlStr, MAX_PATH - 1, NULL, NULL);
                            SysFreeString(bstrUrl);
                            DWORD pathLen = maxLen;
                            if (PathCreateFromUrlA(urlStr, outPath, &pathLen, 0) == S_OK) {
                                pApp->Release();
                                pDisp->Release();
                                pShellWindows->Release();
                                return true;
                            }
                        }
                    }
                    pApp->Release();
                }
                pDisp->Release();
            }
        }
        pShellWindows->Release();
    }
    return false;
}

static void DeliverFileToTarget(const char* srcPath, const char* destPath) {
    if (!srcPath || !srcPath[0] || !destPath || !destPath[0]) return;
    CreateParentDirectoryA(destPath);
    CopyFileA(srcPath, destPath, FALSE);
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHA, destPath, NULL);
    if (g_DragTargetFolder[0]) {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHA, g_DragTargetFolder, NULL);
    }
}

static void FlushPendingDropsToTarget() {
    if (!g_DragTargetFolder[0] || g_PendingDropCount <= 0) return;
    for (int i = 0; i < g_PendingDropCount; i++) {
        char finalDest[MAX_PATH] = {0};
        snprintf(finalDest, sizeof(finalDest), "%s\\%s", g_DragTargetFolder, g_PendingDrops[i].relPath);
        DeliverFileToTarget(g_PendingDrops[i].srcPath, finalDest);
    }
    g_PendingDropCount = 0;
}

typedef HRESULT (WINAPI *PFN_DoDragDrop)(LPDATAOBJECT, LPDROPSOURCE, DWORD, LPDWORD);
static PFN_DoDragDrop g_realDoDragDrop = NULL;

static HRESULT WINAPI Hook_DoDragDrop(LPDATAOBJECT pDataObj, LPDROPSOURCE pDropSource, DWORD dwOKEffects, LPDWORD pdwEffect) {
    EnterCriticalSection(&g_StagedLock);
    g_InDragSession = true;
    g_PendingDropCount = 0;
    LeaveCriticalSection(&g_StagedLock);

    HRESULT hr = S_OK;
    if (g_realDoDragDrop) {
        hr = g_realDoDragDrop(pDataObj, pDropSource, dwOKEffects, pdwEffect);
    } else {
        hr = DoDragDrop(pDataObj, pDropSource, dwOKEffects, pdwEffect);
    }

    if (hr == DRAGDROP_S_DROP) {
        POINT pt;
        GetCursorPos(&pt);
        HWND hwndDrop = WindowFromPoint(pt);
        char targetDir[MAX_PATH] = {0};
        if (GetExplorerFolderPath(hwndDrop, targetDir, sizeof(targetDir)) && targetDir[0]) {
            EnterCriticalSection(&g_StagedLock);
            strncpy(g_DragTargetFolder, targetDir, MAX_PATH - 1);
            g_DragTargetFolder[MAX_PATH - 1] = '\0';
            g_DragTargetFolderActive = true;
            g_LastTransferTick = GetTickCount();

            FlushPendingDropsToTarget();
            LeaveCriticalSection(&g_StagedLock);

            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHA, g_DragTargetFolder, NULL);
        }
    }

    EnterCriticalSection(&g_StagedLock);
    g_InDragSession = false;
    LeaveCriticalSection(&g_StagedLock);
    return hr;
}

static BOOL WINAPI Hook_CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    if (!lpPathName || !lpPathName[0]) return FALSE;
    char workBuf[MAX_PATH];
    lpPathName = AKSYS_WORK_PATH(lpPathName, workBuf);

    BOOL ok = CreateDirectoryRecursiveA(lpPathName);

    char relPath[MAX_PATH] = {0};
    if (GetRelativePathFromAksysOrName(lpPathName, relPath, sizeof(relPath)) && relPath[0]) {
        char tempPath[MAX_PATH] = {0};
        if (GetTempPathA(MAX_PATH, tempPath)) {
            char stageSubDir[MAX_PATH] = {0};
            snprintf(stageSubDir, sizeof(stageSubDir), "%sAKSys_Transfer_Cache\\%s", tempPath, relPath);
            CreateDirectoryRecursiveA(stageSubDir);
        }

        EnterCriticalSection(&g_StagedLock);
        if (g_DragTargetFolderActive && g_DragTargetFolder[0]) {
            char destDir[MAX_PATH] = {0};
            snprintf(destDir, sizeof(destDir), "%s\\%s", g_DragTargetFolder, relPath);
            CreateDirectoryRecursiveA(destDir);
            SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHA, destDir, NULL);
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHA, g_DragTargetFolder, NULL);
            g_LastTransferTick = GetTickCount();
        }
        LeaveCriticalSection(&g_StagedLock);
    }

    return ok;
}

static BOOL WINAPI Hook_CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    if (!lpPathName || !lpPathName[0]) return FALSE;
    char ansiPath[MAX_PATH] = {0};
    WideCharToMultiByte(CP_ACP, 0, lpPathName, -1, ansiPath, MAX_PATH - 1, NULL, NULL);
    return Hook_CreateDirectoryA(ansiPath, lpSecurityAttributes);
}

static void RegisterStagedHandle(HANDLE hFile, const char* srcPath, const char* relPath) {
    if (!hFile || hFile == INVALID_HANDLE_VALUE || !srcPath || !relPath) return;
    EnterCriticalSection(&g_StagedLock);
    for (int i = 0; i < MAX_STAGED_FILES; i++) {
        if (!g_StagedFiles[i].active) {
            g_StagedFiles[i].hFile = hFile;
            strncpy(g_StagedFiles[i].srcPath, srcPath, MAX_PATH - 1);
            if (g_DragTargetFolderActive && g_DragTargetFolder[0]) {
                snprintf(g_StagedFiles[i].destPath, MAX_PATH, "%s\\%s", g_DragTargetFolder, relPath);
            } else {
                g_StagedFiles[i].destPath[0] = 0;
            }
            g_StagedFiles[i].active = true;
            g_LastTransferTick = GetTickCount();
            break;
        }
    }
    LeaveCriticalSection(&g_StagedLock);
}

static void InspectAndRepairRegistryKeyRecursive(HKEY hParent, int depth) {
    if (depth > 6) return;
    char valName[256];
    BYTE data[512];
    DWORD idx = 0;

    while (true) {
        DWORD valNameLen = sizeof(valName) - 1;
        DWORD dataLen = sizeof(data) - 1;
        DWORD type = 0;
        LONG res = RegEnumValueA(hParent, idx, valName, &valNameLen, NULL, &type, data, &dataLen);
        if (res != ERROR_SUCCESS) break;
        valName[valNameLen] = '\0';

        if (type == REG_DWORD && dataLen == sizeof(DWORD)) {
            DWORD val = *(DWORD*)data;
            char lowerName[256] = {0};
            strncpy(lowerName, valName, sizeof(lowerName) - 1);
            _strlwr(lowerName);
            if ((strstr(lowerName, "show") || strstr(lowerName, "view") || strstr(lowerName, "panel") ||
                 strstr(lowerName, "pane") || strstr(lowerName, "rack") || strstr(lowerName, "kg") ||
                 strstr(lowerName, "env") || strstr(lowerName, "lfo") || strstr(lowerName, "zone") ||
                 strstr(lowerName, "key") || strstr(lowerName, "filt") || strstr(lowerName, "theme") ||
                 strstr(lowerName, "mod") || strstr(lowerName, "qlink") || strstr(lowerName, "fx")) && val == 0) {
                DWORD newVal = 1;
                RegSetValueExA(hParent, valName, 0, REG_DWORD, (const BYTE*)&newVal, sizeof(DWORD));
            }
        }
        idx++;
    }

    char subKeyName[256];
    DWORD subIdx = 0;
    while (true) {
        DWORD subLen = sizeof(subKeyName) - 1;
        LONG res = RegEnumKeyExA(hParent, subIdx, subKeyName, &subLen, NULL, NULL, NULL, NULL);
        if (res != ERROR_SUCCESS) break;
        subKeyName[subLen] = '\0';

        HKEY hSub = NULL;
        if (RegOpenKeyExA(hParent, subKeyName, 0, KEY_READ | KEY_WRITE, &hSub) == ERROR_SUCCESS) {
            InspectAndRepairRegistryKeyRecursive(hSub, depth + 1);
            RegCloseKey(hSub);
        }
        subIdx++;
    }
}

static void RepairAkaiProgramEditorRegistry(bool forceResetLayouts) {
    HKEY hAkSys = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\AKSys\\ak.Sys", 0, KEY_READ | KEY_WRITE, &hAkSys) == ERROR_SUCCESS) {
        InspectAndRepairRegistryKeyRecursive(hAkSys, 0);

        if (forceResetLayouts) {
            const char* subKeysToClear[] = {
                "S56\\Program Editor", "S56\\Themes", "S56\\Windows", "S56",
                "Z48\\Program Editor", "Z48\\Themes", "Z48\\Windows", "Z48",
                "MPC4000\\Program Editor", "MPC4000\\Themes", "MPC4000\\Windows", "MPC4000",
                "AKAI S6000\\Program Editor", "AKAI S6000",
                "AKAI S5000\\Program Editor", "AKAI S5000",
                "AKAI Z4\\Program Editor", "AKAI Z4",
                "AKAI Z8\\Program Editor", "AKAI Z8",
                "AKAI MPC4000\\Program Editor", "AKAI MPC4000",
                "Demo S56K 1", "Demo S56K 2"
            };
            for (int i = 0; i < (int)(sizeof(subKeysToClear) / sizeof(subKeysToClear[0])); i++) {
                RegDeleteKeyA(hAkSys, subKeysToClear[i]);
            }
        }
        RegCloseKey(hAkSys);
    }
}

// Shim's own settings live outside HKCU\Software\AKSys\ak.Sys so the layout repair never touches them.
#define SHIM_SETTINGS_KEY "Software\\AKSysShim"

static DWORD ReadShimSettingDword(const char* name, DWORD defVal) {
    HKEY hKey = NULL;
    DWORD val = defVal;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, SHIM_SETTINGS_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0, data = 0, sz = sizeof(DWORD);
        if (RegQueryValueExA(hKey, name, NULL, &type, (LPBYTE)&data, &sz) == ERROR_SUCCESS &&
            type == REG_DWORD && sz == sizeof(DWORD)) {
            val = data;
        }
        RegCloseKey(hKey);
    }
    return val;
}

static void WriteShimSettingDword(const char* name, DWORD val) {
    HKEY hKey = NULL;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, SHIM_SETTINGS_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

// The layout repair forces hidden-panel flags back to 1, which wipes out the user's saved
// editor layout. Only run it when explicitly requested, and only once.
static void RepairAkaiProgramEditorRegistryIfRequested() {
    if (ReadShimSettingDword("RepairEditorLayoutOnNextLaunch", 0) == 0) return;
    RepairAkaiProgramEditorRegistry(false);
    WriteShimSettingDword("RepairEditorLayoutOnNextLaunch", 0);
}

static void EnableDpiAwareness() {
    HMODULE hUser = GetModuleHandleA("user32.dll");
    if (hUser) {
        typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(VOID);
        PFN_SetProcessDPIAware pfnDPIAware = (PFN_SetProcessDPIAware)GetProcAddress(hUser, "SetProcessDPIAware");
        if (pfnDPIAware) pfnDPIAware();
    }
    HMODULE hShcore = LoadLibraryA("Shcore.dll");
    if (hShcore) {
        typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
        PFN_SetProcessDpiAwareness pfnAwareness = (PFN_SetProcessDpiAwareness)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (pfnAwareness) pfnAwareness(2);
    }
}

#define MAX_AKAI_UNITS 16
#define MAX_AKAI_HANDLES 128
#define AKAI_HANDLE_BASE 0x414B5300
#define AKAI_READ_BUFFER_SIZE 262144

enum AkaiDeviceFamily {
    FAMILY_ANY = 0,
    FAMILY_S56,
    FAMILY_Z48,
    FAMILY_MPC4000
};

struct AkaiUsbUnit {
    bool active;
    HANDLE hUsbDevice;
    WINUSB_INTERFACE_HANDLE hWinUsb;
    char winUsbDevicePath[MAX_PATH];
    char instanceId[MAX_PATH];
    char fakeAkaiPath[MAX_PATH];
    USHORT vid;
    USHORT pid;
    AkaiDeviceFamily family;
    UCHAR bulkInPipe;
    UCHAR bulkOutPipe;
    UCHAR intInPipe;
};

enum AkaiHandleType {
    HANDLE_TYPE_UNUSED = 0,
    HANDLE_TYPE_ROOT,
    HANDLE_TYPE_PIPE0,
    HANDLE_TYPE_PIPE1,
    HANDLE_TYPE_PIPE2
};

struct AkaiHandleContext {
    AkaiHandleType type;
    int unitIndex;
    UCHAR pipeId;
    char path[256];
    BYTE readBuffer[AKAI_READ_BUFFER_SIZE];
    DWORD readLen;
    OVERLAPPED winusbOv;
    HANDLE hEvent;
    volatile bool readPending;
    LPVOID pUserDest;
    DWORD userReqLen;
    LPOVERLAPPED pUserOv;
    DWORD lastBytesTransferred;
    BOOL lastResultOk;
};

static CRITICAL_SECTION g_UsbLock;
static AkaiUsbUnit g_Units[MAX_AKAI_UNITS];
static int g_UnitCount = 0;
static DWORD g_LastBytesTransferred = 0;
static volatile bool g_DeviceDllHooked = false;
static AkaiHandleContext g_Handles[MAX_AKAI_HANDLES];
static LONG g_HandleCounter = 0;
static GUID g_LastQueriedGuid = {0};

static bool OpenAkaiWinUsbDevices();

static bool IsAkaiHandle(HANDLE h) {
    ULONG_PTR val = (ULONG_PTR)h;
    return (val >= AKAI_HANDLE_BASE && val < (AKAI_HANDLE_BASE + MAX_AKAI_HANDLES));
}

static int GetAkaiHandleIndex(HANDLE h) {
    if (!IsAkaiHandle(h)) return -1;
    return (int)((ULONG_PTR)h - AKAI_HANDLE_BASE);
}

static int SelectBestUnitForPath(const char* name) {
    if (g_UnitCount <= 0) return 0;
    if (!name) return 0;

    char lower[256] = {0};
    strncpy(lower, name, sizeof(lower) - 1);
    _strlwr(lower);

    const char* pUnit = strstr(lower, "#unit");
    if (pUnit) {
        int idx = atoi(pUnit + 5);
        if (idx >= 0 && idx < g_UnitCount && g_Units[idx].active) {
            return idx;
        }
    }

    bool wantsS56 = (strstr(lower, "s56") != NULL || strstr(lower, "005e") != NULL);
    bool wantsZ48 = (strstr(lower, "z48") != NULL || strstr(lower, "mpc") != NULL ||
                     strstr(lower, "z4") != NULL  || strstr(lower, "z8") != NULL);

    for (int i = 0; i < g_UnitCount; i++) {
        if (!g_Units[i].active) continue;
        if (wantsS56 && g_Units[i].family == FAMILY_S56) return i;
        if (wantsZ48 && (g_Units[i].family == FAMILY_Z48 || g_Units[i].family == FAMILY_MPC4000)) return i;
    }

    return 0;
}

static HANDLE AllocateAkaiHandle(AkaiHandleType type, int unitIndex, const char* path) {
    EnterCriticalSection(&g_UsbLock);
    if (unitIndex < 0 || unitIndex >= MAX_AKAI_UNITS) unitIndex = 0;

    for (int step = 0; step < MAX_AKAI_HANDLES; step++) {
        int i = (InterlockedIncrement(&g_HandleCounter) & 0x7FFFFFFF) % MAX_AKAI_HANDLES;
        if (g_Handles[i].type == HANDLE_TYPE_UNUSED) {
            g_Handles[i].type = type;
            g_Handles[i].unitIndex = unitIndex;

            UCHAR inPipe  = g_Units[unitIndex].active ? g_Units[unitIndex].bulkInPipe  : 0x82;
            UCHAR outPipe = g_Units[unitIndex].active ? g_Units[unitIndex].bulkOutPipe : 0x02;

            if (type == HANDLE_TYPE_PIPE0) g_Handles[i].pipeId = inPipe;
            else if (type == HANDLE_TYPE_PIPE1) g_Handles[i].pipeId = outPipe;
            else g_Handles[i].pipeId = 0;

            if (path) strncpy(g_Handles[i].path, path, sizeof(g_Handles[i].path) - 1);
            else g_Handles[i].path[0] = 0;
            g_Handles[i].readLen = 0;
            g_Handles[i].readPending = false;
            g_Handles[i].pUserDest = NULL;
            g_Handles[i].userReqLen = 0;
            g_Handles[i].pUserOv = NULL;
            g_Handles[i].lastBytesTransferred = 0;
            g_Handles[i].lastResultOk = TRUE;
            if (!g_Handles[i].hEvent) {
                g_Handles[i].hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
                memset(&g_Handles[i].winusbOv, 0, sizeof(OVERLAPPED));
                g_Handles[i].winusbOv.hEvent = g_Handles[i].hEvent;
            } else {
                ResetEvent(g_Handles[i].hEvent);
            }
            HANDLE hResult = (HANDLE)(ULONG_PTR)(AKAI_HANDLE_BASE + i);
            LeaveCriticalSection(&g_UsbLock);
            return hResult;
        }
    }
    LeaveCriticalSection(&g_UsbLock);
    return INVALID_HANDLE_VALUE;
}

static void CancelPendingRead(AkaiHandleContext* ctx, WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR inPipe);

static void FreeAkaiHandle(HANDLE h) {
    int idx = GetAkaiHandleIndex(h);
    if (idx < 0) return;
    EnterCriticalSection(&g_UsbLock);
    int uIdx = g_Handles[idx].unitIndex;
    if (uIdx >= 0 && uIdx < MAX_AKAI_UNITS) {
        CancelPendingRead(&g_Handles[idx], g_Units[uIdx].hWinUsb, g_Units[uIdx].bulkInPipe);
    }
    g_Handles[idx].type = HANDLE_TYPE_UNUSED;
    g_Handles[idx].readPending = false;
    g_Handles[idx].readLen = 0;
    g_Handles[idx].pUserDest = NULL;
    g_Handles[idx].pUserOv = NULL;
    LeaveCriticalSection(&g_UsbLock);
}

static BOOL WINAPI Hook_CloseHandle(HANDLE hObject) {
    if (IsAkaiHandle(hObject)) {
        FreeAkaiHandle(hObject);
        return TRUE;
    }

    char copySrc[MAX_PATH] = {0};
    char copyDest[MAX_PATH] = {0};
    bool shouldDeliver = false;

    EnterCriticalSection(&g_StagedLock);
    for (int i = 0; i < MAX_STAGED_FILES; i++) {
        if (g_StagedFiles[i].active && g_StagedFiles[i].hFile == hObject) {
            strncpy(copySrc, g_StagedFiles[i].srcPath, MAX_PATH - 1);
            if (g_StagedFiles[i].destPath[0]) {
                strncpy(copyDest, g_StagedFiles[i].destPath, MAX_PATH - 1);
                shouldDeliver = true;
            } else if (g_DragTargetFolderActive && g_DragTargetFolder[0]) {
                char relPath[MAX_PATH] = {0};
                GetRelativePathFromAksysOrName(copySrc, relPath, sizeof(relPath));
                snprintf(copyDest, sizeof(copyDest), "%s\\%s", g_DragTargetFolder, relPath);
                shouldDeliver = true;
            } else if (g_InDragSession) {
                if (g_PendingDropCount < MAX_PENDING_DROPS) {
                    strncpy(g_PendingDrops[g_PendingDropCount].srcPath, copySrc, MAX_PATH - 1);
                    GetRelativePathFromAksysOrName(copySrc, g_PendingDrops[g_PendingDropCount].relPath, MAX_PATH);
                    g_PendingDropCount++;
                }
            }
            g_StagedFiles[i].active = false;
            g_StagedFiles[i].hFile = NULL;
            break;
        }
    }
    LeaveCriticalSection(&g_StagedLock);

    BOOL ok = CloseHandle(hObject);

    if (shouldDeliver && copySrc[0] && copyDest[0]) {
        DeliverFileToTarget(copySrc, copyDest);
    }

    return ok;
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sec,
                                     DWORD disp, DWORD flags, HANDLE templ) {
    char workBuf[MAX_PATH];
    name = AKSYS_WORK_PATH(name, workBuf);
    if (name) {
        char lower[MAX_PATH] = {0};
        strncpy(lower, name, sizeof(lower) - 1);
        _strlwr(lower);

        if (strstr(lower, "aks56") || strstr(lower, "akz48") || strstr(lower, "akmpc") ||
            strstr(lower, "vid_09e8") || strstr(lower, "9d4b6529")) {
            OpenAkaiWinUsbDevices();
            int unitIdx = SelectBestUnitForPath(lower);

            if (strstr(lower, "pipe00") || strstr(lower, "pipe_00") || strstr(lower, "pipe0")) {
                return AllocateAkaiHandle(HANDLE_TYPE_PIPE0, unitIdx, name);
            }
            if (strstr(lower, "pipe01") || strstr(lower, "pipe_01") || strstr(lower, "pipe1")) {
                return AllocateAkaiHandle(HANDLE_TYPE_PIPE1, unitIdx, name);
            }
            if (strstr(lower, "pipe02") || strstr(lower, "pipe_02") || strstr(lower, "pipe2")) {
                return AllocateAkaiHandle(HANDLE_TYPE_PIPE2, unitIdx, name);
            }
            return AllocateAkaiHandle(HANDLE_TYPE_ROOT, unitIdx, name);
        }

        if ((acc & GENERIC_WRITE) || disp == CREATE_ALWAYS || disp == CREATE_NEW || disp == OPEN_ALWAYS) {
            share |= (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);

            char relPath[MAX_PATH] = {0};
            GetRelativePathFromAksysOrName(name, relPath, sizeof(relPath));

            CreateParentDirectoryA(name);
            HANDLE hReal = CreateFileA(name, acc, share, sec, disp, flags, templ);
            DIAG_FILE(name, "CreateFileA write '%s' acc=0x%lX disp=%lu -> %s (err %lu)", name, acc, disp,
                      hReal != INVALID_HANDLE_VALUE ? "ok" : "FAILED", hReal != INVALID_HANDLE_VALUE ? 0 : GetLastError());
            if (hReal != INVALID_HANDLE_VALUE) {
                RegisterStagedHandle(hReal, name, relPath);
                return hReal;
            }

            char tempPath[MAX_PATH] = {0};
            if (GetTempPathA(MAX_PATH, tempPath)) {
                char redirectedPath[MAX_PATH] = {0};
                snprintf(redirectedPath, sizeof(redirectedPath), "%sAKSys_Transfer_Cache\\%s", tempPath, relPath);
                CreateParentDirectoryA(redirectedPath);
                HANDLE hRedirected = CreateFileA(redirectedPath, acc, share, sec, disp, flags, templ);
                DIAG_FILE(name, "  -> REDIRECTED to '%s' %s", redirectedPath, hRedirected != INVALID_HANDLE_VALUE ? "ok" : "FAILED");
                if (hRedirected != INVALID_HANDLE_VALUE) {
                    RegisterStagedHandle(hRedirected, redirectedPath, relPath);
                    return hRedirected;
                }
            }
        }
    }
#ifdef AKSHIM_DIAG
    {
        HANDLE h = CreateFileA(name, acc, share, sec, disp, flags, templ);
        DIAG_FILE(name, "CreateFileA read '%s' acc=0x%lX -> %s (err %lu)", name, acc,
                  h != INVALID_HANDLE_VALUE ? "ok" : "FAILED", h != INVALID_HANDLE_VALUE ? 0 : GetLastError());
        return h;
    }
#else
    return CreateFileA(name, acc, share, sec, disp, flags, templ);
#endif
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sec,
                                     DWORD disp, DWORD flags, HANDLE templ) {
    if (name && ((acc & GENERIC_WRITE) || disp == CREATE_ALWAYS || disp == CREATE_NEW || disp == OPEN_ALWAYS)) {
        char ansiName[MAX_PATH] = {0};
        WideCharToMultiByte(CP_ACP, 0, name, -1, ansiName, MAX_PATH - 1, NULL, NULL);
        return Hook_CreateFileA(ansiName, acc, share, sec, disp, flags, templ);
    }
    return CreateFileW(name, acc, share, sec, disp, flags, templ);
}

static bool IsUnitAlreadyOpen(const char* devPath) {
    for (int i = 0; i < g_UnitCount; i++) {
        if (g_Units[i].active && _stricmp(g_Units[i].winUsbDevicePath, devPath) == 0) {
            return true;
        }
    }
    return false;
}

// One physical sampler can be reachable through several interface GUID paths
// (USB device / WinUSB / Akai / Zadig custom); match on the device instance instead.
static bool IsInstanceAlreadyOpen(const char* instanceId) {
    for (int i = 0; i < g_UnitCount; i++) {
        if (g_Units[i].active && _stricmp(g_Units[i].instanceId, instanceId) == 0) {
            return true;
        }
    }
    return false;
}

static bool ConfigureWinUsbUnit(HANDLE hDev, const char* devPath, const char* hwIdLower, const char* instanceId) {
    if (g_UnitCount >= MAX_AKAI_UNITS) return false;

    WINUSB_INTERFACE_HANDLE hWinUsb = NULL;
    if (!WinUsb_Initialize(hDev, &hWinUsb)) {
        return false;
    }

    UCHAR foundBulkIn = 0;
    UCHAR foundBulkOut = 0;
    UCHAR foundIntIn = 0;

    USB_INTERFACE_DESCRIPTOR ifaceDesc;
    if (WinUsb_QueryInterfaceSettings(hWinUsb, 0, &ifaceDesc)) {
        for (UCHAR p = 0; p < ifaceDesc.bNumEndpoints; p++) {
            WINUSB_PIPE_INFORMATION pipeInfo;
            if (WinUsb_QueryPipe(hWinUsb, 0, p, &pipeInfo)) {
                ULONG timeout = 15000;
                UCHAR autoClear = 1;
                UCHAR rawIo = 0;
                UCHAR shortPacket = 0;
                WinUsb_SetPipePolicy(hWinUsb, pipeInfo.PipeId, PIPE_TRANSFER_TIMEOUT, sizeof(ULONG), &timeout);
                WinUsb_SetPipePolicy(hWinUsb, pipeInfo.PipeId, AUTO_CLEAR_STALL, sizeof(UCHAR), &autoClear);
                WinUsb_SetPipePolicy(hWinUsb, pipeInfo.PipeId, RAW_IO, sizeof(UCHAR), &rawIo);
                if (!USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId)) {
                    WinUsb_SetPipePolicy(hWinUsb, pipeInfo.PipeId, SHORT_PACKET_TERMINATE, sizeof(UCHAR), &shortPacket);
                }

                if (pipeInfo.PipeType == UsbdPipeTypeBulk) {
                    if (USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId)) {
                        if (!foundBulkIn) foundBulkIn = pipeInfo.PipeId;
                    } else {
                        if (!foundBulkOut) foundBulkOut = pipeInfo.PipeId;
                    }
                } else if (pipeInfo.PipeType == UsbdPipeTypeInterrupt && USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId)) {
                    if (!foundIntIn) foundIntIn = pipeInfo.PipeId;
                }
            }
        }
    }

    if (!foundBulkIn)  foundBulkIn  = 0x82;
    if (!foundBulkOut) foundBulkOut = 0x02;

    WinUsb_FlushPipe(hWinUsb, foundBulkIn);

    int idx = g_UnitCount++;
    AkaiUsbUnit* u = &g_Units[idx];
    memset(u, 0, sizeof(AkaiUsbUnit));
    u->active = true;
    u->hUsbDevice = hDev;
    u->hWinUsb = hWinUsb;
    strncpy(u->winUsbDevicePath, devPath, MAX_PATH - 1);
    if (instanceId) strncpy(u->instanceId, instanceId, MAX_PATH - 1);
    u->bulkInPipe = foundBulkIn;
    u->bulkOutPipe = foundBulkOut;
    u->intInPipe = foundIntIn;
    u->vid = 0x09E8;
    u->pid = 0x005E;
    u->family = FAMILY_ANY;

    if (hwIdLower) {
        const char* pPid = strstr(hwIdLower, "pid_");
        if (pPid) {
            u->pid = (USHORT)strtoul(pPid + 4, NULL, 16);
        }
        if (u->pid == 0x005E) {
            u->family = FAMILY_S56;
        } else if (u->pid == 0x006C) {
            u->family = FAMILY_MPC4000;
        } else {
            u->family = FAMILY_Z48;
        }
    }

    snprintf(u->fakeAkaiPath, sizeof(u->fakeAkaiPath),
             "\\\\?\\usb#vid_09e8&pid_%04x#unit%d_aks56usb_akz48usb_mpc4000#{9d4b6529-d4a6-11d3-8eae-0050da2e2d53}",
             u->pid, idx);

    return true;
}

static bool TryDirectPathOpen(const char* candidatePath, const char* hwIdLower, const char* instanceId) {
    if (IsUnitAlreadyOpen(candidatePath)) return true;

    HANDLE hDev = CreateFileA(candidatePath, GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_WRITE | FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (hDev != INVALID_HANDLE_VALUE) {
        if (ConfigureWinUsbUnit(hDev, candidatePath, hwIdLower, instanceId)) {
            return true;
        }
        CloseHandle(hDev);
    }
    return false;
}

static bool ScanAllUsbDevicesUniversal() {
    HDEVINFO hDevInfo = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (hDevInfo == INVALID_HANDLE_VALUE) return (g_UnitCount > 0);

    SP_DEVINFO_DATA devData;
    devData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devData); i++) {
        char hwId[512] = {0};
        if (SetupDiGetDeviceRegistryPropertyA(hDevInfo, &devData, SPDRP_HARDWAREID, NULL, (PBYTE)hwId, sizeof(hwId) - 1, NULL)) {
            _strlwr(hwId);
            if (strstr(hwId, "vid_09e8") != NULL) {
                char instanceId[MAX_PATH] = {0};
                if (SetupDiGetDeviceInstanceIdA(hDevInfo, &devData, instanceId, sizeof(instanceId) - 1, NULL)) {
                    if (IsInstanceAlreadyOpen(instanceId)) continue;

                    char symInstance[MAX_PATH] = {0};
                    strncpy(symInstance, instanceId, sizeof(symInstance) - 1);
                    for (int c = 0; symInstance[c]; c++) {
                        if (symInstance[c] == '\\') symInstance[c] = '#';
                    }

                    char customGuidStr[128] = {0};
                    HKEY hDevReg = SetupDiOpenDevRegKey(hDevInfo, &devData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
                    if (hDevReg != INVALID_HANDLE_VALUE) {
                        DWORD gType = 0, gSz = sizeof(customGuidStr) - 1;
                        if (RegQueryValueExA(hDevReg, "DeviceInterfaceGUIDs", NULL, &gType, (LPBYTE)customGuidStr, &gSz) != ERROR_SUCCESS) {
                            gSz = sizeof(customGuidStr) - 1;
                            RegQueryValueExA(hDevReg, "DeviceInterfaceGUID", NULL, &gType, (LPBYTE)customGuidStr, &gSz);
                        }
                        RegCloseKey(hDevReg);
                    }

                    const char* guidStrings[] = {
                        "{a5dcbf10-6530-11d2-901f-00c04fb951ed}",
                        "{88bae032-5a81-49f0-bc3d-a4ff138216d6}",
                        "{9d4b6529-d4a6-11d3-8eae-0050da2e2d53}",
                        customGuidStr[0] ? customGuidStr : NULL
                    };

                    for (int g = 0; g < 4; g++) {
                        if (!guidStrings[g] || !guidStrings[g][0]) continue;
                        char candidatePath[MAX_PATH];
                        snprintf(candidatePath, sizeof(candidatePath), "\\\\?\\%s#%s", symInstance, guidStrings[g]);
                        if (TryDirectPathOpen(candidatePath, hwId, instanceId)) {
                            break;
                        }
                    }
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return (g_UnitCount > 0);
}

static bool OpenAkaiWinUsbDevices() {
    EnterCriticalSection(&g_UsbLock);
    ScanAllUsbDevicesUniversal();
    bool hasAny = (g_UnitCount > 0);
    LeaveCriticalSection(&g_UsbLock);
    return hasAny;
}

static BOOL CompleteIoOperation(LPOVERLAPPED lpOverlapped, DWORD bytesTransferred, BOOL ok) {
    g_LastBytesTransferred = bytesTransferred;
    if (lpOverlapped) {
        lpOverlapped->Internal = ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        lpOverlapped->InternalHigh = bytesTransferred;
        if (lpOverlapped->hEvent) {
            SetEvent(lpOverlapped->hEvent);
        }
    }
    SetLastError(ok ? ERROR_SUCCESS : ERROR_GEN_FAILURE);
    return ok;
}

// Each fake HDEVINFO carries the device family of the plugin that asked for it, so
// S56Device.dll only sees S5000/S6000 units and Z48Device.dll only sees Z4/Z8/MPC4000
// units. Without this every plugin claimed every sampler and ak.Sys listed it twice.
#define FAKE_HDEVINFO_BASE 0x414B0010
#define MAKE_FAKE_HDEVINFO(fam) ((HDEVINFO)(ULONG_PTR)(FAKE_HDEVINFO_BASE + (fam)))

static bool IsFakeDevInfo(HDEVINFO s) {
    ULONG_PTR v = (ULONG_PTR)s;
    return (v >= FAKE_HDEVINFO_BASE && v <= FAKE_HDEVINFO_BASE + FAMILY_MPC4000);
}

static AkaiDeviceFamily FakeDevInfoFamily(HDEVINFO s) {
    return (AkaiDeviceFamily)((ULONG_PTR)s - FAKE_HDEVINFO_BASE);
}

static AkaiDeviceFamily GetCallerDeviceFamily(void* retAddr) {
    HMODULE hCaller = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)retAddr, &hCaller) && hCaller) {
        char modPath[MAX_PATH] = {0};
        if (GetModuleFileNameA(hCaller, modPath, MAX_PATH - 1)) {
            _strlwr(modPath);
            // Only look at the file name: the install folder ("aksys\devices") matches everything.
            const char* base = strrchr(modPath, '\\');
            base = base ? base + 1 : modPath;
            if (strstr(base, "s56")) return FAMILY_S56;
            if (strstr(base, "z48")) return FAMILY_Z48;
            if (strstr(base, "mpc")) return FAMILY_MPC4000;
        }
    }
    return FAMILY_ANY;
}

static bool HasDedicatedMpcPlugin() {
    return GetModuleHandleA("MPC4000Device.dll") != NULL || GetModuleHandleA("MPC4KDevice.dll") != NULL;
}

static bool UnitMatchesFamily(const AkaiUsbUnit* u, AkaiDeviceFamily want) {
    if (want == FAMILY_ANY || u->family == FAMILY_ANY) return true;
    if (want == FAMILY_Z48) {
        return u->family == FAMILY_Z48 || (u->family == FAMILY_MPC4000 && !HasDedicatedMpcPlugin());
    }
    return u->family == want;
}

typedef HDEVINFO (WINAPI *PFN_SetupDiGetClassDevsA)(const GUID*, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI *PFN_SetupDiEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI *PFN_SetupDiGetDeviceInterfaceDetailA)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);

static PFN_SetupDiGetClassDevsA g_realSetupDiGetClassDevsA = NULL;
static PFN_SetupDiEnumDeviceInterfaces g_realSetupDiEnumDeviceInterfaces = NULL;
static PFN_SetupDiGetDeviceInterfaceDetailA g_realSetupDiGetDeviceInterfaceDetailA = NULL;

static bool InstallTrampolineHook(void* targetFunc, void* hookFunc, void** origTrampoline) {
    if (!targetFunc || !hookFunc) return false;
    BYTE* pTarget = (BYTE*)targetFunc;

    // Accept the hot-patch prologue (mov edi,edi; push ebp; mov ebp,esp) or MSVC6's SEH-frame
    // prologue (mov eax,imm32). Both are exactly 5 bytes and position-independent.
    bool hotPatch = (pTarget[0] == 0x8B && pTarget[1] == 0xFF && pTarget[2] == 0x55 && pTarget[3] == 0x8B && pTarget[4] == 0xEC);
    bool movEaxImm = (pTarget[0] == 0xB8);
    if (!hotPatch && !movEaxImm) {
        return false;
    }

    BYTE* pTrampoline = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pTrampoline) return false;

    memcpy(pTrampoline, pTarget, 5);
    pTrampoline[5] = 0xE9;
    *(DWORD*)(pTrampoline + 6) = (DWORD)((pTarget + 5) - (pTrampoline + 10));

    DWORD oldProt;
    if (!VirtualProtect(pTarget, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
        VirtualFree(pTrampoline, 0, MEM_RELEASE);
        return false;
    }

    pTarget[0] = 0xE9;
    *(DWORD*)(pTarget + 1) = (DWORD)((BYTE*)hookFunc - (pTarget + 5));
    VirtualProtect(pTarget, 5, oldProt, &oldProt);

    if (origTrampoline) *origTrampoline = pTrampoline;
    return true;
}

static bool IsAkaiDeviceCaller(void* retAddr) {
    HMODULE hCaller = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)retAddr, &hCaller) && hCaller) {
        char modPath[MAX_PATH] = {0};
        if (GetModuleFileNameA(hCaller, modPath, MAX_PATH - 1)) {
            _strlwr(modPath);
            if (strstr(modPath, "device") || strstr(modPath, "s56") ||
                strstr(modPath, "z48")    || strstr(modPath, "mpc") ||
                strstr(modPath, "dps")    || strstr(modPath, "aksys")) {
                return true;
            }
        }
    }
    return false;
}

static HDEVINFO WINAPI Hook_SetupDiGetClassDevsA(const GUID* g, PCSTR e, HWND h, DWORD f) {
    if (g && (g->Data1 == 0x9D4B6529L ||
              memcmp(g, &GUID_DEVINTERFACE_AKAI_S56, sizeof(GUID)) == 0 ||
              ((f & DIGCF_DEVICEINTERFACE) && IsAkaiDeviceCaller(_ReturnAddress())))) {
        g_LastQueriedGuid = *g;
        OpenAkaiWinUsbDevices();
        return MAKE_FAKE_HDEVINFO(GetCallerDeviceFamily(_ReturnAddress()));
    }
    if (g_realSetupDiGetClassDevsA) return g_realSetupDiGetClassDevsA(g, e, h, f);
    return SetupDiGetClassDevsA(g, e, h, f);
}

static BOOL WINAPI Hook_SetupDiEnumDeviceInterfaces(HDEVINFO s, PSP_DEVINFO_DATA d, const GUID* i, DWORD m, PSP_DEVICE_INTERFACE_DATA id) {
    if (IsFakeDevInfo(s)) {
        OpenAkaiWinUsbDevices();
        AkaiDeviceFamily want = FakeDevInfoFamily(s);
        int unitIdx = -1;
        if (g_UnitCount == 0) {
            if (m == 0) unitIdx = 0;
        } else {
            DWORD matched = 0;
            for (int u = 0; u < g_UnitCount; u++) {
                if (!g_Units[u].active || !UnitMatchesFamily(&g_Units[u], want)) continue;
                if (matched == m) { unitIdx = u; break; }
                matched++;
            }
        }
        if (unitIdx >= 0) {
            if (id) {
                id->InterfaceClassGuid = i ? *i : g_LastQueriedGuid;
                id->Flags = SPINT_ACTIVE;
                id->Reserved = (ULONG_PTR)(0x414B0000 + unitIdx);
            }
            return TRUE;
        }
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    if (g_realSetupDiEnumDeviceInterfaces) return g_realSetupDiEnumDeviceInterfaces(s, d, i, m, id);
    return SetupDiEnumDeviceInterfaces(s, d, i, m, id);
}

static BOOL WINAPI Hook_SetupDiGetDeviceInterfaceDetailA(HDEVINFO s, PSP_DEVICE_INTERFACE_DATA id,
                                                        PSP_DEVICE_INTERFACE_DETAIL_DATA_A det,
                                                        DWORD sz, PDWORD req, PSP_DEVINFO_DATA d) {
    if (IsFakeDevInfo(s)) {
        int unitIdx = 0;
        if (id && (id->Reserved & 0xFFFF0000) == 0x414B0000) {
            unitIdx = (int)(id->Reserved & 0xFFFF);
        }
        const char* pathStr = "\\\\?\\usb#vid_09e8&pid_005e#unit0_aks56usb_akz48usb_mpc4000#{9d4b6529-d4a6-11d3-8eae-0050da2e2d53}";
        if (unitIdx >= 0 && unitIdx < g_UnitCount && g_Units[unitIdx].active && g_Units[unitIdx].fakeAkaiPath[0]) {
            pathStr = g_Units[unitIdx].fakeAkaiPath;
        }

        DWORD needed = (DWORD)(FIELD_OFFSET(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + strlen(pathStr) + 1);
        if (req) *req = needed;
        if (!det || sz < needed) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        strcpy(det->DevicePath, pathStr);
        return TRUE;
    }
    if (g_realSetupDiGetDeviceInterfaceDetailA) return g_realSetupDiGetDeviceInterfaceDetailA(s, id, det, sz, req, d);
    return SetupDiGetDeviceInterfaceDetailA(s, id, det, sz, req, d);
}

static BOOL WINAPI Hook_SetupDiDestroyDeviceInfoList(HDEVINFO s) {
    if (IsFakeDevInfo(s)) return TRUE;
    return SetupDiDestroyDeviceInfoList(s);
}

// The external sample editor is launched with the working-folder path on its command line
// (file = editor exe, params = "\"...\\UploadCache\\x.wav\"") or as the document itself.
static HINSTANCE WINAPI Hook_ShellExecuteA(HWND hwnd, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, INT show) {
    char fileBuf[MAX_PATH], paramBuf[1024], dirBuf[MAX_PATH];
    file = AKSYS_WORK_PATH(file, fileBuf);
    params = AKSYS_WORK_PATH(params, paramBuf);
    dir = AKSYS_WORK_PATH(dir, dirBuf);
    HINSTANCE r = ShellExecuteA(hwnd, op, file, params, dir, show);
    DIAG("ShellExecuteA op='%s' file='%s' params='%s' dir='%s' -> %d", op ? op : "(null)", file ? file : "(null)",
         params ? params : "(null)", dir ? dir : "(null)", (int)(INT_PTR)r);
    return r;
}

static HANDLE WINAPI Hook_FindFirstChangeNotificationA(LPCSTR path, BOOL subtree, DWORD filter) {
    char workBuf[MAX_PATH];
    path = AKSYS_WORK_PATH(path, workBuf);
    if (path == workBuf) CreateDirectoryRecursiveA(path);  // the watched folder must exist in its new home
    HANDLE h = FindFirstChangeNotificationA(path, subtree, filter);
    DIAG("FindFirstChangeNotificationA '%s' subtree=%d filter=0x%lX -> %s (err %lu)", path ? path : "(null)", subtree, filter,
         h != INVALID_HANDLE_VALUE ? "ok" : "FAILED", h != INVALID_HANDLE_VALUE ? 0 : GetLastError());
    return h;
}

static BOOL WINAPI Hook_DeleteFileA(LPCSTR path) {
    char workBuf[MAX_PATH];
    path = AKSYS_WORK_PATH(path, workBuf);
    BOOL ok = DeleteFileA(path);
    DIAG_FILE(path, "DeleteFileA '%s' -> %d", path, ok);
    return ok;
}

static DWORD WINAPI Hook_GetFileAttributesA(LPCSTR path) {
    char workBuf[MAX_PATH];
    path = AKSYS_WORK_PATH(path, workBuf);
    DWORD a = GetFileAttributesA(path);
    DIAG_FILE(path, "GetFileAttributesA '%s' -> 0x%lX", path, a);
    return a;
}

static BOOL WINAPI Hook_GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    char workBuf[MAX_PATH];
    return GetFileAttributesExA(AKSYS_WORK_PATH(path, workBuf), level, info);
}

static BOOL WINAPI Hook_SetFileAttributesA(LPCSTR path, DWORD attrs) {
    char workBuf[MAX_PATH];
    return SetFileAttributesA(AKSYS_WORK_PATH(path, workBuf), attrs);
}

static BOOL WINAPI Hook_RemoveDirectoryA(LPCSTR path) {
    char workBuf[MAX_PATH];
    return RemoveDirectoryA(AKSYS_WORK_PATH(path, workBuf));
}

static HANDLE WINAPI Hook_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data) {
    char workBuf[MAX_PATH];
    return FindFirstFileA(AKSYS_WORK_PATH(pattern, workBuf), data);
}

static BOOL WINAPI Hook_MoveFileA(LPCSTR lpExisting, LPCSTR lpNew) {
    if (!lpExisting || !lpNew) return FALSE;
    char existBuf[MAX_PATH], newBuf[MAX_PATH];
    lpExisting = AKSYS_WORK_PATH(lpExisting, existBuf);
    lpNew = AKSYS_WORK_PATH(lpNew, newBuf);
    DIAG_FILE(lpExisting, "MoveFileA '%s' -> '%s'", lpExisting, lpNew);
    CreateParentDirectoryA(lpNew);
    if (MoveFileA(lpExisting, lpNew)) {
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHA, lpNew, NULL);
        return TRUE;
    }
    char relPath[MAX_PATH] = {0};
    GetRelativePathFromAksysOrName(lpExisting, relPath, sizeof(relPath));
    char tempPath[MAX_PATH] = {0};
    if (GetTempPathA(MAX_PATH, tempPath)) {
        char redirectedSrc[MAX_PATH] = {0};
        snprintf(redirectedSrc, sizeof(redirectedSrc), "%sAKSys_Transfer_Cache\\%s", tempPath, relPath);
        if (MoveFileExA(redirectedSrc, lpNew, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
            SHChangeNotify(SHCNE_CREATE, SHCNF_PATHA, lpNew, NULL);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL WINAPI Hook_MoveFileW(LPCWSTR lpExisting, LPCWSTR lpNew) {
    if (!lpExisting || !lpNew) return FALSE;
    char ansiExist[MAX_PATH] = {0};
    char ansiNew[MAX_PATH] = {0};
    WideCharToMultiByte(CP_ACP, 0, lpExisting, -1, ansiExist, MAX_PATH - 1, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, lpNew, -1, ansiNew, MAX_PATH - 1, NULL, NULL);
    return Hook_MoveFileA(ansiExist, ansiNew);
}

static BOOL WINAPI Hook_CopyFileA(LPCSTR lpExisting, LPCSTR lpNew, BOOL bFailIfExists) {
    if (!lpExisting || !lpNew) return FALSE;
    char existBuf[MAX_PATH], newBuf[MAX_PATH];
    lpExisting = AKSYS_WORK_PATH(lpExisting, existBuf);
    lpNew = AKSYS_WORK_PATH(lpNew, newBuf);
    DIAG_FILE(lpExisting, "CopyFileA '%s' -> '%s'", lpExisting, lpNew);
    CreateParentDirectoryA(lpNew);
    if (CopyFileA(lpExisting, lpNew, bFailIfExists)) {
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHA, lpNew, NULL);
        return TRUE;
    }
    char relPath[MAX_PATH] = {0};
    GetRelativePathFromAksysOrName(lpExisting, relPath, sizeof(relPath));
    char tempPath[MAX_PATH] = {0};
    if (GetTempPathA(MAX_PATH, tempPath)) {
        char redirectedSrc[MAX_PATH] = {0};
        snprintf(redirectedSrc, sizeof(redirectedSrc), "%sAKSys_Transfer_Cache\\%s", tempPath, relPath);
        if (CopyFileA(redirectedSrc, lpNew, bFailIfExists)) {
            SHChangeNotify(SHCNE_CREATE, SHCNF_PATHA, lpNew, NULL);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL WINAPI Hook_CopyFileW(LPCWSTR lpExisting, LPCWSTR lpNew, BOOL bFailIfExists) {
    if (!lpExisting || !lpNew) return FALSE;
    char ansiExist[MAX_PATH] = {0};
    char ansiNew[MAX_PATH] = {0};
    WideCharToMultiByte(CP_ACP, 0, lpExisting, -1, ansiExist, MAX_PATH - 1, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, lpNew, -1, ansiNew, MAX_PATH - 1, NULL, NULL);
    return Hook_CopyFileA(ansiExist, ansiNew, bFailIfExists);
}

static void CompletePendingReadForContext(AkaiHandleContext* ctx, DWORD gotBytes, BOOL ok) {
    ctx->readPending = false;
    ResetEvent(ctx->hEvent);
    ctx->lastBytesTransferred = ok ? gotBytes : 0;
    ctx->lastResultOk = ok;
    g_LastBytesTransferred = ctx->lastBytesTransferred;

    if (ok && gotBytes > 0) {
        if (ctx->pUserDest) {
            DWORD copyLen = (ctx->userReqLen < gotBytes) ? ctx->userReqLen : gotBytes;
            memcpy(ctx->pUserDest, ctx->readBuffer, copyLen);
        }
    }

    // Taken atomically so a cancel and a completing read can never both complete it.
    LPOVERLAPPED uOv = (LPOVERLAPPED)InterlockedExchangePointer((PVOID volatile*)&ctx->pUserOv, NULL);
    ctx->pUserDest = NULL;

    if (uOv) {
        uOv->Internal = ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        uOv->InternalHigh = ok ? gotBytes : 0;
        if (uOv->hEvent) {
            SetEvent(uOv->hEvent);
        }
    }
}

#ifndef STATUS_CANCELLED
#define STATUS_CANCELLED ((ULONG_PTR)0xC0000120L)
#endif

// Cancels a pending pipe read the way the real driver does: the transfer is aborted AND the
// caller's OVERLAPPED is completed as cancelled with its event signalled. Previously only the
// transfer was aborted, so ak.Sys waited on that event until its own timeout ran out when it
// cancelled its background read at shutdown (editor windows lingered for several seconds).
static void CancelPendingRead(AkaiHandleContext* ctx, WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR inPipe) {
    if (!ctx->readPending) return;
    ctx->readPending = false;  // stops AsyncReadMonitorThread from completing it as well
    if (hWinUsb) {
        WinUsb_AbortPipe(hWinUsb, inPipe);
        // Let the aborted transfer finish before the read buffer is reused.
        if (ctx->hEvent) WaitForSingleObject(ctx->hEvent, 200);
    }
    if (ctx->hEvent) ResetEvent(ctx->hEvent);
    ctx->lastBytesTransferred = 0;
    ctx->lastResultOk = FALSE;
    ctx->pUserDest = NULL;
    LPOVERLAPPED uOv = (LPOVERLAPPED)InterlockedExchangePointer((PVOID volatile*)&ctx->pUserOv, NULL);
    DIAG("USB read cancelled; caller's OVERLAPPED %s", uOv ? "completed as cancelled" : "(none)");
    if (uOv) {
        uOv->Internal = STATUS_CANCELLED;
        uOv->InternalHigh = 0;
        if (uOv->hEvent) SetEvent(uOv->hEvent);
    }
}

static DWORD WINAPI AsyncReadMonitorThread(LPVOID lpParam) {
    int idx = (int)(ULONG_PTR)lpParam;
    if (idx < 0 || idx >= MAX_AKAI_HANDLES) return 0;
    AkaiHandleContext* ctx = &g_Handles[idx];
    int uIdx = ctx->unitIndex;
    if (uIdx < 0 || uIdx >= MAX_AKAI_UNITS) return 0;
    WINUSB_INTERFACE_HANDLE hWinUsb = g_Units[uIdx].hWinUsb;

    while (ctx->readPending && hWinUsb) {
        DWORD waitRes = WaitForSingleObject(ctx->hEvent, 100);
        if (!ctx->readPending) break;
        if (waitRes == WAIT_OBJECT_0) {
            ULONG got = 0;
            BOOL ok = WinUsb_GetOverlappedResult(hWinUsb, &ctx->winusbOv, &got, FALSE);
            CompletePendingReadForContext(ctx, got, ok);
            break;
        }
    }
    return 0;
}

static BOOL WINAPI Hook_WriteFile(HANDLE hFile, LPCVOID buf, DWORD len, LPDWORD written, LPOVERLAPPED ov) {
    if (!IsAkaiHandle(hFile)) return WriteFile(hFile, buf, len, written, ov);

    int idx = GetAkaiHandleIndex(hFile);
    if (idx < 0) return CompleteIoOperation(ov, 0, FALSE);
    AkaiHandleContext* ctx = &g_Handles[idx];
    int uIdx = ctx->unitIndex;
    WINUSB_INTERFACE_HANDLE hWinUsb = (uIdx >= 0 && uIdx < MAX_AKAI_UNITS) ? g_Units[uIdx].hWinUsb : NULL;
    UCHAR outPipe = (uIdx >= 0 && uIdx < MAX_AKAI_UNITS && g_Units[uIdx].active) ? g_Units[uIdx].bulkOutPipe : 0x02;

    if (ov) {
        ov->Internal = STATUS_PENDING;
        ov->InternalHigh = 0;
        if (ov->hEvent) ResetEvent(ov->hEvent);
    }

    ULONG totalWritten = 0;
    BOOL ok = FALSE;

    if (hWinUsb && buf && len > 0) {
        const PUCHAR pSrc = (const PUCHAR)buf;
        DWORD remaining = len;
        ok = TRUE;

        OVERLAPPED localOv;
        memset(&localOv, 0, sizeof(localOv));
        localOv.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        while (remaining > 0) {
            DWORD chunk = (remaining > 32768) ? 32768 : remaining;
            ULONG transferred = 0;
            ResetEvent(localOv.hEvent);

            BOOL wOk = WinUsb_WritePipe(hWinUsb, outPipe, (PUCHAR)(pSrc + totalWritten), chunk, &transferred, &localOv);
            if (!wOk && GetLastError() == ERROR_IO_PENDING) {
                wOk = WinUsb_GetOverlappedResult(hWinUsb, &localOv, &transferred, TRUE);
            }
            if (!wOk) {
                WinUsb_ResetPipe(hWinUsb, outPipe);
                ok = FALSE;
                break;
            }
            totalWritten += transferred;
            if (transferred == 0) break;
            remaining -= transferred;
        }
        CloseHandle(localOv.hEvent);
    } else if (hWinUsb && len == 0) {
        ok = TRUE;
    }

    ctx->lastBytesTransferred = ok ? totalWritten : 0;
    ctx->lastResultOk = ok;
    if (written) *written = ctx->lastBytesTransferred;
    return CompleteIoOperation(ov, ctx->lastBytesTransferred, ok);
}

static BOOL WINAPI Hook_ReadFile(HANDLE hFile, LPVOID buf, DWORD len, LPDWORD readBytes, LPOVERLAPPED ov) {
    if (!IsAkaiHandle(hFile)) return ReadFile(hFile, buf, len, readBytes, ov);

    int idx = GetAkaiHandleIndex(hFile);
    if (idx < 0) {
        if (readBytes) *readBytes = 0;
        return CompleteIoOperation(ov, 0, FALSE);
    }

    AkaiHandleContext* ctx = &g_Handles[idx];
    int uIdx = ctx->unitIndex;
    WINUSB_INTERFACE_HANDLE hWinUsb = (uIdx >= 0 && uIdx < MAX_AKAI_UNITS) ? g_Units[uIdx].hWinUsb : NULL;
    UCHAR inPipe = (uIdx >= 0 && uIdx < MAX_AKAI_UNITS && g_Units[uIdx].active) ? g_Units[uIdx].bulkInPipe : 0x82;

    if (!hWinUsb) {
        if (readBytes) *readBytes = 0;
        return CompleteIoOperation(ov, 0, FALSE);
    }

    if (ov) {
        ov->Internal = STATUS_PENDING;
        ov->InternalHigh = 0;
        if (ov->hEvent) ResetEvent(ov->hEvent);
    }

    ULONG totalRead = 0;
    BOOL ok = FALSE;

    if (ctx->readPending) {
        DWORD waitRes = WaitForSingleObject(ctx->hEvent, 20);
        if (waitRes == WAIT_OBJECT_0) {
            ok = WinUsb_GetOverlappedResult(hWinUsb, &ctx->winusbOv, &totalRead, FALSE);
            ctx->readPending = false;
            ResetEvent(ctx->hEvent);
        } else {
            ctx->pUserDest = buf;
            ctx->userReqLen = len;
            ctx->pUserOv = ov;
            if (readBytes) *readBytes = 0;
            SetLastError(ERROR_IO_PENDING);
            return FALSE;
        }
    }

    if (!ctx->readPending && !ok) {
        ULONG firstReq = (len > AKAI_READ_BUFFER_SIZE) ? AKAI_READ_BUFFER_SIZE : len;
        if (firstReq > 32768) firstReq = 32768;

        ResetEvent(ctx->hEvent);
        memset(&ctx->winusbOv, 0, sizeof(OVERLAPPED));
        ctx->winusbOv.hEvent = ctx->hEvent;

        ok = WinUsb_ReadPipe(hWinUsb, inPipe, ctx->readBuffer, firstReq, &totalRead, &ctx->winusbOv);
        DWORD err = GetLastError();

        if (!ok && err == ERROR_IO_PENDING) {
            DWORD initialWait = (len >= 512) ? 2000 : 25;
            DWORD waitRes = WaitForSingleObject(ctx->hEvent, initialWait);
            if (waitRes == WAIT_OBJECT_0) {
                ok = WinUsb_GetOverlappedResult(hWinUsb, &ctx->winusbOv, &totalRead, FALSE);
                ctx->readPending = false;
                ResetEvent(ctx->hEvent);
            } else {
                ctx->pUserDest = buf;
                ctx->userReqLen = len;
                ctx->pUserOv = ov;
                ctx->readPending = true;
                HANDLE hTh = CreateThread(NULL, 0, AsyncReadMonitorThread, (LPVOID)(ULONG_PTR)idx, 0, NULL);
                if (hTh) CloseHandle(hTh);
                if (readBytes) *readBytes = 0;
                SetLastError(ERROR_IO_PENDING);
                return FALSE;
            }
        }

        if (ok && totalRead > 0 && buf) {
            DWORD firstCopy = (totalRead < len) ? totalRead : len;
            memcpy(buf, ctx->readBuffer, firstCopy);
            totalRead = firstCopy;

            PUCHAR pUserBytes = (PUCHAR)buf;
            while (ok && totalRead < len && len >= 512 && (totalRead % 64) == 0) {
                DWORD rem = len - totalRead;
                ULONG chunkReq = (rem > 32768) ? 32768 : rem;
                ULONG chunkGot = 0;

                ResetEvent(ctx->hEvent);
                memset(&ctx->winusbOv, 0, sizeof(OVERLAPPED));
                ctx->winusbOv.hEvent = ctx->hEvent;

                BOOL chunkOk = WinUsb_ReadPipe(hWinUsb, inPipe,
                                               pUserBytes + totalRead,
                                               chunkReq,
                                               &chunkGot, &ctx->winusbOv);
                if (!chunkOk && GetLastError() == ERROR_IO_PENDING) {
                    if (WaitForSingleObject(ctx->hEvent, 1500) == WAIT_OBJECT_0) {
                        chunkOk = WinUsb_GetOverlappedResult(hWinUsb, &ctx->winusbOv, &chunkGot, FALSE);
                    } else {
                        CancelIoEx(g_Units[uIdx].hUsbDevice, &ctx->winusbOv);
                        break;
                    }
                }
                if (!chunkOk || chunkGot == 0) break;
                totalRead += chunkGot;
                if ((chunkGot % 64) != 0) break;
            }
        }
    } else if (ok && totalRead > 0 && buf) {
        DWORD copyLen = (len < totalRead) ? len : totalRead;
        memcpy(buf, ctx->readBuffer, copyLen);
        totalRead = copyLen;
    }

    if (ok && totalRead > 0) {
        ctx->lastBytesTransferred = totalRead;
        ctx->lastResultOk = TRUE;
        if (readBytes) *readBytes = totalRead;
        CompleteIoOperation(ov, totalRead, TRUE);

        if (ov) {
            SetLastError(ERROR_IO_PENDING);
            return FALSE;
        }
        return TRUE;
    }

    ctx->lastBytesTransferred = 0;
    ctx->lastResultOk = FALSE;
    if (readBytes) *readBytes = 0;
    if (ov) {
        SetLastError(ERROR_IO_PENDING);
        return FALSE;
    }

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static BOOL WINAPI Hook_GetOverlappedResult(HANDLE hFile, LPOVERLAPPED ov, LPDWORD trans, BOOL wait) {
    if (IsAkaiHandle(hFile)) {
        int idx = GetAkaiHandleIndex(hFile);
        int uIdx = (idx >= 0) ? g_Handles[idx].unitIndex : 0;
        WINUSB_INTERFACE_HANDLE hWinUsb = (uIdx >= 0 && uIdx < MAX_AKAI_UNITS) ? g_Units[uIdx].hWinUsb : NULL;

        if (idx >= 0 && g_Handles[idx].readPending && g_Handles[idx].hEvent && hWinUsb) {
            DWORD waitTime = wait ? 15000 : 0;
            DWORD waitRes = WaitForSingleObject(g_Handles[idx].hEvent, waitTime);
            if (waitRes == WAIT_OBJECT_0) {
                ULONG got = 0;
                if (WinUsb_GetOverlappedResult(hWinUsb, &g_Handles[idx].winusbOv, &got, FALSE)) {
                    CompletePendingReadForContext(&g_Handles[idx], got, TRUE);
                    if (trans) *trans = got;
                    SetLastError(ERROR_SUCCESS);
                    return TRUE;
                }
            } else {
                if (trans) *trans = 0;
                SetLastError(ERROR_IO_INCOMPLETE);
                return FALSE;
            }
        }

        if (ov && ov->Internal == STATUS_PENDING) {
            if (trans) *trans = 0;
            SetLastError(ERROR_IO_INCOMPLETE);
            return FALSE;
        }

        DWORD bytes = ov ? (DWORD)ov->InternalHigh : ((idx >= 0) ? g_Handles[idx].lastBytesTransferred : g_LastBytesTransferred);
        BOOL ok = (ov == NULL) ? ((idx >= 0) ? g_Handles[idx].lastResultOk : TRUE) : (ov->Internal == STATUS_SUCCESS);
        if (trans) *trans = bytes;
        if (ov && ov->Internal == STATUS_CANCELLED) {
            SetLastError(ERROR_OPERATION_ABORTED);
            return FALSE;
        }
        SetLastError(ok ? ERROR_SUCCESS : ERROR_GEN_FAILURE);
        return ok;
    }
    return GetOverlappedResult(hFile, ov, trans, wait);
}

static BOOL WINAPI Hook_DeviceIoControl(HANDLE hDev, DWORD code, LPVOID inBuf, DWORD inSz,
                                       LPVOID outBuf, DWORD outSz, LPDWORD ret, LPOVERLAPPED ov) {
    if (!IsAkaiHandle(hDev)) return DeviceIoControl(hDev, code, inBuf, inSz, outBuf, outSz, ret, ov);

    int idx = GetAkaiHandleIndex(hDev);
    int uIdx = (idx >= 0) ? g_Handles[idx].unitIndex : 0;
    WINUSB_INTERFACE_HANDLE hWinUsb = (uIdx >= 0 && uIdx < MAX_AKAI_UNITS) ? g_Units[uIdx].hWinUsb : NULL;
    UCHAR inPipe = (uIdx >= 0 && uIdx < MAX_AKAI_UNITS && g_Units[uIdx].active) ? g_Units[uIdx].bulkInPipe : 0x82;
    UCHAR outPipe = (uIdx >= 0 && uIdx < MAX_AKAI_UNITS && g_Units[uIdx].active) ? g_Units[uIdx].bulkOutPipe : 0x02;

    if (code == 0x00220008 || code == 0x00220000 || code == 0x00220004 ||
        code == 0x0022000C || code == 0x00220010 || code == 0x00220014) {
        if (idx >= 0 && g_Handles[idx].type == HANDLE_TYPE_PIPE0 && hWinUsb) {
            g_Handles[idx].readPending = false;
            WinUsb_AbortPipe(hWinUsb, inPipe);
        }
        if (outBuf && outSz > 0) memset(outBuf, 0, outSz);
        if (idx >= 0) {
            g_Handles[idx].lastBytesTransferred = outSz;
            g_Handles[idx].lastResultOk = TRUE;
        }
        if (ret) *ret = outSz;
        return CompleteIoOperation(ov, outSz, TRUE);
    }

    if (hWinUsb && inBuf && inSz > 0 && (!outBuf || outSz == 0)) {
        ULONG transferred = 0;
        OVERLAPPED localOv;
        memset(&localOv, 0, sizeof(localOv));
        localOv.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        BOOL ok = WinUsb_WritePipe(hWinUsb, outPipe, (PUCHAR)inBuf, inSz, &transferred, &localOv);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            ok = WinUsb_GetOverlappedResult(hWinUsb, &localOv, &transferred, TRUE);
        }
        CloseHandle(localOv.hEvent);
        if (idx >= 0) {
            g_Handles[idx].lastBytesTransferred = transferred;
            g_Handles[idx].lastResultOk = ok;
        }
        if (ret) *ret = transferred;
        return CompleteIoOperation(ov, transferred, ok);
    }

    if (outBuf && outSz > 0) memset(outBuf, 0, outSz);
    if (idx >= 0) {
        g_Handles[idx].lastBytesTransferred = outSz;
        g_Handles[idx].lastResultOk = TRUE;
    }
    if (ret) *ret = outSz;
    return CompleteIoOperation(ov, outSz, TRUE);
}

static BOOL WINAPI Hook_CancelIo(HANDLE hFile) {
    if (IsAkaiHandle(hFile)) {
        int idx = GetAkaiHandleIndex(hFile);
        if (idx >= 0) {
            int uIdx = g_Handles[idx].unitIndex;
            WINUSB_INTERFACE_HANDLE hWinUsb = (uIdx >= 0 && uIdx < MAX_AKAI_UNITS) ? g_Units[uIdx].hWinUsb : NULL;
            UCHAR inPipe = (uIdx >= 0 && uIdx < MAX_AKAI_UNITS && g_Units[uIdx].active) ? g_Units[uIdx].bulkInPipe : 0x82;
            CancelPendingRead(&g_Handles[idx], hWinUsb, inPipe);
        }
        return TRUE;
    }
    return CancelIo(hFile);
}

#define MAX_SCALED_WINDOWS 256
#define WM_AKAI_APPLY_ZOOM   (WM_APP + 0x414B)

struct ScaledSubWindow {
    HWND hwnd;
    HWND hwndCanvas;
    WNDPROC origFrameProc;
    WNDPROC origCanvasProc;
    bool isRemotePanel;
    bool isZ48OrMpcRemote;
    bool isMultiWindow;
    bool snapSuppressed;  // WS_MAXIMIZEBOX temporarily removed during an aspect-locked resize
    bool closing;         // frame is being closed/destroyed: no more re-zooming
    bool multiCropLocked; // Multi editor's painted area measured consistently; stop re-measuring
    int multiCropStableCount;
    int lockedCropW;
    int lockedCropH;
    bool isProgramWindow;
    bool isEb20Window;
    bool isDropdownPopup;
    int frameNonClientW;
    int frameNonClientH;
    int baseCanvasW;
    int baseCanvasH;
    int cropW;
    int cropH;
    int curCanvasW;
    int curCanvasH;
    double currentZoom;
    bool inZoomResize;
    DWORD discoveredTick;
    HRGN hNativeBaseRgn;
    HDC hBackDC;
    HBITMAP hBackBmp;
    DWORD* pBackPixels;
    int backW;
    int backH;
    bool initialized;
};

static ScaledSubWindow g_ScaledWins[MAX_SCALED_WINDOWS];
static HWND g_hToolbarWidget = NULL;
static double g_CurrentGlobalZoom = 1.0;
static volatile bool g_KeepAspectRatio = true;
static volatile bool g_ToolbarClosedByUser = false;
static ScaledSubWindow* g_LastClickedEditor = NULL;

// Restore the last zoom preset and aspect lock the user chose (defaults: 1.0x, locked).
static void LoadZoomSettings() {
    DWORD pct = ReadShimSettingDword("ZoomPercent", 100);
    if (pct < 100 || pct > 200) pct = 100;
    g_CurrentGlobalZoom = pct / 100.0;
    g_KeepAspectRatio = (ReadShimSettingDword("KeepAspectRatio", 1) != 0);
}

static ScaledSubWindow* FindByFrame(HWND hwnd) {
    if (!hwnd) return NULL;
    for (int i = 0; i < MAX_SCALED_WINDOWS; i++) {
        if (g_ScaledWins[i].initialized && g_ScaledWins[i].hwnd == hwnd) {
            return &g_ScaledWins[i];
        }
    }
    return NULL;
}

static ScaledSubWindow* FindByCanvasOrFrame(HWND hwnd) {
    if (!hwnd) return NULL;
    for (int i = 0; i < MAX_SCALED_WINDOWS; i++) {
        if (g_ScaledWins[i].initialized) {
            if (g_ScaledWins[i].hwndCanvas == hwnd || g_ScaledWins[i].hwnd == hwnd) {
                return &g_ScaledWins[i];
            }
        }
    }
    HWND hParent = GetParent(hwnd);
    if (hParent) return FindByFrame(hParent);
    return NULL;
}

static int GetEffectiveSourceW(ScaledSubWindow* sw) {
    if (!sw) return 100;
    if (sw->isMultiWindow && sw->cropW >= sw->baseCanvasW / 2 && sw->cropW <= sw->baseCanvasW) return sw->cropW;
    return sw->baseCanvasW;
}

static int GetEffectiveSourceH(ScaledSubWindow* sw) {
    if (!sw) return 100;
    if (sw->isDropdownPopup) return sw->baseCanvasH;
    return (sw->cropH >= 96 && sw->cropH <= sw->baseCanvasH) ? sw->cropH : sw->baseCanvasH;
}

static void EnsureBackbuffer(ScaledSubWindow* sw, HDC hdcRef) {
    if (!sw || sw->baseCanvasW <= 0 || sw->baseCanvasH <= 0) return;
    if (sw->hBackDC && sw->hBackBmp && sw->pBackPixels &&
        sw->backW == sw->baseCanvasW && sw->backH == sw->baseCanvasH) {
        return;
    }

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sw->baseCanvasW;
    bmi.bmiHeader.biHeight = -sw->baseCanvasH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    HDC hNewDC = CreateCompatibleDC(hdcRef);
    HBITMAP hNewBmp = CreateDIBSection(hdcRef, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (hNewBmp && pBits) {
        SelectObject(hNewDC, hNewBmp);
        memset(pBits, 0, (size_t)sw->baseCanvasW * (size_t)sw->baseCanvasH * 4);
    }

    if (sw->hBackDC && sw->hBackBmp && sw->backW > 0 && sw->backH > 0 && hNewBmp) {
        int copyW = (sw->backW < sw->baseCanvasW) ? sw->backW : sw->baseCanvasW;
        int copyH = (sw->backH < sw->baseCanvasH) ? sw->backH : sw->baseCanvasH;
        BitBlt(hNewDC, 0, 0, copyW, copyH, sw->hBackDC, 0, 0, SRCCOPY);
    }

    if (sw->hBackBmp) DeleteObject(sw->hBackBmp);
    if (sw->hBackDC) DeleteDC(sw->hBackDC);

    sw->hBackDC = hNewDC;
    sw->hBackBmp = hNewBmp;
    sw->pBackPixels = (DWORD*)pBits;
    sw->backW = sw->baseCanvasW;
    sw->backH = sw->baseCanvasH;
    sw->cropW = sw->baseCanvasW;
    sw->cropH = sw->baseCanvasH;
}

#define MULTI_CROP_LOCK_COUNT 5

static void ProcessBackbufferPixels(ScaledSubWindow* sw) {
    if (!sw || !sw->pBackPixels || sw->backW <= 10 || sw->backH <= 10) return;
    GdiFlush();

    int W = sw->backW;
    int H = sw->backH;
    DWORD* px = sw->pBackPixels;

    if (sw->isDropdownPopup) {
        sw->cropW = W;
        sw->cropH = H;
        return;
    }

    if (sw->isRemotePanel) {
        const DWORD bezelColor = sw->isZ48OrMpcRemote ? 0x002B3842 : 0x00315163;
        for (int i = 0; i < W * H; i++) {
            DWORD c = px[i];
            BYTE r = (BYTE)((c >> 16) & 0xFF);
            BYTE g = (BYTE)((c >> 8) & 0xFF);
            BYTE b = (BYTE)(c & 0xFF);
            if (r >= 180 && g <= 70 && b >= 180 && (r + b - g * 2) > 260) {
                px[i] = bezelColor;
            }
        }
        sw->cropW = W;
        sw->cropH = H;
        return;
    }

    int prevCropW = sw->cropW;
    int prevCropH = sw->cropH;
    sw->cropW = W;
    if (sw->isEb20Window) {
        sw->cropH = H;
        return;
    }

    int validH = H;
    for (int y = H - 1; y >= 96; y--) {
        DWORD* row = px + y * W;
        int nonBlack = 0;
        for (int x = 8; x < W - 8; x += 4) {
            if ((row[x] & 0x00FFFFFF) != 0) {
                nonBlack++;
                if (nonBlack > 6) break;
            }
        }
        if (nonBlack > 6) {
            validH = y + 1;
            break;
        }
    }

    if (sw->isProgramWindow) {
        if (validH >= 500 && validH <= 560 && sw->baseCanvasH == 546) validH = 546;
        else if (validH >= 300 && validH <= 345) validH = 322;
        else if (validH >= 140 && validH <= 175) validH = 154;
        else if (validH <= 125) validH = 104;
        if (validH != sw->cropH) DIAG("PixelCrop prog: H=%d base=%dx%d cropH %d -> %d", H, sw->baseCanvasW, sw->baseCanvasH, sw->cropH, validH);
        sw->cropH = validH;
    } else if (sw->isMultiWindow) {
        // The Multi editor's view is smaller than the window ak.Sys opens it in; the unpainted
        // strip on the right and bottom is pure black, so scale only the painted area.
        // That area is fixed, but while ak.Sys builds or tears the editor down the measurement
        // jumps around, and every jump re-zooms (resizes) a very wide window. So once the same
        // area has been measured several times running, it is locked in.
        if (sw->multiCropLocked && sw->lockedCropW <= W && sw->lockedCropH <= H) {
            sw->cropW = sw->lockedCropW;
            sw->cropH = sw->lockedCropH;
            return;
        }
        int validW = W;
        for (int x = W - 1; x >= W / 2; x--) {
            int nonBlack = 0;
            for (int y = 0; y < H; y += 4) {
                if ((px[y * W + x] & 0x00FFFFFF) != 0 && ++nonBlack > 6) break;
            }
            if (nonBlack > 6) { validW = x + 1; break; }
        }
        int validMultiH = H;
        for (int y = H - 1; y >= H / 2; y--) {
            DWORD* row = px + y * W;
            int nonBlack = 0;
            for (int x = 0; x < validW; x += 4) {
                if ((row[x] & 0x00FFFFFF) != 0 && ++nonBlack > 6) break;
            }
            if (nonBlack > 6) { validMultiH = y + 1; break; }
        }
        if (validW != prevCropW || validMultiH != prevCropH) {
            DIAG("PixelCrop multi: %dx%d -> %dx%d", W, H, validW, validMultiH);
            sw->multiCropStableCount = 0;
        } else if (++sw->multiCropStableCount >= MULTI_CROP_LOCK_COUNT) {
            sw->multiCropLocked = true;
            sw->lockedCropW = validW;
            sw->lockedCropH = validMultiH;
            DIAG("PixelCrop multi locked at %dx%d", validW, validMultiH);
        }
        sw->cropW = validW;
        sw->cropH = validMultiH;
    } else {
        sw->cropH = H;
    }
}

static HRGN CreateCleanRemoteRegion(ScaledSubWindow* sw, int targetW, int targetH, int insetX, int insetY) {
    double baseW = (sw && sw->baseCanvasW > 100) ? (double)sw->baseCanvasW : 540.0;
    double baseH = (sw && sw->baseCanvasH > 100) ? (double)sw->baseCanvasH : 368.0;
    double scaleX = (double)targetW / baseW;
    double scaleY = (double)targetH / baseH;
    double cornerRadius = (sw && sw->isZ48OrMpcRemote) ? 24.0 : 38.0;
    int radX = (int)(cornerRadius * scaleX + 0.5);
    int radY = (int)(cornerRadius * scaleY + 0.5);
    int left = insetX + (int)(4.0 * scaleX + 0.5);
    int top  = insetY + (int)(4.0 * scaleY + 0.5);
    int right  = insetX + targetW - (int)(4.0 * scaleX + 0.5);
    int bottom = insetY + targetH - (int)(4.0 * scaleY + 0.5);
    return CreateRoundRectRgn(left, top, right, bottom, radX, radY);
}

static void UpdateWindowRegionsForScale(ScaledSubWindow* sw) {
    if (!sw) return;
    if (sw->isRemotePanel && sw->curCanvasW > 100 && sw->curCanvasH > 100) {
        if (sw->hwndCanvas && IsWindow(sw->hwndCanvas)) {
            HRGN hCanvasRgn = CreateCleanRemoteRegion(sw, sw->curCanvasW, sw->curCanvasH, 0, 0);
            SetWindowRgn(sw->hwndCanvas, hCanvasRgn, TRUE);
        }
        if (sw->hwnd && IsWindow(sw->hwnd)) {
            RECT rcWin = {0}, rcClient = {0};
            GetWindowRect(sw->hwnd, &rcWin);
            GetClientRect(sw->hwnd, &rcClient);
            POINT ptClientTopLeft = {0, 0};
            ClientToScreen(sw->hwnd, &ptClientTopLeft);
            int offsetX = ptClientTopLeft.x - rcWin.left;
            int offsetY = ptClientTopLeft.y - rcWin.top;
            HRGN hFrameRgn = CreateCleanRemoteRegion(sw, sw->curCanvasW, sw->curCanvasH, offsetX, offsetY);
            SetWindowRgn(sw->hwnd, hFrameRgn, TRUE);
        }
    } else {
        if (sw->hwnd && IsWindow(sw->hwnd)) SetWindowRgn(sw->hwnd, NULL, FALSE);
        if (sw->hwndCanvas && IsWindow(sw->hwndCanvas)) SetWindowRgn(sw->hwndCanvas, NULL, FALSE);
    }
}

static void StripScrollBars(HWND hCanvas) {
    if (!hCanvas || !IsWindow(hCanvas)) return;
    LONG cStyle = GetWindowLongA(hCanvas, GWL_STYLE);
    if (cStyle & (WS_HSCROLL | WS_VSCROLL)) {
        SetWindowLongA(hCanvas, GWL_STYLE, cStyle & ~(WS_HSCROLL | WS_VSCROLL));
    }
    ShowScrollBar(hCanvas, SB_BOTH, FALSE);
}

static int WINAPI Hook_SetWindowRgn(HWND hWnd, HRGN hRgn, BOOL bRedraw) {
    ScaledSubWindow* sw = FindByCanvasOrFrame(hWnd);
    if (sw && hRgn != NULL) {
        DeleteObject(hRgn);
        if (sw->isRemotePanel && sw->curCanvasW > 100 && sw->curCanvasH > 100) {
            UpdateWindowRegionsForScale(sw);
            return 1;
        }
        return SetWindowRgn(hWnd, NULL, bRedraw);
    }
    return SetWindowRgn(hWnd, hRgn, bRedraw);
}

static BOOL WINAPI Hook_InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase) {
    if (hWnd && lpRect != NULL) {
        ScaledSubWindow* sw = FindByCanvasOrFrame(hWnd);
        if (sw) return InvalidateRect(hWnd, NULL, FALSE);
    }
    return InvalidateRect(hWnd, lpRect, bErase);
}

static BOOL WINAPI Hook_GetClientRect(HWND hWnd, LPRECT lpRect) {
    BOOL ok = GetClientRect(hWnd, lpRect);
    if (ok && lpRect) {
        ScaledSubWindow* sw = FindByCanvasOrFrame(hWnd);
        if (sw && sw->hwndCanvas == hWnd && sw->baseCanvasW > 10 && sw->baseCanvasH > 10) {
            lpRect->left = 0;
            lpRect->top = 0;
            lpRect->right = sw->baseCanvasW;
            lpRect->bottom = sw->baseCanvasH;
        }
    }
    return ok;
}

static BOOL WINAPI Hook_BitBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
                               HDC hdcSrc, int xSrc, int ySrc, DWORD rop) {
    if (hdcDest && hdcSrc && wDest > 0 && hDest > 0) {
        HWND hwnd = WindowFromDC(hdcDest);
        if (hwnd) {
            ScaledSubWindow* sw = FindByCanvasOrFrame(hwnd);
            if (sw && sw->baseCanvasW > 10 && sw->baseCanvasH > 10 &&
                sw->curCanvasW > 10 && sw->curCanvasH > 10) {

                if (xDest == 0 && yDest == 0 && wDest > sw->baseCanvasW && wDest <= 2400) {
                    sw->baseCanvasW = wDest;
                }
                if (xDest == 0 && yDest == 0 && hDest > sw->baseCanvasH && hDest <= 1600) {
                    sw->baseCanvasH = hDest;
                }

                int prevCropH = sw->cropH;
                int prevCropW = sw->cropW;
                RECT rcClip = {0};
                int clipType = GetClipBox(hdcDest, &rcClip);

                EnsureBackbuffer(sw, hdcDest);
                if (sw->hBackDC) {
                    if (!sw->isRemotePanel && !sw->isDropdownPopup && xDest == 0 && yDest == 0 &&
                        wDest >= sw->baseCanvasW - 8 && hDest >= 96 &&
                        clipType == SIMPLEREGION &&
                        ((rcClip.right - rcClip.left) < sw->baseCanvasW - 16 ||
                         (rcClip.bottom - rcClip.top) < hDest - 16)) {
                        int cx = (rcClip.left > 0) ? rcClip.left : 0;
                        int cy = (rcClip.top > 0) ? rcClip.top : 0;
                        int cw = rcClip.right - cx;
                        int ch = rcClip.bottom - cy;
                        if (cw > 0 && ch > 0) {
                            BitBlt(sw->hBackDC, cx, cy, cw, ch, hdcSrc, cx, cy, rop);
                        }
                    } else {
                        BitBlt(sw->hBackDC, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, rop);
                    }
                    ProcessBackbufferPixels(sw);

                    if (sw->isProgramWindow && abs(sw->cropH - prevCropH) > 30 && !sw->inZoomResize && !sw->closing) {
                        PostMessageA(sw->hwnd, WM_AKAI_APPLY_ZOOM, (WPARAM)(int)(sw->currentZoom * 100.0 + 0.5), 0);
                    }
                    if (sw->isMultiWindow && (abs(sw->cropW - prevCropW) > 4 || abs(sw->cropH - prevCropH) > 4) &&
                        !sw->inZoomResize && !sw->closing) {
                        DIAG("Multi re-zoom posted: crop %dx%d -> %dx%d", prevCropW, prevCropH, sw->cropW, sw->cropH);
                        PostMessageA(sw->hwnd, WM_AKAI_APPLY_ZOOM, (WPARAM)(int)(sw->currentZoom * 100.0 + 0.5), 0);
                    }
                }

                int srcW = GetEffectiveSourceW(sw);
                int srcH = GetEffectiveSourceH(sw);

                if (abs(sw->curCanvasW - srcW) > 1 || abs(sw->curCanvasH - srcH) > 1 || sw->isRemotePanel || sw->isDropdownPopup) {
                    SelectClipRgn(hdcDest, NULL);
                    int oldMode = SetStretchBltMode(hdcDest, HALFTONE);
                    SetBrushOrgEx(hdcDest, 0, 0, NULL);
                    StretchBlt(hdcDest, 0, 0, sw->curCanvasW, sw->curCanvasH, sw->hBackDC, 0, 0, srcW, srcH, SRCCOPY);
                    SetStretchBltMode(hdcDest, oldMode);
                    return TRUE;
                }
            }
        }
    }
    return BitBlt(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, rop);
}

static BOOL WINAPI Hook_ScreenToClient(HWND hWnd, LPPOINT lpPoint) {
    BOOL ok = ScreenToClient(hWnd, lpPoint);
    if (ok && lpPoint) {
        ScaledSubWindow* sw = FindByCanvasOrFrame(hWnd);
        if (sw && sw->curCanvasW > 10 && sw->curCanvasH > 10) {
            int srcW = GetEffectiveSourceW(sw);
            int srcH = GetEffectiveSourceH(sw);
            if (abs(sw->curCanvasW - srcW) > 1 || abs(sw->curCanvasH - srcH) > 1) {
                lpPoint->x = (LONG)((double)lpPoint->x * (double)srcW / (double)sw->curCanvasW + 0.5);
                lpPoint->y = (LONG)((double)lpPoint->y * (double)srcH / (double)sw->curCanvasH + 0.5);
            }
        }
    }
    return ok;
}

static BOOL WINAPI Hook_ClientToScreen(HWND hWnd, LPPOINT lpPoint) {
    if (hWnd && lpPoint) {
        ScaledSubWindow* sw = FindByCanvasOrFrame(hWnd);
        if (sw && !sw->isDropdownPopup && sw->curCanvasW > 10 && sw->curCanvasH > 10) {
            g_LastClickedEditor = sw;
            int srcW = GetEffectiveSourceW(sw);
            int srcH = GetEffectiveSourceH(sw);
            if (abs(sw->curCanvasW - srcW) > 1 || abs(sw->curCanvasH - srcH) > 1) {
                POINT scaledPt;
                scaledPt.x = (LONG)((double)lpPoint->x * (double)sw->curCanvasW / (double)srcW + 0.5);
                scaledPt.y = (LONG)((double)lpPoint->y * (double)sw->curCanvasH / (double)srcH + 0.5);
                BOOL ok = ClientToScreen(hWnd, &scaledPt);
                if (ok) *lpPoint = scaledPt;
                return ok;
            }
        }
    }
    return ClientToScreen(hWnd, lpPoint);
}

static LPARAM ScaleMouseLParam(ScaledSubWindow* sw, LPARAM lParam) {
    if (!sw || sw->curCanvasW <= 10 || sw->curCanvasH <= 10) return lParam;
    int srcW = GetEffectiveSourceW(sw);
    int srcH = GetEffectiveSourceH(sw);
    if (abs(sw->curCanvasW - srcW) <= 1 && abs(sw->curCanvasH - srcH) <= 1) return lParam;
    short curX = (short)LOWORD(lParam);
    short curY = (short)HIWORD(lParam);
    short mappedX = (short)((double)curX * (double)srcW / (double)sw->curCanvasW + 0.5);
    short mappedY = (short)((double)curY * (double)srcH / (double)sw->curCanvasH + 0.5);
    return MAKELPARAM(mappedX, mappedY);
}

static void RepaintFromBackbuffer(ScaledSubWindow* sw) {
    if (!sw || !sw->hwndCanvas || !IsWindow(sw->hwndCanvas) || !sw->hBackDC) return;
    ProcessBackbufferPixels(sw);
    HDC hdc = GetDC(sw->hwndCanvas);
    if (hdc) {
        int srcW = GetEffectiveSourceW(sw);
        int srcH = GetEffectiveSourceH(sw);
        SelectClipRgn(hdc, NULL);
        int oldMode = SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, NULL);
        StretchBlt(hdc, 0, 0, sw->curCanvasW, sw->curCanvasH, sw->hBackDC, 0, 0, srcW, srcH, SRCCOPY);
        SetStretchBltMode(hdc, oldMode);
        ReleaseDC(sw->hwndCanvas, hdc);
    }
}

static LRESULT HandleScaledMeasureItem(ScaledSubWindow* sw, HWND hwnd, WNDPROC origProc, WPARAM wParam, LPARAM lParam) {
    LRESULT res = CallWindowProcA(origProc, hwnd, WM_MEASUREITEM, wParam, lParam);
    LPMEASUREITEMSTRUCT lpmis = (LPMEASUREITEMSTRUCT)lParam;
    if (lpmis && sw) {
        double z = sw->currentZoom;
        if (z < 1.0) z = g_CurrentGlobalZoom;
        if (z > 1.02 && lpmis->itemWidth > 0 && lpmis->itemHeight > 0 && lpmis->itemHeight < 80) {
            UINT origW = lpmis->itemWidth;
            UINT origH = lpmis->itemHeight;
            UINT newW = (UINT)(origW * z + 0.5);
            UINT newH = (UINT)(origH * z + 0.5);

            int screenH = GetSystemMetrics(SM_CYSCREEN);
            UINT maxItemH = (UINT)((screenH - 96) / 33);
            if (maxItemH < origH) maxItemH = origH;
            if (newH > maxItemH) newH = maxItemH;

            lpmis->itemWidth = newW;
            lpmis->itemHeight = newH;
        }
    }
    return res;
}

static LRESULT HandleScaledDrawItem(ScaledSubWindow* sw, HWND hwnd, WNDPROC origProc, WPARAM wParam, LPARAM lParam) {
    LPDRAWITEMSTRUCT lpdis = (LPDRAWITEMSTRUCT)lParam;
    if (!lpdis || !lpdis->hDC || !sw) {
        return CallWindowProcA(origProc, hwnd, WM_DRAWITEM, wParam, lParam);
    }

    double z = sw->currentZoom;
    if (z < 1.0) z = g_CurrentGlobalZoom;
    int dstW = lpdis->rcItem.right - lpdis->rcItem.left;
    int dstH = lpdis->rcItem.bottom - lpdis->rcItem.top;

    if (z <= 1.02 || dstW <= 4 || dstH <= 4 || dstW > 800 || dstH > 200) {
        return CallWindowProcA(origProc, hwnd, WM_DRAWITEM, wParam, lParam);
    }

    int srcW = (int)((double)dstW / z + 0.5);
    int srcH = (int)((double)dstH / z + 0.5);
    if (srcW < 16) srcW = 16;
    if (srcH < 11) srcH = 11;
    if (srcH > 16 && dstH <= (int)(16.0 * z + 2.0)) srcH = 14;

    HDC hMemDC = CreateCompatibleDC(lpdis->hDC);
    HBITMAP hMemBmp = CreateCompatibleBitmap(lpdis->hDC, srcW, srcH);
    if (!hMemDC || !hMemBmp) {
        if (hMemBmp) DeleteObject(hMemBmp);
        if (hMemDC) DeleteDC(hMemDC);
        return CallWindowProcA(origProc, hwnd, WM_DRAWITEM, wParam, lParam);
    }

    HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, hMemBmp);
    HFONT hCurFont = (HFONT)GetCurrentObject(lpdis->hDC, OBJ_FONT);
    if (hCurFont) SelectObject(hMemDC, hCurFont);

    DRAWITEMSTRUCT disCopy = *lpdis;
    disCopy.hDC = hMemDC;
    disCopy.rcItem.left = 0;
    disCopy.rcItem.top = 0;
    disCopy.rcItem.right = srcW;
    disCopy.rcItem.bottom = srcH;

    LRESULT res = CallWindowProcA(origProc, hwnd, WM_DRAWITEM, wParam, (LPARAM)&disCopy);

    int oldMode = SetStretchBltMode(lpdis->hDC, HALFTONE);
    SetBrushOrgEx(lpdis->hDC, 0, 0, NULL);
    StretchBlt(lpdis->hDC,
               lpdis->rcItem.left, lpdis->rcItem.top, dstW, dstH,
               hMemDC, 0, 0, srcW, srcH, SRCCOPY);
    SetStretchBltMode(lpdis->hDC, oldMode);

    SelectObject(hMemDC, hOldBmp);
    DeleteObject(hMemBmp);
    DeleteDC(hMemDC);
    return res;
}

static bool IsScaledEditorFrame(HWND hWnd);
static bool TakePendingPlacement(HWND hWnd, POINT* pos);

static BOOL WINAPI Hook_SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags) {
    // Editor being shown for the first time: put it at the position ak.Sys saved for it.
    POINT savedPos;
    if ((uFlags & SWP_SHOWWINDOW) && IsScaledEditorFrame(hWnd) && TakePendingPlacement(hWnd, &savedPos)) {
        DIAG("Restoring saved position (%ld,%ld) on show", savedPos.x, savedPos.y);
        X = savedPos.x;
        Y = savedPos.y;
        uFlags &= ~SWP_NOMOVE;
    }
#ifdef AKSHIM_DIAG
    {
        HWND hFrame = IsScaledEditorFrame(hWnd) ? hWnd : (IsScaledEditorFrame(GetParent(hWnd)) ? GetParent(hWnd) : NULL);
        if (hFrame) {
            char t[128] = {0};
            GetWindowTextA(hFrame, t, sizeof(t) - 1);
            DIAG("SetWindowPos %s '%s' from %s x=%d y=%d cx=%d cy=%d flags=0x%X",
                 hFrame == hWnd ? "frame" : "child", t, DiagCallerName(_ReturnAddress()), X, Y, cx, cy, uFlags);
        }
    }
#endif
    return SetWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

static BOOL WINAPI Hook_MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint) {
#ifdef AKSHIM_DIAG
    {
        HWND hFrame = IsScaledEditorFrame(hWnd) ? hWnd : (IsScaledEditorFrame(GetParent(hWnd)) ? GetParent(hWnd) : NULL);
        if (hFrame) {
            char t[128] = {0};
            GetWindowTextA(hFrame, t, sizeof(t) - 1);
            DIAG("MoveWindow %s '%s' from %s x=%d y=%d w=%d h=%d",
                 hFrame == hWnd ? "frame" : "child", t, DiagCallerName(_ReturnAddress()), X, Y, nWidth, nHeight);
        }
    }
#endif
    return MoveWindow(hWnd, X, Y, nWidth, nHeight, bRepaint);
}

// With "Save editor positions" on, ak.Sys stores each editor's WINDOWPLACEMENT on close. When a
// saved placement exists on the next open, ak.Sys takes a different path: it builds the Program
// Editor off-screen at a fixed 782x143 (top panel only) and then applies the placement, and the
// editor never gets its other panels. Without a saved placement it builds the full 782x585 editor.
// So ak.Sys is told no editor placement is saved (Hook_ReadPlacementFromPrefs, an inline hook:
// the editor frame calls it from inside AKComCtrls.dll, not through an import) and the shim moves
// the editor to the saved position itself as it is shown (Hook_SetWindowPos). Sizes are saved
// unzoomed (Hook_GetWindowPlacement) so the stored values stay sane for older builds too.
#define PLACEMENT_RESTORE_TIMEOUT_MS 60000
#define MAX_PENDING_PLACEMENTS 16

// ak.Sys can read several editors' placements (e.g. reopening editors at startup) before it
// shows any of them, so each one waits in its own slot until its window appears.
struct PendingPlacement {
    char name[128];
    bool hasPos;
    POINT pos;
    bool hasSize;
    SIZE size;  // on-screen client size the window had when it was last saved
    DWORD tick;
};
static PendingPlacement g_PendingPlacements[MAX_PENDING_PLACEMENTS];
static CRITICAL_SECTION g_PendingPlacementLock;
static volatile LONG g_PendingPlacementLockInit = 0;

static void LockPendingPlacements() {
    if (InterlockedCompareExchange(&g_PendingPlacementLockInit, 1, 0) == 0) {
        InitializeCriticalSection(&g_PendingPlacementLock);
        g_PendingPlacementLockInit = 2;
    }
    while (g_PendingPlacementLockInit != 2) Sleep(0);
    EnterCriticalSection(&g_PendingPlacementLock);
}

static void AddPendingPlacement(const char* name, const POINT* pos, const SIZE* size) {
    LockPendingPlacements();
    DWORD now = GetTickCount();
    int slot = -1, oldest = 0;
    for (int i = 0; i < MAX_PENDING_PLACEMENTS; i++) {
        PendingPlacement* p = &g_PendingPlacements[i];
        bool live = p->tick && now - p->tick <= PLACEMENT_RESTORE_TIMEOUT_MS;
        if (live && _stricmp(p->name, name) == 0) { slot = i; break; }
        if (!live && slot < 0) slot = i;
        if (p->tick < g_PendingPlacements[oldest].tick) oldest = i;
    }
    if (slot < 0) slot = oldest;
    PendingPlacement* p = &g_PendingPlacements[slot];
    strncpy(p->name, name, sizeof(p->name) - 1);
    p->hasPos = (pos != NULL);
    if (pos) p->pos = *pos;
    p->hasSize = (size != NULL);
    if (size) p->size = *size;
    p->tick = now;
    LeaveCriticalSection(&g_PendingPlacementLock);
}

// Per-window position and on-screen size, kept by the shim because ak.Sys saves under the
// window's current title ("New Multi  1 [AKAI S6000] [active]  WP") but reads back under the
// plain name ("New Multi  1 WP"), so anything saved while a window was active was never read.
// Stored as "<section>|<plain name> WP" -> {x, y (workspace coords), client cx, client cy}.
#define SHIM_WINDOW_STATE_KEY SHIM_SETTINGS_KEY "\\WindowState"

struct SavedWindowState {
    POINT pos;
    SIZE size;
};

// "New Multi  1 [AKAI S6000] [active]  WP" -> "New Multi  1 WP"
static void NormalizePlacementKey(const char* key, char* out, size_t outSize) {
    char tmp[128];
    strncpy(tmp, key, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char* cut = strstr(tmp, " [");
    if (!cut) cut = strstr(tmp, " WP");
    if (cut) *cut = 0;
    size_t n = strlen(tmp);
    while (n > 0 && tmp[n - 1] == ' ') tmp[--n] = 0;
    snprintf(out, outSize, "%s WP", tmp);
}

static bool LoadSavedWindowState(const char* sec, const char* key, SavedWindowState* st) {
    char plain[128], valName[260];
    NormalizePlacementKey(key, plain, sizeof(plain));
    snprintf(valName, sizeof(valName), "%s|%s", sec, plain);
    HKEY hKey = NULL;
    bool ok = false;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, SHIM_WINDOW_STATE_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        LONG data[4] = {0, 0, 0, 0};
        DWORD type = 0, sz = sizeof(data);
        if (RegQueryValueExA(hKey, valName, NULL, &type, (LPBYTE)data, &sz) == ERROR_SUCCESS &&
            type == REG_BINARY && sz == sizeof(data) && data[2] >= 100 && data[3] >= 40 &&
            data[2] <= 8000 && data[3] <= 5000) {
            st->pos.x = data[0];
            st->pos.y = data[1];
            st->size.cx = data[2];
            st->size.cy = data[3];
            ok = true;
        }
        RegCloseKey(hKey);
    }
    return ok;
}

static void StoreWindowState(const char* sec, const char* key, POINT pos, SIZE size) {
    char plain[128], valName[260];
    NormalizePlacementKey(key, plain, sizeof(plain));
    snprintf(valName, sizeof(valName), "%s|%s", sec, plain);
    HKEY hKey = NULL;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, SHIM_WINDOW_STATE_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        LONG data[4] = { pos.x, pos.y, size.cx, size.cy };
        RegSetValueExA(hKey, valName, 0, REG_BINARY, (const BYTE*)data, sizeof(data));
        RegCloseKey(hKey);
    }
}

// ak.Sys saves a window by calling GetWindowPlacement and then WritePlacementToPrefs on the same
// thread, so the size seen in the first call is stored under the key given to the second.
static bool g_LastPlacementSizeValid = false;
static SIZE g_LastPlacementSize = {0, 0};

typedef int (__cdecl *PFN_ReadPlacementFromPrefs)(void* section, void* key, WINDOWPLACEMENT* wp);
static PFN_ReadPlacementFromPrefs g_realReadPlacementFromPrefs = NULL;

// Both CString arguments are passed by value (one pointer each, to the string data) and are
// destroyed by the callee, so they are forwarded untouched to the real function.
static int __cdecl Hook_ReadPlacementFromPrefs(void* section, void* key, WINDOWPLACEMENT* wp) {
    char sec[128] = {0}, name[128] = {0};
    __try {
        if (section) strncpy(sec, (const char*)section, sizeof(sec) - 1);
        if (key) strncpy(name, (const char*)key, sizeof(name) - 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sec[0] = name[0] = 0;
    }

    if (!g_realReadPlacementFromPrefs) return 0;
    int found = g_realReadPlacementFromPrefs(section, key, wp);
    if (strstr(sec, " WP") && !strstr(name, " WP")) {
        char tmp[128];
        strcpy(tmp, sec); strcpy(sec, name); strcpy(name, tmp);
    }
    DIAG("ReadPlacementFromPrefs called: '%s' / '%s' found=%d", sec, name, found);

    // The main window and editor list stay entirely on ak.Sys's own path.
    if (!wp || _stricmp(sec, "aksys") == 0 || !strstr(name, " WP")) return found;

    SavedWindowState st;
    bool hasState = LoadSavedWindowState(sec, name, &st);
    if (!found && !hasState) return found;

    // "<title> WP" -> "<title>"; matched as a prefix of the window's title.
    char title[128];
    strncpy(title, name, sizeof(title) - 1);
    title[sizeof(title) - 1] = 0;
    char* suffix = strstr(title, " WP");
    if (suffix) *suffix = 0;
    size_t n = strlen(title);
    while (n > 0 && title[n - 1] == ' ') title[--n] = 0;

    // The front panel ("<device> WP") builds fine from a saved placement: ak.Sys restores its
    // position, the shim only restores its size.
    bool isFrontPanel = _stricmp(title, sec) == 0;
    if (isFrontPanel) {
        if (hasState) AddPendingPlacement(title, NULL, &st.size);
        DIAG("ReadPlacementFromPrefs front panel '%s' size %s", title, hasState ? "pending" : "none");
        return found;
    }

    // Prefer the shim's own record: ak.Sys's may be stale (saved under an "[active]" title).
    // Positions are workspace coordinates (relative to the primary monitor's work area);
    // SetWindowPos wants screen coordinates. They differ when the taskbar is top/left.
    POINT pos = hasState ? st.pos : POINT{ wp->rcNormalPosition.left, wp->rcNormalPosition.top };
    RECT rcWork = {0};
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &rcWork, 0)) {
        pos.x += rcWork.left;
        pos.y += rcWork.top;
    }
    AddPendingPlacement(title, &pos, hasState ? &st.size : NULL);
    DIAG("ReadPlacementFromPrefs '%s' / '%s' -> position (%ld,%ld) size %ldx%ld from %s, reported as not saved",
         sec, title, pos.x, pos.y, hasState ? st.size.cx : 0, hasState ? st.size.cy : 0, hasState ? "shim" : "ak.Sys");
    return 0;
}

// Finds the pending entry whose name is the longest prefix of the window's title and takes
// its position (wantSize=false) or size (wantSize=true).
static bool TakePendingEntry(HWND hWnd, bool wantSize, POINT* pos, SIZE* size) {
    char title[128] = {0};
    GetWindowTextA(hWnd, title, sizeof(title) - 1);
    if (!title[0]) return false;
    LockPendingPlacements();
    // Longest matching name wins, so "New Program 1" never grabs "New Program 10"'s slot.
    DWORD now = GetTickCount();
    int best = -1;
    size_t bestLen = 0;
    for (int i = 0; i < MAX_PENDING_PLACEMENTS; i++) {
        PendingPlacement* p = &g_PendingPlacements[i];
        if (!p->tick || now - p->tick > PLACEMENT_RESTORE_TIMEOUT_MS || !p->name[0]) continue;
        if (wantSize ? !p->hasSize : !p->hasPos) continue;
        size_t len = strlen(p->name);
        if (_strnicmp(title, p->name, len) != 0) continue;
        char next = title[len];
        if (next != '\0' && next != ' ') continue;
        if (len > bestLen) { best = i; bestLen = len; }
    }
    if (best >= 0) {
        PendingPlacement* p = &g_PendingPlacements[best];
        if (wantSize) { *size = p->size; p->hasSize = false; }
        else { *pos = p->pos; p->hasPos = false; }
        if (!p->hasPos && !p->hasSize) p->tick = 0;
    }
    LeaveCriticalSection(&g_PendingPlacementLock);
    return best >= 0;
}

static bool TakePendingPlacement(HWND hWnd, POINT* pos) { return TakePendingEntry(hWnd, false, pos, NULL); }
static bool TakePendingSize(HWND hWnd, SIZE* size) { return TakePendingEntry(hWnd, true, NULL, size); }
static bool IsScaledEditorFrame(HWND hWnd) {
    if (!hWnd || GetMenu(hWnd) != NULL) return false;  // main frame has the menu bar
    char clsName[128] = {0};
    char title[128] = {0};
    GetClassNameA(hWnd, clsName, sizeof(clsName) - 1);
    GetWindowTextA(hWnd, title, sizeof(title) - 1);
    return strstr(clsName, "AfxFrameOrView") != NULL && title[0] != '\0';
}

static BOOL WINAPI Hook_GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT* lpwndpl) {
    BOOL ok = GetWindowPlacement(hWnd, lpwndpl);
    ScaledSubWindow* sw = ok && lpwndpl ? FindByFrame(hWnd) : NULL;
    g_LastPlacementSizeValid = false;
    if (sw && sw->initialized && !sw->isDropdownPopup && sw->baseCanvasW > 50 && sw->baseCanvasH > 30) {
        RECT rcCli = {0};
        GetClientRect(hWnd, &rcCli);
        if (rcCli.right >= 100 && rcCli.bottom >= 40 && !IsIconic(hWnd)) {
            g_LastPlacementSize.cx = rcCli.right;
            g_LastPlacementSize.cy = rcCli.bottom;
            g_LastPlacementSizeValid = true;
        }
        int nativeH = GetEffectiveSourceH(sw);
        RECT* rc = &lpwndpl->rcNormalPosition;
        DIAG("GetWindowPlacement '%s' saved (%ld,%ld)-(%ld,%ld) -> native %dx%d",
             sw->isProgramWindow ? "prog" : "other", rc->left, rc->top, rc->right, rc->bottom,
             sw->baseCanvasW + sw->frameNonClientW, nativeH + sw->frameNonClientH);
        rc->right = rc->left + sw->baseCanvasW + sw->frameNonClientW;
        rc->bottom = rc->top + nativeH + sw->frameNonClientH;
    }
    return ok;
}

static BOOL WINAPI Hook_SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT* lpwndpl) {
#ifdef AKSHIM_DIAG
    if (lpwndpl) {
        char t[128] = {0}, c[128] = {0};
        GetWindowTextA(hWnd, t, sizeof(t) - 1);
        GetClassNameA(hWnd, c, sizeof(c) - 1);
        RECT rcCur = {0};
        GetWindowRect(hWnd, &rcCur);
        DIAG("SetWindowPlacement '%s' class=%s menu=%d editor=%d show=%u req=(%ld,%ld)-(%ld,%ld) cur=%ldx%ld",
             t, c, GetMenu(hWnd) != NULL, IsScaledEditorFrame(hWnd), lpwndpl->showCmd,
             lpwndpl->rcNormalPosition.left, lpwndpl->rcNormalPosition.top,
             lpwndpl->rcNormalPosition.right, lpwndpl->rcNormalPosition.bottom,
             rcCur.right - rcCur.left, rcCur.bottom - rcCur.top);
    }
#endif
    if (lpwndpl && IsScaledEditorFrame(hWnd) && !FindByFrame(hWnd)) {
        // The front panel draws its fixed-size panel into whatever size it is restored at and
        // leaves the rest black, so it only gets its position here; the shim restores its
        // on-screen size afterwards.
        char t[128] = {0};
        GetWindowTextA(hWnd, t, sizeof(t) - 1);
        bool isFrontPanel = (_strnicmp(t, "AKAI ", 5) == 0 && !strchr(t, '[')) || _stricmp(t, "MPC4000") == 0;
        if (isFrontPanel) {
            RECT rcCur = {0};
            GetWindowRect(hWnd, &rcCur);
            WINDOWPLACEMENT wp = *lpwndpl;
            wp.rcNormalPosition.right = wp.rcNormalPosition.left + (rcCur.right - rcCur.left);
            wp.rcNormalPosition.bottom = wp.rcNormalPosition.top + (rcCur.bottom - rcCur.top);
            DIAG("SetWindowPlacement front panel '%s': position only, size kept %ldx%ld", t,
                 rcCur.right - rcCur.left, rcCur.bottom - rcCur.top);
            return SetWindowPlacement(hWnd, &wp);
        }

        // Placements saved by older builds hold the zoomed size; the editor's native width is
        // the width ak.Sys just created it at, so a wider saved width means a zoomed save.
        RECT rcCur = {0}, rcCli = {0};
        GetWindowRect(hWnd, &rcCur);
        GetClientRect(hWnd, &rcCli);
        int ncW = (rcCur.right - rcCur.left) - rcCli.right;
        int ncH = (rcCur.bottom - rcCur.top) - rcCli.bottom;
        int nativeCliW = rcCli.right;
        int savedCliW = (lpwndpl->rcNormalPosition.right - lpwndpl->rcNormalPosition.left) - ncW;
        int savedCliH = (lpwndpl->rcNormalPosition.bottom - lpwndpl->rcNormalPosition.top) - ncH;
        if (nativeCliW > 50 && savedCliW > nativeCliW + 24 && savedCliH > 0) {
            double z = (double)savedCliW / (double)nativeCliW;
            // Zoom presets start at 1.25x; smaller ratios are frame/border differences
            // (e.g. the borderless front panel), not a zoomed save.
            if (z >= 1.2 && z <= 4.0) {
                WINDOWPLACEMENT wp = *lpwndpl;
                wp.rcNormalPosition.right = wp.rcNormalPosition.left + nativeCliW + ncW;
                wp.rcNormalPosition.bottom = wp.rcNormalPosition.top + (int)(savedCliH / z + 0.5) + ncH;
                DIAG("SetWindowPlacement unzoom z=%.2f -> client %dx%d", z, nativeCliW, (int)(savedCliH / z + 0.5));
                return SetWindowPlacement(hWnd, &wp);
            }
        }
    }
    return SetWindowPlacement(hWnd, lpwndpl);
}

static void PerformMainThreadZoom(ScaledSubWindow* sw, double zoom) {
    if (!sw || !sw->initialized || !IsWindow(sw->hwnd) || sw->isDropdownPopup) return;
    sw->currentZoom = zoom;
    sw->inZoomResize = true;

    ProcessBackbufferPixels(sw);
    int srcW = GetEffectiveSourceW(sw);
    int srcH = GetEffectiveSourceH(sw);

    int targetCanvasW = (int)(srcW * zoom + 0.5);
    int targetCanvasH = (int)(srcH * zoom + 0.5);
    if (sw->isProgramWindow) DIAG("Zoom prog: z=%.2f src=%dx%d base=%dx%d cropH=%d", zoom, srcW, srcH, sw->baseCanvasW, sw->baseCanvasH, sw->cropH);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    if (sw->isMultiWindow && targetCanvasW > (screenW - 80) && zoom > 1.0) {
        targetCanvasW = screenW - 80;
        if (g_KeepAspectRatio && srcW > 0) {
            targetCanvasH = (int)((double)targetCanvasW * (double)srcH / (double)srcW + 0.5);
        }
    }

    int targetFrameW = targetCanvasW + sw->frameNonClientW;
    int targetFrameH = targetCanvasH + sw->frameNonClientH;

    sw->curCanvasW = targetCanvasW;
    sw->curCanvasH = targetCanvasH;

    StripScrollBars(sw->hwndCanvas);

    SetWindowPos(sw->hwnd, NULL, 0, 0, targetFrameW, targetFrameH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    RECT rcClient = {0};
    GetClientRect(sw->hwnd, &rcClient);
    int cW = rcClient.right - rcClient.left;
    int cH = rcClient.bottom - rcClient.top;
    if (cW > 50 && cH > 30) {
        sw->curCanvasW = cW;
        sw->curCanvasH = cH;
        if (sw->hwndCanvas && IsWindow(sw->hwndCanvas)) {
            StripScrollBars(sw->hwndCanvas);
            SetWindowPos(sw->hwndCanvas, NULL, 0, 0, cW, cH,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }

    UpdateWindowRegionsForScale(sw);

    if (sw->hwndCanvas && IsWindow(sw->hwndCanvas)) {
        InvalidateRect(sw->hwndCanvas, NULL, FALSE);
        RepaintFromBackbuffer(sw);
    }
    InvalidateRect(sw->hwnd, NULL, FALSE);
    sw->inZoomResize = false;
}

// The front panel is clipped to a rounded region and its canvas covers the whole window, so it
// has no sizing border for Windows to find. Treat its right/bottom edges and bottom-right corner
// as sizing areas (the corner zone is large enough to reach inside the rounded region).
#define REMOTE_EDGE_GRIP 10
#define REMOTE_CORNER_GRIP 48

static LRESULT RemotePanelSizingHitTest(ScaledSubWindow* sw, LPARAM lParam) {
    if (!sw || !sw->isRemotePanel || !sw->hwnd) return HTNOWHERE;
    RECT rc = {0};
    GetWindowRect(sw->hwnd, &rc);
    int x = (short)LOWORD(lParam);
    int y = (short)HIWORD(lParam);
    int fromRight = rc.right - x;
    int fromBottom = rc.bottom - y;
    if (fromRight < 0 || fromBottom < 0 || x < rc.left || y < rc.top) return HTNOWHERE;
    if (fromRight < REMOTE_CORNER_GRIP && fromBottom < REMOTE_CORNER_GRIP) return HTBOTTOMRIGHT;
    if (fromRight < REMOTE_EDGE_GRIP) return HTRIGHT;
    if (fromBottom < REMOTE_EDGE_GRIP) return HTBOTTOM;
    return HTNOWHERE;
}

static LRESULT CALLBACK CanvasScaleWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ScaledSubWindow* sw = FindByCanvasOrFrame(hwnd);
    if (!sw || !sw->origCanvasProc) return DefWindowProcA(hwnd, msg, wParam, lParam);
    WNDPROC origProc = sw->origCanvasProc;

    switch (msg) {
        case WM_MEASUREITEM: return HandleScaledMeasureItem(sw, hwnd, origProc, wParam, lParam);
        case WM_DRAWITEM:    return HandleScaledDrawItem(sw, hwnd, origProc, wParam, lParam);
        case WM_NCHITTEST: {
            // Let the frame handle the front panel's sizing edges.
            if (RemotePanelSizingHitTest(sw, lParam) != HTNOWHERE) return HTTRANSPARENT;
            break;
        }
        case WM_WINDOWPOSCHANGING: {
            WINDOWPOS* wp = (WINDOWPOS*)lParam;
            if (wp && !(wp->flags & SWP_NOSIZE) && !sw->inZoomResize && !sw->isRemotePanel) {
                if (sw->isProgramWindow && wp->cy > 100 && wp->cy <= 900) {
                    DIAG("Canvas WPCHANGING prog: req %dx%d cropH %d -> %d", wp->cx, wp->cy, sw->cropH, wp->cy);
                    if (sw->hBackDC && wp->cy < sw->cropH) {
                        RECT rcClear = { 0, wp->cy, sw->baseCanvasW, sw->baseCanvasH };
                        FillRect(sw->hBackDC, &rcClear, (HBRUSH)GetStockObject(BLACK_BRUSH));
                    }
                    if (wp->cy > sw->baseCanvasH) sw->baseCanvasH = wp->cy;
                    sw->cropH = wp->cy;
                    wp->cx = (int)(sw->baseCanvasW * sw->currentZoom + 0.5);
                    wp->cy = (int)(sw->cropH * sw->currentZoom + 0.5);
                    sw->curCanvasW = wp->cx;
                    sw->curCanvasH = wp->cy;
                    PostMessageA(sw->hwnd, WM_AKAI_APPLY_ZOOM, (WPARAM)(int)(sw->currentZoom * 100.0 + 0.5), 0);
                }
            }
            break;
        }
        case WM_SIZE: {
            StripScrollBars(hwnd);
            if (sw->baseCanvasW > 50 && sw->baseCanvasH > 30) {
                LRESULT sizeRes = CallWindowProcA(origProc, hwnd, msg, wParam, MAKELPARAM(sw->baseCanvasW, sw->baseCanvasH));
                if (sw->isEb20Window) StripScrollBars(hwnd);
                return sizeRes;
            }
            break;
        }
        case WM_ERASEBKGND: {
            if (sw->hBackDC) {
                RepaintFromBackbuffer(sw);
                return 1;
            }
            break;
        }
        case WM_PAINT: {
            StripScrollBars(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            LRESULT res = CallWindowProcA(origProc, hwnd, msg, wParam, lParam);
            if (sw->hBackDC) RepaintFromBackbuffer(sw);
            return res;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEMOVE: {
            g_LastClickedEditor = sw;
            lParam = ScaleMouseLParam(sw, lParam);
            LRESULT res = CallWindowProcA(origProc, hwnd, msg, wParam, lParam);
            if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || (msg == WM_MOUSEMOVE && (wParam & MK_LBUTTON))) {
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return res;
        }
        case WM_NCDESTROY: {
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)origProc);
            sw->hwndCanvas = NULL;
            return CallWindowProcA(origProc, hwnd, msg, wParam, lParam);
        }
    }
    return CallWindowProcA(origProc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK FrameScaleWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ScaledSubWindow* sw = FindByFrame(hwnd);
    if (!sw || !sw->origFrameProc) return DefWindowProcA(hwnd, msg, wParam, lParam);
    WNDPROC origProc = sw->origFrameProc;

    switch (msg) {
        case WM_MEASUREITEM: return HandleScaledMeasureItem(sw, hwnd, origProc, wParam, lParam);
        case WM_DRAWITEM:    return HandleScaledDrawItem(sw, hwnd, origProc, wParam, lParam);
        case WM_AKAI_APPLY_ZOOM: {
            double z = (double)wParam / 100.0;
            if (z >= 0.5 && z <= 4.0 && !sw->closing) PerformMainThreadZoom(sw, z);
            return 0;
        }
        case WM_CLOSE:
        case WM_DESTROY: {
            DIAG("Frame %s '%s'", msg == WM_CLOSE ? "WM_CLOSE" : "WM_DESTROY",
                 sw->isMultiWindow ? "multi" : (sw->isProgramWindow ? "prog" : "other"));
            // WM_CLOSE can still be cancelled (e.g. a "save changes?" prompt); WM_DESTROY can't.
            if (msg == WM_DESTROY) sw->closing = true;
            break;
        }
        case WM_NCHITTEST: {
            LRESULT ht = RemotePanelSizingHitTest(sw, lParam);
            if (ht != HTNOWHERE) return ht;
            break;
        }
        case WM_WINDOWPOSCHANGING: {
            WINDOWPOS* wp = (WINDOWPOS*)lParam;
            if (wp && !(wp->flags & SWP_NOSIZE) && !sw->inZoomResize && sw->isProgramWindow) {
                int reqClientW = wp->cx - sw->frameNonClientW;
                int reqClientH = wp->cy - sw->frameNonClientH;
                DIAG("Frame WPCHANGING prog: reqClient %dx%d base=%dx%d cropH=%d match=%d", reqClientW, reqClientH,
                     sw->baseCanvasW, sw->baseCanvasH, sw->cropH,
                     (abs(reqClientW - sw->baseCanvasW) <= 24 && reqClientH >= 96 && reqClientH <= 900));
                if (abs(reqClientW - sw->baseCanvasW) <= 24 && reqClientH >= 96 && reqClientH <= 900) {
                    if (sw->hBackDC && reqClientH < sw->cropH) {
                        RECT rcClear = { 0, reqClientH, sw->baseCanvasW, sw->baseCanvasH };
                        FillRect(sw->hBackDC, &rcClear, (HBRUSH)GetStockObject(BLACK_BRUSH));
                    }
                    if (reqClientH > sw->baseCanvasH) sw->baseCanvasH = reqClientH;
                    sw->cropH = reqClientH;
                    wp->cx = (int)(sw->baseCanvasW * sw->currentZoom + 0.5) + sw->frameNonClientW;
                    wp->cy = (int)(sw->cropH * sw->currentZoom + 0.5) + sw->frameNonClientH;
                }
            }
            break;
        }
        case WM_NCLBUTTONDOWN: {
            // With the aspect lock the window edge lags the mouse, so dragging an edge to the
            // screen edge triggers Windows' "maximise vertically" snap preview. Snap is only offered
            // to windows with a maximise box, and Windows decides when the resize starts, so drop
            // the box before the resize loop begins and restore it on WM_EXITSIZEMOVE.
            if (g_KeepAspectRatio && !sw->snapSuppressed && wParam >= HTLEFT && wParam <= HTBOTTOMRIGHT) {
                LONG style = GetWindowLongA(hwnd, GWL_STYLE);
                if (style & WS_MAXIMIZEBOX) {
                    SetWindowLongA(hwnd, GWL_STYLE, style & ~WS_MAXIMIZEBOX);
                    sw->snapSuppressed = true;
                }
            }
            break;
        }
        case WM_SIZING: {
            if (g_KeepAspectRatio && lParam) {
                RECT* rc = (RECT*)lParam;
                int srcW = GetEffectiveSourceW(sw);
                int srcH = GetEffectiveSourceH(sw);
                if (srcW > 50 && srcH > 30) {
                    double aspect = (double)srcW / (double)srcH;
                    int curClientW = (rc->right - rc->left) - sw->frameNonClientW;
                    int curClientH = (rc->bottom - rc->top) - sw->frameNonClientH;
                    if (curClientW < 120) curClientW = 120;
                    if (curClientH < 60)  curClientH = 60;

                    if (wParam == WMSZ_LEFT || wParam == WMSZ_RIGHT ||
                        wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT ||
                        wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_BOTTOMRIGHT) {
                        int newClientH = (int)((double)curClientW / aspect + 0.5);
                        int newFrameH = newClientH + sw->frameNonClientH;
                        if (wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT) {
                            rc->top = rc->bottom - newFrameH;
                        } else {
                            rc->bottom = rc->top + newFrameH;
                        }
                    } else if (wParam == WMSZ_TOP || wParam == WMSZ_BOTTOM) {
                        int newClientW = (int)((double)curClientH * aspect + 0.5);
                        int newFrameW = newClientW + sw->frameNonClientW;
                        rc->right = rc->left + newFrameW;
                    }
                    return TRUE;
                }
            }
            break;
        }
        case WM_EXITSIZEMOVE: {
            if (sw->snapSuppressed) {
                SetWindowLongA(hwnd, GWL_STYLE, GetWindowLongA(hwnd, GWL_STYLE) | WS_MAXIMIZEBOX);
                sw->snapSuppressed = false;
            }
            break;
        }
        case WM_GETMINMAXINFO: {
            LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
            if (mmi) {
                mmi->ptMinTrackSize.x = 200;
                mmi->ptMinTrackSize.y = 80;
                mmi->ptMaxTrackSize.x = 7680;
                mmi->ptMaxTrackSize.y = 4320;
                mmi->ptMaxSize.x = 7680;
                mmi->ptMaxSize.y = 4320;
                // With the aspect lock, no height beyond what the monitor's width allows at the
                // window's shape is reachable, so don't let Windows offer one (snap previews).
                int srcW = GetEffectiveSourceW(sw);
                int srcH = GetEffectiveSourceH(sw);
                MONITORINFO mi;
                mi.cbSize = sizeof(mi);
                if (g_KeepAspectRatio && srcW > 50 && srcH > 30 &&
                    GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                    int maxClientW = (mi.rcWork.right - mi.rcWork.left) - sw->frameNonClientW;
                    int maxFrameH = (int)((double)maxClientW * srcH / srcW + 0.5) + sw->frameNonClientH;
                    if (maxFrameH > mmi->ptMinTrackSize.y && maxFrameH < mmi->ptMaxTrackSize.y) {
                        mmi->ptMaxTrackSize.y = maxFrameH;
                    }
                }
                return 0;
            }
            break;
        }
        case WM_SIZE: {
            if (wParam != SIZE_MINIMIZED) {
                RECT rcClient = {0};
                GetClientRect(hwnd, &rcClient);
                int cW = rcClient.right - rcClient.left;
                int cH = rcClient.bottom - rcClient.top;
                if (cW > 50 && cH > 30) {
                    sw->curCanvasW = cW;
                    sw->curCanvasH = cH;
                    int srcW = GetEffectiveSourceW(sw);
                    if (srcW > 50) sw->currentZoom = (double)cW / (double)srcW;
                    if (sw->hwndCanvas && IsWindow(sw->hwndCanvas)) {
                        sw->inZoomResize = true;
                        StripScrollBars(sw->hwndCanvas);
                        SetWindowPos(sw->hwndCanvas, NULL, 0, 0, cW, cH,
                                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                        sw->inZoomResize = false;
                        UpdateWindowRegionsForScale(sw);
                        InvalidateRect(sw->hwndCanvas, NULL, FALSE);
                        RepaintFromBackbuffer(sw);
                    }
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEMOVE: {
            g_LastClickedEditor = sw;
            lParam = ScaleMouseLParam(sw, lParam);
            return CallWindowProcA(origProc, hwnd, msg, wParam, lParam);
        }
        case WM_NCDESTROY: {
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)origProc);
            if (sw->hNativeBaseRgn) {
                DeleteObject(sw->hNativeBaseRgn);
                sw->hNativeBaseRgn = NULL;
            }
            if (sw->hBackBmp) {
                DeleteObject(sw->hBackBmp);
                sw->hBackBmp = NULL;
            }
            if (sw->hBackDC) {
                DeleteDC(sw->hBackDC);
                sw->hBackDC = NULL;
            }
            sw->pBackPixels = NULL;
            sw->hwnd = NULL;
            sw->initialized = false;
            return CallWindowProcA(origProc, hwnd, msg, wParam, lParam);
        }
    }
    return CallWindowProcA(origProc, hwnd, msg, wParam, lParam);
}

struct EnumScaleCtx {
    double zoom;
    bool forceApplyZoom;
};

static void EnsureWritableWorkingDirectoryAndDragDropFilters() {
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *PFN_ChangeWindowMessageFilter)(UINT, DWORD);
        PFN_ChangeWindowMessageFilter pfnFilter = (PFN_ChangeWindowMessageFilter)GetProcAddress(hUser32, "ChangeWindowMessageFilter");
        if (pfnFilter) {
            pfnFilter(0x0233 /* WM_DROPFILES */, 1);
            pfnFilter(0x0049 /* WM_COPYGLOBALDATA */, 1);
            pfnFilter(0x004A /* WM_COPYDATA */, 1);
        }
    }
}

static void RegisterAfxFrameWindow(HWND hwnd, double currentZoom, bool forceApplyZoom) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return;

    char clsName[128] = {0};
    char title[128] = {0};
    GetClassNameA(hwnd, clsName, sizeof(clsName) - 1);
    GetWindowTextA(hwnd, title, sizeof(title) - 1);

    if (strstr(clsName, "AfxFrameOrView") == NULL || title[0] == '\0') return;

    ScaledSubWindow* existing = FindByFrame(hwnd);
    if (existing) {
        if (forceApplyZoom && currentZoom >= 0.5 && !existing->isDropdownPopup) {
            DWORD_PTR res = 0;
            SendMessageTimeoutA(hwnd, WM_AKAI_APPLY_ZOOM, (WPARAM)(int)(currentZoom * 100.0 + 0.5), 0,
                                SMTO_ABORTIFHUNG | SMTO_NORMAL, 250, &res);
        }
        return;
    }

    RECT rcWin = {0}, rcClient = {0};
    GetWindowRect(hwnd, &rcWin);
    GetClientRect(hwnd, &rcClient);
    int winW = rcWin.right - rcWin.left;
    int winH = rcWin.bottom - rcWin.top;
    int clientW = rcClient.right - rcClient.left;
    int clientH = rcClient.bottom - rcClient.top;
    if (winW < 100 || winH < 40) return;

    // Fallback for editors shown without SetWindowPos(SWP_SHOWWINDOW): apply the saved position now.
    POINT savedPos;
    if (TakePendingPlacement(hwnd, &savedPos)) {
        DIAG("Restoring saved position (%ld,%ld) on register", savedPos.x, savedPos.y);
        SetWindowPos(hwnd, NULL, savedPos.x, savedPos.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    char lowerTitle[128] = {0};
    strncpy(lowerTitle, title, sizeof(lowerTitle) - 1);
    _strlwr(lowerTitle);

    bool isS56Remote = (strcmp(title, "AKAI S6000") == 0 || strcmp(title, "AKAI S5000") == 0);
    bool isZ48OrMpcRemote = (!isS56Remote &&
                             (strcmp(title, "AKAI Z4") == 0 || strcmp(title, "AKAI Z8") == 0 ||
                              strcmp(title, "AKAI MPC4000") == 0 || strcmp(title, "MPC4000") == 0 ||
                              strstr(lowerTitle, "remote") != NULL || strstr(lowerTitle, "panel") != NULL));
    bool isRemote = (isS56Remote || isZ48OrMpcRemote);
    bool isEb20 = (!isRemote && (strstr(lowerTitle, "eb20") != NULL || strstr(lowerTitle, "effect") != NULL ||
                                 strstr(lowerTitle, "fx") != NULL || strstr(lowerTitle, "reverb") != NULL));
    bool isMulti = (!isRemote && !isEb20 &&
                    (strstr(lowerTitle, "multi") != NULL || strstr(lowerTitle, "mix") != NULL || winW > 1000));
    bool isProg = (!isRemote && !isEb20 && !isMulti);

    HWND hCanvas = NULL;
    HWND hChild = GetWindow(hwnd, GW_CHILD);
    while (hChild) {
        char cCls[64] = {0};
        GetClassNameA(hChild, cCls, sizeof(cCls) - 1);
        if (strstr(cCls, "Afx:") && !hCanvas) hCanvas = hChild;
        hChild = GetWindow(hChild, GW_HWNDNEXT);
    }

    // Borderless windows (the front panel) really have no non-client area.
    int nonClientW = (winW >= clientW) ? (winW - clientW) : 16;
    int nonClientH = (winH >= clientH) ? (winH - clientH) : 39;

    int trueBaseW = clientW;
    int trueBaseH = clientH;
    if (isS56Remote) {
        trueBaseW = 540;
        trueBaseH = 368;
    } else if (isZ48OrMpcRemote) {
        trueBaseW = (clientW > 100) ? clientW : 540;
        trueBaseH = (clientH > 80)  ? clientH : 368;
    } else if (isMulti) {
        trueBaseW = (clientW > 800) ? clientW : 1600;
        trueBaseH = (clientH > 200) ? clientH : 522;
    } else if (isEb20) {
        trueBaseW = (clientW > 200) ? clientW : 650;

        // The EB20 rack is taller than the window the app first opens it in (the app
        // relies on a vertical scrollbar), so clientH cuts off the bottom row of knobs.
        // Read the real content height from the scroll range; fall back to 4 racks x 88px.
        int ebContentH = 0;
        HWND ebScrollCands[2] = { hCanvas, hwnd };
        for (int ebi = 0; ebi < 2 && ebContentH == 0; ebi++) {
            HWND hs = ebScrollCands[ebi];
            if (!hs) continue;
            SCROLLINFO ebSi;
            memset(&ebSi, 0, sizeof(ebSi));
            ebSi.cbSize = sizeof(ebSi);
            ebSi.fMask = SIF_RANGE | SIF_PAGE;
            if (!GetScrollInfo(hs, SB_VERT, &ebSi)) continue;
            int ebRange = ebSi.nMax - ebSi.nMin;
            if (ebRange <= 0) continue;
            RECT rcView = {0};
            GetClientRect(hs, &rcView);
            int ebTotal = (ebSi.nPage > 0) ? (ebRange + 1) : ((rcView.bottom - rcView.top) + ebRange);
            if (ebTotal >= 150 && ebTotal <= 1200) ebContentH = ebTotal;
        }
        trueBaseH = (ebContentH >= 150) ? ebContentH : 352;
    } else {
        trueBaseW = (clientW > 400 && abs(clientW - 766) > 32) ? clientW : 766;
        trueBaseH = (clientH > 546) ? clientH : 546;
    }

    int slot = -1;
    for (int i = 0; i < MAX_SCALED_WINDOWS; i++) {
        if (!g_ScaledWins[i].initialized || !IsWindow(g_ScaledWins[i].hwnd)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return;

    ScaledSubWindow* sw = &g_ScaledWins[slot];
    memset(sw, 0, sizeof(ScaledSubWindow));
    sw->hwnd = hwnd;
    sw->hwndCanvas = hCanvas;
    sw->isRemotePanel = isRemote;
    sw->isZ48OrMpcRemote = isZ48OrMpcRemote;
    sw->isMultiWindow = isMulti;
    sw->isProgramWindow = isProg;
    sw->isEb20Window = isEb20;
    sw->isDropdownPopup = false;
    sw->frameNonClientW = nonClientW;
    sw->frameNonClientH = nonClientH;
    sw->baseCanvasW = trueBaseW;
    sw->baseCanvasH = trueBaseH;
    sw->cropW = trueBaseW;
    sw->cropH = (isProg && clientH < 500) ? clientH : trueBaseH;
    DIAG("Register '%s' prog=%d remote=%d multi=%d eb20=%d win=%dx%d client=%dx%d base=%dx%d cropH=%d zoom=%.2f",
         title, isProg, isRemote, isMulti, isEb20, winW, winH, clientW, clientH, trueBaseW, trueBaseH, sw->cropH, currentZoom);
    sw->curCanvasW = clientW;
    sw->curCanvasH = clientH;
    sw->currentZoom = 1.0;
    sw->inZoomResize = false;
    sw->discoveredTick = GetTickCount();
    sw->hNativeBaseRgn = NULL;
    sw->hBackDC = NULL;
    sw->hBackBmp = NULL;
    sw->pBackPixels = NULL;
    sw->backW = 0;
    sw->backH = 0;
    sw->initialized = true;
    g_LastClickedEditor = sw;

    if (hCanvas) {
        StripScrollBars(hCanvas);
        HDC hdcWin = GetDC(hCanvas);
        if (hdcWin) {
            EnsureBackbuffer(sw, hdcWin);
            if (sw->hBackDC) {
                BitBlt(sw->hBackDC, 0, 0, sw->baseCanvasW, sw->baseCanvasH, hdcWin, 0, 0, SRCCOPY);
                ProcessBackbufferPixels(sw);
            }
            ReleaseDC(hCanvas, hdcWin);
        }
    }

    LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    if (isRemote) {
        SetWindowLongA(hwnd, GWL_STYLE, style | WS_THICKFRAME);
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, style | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
    }

    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *PFN_ChangeWindowMessageFilterEx)(HWND, UINT, DWORD, PVOID);
        PFN_ChangeWindowMessageFilterEx pfnEx = (PFN_ChangeWindowMessageFilterEx)GetProcAddress(hUser32, "ChangeWindowMessageFilterEx");
        if (pfnEx) {
            pfnEx(hwnd, 0x0233 /* WM_DROPFILES */, 1, NULL);
            pfnEx(hwnd, 0x0049 /* WM_COPYGLOBALDATA */, 1, NULL);
            pfnEx(hwnd, 0x004A /* WM_COPYDATA */, 1, NULL);
            if (hCanvas) {
                pfnEx(hCanvas, 0x0233, 1, NULL);
                pfnEx(hCanvas, 0x0049, 1, NULL);
                pfnEx(hCanvas, 0x004A, 1, NULL);
            }
        }
    }

    sw->origFrameProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)FrameScaleWndProc);
    if (hCanvas) {
        sw->origCanvasProc = (WNDPROC)SetWindowLongPtrA(hCanvas, GWLP_WNDPROC, (LONG_PTR)CanvasScaleWndProc);
    }

    double initialZoom = (currentZoom >= 1.0) ? currentZoom : 1.0;
    DWORD_PTR res = 0;
    SendMessageTimeoutA(hwnd, WM_AKAI_APPLY_ZOOM, (WPARAM)(int)(initialZoom * 100.0 + 0.5), 0,
                        SMTO_ABORTIFHUNG | SMTO_NORMAL, 250, &res);

    // Give the window back the on-screen size it had when it was last closed (the frame's
    // WM_SIZE handler derives the window's zoom from it).
    SIZE savedSize;
    if (TakePendingSize(hwnd, &savedSize)) {
        RECT rcW = {0}, rcC = {0};
        GetWindowRect(hwnd, &rcW);
        GetClientRect(hwnd, &rcC);
        int ncW = (rcW.right - rcW.left) - rcC.right;
        int ncH = (rcW.bottom - rcW.top) - rcC.bottom;
        DIAG("Restoring saved size %ldx%ld for '%s'", savedSize.cx, savedSize.cy, title);
        SetWindowPos(hwnd, NULL, 0, 0, savedSize.cx + ncW, savedSize.cy + ncH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static BOOL CALLBACK EnumChildProc(HWND hwndChild, LPARAM lParam) {
    EnumScaleCtx* ctx = (EnumScaleCtx*)lParam;
    RegisterAfxFrameWindow(hwndChild, ctx->zoom, ctx->forceApplyZoom);
    return TRUE;
}

static BOOL CALLBACK EnumTopWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == g_MainProcessId) {
        EnumScaleCtx* ctx = (EnumScaleCtx*)lParam;
        RegisterAfxFrameWindow(hwnd, ctx->zoom, ctx->forceApplyZoom);
        EnumChildWindows(hwnd, EnumChildProc, lParam);
    }
    return TRUE;
}

static void ScanAndScaleAllSubWindows(double requestedZoom, bool forceApplyZoom) {
    EnumScaleCtx ctx;
    ctx.zoom = requestedZoom;
    ctx.forceApplyZoom = forceApplyZoom;
    EnumWindows(EnumTopWindowsProc, (LPARAM)&ctx);
}

#define ID_BTN_ZOOM_100 9001
#define ID_BTN_ZOOM_125 9002
#define ID_BTN_ZOOM_150 9003
#define ID_BTN_ZOOM_175 9004
#define ID_BTN_ZOOM_200 9005
#define ID_CHK_ASPECT   9006

static LRESULT CALLBACK ZoomToolbarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SETCURSOR: {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == ID_CHK_ASPECT) {
                LRESULT chk = SendMessageA((HWND)lParam, BM_GETCHECK, 0, 0);
                g_KeepAspectRatio = (chk == BST_CHECKED);
                WriteShimSettingDword("KeepAspectRatio", g_KeepAspectRatio ? 1 : 0);
                if (g_KeepAspectRatio) {
                    ScanAndScaleAllSubWindows(g_CurrentGlobalZoom, true);
                }
                return 0;
            }

            double zoom = 1.0;
            switch (wmId) {
                case ID_BTN_ZOOM_100: zoom = 1.0; break;
                case ID_BTN_ZOOM_125: zoom = 1.25; break;
                case ID_BTN_ZOOM_150: zoom = 1.5; break;
                case ID_BTN_ZOOM_175: zoom = 1.75; break;
                case ID_BTN_ZOOM_200: zoom = 2.0; break;
                default: return 0;
            }
            g_CurrentGlobalZoom = zoom;
            WriteShimSettingDword("ZoomPercent", (DWORD)(zoom * 100.0 + 0.5));
            ScanAndScaleAllSubWindows(zoom, true);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH hBr = CreateSolidBrush(RGB(240, 244, 248));
            FillRect(hdc, &rc, hBr);
            DeleteObject(hBr);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(15, 23, 42));
            HFONT hOldFont = (HFONT)SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
            TextOutA(hdc, 10, 14, "UI Scale:", 9);
            SelectObject(hdc, hOldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE: {
            g_ToolbarClosedByUser = true;
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI ZoomToolbarUIThread(LPVOID) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = ZoomToolbarWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "AkaiZoomBarClassV156";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);

    g_hToolbarWidget = CreateWindowExA(WS_EX_TOPMOST,
                                       "AkaiZoomBarClassV156",
                                       "ak.sys S5000/S6000/Z4/Z8/MPC4000 Zoom (v3.1.4)",
                                       WS_POPUP | WS_CAPTION | WS_VISIBLE | WS_BORDER | WS_SYSMENU | WS_MINIMIZEBOX,
                                       140, 90, 410, 106,
                                       NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!g_hToolbarWidget) return 0;

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    struct { int id; const char* text; int x; } btns[] = {
        { ID_BTN_ZOOM_100, "1.0x", 75 },
        { ID_BTN_ZOOM_125, "1.25x", 135 },
        { ID_BTN_ZOOM_150, "1.5x", 195 },
        { ID_BTN_ZOOM_175, "1.75x", 255 },
        { ID_BTN_ZOOM_200, "2.0x", 315 }
    };

    for (int i = 0; i < 5; i++) {
        HWND hBtn = CreateWindowExA(0, "BUTTON", btns[i].text,
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                    btns[i].x, 8, 54, 26,
                                    g_hToolbarWidget, (HMENU)(INT_PTR)btns[i].id,
                                    GetModuleHandleA(NULL), NULL);
        if (hBtn && hFont) {
            SendMessageA(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
            ShowWindow(hBtn, SW_SHOW);
            UpdateWindow(hBtn);
        }
    }

    HWND hChk = CreateWindowExA(0, "BUTTON", "Lock Original Aspect Ratio When Resizing",
                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                                12, 40, 370, 20,
                                g_hToolbarWidget, (HMENU)(INT_PTR)ID_CHK_ASPECT,
                                GetModuleHandleA(NULL), NULL);
    if (hChk) {
        if (hFont) SendMessageA(hChk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageA(hChk, BM_SETCHECK, g_KeepAspectRatio ? BST_CHECKED : BST_UNCHECKED, 0);
        ShowWindow(hChk, SW_SHOW);
        UpdateWindow(hChk);
    }

    ShowWindow(g_hToolbarWidget, SW_SHOW);
    UpdateWindow(g_hToolbarWidget);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    g_hToolbarWidget = NULL;
    return 0;
}

static void EnsureZoomToolbarRunning() {
    if (g_ToolbarClosedByUser) return;
    if (g_hToolbarWidget && IsWindow(g_hToolbarWidget)) return;
    HANDLE hThread = CreateThread(NULL, 0, ZoomToolbarUIThread, NULL, 0, NULL);
    if (hThread) CloseHandle(hThread);
}

static void* ResolveHookFunction(const char* fn) {
    if (strcmp(fn, "DoDragDrop") == 0) return (void*)Hook_DoDragDrop;
    if (strcmp(fn, "CreateDirectoryA") == 0) return (void*)Hook_CreateDirectoryA;
    if (strcmp(fn, "CreateDirectoryW") == 0) return (void*)Hook_CreateDirectoryW;
    if (strcmp(fn, "CreateFileA") == 0) return (void*)Hook_CreateFileA;
    if (strcmp(fn, "CreateFileW") == 0) return (void*)Hook_CreateFileW;
    if (strcmp(fn, "MoveFileA") == 0) return (void*)Hook_MoveFileA;
    if (strcmp(fn, "MoveFileW") == 0) return (void*)Hook_MoveFileW;
    if (strcmp(fn, "CopyFileA") == 0) return (void*)Hook_CopyFileA;
    if (strcmp(fn, "CopyFileW") == 0) return (void*)Hook_CopyFileW;
    if (strcmp(fn, "ReadFile") == 0) return (void*)Hook_ReadFile;
    if (strcmp(fn, "WriteFile") == 0) return (void*)Hook_WriteFile;
    if (strcmp(fn, "DeviceIoControl") == 0) return (void*)Hook_DeviceIoControl;
    if (strcmp(fn, "GetOverlappedResult") == 0) return (void*)Hook_GetOverlappedResult;
    if (strcmp(fn, "CancelIo") == 0) return (void*)Hook_CancelIo;
    if (strcmp(fn, "CloseHandle") == 0) return (void*)Hook_CloseHandle;
    if (strcmp(fn, "SetupDiGetClassDevsA") == 0) return (void*)Hook_SetupDiGetClassDevsA;
    if (strcmp(fn, "SetupDiEnumDeviceInterfaces") == 0) return (void*)Hook_SetupDiEnumDeviceInterfaces;
    if (strcmp(fn, "SetupDiGetDeviceInterfaceDetailA") == 0) return (void*)Hook_SetupDiGetDeviceInterfaceDetailA;
    if (strcmp(fn, "SetupDiDestroyDeviceInfoList") == 0) return (void*)Hook_SetupDiDestroyDeviceInfoList;
    if (strcmp(fn, "BitBlt") == 0) return (void*)Hook_BitBlt;
    if (strcmp(fn, "SetWindowRgn") == 0) return (void*)Hook_SetWindowRgn;
    if (strcmp(fn, "ScreenToClient") == 0) return (void*)Hook_ScreenToClient;
    if (strcmp(fn, "ClientToScreen") == 0) return (void*)Hook_ClientToScreen;
    if (strcmp(fn, "SetWindowPos") == 0) return (void*)Hook_SetWindowPos;
    if (strcmp(fn, "SetWindowPlacement") == 0) return (void*)Hook_SetWindowPlacement;
    if (strcmp(fn, "GetWindowPlacement") == 0) return (void*)Hook_GetWindowPlacement;
    if (strcmp(fn, "ShellExecuteA") == 0) return (void*)Hook_ShellExecuteA;
    if (strcmp(fn, "FindFirstChangeNotificationA") == 0) return (void*)Hook_FindFirstChangeNotificationA;
    if (strcmp(fn, "DeleteFileA") == 0) return (void*)Hook_DeleteFileA;
    if (strcmp(fn, "GetFileAttributesA") == 0) return (void*)Hook_GetFileAttributesA;
    if (strcmp(fn, "GetFileAttributesExA") == 0) return (void*)Hook_GetFileAttributesExA;
    if (strcmp(fn, "SetFileAttributesA") == 0) return (void*)Hook_SetFileAttributesA;
    if (strcmp(fn, "RemoveDirectoryA") == 0) return (void*)Hook_RemoveDirectoryA;
    if (strcmp(fn, "FindFirstFileA") == 0) return (void*)Hook_FindFirstFileA;
#ifdef AKSHIM_DIAG
    if (strcmp(fn, "MoveWindow") == 0) return (void*)Hook_MoveWindow;
#endif
    if (strcmp(fn, "GetClientRect") == 0) return (void*)Hook_GetClientRect;
    if (strcmp(fn, "InvalidateRect") == 0) return (void*)Hook_InvalidateRect;
    return NULL;
}

static void PatchModuleIAT(HMODULE hMod, const char* modName) {
    if (!hMod || hMod == g_hShimModule) return;

    __try {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hMod;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hMod + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;

        DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!impRva) return;
        PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hMod + impRva);

        int count = 0;
        for (; imp->Name; imp++) {
            PIMAGE_THUNK_DATA thunk = (PIMAGE_THUNK_DATA)((BYTE*)hMod + imp->FirstThunk);
            PIMAGE_THUNK_DATA origThunk = imp->OriginalFirstThunk ?
                                          (PIMAGE_THUNK_DATA)((BYTE*)hMod + imp->OriginalFirstThunk) : thunk;

            for (; thunk->u1.AddressOfData; thunk++, origThunk++) {
                if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
                PIMAGE_IMPORT_BY_NAME name = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hMod + origThunk->u1.AddressOfData);
                void* rep = ResolveHookFunction((const char*)name->Name);
                if (rep && thunk->u1.Function != (ULONG_PTR)rep) {
                    DWORD old;
                    if (VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), PAGE_EXECUTE_READWRITE, &old)) {
                        thunk->u1.Function = (ULONG_PTR)rep;
                        VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), old, &old);
                        count++;
                    }
                }
            }
        }
        if (count > 0 && modName) {
            if (strstr(modName, "S56") || strstr(modName, "s56") ||
                strstr(modName, "Z48") || strstr(modName, "z48") ||
                strstr(modName, "MPC") || strstr(modName, "mpc")) {
                g_DeviceDllHooked = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ScanAndPatchAllLoadedModules() {
    const char* knownModules[] = {
        "S56Device.dll",
        "devices\\S56Device.dll",
        "C:\\Program Files (x86)\\AKAIPRO\\aksys\\devices\\S56Device.dll",
        "Z48Device.dll",
        "devices\\Z48Device.dll",
        "C:\\Program Files (x86)\\AKAIPRO\\aksys\\devices\\Z48Device.dll",
        "MPC4000Device.dll",
        "devices\\MPC4000Device.dll",
        "MPC4KDevice.dll",
        "devices\\MPC4KDevice.dll",
        "DPS24Device.dll",
        "devices\\DPS24Device.dll",
        "AKComCtrls.dll",
        "AKSys.exe",
        "aksys.exe",
        "MFC42.DLL",
        "mfc42.dll"
    };
    for (int i = 0; i < (int)(sizeof(knownModules) / sizeof(knownModules[0])); i++) {
        HMODULE h = GetModuleHandleA(knownModules[i]);
        if (h) PatchModuleIAT(h, knownModules[i]);
    }

    char exePath[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH - 1)) {
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash) {
            *lastSlash = '\0';
            char searchPattern[MAX_PATH];
            snprintf(searchPattern, sizeof(searchPattern), "%s\\devices\\*.dll", exePath);
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(searchPattern, &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    HMODULE hDevDll = GetModuleHandleA(fd.cFileName);
                    if (hDevDll) PatchModuleIAT(hDevDll, fd.cFileName);
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
        }
    }

    HMODULE hMain = GetModuleHandleA(NULL);
    if (hMain) PatchModuleIAT(hMain, "AKSys.exe(Main)");
}

static PFN_ReadPlacementFromPrefs g_realWritePlacementToPrefs = NULL;  // same signature

static int __cdecl Hook_WritePlacementToPrefs(void* section, void* key, WINDOWPLACEMENT* wp) {
    char sec[128] = {0}, name[128] = {0};
    __try {
        if (section) strncpy(sec, (const char*)section, sizeof(sec) - 1);
        if (key) strncpy(name, (const char*)key, sizeof(name) - 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sec[0] = name[0] = 0;
    }
    if (strstr(sec, " WP") && !strstr(name, " WP")) {
        char tmp[128];
        strcpy(tmp, sec); strcpy(sec, name); strcpy(name, tmp);
    }
    if (wp) DIAG("WritePlacementToPrefs '%s' / '%s' rect=(%ld,%ld)-(%ld,%ld) size=%s %ldx%ld", sec, name,
                 wp->rcNormalPosition.left, wp->rcNormalPosition.top, wp->rcNormalPosition.right, wp->rcNormalPosition.bottom,
                 g_LastPlacementSizeValid ? "stored" : "none", g_LastPlacementSize.cx, g_LastPlacementSize.cy);
    if (wp && g_LastPlacementSizeValid && sec[0] && strstr(name, " WP")) {
        POINT pos = { wp->rcNormalPosition.left, wp->rcNormalPosition.top };
        StoreWindowState(sec, name, pos, g_LastPlacementSize);
    }
    g_LastPlacementSizeValid = false;
    return g_realWritePlacementToPrefs ? g_realWritePlacementToPrefs(section, key, wp) : 0;
}

static void EnsureReadPlacementHook() {
    static bool attempted = false;
    if (attempted) return;
    HMODULE hCom = GetModuleHandleA("AKComCtrls.dll");
    if (!hCom) return;
    attempted = true;
    void* pRead = (void*)GetProcAddress(hCom, "?ReadPlacementFromPrefs@CComCtrls@@SAHVCString@@0AAUtagWINDOWPLACEMENT@@@Z");
    bool ok = pRead && InstallTrampolineHook(pRead, (void*)Hook_ReadPlacementFromPrefs, (void**)&g_realReadPlacementFromPrefs);
    DIAG("ReadPlacementFromPrefs inline hook %s (addr=%p first byte=%02X)", ok ? "installed" : "FAILED",
         pRead, pRead ? *(BYTE*)pRead : 0);
    void* pWrite = (void*)GetProcAddress(hCom, "?WritePlacementToPrefs@CComCtrls@@SAHVCString@@0AAUtagWINDOWPLACEMENT@@@Z");
    bool okW = pWrite && InstallTrampolineHook(pWrite, (void*)Hook_WritePlacementToPrefs, (void**)&g_realWritePlacementToPrefs);
    DIAG("WritePlacementToPrefs inline hook %s", okW ? "installed" : "FAILED");
}

static void InstallSetupApiTrampolines() {
    EnsureReadPlacementHook();
    HMODULE hSetup = GetModuleHandleA("setupapi.dll");
    if (!hSetup) hSetup = LoadLibraryA("setupapi.dll");
    if (hSetup) {
        void* pG = (void*)GetProcAddress(hSetup, "SetupDiGetClassDevsA");
        void* pE = (void*)GetProcAddress(hSetup, "SetupDiEnumDeviceInterfaces");
        void* pD = (void*)GetProcAddress(hSetup, "SetupDiGetDeviceInterfaceDetailA");

        if (pG && !g_realSetupDiGetClassDevsA) {
            InstallTrampolineHook(pG, (void*)Hook_SetupDiGetClassDevsA, (void**)&g_realSetupDiGetClassDevsA);
        }
        if (pE && !g_realSetupDiEnumDeviceInterfaces) {
            InstallTrampolineHook(pE, (void*)Hook_SetupDiEnumDeviceInterfaces, (void**)&g_realSetupDiEnumDeviceInterfaces);
        }
        if (pD && !g_realSetupDiGetDeviceInterfaceDetailA) {
            InstallTrampolineHook(pD, (void*)Hook_SetupDiGetDeviceInterfaceDetailA, (void**)&g_realSetupDiGetDeviceInterfaceDetailA);
        }
    }

    HMODULE hOle = GetModuleHandleA("ole32.dll");
    if (!hOle) hOle = LoadLibraryA("ole32.dll");
    if (hOle) {
        void* pDrag = (void*)GetProcAddress(hOle, "DoDragDrop");
        if (pDrag && !g_realDoDragDrop) {
            InstallTrampolineHook(pDrag, (void*)Hook_DoDragDrop, (void**)&g_realDoDragDrop);
        }
    }
}

static DWORD WINAPI WatcherThread(LPVOID) {
    EnableDpiAwareness();
    EnsureWritableWorkingDirectoryAndDragDropFilters();
    RepairAkaiProgramEditorRegistryIfRequested();
    LoadZoomSettings();
    DIAG("Shim watcher started, zoom=%.2f aspect=%d", g_CurrentGlobalZoom, (int)g_KeepAspectRatio);
    OpenAkaiWinUsbDevices();
    InstallSetupApiTrampolines();

    for (int i = 0; i < 40; i++) {
        ScanAndPatchAllLoadedModules();
        if (g_DeviceDllHooked && i > 5) break;
        Sleep(50);
    }

    while (true) {
        Sleep(250);
        ScanAndPatchAllLoadedModules();
        EnsureReadPlacementHook();
        EnsureZoomToolbarRunning();
        ScanAndScaleAllSubWindows(g_CurrentGlobalZoom, false);

        EnterCriticalSection(&g_StagedLock);
        if (g_DragTargetFolderActive && g_LastTransferTick > 0) {
            if (GetTickCount() - g_LastTransferTick > 25000) {
                g_DragTargetFolderActive = false;
                g_DragTargetFolder[0] = 0;
            }
        }
        LeaveCriticalSection(&g_StagedLock);
    }

    return 0;
}

static HMODULE g_hRealVersion = NULL;

static void LoadRealVersionDll() {
    if (g_hRealVersion) return;
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    strcat(sysDir, "\\version.dll");
    g_hRealVersion = LoadLibraryA(sysDir);
}

static void CheckAndHookCaller(void* returnAddr) {
    HMODULE hCaller = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)returnAddr, &hCaller)) {
        if (hCaller && hCaller != g_hShimModule) {
            PatchModuleIAT(hCaller, "CallerModule");
        }
    }
    ScanAndPatchAllLoadedModules();
}

extern "C" {
    BOOL WINAPI Proxy_GetFileVersionInfoA(LPCSTR f, DWORD h, DWORD l, LPVOID d) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "GetFileVersionInfoA") : NULL;
        typedef BOOL (WINAPI *PFN)(LPCSTR, DWORD, DWORD, LPVOID);
        return p ? ((PFN)p)(f, h, l, d) : FALSE;
    }
    BOOL WINAPI Proxy_GetFileVersionInfoW(LPCWSTR f, DWORD h, DWORD l, LPVOID d) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "GetFileVersionInfoW") : NULL;
        typedef BOOL (WINAPI *PFN)(LPCWSTR, DWORD, DWORD, LPVOID);
        return p ? ((PFN)p)(f, h, l, d) : FALSE;
    }
    DWORD WINAPI Proxy_GetFileVersionInfoSizeA(LPCSTR f, LPDWORD h) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "GetFileVersionInfoSizeA") : NULL;
        typedef DWORD (WINAPI *PFN)(LPCSTR, LPDWORD);
        return p ? ((PFN)p)(f, h) : 0;
    }
    DWORD WINAPI Proxy_GetFileVersionInfoSizeW(LPCWSTR f, LPDWORD h) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "GetFileVersionInfoSizeW") : NULL;
        typedef DWORD (WINAPI *PFN)(LPCWSTR, LPDWORD);
        return p ? ((PFN)p)(f, h) : 0;
    }
    BOOL WINAPI Proxy_VerQueryValueA(LPCVOID b, LPCSTR s, LPVOID *pBlock, PUINT l) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "VerQueryValueA") : NULL;
        typedef BOOL (WINAPI *PFN)(LPCVOID, LPCSTR, LPVOID*, PUINT);
        return p ? ((PFN)p)(b, s, pBlock, l) : FALSE;
    }
    BOOL WINAPI Proxy_VerQueryValueW(LPCVOID b, LPCWSTR s, LPVOID *pBlock, PUINT l) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "VerQueryValueW") : NULL;
        typedef BOOL (WINAPI *PFN)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
        return p ? ((PFN)p)(b, s, pBlock, l) : FALSE;
    }
}

#pragma comment(linker, "/EXPORT:GetFileVersionInfoA=_Proxy_GetFileVersionInfoA@16")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoW=_Proxy_GetFileVersionInfoW@16")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeA=_Proxy_GetFileVersionInfoSizeA@8")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeW=_Proxy_GetFileVersionInfoSizeW@8")
#pragma comment(linker, "/EXPORT:VerQueryValueA=_Proxy_VerQueryValueA@16")
#pragma comment(linker, "/EXPORT:VerQueryValueW=_Proxy_VerQueryValueW@16")

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hShimModule = hModule;
        g_MainProcessId = GetCurrentProcessId();
        g_MainThreadId = GetCurrentThreadId();
        DisableThreadLibraryCalls(hModule);
        memset(g_Units, 0, sizeof(g_Units));
        memset(g_Handles, 0, sizeof(g_Handles));
        memset(g_ScaledWins, 0, sizeof(g_ScaledWins));
        memset(g_StagedFiles, 0, sizeof(g_StagedFiles));
        memset(g_PendingDrops, 0, sizeof(g_PendingDrops));
        g_UnitCount = 0;
        g_HandleCounter = 0;
        g_PendingDropCount = 0;
        InitializeCriticalSection(&g_UsbLock);
        InitializeCriticalSection(&g_StagedLock);

        CreateThread(NULL, 0, WatcherThread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_hToolbarWidget && IsWindow(g_hToolbarWidget)) DestroyWindow(g_hToolbarWidget);
        for (int i = 0; i < MAX_AKAI_HANDLES; i++) {
            if (g_Handles[i].hEvent) CloseHandle(g_Handles[i].hEvent);
        }
        for (int u = 0; u < g_UnitCount; u++) {
            if (g_Units[u].hWinUsb) WinUsb_Free(g_Units[u].hWinUsb);
            if (g_Units[u].hUsbDevice != INVALID_HANDLE_VALUE && g_Units[u].hUsbDevice != NULL) {
                CloseHandle(g_Units[u].hUsbDevice);
            }
        }
        DeleteCriticalSection(&g_UsbLock);
        DeleteCriticalSection(&g_StagedLock);
    }
    return TRUE;
}
