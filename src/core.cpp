// ============================================================================
// core.cpp - Core application logic.
// ============================================================================
#include "core.h"
#include "install_locale.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <cwchar>
#include <algorithm>

// Stable FOF_* ABI flags: 0x40=undo/recycle, 0x10=no confirmation, 0x04=silent, 0x400=no error UI.
// Provide standard values when newer NTDDI_VERSION settings hide them in MinGW headers.
#ifndef FOF_ALLOWUNDO
#define FOF_ALLOWUNDO 0x0040
#endif
#ifndef FOF_NOCONFIRMATION
#define FOF_NOCONFIRMATION 0x0010
#endif
#ifndef FOF_SILENT
#define FOF_SILENT 0x0004
#endif
#ifndef FOF_NOERRORUI
#define FOF_NOERRORUI 0x0400
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kAppName = L"Dynamic Wallpapers";
static const wchar_t* kLegacyName = L"TimeWall";              // Legacy internal name for settings, startup, and uninstall entries.
static const wchar_t* kIniSection = L"Dynamic Wallpapers";    // Current config.ini section.
static const wchar_t* kLegacyIniSection = L"TimeWall";        // Migrate this legacy section when reading settings.

// ---------------------------------------------------------------------------
// Path helpers.
// ---------------------------------------------------------------------------
std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    std::wstring r = a;
    if (r.back() != L'\\' && r.back() != L'/') r += L'\\';
    if (!b.empty() && (b.front() == L'\\' || b.front() == L'/')) return r + b.substr(1);
    return r + b;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return buf;
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

