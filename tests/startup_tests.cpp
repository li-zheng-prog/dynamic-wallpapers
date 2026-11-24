// Exercise the real UI state machine with system-setting writes replaced by fakes.
#define ApplyWallpaper TestApplyWallpaper
#define SaveConfig TestSaveConfig
#define SetAutoStart TestSetAutoStart
#define ReadWallpaperSnapshot TestReadWallpaperSnapshot
#define SystemUsesDarkMode TestSystemUsesDarkMode
#define wWinMain UnusedApplicationEntry
#include "../src/main.cpp"
#undef ApplyWallpaper
#undef SaveConfig
#undef SetAutoStart
#undef ReadWallpaperSnapshot
#undef SystemUsesDarkMode
#undef wWinMain
#include <cstdio>
#include <cstdlib>
#include <functional>

static int checks = 0, applyCalls = 0;
static bool applySucceeds = true;
static bool deferApply = false;
static std::function<void()> duringApply;
static std::function<void()> duringRead;
static std::wstring lastImage;
static int lastStyle = -1;
static bool snapshotAvailable = true, appDark = false;
static Config savedConfig;
static std::vector<std::pair<std::wstring, std::wstring>> wallpaper = {{L"monitor-a", L"initial.jpg"}};

static void Check(bool condition, const char* name) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}

bool TestApplyWallpaper(const std::wstring& image, int style) {
    ++applyCalls;
    lastImage = image;
    lastStyle = style;
    if (duringApply) duringApply();
    if (applySucceeds && !deferApply) for (auto& monitor : wallpaper) monitor.second = image;
    return applySucceeds;
}
void TestSaveConfig(const Config& config) { savedConfig = config; }
void TestSetAutoStart(bool) {}
bool TestReadWallpaperSnapshot(std::vector<std::pair<std::wstring, std::wstring>>& out) {
    if (duringRead) duringRead();
    if (!snapshotAvailable) return false;
    out = wallpaper;
    return !out.empty();
}
bool TestSystemUsesDarkMode() { return appDark; }

static void TestWallpaperTracker() {
    using Result = WallpaperTracker::Result;
    const std::wstring day = L"C:\\Themes\\day.jpg", night = L"C:\\Themes\\night.jpg";
    Check(SameWallpaperPath(day, L"c:/Themes/sub/../DAY.jpg"), "canonical full path comparison");
    Check(SameWallpaperPath(day, L"\\\\?\\C:\\Themes\\day.jpg"), "extended path comparison");
    Check(!SameWallpaperPath(day, L"C:\\Other\\day.jpg"), "same filename elsewhere is external");
    Check(!SameWallpaperPath(day, L""), "empty path is distinct from a theme image");
    WallpaperTracker tracker;
    WallpaperSnapshot current = {{L"a", day}, {L"b", day}};
    auto observe = [&](ULONGLONG now) { return tracker.Observe(current, day, night, now); };
    observe(0);
    tracker.Begin(night, 4, 10);
    tracker.Applied(true);
    Check(observe(20) == Result::None && tracker.pending,
          "successful API call with unchanged desktop remains unconfirmed");
    current[0].second = night;
    Check(observe(30) == Result::None && tracker.pending && !tracker.Blocked(),
          "partial monitor completion keeps request pending");
    current[1].second = L"";
    Check(observe(40) == Result::None && tracker.pending && tracker.Blocked(),
          "transient empty monitor during write is inconclusive");
    current[1].second = night;
    Check(observe(50) == Result::Confirmed && !tracker.pending,
          "all monitors must confirm the requested image");
    Check(observe(60) == Result::None && !tracker.Blocked(), "duplicate notification is harmless");

    tracker.Begin(day, 4, 70); tracker.Applied(true);
    const auto oldGeneration = tracker.generation;
    tracker.Begin(night, 4, 80); tracker.Applied(true);
    current[0].second = day;
    Check(observe(90) == Result::None && tracker.generation > oldGeneration && tracker.target == night,
          "rapid reversal cannot confirm a superseded request");
    current[0].second = night;
    Check(observe(100) == Result::Confirmed, "latest request eventually confirms");

    tracker.Begin(day, 4, 200); tracker.Applied(true);
    current[0].second = L"C:\\manual.jpg";
    Check(observe(210) == Result::None && tracker.Blocked(),
          "manual change during outstanding write immediately blocks automatic writes");
    Check(observe(700) == Result::None, "less than one second cannot confirm external change");
    Check(observe(1210) == Result::External, "manual change during transition is not ignored");
    tracker.ResetSelection();
    current = {{L"a", day}, {L"b", day}}; observe(2000);
    current[0].second = L"C:\\manual.jpg"; observe(2010);
    tracker.ReadFailed();
    Check(observe(4000) == Result::None && tracker.Blocked(), "read failure restarts external confirmation");
    current[0].second = night;
    Check(observe(4500) == Result::None && !tracker.Blocked(), "return to theme cancels external candidate");
    current[0].second = L"C:\\manual-a.jpg"; observe(5000);
    current[0].second = L"C:\\manual-b.jpg";
    Check(observe(6000) == Result::None, "changing external candidate restarts confirmation");
    Check(observe(7000) == Result::External, "stable replacement is confirmed");

    tracker.ResetSelection();
    current = {{L"a", day}, {L"b", day}}; observe(8000);
    current.push_back({L"c", L"C:\\other.jpg"});
    Check(observe(8010) == Result::None && !tracker.Blocked(), "new monitor adopts its own baseline");
    std::reverse(current.begin(), current.end());
    current.pop_back();
    Check(observe(8020) == Result::None && !tracker.Blocked(), "monitor reorder and removal preserve theme");
    current[0].second = L"C:\\manual.jpg"; observe(8030);
    current.erase(current.begin());
    Check(observe(9500) == Result::None && !tracker.Blocked(), "removed monitor cancels its external candidate");

    tracker.ResetSelection();
    current = {{L"a", day}}; observe(10000);
    for (unsigned attempt = 0; attempt < WallpaperTracker::maxAttempts; ++attempt) {
        ULONGLONG now = 10100 + attempt * 6000;
        Check(tracker.CanRequest(night, 4), "bounded retry is available");
        tracker.Begin(night, 4, now); tracker.Applied(true);
        Check(observe(now + WallpaperTracker::settleMs) == Result::TimedOut,
              "unchanged desktop times out without leaving theme");
    }
    Check(!tracker.CanRequest(night, 4), "three unconfirmed requests stop automatic retries");
    current[0].second = night;
    Check(observe(30000) == Result::Confirmed, "late successful update is still recognized");
    tracker.Begin(day, 4, 31000); tracker.Applied(true);
    current[0].second = L"";
    observe(31010);
    Check(observe(36000) == Result::TimedOut && tracker.Blocked(),
          "persistent solid color is reconsidered after transition deadline");
    Check(observe(37000) == Result::External, "persistent solid color exits after confirmation");

    tracker.ResetSelection();
    current = {{L"a", day}}; observe(40000);
    for (unsigned attempt = 0; attempt < WallpaperTracker::maxAttempts; ++attempt) {
        tracker.Begin(night, 4, 40010 + attempt); tracker.Applied(false);
    }
    Check(!tracker.CanRequest(night, 4) && tracker.CanRequest(day, 4),
          "API failure retry budget is per target and position");
    tracker.ResetSelection(); observe(41000);
    Check(tracker.CanRequest(night, 4) && !tracker.Blocked(), "explicit selection clears retry and candidate state");
    std::puts("PASS: delayed writes, rapid reversal, stable external confirmation, topology and bounded retries");
}

