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
#include <string>
#include <vector>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include "gfx.h"
#include "ui.h"
#include "resource.h"

void RenderFrame();   // Forward declaration for repainting during installation.

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

static const wchar_t* kWindowClass = L"DynamicWallpapersSetupWindow";
static const float kWinW = 700.0f;
static const float kWinH = 560.0f;
static const float kPadX = 28.0f;
static const wchar_t* kAppDisplay = L"Dynamic Wallpapers";      // Display name for titles, shortcuts, and the uninstall entry.
static const wchar_t* kAppExe     = L"DynamicWallpapers.exe";   // Installed executable filename.
static const wchar_t* kLegacyDisplay = L"\u58c1\u7eb8\u968f\u65f6\u95f4\u53d8\u5316";        // Exact legacy display/shortcut name, retained only for migration.
static const wchar_t* kLegacyAppName  = L"TimeWall";            // Legacy installation directory, uninstall entry, and startup entry name.
static const wchar_t* kVersion = L"1.0.0";

#define RES_MANIFEST 2000
#define WM_APP_QUIT   (WM_APP + 3)

enum CtrlId {
    CID_NONE = -1,
    CID_MIN = 1, CID_CLOSE,
    CID_BROWSE = 10,
    CID_CHK_DESKTOP, CID_CHK_STARTMENU, CID_CHK_AUTOSTART,
    CID_THEME0 = 20, CID_THEME1, CID_THEME2, CID_THEME3,
    CID_INSTALL = 40, CID_CANCEL, CID_RUN, CID_FINISH,
};

enum Page { PAGE_OPTIONS, PAGE_INSTALLING, PAGE_DONE };

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
};

struct Setup {
    HWND hwnd = nullptr;
    Gfx gfx;
    Palette pal;
    Layout L;
    Page page = PAGE_OPTIONS;
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
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DynamicWallpapers",
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
    L.captionMin   = D2D1::RectF(W - 88, 0, W - 44, Ui::TitleBarH);
    L.captionClose = D2D1::RectF(W - 44, 0, W, Ui::TitleBarH);
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
}

static int HitTest(float x, float y) {
    const Layout& L = g_s.L;
    auto in = [&](const D2D1_RECT_F& r) {
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };
    if (in(L.captionClose)) return CID_CLOSE;
    if (in(L.captionMin)) return CID_MIN;
    if (g_s.page == PAGE_OPTIONS) {
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

static bool CreateShortcut(const std::wstring& lnk, const std::wstring& target,
                           const std::wstring& args, const std::wstring& desc,
                           const std::wstring& icon) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link)))
        return false;
    link->SetPath(target.c_str());
    if (!args.empty()) link->SetArguments(args.c_str());
    if (!desc.empty()) link->SetDescription(desc.c_str());
    if (!icon.empty()) link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

// Register the Apps & features uninstall entry.
static void WriteRegistry() {
    const wchar_t* uninst = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DynamicWallpapers";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, uninst, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
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
    setSz(L"DisplayIcon", exe);
    setSz(L"UninstallString", uninstallCmd);
    setSz(L"InstallLocation", g_s.installDir);
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
    for (auto& it : g_s.items) {
        if (it.kind == L"theme") {
            bool inc = false;
            for (auto& t : g_s.themes) if (t.name == it.theme && t.include) inc = true;
            if (!inc) continue;
        }
        step(it);
    }

    // Shortcuts use --show to open the main window.
    std::wstring exe = JoinPath(g_s.installDir, kAppExe);
    wchar_t buf[MAX_PATH * 2] = {0};
    if (g_s.desktopLnk && SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf))) {
        std::wstring lnk = JoinPath(buf, std::wstring(kAppDisplay) + L".lnk");
        CreateShortcut(lnk, exe, L"--show", L"Automatically change desktop wallpapers on a schedule", exe);
    }
    if (g_s.startMenuLnk &&
        SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::wstring dir = JoinPath(JoinPath(buf, L"Microsoft\\Windows\\Start Menu\\Programs"),
                                    kAppDisplay);
        EnsureDir(dir);
        CreateShortcut(JoinPath(dir, std::wstring(kAppDisplay) + L".lnk"), exe, L"--show",
                       L"Automatically change desktop wallpapers on a schedule", exe);
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
    WriteRegistry();
    g_s.finished = true;
    g_s.page = PAGE_DONE;
}