// Name migration (2026-09-29): TimeWall stored settings in %APPDATA%\TimeWall.
// Move the directory to %APPDATA%\Dynamic Wallpapers to preserve settings and user themes.
// Only migrate if the new directory is absent; fall back to copying config.ini if moving fails.
// ThemeDirs() continues scanning the legacy path as a fallback.
static void MigrateLegacyAppData(const std::wstring& appdata, const std::wstring& newDir) {
    static bool done = false;
    if (done) return;
    done = true;
    if (GetFileAttributesW(newDir.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    std::wstring oldDir = JoinPath(appdata, kLegacyName);
    if (GetFileAttributesW(oldDir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (!MoveFileExW(oldDir.c_str(), newDir.c_str(), MOVEFILE_COPY_ALLOWED)) {
        CreateDirectoryW(newDir.c_str(), nullptr);
        CopyFileW(JoinPath(oldDir, L"config.ini").c_str(),
                  JoinPath(newDir, L"config.ini").c_str(), FALSE);
    }
}

std::wstring AppDataDir() {
    static const std::wstring dir = [] {
        wchar_t buf[MAX_PATH * 2] = {0};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf)))
            return std::wstring();
        std::wstring d = JoinPath(buf, kAppName);
        MigrateLegacyAppData(buf, d);
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

std::wstring ConfigPath() {
    static const std::wstring path = JoinPath(AppDataDir(), L"config.ini");
    return path;
}

// Theme search order: 1) themes beside the executable (bundled and user-added themes).
// 2) %APPDATA%\Dynamic Wallpapers\themes (user directory).
// 3) %APPDATA%\TimeWall\themes (fallback if migration fails).
std::vector<std::wstring> ThemeDirs() {
    std::vector<std::wstring> dirs;
    dirs.push_back(JoinPath(ExeDir(), L"themes"));
    std::wstring user = JoinPath(AppDataDir(), L"themes");
    wchar_t buf[MAX_PATH * 2] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::wstring legacy = JoinPath(JoinPath(buf, kLegacyName), L"themes");
        if (std::find(dirs.begin(), dirs.end(), legacy) == dirs.end()) dirs.push_back(legacy);
    }
    if (std::find(dirs.begin(), dirs.end(), user) == dirs.end()) dirs.push_back(user);
    return dirs;
}

// ---------------------------------------------------------------------------
// INI settings; the Windows API handles UTF-16.
// ---------------------------------------------------------------------------
static std::wstring ReadIniRaw(const wchar_t* section, const wchar_t* key) {
    wchar_t buf[512] = {0};
    GetPrivateProfileStringW(section, key, L"", buf, 512, ConfigPath().c_str());
    return buf;
}

// Fall back to the legacy [TimeWall] section when the current section is absent.
// SaveConfig writes the current section and removes the legacy one.
static int GetInt(const wchar_t* key, int def) {
    std::wstring s = ReadIniRaw(kIniSection, key);
    if (s.empty()) s = ReadIniRaw(kLegacyIniSection, key);
    return s.empty() ? def : _wtoi(s.c_str());
}
static std::wstring GetStr(const wchar_t* key, const std::wstring& def) {
    std::wstring s = ReadIniRaw(kIniSection, key);
    if (s.empty()) s = ReadIniRaw(kLegacyIniSection, key);
    return s.empty() ? def : s;
}
static void PutInt(const wchar_t* key, int v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    WritePrivateProfileStringW(kIniSection, key, buf, ConfigPath().c_str());
}
static void PutStr(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(kIniSection, key, v.c_str(), ConfigPath().c_str());
}

Config LoadConfig() {
    Config c;
    c.theme        = GetStr(L"Theme", L"");
    c.noTheme      = GetInt(L"NoTheme", 0) != 0;
    c.dayStart     = GetInt(L"DayStart", 6 * 60);
    c.nightStart   = GetInt(L"NightStart", 18 * 60);
    c.autoSwitch   = GetInt(L"AutoSwitch", 1) != 0;
    c.switchMode   = GetInt(L"SwitchMode", 0);
    if (c.switchMode < 0 || c.switchMode > 1) c.switchMode = 0;
    c.startOnBoot  = GetInt(L"StartOnBoot", 0) != 0;
    c.minimizeToTray = GetInt(L"MinimizeToTray", 1) != 0;
    c.wallpaperStyle = GetInt(L"WallpaperStyle", 4);
    c.appearance   = GetInt(L"Appearance", 0);
    if (c.appearance < 0 || c.appearance > 2) c.appearance = 0;
    // Keep times distinct and within 0..1439 minutes.
    auto clamp = [](int v) { return v < 0 ? 0 : (v > 1439 ? 1439 : v); };
    c.dayStart = clamp(c.dayStart);
    c.nightStart = clamp(c.nightStart);
    if (c.dayStart == c.nightStart) c.nightStart = (c.dayStart + 720) % 1440;
    if (c.wallpaperStyle < 0 || c.wallpaperStyle > 5) c.wallpaperStyle = 4;
    return c;
}

void SaveConfig(const Config& c) {
    PutStr(L"Theme", c.theme);
    PutInt(L"NoTheme", c.noTheme ? 1 : 0);
    PutInt(L"DayStart", c.dayStart);
    PutInt(L"NightStart", c.nightStart);
    PutInt(L"AutoSwitch", c.autoSwitch ? 1 : 0);
    PutInt(L"SwitchMode", c.switchMode);
    PutInt(L"StartOnBoot", c.startOnBoot ? 1 : 0);
    PutInt(L"MinimizeToTray", c.minimizeToTray ? 1 : 0);
    PutInt(L"WallpaperStyle", c.wallpaperStyle);
    PutInt(L"Appearance", c.appearance);
    // Remove [TimeWall] after writing all values to the current section.
    WritePrivateProfileStringW(kLegacyIniSection, nullptr, nullptr, ConfigPath().c_str());
}

// ---------------------------------------------------------------------------
// Theme discovery.
// ---------------------------------------------------------------------------
static bool IsImageFile(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = towlower(c);
    return ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".bmp";
}

// Extract the trailing number (The Desert_5.jpg -> 5) for natural sorting and index matching.
static int TrailingNumber(const std::wstring& file) {
    size_t dot = file.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos ? file : file.substr(0, dot);
    size_t p = stem.find_last_of(L'_');
    if (p == std::wstring::npos) return 0;
    return (int)wcstol(stem.substr(p + 1).c_str(), nullptr, 10);
}

static void ListImages(const std::wstring& folder, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(folder, L"*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName;
        if (IsImageFile(n)) out.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        int na = TrailingNumber(a), nb = TrailingNumber(b);
        if (na != nb) return na < nb;
        return a < b;
    });
}

// Minimal theme.json parsing: only the fields needed by this app.
static bool JsonString(const std::wstring& js, const wchar_t* key, std::wstring& out) {
    std::wstring pat = std::wstring(L"\"") + key + L"\"";
    size_t p = js.find(pat);
    if (p == std::wstring::npos) return false;
    p = js.find(L':', p + pat.size());
    if (p == std::wstring::npos) return false;
    p = js.find(L'"', p);
    if (p == std::wstring::npos) return false;
    size_t q = js.find(L'"', p + 1);
    if (q == std::wstring::npos) return false;
    out = js.substr(p + 1, q - p - 1);
    return true;
}

static bool JsonIntArray(const std::wstring& js, const wchar_t* key, std::vector<int>& out) {
    std::wstring pat = std::wstring(L"\"") + key + L"\"";
    size_t p = js.find(pat);
    if (p == std::wstring::npos) return false;
    p = js.find(L'[', p);
    if (p == std::wstring::npos) return false;
    size_t q = js.find(L']', p);
    if (q == std::wstring::npos) return false;
    std::wstring body = js.substr(p + 1, q - p - 1);
    int cur = 0; bool has = false;
    for (wchar_t ch : body) {
        if (ch >= L'0' && ch <= L'9') { cur = cur * 10 + (ch - L'0'); has = true; }
        else { if (has) { out.push_back(cur); cur = 0; has = false; } }
    }
    if (has) out.push_back(cur);
    return !out.empty();
}

// Resolve an imageFilename pattern such as "The Desert_*.jpg" to an actual file.
static std::wstring ResolveImageName(const std::wstring& folder, const std::wstring& pattern,
                                     int index, const std::vector<std::wstring>& files) {
    if (!pattern.empty()) {
        std::wstring name = pattern;
        size_t star = name.find(L'*');
        if (star != std::wstring::npos) {
            name = name.substr(0, star) + std::to_wstring(index) + name.substr(star + 1);
            if (FileExists(JoinPath(folder, name))) return name;
        } else if (FileExists(JoinPath(folder, name))) {
            return name;
        }
        // If the pattern fails, fall back to a filename containing the number.
        for (auto& f : files)
            if (TrailingNumber(f) == index) return f;
    }
    for (auto& f : files)
        if (TrailingNumber(f) == index) return f;
    return L"";
}

static bool LoadTheme(const std::wstring& folder, const std::wstring& folderName, ThemeInfo& t) {
    std::vector<std::wstring> files;
    ListImages(folder, files);
    t.folder = folder;
    t.name = folderName;
    t.imageCount = (int)files.size();
    if (files.empty()) return false;

    std::wstring json;
    std::wstring jsonPath = JoinPath(folder, L"theme.json");
    if (FileExists(jsonPath)) {
        HANDLE h = CreateFileW(jsonPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(h, nullptr);
            if (size > 0 && size < 1024 * 1024) {
                std::string buf(size, 0);
                DWORD read = 0;
                ReadFile(h, &buf[0], size, &read, nullptr);
                int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)read, nullptr, 0);
                json.resize(wlen);
                MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)read, &json[0], wlen);
                // Remove U+FEFF decoded from a UTF-8 BOM.
                if (!json.empty() && json[0] == 0xFEFF) json.erase(0, 1);
            }
            CloseHandle(h);
        }
    }

    std::wstring display, pattern, dayName, nightName;
    if (!json.empty()) {
        JsonString(json, L"displayName", display);
        JsonString(json, L"imageFilename", pattern);
    }
    if (!display.empty()) t.name = display;

    std::vector<int> dayList, nightList, sunriseList, sunsetList;
    if (!json.empty()) {
        JsonIntArray(json, L"dayImageList", dayList);
        JsonIntArray(json, L"nightImageList", nightList);
        JsonIntArray(json, L"sunriseImageList", sunriseList);
        JsonIntArray(json, L"sunsetImageList", sunsetList);
    }

    // Day: first dayImageList entry, or last sunriseImageList entry as a fallback.
    // Night: first nightImageList entry, or last sunsetImageList entry as a fallback.
    if (!dayList.empty())        dayName = ResolveImageName(folder, pattern, dayList.front(), files);
    else if (!sunriseList.empty()) dayName = ResolveImageName(folder, pattern, sunriseList.back(), files);
    if (!nightList.empty())      nightName = ResolveImageName(folder, pattern, nightList.front(), files);
    else if (!sunsetList.empty()) nightName = ResolveImageName(folder, pattern, sunsetList.back(), files);

    // If theme.json or its fields are absent, use the first and last images for day and night.
    if (dayName.empty()) dayName = files.front();
    if (nightName.empty()) nightName = files.size() > 1 ? files.back() : files.front();

    t.dayImage = JoinPath(folder, dayName);
    t.nightImage = JoinPath(folder, nightName);
    return true;
}