static void TestWallpaperState() {
    g_app.cfg = Config{};
    g_app.simulateHour = 12;
    ThemeInfo theme;
    theme.name = L"Test";
    theme.dayImage = L"day.jpg";
    theme.nightImage = L"night.jpg";
    g_app.themes = {theme};
    ApplyCurrent(true);
    Check(applyCalls == 1 && lastImage == theme.dayImage, "initial wallpaper");
    SendMessageW(g_app.hwnd, WM_TIMER, TM_TICK, 0);
    Check(applyCalls == 1, "first scheduler tick skips successful wallpaper");
    ApplyCurrent(true);
    Check(applyCalls == 2, "explicit apply bypasses cache");
    applySucceeds = false;
    ApplyCurrent(true);
    Check(applyCalls == 3 && g_app.appliedImage.empty(), "failed force clears cache");
    ApplyCurrent(false);
    Check(applyCalls == 4, "failed apply is retried");
    applySucceeds = true;
    ApplyCurrent(false);
    ApplyCurrent(false);
    Check(applyCalls == 5, "retry success restores cache");
    g_app.cfg.wallpaperStyle = 3;
    ApplyCurrent(false);
    Check(applyCalls == 6 && lastStyle == 3, "style change invalidates cache");
    g_app.cfg.autoSwitch = false;
    g_app.simulateHour = 23;
    ApplyCurrent(false);
    Check(applyCalls == 6, "disabled auto switch skips schedule");
    ApplyCurrent(true);
    Check(applyCalls == 7 && lastImage == theme.nightImage, "manual night while paused");
    g_app.cfg.autoSwitch = true;
    g_app.simulateHour = 12;
    duringApply = [] { ApplyCurrent(false); };
    ApplyCurrent(false);
    Check(applyCalls == 8 && !g_app.applyingWallpaper, "nested broadcast cannot reenter apply");
    duringApply = [] {
        g_app.themes.clear();
        g_app.cfg.wallpaperStyle = 2;
    };
    ApplyCurrent(true);
    Check(applyCalls == 9 && g_app.wallpaper.target == theme.dayImage &&
          g_app.wallpaper.style == 3, "request records applied values across nested changes");
    duringApply = {};
    ApplyCurrent(true);
    Check(applyCalls == 9, "empty theme list is safe");
    std::puts("PASS: wallpaper deduplication, retries, schedule, reentrancy");
}

static void TestDelayedWallpaper() {
    g_app.cfg = Config{};
    g_app.cfg.theme = L"Test";
    g_app.cfg.switchMode = 1;
    g_app.selIdx = 0;
    g_app.glass = false;
    ThemeInfo theme;
    theme.name = L"Test"; theme.dayImage = L"day.jpg"; theme.nightImage = L"night.jpg";
    g_app.themes = {theme};
    wallpaper = {{L"monitor-a", L"initial.jpg"}, {L"monitor-b", L"initial.jpg"}};
    g_app.wallpaper.ResetSelection();
    g_app.appliedImage.clear();
    appDark = false;
    ApplyCurrent(true);
    deferApply = true;
    appDark = true;
    SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, 0, 0);
    Check(g_app.appliedImage.empty() && g_app.wallpaper.pending,
          "API success does not populate cache before desktop confirmation");
    wallpaper[0].second = theme.nightImage;
    CheckExternalWallpaper();
    Check(!g_app.cfg.noTheme, "delayed first monitor does not deselect theme");
    wallpaper[1].second = theme.nightImage;
    SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, SPI_SETDESKWALLPAPER, 0);
    Check(!g_app.cfg.noTheme, "delayed wallpaper broadcast preserves theme");
    deferApply = false;
    appDark = false;
    SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, 0, 0);
    Check(!g_app.cfg.noTheme && lastImage == theme.dayImage,
          "following system still works after delayed completion");
    g_app.cfg.autoSwitch = false;
    wallpaper[0].second = theme.nightImage;
    SendMessageW(g_app.hwnd, WM_TIMER, TM_TICK, 0);
    Check(!g_app.cfg.noTheme && wallpaper[0].second == theme.nightImage,
          "paused theme accepts its other image without automatic writing");
    g_app.cfg.autoSwitch = true;
    SendMessageW(g_app.hwnd, WM_TIMER, TM_TICK, 0);
    Check(!g_app.cfg.noTheme && wallpaper[0].second == theme.dayImage,
          "resuming follows target even when previously applied cache matches");
    // A real external change during the next delayed write must block every
    // automatic entry point, including forced resume and mode changes.
    deferApply = true;
    appDark = true;
    SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, 0, 0);
    wallpaper[0].second = L"manual-during-transition.jpg";
    int calls = applyCalls;
    SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, 0, 0);
    SendMessageW(g_app.hwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    ApplyCurrent(true);
    Check(applyCalls == calls && !g_app.cfg.noTheme,
          "external candidate blocks automatic writes and forced resume");
    CheckExternalWallpaper(GetTickCount64() + WallpaperTracker::confirmMs);
    Check(g_app.cfg.noTheme && savedConfig.noTheme && applyCalls == calls,
          "external wallpaper during write is preserved and persisted");
    deferApply = false;
    SelectTheme(0);
    Check(!g_app.cfg.noTheme && !g_app.wallpaper.Blocked() && g_app.appliedImage == theme.nightImage,
          "explicit selection clears pending external state");
    appDark = false;
    std::puts("PASS: delayed system following, pause/resume and manual changes during writes");
}

