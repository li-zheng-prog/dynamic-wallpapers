// ============================================================================
// setup.cpp - Single-window installer with WinUI 3 styling, sharing the gfx/ui layers.
//
// All installation files are embedded as RCDATA; no external files are needed at runtime.
// Each file has a resource ID assigned by build/pack_setup.py in payload.rc.
// Resource 2000 is the manifest: id|kind|relative path|theme name|file size.
// Install to %LOCALAPPDATA%\Programs\Dynamic Wallpapers without administrator rights.
// Register an Apps & features entry that runs DynamicWallpapers.exe --uninstall.
// Remove legacy installation folders, shortcuts, uninstall entries, and startup entries.
// First merge legacy user themes into the new directory to preserve user data.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <cmath>
#include <string>
#include <vector>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include "gfx.h"
#include "ui.h"
#include "resource.h"
#include "version.h"
#include "install_locale.h"
#include "shell_identity.h"

void RenderFrame();   // Forward declaration for repainting during installation.

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

static const wchar_t* kWindowClass = L"DynamicWallpapersSetupWindow";
static const float kWinW = 700.0f;
static const float kWinH = 560.0f;
static constexpr float kLanguageW = 524.0f, kLanguageH = 242.0f;
static const float kPadX = 28.0f;
static const wchar_t* kAppDisplay = L"Dynamic Wallpapers";      // Display name for titles, shortcuts, and the uninstall entry.
static const wchar_t* kAppExe     = L"DynamicWallpapers.exe";   // Installed executable filename.
static const wchar_t* kLegacyDisplay = L"\u58c1\u7eb8\u968f\u65f6\u95f4\u53d8\u5316";        // Exact legacy display/shortcut name, retained only for migration.
static const wchar_t* kLegacyAppName  = L"TimeWall";            // Legacy installation directory, uninstall entry, and startup entry name.
static const wchar_t* kVersion = TEXT(DW_VERSION_TEXT);

#define RES_MANIFEST 2000
#define WM_APP_QUIT   (WM_APP + 3)

enum CtrlId {
    CID_NONE = -1,
    CID_MIN = 1, CID_CLOSE,
    CID_BROWSE = 10,
    CID_CHK_DESKTOP, CID_CHK_STARTMENU, CID_CHK_AUTOSTART,
    CID_THEME0 = 20, CID_THEME1, CID_THEME2, CID_THEME3,
    CID_INSTALL = 40, CID_CANCEL, CID_RUN, CID_FINISH,
    CID_LANG_ZH, CID_LANG_EN, CID_LANG_NEXT, CID_LANG_CANCEL,
    CID_CONFLICT_CONTINUE, CID_CONFLICT_CANCEL,
};

enum Page { PAGE_LANGUAGE, PAGE_OPTIONS, PAGE_INSTALLING, PAGE_DONE };

struct PayloadItem {
    int id = 0;
    std::wstring kind;    // app / theme
    std::wstring rel;     // Path relative to the installation directory.
    std::wstring theme;   // Theme name when kind=theme.
    unsigned long long size = 0;
};

struct ThemeGroup {
    std::wstring name;
    unsigned long long size = 0;
    int imageCount = 0;
    bool include = true;
};

struct Layout {
    D2D1_RECT_F captionMin, captionClose;
    D2D1_RECT_F dirBox, browseBtn;
    D2D1_RECT_F chkDesktop, chkStartMenu, chkAutoStart;
    D2D1_RECT_F themeChk[4];
    D2D1_RECT_F installBtn, cancelBtn, runBtn, finishBtn;
    D2D1_RECT_F progress;
    D2D1_RECT_F languageChoice, languageNext, languageCancel;
    D2D1_RECT_F conflictCard, conflictContinue, conflictCancel;
};

struct Setup {
    HWND hwnd = nullptr;
    Gfx gfx;
    Palette pal;
    Layout L;
    Page page = PAGE_LANGUAGE;
    bool chinese = false, conflict = false;
    bool keyboardFocus = false;
    float languageAnim = 0, languageScale = 1;
    int focus = CID_NONE;
    std::wstring installDir;
    bool desktopLnk = true, startMenuLnk = true, autoStart = true;
    std::vector<PayloadItem> items;
    std::vector<ThemeGroup> themes;
    int hot = CID_NONE, pressed = CID_NONE;
    // Installation progress.
    int doneCount = 0, totalCount = 0;
    std::wstring currentFile;
    bool finished = false;
    std::wstring errorText;
    int errorCount = 0;
    ID2D1Bitmap* logoBmp = nullptr;         // Title-bar logo, shared with the main app.
    bool logoTried = false;
};
static Setup g_s;

static const wchar_t* Tr(const wchar_t* english, const wchar_t* chinese) {
    return g_s.chinese ? chinese : english;
}

static const wchar_t* SetupTitle() {
    return Tr(L"Dynamic Wallpapers Setup", L"Dynamic Wallpapers 安装程序");
}

static float WindowWidth() { return g_s.page == PAGE_LANGUAGE ? kLanguageW : kWinW; }
static float WindowHeight() { return g_s.page == PAGE_LANGUAGE ? kLanguageH : kWinH; }

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------
static std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    std::wstring r = a;
    if (r.back() != L'\\') r += L'\\';
    return r + b;
}

static void EnsureDir(const std::wstring& dir) {
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        if (dir[i] == L'\\' || dir[i] == L'/') {
            if (cur.size() > 3) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

static std::wstring FormatSize(unsigned long long bytes) {
    wchar_t buf[64];
    if (bytes >= 1024ull * 1024 * 1024)
        swprintf(buf, 64, L"%.1f GB", bytes / 1024.0 / 1024 / 1024);
    else if (bytes >= 1024ull * 1024)
        swprintf(buf, 64, L"%.0f MB", bytes / 1024.0 / 1024);
    else
        swprintf(buf, 64, L"%.0f KB", bytes / 1024.0);
    return buf;
}

static std::wstring DefaultInstallDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf);
    return JoinPath(JoinPath(buf, L"Programs"), kAppDisplay);
}