std::vector<ThemeInfo> ScanThemes() {
    std::vector<ThemeInfo> out;
    for (auto& dir : ThemeDirs()) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(JoinPath(dir, L"*.*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            ThemeInfo t;
            if (LoadTheme(JoinPath(dir, name), name, t) && t.valid()) {
                bool dup = false;   // Keep the first theme with each name; the executable directory takes priority.
                for (auto& e : out) if (e.name == t.name) dup = true;
                if (!dup) out.push_back(t);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const ThemeInfo& a, const ThemeInfo& b) {
        return a.name < b.name;
    });
    return out;
}

const ThemeInfo* FindTheme(const std::vector<ThemeInfo>& list, const std::wstring& name) {
    for (auto& t : list) if (t.name == name) return &t;
    // The caller, --delete-theme=<name>, must not fall back to the first theme on a typo.
    // Return no match so exit code 2 remains meaningful and unrelated themes stay safe.
    return nullptr;
}

// ---------------------------------------------------------------------------
// User themes (Add theme).
// ---------------------------------------------------------------------------
// Probe write access with a small file, including restricted Program Files installs.
static void EnsureDir(const std::wstring& dir);      // Defined below.
static bool DirWritable(const std::wstring& dir) {
    EnsureDir(dir);
    std::wstring probe = JoinPath(dir, L".write_test");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// Prefer the themes directory beside the executable. Imported images are independent copies.
// If this location is not writable (for example, Program Files), fall back to
// %APPDATA%\Dynamic Wallpapers\themes.
std::wstring UserThemeDir() {
    std::wstring prog = JoinPath(ExeDir(), L"themes");
    if (DirWritable(prog)) return prog;
    std::wstring app = JoinPath(AppDataDir(), L"themes");
    EnsureDir(app);
    return app;
}

// Create parent directories; existing directories are harmless.
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

std::vector<std::wstring> ListImagesIn(const std::wstring& folder) {
    std::vector<std::wstring> names, full;
    ListImages(folder, names);
    for (auto& n : names) full.push_back(JoinPath(folder, n));
    return full;
}

bool DiscoverThemeImages(const std::wstring& folder, const std::wstring& name,
                         std::wstring& dayImage, std::wstring& nightImage) {
    ThemeInfo t;
    if (!LoadTheme(folder, name, t)) return false;
    dayImage = t.dayImage;
    nightImage = t.nightImage;
    return !dayImage.empty() && !nightImage.empty();
}

static bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Check all theme roots for a case-insensitive folder name or display name collision.
// ScanThemes deduplicates names with executable-directory priority; checking only one root
// could hide a newly added or renamed theme. exceptFolder excludes the theme itself.
static bool ThemeNameTaken(const std::wstring& name, const std::wstring& exceptFolder) {
    for (const std::wstring& dir : ThemeDirs()) {
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(JoinPath(dir, L"*.*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring fn = fd.cFileName;
            if (fn == L"." || fn == L"..") continue;
            std::wstring full = JoinPath(dir, fn);
            if (!exceptFolder.empty() && _wcsicmp(exceptFolder.c_str(), full.c_str()) == 0) continue;
            if (_wcsicmp(fn.c_str(), name.c_str()) == 0) { FindClose(h); return true; }
            ThemeInfo t;
            if (LoadTheme(full, fn, t) && t.name == name) { FindClose(h); return true; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return false;
}

bool ImportTheme(const std::wstring& name, const std::wstring& dayImage,
                 const std::wstring& nightImage, std::wstring& outName, std::wstring* err) {
    // Return specific failure reasons to both the UI and command-line callers.
    auto fail = [&](const std::wstring& why) {
        if (err) *err = why;
        return false;
    };
    if (dayImage.empty() || nightImage.empty()) return fail(AppText(L"Select two images.", L"请选择两张图片。"));
    // Use the rename validation rules so paths such as "..\..\x" cannot escape the theme root.
    std::wstring nameErr;
    if (!ThemeNameValid(name, nameErr)) return fail(nameErr);
    std::wstring root = UserThemeDir();
    if (root.empty()) return fail(AppText(L"The themes directory is unavailable.", L"主题目录不可用。"));
    EnsureDir(root);

    // Append a suffix on conflicts; ThemeNameTaken checks every theme directory.
    std::wstring finalName = name;
    for (int i = 2; ThemeNameTaken(finalName, L""); ++i) {
        wchar_t suffix[16];
        swprintf(suffix, 16, L" (%d)", i);
        finalName = name + suffix;
        if (i > 99) return fail(AppText(L"Too many themes share this name. Choose another name.", L"同名主题过多，请试用其他名称。"));
    }
    std::wstring target = JoinPath(root, finalName);
    if (!CreateDirectoryW(target.c_str(), nullptr))
        return fail(AppText(L"Cannot create a theme folder in the app directory or %APPDATA%.", L"无法在应用目录或 %APPDATA% 中创建主题文件夹。"));

    // Copy both images with .jpg suffixes; decoding still recognizes the actual image format.
    auto copyAs = [&](const std::wstring& src, int index) -> bool {
        std::wstring dst = JoinPath(target, finalName + L"_" + std::to_wstring(index) + L".jpg");
        return CopyFileW(src.c_str(), dst.c_str(), FALSE) != FALSE;
    };
    if (!copyAs(dayImage, 1)) return fail(AppText(L"Cannot copy the light image. It may have been moved or deleted.", L"无法复制浅色图片。文件可能已移动或删除。"));
    if (!copyAs(nightImage, 2)) return fail(AppText(L"Cannot copy the dark image. It may have been moved or deleted.", L"无法复制深色图片。文件可能已移动或删除。"));

    // Write UTF-8 theme.json in WDD format: day=1, night=2.
    std::wstring json = L"{\r\n"
                        L"  \"imageFilename\": \"" + finalName + L"_*.jpg\",\r\n"
                        L"  \"displayName\": \"" + finalName + L"\",\r\n"
                        L"  \"dayImageList\": [1],\r\n"
                        L"  \"nightImageList\": [2]\r\n"
                        L"}\r\n";
    std::wstring jsonPath = JoinPath(target, L"theme.json");
    HANDLE h = CreateFileW(jsonPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail(AppText(L"Cannot write theme.json.", L"无法写入 theme.json。"));
    int n = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(), &utf8[0], n, nullptr, nullptr);
    DWORD written = 0;
    BOOL ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok) return fail(AppText(L"Cannot write theme.json.", L"无法写入 theme.json。"));

    outName = finalName;
    return true;
}

// ---------------------------------------------------------------------------
// Rename the folder and update theme.json displayName so rescanning shows the new name.
// Image filenames stay unchanged; imageFilename is a wildcard pattern.
// ---------------------------------------------------------------------------
bool ThemeNameValid(const std::wstring& name, std::wstring& err) {
    if (name.empty()) { err = AppText(L"Enter a name.", L"请输入名称。"); return false; }
    if (name.size() > 60) { err = AppText(L"Use 60 characters or fewer.", L"名称不能超过 60 个字符。"); return false; }
    for (wchar_t c : name) {
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) {
        err = AppText(L"Names cannot contain \\ / : * ? \" < > | characters.",
                      L"名称不能含有 \\ / : * ? \" < > | 字符。");
            return false;
        }
    }
    if (name == L"." || name == L"..") { err = AppText(L"This name is not allowed.", L"不能使用此名称。"); return false; }
    return true;
}

bool ThemeNameAvailable(const std::wstring& name, const std::wstring& exceptFolder, std::wstring& err) {
    if (!ThemeNameValid(name, err)) return false;
    if (ThemeNameTaken(name, exceptFolder)) {
        err = AppText(L"A theme with this name already exists. Choose another name.", L"已存在同名主题，请使用其他名称。");
        return false;
    }
    return true;
}

// Update displayName, inserting it if absent; skip nonexistent theme.json files.
static void UpdateThemeJsonDisplayName(const std::wstring& folder, const std::wstring& newName) {
    std::wstring path = JoinPath(folder, L"theme.json");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, nullptr), read = 0;
    std::string bytes(size, 0);
    if (size) ReadFile(h, &bytes[0], size, &read, nullptr);
    CloseHandle(h);

    // Decode UTF-8 to UTF-16 and remove any BOM.
    if (read >= 3 && (unsigned char)bytes[0] == 0xEF) bytes.erase(0, 3);
    int wn = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), nullptr, 0);
    std::wstring w((size_t)(wn > 0 ? wn : 0), 0);
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), &w[0], wn);

    // Replace the quoted displayName value, or insert it after the opening brace.
    const std::wstring key = L"\"displayName\"";
    size_t k = w.find(key);
    if (k != std::wstring::npos) {
        size_t c = w.find(L':', k + key.size());
        size_t q1 = (c == std::wstring::npos) ? std::wstring::npos : w.find(L'"', c + 1);
        size_t q2 = (q1 == std::wstring::npos) ? std::wstring::npos : w.find(L'"', q1 + 1);
        if (q1 == std::wstring::npos || q2 == std::wstring::npos) return;
        w = w.substr(0, q1 + 1) + newName + w.substr(q2);
    } else {
        size_t brace = w.find(L'{');
        if (brace == std::wstring::npos) return;
        w.insert(brace + 1, L"\r\n  \"displayName\": \"" + newName + L"\",");
    }

    // Write UTF-8 without a BOM.
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n : 0), 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                    nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (!out.empty()) WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(h);
}