static void TestNestedThemeSelection() {
    g_app.cfg = Config{};
    g_app.cfg.theme = L"Old";
    g_app.selIdx = 0;
    g_app.simulateHour = 12;
    g_app.glass = false;
    ThemeInfo oldTheme, newTheme;
    oldTheme.name = L"Old"; oldTheme.dayImage = L"old-day.jpg"; oldTheme.nightImage = L"old-night.jpg";
    newTheme.name = L"New"; newTheme.dayImage = L"new-day.jpg"; newTheme.nightImage = L"new-night.jpg";
    g_app.themes = {oldTheme, newTheme};
    wallpaper = {{L"a", L"initial.jpg"}};
    int calls = applyCalls;
    duringApply = [] { SelectTheme(0); SelectTheme(1); };
    ApplyCurrent(true, true);
    duringApply = {};
    Check(g_app.cfg.theme == newTheme.name && g_app.appliedImage.empty() && applyCalls == calls + 1,
          "nested selection cancels old request without committing its cache");
    MSG msg;
    while (PeekMessageW(&msg, g_app.hwnd, kApplySelectionMessage, kApplySelectionMessage, PM_REMOVE))
        DispatchMessageW(&msg);
    Check(!g_app.cfg.noTheme && g_app.appliedImage == newTheme.dayImage && applyCalls == calls + 2,
          "nested selections coalesce and apply only the latest theme");
    g_app.cfg.autoSwitch = false;
    wallpaper[0].second = oldTheme.nightImage;
    CheckExternalWallpaper();
    CheckExternalWallpaper(GetTickCount64() + WallpaperTracker::confirmMs);
    Check(g_app.cfg.noTheme, "old theme folder is not a blanket whitelist");
    SelectTheme(0);
    duringRead = [] {
        SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, 0, 0);
        g_app.themes = {g_app.themes[1]};
        SelectTheme(0);
    };
    CheckExternalWallpaper();
    duringRead = {};
    Check(g_app.cfg.theme == newTheme.name && g_app.appliedImage.empty(),
          "selection during snapshot read invalidates the previous result");
    while (PeekMessageW(&msg, g_app.hwnd, kApplySelectionMessage, kApplySelectionMessage, PM_REMOVE))
        DispatchMessageW(&msg);
    Check(g_app.appliedImage == newTheme.dayImage && !g_app.selectionDeferred && !g_app.selectionQueued,
          "snapshot reentrancy defers and applies latest selection safely");
    std::puts("PASS: nested selection generation and deferred latest-theme apply");
}

static void TestExternalWallpaper() {
    g_app.cfg = Config{};
    g_app.cfg.theme = L"Test";
    g_app.selIdx = 0;
    g_app.simulateHour = 12;
    ThemeInfo theme;
    theme.name = L"Test"; theme.dayImage = L"day.jpg"; theme.nightImage = L"night.jpg";
    g_app.themes = {theme};
    wallpaper = {{L"monitor-a", L"initial.jpg"}, {L"monitor-b", L"initial.jpg"}};
    g_app.wallpaper.ResetSelection();
    g_app.appliedImage.clear();
    g_app.glass = false;
    ApplyCurrent(true);
    int calls = applyCalls;
    CheckExternalWallpaper();
    Check(!g_app.cfg.noTheme, "application's wallpaper changes do not deselect theme");
    wallpaper.push_back({L"monitor-c", L"other.jpg"});
    CheckExternalWallpaper();
    Check(!g_app.cfg.noTheme, "new monitor is not mistaken for an external change");
    wallpaper[1].second = L"manual.jpg";
    snapshotAvailable = false;
    CheckExternalWallpaper();
    Check(!g_app.cfg.noTheme, "failed read does not clear selection");
    snapshotAvailable = true;
    g_app.cfg.autoSwitch = false;
    SendMessageW(g_app.hwnd, WM_TIMER, TM_TICK, 0);
    Check(!g_app.cfg.noTheme && applyCalls == calls,
          "external change waits for confirmation without overwriting wallpaper");
    CheckExternalWallpaper(GetTickCount64() + WallpaperTracker::confirmMs);
    Check(g_app.cfg.noTheme && g_app.selIdx == -1 && savedConfig.noTheme &&
          savedConfig.theme.empty(), "manual change while paused persists no-theme state");
    g_app.cfg = savedConfig;
    g_app.selIdx = 0;
    Check(SelectedTheme() == nullptr, "reloaded no-theme state has no selected theme");
    g_app.cfg.autoSwitch = true;
    ApplyCurrent(true);
    SendMessageW(g_app.hwnd, WM_TIMER, TM_TICK, 0);
    Check(applyCalls == calls, "no-theme state never reapplies automatically or forcibly");
    SelectTheme(0);
    Check(!g_app.cfg.noTheme && !savedConfig.noTheme && savedConfig.theme == theme.name &&
          applyCalls == calls + 1, "choosing a theme resumes and persists selection");
    calls = applyCalls;
    wallpaper[0].second = L"";
    CheckExternalWallpaper();
    Check(!g_app.cfg.noTheme, "solid color waits for confirmation");
    CheckExternalWallpaper(GetTickCount64() + WallpaperTracker::confirmMs);
    Check(g_app.cfg.noTheme && applyCalls == calls, "solid color background exits theme");
    std::puts("PASS: manual wallpaper changes, monitors, failure, persistence and re-selection");
}