// Treat registered versions or an existing executable at the destination as an upgrade.
static bool HasExistingInstall() {
    for (const wchar_t* key : {
            kUninstallKey,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TimeWall" }) {
        HKEY opened = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &opened) == ERROR_SUCCESS) {
            RegCloseKey(opened);
            return true;
        }
    }
    DWORD attributes = GetFileAttributesW(JoinPath(g_s.installDir, kAppExe).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Read the embedded manifest and group theme files into checkbox entries.
static void LoadManifest() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(RES_MANIFEST), RT_RCDATA);
    if (!res) return;
    HGLOBAL h = LoadResource(nullptr, res);
    const char* data = (const char*)LockResource(h);
    DWORD size = SizeofResource(nullptr, res);
    if (!data || !size) return;
    std::string text(data, size);
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text.erase(0, 3);   // BOM
    std::wstring w;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    w.resize(wlen);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &w[0], wlen);

    std::wstringstream ss(w);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        PayloadItem it;
        std::wstringstream ls(line);
        std::wstring part;
        int idx = 0;
        while (std::getline(ls, part, L'|')) {
            switch (idx++) {
            case 0: it.id = _wtoi(part.c_str()); break;
            case 1: it.kind = part; break;
            case 2: it.rel = part; break;
            case 3: it.theme = part; break;
            case 4: it.size = _wtoi64(part.c_str()); break;
            }
        }
        if (it.id) g_s.items.push_back(it);
    }
    for (auto& it : g_s.items) {
        if (it.kind != L"theme") continue;
        ThemeGroup* g = nullptr;
        for (auto& t : g_s.themes) if (t.name == it.theme) g = &t;
        if (!g) {
            ThemeGroup ng;
            ng.name = it.theme;
            g_s.themes.push_back(ng);
            g = &g_s.themes.back();
        }
        g->size += it.size;
        if (it.rel.find(L".jpg") != std::wstring::npos || it.rel.find(L".png") != std::wstring::npos)
            g->imageCount++;
    }
}

// ---------------------------------------------------------------------------
// Layout.
// ---------------------------------------------------------------------------
static void ComputeLayout(Layout& L) {
    float W = kWinW;
    const float captionW = WindowWidth();
    L.captionMin   = D2D1::RectF(captionW - 88, 0, captionW - 44, Ui::TitleBarH);
    L.captionClose = D2D1::RectF(captionW - 44, 0, captionW, Ui::TitleBarH);
    // Options page.
    L.dirBox    = D2D1::RectF(kPadX, 158, W - kPadX - 96, 194);
    L.browseBtn = D2D1::RectF(W - kPadX - 88, 158, W - kPadX, 194);
    L.chkDesktop    = D2D1::RectF(kPadX, 208, kPadX + 20, 228);
    L.chkStartMenu  = D2D1::RectF(kPadX, 242, kPadX + 20, 262);
    L.chkAutoStart  = D2D1::RectF(kPadX, 276, kPadX + 20, 296);
    for (int i = 0; i < 4; ++i)
        L.themeChk[i] = D2D1::RectF(kPadX, 344 + i * 32, kPadX + 20, 364 + i * 32);
    // Bottom buttons share positions: Cancel/Install on options, Finish/Launch now on completion.
    const float by = kWinH - 62;
    L.installBtn = D2D1::RectF(W - kPadX - 116, by, W - kPadX, by + Ui::ButtonH);
    L.cancelBtn  = D2D1::RectF(W - kPadX - 116 - 108, by, W - kPadX - 116 - 8, by + Ui::ButtonH);
    L.runBtn     = D2D1::RectF(W - kPadX - 116, by, W - kPadX, by + Ui::ButtonH);
    L.finishBtn  = D2D1::RectF(W - kPadX - 116 - 108, by, W - kPadX - 116 - 8, by + Ui::ButtonH);
    // Progress page.
    L.progress   = D2D1::RectF(kPadX, 196, W - kPadX, 202);
    const float languageRight = kLanguageW - kPadX;
    L.languageChoice = D2D1::RectF(languageRight - 196, 118, languageRight, 118 + Ui::ButtonH);
    L.languageCancel = D2D1::RectF(languageRight - 208, 183, languageRight - 108, 183 + Ui::ButtonH);
    L.languageNext = D2D1::RectF(languageRight - 100, 183, languageRight, 183 + Ui::ButtonH);
    constexpr float conflictH = 168;
    const float conflictTop = (kWinH - conflictH) / 2;
    const float conflictBottom = conflictTop + conflictH;
    const float conflictButtonsY = conflictBottom - 24 - Ui::ButtonH;
    L.conflictCard = D2D1::RectF(102, conflictTop, W - 102, conflictBottom);
    L.conflictCancel = D2D1::RectF(288, conflictButtonsY, 412, conflictButtonsY + Ui::ButtonH);
    L.conflictContinue = D2D1::RectF(422, conflictButtonsY, W - 126, conflictButtonsY + Ui::ButtonH);
}

static D2D1_RECT_F LanguageSegment(int id) {
    auto r = g_s.L.languageChoice;
    const float mid = (r.left + r.right) / 2;
    if (id == CID_LANG_ZH) r.right = mid;
    else r.left = mid;
    return r;
}

static bool IsLanguageSegment(int id) { return id == CID_LANG_ZH || id == CID_LANG_EN; }

static void AnimateLanguage(HWND h) {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    if (!enabled || !SetTimer(h, 1, 16, nullptr)) {
        g_s.languageAnim = g_s.chinese ? 0.0f : 1.0f;
        g_s.languageScale = 1.0f;
    }
}