bool RenameTheme(const std::wstring& folder, const std::wstring& newName, std::wstring& err) {
    if (folder.empty()) { err = AppText(L"The theme directory does not exist.", L"主题目录不存在。"); return false; }
    if (!ThemeNameAvailable(newName, folder, err)) return false;
    size_t sl = folder.find_last_of(L"\\/");
    if (sl == std::wstring::npos) { err = AppText(L"The theme directory path is invalid.", L"主题目录路径无效。"); return false; }
    std::wstring parent = folder.substr(0, sl);
    std::wstring target = JoinPath(parent, newName);
    if (target == folder) return true;                       // Unchanged.
    if (!MoveFileExW(folder.c_str(), target.c_str(), 0)) {
        err = AppText(L"Cannot rename the folder. It already exists or is in use. Close it in File Explorer and retry.", L"无法重命名文件夹。文件夹可能已存在或正在使用。请在文件资源管理器中关闭它，然后重试。");
        return false;
    }
    UpdateThemeJsonDisplayName(target, newName);
    return true;
}

// Reject dangerous deletion targets such as drive roots (for example, C:\).
static bool SafeToDelete(const std::wstring& folder) {
    if (folder.empty() || folder.size() <= 3) return false;
    // Require an absolute drive path with a component after the root.
    if (folder.size() >= 3 && folder[1] == L':' && (folder[2] == L'\\' || folder[2] == L'/'))
        return folder.size() > 3;
    return false;
}