static void TestFollowAppMode() {
    g_app.cfg = Config{};
    g_app.cfg.theme = L"Test";
    g_app.cfg.switchMode = 1;
    g_app.cfg.appearance = 0;
    g_app.selIdx = 0;
    appDark = false;
    RefreshPalette();
    Check(!g_app.pal.dark && TargetStage() == 0, "light application mode controls UI and wallpaper");
    appDark = true;
    RefreshPalette();
    Check(g_app.pal.dark && TargetStage() == 1, "dark application mode controls UI and wallpaper");
    g_app.cfg.appearance = 1;
    RefreshPalette();
    Check(!g_app.pal.dark && TargetStage() == 1, "explicit UI override leaves wallpaper following applications");
    appDark = false;
    std::puts("PASS: UI and wallpaper follow Windows application mode");
}

static IDXGISurface* TargetSurface(float* dpi = nullptr) {
    ID2D1Image* image = nullptr;
    g_app.gfx.dc()->GetTarget(&image);
    Check(image != nullptr, "render target exists");
    ID2D1Bitmap1* bitmap = nullptr;
    HRESULT hr = image->QueryInterface(IID_ID2D1Bitmap1, (void**)&bitmap);
    image->Release();
    Check(SUCCEEDED(hr), "target is bitmap");
    if (dpi) { float y; bitmap->GetDpi(dpi, &y); }
    IDXGISurface* surface = nullptr;
    hr = bitmap->GetSurface(&surface);
    bitmap->Release();
    Check(SUCCEEDED(hr), "target has DXGI surface");
    return surface;
}

static void TestLazyGraphicsAndDpi() {
    g_app.glass = true;
    EnsureGlassBitmap(true);
    RenderFrame();
    StartAnim();
    Check(!g_app.gfx.dc() && !g_glass.small && !g_timerPrecisionUsers,
          "hidden startup does not initialize graphics or animation timers");
    g_app.glass = false;
    g_app.solidBg = true;
    Check(EnsureUiInitialized(), "first UI initialization");
    auto* context = g_app.gfx.dc();
    Check(EnsureUiInitialized() && context == g_app.gfx.dc(), "UI initialization is idempotent");
    UINT dpi = GetDpiForWindow(g_app.hwnd);
    float targetDpi = 0;
    IDXGISurface* before = TargetSurface(&targetDpi);
    Check(std::fabs(targetDpi - dpi) < 0.01f && g_app.gfx.scale() == dpi / 96.0f,
          "initial target uses window DPI");
    RECT rc;
    GetClientRect(g_app.hwnd, &rc);
    g_app.gfx.resize(rc.right, rc.bottom, dpi + 48);
    IDXGISurface* after = TargetSurface(&targetDpi);
    IUnknown *a = nullptr, *b = nullptr;
    before->QueryInterface(IID_IUnknown, (void**)&a);
    after->QueryInterface(IID_IUnknown, (void**)&b);
    Check(a == b && std::fabs(targetDpi - (dpi + 48)) < 0.01f,
          "DPI change reuses actual swapchain buffer");
    a->Release(); b->Release(); before->Release(); after->Release();
    g_app.gfx.resize(rc.right, rc.bottom, dpi);
    Check(g_app.gfx.begin(), "drawing remains available after DPI roundtrip");
    g_app.gfx.dc()->Clear(D2D1::ColorF(0, 0.0f));
    g_app.gfx.end();
    {
        AnimationTimerPrecision outer, inner;
        outer.acquire(); outer.acquire(); inner.acquire();
        Check(g_timerPrecisionUsers == 2, "nested precision request is balanced");
        inner.release();
        Check(g_timerPrecisionUsers == 1, "nested precision release preserves outer request");
    }
    Check(g_timerPrecisionUsers == 0, "idle releases timer precision");
    std::puts("PASS: lazy graphics, real GPU buffer reuse on DPI change, timer lifetime");
}