static void ResizeSetup(HWND h, UINT dpi, const RECT& anchor) {
    const int w = (int)std::lround(WindowWidth() * dpi / 96.0f);
    const int height = (int)std::lround(WindowHeight() * dpi / 96.0f);
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    int x = anchor.left + (anchor.right - anchor.left - w) / 2;
    int y = anchor.top + (anchor.bottom - anchor.top - height) / 2;
    if (GetMonitorInfoW(mon, &info)) {
        const RECT& r = info.rcWork;
        if (x + w > r.right) x = r.right - w;
        if (y + height > r.bottom) y = r.bottom - height;
        if (x < r.left) x = r.left;
        if (y < r.top) y = r.top;
    }
    ComputeLayout(g_s.L);
    g_s.gfx.resize(w, height, dpi);
    SetWindowPos(h, nullptr, x, y, w, height, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(h, nullptr, FALSE);
}

static int HitTest(float x, float y) {
    const Layout& L = g_s.L;
    auto in = [&](const D2D1_RECT_F& r) {
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };
    if (g_s.conflict) {
        if (in(L.conflictCancel)) return CID_CONFLICT_CANCEL;
        if (in(L.conflictContinue)) return CID_CONFLICT_CONTINUE;
        return CID_NONE;
    }
    if (in(L.captionClose)) return CID_CLOSE;
    if (in(L.captionMin)) return CID_MIN;
    if (g_s.page == PAGE_LANGUAGE) {
        if (in(LanguageSegment(CID_LANG_ZH))) return CID_LANG_ZH;
        if (in(LanguageSegment(CID_LANG_EN))) return CID_LANG_EN;
        if (in(L.languageNext)) return CID_LANG_NEXT;
        if (in(L.languageCancel)) return CID_LANG_CANCEL;
    } else if (g_s.page == PAGE_OPTIONS) {
        if (in(L.browseBtn)) return CID_BROWSE;
        if (in(L.chkDesktop)) return CID_CHK_DESKTOP;
        if (in(L.chkStartMenu)) return CID_CHK_STARTMENU;
        if (in(L.chkAutoStart)) return CID_CHK_AUTOSTART;
        for (int i = 0; i < (int)g_s.themes.size() && i < 4; ++i)
            if (in(L.themeChk[i])) return CID_THEME0 + i;
        if (in(L.installBtn)) return CID_INSTALL;
        if (in(L.cancelBtn)) return CID_CANCEL;
    } else if (g_s.page == PAGE_DONE) {
        if (in(L.runBtn)) return CID_RUN;
        if (in(L.finishBtn)) return CID_FINISH;
    }
    return CID_NONE;
}

// ---------------------------------------------------------------------------
// Installation steps.
// ---------------------------------------------------------------------------
static bool ExtractResource(int id, const std::wstring& destPath) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    HGLOBAL h = LoadResource(nullptr, res);
    void* data = LockResource(h);
    DWORD size = SizeofResource(nullptr, res);
    EnsureDir(destPath.substr(0, destPath.find_last_of(L'\\')));
    HANDLE f = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

static bool UpdateShortcut(const std::wstring& lnk, const std::wstring& target,
                           const std::wstring& desc, const std::wstring& icon, bool create) {
    if (!create && GetFileAttributesW(lnk.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link)))
        return false;
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        bool ours = create;
        if (!create && SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
            wchar_t oldTarget[MAX_PATH * 2] = {};
            ours = SUCCEEDED(link->GetPath(oldTarget, (int)_countof(oldTarget), nullptr, 0)) &&
                   _wcsicmp(oldTarget, target.c_str()) == 0;
        }
        if (ours) {
            if (create) {
                link->SetPath(target.c_str());
                link->SetArguments(L"--show");
                link->SetDescription(desc.c_str());
            }
            link->SetIconLocation(icon.c_str(), 0);
            IPropertyStore* store = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_IPropertyStore, (void**)&store))) {
                if (SUCCEEDED(SetShellString(store, PKEY_AppUserModel_ID, kShellAppId))) store->Commit();
                store->Release();
            }
            ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
        }
        pf->Release();
    }
    link->Release();
    if (ok) SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, lnk.c_str(), nullptr);
    return ok;
}

// Register the Apps & features uninstall entry.
static void WriteRegistry(const std::wstring& icon) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    std::wstring exe = JoinPath(g_s.installDir, kAppExe);
    std::wstring uninstallCmd = L"\"" + exe + L"\" --uninstall";
    auto setSz = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)v.c_str(),
                       (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    };
    setSz(L"DisplayName", kAppDisplay);
    setSz(L"DisplayVersion", kVersion);
    setSz(L"Publisher", kAppDisplay);
    setSz(L"DisplayIcon", icon);
    setSz(L"UninstallString", uninstallCmd);
    setSz(L"InstallLocation", g_s.installDir);
    WriteInstallLanguage(key, g_s.chinese);
    DWORD one = 1;
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    unsigned long long total = 0;
    for (auto& it : g_s.items) total += it.size;
    DWORD kb = (DWORD)(total / 1024);
    RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD, (const BYTE*)&kb, sizeof(kb));
    RegCloseKey(key);
}