// Move deleted themes to the Recycle Bin with IFileOperation.
// MinGW may hide SHFileOperation with newer NTDDI_VERSION; FOF_ALLOWUNDO gives equivalent behavior.
bool DeleteThemeFolder(const std::wstring& folder, std::wstring& outErr) {
    outErr.clear();
    if (folder.empty() || !DirExists(folder)) {
        outErr = AppText(L"The theme folder no longer exists.", L"主题文件夹已不存在。");
        return false;
    }
    if (!SafeToDelete(folder)) {
        outErr = AppText(L"Deletion refused: unsafe path.", L"未删除主题：路径不在允许的范围内。");
        return false;
    }

    bool ok = false;
    IFileOperation* pfo = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pfo))) && pfo) {
        pfo->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI);
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
            if (SUCCEEDED(pfo->DeleteItem(item, nullptr)))
                ok = SUCCEEDED(pfo->PerformOperations());
            item->Release();
        }
        pfo->Release();
    }
    // Do not fall back to direct deletion on failure: a folder in use could be partially erased
    // without a recoverable Recycle Bin entry. Report the error so the user can retry.
    // IFileOperation itself handles drives without a Recycle Bin.
    if (!ok || DirExists(folder)) {
        outErr = AppText(L"Cannot delete the theme. Files may be in use or access may be denied.", L"无法删除主题。文件可能正在使用，或没有访问权限。");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Time and day/night stages.
// ---------------------------------------------------------------------------
int StageAt(const Config& c, int m) {
    int d = c.dayStart, n = c.nightStart;
    if (d == n) return 0;
    if (d < n) return (m >= d && m < n) ? 0 : 1;   // Daytime does not cross midnight.
    return (m >= d || m < n) ? 0 : 1;              // Daytime crosses midnight, for example 22:00-06:00.
}

int NextSwitchAt(const Config& c, int m) {
    int d = c.dayStart, n = c.nightStart;
    if (d == n) return d;
    int best = -1, bestDelta = 0;
    int cands[2] = {d, n};
    for (int i = 0; i < 2; ++i) {
        int delta = ((cands[i] - m) % 1440 + 1440) % 1440;
        if (delta == 0) delta = 1440;
        if (best < 0 || delta < bestDelta) { best = cands[i]; bestDelta = delta; }
    }
    return best;
}

int MinutesUntilSwitch(const Config& c, int m) {
    int d = NextSwitchAt(c, m);
    return ((d - m) % 1440 + 1440) % 1440;
}

std::wstring FormatHM(int minutes) {
    minutes = ((minutes % 1440) + 1440) % 1440;
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d:%02d", minutes / 60, minutes % 60);
    return buf;
}

std::wstring FormatDuration(int minutes) {
    if (minutes <= 0) return AppText(L"soon", L"即将切换");
    int h = minutes / 60, m = minutes % 60;
    wchar_t buf[64];
    if (h > 0 && m > 0) swprintf(buf, 64, AppText(L"%d hr %d min", L"%d 小时 %d 分钟"), h, m);
    else if (h > 0)     swprintf(buf, 64, AppText(L"%d hr", L"%d 小时"), h);
    else                swprintf(buf, 64, AppText(L"%d min", L"%d 分钟"), m);
    return buf;
}

// Read the current wallpaper path from the registry; return empty if unavailable or missing.
// Query the size first because wallpaper paths may exceed MAX_PATH.
std::wstring CurrentWallpaperPath() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return std::wstring();
    DWORD n = 0, type = 0;
    LSTATUS st = RegQueryValueExW(k, L"WallPaper", nullptr, &type, nullptr, &n);
    std::wstring path;
    if (st == ERROR_SUCCESS && type == REG_SZ && n >= sizeof(wchar_t)) {
        path.resize(n / sizeof(wchar_t));
        st = RegQueryValueExW(k, L"WallPaper", nullptr, nullptr, (LPBYTE)&path[0], &n);
    }
    RegCloseKey(k);
    while (!path.empty() && path.back() == L'\0') path.pop_back();   // The value includes its terminating null.
    if (st != ERROR_SUCCESS || path.empty() || !FileExists(path)) return std::wstring();
    return path;
}