static void TestNativeMinimize() {
    HWND h = g_app.hwnd;
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    Check((style & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW && !(style & WS_POPUP),
          "main window retains standard caption and system controls");
    RECT outer, client;
    GetWindowRect(h, &outer); GetClientRect(h, &client);
    Check(outer.right - outer.left == client.right && outer.bottom - outer.top == client.bottom,
          "standard window style keeps the custom full-client frame");
    ShowWindow(h, SW_SHOWNOACTIVATE);
    TrackNormRect();
    WINDOWPLACEMENT normal = {sizeof(normal)};
    GetWindowPlacement(h, &normal);
    const RECT normRect = g_app.normRect;
    const float width = g_app.winWDip, height = g_app.winHDip;

    // Click the actual custom caption button while the startup fade is unfinished.
    g_app.winAnim = 0.35f;
    g_app.winAnimating = true;
    const auto& button = g_app.L.captionMin;
    LPARAM point = MAKELPARAM((int)((button.left + button.right) * 0.5f * g_app.gfx.scale()),
                             (int)((button.top + button.bottom) * 0.5f * g_app.gfx.scale()));
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(h, WM_LBUTTONUP, 0, point);
    Check(IsIconic(h), "custom caption button minimizes through Windows");
    Check(g_app.winWDip == width && g_app.winHDip == height,
          "minimizing does not resize the content layout to icon dimensions");
    Check(!g_app.winAnimating && !g_app.hideAfterAnim && g_app.winAnim == 1.0f,
          "minimizing cancels the unfinished fade");
    WINDOWPLACEMENT minimized = {sizeof(minimized)};
    GetWindowPlacement(h, &minimized);
    Check(EqualRect(&normal.rcNormalPosition, &minimized.rcNormalPosition),
          "minimizing preserves the system restore rectangle");
    Check(EqualRect(&normRect, &g_app.normRect), "minimizing preserves the maximize restore rectangle");
    ShowMainWindow();
    GetWindowRect(h, &outer);
    Check(!IsIconic(h) && !IsZoomed(h) && IsWindowVisible(h) && EqualRect(&outer, &normRect),
          "tray or second-instance show restores the normal position");
    Check(!g_app.winAnimating && g_app.winAnim == 1.0f,
          "restoring from the taskbar adds no application fade");

    // A pending tray hide must not run after a native minimize / restore cycle.
    StartWindowAnim(false);
    SendMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE | 1, 0);
    SendMessageW(h, WM_SYSCOMMAND, SC_RESTORE | 1, 0);
    StepAnim();
    Check(!IsIconic(h) && IsWindowVisible(h) && !g_app.hideAfterAnim,
          "system menu minimize/restore cancels a pending tray hide");

    const float zoomMs = g_app.zoomMs;
    g_app.zoomMs = 40.0f;
    StartZoom(true);
    Check(IsZoomed(h), "existing maximize transition still reaches maximized state");
    SendMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    GetWindowPlacement(h, &minimized);
    Check(IsIconic(h) && (minimized.flags & WPF_RESTORETOMAXIMIZED),
          "Windows remembers minimize from maximized state");
    Check(EqualRect(&normal.rcNormalPosition, &minimized.rcNormalPosition),
          "maximized minimize preserves the original normal placement");
    SendMessageW(h, WM_SYSCOMMAND, SC_RESTORE, 0);
    Check(IsZoomed(h) && g_app.maximized && !IsIconic(h),
          "system restore returns a minimized maximized window to maximized state");
    SendMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    ShowMainWindow();
    Check(IsZoomed(h) && !IsIconic(h) && !g_app.winAnimating,
          "tray restore also preserves maximized state without a custom fade");
    StartZoom(false);
    GetWindowRect(h, &outer);
    Check(!IsZoomed(h) && EqualRect(&outer, &normRect),
          "restore from maximized still returns to the original position");
    g_app.zoomMs = zoomMs;
    ShowWindow(h, SW_HIDE);
    std::puts("PASS: native minimize/restore, placement, maximized state, fade interruption");
}

static std::wstring LayoutFont(IDWriteTextLayout* layout, UINT32 position) {
    wchar_t family[256] = {};
    Check(SUCCEEDED(layout->GetFontFamilyName(position, family, ARRAYSIZE(family))), "read font run");
    return family;
}