// The rename changed installation paths, shortcuts, uninstall entries, and startup entries.
// Remove stale entries so Apps & features and the Start menu do not point to missing files,
// and Windows startup does not attempt to launch an obsolete executable.
// Merge missing themes from the old themes directory first to preserve user additions.
static void CopyTree(const std::wstring& src, const std::wstring& dst) {
    EnsureDir(dst);
    WIN32_FIND_DATAW fd = {};
    HANDLE hf = FindFirstFileW(JoinPath(src, L"*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring s = JoinPath(src, fd.cFileName), d = JoinPath(dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CopyTree(s, d);
        else CopyFileW(s.c_str(), d.c_str(), FALSE);
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
}

static void CleanLegacyInstall() {
    wchar_t local[MAX_PATH * 2] = {0}, appdata[MAX_PATH * 2] = {0}, desktop[MAX_PATH * 2] = {0};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop);

    // 1. Copy missing user themes from the legacy install, then remove the old directory.
    std::wstring oldDir = JoinPath(JoinPath(local, L"Programs"), kLegacyAppName);
    if (GetFileAttributesW(oldDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring oldThemes = JoinPath(oldDir, L"themes");
        std::wstring newThemes = JoinPath(g_s.installDir, L"themes");
        WIN32_FIND_DATAW fd = {};
        HANDLE hf = FindFirstFileW(JoinPath(oldThemes, L"*").c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                std::wstring dst = JoinPath(newThemes, fd.cFileName);
                if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES)
                    CopyTree(JoinPath(oldThemes, fd.cFileName), dst);
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
        std::wstring cmd = L"cmd.exe /c rmdir /s /q \"" + oldDir + L"\"";
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 8000);    // Wait for deletion to finish before proceeding.
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    // 2. Remove legacy Start menu and desktop shortcuts.
    std::wstring smDir = JoinPath(JoinPath(appdata, L"Microsoft\\Windows\\Start Menu\\Programs"),
                                  kLegacyDisplay);
    DeleteFileW(JoinPath(smDir, std::wstring(kLegacyDisplay) + L".lnk").c_str());
    RemoveDirectoryW(smDir.c_str());
    DeleteFileW(JoinPath(desktop, std::wstring(kLegacyDisplay) + L".lnk").c_str());
    // 3. Remove legacy uninstall and startup entries.
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TimeWall");
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kLegacyAppName);
        RegCloseKey(key);
    }
}

// Install in stages, processing messages after each stage to keep the UI responsive.
static void RunInstall() {
    g_s.totalCount = 0;
    for (auto& it : g_s.items) {
        if (it.kind == L"theme") {
            bool inc = false;
            for (auto& t : g_s.themes) if (t.name == it.theme && t.include) inc = true;
            if (!inc) continue;
        }
        g_s.totalCount++;
    }
    g_s.doneCount = 0;
    g_s.errorCount = 0;
    g_s.errorText.clear();
    g_s.page = PAGE_INSTALLING;

    auto step = [&](const PayloadItem& it) {
        g_s.currentFile = it.rel;
        RenderFrame();
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!ExtractResource(it.id, JoinPath(g_s.installDir, it.rel))) {
            g_s.errorCount++;
            if (g_s.errorCount == 1) g_s.errorText = it.rel;   // Show the first failed filename and summarize any additional errors by count.
        }
        g_s.doneCount++;
    };

    // Request running apps to quit before updating; check both current and legacy window classes.
    for (const wchar_t* cls : { L"DynamicWallpapersMainWindow", L"TimeWallMainWindow" }) {
        if (HWND other = FindWindowW(cls, nullptr)) {
            PostMessageW(other, WM_APP_QUIT, 0, 0);
            Sleep(600);
        }
    }
    EnsureDir(g_s.installDir);
    CleanLegacyInstall();
    const std::wstring exe = JoinPath(g_s.installDir, kAppExe);
    const std::wstring icon = JoinPath(g_s.installDir, kShellIconFile);
    for (auto& it : g_s.items) {
        if (it.kind == L"theme") {
            bool inc = false;
            for (auto& t : g_s.themes) if (t.name == it.theme && t.include) inc = true;
            if (!inc) continue;
        }
        const int errorsBefore = g_s.errorCount;
        step(it);
        if ((it.rel == kAppExe || it.rel == kShellIconFile) && g_s.errorCount == errorsBefore) {
            const auto changed = JoinPath(g_s.installDir, it.rel);
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, changed.c_str(), nullptr);
            if (it.rel == kShellIconFile) {
                // Already pinned shortcuts may still refer to the previous icon file.
                for (const wchar_t* oldIcon : {L"DynamicWallpapers-logo-v2.ico", L"DynamicWallpapers-logo-v3.ico", L"DynamicWallpapers-logo-v4.ico"}) {
                    const auto previous = JoinPath(g_s.installDir, oldIcon);
                    const DWORD attrs = GetFileAttributesW(previous.c_str());
                    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                        ExtractResource(it.id, previous))
                        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, previous.c_str(), nullptr);
                }
            }
        }
    }

    // Refresh existing app shortcuts even when the user opts out of creating new ones.
    const wchar_t* shortcutDesc = Tr(L"Automatically change desktop wallpapers on a schedule",
                                     L"按计划自动切换桌面壁纸");
    wchar_t buf[MAX_PATH * 2] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf))) {
        std::wstring lnk = JoinPath(buf, std::wstring(kAppDisplay) + L".lnk");
        UpdateShortcut(lnk, exe, shortcutDesc, icon, g_s.desktopLnk);
    }
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::wstring dir = JoinPath(JoinPath(buf, L"Microsoft\\Windows\\Start Menu\\Programs"),
                                    kAppDisplay);
        if (g_s.startMenuLnk) EnsureDir(dir);
        UpdateShortcut(JoinPath(dir, std::wstring(kAppDisplay) + L".lnk"), exe,
                       shortcutDesc, icon, g_s.startMenuLnk);
    }
    // Start with Windows.
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        if (g_s.autoStart) {
            std::wstring cmd = L"\"" + exe + L"\" --autostart";   // Start silently in the system tray.
            RegSetValueExW(key, kAppDisplay, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        }
        RegDeleteValueW(key, kLegacyAppName);      // Remove the old startup entry so only one remains.
        RegCloseKey(key);
    }
    WriteRegistry(icon);
    g_s.finished = true;
    g_s.page = PAGE_DONE;
}