// ---------------------------------------------------------------------------
// Wallpaper position values match IDesktopWallpaper::SetPosition.
// ---------------------------------------------------------------------------
bool ReadWallpaperSnapshot(std::vector<std::pair<std::wstring, std::wstring>>& snapshot) {
    IDesktopWallpaper* dw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                IID_IDesktopWallpaper, (void**)&dw))) return false;
    std::vector<std::pair<std::wstring, std::wstring>> result;
    UINT count = 0;
    bool ok = SUCCEEDED(dw->GetMonitorDevicePathCount(&count));
    for (UINT i = 0; ok && i < count; ++i) {
        LPWSTR monitor = nullptr, image = nullptr;
        HRESULT hr = dw->GetMonitorDevicePathAt(i, &monitor);
        if (SUCCEEDED(hr)) hr = dw->GetWallpaper(monitor, &image);
        if (SUCCEEDED(hr) && monitor) result.emplace_back(monitor, image ? image : L"");
        else ok = false;
        CoTaskMemFree(image);
        CoTaskMemFree(monitor);
    }
    dw->Release();
    if (!ok || result.empty()) return false;
    snapshot = std::move(result);
    return true;
}

bool ApplyWallpaper(const std::wstring& image, int style) {
    if (image.empty() || !FileExists(image)) return false;

    // Notify Explorer to enable wallpaper cross-fades, following WinDynamicDesktop.
    if (HWND progman = FindWindowW(L"Progman", nullptr)) {
        DWORD_PTR res = 0;
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_ABORTIFHUNG, 1000, &res);
    }

    bool ok = false;
    IDesktopWallpaper* dw = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                   IID_IDesktopWallpaper, (void**)&dw))) {
        dw->SetPosition((DESKTOP_WALLPAPER_POSITION)style);
        ok = SUCCEEDED(dw->SetWallpaper(nullptr, image.c_str()));
        dw->Release();
    }
    if (!ok) {
        // Legacy API fallback also updates the registry and broadcasts the change.
        ok = SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (void*)image.c_str(),
                                   SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != FALSE;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Start with Windows.
// ---------------------------------------------------------------------------
bool AutoStartEnabled() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return false;
    return buf[0] != 0;
}