static void TestLocalization() {
    GUID id;
    wchar_t name[64] = {};
    Check(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, name, 64), "create unique locale test key name");
    const std::wstring keyPath = std::wstring(L"Software\\DynamicWallpapersLocaleTest-") + name;
    HKEY key = nullptr;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                         KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS,
          "create isolated locale test key");
    const bool fallback = ReadInstallLanguage(key, true) && !ReadInstallLanguage(key, false);
    const bool chinese = WriteInstallLanguage(key, true) == ERROR_SUCCESS && ReadInstallLanguage(key, false);
    const bool english = WriteInstallLanguage(key, false) == ERROR_SUCCESS && !ReadInstallLanguage(key, true);
    RegCloseKey(key);
    RegDeleteKeyW(HKEY_CURRENT_USER, keyPath.c_str());
    Check(fallback, "missing install language follows Windows");
    Check(chinese, "Chinese choice survives registry handoff on an English system");
    Check(english, "English choice survives registry handoff on a Chinese system");

    g_appChinese = chinese;
    g_app.gfx.setTextLocale(g_appChinese);
    Check(std::wstring(AppText(L"Themes", L"主题")) == L"主题" && FormatDuration(65) == L"1 小时 5 分钟",
          "main window and core strings use the same language");
    auto* zh = g_app.gfx.format(FONT_UI, 14.0f);
    auto* en = g_app.gfx.format(FONT_UI, 14.0f, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_TEXT_ALIGNMENT_LEADING, false, L"en-US");
    wchar_t zhLocale[16] = {}, enLocale[16] = {};
    Check(zh && en && zh != en && SUCCEEDED(zh->GetLocaleName(zhLocale, 16)) &&
          SUCCEEDED(en->GetLocaleName(enLocale, 16)) && lstrcmpiW(zhLocale, L"zh-CN") == 0 &&
          lstrcmpiW(enLocale, L"en-US") == 0, "font cache separates Chinese and English layouts");
    auto* fallbackFace = g_app.gfx.format(L"DynamicWallpapersMissingTestFont,Segoe UI", 14.0f);
    wchar_t family[64] = {};
    Check(fallbackFace && SUCCEEDED(fallbackFace->GetFontFamilyName(family, 64)) &&
          lstrcmpiW(family, L"Segoe UI") == 0, "missing fonts skip to a real installed family");
    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    Check(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0), "read Windows message font");
    Check(SUCCEEDED(en->GetFontFamilyName(family, ARRAYSIZE(family))), "read system UI font");
    const std::wstring systemFamily = family;
    Check(SUCCEEDED(zh->GetFontFamilyName(family, ARRAYSIZE(family))) && systemFamily == family &&
          (systemFamily == metrics.lfMessageFont.lfFaceName || systemFamily == L"Segoe UI"),
          "both UI languages use Windows typography by default");
    Check(ResolveAppLanguage(AppLanguage::System, true) && !ResolveAppLanguage(AppLanguage::System, false) &&
          ResolveAppLanguage(AppLanguage::Chinese, false) && !ResolveAppLanguage(AppLanguage::English, true) &&
          ParseAppLanguage(AppLanguageTag(AppLanguage::Chinese)) == AppLanguage::Chinese &&
          ParseAppLanguage(L"invalid") == AppLanguage::System, "default, forced and saved application languages");
    Check(DecodeFontPreference(EncodeFontPreference(L"\u5fae\u8f6f\u96c5\u9ed1 2")) == L"\u5fae\u8f6f\u96c5\u9ed1 2" &&
          DecodeFontPreference(L"Tahoma") == L"Tahoma" && DecodeFontPreference(L"u16:broken").empty() &&
          DecodeFontPreference(EncodeFontPreference(L"")).empty(), "new, legacy, invalid and default font preference values");

    const auto fonts = g_app.gfx.installedFonts();
    Check(std::any_of(fonts.begin(), fonts.end(), [](const auto& f) { return f.name == L"Arial" && !f.label.empty(); }),
          "enumerate local font names and display labels");
    const wchar_t* mixed = L"A\u4E2D\U00020000\uFF0Cz";
    auto* layout = g_app.gfx.textLayout(mixed, en, 400, 100);
    wchar_t scriptLocale[16] = {};
    Check(layout && LayoutFont(layout, 0) == systemFamily && LayoutFont(layout, 1) == systemFamily &&
          SUCCEEDED(layout->GetLocaleName(1, scriptLocale, ARRAYSIZE(scriptLocale))) && lstrcmpiW(scriptLocale, L"zh-CN") == 0,
          "system retains base font and supplies appropriate Chinese fallback locale in English UI");
    IDWriteTextLayout2* layout2 = nullptr;
    IDWriteFontFallback* forcedFallback = nullptr;
    Check(SUCCEEDED(layout->QueryInterface(IID_PPV_ARGS(&layout2))) &&
          SUCCEEDED(layout2->GetFontFallback(&forcedFallback)) && !forcedFallback,
          "default layout does not override the Windows fallback chain");
    layout2->Release();
    layout->Release();
    g_app.gfx.setUiFonts(L"Tahoma", L"Arial");
    layout = g_app.gfx.textLayout(mixed, en, 400, 100);
    Check(layout && LayoutFont(layout, 0) == L"Arial" && LayoutFont(layout, 1) == L"Tahoma" &&
          LayoutFont(layout, 2) == L"Tahoma" && LayoutFont(layout, 3) == L"Tahoma" &&
          LayoutFont(layout, 4) == L"Tahoma" && LayoutFont(layout, 5) == L"Arial",
          "each script uses its selected family in the same layout, including supplementary Han");
    layout->Release();
    layout = g_app.gfx.textLayout(L"A", fallbackFace, 400, 100);
    Check(layout && LayoutFont(layout, 0) == L"Segoe UI", "explicit families and icon formats bypass custom fonts");
    layout->Release();
    g_app.gfx.setUiFonts(L"DynamicWallpapersMissingTestFont", L"Arial");
    layout = g_app.gfx.textLayout(mixed, en, 400, 100);
    Check(layout && LayoutFont(layout, 1) == systemFamily && LayoutFont(layout, 0) == L"Arial",
          "unavailable preference falls back without affecting the other script");
    layout->Release();
    g_app.gfx.setUiFonts(L"", L"");
    g_appChinese = false;
    g_app.gfx.setTextLocale(false);
    std::puts("PASS: independent installer/app languages, localized text and mixed DirectWrite fonts");
}

