// ============================================================================
// core.h - Paths, settings, themes, scheduling, wallpaper changes, and startup.
//
// Theme format and wallpaper handling follow WinDynamicDesktop:
// theme.json fields: imageFilename / dayImageList / nightImageList /
// sunriseImageList / sunsetImageList; this app uses one day image and one night image.
// Use IDesktopWallpaper for monitors and positioning, and notify Progman to enable fades.
// ============================================================================
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <utility>
#include <vector>

// A theme is a folder containing images and optional WDD-compatible theme.json.
struct ThemeInfo {
    std::wstring name;        // Display name from theme.json, falling back to the folder name.
    std::wstring folder;      // Absolute theme folder path.
    std::wstring dayImage;    // Absolute daytime wallpaper path.
    std::wstring nightImage;  // Absolute nighttime wallpaper path.
    int imageCount = 0;       // Total images in the folder.
    bool valid() const { return !dayImage.empty() && !nightImage.empty(); }
};

// Settings are stored in %APPDATA%\Dynamic Wallpapers\config.ini.
// Automatically migrate the legacy %APPDATA%\TimeWall directory on first launch.
struct Config {
    std::wstring theme;          // Selected theme name.
    bool noTheme = false;        // Keep no theme selected after the user changes the Windows wallpaper.
    int dayStart = 6 * 60;       // Day start in minutes since midnight.
    int nightStart = 18 * 60;    // Night start in minutes since midnight.
    bool autoSwitch = true;      // Master switch for automatic wallpaper changes.
    // Switch mode: 0=schedule using the two times above.
    // 1=system app theme: light image for light mode, dark image for dark mode.
    int switchMode = 0;
    bool startOnBoot = false;    // Start automatically with Windows.
    bool minimizeToTray = true;  // Close the window to the system tray.
    // Wallpaper position: 0=center, 1=tile, 2=stretch, 3=fit, 4=fill, 5=span; matches IDesktopWallpaper.
    int wallpaperStyle = 4;
    int appearance = 0;          // Appearance: 0=system, 1=light, 2=dark.
};

// Paths.
std::wstring ExeDir();
std::wstring ExePath();
std::wstring AppDataDir();                       // %APPDATA%\Dynamic Wallpapers; migrated from TimeWall.
std::wstring ConfigPath();
std::vector<std::wstring> ThemeDirs();           // Theme roots beside the executable and in user data directories.
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
bool FileExists(const std::wstring& p);

// Settings.
Config LoadConfig();
void SaveConfig(const Config& c);

// Themes.
std::vector<ThemeInfo> ScanThemes();
// Find by name; return nullptr if absent. Never fall back to the first theme: callers may delete it.
const ThemeInfo* FindTheme(const std::vector<ThemeInfo>& list, const std::wstring& name);

// User themes (Add theme).
std::wstring UserThemeDir();                       // %APPDATA%\Dynamic Wallpapers\themes
std::vector<std::wstring> ListImagesIn(const std::wstring& folder);  // Naturally sorted image paths.
bool RenameTheme(const std::wstring& folder, const std::wstring& newName, std::wstring& err);
// Validate the name and check all roots for conflicts, excluding exceptFolder itself.
bool ThemeNameAvailable(const std::wstring& name, const std::wstring& exceptFolder, std::wstring& err);
// Require 1-60 characters, excluding path separators, reserved punctuation, ".", and "..".
// Pure string validation with no file I/O; safe for live dialog feedback.
bool ThemeNameValid(const std::wstring& name, std::wstring& err);
// Discover day/night images using theme rules, with or without theme.json.
bool DiscoverThemeImages(const std::wstring& folder, const std::wstring& name,
                         std::wstring& dayImage, std::wstring& nightImage);
// Copy the selected pair as <name>_1.jpg / _2.jpg and generate theme.json.
bool ImportTheme(const std::wstring& name, const std::wstring& dayImage,
                 const std::wstring& nightImage, std::wstring& outName,
                 std::wstring* err = nullptr);
// Move the theme folder to the Recycle Bin; outErr describes failures.
bool DeleteThemeFolder(const std::wstring& folder, std::wstring& outErr);

// Time and stages.
int StageAt(const Config& c, int minuteOfDay);       // 0=day, 1=night.
int NextSwitchAt(const Config& c, int minuteOfDay);  // Next switching time.
int MinutesUntilSwitch(const Config& c, int minuteOfDay);
std::wstring FormatHM(int minutes);                  // "06:00"
std::wstring FormatDuration(int minutes);            // "2 hr 14 min".

// Wallpaper.
std::wstring CurrentWallpaperPath();                 // Current wallpaper path for the frosted background.
bool ApplyWallpaper(const std::wstring& image, int style);
// Read each monitor's wallpaper. Empty paths mean a solid background; false means a read failure.
bool ReadWallpaperSnapshot(std::vector<std::pair<std::wstring, std::wstring>>& snapshot);

// Startup: HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
bool AutoStartEnabled();
// Check --autostart / --background / --tray for legacy startup migration.
bool AutoStartHasBackgroundFlag();
void SetAutoStart(bool on);

// Single instance.
bool SingleInstanceAcquire();
// Notify the running instance; default 0x8001 = WM_APP+1 = show window.
void NotifyExistingInstance(int msg = 0x8001);

// Other helpers.
// Apply the system light/dark setting to the DWM window title bar.
void SetDarkModeAttribute(HWND hwnd, bool dark);