// ---------------------------------------------------------------------------
// Drawing.
// ---------------------------------------------------------------------------
static void DrawCaption(Gfx& g, const Palette& p) {
    // Use the same IDR_LOGO_PNG resource as the main app.
    float ix = 28, iy = Ui::TitleBarH / 2;
    if (!g_s.logoBmp && !g_s.logoTried) {
        g_s.logoTried = true;
        g_s.logoBmp = g.loadImageFromResource(IDR_LOGO_PNG, 64);
    }
    if (g_s.logoBmp) {
        float s = 20.0f;
        D2D1_RECT_F dst = D2D1::RectF(ix - s / 2, iy - s / 2, ix + s / 2, iy + s / 2);
        g.dc()->DrawBitmap(g_s.logoBmp, dst, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    }
    IDWriteTextFormat* f = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g.textOptical(SetupTitle(), f, D2D1::RectF(48, 0, 515, Ui::TitleBarH), p.textPrimary);
    UiCaptionButton(g, g_s.L.captionMin, Ui::CAP_MIN, g_s.hot == CID_MIN, g_s.pressed == CID_MIN, p);
    UiCaptionButton(g, g_s.L.captionClose, Ui::CAP_CLOSE, g_s.hot == CID_CLOSE, g_s.pressed == CID_CLOSE, p);
}

static void DrawDialogCard(Gfx& g, const Palette& p, const D2D1_RECT_F& r) {
    auto card = D2D1::RoundedRect(r, 8, 8);
    g.fillRound(card, p.dark ? ColHex(0x272727) : ColHex(0xFAFAFA));
    g.strokeRound(card, p.ctrlStroke, 1);
}

static void DrawFocus(Gfx& g, const Palette& p, const D2D1_RECT_F& r, int id) {
    if (!g_s.keyboardFocus || g_s.focus != id || GetFocus() != g_s.hwnd) return;
    auto outline = D2D1::RectF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3);
    g.strokeRound(D2D1::RoundedRect(outline, 7, 7), p.accentText, 1.5f);
}

static void DrawDialogButton(Gfx& g, const Palette& p, const D2D1_RECT_F& r,
                             int id, const wchar_t* label, bool primary) {
    UiButton(g, r, label, p, primary, g_s.hot == id, g_s.pressed == id);
    DrawFocus(g, p, r, id);
}

static void DrawLanguagePage(Gfx& g, const Palette& p, IDWriteTextFormat* title, IDWriteTextFormat* body) {
    const Layout& L = g_s.L;
    g.textOptical(Tr(L"Select setup language", L"选择安装语言"), title,
                  D2D1::RectF(kPadX, 60, kLanguageW - kPadX, 92), p.textPrimary);
    g.textOptical(Tr(L"Language", L"界面语言"), body,
                  D2D1::RectF(kPadX, L.languageChoice.top, L.languageChoice.left - 12, L.languageChoice.bottom),
                  p.textPrimary);
    static const std::vector<std::wstring> labels = {L"简体中文", L"English"};
    static const wchar_t* const locales[] = {L"zh-CN", L"en-US"};
    const int hot = g_s.hot == CID_LANG_ZH ? 0 : g_s.hot == CID_LANG_EN ? 1 : -1;
    UiSegmented(g, L.languageChoice, labels, g_s.chinese ? 0 : 1, hot, p,
                g_s.languageAnim, g_s.languageScale, locales);
    if (IsLanguageSegment(g_s.focus)) DrawFocus(g, p, L.languageChoice, g_s.focus);
    if (auto* stroke = g.brush(p.divider))
        g.dc()->DrawLine(D2D1::Point2F(kPadX, 168), D2D1::Point2F(kLanguageW - kPadX, 168), stroke);
    DrawDialogButton(g, p, L.languageCancel, CID_LANG_CANCEL, Tr(L"Cancel", L"取消"), false);
    DrawDialogButton(g, p, L.languageNext, CID_LANG_NEXT, Tr(L"Continue", L"继续"), true);
}