static void TestAppearancePickersAndResize() {
    const Config previous = g_app.cfg;
    HWND h = g_app.hwnd;
    RECT before = {};
    GetWindowRect(h, &before);
    const float scale = g_app.gfx.scale();
    MINMAXINFO limits = {};
    SendMessageW(h, WM_GETMINMAXINFO, 0, (LPARAM)&limits);
    Check(limits.ptMinTrackSize.x < kWinW * scale && limits.ptMinTrackSize.y < kWinH * scale,
          "native tracking permits shrinking below both startup dimensions");
    SetWindowPos(h, nullptr, 0, 0, limits.ptMinTrackSize.x, limits.ptMinTrackSize.y, SWP_NOMOVE | SWP_NOZORDER);
    const auto& L = g_app.L;
    Check(L.maxScroll > 0 && L.ovCard.top >= Ui::TitleBarH && L.ovOk.bottom < g_app.winHDip &&
          L.ovCard.left >= 0 && L.ovCard.right <= g_app.winWDip,
          "small client scrolls and still fits the largest modal");

    g_app.cfg.language = AppLanguage::English;
    g_app.languageAnim = (float)g_app.cfg.language;
    g_app.preferenceMotion = true;
    RefreshAppLanguage();
    g_app.statusMsg = L"Old message";
    SendMessageW(h, WM_KEYDOWN, VK_TAB, 0);
    Check(g_app.picker.focus == 0 && g_app.picker.open < 0 &&
          L.preferences[0].bottom - g_app.scroll < g_app.winHDip,
          "keyboard reveals and focuses the language segments without a dropdown");
    const auto& lang = L.preferences[0];
    const float langY = (lang.top + lang.bottom) / 2 - g_app.scroll;
    for (int i = 0; i < 3; ++i)
        Check(HitTest(lang.left + (i + 0.5f) * (lang.right - lang.left) / 3, langY) == CID_LANG0 + i,
              "each language segment has its own hit target");
    SendMessageW(h, WM_KEYDOWN, VK_LEFT, 0);
    Check(g_app.picker.open < 0 && g_appChinese && savedConfig.language == AppLanguage::Chinese &&
          g_app.statusMsg.empty() && FormatDuration(60) == L"1 小时", "language applies and saves without restarting");
    StepPreferenceMotion(0.5f, 0.5f, 0.5f);
    Check(g_app.languageAnim > 1 && g_app.languageAnim < 2, "language indicator animates between segments");
    SendMessageW(h, WM_KEYDOWN, VK_TAB, 0);
    SendMessageW(h, WM_KEYDOWN, VK_RETURN, 0);
    const auto& c = g_app.picker;
    Check(c.open == 1 && FontChoiceCount() > c.rows && std::wstring(FontChoiceLabel(0)) == L"跟随系统",
          "font picker puts system before installed fonts");
    const float x = c.popup.left + 30;
    const float oppositeEdge = c.dropsBelow ? c.popup.bottom - 12 : c.popup.top + 12;
    Check(c.reveal == 0 && HitPreferenceItem(x, oppositeEdge) < 0, "opening starts hidden and clipped rows cannot be hit");
    StepPreferenceMotion(0.5f, 0.5f, 0.5f);
    const auto half = RevealedFontPopup(c);
    Check(c.reveal == 0.5f && (c.dropsBelow ? half.top == c.popup.top : half.bottom == c.popup.bottom) &&
          HitPreferenceItem(x, oppositeEdge) < 0, "partial reveal is anchored to the font control");
    auto finishMotion = []() { for (int i = 0; i < 25; ++i) StepPreferenceMotion(0.5f, 0.5f, 0.5f); };
    finishMotion();
    Check(c.reveal == 1 && HitPreferenceItem(x, oppositeEdge) >= 0, "completed reveal exposes all visible rows");

    SendMessageW(h, WM_KEYDOWN, VK_HOME, 0);
    finishMotion();
    SendMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, -15), 0);
    Check(c.scroll == 0 && c.scrollTarget == 12, "high-resolution wheel or touchpad delta sets a fractional-row target");
    StepPreferenceMotion(0.5f, 0.5f, 0.5f);
    Check(c.scroll == 6 && HitPreferenceItem(x, c.popup.top + kPickerPad + 28) == 1,
          "pointer hits the row drawn at the intermediate scroll offset");
    SendMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    Check(c.scroll == 6 && c.scrollTarget == 108, "a wheel notch adds to the scroll target without jumping the list");
    finishMotion();
    Check(c.scroll == 108, "wheel animation settles at its target");

    SendMessageW(h, WM_KEYDOWN, VK_HOME, 0);
    finishMotion();
    for (int i = 0; i < c.rows; ++i) SendMessageW(h, WM_KEYDOWN, VK_DOWN, 0);
    Check(c.highlighted == c.rows && c.scroll == 0 && c.scrollTarget == kPickerRow,
          "arrowing past the viewport sets a nearby scroll target");
    StepPreferenceMotion(0.5f, 0.5f, 0.5f);
    Check(c.scroll == kPickerRow / 2, "keyboard scrolling moves through an intermediate offset");
    SendMessageW(h, WM_KEYDOWN, VK_END, 0);
    finishMotion();
    Check(c.scroll == FontScrollLimit(c), "long font list navigates to the last page");
    const int last = c.highlighted;
    const auto expected = g_app.installedFonts[last - 1].name;
    const auto point = MAKELPARAM((int)((c.popup.left + 30) * scale),
        (int)((c.popup.top + kPickerPad + (c.rows - 0.5f) * kPickerRow) * scale));
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(h, WM_LBUTTONUP, 0, point);
    Check(c.open < 0 && c.displayed == 1 && HitPreferenceItem(x, oppositeEdge) < 0 && savedConfig.chineseFont == expected &&
          savedConfig.englishFont == previous.englishFont, "mouse selects and saves only the targeted script");
    StepPreferenceMotion(0.5f, 0.5f, 0.5f);
    Check(c.reveal == 0.5f && c.displayed == 1, "close keeps the dropdown visible during the collapse");
    finishMotion();
    Check(c.reveal == 0 && c.displayed < 0, "close animation removes the dropdown when settled");
    g_app.preferenceMotion = false;
    OpenPreferencePicker(2);
    SendMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, -15), 0);
    Check(c.reveal == 1 && c.scroll == c.scrollTarget, "motion disabled makes appearance and scrolling immediate");
    SendMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0);
    Check(c.displayed < 0, "motion disabled closes immediately");
    g_app.cfg = previous;
    RefreshAppLanguage();
    g_app.gfx.setUiFonts(previous.chineseFont, previous.englishFont);
    g_app.preferenceMotion = true;
    g_app.languageAnim = (float)previous.language;
    g_app.picker = {};
    g_app.scroll = g_app.scrollTarget = 0;
    SetWindowPos(h, nullptr, 0, 0, before.right - before.left, before.bottom - before.top, SWP_NOMOVE | SWP_NOZORDER);
    std::puts("PASS: language segments, dropdown expand/collapse, wheel and keyboard easing, and small-window layout");
}

static std::wstring MakeBitmap(const wchar_t* name, LONG width, LONG height) {
    std::wstring path = JoinPath(ExeDir(), name);
    BITMAPFILEHEADER file = {};
    BITMAPINFOHEADER info = {};
    info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::vector<DWORD> pixels((size_t)width * height, 0xFF3070B0);
    file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + (DWORD)(pixels.size() * sizeof(DWORD));
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(handle != INVALID_HANDLE_VALUE, "create isolated image fixture");
    DWORD written;
    bool ok = WriteFile(handle, &file, sizeof(file), &written, nullptr) &&
              WriteFile(handle, &info, sizeof(info), &written, nullptr) &&
              WriteFile(handle, pixels.data(), (DWORD)(pixels.size() * 4), &written, nullptr);
    CloseHandle(handle);
    Check(ok, "write isolated image fixture");
    return path;
}

static std::vector<ThumbResult> WaitResults(ThumbLoader& loader, size_t count) {
    std::vector<ThumbResult> results;
    ULONGLONG until = GetTickCount64() + 5000;
    while (results.size() < count && GetTickCount64() < until) {
        auto ready = loader.takeResults();
        for (auto& result : ready) results.push_back(std::move(result));
        if (results.size() < count) Sleep(1);
    }
    Check(results.size() == count, "background decode finishes within timeout");
    return results;
}