// ---------------------------------------------------------------------------
// Drawing.
// ---------------------------------------------------------------------------
static void DrawCaption(Gfx& g, const Palette& p) {
    // Use the same IDR_LOGO_PNG resource as the main app.
    // Fall back to a hand-drawn sun and moon if it cannot be loaded.
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
    } else {
        ID2D1SolidColorBrush* b = g.brush(ColHex(0xFFB900));
        if (b) g.dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ix - 2.5f, iy), 6.5f, 6.5f), b);
        ID2D1SolidColorBrush* mc = g.brush(p.textPrimary);
        if (mc) {
            g.dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ix + 4.0f, iy), 6.5f, 6.5f), mc);
            ID2D1SolidColorBrush* cut = g.brush(p.windowBgTop);
            if (cut) g.dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ix + 6.6f, iy - 1.6f), 5.6f, 5.6f), cut);
        }
    }
    IDWriteTextFormat* f = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g.text(std::wstring(kAppDisplay) + L" Setup", f, D2D1::RectF(48, 10, 420, 30), p.textPrimary);
    UiCaptionButton(g, g_s.L.captionMin, Ui::CAP_MIN, g_s.hot == CID_MIN, g_s.pressed == CID_MIN, p);
    UiCaptionButton(g, g_s.L.captionClose, Ui::CAP_CLOSE, g_s.hot == CID_CLOSE, g_s.pressed == CID_CLOSE, p);
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
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, kWinH));
        if (SUCCEEDED(g.dc()->CreateLinearGradientBrush(props, stops, &bg)) && bg) {
            g.dc()->FillRectangle(D2D1::RectF(0, 0, kWinW, kWinH), bg);
            bg->Release();
        }
        stops->Release();
    }
    DrawCaption(g, p);

    IDWriteTextFormat* fTitle = g.format(FONT_UI, 21.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fSub   = g.format(FONT_UI, 13.0f);
    IDWriteTextFormat* fSec   = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);

    if (g_s.page == PAGE_OPTIONS) {
        g.text(L"Install " + std::wstring(kAppDisplay) + L" " + kVersion, fTitle,
               D2D1::RectF(kPadX, 62, kWinW - kPadX, 96), p.textPrimary);
        g.text(L"Day and night wallpapers on a schedule. No administrator access needed.", fSub,
               D2D1::RectF(kPadX, 96, kWinW - kPadX, 120), p.textSecondary);

        g.text(L"Install location", fSec, D2D1::RectF(kPadX, 132, 200, 152), p.textSecondary);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(g_s.L.dirBox, 6, 6);
        g.fillRound(rr, p.ctrlFill);
        g.strokeRound(rr, p.ctrlStroke, 1.0f);
        IDWriteTextFormat* fPath = g.format(FONT_UI, 12.5f);
        g.text(g_s.installDir, fPath,
               D2D1::RectF(g_s.L.dirBox.left + 12, g_s.L.dirBox.top + 10,
                           g_s.L.dirBox.right - 10, g_s.L.dirBox.bottom - 8), p.textPrimary);
        UiButton(g, g_s.L.browseBtn, L"Browse...", p, false, g_s.hot == CID_BROWSE,
                 g_s.pressed == CID_BROWSE);

        UiCheckbox(g, g_s.L.chkDesktop.left, g_s.L.chkDesktop.top, L"Create a desktop shortcut",
                   g_s.desktopLnk, g_s.hot == CID_CHK_DESKTOP, p);
        UiCheckbox(g, g_s.L.chkStartMenu.left, g_s.L.chkStartMenu.top, L"Create a Start menu shortcut",
                   g_s.startMenuLnk, g_s.hot == CID_CHK_STARTMENU, p);
        UiCheckbox(g, g_s.L.chkAutoStart.left, g_s.L.chkAutoStart.top, L"Start with Windows",
                   g_s.autoStart, g_s.hot == CID_CHK_AUTOSTART, p);

        g.text(L"Included wallpaper themes", fSec, D2D1::RectF(kPadX, 314, 300, 334), p.textSecondary);
        for (int i = 0; i < (int)g_s.themes.size() && i < 4; ++i) {
            std::wstring label = g_s.themes[i].name + L" (" +
                std::to_wstring(g_s.themes[i].imageCount) + L" images · " +
                FormatSize(g_s.themes[i].size) + L")";
            UiCheckbox(g, g_s.L.themeChk[i].left, g_s.L.themeChk[i].top, label,
                       g_s.themes[i].include, g_s.hot == CID_THEME0 + i, p);
        }

        UiButton(g, g_s.L.cancelBtn, L"Cancel", p, false, g_s.hot == CID_CANCEL,
                 g_s.pressed == CID_CANCEL);
        UiButton(g, g_s.L.installBtn, L"Install", p, true, g_s.hot == CID_INSTALL,
                 g_s.pressed == CID_INSTALL, true, L"\uE896");
    } else if (g_s.page == PAGE_INSTALLING) {
        g.text(L"Installing...", fTitle, D2D1::RectF(kPadX, 78, kWinW - kPadX, 112), p.textPrimary);
        float prog = g_s.totalCount ? (float)g_s.doneCount / g_s.totalCount : 0.0f;
        UiProgress(g, g_s.L.progress, prog, p);
        std::wstring line = g_s.currentFile.empty() ? L"Preparing..." : g_s.currentFile;
        g.text(line, fSub, D2D1::RectF(kPadX, 220, kWinW - kPadX, 244), p.textSecondary);
        wchar_t buf[64];
        swprintf(buf, 64, L"%d / %d", g_s.doneCount, g_s.totalCount);
        IDWriteTextFormat* fRight = g.format(FONT_UI, 12.5f);
        g.textOptical(buf, fRight, D2D1::RectF(kWinW / 2, 220, kWinW - kPadX, 244),
                      p.textSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else {
        g.text(g_s.errorText.empty() ? L"Installation complete" : L"Installation completed with errors", fTitle,
               D2D1::RectF(kPadX, 78, kWinW - kPadX, 112), p.textPrimary);
        IDWriteTextFormat* fBody = g.format(FONT_UI, 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_TEXT_ALIGNMENT_LEADING, true);
        std::wstring body = g_s.errorText.empty()
            ? L"Ready to use. The app runs in the system tray and switches wallpapers automatically."
            : (L"Failed to write " + std::to_wstring(g_s.errorCount) + L" file(s): " + g_s.errorText);
        g.text(body, fBody, D2D1::RectF(kPadX, 124, kWinW - kPadX, 168),
               g_s.errorText.empty() ? p.textSecondary : p.accent);
        g.text(L"Installed in: " + g_s.installDir, fBody,
               D2D1::RectF(kPadX, 176, kWinW - kPadX, 220), p.textSecondary);
        g.text(L"Right-click the tray icon to change settings, pause switching, or quit.", fBody,
               D2D1::RectF(kPadX, 228, kWinW - kPadX, 272), p.textSecondary);

        UiButton(g, g_s.L.finishBtn, L"Finish", p, false, g_s.hot == CID_FINISH,
                 g_s.pressed == CID_FINISH);
        UiButton(g, g_s.L.runBtn, L"Launch now", p, true, g_s.hot == CID_RUN,
                 g_s.pressed == CID_RUN, true, L"\uE768");
    }
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
    dlg->SetTitle(L"Choose install location");
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

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (w) return 0;               // Extend the client area over the system title bar.
        break;
    case WM_NCHITTEST: {
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
        g_s.pressed = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        float s = g_s.gfx.scale();
        int id = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        ReleaseCapture();
        int pressed = g_s.pressed;
        g_s.pressed = CID_NONE;
        if (id != pressed || id == CID_NONE) {
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        switch (id) {
        case CID_MIN: ShowWindow(h, SW_MINIMIZE); break;
        case CID_CLOSE:
            if (g_s.page == PAGE_INSTALLING) break;   // Do not close halfway through writing installation files.
            DestroyWindow(h);
            break;
        case CID_CANCEL: DestroyWindow(h); break;
        case CID_BROWSE: PickFolder(); break;
        case CID_CHK_DESKTOP: g_s.desktopLnk = !g_s.desktopLnk; break;
        case CID_CHK_STARTMENU: g_s.startMenuLnk = !g_s.startMenuLnk; break;
        case CID_CHK_AUTOSTART: g_s.autoStart = !g_s.autoStart; break;
        case CID_INSTALL:
            if (g_s.items.empty()) {
                MessageBoxW(h, L"Installer data is missing (empty payload resources).", kAppDisplay,
                            MB_OK | MB_ICONERROR);
                break;
            }
            if (!HasExistingInstall() &&
                MessageBoxW(h,
                    L"Running Dynamic Wallpapers and Wallpaper Engine together may cause them to overwrite each other's wallpapers.\n\n"
                    L"Please quit or pause Wallpaper Engine while using this app.\n\nContinue installing?",
                    L"Wallpaper app conflict", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                break;
            RunInstall();
            break;
        case CID_RUN: {
            std::wstring exe = JoinPath(g_s.installDir, kAppExe);
            ShellExecuteW(nullptr, L"open", exe.c_str(), L"--show", g_s.installDir.c_str(),
                          SW_SHOWNORMAL);
            DestroyWindow(h);
            return 0;
        }
        case CID_FINISH: DestroyWindow(h); return 0;
        default:
            if (id >= CID_THEME0 && id <= CID_THEME3) {
                int i = id - CID_THEME0;
                if (i < (int)g_s.themes.size()) g_s.themes[i].include = !g_s.themes[i].include;
            }
            break;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_APP_QUIT:                  // The main app requests shutdown during uninstall or update.
        DestroyWindow(h);
        return 0;
    case WM_CLOSE:
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

    LoadManifest();
    g_s.installDir = DefaultInstallDir();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    int w = (int)(kWinW * dpi / 96.0f + 0.5f);
    int h = (int)(kWinH * dpi / 96.0f + 0.5f);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, L"Dynamic Wallpapers Setup",
                                WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
                                sx, sy, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    g_s.hwnd = hwnd;

    int corner = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int dark = 0;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    g_s.pal = MakePalette(SystemUsesDarkMode(), SystemAccentColor());
    if (!g_s.gfx.init(hwnd)) {
        MessageBoxW(nullptr, L"Graphics initialization failed.", kAppDisplay, MB_OK | MB_ICONERROR);
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