bool AutoStartHasBackgroundFlag() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return true;                               // If reading failed, avoid repeatedly rewriting the value.
    std::wstring s = buf;
    return s.find(L"--autostart") != std::wstring::npos ||
           s.find(L"--background") != std::wstring::npos ||
           s.find(L"--tray") != std::wstring::npos;
}

void SetAutoStart(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        // Include --autostart so startup launches silently into the system tray.
        std::wstring cmd = L"\"" + ExePath() + L"\" --autostart";
        RegSetValueExW(key, kAppName, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kAppName);
    }
    RegDeleteValueW(key, kLegacyName);          // Remove the legacy entry so only one startup command remains.
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// Single-instance handling.
// ---------------------------------------------------------------------------
static HANDLE g_mutex = nullptr;

bool SingleInstanceAcquire() {
    g_mutex = CreateMutexW(nullptr, FALSE, L"Local\\DynamicWallpapers_SingleInstance_Mutex");
    if (!g_mutex) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return false;
    return true;
}

// Find the main window by its fixed class name and send a custom message (show by default).
// Also check the legacy class name during upgrades when an older process may still be running.
void NotifyExistingInstance(int msg) {
    if (HWND h = FindWindowW(L"DynamicWallpapersMainWindow", nullptr))
        PostMessageW(h, msg, 0, 0);
    else if (HWND h2 = FindWindowW(L"TimeWallMainWindow", nullptr))
        PostMessageW(h2, msg, 0, 0);
}

// ---------------------------------------------------------------------------
void SetDarkModeAttribute(HWND hwnd, bool dark) {
    BOOL v = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &v, sizeof(v));
}