static void TestThumbnails() {
    auto normal = MakeBitmap(L"thumbnail-fixture.bmp", 900, 600);
    auto tall = MakeBitmap(L"thumbnail-tall.bmp", 1, 8192);
    ThumbLoader loader;
    const UINT message = WM_APP + 20;
    loader.submit(g_app.hwnd, message, 1, {{1, 0, normal}, {1, 1, tall}, {1, 2, normal + L".missing"}});
    auto decoded = WaitResults(loader, 3);
    Check(decoded[0].width == 420 && decoded[0].height == 280 &&
          decoded[0].pixels.size() == 420 * 280 * 4, "thumbnail is scaled PBGRA");
    Check(decoded[1].width == 1 && decoded[1].height == 4096, "extreme aspect ratio is bounded");
    Check(decoded[2].pixels.empty(), "missing file returns a failure result");
    for (size_t gen = 2; gen <= 60; ++gen) {
        loader.cancel(gen);
        loader.submit(g_app.hwnd, message, gen, {{gen, 0, normal}, {gen, 1, normal}});
    }
    auto latest = WaitResults(loader, 2);
    Check(latest[0].request.generation == 60 && latest[1].request.generation == 60,
          "rapid viewport replacement discards obsolete generations");
    loader.submit(g_app.hwnd, message, 60, {{60, 2, normal}, {60, 3, normal}});
    loader.stop(); loader.stop();
    Check(loader.takeResults().empty(), "shutdown joins worker and discards queued pixels");

    ThemeInfo theme;
    theme.name = L"Thumbnail fixture"; theme.dayImage = theme.nightImage = normal;
    g_app.themes.assign(48, theme);
    ComputeLayout(g_app.L);
    SyncThumbs();
    // Cancel work while scrolling and changing the theme list.
    g_app.scroll = g_app.scrollTarget = g_app.L.maxScroll;
    SyncThumbs();
    ClearThumbs();
    g_app.themes.assign(1, theme);
    g_app.scroll = g_app.scrollTarget = 0;
    ComputeLayout(g_app.L);
    SyncThumbs();
    ULONGLONG until = GetTickCount64() + 5000;
    while (!g_app.thumbs[0] && GetTickCount64() < until) {
        MSG msg;
        while (PeekMessageW(&msg, g_app.hwnd, kThumbReadyMessage, kThumbReadyMessage, PM_REMOVE))
            DispatchMessageW(&msg);
        Sleep(1);
    }
    Check(g_app.thumbs[0] && g_app.thumbs.size() == 1,
          "UI accepts current thumbnail after scroll and theme replacement");
    ClearThumbs();
    g_thumbLoader.stop();
    DeleteFileW(normal.c_str()); DeleteFileW(tall.c_str());
    std::puts("PASS: WIC decoding, missing images, cancellation, scrolling, UI upload, shutdown");
}

static void TestShellIdentity() {
    const std::wstring exe = ExePath();
    const std::wstring icon = JoinPath(ExeDir(), kShellIconFile) + L",0";
    SetShellWindowRelaunch(g_app.hwnd, exe, icon, L" --show", IDS_SHELL_TITLE, kShellAppId);
    IPropertyStore* store = nullptr;
    Check(SUCCEEDED(SHGetPropertyStoreForWindow(g_app.hwnd, IID_IPropertyStore, (void**)&store)),
          "read the window's Windows Shell properties");
    auto expect = [&](REFPROPERTYKEY key, const std::wstring& expected, const char* message) {
        PROPVARIANT value = {};
        Check(SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR &&
              value.pwszVal && expected == value.pwszVal, message);
        PropVariantClear(&value);
    };
    expect(PKEY_AppUserModel_ID, kShellAppId, "window uses the app's explicit Shell identity");
    expect(PKEY_AppUserModel_RelaunchIconResource, icon, "Shell uses a versioned icon path");
    expect(PKEY_AppUserModel_RelaunchCommand, L"\"" + exe + L"\" --show", "Shell relaunch opens the window");
    expect(PKEY_AppUserModel_RelaunchDisplayNameResource, L"@" + exe + L",-" + std::to_wstring(IDS_SHELL_TITLE),
           "Shell relaunch uses the embedded app title");
    store->Release();
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "initialize COM");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DynamicWallpapersIsolatedTests";
    Check(RegisterClassW(&wc) != 0, "register isolated window class");
    g_app.hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName,
        L"Dynamic Wallpapers isolated tests", kMainWindowStyle, 0, 0, 700, 764,
        nullptr, nullptr, wc.hInstance, nullptr);
    Check(g_app.hwnd != nullptr, "create hidden isolated test window");
    if (argc > 1 && lstrcmpW(argv[1], L"--preview-localization") == 0) {
        if (argc > 2) g_app.cfg.language = ParseAppLanguage(argv[2]);
        RefreshAppLanguage();
        g_app.languageAnim = (float)g_app.cfg.language;
        RefreshPreferenceMotion();
        g_app.cfg.autoSwitch = false;
        g_app.cfg.noTheme = true;
        g_app.cfg.minimizeToTray = false;
        g_app.glass = false;
        g_app.solidBg = true;
        RefreshPalette();
        Check(EnsureUiInitialized(), "initialize localization preview");
        ShowWindow(g_app.hwnd, SW_SHOW);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        g_app.gfx.shutdown();
        CoUninitialize();
        return 0;
    }
    TestWallpaperState();
    TestWallpaperTracker();
    TestDelayedWallpaper();
    TestExternalWallpaper();
    TestNestedThemeSelection();
    TestFollowAppMode();
    TestLazyGraphicsAndDpi();
    TestLocalization();
    TestAppearancePickersAndResize();
    TestNativeMinimize();
    TestThumbnails();
    TestShellIdentity();
    DestroyWindow(g_app.hwnd);
    g_app.gfx.shutdown();
    CoUninitialize();
    std::printf("PASS: all %d checks (no wallpaper/config/autostart writes)\n", checks);
    return 0;
}