static void DrawConflictDialog(Gfx& g, const Palette& p) {
    const Layout& L = g_s.L;
    UiDim(g, kWinW, kWinH, p);
    DrawDialogCard(g, p, L.conflictCard);

    // Amber is limited to the warning badge; the buttons keep the system accent.
    const float top = L.conflictCard.top + 24;
    const auto badge = D2D1::RectF(126, top, 167, top + 41);
    g.fillRound(D2D1::RoundedRect(badge, 8, 8), ColAlpha(p.dayDot, p.dark ? 0.16f : 0.20f));
    auto* icon = g.format(FONT_ICON, 20.0f);
    g.textOptical(L"\uE7BA", icon, badge, p.dark ? p.dayDot : ColHex(0x795000),
                  DWRITE_TEXT_ALIGNMENT_CENTER);

    auto* title = g.format(FONT_UI, 19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto* body = g.format(FONT_UI, 14.0f);
    g.textOptical(Tr(L"Wallpaper Engine conflicts with this app", L"Wallpaper Engine 与本软件冲突"), title,
                  D2D1::RectF(181, top, 574, top + 26), p.textPrimary);
    g.textOptical(Tr(L"The two apps cannot coexist", L"两个软件无法共存"), body,
                  D2D1::RectF(181, top + 32, 574, top + 54), p.textSecondary);
    DrawDialogButton(g, p, L.conflictCancel, CID_CONFLICT_CANCEL, Tr(L"Cancel", L"取消"), false);
    DrawDialogButton(g, p, L.conflictContinue, CID_CONFLICT_CONTINUE,
                     Tr(L"Continue installing", L"继续安装"), true);
}

void RenderFrame() {
    Gfx& g = g_s.gfx;
    const Palette& p = g_s.pal;
    if (!g.begin()) return;
    g.dc()->Clear(p.windowBgTop);
    // Background gradient.
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_GRADIENT_STOP gs[2] = {{0.0f, p.windowBgTop}, {1.0f, p.windowBgBottom}};
    if (SUCCEEDED(g.dc()->CreateGradientStopCollection(gs, 2, &stops)) && stops) {
        ID2D1LinearGradientBrush* bg = nullptr;
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props =
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, WindowHeight()));
        if (SUCCEEDED(g.dc()->CreateLinearGradientBrush(props, stops, &bg)) && bg) {
            g.dc()->FillRectangle(D2D1::RectF(0, 0, WindowWidth(), WindowHeight()), bg);
            bg->Release();
        }
        stops->Release();
    }
    DrawCaption(g, p);

    IDWriteTextFormat* fTitle = g.format(FONT_UI, 21.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fSub   = g.format(FONT_UI, 13.0f);
    IDWriteTextFormat* fSec   = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);

    if (g_s.page == PAGE_LANGUAGE) {
        DrawLanguagePage(g, p, fTitle, fSub);
    } else if (g_s.page == PAGE_OPTIONS) {
        g.text(Tr(L"Install ", L"安装 ") + std::wstring(kAppDisplay) + L" " + kVersion, fTitle,
               D2D1::RectF(kPadX, 62, kWinW - kPadX, 96), p.textPrimary);
        g.text(Tr(L"Day and night wallpapers on a schedule. No administrator access needed.",
                  L"按计划切换日间和夜间壁纸。安装无需管理员权限。"), fSub,
               D2D1::RectF(kPadX, 96, kWinW - kPadX, 120), p.textSecondary);

        g.text(Tr(L"Install location", L"安装位置"), fSec, D2D1::RectF(kPadX, 132, 200, 152), p.textSecondary);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(g_s.L.dirBox, 6, 6);
        g.fillRound(rr, p.ctrlFill);
        g.strokeRound(rr, p.ctrlStroke, 1.0f);
        IDWriteTextFormat* fPath = g.format(FONT_UI, 12.5f);
        g.text(g_s.installDir, fPath,
               D2D1::RectF(g_s.L.dirBox.left + 12, g_s.L.dirBox.top + 10,
                           g_s.L.dirBox.right - 10, g_s.L.dirBox.bottom - 8), p.textPrimary);
        UiButton(g, g_s.L.browseBtn, Tr(L"Browse…", L"浏览…"), p, false, g_s.hot == CID_BROWSE,
                 g_s.pressed == CID_BROWSE);

        UiCheckbox(g, g_s.L.chkDesktop.left, g_s.L.chkDesktop.top, Tr(L"Create a desktop shortcut", L"创建桌面快捷方式"),
                   g_s.desktopLnk, g_s.hot == CID_CHK_DESKTOP, p);
        UiCheckbox(g, g_s.L.chkStartMenu.left, g_s.L.chkStartMenu.top, Tr(L"Create a Start menu shortcut", L"创建开始菜单快捷方式"),
                   g_s.startMenuLnk, g_s.hot == CID_CHK_STARTMENU, p);
        UiCheckbox(g, g_s.L.chkAutoStart.left, g_s.L.chkAutoStart.top, Tr(L"Start with Windows", L"随 Windows 启动"),
                   g_s.autoStart, g_s.hot == CID_CHK_AUTOSTART, p);

        g.text(Tr(L"Included wallpaper themes", L"内置壁纸主题"), fSec,
               D2D1::RectF(kPadX, 314, 300, 334), p.textSecondary);
        for (int i = 0; i < (int)g_s.themes.size() && i < 4; ++i) {
            std::wstring label = g_s.themes[i].name + L" (" +
                std::to_wstring(g_s.themes[i].imageCount) +
                Tr(g_s.themes[i].imageCount == 1 ? L" image · " : L" images · ", L" 张图片 · ") +
                FormatSize(g_s.themes[i].size) + L")";
            UiCheckbox(g, g_s.L.themeChk[i].left, g_s.L.themeChk[i].top, label,
                       g_s.themes[i].include, g_s.hot == CID_THEME0 + i, p);
        }

        UiButton(g, g_s.L.cancelBtn, Tr(L"Cancel", L"取消"), p, false, g_s.hot == CID_CANCEL,
                 g_s.pressed == CID_CANCEL);
        UiButton(g, g_s.L.installBtn, Tr(L"Install", L"安装"), p, true, g_s.hot == CID_INSTALL,
                 g_s.pressed == CID_INSTALL, true, L"\uE896");
        DrawFocus(g, p, g_s.L.installBtn, CID_INSTALL);
    } else if (g_s.page == PAGE_INSTALLING) {
        g.text(Tr(L"Installing…", L"正在安装…"), fTitle, D2D1::RectF(kPadX, 78, kWinW - kPadX, 112), p.textPrimary);
        float prog = g_s.totalCount ? (float)g_s.doneCount / g_s.totalCount : 0.0f;
        UiProgress(g, g_s.L.progress, prog, p);
        std::wstring line = g_s.currentFile.empty() ? Tr(L"Preparing…", L"正在准备…") : g_s.currentFile;
        g.text(line, fSub, D2D1::RectF(kPadX, 220, kWinW - kPadX - 90, 244), p.textSecondary);
        wchar_t buf[64];
        swprintf(buf, 64, L"%d / %d", g_s.doneCount, g_s.totalCount);
        IDWriteTextFormat* fRight = g.format(FONT_UI, 12.5f);
        g.textOptical(buf, fRight, D2D1::RectF(kWinW / 2, 220, kWinW - kPadX, 244),
                      p.textSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else {
        const bool success = g_s.errorCount == 0;
        g.text(success ? Tr(L"Installation complete", L"安装完成")
                       : Tr(L"Installation completed with errors", L"安装完成，但出现错误"), fTitle,
               D2D1::RectF(kPadX, 78, kWinW - kPadX, 112), p.textPrimary);
        IDWriteTextFormat* fBody = g.format(FONT_UI, 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_TEXT_ALIGNMENT_LEADING, true);
        std::wstring body = success
            ? Tr(L"Ready to use. The app runs in the system tray and switches wallpapers automatically.",
                 L"应用已就绪，将在系统托盘运行并自动切换壁纸。")
            : (Tr(L"Could not write ", L"无法写入 ") + std::to_wstring(g_s.errorCount) +
               Tr(g_s.errorCount == 1 ? L" file: " : L" files. First failed file: ",
                  L" 个文件。首个失败的文件：") + g_s.errorText);
        g.text(body, fBody, D2D1::RectF(kPadX, 124, kWinW - kPadX, 168),
               success ? p.textSecondary : p.accentText);
        g.text(Tr(L"Installed in: ", L"安装位置：") + g_s.installDir, fBody,
               D2D1::RectF(kPadX, 176, kWinW - kPadX, 220), p.textSecondary);
        g.text(Tr(L"Right-click the tray icon to change settings, pause switching, or quit.",
                  L"右键单击托盘图标，可更改设置、暂停切换或退出。"), fBody,
               D2D1::RectF(kPadX, 228, kWinW - kPadX, 272), p.textSecondary);

        UiButton(g, g_s.L.finishBtn, Tr(L"Finish", L"完成"), p, false, g_s.hot == CID_FINISH,
                 g_s.pressed == CID_FINISH);
        UiButton(g, g_s.L.runBtn, Tr(L"Launch now", L"立即启动"), p, true, g_s.hot == CID_RUN,
                 g_s.pressed == CID_RUN, true, L"\uE768");
    }
    if (g_s.conflict) DrawConflictDialog(g, p);
    g.end();
}

// ---------------------------------------------------------------------------
// Window procedure.
// ---------------------------------------------------------------------------
static void PickFolder() {
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileDialog, (void**)&dlg)))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(Tr(L"Choose install location", L"选择安装位置"));
    dlg->SetOkButtonLabel(Tr(L"Select folder", L"选择文件夹"));
    if (SUCCEEDED(dlg->Show(g_s.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                g_s.installDir = JoinPath(path, kAppDisplay);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

static void ResolveConflict(bool proceed) {
    g_s.conflict = false;
    g_s.hot = g_s.pressed = CID_NONE;
    g_s.focus = proceed ? CID_NONE : CID_INSTALL;
    if (proceed) RunInstall();
}

static void ActivateControl(HWND h, int id) {
    switch (id) {
    case CID_MIN: ShowWindow(h, SW_MINIMIZE); break;
    case CID_CLOSE:
        if (g_s.page == PAGE_INSTALLING) break;
        [[fallthrough]];
    case CID_LANG_CANCEL: case CID_CANCEL: case CID_FINISH:
        DestroyWindow(h);
        return;
    case CID_LANG_ZH: case CID_LANG_EN:
        g_s.chinese = id == CID_LANG_ZH;
        g_s.focus = id;
        g_s.gfx.setTextLocale(g_s.chinese);
        SetWindowTextW(h, SetupTitle());
        AnimateLanguage(h);
        break;
    case CID_LANG_NEXT: {
        KillTimer(h, 1);
        RECT r;
        GetWindowRect(h, &r);
        g_s.page = PAGE_OPTIONS;
        g_s.hot = CID_NONE;
        g_s.focus = g_s.keyboardFocus ? CID_INSTALL : CID_NONE;
        ResizeSetup(h, GetDpiForWindow(h), r);
        break;
    }
    case CID_BROWSE: PickFolder(); break;
    case CID_CHK_DESKTOP: g_s.desktopLnk = !g_s.desktopLnk; break;
    case CID_CHK_STARTMENU: g_s.startMenuLnk = !g_s.startMenuLnk; break;
    case CID_CHK_AUTOSTART: g_s.autoStart = !g_s.autoStart; break;
    case CID_INSTALL:
        if (g_s.items.empty()) {
            InstallDialog(h, SetupTitle(), Tr(L"Installer data is missing", L"缺少安装数据"),
                          Tr(L"Download a new copy of the installer and try again.", L"请重新下载安装程序后再试。"),
                          TD_ERROR_ICON, Tr(L"OK", L"确定"));
        } else if (!HasExistingInstall()) {
            g_s.conflict = true;
            g_s.hot = CID_NONE;
            g_s.focus = CID_CONFLICT_CANCEL;
            g_s.keyboardFocus = true;
        } else {
            g_s.focus = CID_NONE;
            RunInstall();
        }
        break;
    case CID_CONFLICT_CANCEL: ResolveConflict(false); break;
    case CID_CONFLICT_CONTINUE: ResolveConflict(true); break;
    case CID_RUN: {
        std::wstring exe = JoinPath(g_s.installDir, kAppExe);
        ShellExecuteW(nullptr, L"open", exe.c_str(), L"--show", g_s.installDir.c_str(), SW_SHOWNORMAL);
        DestroyWindow(h);
        return;
    }
    default:
        if (id >= CID_THEME0 && id <= CID_THEME3) {
            int i = id - CID_THEME0;
            if (i < (int)g_s.themes.size()) g_s.themes[i].include = !g_s.themes[i].include;
        }
        break;
    }
    InvalidateRect(h, nullptr, FALSE);
}

static bool HandleKey(HWND h, WPARAM key, LPARAM l) {
    if (g_s.conflict || g_s.page == PAGE_LANGUAGE) {
        if (key == VK_ESCAPE) {
            ActivateControl(h, g_s.conflict ? CID_CONFLICT_CANCEL : CID_LANG_CANCEL);
            return true;
        }
        if (key == VK_TAB) {
            g_s.keyboardFocus = true;
            if (g_s.conflict) {
                g_s.focus = g_s.focus == CID_CONFLICT_CANCEL ? CID_CONFLICT_CONTINUE : CID_CONFLICT_CANCEL;
            } else {
                const int choices[] = {g_s.chinese ? CID_LANG_ZH : CID_LANG_EN, CID_LANG_CANCEL, CID_LANG_NEXT};
                int i = g_s.focus == CID_LANG_CANCEL ? 1 : g_s.focus == CID_LANG_NEXT ? 2 : 0;
                g_s.focus = choices[(i + (GetKeyState(VK_SHIFT) < 0 ? 2 : 1)) % 3];
            }
            InvalidateRect(h, nullptr, FALSE);
            return true;
        }
        if (!g_s.conflict && (g_s.focus == CID_LANG_ZH || g_s.focus == CID_LANG_EN) &&
            (key == VK_LEFT || key == VK_RIGHT)) {
            g_s.keyboardFocus = true;
            ActivateControl(h, key == VK_LEFT ? CID_LANG_ZH : CID_LANG_EN);
            return true;
        }
        if (key == VK_RETURN || key == VK_SPACE) {
            // A held key must not activate an action on the next page or reopen the warning.
            if (!(l & (1L << 30))) {
                int id = g_s.focus;
                if (!g_s.conflict && key == VK_RETURN && (id == CID_LANG_ZH || id == CID_LANG_EN))
                    id = CID_LANG_NEXT;
                ActivateControl(h, id);
            }
            return true;
        }
    } else if (g_s.page == PAGE_OPTIONS && g_s.focus == CID_INSTALL &&
               (key == VK_RETURN || key == VK_SPACE)) {
        if (!(l & (1L << 30))) ActivateControl(h, CID_INSTALL);
        return true;
    }
    return false;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (w) return 0;               // Extend the client area over the system title bar.
        break;
    case WM_NCHITTEST: {
        if (g_s.conflict) return HTCLIENT;
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &pt);
        float s = g_s.gfx.scale();
        float x = pt.x / s, y = pt.y / s;
        const Layout& L = g_s.L;
        auto in = [&](const D2D1_RECT_F& r) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        };
        if (in(L.captionMin) || in(L.captionClose) || y >= Ui::TitleBarH) return HTCLIENT;
        return HTCAPTION;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED:
        ResizeSetup(h, HIWORD(w), *reinterpret_cast<const RECT*>(l));
        return 0;
    case WM_TIMER:
        if (w == 1 && g_s.page == PAGE_LANGUAGE) {
            const float target = g_s.chinese ? 0.0f : 1.0f;
            const int selected = g_s.chinese ? CID_LANG_ZH : CID_LANG_EN;
            const float scale = g_s.pressed == selected ? 0.93f : 1.0f;
            const float d = target - g_s.languageAnim;
            const float ds = scale - g_s.languageScale;
            const bool settled = std::abs(d) < 0.002f && std::abs(ds) < 0.002f;
            g_s.languageAnim = settled ? target : g_s.languageAnim + d * 0.32f;
            g_s.languageScale = settled ? scale : g_s.languageScale + ds * 0.32f;
            if (settled) KillTimer(h, 1);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (HandleKey(h, w, l)) return 0;
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        RenderFrame();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        float s = g_s.gfx.scale();
        int id = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        if (id != g_s.hot) {
            g_s.hot = id;
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_s.hot = CID_NONE;
        g_s.pressed = CID_NONE;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        float s = g_s.gfx.scale();
        g_s.keyboardFocus = false;
        g_s.pressed = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        if (IsLanguageSegment(g_s.pressed)) AnimateLanguage(h);
        if (g_s.pressed >= CID_LANG_ZH && g_s.pressed <= CID_CONFLICT_CANCEL)
            g_s.focus = g_s.pressed;
        else if (!g_s.conflict && g_s.page == PAGE_OPTIONS)
            g_s.focus = g_s.pressed == CID_INSTALL ? CID_INSTALL : CID_NONE;
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        float s = g_s.gfx.scale();
        int id = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        int pressed = g_s.pressed;
        g_s.pressed = CID_NONE;
        ReleaseCapture();
        if (id != pressed || id == CID_NONE) {
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        ActivateControl(h, id);
        return 0;
    }
    case WM_CAPTURECHANGED:
        g_s.pressed = CID_NONE;
        if (g_s.page == PAGE_LANGUAGE) AnimateLanguage(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_QUIT:                  // The main app requests shutdown during uninstall or update.
        DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        if (g_s.conflict) {
            ActivateControl(h, CID_CONFLICT_CANCEL);
            return 0;
        }
        if (g_s.page == PAGE_INSTALLING) return 0;   // Apply the same close guard for Alt+F4 and the system menu.
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetCurrentProcessExplicitAppUserModelID(kShellSetupId);

    LoadManifest();
    g_s.installDir = DefaultInstallDir();
    g_s.chinese = WindowsPrefersChinese();
    g_s.gfx.setTextLocale(g_s.chinese);
    g_s.languageAnim = g_s.chinese ? 0.0f : 1.0f;
    g_s.focus = g_s.chinese ? CID_LANG_ZH : CID_LANG_EN;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    const UINT dpi = GetDpiForSystem();
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                 GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    int w = (int)(WindowWidth() * dpi / 96.0f + 0.5f);
    int h = (int)(WindowHeight() * dpi / 96.0f + 0.5f);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, SetupTitle(),
                                WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
                                sx, sy, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    g_s.hwnd = hwnd;
    if (wc.hIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    if (wc.hIconSm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exe, (DWORD)_countof(exe));
    if (exeLen && exeLen < _countof(exe)) {
        SetShellWindowRelaunch(hwnd, exe, std::wstring(exe) + L",-" + std::to_wstring(IDI_TASKBAR_V5),
                               L"", IDS_SHELL_TITLE, kShellSetupId);
    }

    int corner = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int dark = 0;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    g_s.pal = MakePalette(SystemUsesDarkMode(), SystemAccentColor());
    if (!g_s.gfx.init(hwnd)) {
        InstallDialog(nullptr, SetupTitle(), Tr(L"Graphics initialization failed", L"图形初始化失败"),
                      Tr(L"The installer could not start its graphics interface.", L"安装程序无法启动图形界面。"),
                      TD_ERROR_ICON, Tr(L"OK", L"确定"));
        return 2;
    }
    g_s.gfx.resize(w, h, dpi);
    ComputeLayout(g_s.L);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_s.gfx.shutdown();
    CoUninitialize();
    return 0;
}
