// ============================================================================
// main.cpp - Window, layout, input, tray, and scheduled wallpaper switching.
//
// Use the day image between day-start and night-start, and the night image otherwise,
// or follow the system app theme. Save changes immediately to %APPDATA%\Dynamic Wallpapers\config.ini.
//
// Standard top-level window with a custom caption; DWM supplies Windows 11 corners and shadows.
// Windows handles minimize/taskbar restore; StartZoom animates maximize transitions.
//
// CLI: --show opens the window; --apply applies once and exits; --autostart starts in the tray.
// --set-theme=<name> / --add-theme=<folder> / --delete-theme=<name>.
// --uninstall removes the app; --simulate-hour=19 tests scheduling; --opaque disables blur.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>       // GET_X_LPARAM/GET_Y_LPARAM, GET_WHEEL_DELTA_WPARAM
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <imm.h>            // The theme name editor handles WM_IME_* composition messages.
#include <winternl.h>       // RTL_OSVERSIONINFOW selects acrylic support by Windows version.
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <cmath>            // powf for zoom animation easing.
// timeBeginPeriod(1) requests 1 ms timer precision; the default ~15.6 ms WM_TIMER tick
// cannot provide smooth animation on 120 Hz displays. This does not improve GetTickCount64,
// so animations use QPC timestamps instead (see qpcMs).
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);
#include "gfx.h"
#include "ui.h"
#include "core.h"
#include "resource.h"
#include "thumbs.h"
#include "wallpaper_state.h"
#include "install_locale.h"
#include "shell_identity.h"

#pragma comment(lib, "shell32")

// ---------------------------------------------------------------------------
// Constants and global state.
// ---------------------------------------------------------------------------
static const wchar_t* kWindowClass = L"DynamicWallpapersMainWindow";
// Keep caption styles for native minimize/restore; WM_NCCALCSIZE removes only the visible frame.
static constexpr DWORD kMainWindowStyle = WS_OVERLAPPEDWINDOW;
static const wchar_t* kAppDisplay = L"Dynamic Wallpapers";
static const wchar_t* kLegacyDisplay = L"\u58c1\u7eb8\u968f\u65f6\u95f4\u53d8\u5316";   // Exact legacy shortcut/Start menu name, retained only for migration.
static const float kWinW = 700.0f;      // Default window width in DIPs.
static const float kWinH = 788.0f;      // Default height in DIPs; must match ComputeLayout's natural content height.
                                        // Update this when adding or removing sections to avoid clipping the bottom.
static const float kPadX = 24.0f;
static const float kCardW = 152.0f;     // Theme card dimensions in DIPs.
static const float kCardH = 148.0f;
static const float kCardGapY = 16.0f;   // Vertical gap between card rows in DIPs.
static const int   kMaxCards = 6;       // Maximum cards per row in wide windows.
static constexpr UINT kApplySelectionMessage = WM_APP + 6;
static const int   kThemeSlots = 48;    // Grid capacity also determines the theme control ID ranges.

// Theme control IDs are base ID plus theme index:
// 100..147=select, 200..247=delete, 300..347=rename.
enum CtrlId {
    CID_NONE = -1,
    CID_MIN = 1, CID_CLOSE, CID_MAX,
    CID_THEME0 = 100,
    CID_THEME_DEL0 = 200,
    CID_THEME_REN0 = 300,
    // Page scrollbar thumb and track; see DrawScrollbar / HitTest.
    CID_SCROLLBAR = 400, CID_SCROLLTRACK,
    CID_DAY_MINUS = 30, CID_DAY_PLUS, CID_NIGHT_MINUS, CID_NIGHT_PLUS,
    CID_AUTO = 40, CID_BOOT,
    CID_OPENTHEMES = 51, CID_ADD_THEME,
    CID_APPEARANCE0 = 60, CID_APPEARANCE1, CID_APPEARANCE2,
    CID_SWITCH0 = 64, CID_SWITCH1,          // Switch mode segments: schedule / system theme.
    CID_OV_CANCEL = 70, CID_OV_OK, CID_OV_NAME = 76, CID_OV_LIGHT_SLOT, CID_OV_DARK_SLOT,
    CID_DLG_CANCEL = 80, CID_DLG_OK,
    CID_REN_CANCEL = 83, CID_REN_OK, CID_REN_NAME,
};

enum TimerId { TM_TICK = 1, TM_ANIM = 2, TM_CARET = 3, TM_WALLPAPER = 4 };

// Recomputed layout shared by rendering and hit testing.
// Rectangles use content coordinates; window y = content y - scroll.
// The page scrolls as a whole; the title bar and dialogs remain fixed.
struct Layout {
    float contentL = kPadX, contentR = 700.0f - kPadX;   // Center and limit content width in wide windows.
    D2D1_RECT_F captionMin, captionClose, captionMax;
    std::vector<D2D1_RECT_F> themeCards;    // Wrapping card grid.
    std::vector<D2D1_RECT_F> themeDel;      // Top-right delete button.
    std::vector<D2D1_RECT_F> themeRen;      // Top-right rename button.
    int cardCount = 0;
    int perRow = 1;
    float contentH = 0;                     // Total content height; at least the viewport height.
    float visibleH = 0;
    float maxScroll = 0;
    float scrollTrackTop = 0, scrollTrackH = 0;   // Scrollbar track.
    int firstVis = 0, lastVis = -1;         // Inclusive range of visible card indices.
    D2D1_RECT_F addThemeBtn;
    D2D1_RECT_F dayStepper, nightStepper;
    D2D1_RECT_F appearanceSeg;
    D2D1_RECT_F switchSeg;
    D2D1_RECT_F autoToggle, bootToggle;
    D2D1_RECT_F openThemesBtn;
    D2D1_RECT_F themeSectionTitle, timeSectionTitle, appearanceSectionTitle, optionSectionTitle;
    D2D1_RECT_F switchSectionTitle;
    D2D1_RECT_F statusRow;
    D2D1_RECT_F ovCard, ovName;             // Add-theme dialog with two image slots and a name field.
    D2D1_RECT_F ovLightSlot, ovDarkSlot;
    D2D1_RECT_F ovCancel, ovOk;
    D2D1_RECT_F dlgCard, dlgCancel, dlgOk;  // Delete confirmation dialog.
    D2D1_RECT_F renCard, renField, renCancel, renOk;   // Rename dialog.
};

// Delete confirmation state; actual deletion requires confirmation.
struct DelUI {
    bool active = false;
    int idx = -1;
    std::wstring err;                       // Deletion error message.
};

// Custom editor state for adding and renaming themes.
// Supports text, arrows, Home/End, Backspace/Delete, Shift selection, Ctrl+A/V, and blinking caret.
// IME uses WM_IME_*; draw underlined preedit text at the caret.
struct TextEdit {
    std::wstring text;
    int caret = 0;                          // Caret position in 0..text.size().
    int sel = 0;                            // Selection anchor; sel == caret means no selection.
    std::wstring preedit;                   // IME preedit text.
    bool hasSel() const { return sel != caret; }
    void selRange(int& a, int& b) const { a = sel < caret ? sel : caret; b = sel < caret ? caret : sel; }
    void clearSel() { sel = caret; }
};

// Add theme: choose an image in each slot, then enter a name.
// A theme requires two distinct images: light/day and dark/night.
struct AddThemeUI {
    bool active = false;
    std::wstring lightPath, darkPath;
    ID2D1Bitmap* lightBmp = nullptr;        // Preview thumbnail.
    ID2D1Bitmap* darkBmp = nullptr;
    TextEdit name;
    std::wstring hint;
};

// Rename dialog opens from the card action with the current name prefilled.
struct RenameUI {
    bool active = false;
    int idx = -1;
    TextEdit name;
    std::wstring hint;
};

struct App {
    HWND hwnd = nullptr;
    Gfx gfx;
    Config cfg;
    std::vector<ThemeInfo> themes;
    int selIdx = 0;
    std::wstring appliedImage;               // Update only after all monitors confirm; retry failures a limited number of times.
    int appliedStyle = -1;
    bool applyingWallpaper = false;          // Prevent reentrancy from wallpaper settings broadcasts.
    bool readingWallpaper = false;
    bool selectionQueued = false;
    bool selectionDeferred = false;
    WallpaperTracker wallpaper;
    Palette pal;
    bool glass = true;                      // Custom frosted backdrop, forced by --self-glass.
    bool liveGlass = false;                 // System acrylic with live DWM blur, Windows 11 22H2+.
    bool solidBg = false;                   // Solid theme-aware background: default on Windows 10, or --solid.
    float winWDip = kWinW, winHDip = kWinH; // Current client size in DIPs, updated for maximize/snap layouts.
    bool maximized = false;
    // Animate maximize/restore rectangles; Windows handles minimize and taskbar restore.
    bool zooming = false;
    bool zoomToMax = false;
    RECT zoomFrom = {}, zoomTo = {};
    RECT normRect = {};                     // Track the restore rectangle independently of rcNormalPosition.
    double zoomStartMs = 0;                 // Animation start in QPC milliseconds, not GetTickCount64.
    float zoomMs = 200.0f;                  // Zoom duration, overridable with --zoom-ms=N.
    // scroll is current position; wheel changes scrollTarget, which the animation approaches.
    float scroll = 0.0f, scrollTarget = 0.0f;
    double lastScrollMs = 0;
    float sbAlpha = 0.0f;                   // Scrollbar opacity from 0 to 1 with fade transitions.
    bool sbDragging = false;
    float sbDragGrab = 0.0f;
    // Cache thumbnails by theme index for the viewport plus one buffer row; release offscreen entries.
    std::vector<ID2D1Bitmap*> thumbs;
    std::vector<char> thumbTried;           // Do not retry failed thumbnails every frame.
    std::vector<char> delVisible;           // Only hit-test card actions that were actually drawn in the previous frame.
    ID2D1Bitmap* logoBmp = nullptr;         // Embedded title-bar logo.
    bool logoTried = false;
    ID2D1LinearGradientBrush* bgBrush = nullptr;
    ID2D1Layer* uiLayer = nullptr;          // Whole-frame opacity for window fades and disabled schedule controls.
    Layout L;
    AddThemeUI addUI;
    DelUI delUI;
    RenameUI renUI;
    // Avoid nesting a disabled-section layer inside a whole-frame layer: D2D can truncate the frame.
    // Skipping the inner layer during the brief 150 ms fade is visually negligible.
    bool frameLayerPushed = false;
    bool caretOn = true;                    // Caret blink on a shared 500 ms timer.
    HIMC himc = nullptr;                    // Attach the IME context only while a text editor is open.

    // Interaction state.
    int hot = CID_NONE, pressed = CID_NONE;
    float toggleAnim[2] = {1.0f, 0.0f};     // 0=automatic switching, 1=start with Windows.
    float toggleTarget[2] = {1.0f, 0.0f};
    float segAnim = 0.0f;                   // Appearance indicator position in 0..2.
    int segTarget = 0;
    float segScale = 1.0f;                  // Shrink to 0.93 when pressed and spring back on release.
    bool segPressed = false;
    float swAnim = 0.0f;                    // Switch mode indicator in 0..1, using the appearance control's animation style.
    int swTarget = 0;
    float swScale = 1.0f;
    bool swPressed = false;
    bool trayAdded = false;
    HICON trayIcon = nullptr;               // Load the tray icon once, reuse on re-add, and destroy on WM_DESTROY.

    // Window show/hide animation: fade plus a slight upward slide.
    float winAnim = 1.0f;
    float winAnimTarget = 1.0f;
    bool winAnimating = false;
    bool hideAfterAnim = false;

    // Testing and diagnostics.
    int simulateHour = -1;
    std::wstring statusMsg;                 // Temporary status message, such as wallpaper applied.
    ULONGLONG statusMsgUntil = 0;
};
static App g_app;
static ThumbLoader g_thumbLoader;
static size_t g_thumbGeneration = 0;
static const UINT kThumbReadyMessage = WM_APP + 5;

// Request higher timer precision only during animation; nested animations share one request.
static unsigned g_timerPrecisionUsers = 0;
class AnimationTimerPrecision {
public:
    AnimationTimerPrecision() = default;
    AnimationTimerPrecision(const AnimationTimerPrecision&) = delete;
    AnimationTimerPrecision& operator=(const AnimationTimerPrecision&) = delete;
    ~AnimationTimerPrecision() { release(); }
    void acquire() {
        if (active_) return;
        if (!g_timerPrecisionUsers && timeBeginPeriod(1) != 0) return;
        ++g_timerPrecisionUsers;
        active_ = true;
    }
    void release() {
        if (!active_) return;
        active_ = false;
        if (--g_timerPrecisionUsers == 0) timeEndPeriod(1);
    }
private:
    bool active_ = false;
};
static AnimationTimerPrecision g_animTimerPrecision;

// Forward declarations for functions used across this file.
static void ComputeLayout(Layout& L);
static void UpdateScrollView(Layout& L);
static void RefreshPalette();
static void ApplyCurrent(bool force, bool explicitRequest = false);
static void StartAnim();
static void StartZoom(bool toMax);
static void RunZoomAnim();
static void StartWindowAnim(bool show);
static void TrackNormRect();
static void ShowMainWindow();
static bool EnsureUiInitialized();
static void HideMainWindow();
static void SelectTheme(int idx);
static void OpenThemesFolder();
static void OpenAddTheme();
static void CloseAddTheme();
static void OpenRenameTheme(int idx);
static void CloseRenameTheme();
static void OpenDelTheme(int idx);
static void CloseDelTheme();
static void EnsureGlassBitmap(bool force);
static void ClearThumbs();
static void SyncThumbs();
static void EnsureCardVisible(int idx, bool instant = false);
static void MarkScrolled();
static double qpcMs();

// ---------------------------------------------------------------------------
// --simulate-hour overrides only the hour; minutes still use the real clock.
// ---------------------------------------------------------------------------
static int CurrentMinutes() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return g_app.simulateHour >= 0 ? g_app.simulateHour * 60 + st.wMinute
                                   : st.wHour * 60 + st.wMinute;
}

// ---------------------------------------------------------------------------
// Layout computation.
// ---------------------------------------------------------------------------
static void ComputeLayout(Layout& L) {
    const float W = g_app.winWDip, H = g_app.winHDip;   // Use current window size for maximize/snap layouts.

    // Caption buttons follow Windows 11: minimize, maximize, close; each is 46 DIPs wide.
    L.captionClose = D2D1::RectF(W - 46, 0, W, Ui::TitleBarH);
    L.captionMax   = D2D1::RectF(W - 92, 0, W - 46, Ui::TitleBarH);
    L.captionMin   = D2D1::RectF(W - 138, 0, W - 92, Ui::TitleBarH);

    // Limit and center content width to avoid stretching controls in maximized windows.
    const float kMaxContent = 1060.0f;
    float contentW = W - 2 * kPadX;
    if (contentW > kMaxContent) contentW = kMaxContent;
    if (contentW < 300) contentW = 300;
    const float lx = (W - contentW) / 2, rx = lx + contentW;
    L.contentL = lx;
    L.contentR = rx;

    // Theme grid wraps to additional rows; overflow is accessible by scrolling.
    float y = 56;
    L.themeSectionTitle = D2D1::RectF(lx, y, rx, y + 27);
    L.addThemeBtn = D2D1::RectF(rx - 116, y - 1, rx, y + 25);
    y += 36;                                             // Section heading and divider.
    const float gap = 12;
    int total = (int)g_app.themes.size();
    if (total > kThemeSlots) total = kThemeSlots;
    int perRow = (int)((contentW + gap) / (kCardW + gap));
    if (perRow < 1) perRow = 1;
    if (perRow > kMaxCards) perRow = kMaxCards;
    L.cardCount = total;
    L.perRow = perRow;
    L.themeCards.assign(total, D2D1::RectF(0, 0, 0, 0));
    L.themeDel.assign(total, D2D1::RectF(0, 0, 0, 0));
    L.themeRen.assign(total, D2D1::RectF(0, 0, 0, 0));
    // Compute column positions once for a full row; reuse them for every row so a partial
    // last row stays aligned with the columns above.
    float gy = y;
    if (total == 0) {
        gy += kCardH + kCardGapY;                        // Reserve room for an empty-state message even with no themes.
    } else {
        float used = perRow * kCardW;
        float sp = perRow > 1 ? (contentW - used) / (perRow - 1) : 0;
        if (sp > 30) sp = 30;
        float x0 = lx;
        if (perRow > 1) x0 = lx + (contentW - (used + sp * (perRow - 1))) / 2;
        for (int r = 0; r * perRow < total; ++r) {
            int cnt = total - r * perRow;
            if (cnt > perRow) cnt = perRow;
            for (int i = 0; i < cnt; ++i) {
                int ci = r * perRow + i;
                float cx0 = x0 + i * (kCardW + sp);
                L.themeCards[ci] = D2D1::RectF(cx0, gy, cx0 + kCardW, gy + kCardH);
                L.themeDel[ci] = UiThemeCardDeleteRect(L.themeCards[ci]);
                L.themeRen[ci] = UiThemeCardRenameRect(L.themeCards[ci]);
            }
            gy += kCardH + kCardGapY;
        }
    }
    y = gy - kCardGapY + 18;                             // Replace the last row gap with section spacing.

    // Switch mode: schedule or system app theme.
    L.switchSectionTitle = D2D1::RectF(lx, y, rx, y + 27);
    y += 36;
    L.switchSeg = D2D1::RectF(rx - 196, y + 2, rx, y + 2 + 34);   // 196 DIPs total, 98 per segment.
    y += 34 + 18;

    // Schedule.
    L.timeSectionTitle = D2D1::RectF(lx, y, rx, y + 27);
    y += 36;
    const float stepperW = 132, stepperH = 36;
    L.dayStepper   = D2D1::RectF(rx - stepperW, y, rx, y + stepperH);
    y += stepperH + 10;
    L.nightStepper = D2D1::RectF(rx - stepperW, y, rx, y + stepperH);
    y += stepperH + 18;

    // Appearance.
    L.appearanceSectionTitle = D2D1::RectF(lx, y, rx, y + 27);
    y += 36;
    L.appearanceSeg = D2D1::RectF(rx - 234, y, rx, y + 34);
    y += 34 + 18;

    // Options.
    L.optionSectionTitle = D2D1::RectF(lx, y, rx, y + 27);
    y += 36;
    L.autoToggle  = D2D1::RectF(rx - Ui::ToggleW, y + 8, rx, y + 8 + Ui::ToggleH);
    y += 36;
    L.bootToggle  = D2D1::RectF(rx - Ui::ToggleW, y + 8, rx, y + 8 + Ui::ToggleH);
    y += 36;

    // Keep the status row and bottom action at the bottom when the window is taller than the content.
    const float statusH = 24;
    const float bottomBlock = statusH + 10 + Ui::ButtonH + 24;
    float statusTop = y;
    float stickTop = H - bottomBlock;
    bool stick = stickTop > statusTop;
    if (stick) statusTop = stickTop;
    L.statusRow = D2D1::RectF(lx, statusTop, rx, statusTop + statusH);
    L.openThemesBtn = D2D1::RectF(rx - 180, statusTop + statusH + 10, rx,
                                  statusTop + statusH + 10 + Ui::ButtonH);
    // Add 24 DIPs of bottom padding only for real overflow, avoiding a nearly immovable scrollbar
    // when the content exceeds the viewport by only a few DIPs.
    const float naturalBottom = statusTop + statusH + 10 + Ui::ButtonH;
    float contentBottom = naturalBottom;
    if (naturalBottom - H > 4.0f) contentBottom = naturalBottom + 24.0f;
    if (contentBottom < H) contentBottom = H;
    L.contentH = contentBottom;

    // Centered Add theme dialog.
    const float cw = 560, ch = 424;
    float cx = (W - cw) / 2, cy0 = (H - ch) / 2 - 40;
    if (cy0 < 90) cy0 = 90;
    L.ovCard = D2D1::RectF(cx, cy0, cx + cw, cy0 + ch);
    float px = L.ovCard.left + 24, pr = L.ovCard.right - 24;
    L.ovName      = D2D1::RectF(px, L.ovCard.top + 76, pr, L.ovCard.top + 112);
    L.ovLightSlot = D2D1::RectF(px, L.ovCard.top + 152, px + 240, L.ovCard.top + 292);
    L.ovDarkSlot  = D2D1::RectF(px + 272, L.ovCard.top + 152, px + 512, L.ovCard.top + 292);
    L.ovCancel = D2D1::RectF(L.ovCard.right - 24 - 108 - 116, L.ovCard.bottom - 24 - 34,
                             L.ovCard.right - 24 - 116, L.ovCard.bottom - 24);
    L.ovOk     = D2D1::RectF(L.ovCard.right - 24 - 108, L.ovCard.bottom - 24 - 34,
                             L.ovCard.right - 24, L.ovCard.bottom - 24);

    // Delete confirmation dialog.
    const float dw = 470, dh = 254;
    float dx = (W - dw) / 2, dy0 = (H - dh) / 2 - 24;
    L.dlgCard = D2D1::RectF(dx, dy0, dx + dw, dy0 + dh);
    L.dlgCancel = D2D1::RectF(L.dlgCard.right - 24 - 116 - 108, L.dlgCard.bottom - 24 - 34,
                              L.dlgCard.right - 24 - 116, L.dlgCard.bottom - 24);
    L.dlgOk     = D2D1::RectF(L.dlgCard.right - 24 - 108, L.dlgCard.bottom - 24 - 34,
                              L.dlgCard.right - 24, L.dlgCard.bottom - 24);

    // Rename dialog.
    const float rw = 470, rh = 270;
    float renX = (W - rw) / 2, renY = (H - rh) / 2 - 24;
    L.renCard  = D2D1::RectF(renX, renY, renX + rw, renY + rh);
    L.renField = D2D1::RectF(L.renCard.left + 24, L.renCard.top + 104,
                             L.renCard.right - 24, L.renCard.top + 140);
    L.renCancel = D2D1::RectF(L.renCard.right - 24 - 116 - 108, L.renCard.bottom - 24 - 34,
                              L.renCard.right - 24 - 116, L.renCard.bottom - 24);
    L.renOk     = D2D1::RectF(L.renCard.right - 24 - 108, L.renCard.bottom - 24 - 34,
                              L.renCard.right - 24, L.renCard.bottom - 24);

    // Recompute scroll metrics last.
    UpdateScrollView(L);
}

// ---------------------------------------------------------------------------
// Translate layout rectangles by -scroll for drawing.
// Convert pointer y to y + scroll for hit testing; see HitTest / RenderFrame.
// ---------------------------------------------------------------------------
static void VisibleCardRange(const Layout& L, float scroll, int& first, int& last) {
    first = 0;
    last = L.cardCount - 1;
    const float viewTop = scroll + Ui::TitleBarH;        // Visible content begins below the title bar.
    const float viewBot = scroll + g_app.winHDip;
    while (first < L.cardCount && L.themeCards[first].bottom <= viewTop) ++first;
    while (last >= first && L.themeCards[last].top >= viewBot) --last;
}

// Update maximum scroll, track dimensions, and clamp current/target scroll values.
// Call at the end of ComputeLayout whenever window size or theme count changes.
static void UpdateScrollView(Layout& L) {
    const float H = g_app.winHDip;
    L.visibleH = H;
    float maxS = L.contentH - H;
    if (maxS < 0) maxS = 0;
    L.maxScroll = maxS;
    if (maxS <= 0.0f) {
        g_app.scroll = 0.0f;                 // Return to the top if all content fits, avoiding a blank strip.
        g_app.scrollTarget = 0.0f;
    } else {
        if (g_app.scrollTarget > maxS) g_app.scrollTarget = maxS;
        if (g_app.scrollTarget < 0) g_app.scrollTarget = 0;
        if (g_app.scroll > maxS) g_app.scroll = maxS;    // Clamp immediately after shrinking the window or reducing the theme count.
        if (g_app.scroll < 0) g_app.scroll = 0;
    }
    L.scrollTrackTop = Ui::TitleBarH + 6;
    L.scrollTrackH = H - Ui::TitleBarH - 12;
    if (L.scrollTrackH < 20) L.scrollTrackH = 20;
}

static void ClampScrollTarget() {
    float maxS = g_app.L.maxScroll;
    if (g_app.scrollTarget > maxS) g_app.scrollTarget = maxS;
    if (g_app.scrollTarget < 0) g_app.scrollTarget = 0;
}

// Compute the thumb from current scroll; share this rectangle between rendering and hit testing.
// Thumb length = track length * viewport/content height, with a 36 DIP minimum.
static D2D1_RECT_F ScrollThumbRect() {
    const Layout& L = g_app.L;
    float trackH = L.scrollTrackH;
    float thumbH = (L.contentH > 1.0f) ? trackH * (L.visibleH / L.contentH) : trackH;
    if (thumbH < 36.0f) thumbH = 36.0f;
    if (thumbH > trackH) thumbH = trackH;
    float travel = trackH - thumbH;
    float t = (L.maxScroll > 0.0f) ? (g_app.scroll / L.maxScroll) : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float ty = L.scrollTrackTop + t * travel;
    float right = g_app.winWDip - 5.0f;
    return D2D1::RectF(right - 6.0f, ty, right, ty + thumbH);
}

static void MarkScrolled() {
    g_app.lastScrollMs = qpcMs();
}

// Scroll only when the selected card is clipped at the top or bottom of the viewport.
// Use instant=true at startup to avoid an unnecessary scroll animation.
static void EnsureCardVisible(int idx, bool instant) {
    const Layout& L = g_app.L;
    if (idx < 0 || idx >= (int)L.themeCards.size()) return;
    if (L.maxScroll <= 0.0f) return;
    const float pad = 16.0f;
    const float vt = g_app.scrollTarget + Ui::TitleBarH;
    const float vb = g_app.scrollTarget + g_app.winHDip;
    float t = g_app.scrollTarget;
    if (L.themeCards[idx].top - pad < vt)
        t = L.themeCards[idx].top - pad - Ui::TitleBarH;
    else if (L.themeCards[idx].bottom + pad > vb)
        t = L.themeCards[idx].bottom + pad - g_app.winHDip;
    if (t < 0) t = 0;
    if (t > L.maxScroll) t = L.maxScroll;
    if (t == g_app.scrollTarget) return;                     // Skip animation when nothing changed.
    g_app.scrollTarget = t;
    if (instant) g_app.scroll = t;
    MarkScrolled();
}

// Map pointer coordinates in window DIPs to a control ID.
static int HitTest(float x, float y) {
    const Layout& L = g_app.L;
    auto in = [&](const D2D1_RECT_F& r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };

    // Modal dialogs handle only their own controls and do not scroll with the page.
    if (g_app.addUI.active) {
        if (in(L.ovCancel)) return CID_OV_CANCEL;
        if (in(L.ovOk)) return CID_OV_OK;
        if (in(L.ovName)) return CID_OV_NAME;
        if (in(L.ovLightSlot)) return CID_OV_LIGHT_SLOT;
        if (in(L.ovDarkSlot)) return CID_OV_DARK_SLOT;
        return CID_NONE;
    }
    if (g_app.delUI.active) {
        if (in(L.dlgCancel)) return CID_DLG_CANCEL;
        if (in(L.dlgOk)) return CID_DLG_OK;
        return CID_NONE;
    }
    if (g_app.renUI.active) {
        if (in(L.renCancel)) return CID_REN_CANCEL;
        if (in(L.renOk)) return CID_REN_OK;
        if (in(L.renField)) return CID_REN_NAME;
        return CID_NONE;
    }

    // The fixed title bar uses window coordinates directly.
    // Maximize uses nonclient HTMAXBUTTON hit testing for native snap layouts.
    // Content scrolled beneath the title bar is clipped and must not receive clicks.
    if (y < Ui::TitleBarH) {
        if (in(L.captionClose)) return CID_CLOSE;
        if (in(L.captionMin)) return CID_MIN;
        return CID_NONE;
    }

    // Only a visible scrollbar is interactive; support thumb dragging and track paging.
    if (L.maxScroll > 0.0f && g_app.sbAlpha > 0.15f && x >= g_app.winWDip - 14.0f) {
        if (in(ScrollThumbRect())) return CID_SCROLLBAR;
        D2D1_RECT_F track = D2D1::RectF(g_app.winWDip - 14.0f, L.scrollTrackTop,
                                        g_app.winWDip, L.scrollTrackTop + L.scrollTrackH);
        if (in(track)) return CID_SCROLLTRACK;
    }

    // Map pointer y to content y + scroll before comparing layout rectangles.
    const float cy = y + g_app.scroll;
    auto inC = [&](const D2D1_RECT_F& r) {
        return x >= r.left && x <= r.right && cy >= r.top && cy <= r.bottom;
    };

    if (inC(L.addThemeBtn)) return CID_ADD_THEME;
    // Card actions are clickable only when drawn on a hovered card, avoiding accidental changes.
    for (int i = 0; i < L.cardCount; ++i)
        if (i < (int)g_app.delVisible.size() && g_app.delVisible[i] && inC(L.themeDel[i]))
            return CID_THEME_DEL0 + i;
    for (int i = 0; i < L.cardCount; ++i)
        if (i < (int)g_app.delVisible.size() && g_app.delVisible[i] && inC(L.themeRen[i]))
            return CID_THEME_REN0 + i;
    for (int i = 0; i < L.cardCount; ++i)
        if (inC(L.themeCards[i])) return CID_THEME0 + i;
    // Steppers have 36 DIP end buttons. Disable schedule interaction while following the system theme.
    if (g_app.cfg.switchMode == 0) {
        if (inC(L.dayStepper))
            return x < L.dayStepper.left + 36 ? CID_DAY_MINUS : CID_DAY_PLUS;
        if (inC(L.nightStepper))
            return x < L.nightStepper.left + 36 ? CID_NIGHT_MINUS : CID_NIGHT_PLUS;
    }
    if (inC(L.appearanceSeg)) {
        float segW = (L.appearanceSeg.right - L.appearanceSeg.left) / 3.0f;
        int seg = (int)((x - L.appearanceSeg.left) / segW);
        if (seg < 0) seg = 0;
        if (seg > 2) seg = 2;
        return CID_APPEARANCE0 + seg;
    }
    if (inC(L.switchSeg))
        return x < (L.switchSeg.left + L.switchSeg.right) / 2 ? CID_SWITCH0 : CID_SWITCH1;
    if (inC(L.autoToggle)) return CID_AUTO;
    if (inC(L.bootToggle)) return CID_BOOT;
    if (inC(L.openThemesBtn)) return CID_OPENTHEMES;
    return CID_NONE;
}

// ---------------------------------------------------------------------------
// Wallpaper application.
// ---------------------------------------------------------------------------
static const ThemeInfo* SelectedTheme() {
    if (g_app.cfg.noTheme) return nullptr;
    if (g_app.themes.empty()) return nullptr;
    if (g_app.selIdx < 0 || g_app.selIdx >= (int)g_app.themes.size()) g_app.selIdx = 0;
    return &g_app.themes[g_app.selIdx];
}

// Only poll rapidly while a write or an external-change confirmation is pending.
static void QueueDeferredSelection() {
    if (g_app.selectionDeferred && !g_app.selectionQueued && g_app.hwnd)
        g_app.selectionQueued = PostMessageW(g_app.hwnd, kApplySelectionMessage, 0, 0) != FALSE;
}

static void UpdateWallpaperTimer() {
    if (!g_app.hwnd) return;
    if (g_app.wallpaper.NeedsPoll()) SetTimer(g_app.hwnd, TM_WALLPAPER, 500, nullptr);
    else KillTimer(g_app.hwnd, TM_WALLPAPER);
}

static void WallpaperApplyFailed() {
    g_app.appliedImage.clear();
    g_app.appliedStyle = -1;
    g_app.statusMsg = g_app.wallpaper.attempts >= WallpaperTracker::maxAttempts
        ? AppText(L"Switch failed. Choose Apply now to retry.", L"切换失败。请选择「立即应用」重试。")
        : AppText(L"Wallpaper change unconfirmed. Retrying...", L"尚未确认壁纸切换，正在重试…");
    g_app.statusMsgUntil = GetTickCount64() + 6000;
}

static void CheckExternalWallpaper(ULONGLONG now = GetTickCount64()) {
    if (g_app.applyingWallpaper || g_app.readingWallpaper || g_app.cfg.noTheme) return;
    const ThemeInfo* theme = SelectedTheme();
    if (!theme) return;
    const std::wstring day = theme->dayImage, night = theme->nightImage;
    const ULONGLONG generation = g_app.wallpaper.generation;
    WallpaperSnapshot current;
    g_app.readingWallpaper = true;
    bool read = ReadWallpaperSnapshot(current);
    g_app.readingWallpaper = false;
    QueueDeferredSelection();
    if (generation != g_app.wallpaper.generation || g_app.cfg.noTheme) return;
    if (!read) {
        g_app.wallpaper.ReadFailed();
        if (g_app.wallpaper.pending && now - g_app.wallpaper.started >= WallpaperTracker::settleMs) {
            g_app.wallpaper.pending = false;
            WallpaperApplyFailed();
        }
        UpdateWallpaperTimer();
        return;
    }
    auto result = g_app.wallpaper.Observe(current, day, night, now);
    if (result == WallpaperTracker::Result::Confirmed) {
        g_app.appliedImage = g_app.wallpaper.target;
        g_app.appliedStyle = g_app.wallpaper.style;
        g_app.statusMsg = SameWallpaperPath(g_app.appliedImage, day)
            ? AppText(L"Light wallpaper applied", L"已应用浅色壁纸") : AppText(L"Dark wallpaper applied", L"已应用深色壁纸");
        g_app.statusMsgUntil = GetTickCount64() + 4000;
        if (g_app.glass) EnsureGlassBitmap(true);
    } else if (result == WallpaperTracker::Result::TimedOut) {
        WallpaperApplyFailed();
    } else if (result == WallpaperTracker::Result::External) {
        g_app.cfg.noTheme = true;
        g_app.cfg.theme.clear();
        g_app.selIdx = -1;
        g_app.appliedImage.clear();
        g_app.appliedStyle = -1;
        g_app.wallpaper.ResetSelection();
        g_app.statusMsg = AppText(L"External wallpaper detected. Theme deselected.", L"检测到外部修改，已取消选中的主题。");
        g_app.statusMsgUntil = GetTickCount64() + 6000;
        SaveConfig(g_app.cfg);
        if (g_app.glass) EnsureGlassBitmap(true);
    }
    UpdateWallpaperTimer();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// Desired wallpaper stage: 0=light/day, 1=dark/night.
// Schedule and system modes share the same pair of images but choose them differently.
static int TargetStage() {
    if (g_app.cfg.switchMode == 1) return SystemUsesDarkMode() ? 1 : 0;
    return StageAt(g_app.cfg, CurrentMinutes());
}

static void ApplyCurrent(bool force, bool explicitRequest) {
    if (explicitRequest) {
        g_app.wallpaper.ResetSelection();
        g_app.appliedImage.clear();
        g_app.appliedStyle = -1;
    }
    if (g_app.applyingWallpaper || g_app.readingWallpaper) {
        g_app.selectionDeferred = g_app.selectionDeferred || explicitRequest;
        return;
    }
    if (!SelectedTheme()) return;
    // Every write passes through the same external-change check, including
    // resume, settings changes and retries. Explicit user selection starts anew.
    const ULONGLONG checkedGeneration = g_app.wallpaper.generation;
    CheckExternalWallpaper();
    if (checkedGeneration != g_app.wallpaper.generation) return;
    const ThemeInfo* t = SelectedTheme();
    if (!t || (g_app.wallpaper.Blocked() && !explicitRequest)) return;
    int stage = TargetStage();
    const std::wstring img = stage == 0 ? t->dayImage : t->nightImage;
    const int style = g_app.cfg.wallpaperStyle;
    if (!force && !g_app.cfg.autoSwitch) return;
    // A confirmed cache is valid only while the existing monitors still show
    // it. Known day/night changes must continue to follow the selected mode.
    bool matches = true;
    for (const auto& item : g_app.wallpaper.observed) {
        if ((SameWallpaperPath(item.second, t->dayImage) || SameWallpaperPath(item.second, t->nightImage)) &&
            !SameWallpaperPath(item.second, img)) matches = false;
    }
    if (!force && matches && SameWallpaperPath(img, g_app.appliedImage) &&
        style == g_app.appliedStyle) return;
    if (!explicitRequest && !g_app.wallpaper.CanRequest(img, style)) return;
    const ULONGLONG generation = g_app.wallpaper.Begin(img, style, GetTickCount64());
    g_app.appliedImage.clear();
    g_app.appliedStyle = -1;
    g_app.applyingWallpaper = true;
    bool ok = ApplyWallpaper(img, style);
    g_app.applyingWallpaper = false;
    QueueDeferredSelection();
    // Synchronous Windows broadcasts may run nested UI commands.
    if (generation != g_app.wallpaper.generation) return;
    g_app.wallpaper.Applied(ok);
    if (ok) {
        g_app.statusMsg = AppText(L"Confirming wallpaper change...", L"正在确认壁纸切换…");
        g_app.statusMsgUntil = GetTickCount64() + 4000;
        CheckExternalWallpaper();
    } else {
        WallpaperApplyFailed();
    }
    UpdateWallpaperTimer();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Drawing.
// ---------------------------------------------------------------------------
static void DrawTitleBar(Gfx& g, const Palette& p) {
    // Draw the embedded IDR_LOGO_PNG logo at 20 DIPs; its shape is baked into the image.
    float ix = 24, iy = Ui::TitleBarH / 2;
    if (!g_app.logoBmp && !g_app.logoTried) {
        g_app.logoTried = true;
        g_app.logoBmp = g.loadImageFromResource(IDR_LOGO_PNG, 64);
    }
    if (g_app.logoBmp) {
        float s = 20.0f;
        D2D1_RECT_F dst = D2D1::RectF(ix - s / 2, iy - s / 2, ix + s / 2, iy + s / 2);
        g.dc()->DrawBitmap(g_app.logoBmp, dst, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    }
    IDWriteTextFormat* f = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g.textOptical(kAppDisplay, f, D2D1::RectF(46, 0, 400, Ui::TitleBarH), p.textPrimary);

    // Caption buttons; HTMAXBUTTON lets Windows show its snap layout flyout.
    int maxMode = g_app.maximized ? Ui::CAP_RESTORE : Ui::CAP_MAX;
    UiCaptionButton(g, g_app.L.captionMax, maxMode, g_app.hot == CID_MAX,
                    g_app.pressed == CID_MAX, p);
    UiCaptionButton(g, g_app.L.captionMin, Ui::CAP_MIN, g_app.hot == CID_MIN,
                    g_app.pressed == CID_MIN, p);
    UiCaptionButton(g, g_app.L.captionClose, Ui::CAP_CLOSE, g_app.hot == CID_CLOSE,
                    g_app.pressed == CID_CLOSE, p);
}

static void DrawThemes(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    if ((int)g_app.delVisible.size() != kThemeSlots) g_app.delVisible.assign(kThemeSlots, 0);
    for (int i = 0; i < kThemeSlots; ++i) g_app.delVisible[i] = 0;   // Clear the previous frame's action visibility flags.
    UiSectionTitle(g, L.themeSectionTitle.left, L.themeSectionTitle.top,
                   L.themeSectionTitle.right - L.themeSectionTitle.left, AppText(L"Themes", L"主题"), p);
    UiTextButton(g, L.addThemeBtn, AppText(L"Add theme", L"添加主题"), p, g_app.hot == CID_ADD_THEME,
                 g_app.pressed == CID_ADD_THEME, L"\uE710");
    if (L.cardCount == 0) {
        IDWriteTextFormat* f = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_TEXT_ALIGNMENT_LEADING, true);
        g.text(AppText(L"No themes yet. Click Add theme and choose a light and a dark wallpaper.", L"暂无主题。选择「添加主题」，然后选取一张浅色壁纸和一张深色壁纸。"),
               f, D2D1::RectF(L.contentL, L.themeSectionTitle.top + 40, L.contentR,
                              L.themeSectionTitle.top + 88), p.textSecondary);
        return;
    }
    // Draw only visible cards; SyncThumbs computes firstVis/lastVis from the viewport.
    for (int ti = L.firstVis; ti <= L.lastVis; ++ti) {
        if (ti < 0 || ti >= L.cardCount || ti >= (int)g_app.themes.size()) continue;
        // New themes use light/dark pairs; show image counts for legacy multi-image themes.
        std::wstring sub = (g_app.themes[ti].imageCount == 2)
                               ? AppText(L"Light + dark", L"浅色与深色")
                               : (std::to_wstring(g_app.themes[ti].imageCount) + AppText(L" images", L" 张图片"));
        bool showDel = (g_app.hot == CID_THEME0 + ti || g_app.hot == CID_THEME_DEL0 + ti ||
                        g_app.hot == CID_THEME_REN0 + ti);
        bool delHot  = (g_app.hot == CID_THEME_DEL0 + ti);
        bool renHot  = (g_app.hot == CID_THEME_REN0 + ti);
        g_app.delVisible[ti] = showDel ? 1 : 0;   // Hit testing recognizes only actions that were drawn.
        ID2D1Bitmap* thumb = (ti < (int)g_app.thumbs.size()) ? g_app.thumbs[ti] : nullptr;
        UiThemeCard(g, L.themeCards[ti], thumb, g_app.themes[ti].name, sub,
                    !g_app.cfg.noTheme && ti == g_app.selIdx, g_app.hot == CID_THEME0 + ti, p,
                    showDel, delHot, renHot);
    }
}

static void DrawAppearance(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    UiSectionTitle(g, L.appearanceSectionTitle.left, L.appearanceSectionTitle.top,
                   L.appearanceSectionTitle.right - L.appearanceSectionTitle.left, AppText(L"Appearance", L"外观"), p);
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    g.textOptical(AppText(L"App theme", L"应用外观"), f, D2D1::RectF(L.contentL, L.appearanceSeg.top, L.appearanceSeg.left - 12, L.appearanceSeg.bottom),
                  p.textPrimary);
    std::vector<std::wstring> items = {AppText(L"System", L"跟随系统"), AppText(L"Light", L"浅色"), AppText(L"Dark", L"深色")};
    int hotSeg = -1;
    if (g_app.hot >= CID_APPEARANCE0 && g_app.hot <= CID_APPEARANCE2)
        hotSeg = g_app.hot - CID_APPEARANCE0;
    // Changes from the tray menu also trigger the appearance indicator animation here.
    if (g_app.segTarget != g_app.cfg.appearance) {
        g_app.segTarget = g_app.cfg.appearance;
        StartAnim();
    }
    UiSegmented(g, L.appearanceSeg, items, g_app.cfg.appearance, hotSeg, p,
                g_app.segAnim, g_app.segScale);
}

static void DrawSwitchSection(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    UiSectionTitle(g, L.switchSectionTitle.left, L.switchSectionTitle.top,
                   L.switchSectionTitle.right - L.switchSectionTitle.left, AppText(L"Switching", L"壁纸切换"), p);
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    g.textOptical(AppText(L"Switch mode", L"切换模式"), f, D2D1::RectF(L.contentL, L.switchSeg.top, L.switchSeg.left - 12, L.switchSeg.bottom),
                  p.textPrimary);
    // Smooth indicator motion and press scaling also reflect mode changes from the tray menu.
    if (g_app.swTarget != g_app.cfg.switchMode) {
        g_app.swTarget = g_app.cfg.switchMode;
        StartAnim();
    }
    std::vector<std::wstring> modes = {AppText(L"Schedule", L"定时切换"), AppText(L"System", L"跟随系统")};
    int hotSw = (g_app.hot == CID_SWITCH0) ? 0 : (g_app.hot == CID_SWITCH1 ? 1 : -1);
    UiSegmented(g, L.switchSeg, modes, g_app.cfg.switchMode, hotSw, p,
                g_app.swAnim, g_app.swScale);
}

static void DrawTimeSection(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    // Dim the schedule section in system mode and show an explanatory note.
    // Do not nest this layer inside a whole-frame opacity layer; D2D can truncate the frame.
    // See App::frameLayerPushed.
    bool dim = (g_app.cfg.switchMode == 1) && !g_app.frameLayerPushed;
    if (dim) {
        if (!g_app.uiLayer) g.dc()->CreateLayer(nullptr, &g_app.uiLayer);
        if (g_app.uiLayer) {
            D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
                D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(), 0.4f, nullptr, D2D1_LAYER_OPTIONS_NONE);
            g.dc()->PushLayer(&lp, g_app.uiLayer);
        } else {
            dim = false;
        }
    }
    UiSectionTitle(g, L.timeSectionTitle.left, L.timeSectionTitle.top,
                   L.timeSectionTitle.right - L.timeSectionTitle.left, AppText(L"Schedule", L"定时切换"), p);
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    // Align row labels with adjacent controls using optical vertical centering.
    g.textOptical(AppText(L"Day starts", L"日间开始"), f, D2D1::RectF(L.contentL, L.dayStepper.top, L.dayStepper.left - 12, L.dayStepper.bottom),
                  p.textPrimary);
    g.textOptical(AppText(L"Night starts", L"夜间开始"), f, D2D1::RectF(L.contentL, L.nightStepper.top, L.nightStepper.left - 12, L.nightStepper.bottom),
                  p.textPrimary);
    UiStepper(g, L.dayStepper, FormatHM(g_app.cfg.dayStart), p,
              g_app.hot == CID_DAY_MINUS ? 0 : (g_app.hot == CID_DAY_PLUS ? 1 : -1),
              g_app.pressed == CID_DAY_MINUS ? 0 : (g_app.pressed == CID_DAY_PLUS ? 1 : -1));
    UiStepper(g, L.nightStepper, FormatHM(g_app.cfg.nightStart), p,
              g_app.hot == CID_NIGHT_MINUS ? 0 : (g_app.hot == CID_NIGHT_PLUS ? 1 : -1),
              g_app.pressed == CID_NIGHT_MINUS ? 0 : (g_app.pressed == CID_NIGHT_PLUS ? 1 : -1));
    if (dim) {
        g.dc()->PopLayer();
        IDWriteTextFormat* fHint = g.format(FONT_UI, 12.0f);
        g.textOptical(AppText(L"Following the system theme. Schedule is inactive.", L"当前跟随系统外观，定时切换未启用。"), fHint,
                      D2D1::RectF(L.contentL, L.timeSectionTitle.top, L.contentR, L.timeSectionTitle.top + 20),
                      p.textSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

static void DrawOptions(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    UiSectionTitle(g, L.optionSectionTitle.left, L.optionSectionTitle.top,
                   L.optionSectionTitle.right - L.optionSectionTitle.left, AppText(L"Options", L"选项"), p);
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    g.textOptical(AppText(L"Switch wallpapers automatically", L"自动切换壁纸"), f, D2D1::RectF(L.contentL, L.autoToggle.top, L.autoToggle.left - 12, L.autoToggle.bottom),
                  p.textPrimary);
    g.textOptical(AppText(L"Start with Windows", L"随 Windows 启动"), f, D2D1::RectF(L.contentL, L.bootToggle.top, L.bootToggle.left - 12, L.bootToggle.bottom),
                  p.textPrimary);
    UiToggle(g, L.autoToggle.left, L.autoToggle.top, g_app.cfg.autoSwitch, g_app.toggleAnim[0],
             g_app.hot == CID_AUTO, p);
    UiToggle(g, L.bootToggle.left, L.bootToggle.top, g_app.cfg.startOnBoot, g_app.toggleAnim[1],
             g_app.hot == CID_BOOT, p);
}

static void DrawStatus(Gfx& g, const Palette& p) {
    std::wstring feedback;
    if (g_app.simulateHour >= 0) feedback = AppText(L"Simulated", L"模拟时间");
    if (!g_app.statusMsg.empty() && GetTickCount64() < g_app.statusMsgUntil) {
        if (!feedback.empty()) feedback += L"  ";
        feedback += g_app.statusMsg;
    }

    std::wstring status;
    if (!SelectedTheme()) {
        status = AppText(L"Select a theme to enable wallpaper switching.", L"请先选择一个壁纸主题。");
    } else if (!g_app.cfg.autoSwitch) {
        status = AppText(L"Switching paused. Enable it from the tray menu.", L"壁纸切换已暂停，可从托盘菜单恢复。");
    } else if (g_app.cfg.switchMode != 1) {
        const int now = CurrentMinutes();
        status = AppText(L"Next switch: ", L"下次切换：") + FormatHM(NextSwitchAt(g_app.cfg, now)) +
                 AppText(L" (in ", L"（剩余 ") + FormatDuration(MinutesUntilSwitch(g_app.cfg, now)) +
                 AppText(L")", L"）");
    }

    if (feedback.empty() && status.empty()) return;
    const auto& row = g_app.L.statusRow;
    auto* f = g.format(FONT_UI, 12.0f);
    constexpr float gap = 12.0f;
    // Keep feedback readable when the two messages do not fit on the same line.
    g.dc()->PushAxisAlignedClip(row, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (!status.empty() && (feedback.empty() ||
        g.textWidth(feedback, f) + gap + g.textWidth(status, f) <= row.right - row.left)) {
        g.textOptical(status, f, row, p.textSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (!feedback.empty()) g.textOptical(feedback, f, row, p.accentText);
    g.dc()->PopAxisAlignedClip();
}

// ---------------------------------------------------------------------------
// Text editor shared by theme creation and renaming.
// ---------------------------------------------------------------------------
static std::wstring TrimStr(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Remove any selection before inserting, pasting, or deleting text.
static void EditDeleteSelection(TextEdit& e) {
    if (!e.hasSel()) return;
    int a, b;
    e.selRange(a, b);
    e.text.erase(a, b - a);
    e.caret = a;
    e.clearSel();
}

static void EditInsert(TextEdit& e, const std::wstring& s) {
    std::wstring add;
    for (wchar_t c : s) if (c >= 32 && c != 127) add += c;      // Filter control characters.
    if (add.empty()) return;
    EditDeleteSelection(e);
    if (e.caret < 0) e.caret = 0;
    if (e.caret > (int)e.text.size()) e.caret = (int)e.text.size();
    e.text.insert(e.caret, add);
    e.caret += (int)add.size();
    e.clearSel();
}

// Treat surrogate pairs as one character so Backspace cannot split an emoji.
static int PrevPos(const std::wstring& s, int pos) {
    if (pos <= 0) return 0;
    int p = pos - 1;
    if (p > 0 && s[p] >= 0xDC00 && s[p] <= 0xDFFF && s[p - 1] >= 0xD800 && s[p - 1] <= 0xDBFF) p--;
    return p;
}
static int NextPos(const std::wstring& s, int pos) {
    int n = (int)s.size();
    if (pos >= n) return n;
    int p = pos + 1;
    if (p < n && s[pos] >= 0xD800 && s[pos] <= 0xDBFF && s[p] >= 0xDC00 && s[p] <= 0xDFFF) p++;
    return p;
}

static void EditPaste(TextEdit& e) {
    if (!OpenClipboard(g_app.hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            EditInsert(e, p);        // EditInsert filters newlines and other control characters.
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

// vk is the key; Shift extends the selection.
static void EditKeyDown(TextEdit& e, WPARAM vk, bool shift) {
    int n = (int)e.text.size();
    auto move = [&](int to) {
        if (shift) {
            if (!e.hasSel()) e.sel = e.caret;     // Start extending the selection.
            e.caret = to;
        } else {
            e.caret = to;
            e.clearSel();
        }
    };
    switch (vk) {
    case VK_LEFT:  move(PrevPos(e.text, e.caret)); break;
    case VK_RIGHT: move(NextPos(e.text, e.caret)); break;
    case VK_HOME:  move(0); break;
    case VK_END:   move(n); break;
    case VK_BACK:
        if (e.hasSel()) EditDeleteSelection(e);
        else if (e.caret > 0) {
            int p = PrevPos(e.text, e.caret);
            e.text.erase(p, e.caret - p);
            e.caret = p;
            e.clearSel();
        }
        break;
    case VK_DELETE:
        if (e.hasSel()) EditDeleteSelection(e);
        else if (e.caret < n) {
            e.text.erase(e.caret, NextPos(e.text, e.caret) - e.caret);
            e.clearSel();
        }
        break;
    default: break;
    }
}

// Build display text with IME preedit inserted at the caret, and return its displayed caret position.
static std::wstring EditDisplay(const TextEdit& e, int* caretOut) {
    std::wstring s = e.text;
    int c = e.caret < 0 ? 0 : (e.caret > (int)e.text.size() ? (int)e.text.size() : e.caret);
    if (!e.preedit.empty()) {
        s.insert(c, e.preedit);
        c += (int)e.preedit.size();
    }
    if (caretOut) *caretOut = c;
    return s;
}

static void EditSelectAll(TextEdit& e) {
    e.sel = 0;
    e.caret = (int)e.text.size();
}

// Place the caret from a click and clear selection; x excludes the field's left padding.
static void EditSetCaretFromX(Gfx& g, TextEdit& e, float x) {
    if (e.text.empty()) { e.caret = 0; e.clearSel(); return; }
    int caretPos = 0;
    std::wstring disp = EditDisplay(e, &caretPos);
    int pre = (int)e.preedit.size();
    int start = e.caret;
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    int bestI = caretPos;
    float bestD = 1e9f;
    for (int i = 0; i <= (int)disp.size(); ++i) {
        float d = fabsf(g.textWidth(disp.substr(0, i), f) - x);
        if (d < bestD) { bestD = d; bestI = i; }
    }
    int pos;
    if (bestI <= start) pos = bestI;
    else if (bestI >= start + pre) pos = bestI - pre;
    else pos = start;                        // A click inside preedit text places the caret before it.
    e.caret = pos;
    e.clearSel();
}

// Attach IME and start caret blinking when opening a text dialog; reverse on close.
static void PrepareTextInput() {
    if (g_app.himc) ImmAssociateContext(g_app.hwnd, g_app.himc);
    g_app.caretOn = true;
    SetTimer(g_app.hwnd, TM_CARET, 500, nullptr);
}
static void EndTextInput() {
    KillTimer(g_app.hwnd, TM_CARET);
    ImmAssociateContext(g_app.hwnd, nullptr);      // Detach IME outside editors so unrelated keystrokes do not open candidate windows.
}

static TextEdit* ActiveEdit() {
    if (g_app.addUI.active) return &g_app.addUI.name;
    if (g_app.renUI.active) return &g_app.renUI.name;
    return nullptr;
}
static bool AnyDialogOpen() {
    return g_app.addUI.active || g_app.delUI.active || g_app.renUI.active;
}

// Position the IME candidate window below the active editor.
static void ImeFollowEdit() {
    TextEdit* e = ActiveEdit();
    if (!e) return;
    HIMC hImc = ImmGetContext(g_app.hwnd);
    if (!hImc) return;
    const D2D1_RECT_F& r = g_app.addUI.active ? g_app.L.ovName : g_app.L.renField;
    POINT pt = {(int)(r.left * g_app.gfx.scale()), (int)((r.bottom + 4) * g_app.gfx.scale())};
    ClientToScreen(g_app.hwnd, &pt);
    COMPOSITIONFORM cf = {};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = pt;
    ImmSetCompositionWindow(hImc, &cf);
    CANDIDATEFORM cdf = {};
    cdf.dwIndex = 0;
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos = pt;
    ImmSetCandidateWindow(hImc, &cdf);
    ImmReleaseContext(g_app.hwnd, hImc);
}

// Read committed and preedit strings from WM_IME_COMPOSITION.
static void ImeHandleComposition(LPARAM l) {
    TextEdit* e = ActiveEdit();
    if (!e) return;
    HIMC hImc = ImmGetContext(g_app.hwnd);
    if (!hImc) return;
    if (l & GCS_RESULTSTR) {
        LONG n = ImmGetCompositionStringW(hImc, GCS_RESULTSTR, nullptr, 0);
        if (n > 0) {
            std::wstring s((size_t)n / sizeof(wchar_t), 0);
            ImmGetCompositionStringW(hImc, GCS_RESULTSTR, &s[0], n);
            EditInsert(*e, s);
        }
        e->preedit.clear();
    }
    if (l & GCS_COMPSTR) {
        LONG n = ImmGetCompositionStringW(hImc, GCS_COMPSTR, nullptr, 0);
        e->preedit.assign(n > 0 ? (size_t)n / sizeof(wchar_t) : 0, 0);
        if (n > 0) ImmGetCompositionStringW(hImc, GCS_COMPSTR, &e->preedit[0], n);
    }
    ImmReleaseContext(g_app.hwnd, hImc);
    ImeFollowEdit();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Open a system file picker only when an image slot is clicked.
// ---------------------------------------------------------------------------
static bool PickImageFile(const std::wstring& title, std::wstring& outPath) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog, (void**)&dlg)))
        return false;
    COMDLG_FILTERSPEC filters[] = {{AppText(L"Images", L"图片"), L"*.jpg;*.jpeg;*.png;*.bmp"},
                                   {AppText(L"All files", L"所有文件"), L"*.*"}};
    dlg->SetFileTypes(2, filters);
    dlg->SetFileTypeIndex(1);
    dlg->SetTitle(title.c_str());
    dlg->SetOkButtonLabel(AppText(L"Open", L"打开"));
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(g_app.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                outPath = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// Use the image filename without its extension as the initial name if the field is empty.
static std::wstring FileBaseName(const std::wstring& path) {
    std::wstring s = path;
    size_t sl = s.find_last_of(L"\\/");
    if (sl != std::wstring::npos) s = s.substr(sl + 1);
    size_t dot = s.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) s = s.substr(0, dot);
    return s;
}

// ---------------------------------------------------------------------------
// Add theme dialog.
// ---------------------------------------------------------------------------
static void CloseAddTheme() {
    AddThemeUI& a = g_app.addUI;
    if (a.lightBmp) { a.lightBmp->Release(); a.lightBmp = nullptr; }
    if (a.darkBmp) { a.darkBmp->Release(); a.darkBmp = nullptr; }
    a.active = false;
    a.lightPath.clear();
    a.darkPath.clear();
    a.name.text.clear();
    a.name.caret = 0;
    a.name.preedit.clear();
    a.hint.clear();
    EndTextInput();
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void OpenAddTheme() {
    AddThemeUI& a = g_app.addUI;
    if (a.lightBmp) { a.lightBmp->Release(); a.lightBmp = nullptr; }
    if (a.darkBmp) { a.darkBmp->Release(); a.darkBmp = nullptr; }
    a.lightPath.clear();
    a.darkPath.clear();
    a.name.text.clear();
    a.name.caret = 0;
    a.name.preedit.clear();
    a.hint.clear();
    a.active = true;
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    PrepareTextInput();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// Open an image picker and immediately display the selected thumbnail.
static void PickAddThemeImage(bool light) {
    AddThemeUI& a = g_app.addUI;
    std::wstring path;
    if (!PickImageFile(light ? AppText(L"Choose a light wallpaper", L"选择浅色壁纸") : AppText(L"Choose a dark wallpaper", L"选择深色壁纸"), path))
        return;
    ID2D1Bitmap* bmp = g_app.gfx.loadImage(path, 640);
    if (!bmp) {
        a.hint = AppText(L"Cannot open this image. Use JPG, PNG, or BMP.", L"无法打开此图片。请选择 JPG、PNG 或 BMP 文件。");
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    if (light) {
        if (a.lightBmp) a.lightBmp->Release();
        a.lightBmp = bmp;
        a.lightPath = path;
    } else {
        if (a.darkBmp) a.darkBmp->Release();
        a.darkBmp = bmp;
        a.darkPath = path;
    }
    if (TrimStr(a.name.text).empty()) {                 // Prefill an empty name with the filename.
        a.name.text = FileBaseName(path);
        a.name.caret = (int)a.name.text.size();
        a.name.clearSel();
    }
    // Validate the image pair and clear stale errors.
    if (!a.lightPath.empty() && !a.darkPath.empty() &&
        _wcsicmp(a.lightPath.c_str(), a.darkPath.c_str()) == 0)
        a.hint = AppText(L"Choose different images for light and dark modes.", L"请为浅色和深色模式选择不同的图片。");
    else
        a.hint.clear();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void ConfirmAddTheme() {
    AddThemeUI& a = g_app.addUI;
    std::wstring nm = TrimStr(a.name.text);
    if (a.lightPath.empty() || a.darkPath.empty()) {
        a.hint = AppText(L"Choose a light image and a dark image.", L"请选择一张浅色图片和一张深色图片。");
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    if (_wcsicmp(a.lightPath.c_str(), a.darkPath.c_str()) == 0) {
        a.hint = AppText(L"Choose different images for light and dark modes.", L"请为浅色和深色模式选择不同的图片。");
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    if (nm.empty()) {
        a.hint = AppText(L"Enter a theme name.", L"请输入主题名称。");
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    // Repeat live name validation for Enter-key confirmation, so invalid names receive a specific
    // error instead of a misleading directory-write failure from CreateDirectoryW.
    std::wstring nameErr;
    if (!ThemeNameValid(nm, nameErr)) {
        a.hint = nameErr;
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    std::wstring outName, err;
    if (!ImportTheme(nm, a.lightPath, a.darkPath, outName, &err)) {
        a.hint = AppText(L"Could not add theme", L"无法添加主题") + (err.empty() ? std::wstring(L"") : (L": " + err));
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    g_app.themes = ScanThemes();
    g_app.selIdx = 0;
    for (size_t i = 0; i < g_app.themes.size(); ++i)
        if (g_app.themes[i].name == outName) g_app.selIdx = (int)i;
    g_app.cfg.theme = outName;
    g_app.cfg.noTheme = false;
    SaveConfig(g_app.cfg);
    ClearThumbs();
    ComputeLayout(g_app.L);
    EnsureCardVisible(g_app.selIdx);     // Scroll the newly added theme into view if it is below the viewport.
    ApplyCurrent(true, true);
    g_app.statusMsg = AppText(L"Theme added: ", L"已添加主题：") + outName;
    g_app.statusMsgUntil = GetTickCount64() + 6000;
    CloseAddTheme();
}

// ---------------------------------------------------------------------------
// Theme renaming via the card's rename action.
// ---------------------------------------------------------------------------
static void CloseRenameTheme() {
    RenameUI& r = g_app.renUI;
    r.active = false;
    r.idx = -1;
    r.name.text.clear();
    r.name.caret = 0;
    r.name.preedit.clear();
    r.hint.clear();
    EndTextInput();
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void OpenRenameTheme(int idx) {
    if (idx < 0 || idx >= (int)g_app.themes.size()) return;
    RenameUI& r = g_app.renUI;
    r.idx = idx;
    r.name.text = g_app.themes[idx].name;
    r.name.caret = (int)r.name.text.size();
    r.name.clearSel();
    r.name.preedit.clear();
    r.hint.clear();
    r.active = true;
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    PrepareTextInput();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void ConfirmRenameTheme() {
    RenameUI& r = g_app.renUI;
    if (r.idx < 0 || r.idx >= (int)g_app.themes.size()) { CloseRenameTheme(); return; }
    std::wstring oldName = g_app.themes[r.idx].name;
    std::wstring folder = g_app.themes[r.idx].folder;
    std::wstring nm = TrimStr(r.name.text);
    if (nm == oldName) { CloseRenameTheme(); return; }        // No action needed if the name is unchanged.
    std::wstring err;
    if (!RenameTheme(folder, nm, err)) {
        r.hint = err;
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    bool wasSelected = (g_app.cfg.theme == oldName);
    g_app.themes = ScanThemes();
    if (wasSelected) g_app.cfg.theme = nm;
    g_app.selIdx = g_app.cfg.noTheme ? -1 : 0;
    for (size_t i = 0; i < g_app.themes.size(); ++i)
        if (g_app.themes[i].name == g_app.cfg.theme) g_app.selIdx = (int)i;
    SaveConfig(g_app.cfg);
    ClearThumbs();
    ComputeLayout(g_app.L);
    EnsureCardVisible(g_app.selIdx);
    ApplyCurrent(true, true);
    g_app.statusMsg = AppText(L"Theme renamed: ", L"已重命名主题：") + oldName + L" → " + nm;
    g_app.statusMsgUntil = GetTickCount64() + 6000;
    CloseRenameTheme();
}

// ---------------------------------------------------------------------------
// Delete confirmation dialog.
// ---------------------------------------------------------------------------
static void OpenDelTheme(int idx) {
    if (idx < 0 || idx >= (int)g_app.themes.size()) return;
    g_app.delUI.active = true;
    g_app.delUI.idx = idx;
    g_app.delUI.err.clear();
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void CloseDelTheme() {
    g_app.delUI.active = false;
    g_app.delUI.idx = -1;
    g_app.delUI.err.clear();
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void ConfirmDeleteTheme() {
    DelUI& d = g_app.delUI;
    if (d.idx < 0 || d.idx >= (int)g_app.themes.size()) { CloseDelTheme(); return; }
    ThemeInfo t = g_app.themes[d.idx];       // Copy first because the theme list will be rebuilt.
    bool wasSelected = (d.idx == g_app.selIdx);

    std::wstring err;
    if (!DeleteThemeFolder(t.folder, err)) {
        d.err = err;                         // Keep the dialog open with the error so the user can cancel.
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }

    // Rebuild the list and adjust selection when an earlier theme was removed.
    g_app.themes = ScanThemes();
    int n = (int)g_app.themes.size();
    int sel = g_app.selIdx - (d.idx < g_app.selIdx ? 1 : 0);
    if (sel > n - 1) sel = n - 1;
    if (sel < 0 && !g_app.cfg.noTheme) sel = 0;
    g_app.selIdx = sel;
    g_app.cfg.theme = n == 0 || sel < 0 ? L"" : g_app.themes[sel].name;
    SaveConfig(g_app.cfg);
    ClearThumbs();
    ComputeLayout(g_app.L);
    if (wasSelected) ApplyCurrent(true, true);     // If deleting the selected theme, switch to the next one immediately.

    g_app.statusMsg = AppText(L"Theme deleted: ", L"已删除主题：") + t.name + AppText(L" (restore it from the Recycle Bin)", L"（可从回收站恢复）");
    g_app.statusMsgUntil = GetTickCount64() + 6000;
    CloseDelTheme();
}

// Draw a thumbnail using aspect-preserving cover scaling.
static void DrawThumbCover(Gfx& g, ID2D1Bitmap* bmp, const D2D1_RECT_F& r, float radius,
                          const Palette& p) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    g.fillRound(rr, ColAlpha(p.textPrimary, 0.05f));
    if (bmp) {
        D2D1_SIZE_F sz = bmp->GetSize();
        float tw = r.right - r.left, th = r.bottom - r.top;
        if (sz.width > 0 && sz.height > 0) {
            float s = (sz.width / sz.height > tw / th) ? (th / sz.height) : (tw / sz.width);
            float dw = sz.width * s, dh = sz.height * s;
            D2D1_RECT_F dst = D2D1::RectF(r.left + (tw - dw) / 2, r.top + (th - dh) / 2,
                                          r.left + (tw - dw) / 2 + dw, r.top + (th - dh) / 2 + dh);
            g.pushClip(rr);
            g.dc()->DrawBitmap(bmp, dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            g.popClip();
        }
    }
    g.strokeRound(rr, ColAlpha(p.textPrimary, 0.12f), 1.0f);
}

// Text field: rounded fill, focus-accent border, text, caret, and placeholder.
// Scroll overflowing text horizontally to keep the caret visible.
static void DrawTextEdit(Gfx& g, const D2D1_RECT_F& r, TextEdit& e, const Palette& p,
                         const std::wstring& placeholder, bool focused) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 5, 5);
    g.fillRound(rr, p.dark ? Col(1, 1, 1, 0.07f) : Col(0, 0, 0, 0.035f));
    g.strokeRound(rr, focused ? p.accent : (p.dark ? Col(1, 1, 1, 0.12f) : Col(0, 0, 0, 0.10f)),
                  focused ? 1.4f : 1.0f);
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    float pad = 10.0f, avail = (r.right - r.left) - pad * 2;
    bool empty = e.text.empty() && e.preedit.empty();
    if (empty)
        g.textOptical(placeholder, f, D2D1::RectF(r.left + pad, r.top, r.right - pad, r.bottom),
                      p.textTertiary);
    g.pushClip(rr);
    float shift = 0;
    if (!empty) {
        int caretPos = 0;
        std::wstring disp = EditDisplay(e, &caretPos);
        float caretX = g.textWidth(disp.substr(0, caretPos), f);
        if (caretX > avail) shift = caretX - avail;
        // Draw the translucent accent selection highlight before the text.
        if (e.hasSel()) {
            int a, b;
            e.selRange(a, b);
            float x1 = g.textWidth(disp.substr(0, a), f);
            float x2 = g.textWidth(disp.substr(0, b), f);
            D2D1_RECT_F hl = D2D1::RectF(r.left + pad + x1 - shift, r.top + 5,
                                         r.left + pad + x2 - shift, r.bottom - 5);
            g.dc()->FillRoundedRectangle(D2D1::RoundedRect(hl, 2, 2), g.brush(ColAlpha(p.accent, 0.35f)));
        }
        g.textOptical(disp, f, D2D1::RectF(r.left + pad - shift, r.top, r.right - pad - shift, r.bottom),
                      p.textPrimary);
        if (focused && g_app.caretOn) {
            float cy = (r.top + r.bottom) / 2, chh = (r.bottom - r.top) * 0.46f;
            float x = r.left + pad + caretX - shift;
            g.dc()->DrawLine(D2D1::Point2F(x, cy - chh / 2), D2D1::Point2F(x, cy + chh / 2),
                             g.brush(p.textPrimary), 1.2f);
        }
    } else if (focused && g_app.caretOn) {
        float cy = (r.top + r.bottom) / 2, chh = (r.bottom - r.top) * 0.46f;
        g.dc()->DrawLine(D2D1::Point2F(r.left + pad, cy - chh / 2),
                         D2D1::Point2F(r.left + pad, cy + chh / 2), g.brush(p.textPrimary), 1.2f);
    }
    g.popClip();
}

// Image slot: plus/prompt when empty, cover thumbnail when selected, change overlay on hover.
static void DrawImageSlot(Gfx& g, const D2D1_RECT_F& r, ID2D1Bitmap* bmp, const std::wstring& hint,
                          const Palette& p, bool hot, bool pressed) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 6, 6);
    if (bmp) {
        DrawThumbCover(g, bmp, r, 6, p);
        if (hot || pressed) {
            g.fillRound(rr, Col(0, 0, 0, pressed ? 0.34f : 0.24f));
            IDWriteTextFormat* f = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                            DWRITE_TEXT_ALIGNMENT_CENTER);
            g.textOptical(AppText(L"Click to change", L"点击更换"), f, D2D1::RectF(r.left, r.top, r.right, r.bottom),
                          Col(1, 1, 1, 0.95f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    } else {
        g.fillRound(rr, hot || pressed ? ColAlpha(p.accent, 0.10f)
                                       : (p.dark ? Col(1, 1, 1, 0.04f) : Col(0, 0, 0, 0.025f)));
        g.strokeRound(rr, hot || pressed ? p.accent
                                         : (p.dark ? Col(1, 1, 1, 0.16f) : Col(0, 0, 0, 0.14f)), 1.0f);
        IDWriteTextFormat* gi = g.format(FONT_ICON, 22.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_TEXT_ALIGNMENT_CENTER);
        g.textOptical(L"\uE710", gi, D2D1::RectF(r.left, r.top + 36, r.right, r.top + 74),
                      hot || pressed ? p.accentText : p.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        IDWriteTextFormat* f = g.format(FONT_UI, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_TEXT_ALIGNMENT_CENTER);
        g.textOptical(hint, f, D2D1::RectF(r.left + 8, r.top + 78, r.right - 8, r.top + 102),
                      p.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

static void DrawAddThemeOverlay(Gfx& g, const Palette& p) {
    UiDim(g, g_app.winWDip, g_app.winHDip, p);
    const Layout& L = g_app.L;
    AddThemeUI& a = g_app.addUI;
    // Use an opaque dialog card so background controls do not show through.
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(L.ovCard, 8, 8);
    g.fillRound(card, p.dark ? ColHex(0x272727) : ColHex(0xFAFAFA));
    g.strokeRound(card, p.dark ? Col(1, 1, 1, 0.10f) : Col(0, 0, 0, 0.08f), 1.0f);

    IDWriteTextFormat* fTitle = g.format(FONT_UI, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fSub   = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_TEXT_ALIGNMENT_LEADING, true);
    IDWriteTextFormat* fLabel = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float x0 = L.ovCard.left + 24, x1 = L.ovCard.right - 24;

    g.textOptical(AppText(L"Add theme", L"添加主题"), fTitle, D2D1::RectF(x0, L.ovCard.top + 20, x1, L.ovCard.top + 50),
                  p.textPrimary);
    g.textOptical(AppText(L"Theme name", L"主题名称"), fLabel, D2D1::RectF(x0, L.ovCard.top + 54, x1, L.ovCard.top + 74),
                  p.textSecondary);
    DrawTextEdit(g, L.ovName, a.name, p, AppText(L"e.g. Desert, Sunrise...", L"例如：沙漠、日出…"), true);

    auto caption = [&](const D2D1_RECT_F& slot, const wchar_t* t) {
        g.textOptical(t, fLabel, D2D1::RectF(slot.left, slot.top - 22, slot.right, slot.top - 2),
                      p.textSecondary);
    };
    caption(L.ovLightSlot, AppText(L"Light", L"浅色"));
    caption(L.ovDarkSlot, AppText(L"Dark", L"深色"));
    DrawImageSlot(g, L.ovLightSlot, a.lightBmp, AppText(L"Choose light image", L"选择浅色图片"), p,
                  g_app.hot == CID_OV_LIGHT_SLOT, g_app.pressed == CID_OV_LIGHT_SLOT);
    DrawImageSlot(g, L.ovDarkSlot, a.darkBmp, AppText(L"Choose dark image", L"选择深色图片"), p,
                  g_app.hot == CID_OV_DARK_SLOT, g_app.pressed == CID_OV_DARK_SLOT);

    // Bottom hint: accent color on error; otherwise show the destination directory.
    // Validate the name each frame with pure string checks; disable Add for invalid names.
    std::wstring nm = TrimStr(a.name.text);
    std::wstring nameErr;
    bool nameOk = nm.empty() || ThemeNameValid(nm, nameErr);
    bool sameImage = !a.lightPath.empty() && !a.darkPath.empty() &&
                     _wcsicmp(a.lightPath.c_str(), a.darkPath.c_str()) == 0;
    bool ready = !a.lightPath.empty() && !a.darkPath.empty() && !sameImage &&
                 !nm.empty() && nameOk;
    std::wstring hint;
    bool hintIsError = false;
    if (!a.hint.empty()) { hint = a.hint; hintIsError = true; }
    else if (!nameOk) { hint = nameErr; hintIsError = true; }
    else if (ready) {
        std::wstring dir = UserThemeDir(), exe = ExeDir(), shown = dir;
        if (!exe.empty() && dir.rfind(exe, 0) == 0)          // Show a relative path when the destination is inside the app directory.
            shown = AppText(L"App directory", L"应用目录") + dir.substr(exe.size());
        hint = AppText(L"Copy both images to ", L"将两张图片复制到 ") + shown + L"\\" + nm +
               AppText(L"\\ (originals remain unchanged)", L"\\（保留原文件）");
    }
    if (!hint.empty())
        g.text(hint, fSub, D2D1::RectF(x0, L.ovCard.bottom - 120, x1, L.ovCard.bottom - 68),
                      hintIsError ? p.accentText : p.textSecondary);

    UiButton(g, L.ovCancel, AppText(L"Cancel", L"取消"), p, false, g_app.hot == CID_OV_CANCEL,
             g_app.pressed == CID_OV_CANCEL);
    UiButton(g, L.ovOk, AppText(L"Add", L"添加"), p, true, g_app.hot == CID_OV_OK, g_app.pressed == CID_OV_OK,
             ready, L"\uE710");
}

static void DrawRenameDialog(Gfx& g, const Palette& p) {
    UiDim(g, g_app.winWDip, g_app.winHDip, p);
    const Layout& L = g_app.L;
    RenameUI& r = g_app.renUI;
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(L.renCard, 8, 8);
    g.fillRound(card, p.dark ? ColHex(0x272727) : ColHex(0xFAFAFA));
    g.strokeRound(card, p.dark ? Col(1, 1, 1, 0.10f) : Col(0, 0, 0, 0.08f), 1.0f);

    std::wstring oldName, folder;
    if (r.idx >= 0 && r.idx < (int)g_app.themes.size()) {
        oldName = g_app.themes[r.idx].name;
        folder = g_app.themes[r.idx].folder;
    }
    IDWriteTextFormat* fTitle = g.format(FONT_UI, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fSub   = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_TEXT_ALIGNMENT_LEADING, true);
    IDWriteTextFormat* fPath  = g.format(FONT_UI, 11.0f);
    float x0 = L.renCard.left + 24, x1 = L.renCard.right - 24;

    g.textOptical(AppText(L"Rename theme: ", L"重命名主题：") + oldName, fTitle,
                  D2D1::RectF(x0, L.renCard.top + 20, x1, L.renCard.top + 48), p.textPrimary);
    g.textOptical(AppText(L"Renames the folder; images stay unchanged.", L"仅重命名文件夹，图片不会更改。"), fSub,
                  D2D1::RectF(x0, L.renCard.top + 54, x1, L.renCard.top + 76), p.textSecondary);
    // Shorten app-relative paths to avoid clipping long absolute paths.
    std::wstring shown = folder;
    std::wstring exe = ExeDir();
    if (!exe.empty() && folder.rfind(exe, 0) == 0) shown = AppText(L"App directory", L"应用目录") + folder.substr(exe.size());
    g.textOptical(shown, fPath, D2D1::RectF(x0, L.renCard.top + 78, x1, L.renCard.top + 98),
                  p.textTertiary);
    DrawTextEdit(g, L.renField, r.name, p, AppText(L"New name", L"新名称"), true);
    if (!r.hint.empty()) {
        g.text(r.hint, fSub, D2D1::RectF(x0, L.renCard.top + 146, x1, L.renCard.top + 198),
                      p.accentText);
    }
    UiButton(g, L.renCancel, AppText(L"Cancel", L"取消"), p, false, g_app.hot == CID_REN_CANCEL,
             g_app.pressed == CID_REN_CANCEL);
    UiButton(g, L.renOk, AppText(L"Save", L"保存"), p, true, g_app.hot == CID_REN_OK, g_app.pressed == CID_REN_OK,
             !TrimStr(r.name.text).empty());
}

static void DrawDelThemeDialog(Gfx& g, const Palette& p) {
    UiDim(g, g_app.winWDip, g_app.winHDip, p);
    const Layout& L = g_app.L;
    DelUI& d = g_app.delUI;
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(L.dlgCard, 8, 8);
    g.fillRound(card, p.dark ? ColHex(0x272727) : ColHex(0xFAFAFA));
    g.strokeRound(card, p.dark ? Col(1, 1, 1, 0.10f) : Col(0, 0, 0, 0.08f), 1.0f);

    std::wstring name, folder;
    if (d.idx >= 0 && d.idx < (int)g_app.themes.size()) {
        name = g_app.themes[d.idx].name;
        folder = g_app.themes[d.idx].folder;
    }
    IDWriteTextFormat* fTitle = g.format(FONT_UI, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fBody  = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_TEXT_ALIGNMENT_LEADING, true);
    IDWriteTextFormat* fPath  = g.format(FONT_UI, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_TEXT_ALIGNMENT_LEADING, true);
    float x0 = L.dlgCard.left + 24, x1 = L.dlgCard.right - 24, y0 = L.dlgCard.top;

    g.textOptical(AppText(L"Delete theme: ", L"删除主题：") + name + L"?", fTitle,
                  D2D1::RectF(x0, y0 + 20, x1, y0 + 48), p.textPrimary);
    g.text(AppText(L"Move this theme to the Recycle Bin. You can restore it later. Original images stay unchanged.", L"主题将移到回收站，可随时恢复。原始图片不会更改。"),
           fBody, D2D1::RectF(x0, y0 + 56, x1, y0 + 92), p.textSecondary);
    g.text(folder, fPath, D2D1::RectF(x0, y0 + 92, x1, y0 + 132), p.textTertiary);
    if (!d.err.empty()) {
        IDWriteTextFormat* fErr = g.format(FONT_UI, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_TEXT_ALIGNMENT_LEADING, true);
        g.text(d.err, fErr, D2D1::RectF(x0, y0 + 132, x1, y0 + 184), ColHex(0xC42B1C));
    }

    UiButton(g, L.dlgCancel, AppText(L"Cancel", L"取消"), p, false, g_app.hot == CID_DLG_CANCEL,
             g_app.pressed == CID_DLG_CANCEL);
    // Use a temporary danger-accent palette for the Delete button.
    Palette pd = p;
    pd.accent        = ColHex(0xC42B1C);
    pd.accentHover   = ColHex(0xD13A2B);
    pd.accentPressed = ColHex(0xA82318);
    pd.textOnAccent  = Col(1, 1, 1, 1);
    UiButton(g, L.dlgOk, AppText(L"Delete", L"删除"), pd, true, g_app.hot == CID_DLG_OK, g_app.pressed == CID_DLG_OK);
}

// ---------------------------------------------------------------------------
// Window backdrop.
//
// On Windows 11 22H2+, DWMWA_SYSTEMBACKDROP_TYPE=3 blurs content behind the window live.
// The client area must remain transparent to reveal DWM's backdrop,
// and the window must have WS_EX_NOREDIRECTIONBITMAP.
// Windows 10 uses a solid theme-aware background; AccentPolicy acrylic lags during dragging,
// and wallpaper-based simulated blur cannot reflect other windows.
// --self-glass forces DrawGlassBackdrop; --opaque uses an opaque gradient.
// ---------------------------------------------------------------------------
static bool IsWin11_22H2OrLater() {
    typedef LONG (WINAPI* PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return false;
    PFN_RtlGetVersion fn = (PFN_RtlGetVersion)(void*)GetProcAddress(nt, "RtlGetVersion");
    if (!fn) return false;
    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return false;
    return vi.dwMajorVersion > 10 ||
           (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22621);
}

// Simulate frosted glass by downsampling the wallpaper to roughly 1/16 then enlarging it,
// with a tint and subtle noise. Map the wallpaper in cover mode to screen coordinates,
// and resample changes within about 1.5 seconds so window movement reveals the matching area.
struct Glass {
    std::wstring path;                      // Loaded wallpaper path.
    ID2D1Bitmap* small = nullptr;           // Downsampled wallpaper used as the blur source.
    ID2D1Bitmap* noise = nullptr;           // Tiled 128x128 noise texture.
    ID2D1BitmapBrush* noiseBrush = nullptr;
    int screenW = 0, screenH = 0;
};
static Glass g_glass;

// Lower alpha is clearer; higher alpha produces a stronger frosted tint.
static D2D1_COLOR_F GlassVeil(const Palette& p) {
    return p.dark ? Col(0.05f, 0.05f, 0.06f, 0.38f)
                  : Col(1.0f, 1.0f, 1.0f, 0.46f);
}

static void EnsureGlassBitmap(bool force = false) {
    if (!g_app.gfx.dc()) return;
    static ULONGLONG lastCheck = 0;
    ULONGLONG now = GetTickCount64();
    if (!force && now - lastCheck < 1500) return;   // Read the registry at most once every 1.5 seconds.
    lastCheck = now;
    std::wstring wp = CurrentWallpaperPath();
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (g_glass.small && wp == g_glass.path && sw == g_glass.screenW && sh == g_glass.screenH)
        return;
    if (g_glass.small) { g_glass.small->Release(); g_glass.small = nullptr; }
    g_glass.path = wp;
    g_glass.screenW = sw;
    g_glass.screenH = sh;
    if (wp.empty()) return;
    int smallW = sw / 16;                           // Smaller source images produce stronger blur.
    if (smallW < 48) smallW = 48;
    g_glass.small = g_app.gfx.loadImage(wp, (UINT)smallW);
}

// Fixed-seed noise with alternating subtle light/dark pixels and premultiplied alpha 0..13.
static void EnsureGlassNoise(Gfx& g) {
    if (g_glass.noiseBrush) return;
    const int N = 128;
    unsigned* px = new unsigned[N * N];
    unsigned s = 0x9E3779B9u;
    for (int i = 0; i < N * N; ++i) {
        s = s * 1664525u + 1013904223u;
        unsigned v = (s >> 16) & 0xFF;
        unsigned a = v % 14;
        unsigned c = (v & 1) ? a : 0;
        px[i] = (a << 24) | (c << 16) | (c << 8) | c;
    }
    g_glass.noise = g.createBitmapFromPixels(px, N, N);
    delete[] px;
    if (g_glass.noise) {
        D2D1_BITMAP_BRUSH_PROPERTIES bp = D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        g.dc()->CreateBitmapBrush(g_glass.noise, bp, &g_glass.noiseBrush);
    }
}

static void DrawGlassBackdrop(Gfx& g, const Palette& p) {
    const float w = g_app.winWDip, h = g_app.winHDip;
    EnsureGlassBitmap();
    if (g_glass.small) {
        // Map the window's screen rectangle back into the wallpaper's cover-scaled bitmap.
        D2D1_SIZE_F bm = g_glass.small->GetSize();
        float sw = (float)g_glass.screenW, sh = (float)g_glass.screenH;
        if (bm.width > 0 && bm.height > 0) {
            float k = (sw / bm.width > sh / bm.height) ? (sw / bm.width) : (sh / bm.height);
            float ox = (sw - bm.width * k) / 2, oy = (sh - bm.height * k) / 2;
            RECT wr;
            GetWindowRect(g_app.hwnd, &wr);
            float sc = g_app.gfx.scale();
            D2D1_RECT_F src = D2D1::RectF(
                ((float)wr.left - ox) / k, ((float)wr.top - oy) / k,
                ((float)wr.left + w * sc - ox) / k, ((float)wr.top + h * sc - oy) / k);
            g.dc()->DrawBitmap(g_glass.small, D2D1::RectF(0, 0, w, h), 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
        }
    } else {
        // Use a solid fill if there is no wallpaper, such as a solid-color desktop.
        g.dc()->Clear(p.dark ? ColHex(0x1C1C1C) : ColHex(0xF3F3F3));
    }
    // Tint preserves text and control readability.
    g.dc()->FillRectangle(D2D1::RectF(0, 0, w, h), g.brush(GlassVeil(p)));
    // Anchor noise to the screen so it does not slide with the dragged window.
    EnsureGlassNoise(g);
    if (g_glass.noiseBrush) {
        RECT wr;
        GetWindowRect(g_app.hwnd, &wr);
        float sc = g_app.gfx.scale();
        float ox = (float)(((int)(wr.left / sc)) % 128);
        float oy = (float)(((int)(wr.top / sc)) % 128);
        g_glass.noiseBrush->SetTransform(D2D1::Matrix3x2F::Translation(ox, oy));
        g.dc()->FillRectangle(D2D1::RectF(0, 0, w, h), g_glass.noiseBrush);
    }
}

// WinUI-style thin scrollbar appears on scroll and fades after roughly one second of inactivity.
// Do not draw the track; increase thumb opacity on hover, as on Windows 11.
static void DrawScrollbar(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    if (L.maxScroll <= 0.0f || g_app.sbAlpha <= 0.01f) return;
    D2D1_RECT_F th = ScrollThumbRect();
    bool hot = (g_app.hot == CID_SCROLLBAR || g_app.sbDragging);
    float a = g_app.sbAlpha * (hot ? 0.42f : 0.26f);
    D2D1_COLOR_F c = p.dark ? Col(1, 1, 1, a) : Col(0, 0, 0, a);
    g.fillRound(D2D1::RoundedRect(th, 3.0f, 3.0f), c);
}

static void RenderFrame() {
    Gfx& g = g_app.gfx;
    if (!g.dc()) return;
    const Palette& p = g_app.pal;
    SyncThumbs();                            // Thumbnail synchronization may create/release D2D bitmaps, so run it before begin().
    if (!g.begin()) return;
    g.dc()->Clear(Col(0, 0, 0, 0));

    // Window animation: opacity and a slight upward slide; winAnim 0=hidden, 1=fully visible.
    float frameAlpha = g_app.winAnim;
    bool useLayer = frameAlpha < 0.999f;
    float baseTy = 0.0f;                     // Vertical content offset combines the entrance slide and page scroll.
    if (useLayer) {
        if (!g_app.uiLayer) g.dc()->CreateLayer(nullptr, &g_app.uiLayer);
        if (g_app.uiLayer) {
            D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
                D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(), frameAlpha, nullptr, D2D1_LAYER_OPTIONS_NONE);
            baseTy = (1.0f - g_app.winAnim) * 16.0f;
            g.dc()->PushLayer(&lp, g_app.uiLayer);
        } else {
            useLayer = false;
        }
    }
    g_app.frameLayerPushed = useLayer;

    // Backdrop: system acrylic, custom frosted glass, or opaque gradient.
    if (g_app.liveGlass) {
        // Keep the client transparent for DWM blur, then add a thin tint for readability.
        g.dc()->Clear(Col(0, 0, 0, 0));
        D2D1_COLOR_F veil = p.dark ? Col(0.04f, 0.04f, 0.05f, 0.32f)
                                   : Col(1.0f, 1.0f, 1.0f, 0.40f);
        g.dc()->FillRectangle(D2D1::RectF(0, 0, g_app.winWDip, g_app.winHDip), g.brush(veil));
    } else if (g_app.glass) {
        DrawGlassBackdrop(g, p);
    } else if (g_app.solidBg) {
        // Windows 10 default: solid theme color, light 0xF3F3F3 or dark 0x202020.
        g.dc()->Clear(p.windowBgTop);
    } else {
        if (!g_app.bgBrush) {
            ID2D1GradientStopCollection* stops = nullptr;
            D2D1_GRADIENT_STOP gs[2] = {{0.0f, p.windowBgTop}, {1.0f, p.windowBgBottom}};
            if (SUCCEEDED(g.dc()->CreateGradientStopCollection(gs, 2, &stops)) && stops) {
                D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props =
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0),
                                                        D2D1::Point2F(0, g_app.winHDip));
                g.dc()->CreateLinearGradientBrush(props, stops, &g_app.bgBrush);
                stops->Release();
            }
        }
        if (g_app.bgBrush)
            g.dc()->FillRectangle(D2D1::RectF(0, 0, g_app.winWDip, g_app.winHDip), g_app.bgBrush);
        else
            g.dc()->Clear(p.windowBgTop);
    }

    // Clip below the title bar and translate by scroll; title bar and dialogs stay fixed.
    g.dc()->PushAxisAlignedClip(
        D2D1::RectF(0, Ui::TitleBarH, g_app.winWDip, g_app.winHDip),
        D2D1_ANTIALIAS_MODE_ALIASED);
    g.dc()->SetTransform(D2D1::Matrix3x2F::Translation(0, baseTy - g_app.scroll));

    DrawThemes(g, p);
    DrawSwitchSection(g, p);
    DrawTimeSection(g, p);
    DrawAppearance(g, p);
    DrawOptions(g, p);
    DrawStatus(g, p);

    // Bottom action; settings apply automatically, while Apply now is available in the tray menu.
    UiButton(g, g_app.L.openThemesBtn, AppText(L"Open themes folder", L"打开主题文件夹"), p, false,
             g_app.hot == CID_OPENTHEMES, g_app.pressed == CID_OPENTHEMES, true);

    g.dc()->SetTransform(D2D1::Matrix3x2F::Identity());
    g.dc()->PopAxisAlignedClip();

    DrawTitleBar(g, p);
    DrawScrollbar(g, p);

    if (g_app.delUI.active) DrawDelThemeDialog(g, p);
    if (g_app.renUI.active) DrawRenameDialog(g, p);
    if (g_app.addUI.active) DrawAddThemeOverlay(g, p);   // Draw modal dialogs above all other content.

    if (useLayer) {
        g.dc()->PopLayer();
        g.dc()->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    g.end();
}

// ---------------------------------------------------------------------------
// System tray.
// ---------------------------------------------------------------------------
static void TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, AppText(L"Show window", L"打开主窗口"));
    AppendMenuW(m, MF_STRING, 2, AppText(L"Apply now", L"立即应用"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_app.cfg.autoSwitch ? MF_CHECKED : 0), 3, AppText(L"Switch wallpapers automatically", L"自动切换壁纸"));
    // Switch-mode submenu shares the main window's setting.
    HMENU swmode = CreatePopupMenu();
    AppendMenuW(swmode, MF_STRING | (g_app.cfg.switchMode == 0 ? MF_CHECKED : 0), 21, AppText(L"Schedule", L"定时切换"));
    AppendMenuW(swmode, MF_STRING | (g_app.cfg.switchMode == 1 ? MF_CHECKED : 0), 22, AppText(L"Follow system theme", L"跟随系统外观"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)swmode, AppText(L"Switch mode", L"切换模式"));
    AppendMenuW(m, MF_STRING | (g_app.cfg.startOnBoot ? MF_CHECKED : 0), 4, AppText(L"Start with Windows", L"随 Windows 启动"));
    // Appearance submenu shares the main window's setting.
    HMENU appearance = CreatePopupMenu();
    AppendMenuW(appearance, MF_STRING | (g_app.cfg.appearance == 0 ? MF_CHECKED : 0), 11, AppText(L"System", L"跟随系统"));
    AppendMenuW(appearance, MF_STRING | (g_app.cfg.appearance == 1 ? MF_CHECKED : 0), 12, AppText(L"Light", L"浅色"));
    AppendMenuW(appearance, MF_STRING | (g_app.cfg.appearance == 2 ? MF_CHECKED : 0), 13, AppText(L"Dark", L"深色"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)appearance, AppText(L"Appearance", L"外观"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 9, AppText(L"Quit", L"退出"));
    SetForegroundWindow(g_app.hwnd);
    POINT pt;
    GetCursorPos(&pt);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case 1:
        ShowMainWindow();
        break;
    case 2: ApplyCurrent(true, true); break;
    case 3:
        g_app.cfg.autoSwitch = !g_app.cfg.autoSwitch;
        g_app.toggleTarget[0] = g_app.cfg.autoSwitch ? 1.0f : 0.0f;
        SaveConfig(g_app.cfg);
        if (g_app.cfg.autoSwitch) { CheckExternalWallpaper(); ApplyCurrent(true); }
        StartAnim();
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        break;
    case 4:
        g_app.cfg.startOnBoot = !g_app.cfg.startOnBoot;
        g_app.toggleTarget[1] = g_app.cfg.startOnBoot ? 1.0f : 0.0f;
        SetAutoStart(g_app.cfg.startOnBoot);
        SaveConfig(g_app.cfg);
        StartAnim();
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        break;
    case 11: case 12: case 13:
        g_app.cfg.appearance = cmd - 11;
        SaveConfig(g_app.cfg);
        RefreshPalette();
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        break;
    case 21: case 22:
        g_app.cfg.switchMode = cmd - 21;
        g_app.swTarget = g_app.cfg.switchMode;
        SaveConfig(g_app.cfg);
        if (g_app.cfg.autoSwitch) { CheckExternalWallpaper(); ApplyCurrent(true); }
        StartAnim();
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        break;
    case 9: DestroyWindow(g_app.hwnd); break;
    default: break;
    }
}

// Explorer broadcasts TaskbarCreated after restarting, which removes icons from the old tray.
// Re-add the tray icon when receiving this message.
static UINT g_taskbarCreatedMsg = 0;

static void AddTrayIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 2;
    // Load at the DPI-aware small-icon size (32x32 at 200%) to avoid blurry upscaling.
    // Reuse the handle to avoid leaking GDI resources when the tray icon is re-added.
    if (!g_app.trayIcon) {
        int cx = GetSystemMetrics(SM_CXSMICON);
        int cy = GetSystemMetrics(SM_CYSMICON);
        if (cx <= 0) cx = 16;
        if (cy <= 0) cy = 16;
        g_app.trayIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
                                           IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
        if (!g_app.trayIcon)
            g_app.trayIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
                                               IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_DEFAULTCOLOR);
    }
    nid.hIcon = g_app.trayIcon;
    wcscpy_s(nid.szTip, kAppDisplay);
    g_app.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

static void RemoveTrayIcon() {
    if (!g_app.trayAdded) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_app.trayAdded = false;
}

// ---------------------------------------------------------------------------
// Thumbnails and palette.
// ---------------------------------------------------------------------------
static void ClearThumbs() {
    g_thumbLoader.cancel(++g_thumbGeneration);
    for (ID2D1Bitmap* b : g_app.thumbs)
        if (b) b->Release();
    g_app.thumbs.clear();
    g_app.thumbTried.clear();
}

// Keep only visible thumbnails plus a buffer row above and below to bound memory use.
// Decode on the background thread; create GPU bitmaps on the UI thread.
static void SyncThumbs() {
    Layout& L = g_app.L;
    const int n = (int)g_app.themes.size();
    if ((int)g_app.thumbs.size() != n) {     // Theme list changed after add, delete, rename, or rescan.
        ClearThumbs();
        g_app.thumbs.assign(n, nullptr);
        g_app.thumbTried.assign(n, 0);
    }
    if (n == 0) {
        L.firstVis = 0;
        L.lastVis = -1;
        return;
    }
    VisibleCardRange(L, g_app.scroll, L.firstVis, L.lastVis);
    if (L.lastVis < L.firstVis) {            // No cards intersect the viewport.
        L.firstVis = 0;
        L.lastVis = -1;
    }
    const int perRow = L.perRow > 0 ? L.perRow : 1;
    const int keepA = L.firstVis - perRow;
    const int keepB = L.lastVis + perRow;
    for (int i = 0; i < n; ++i) {
        bool keep = (i >= keepA && i <= keepB);
        if (!keep && g_app.thumbs[i]) {
            g_app.thumbs[i]->Release();
            g_app.thumbs[i] = nullptr;
            g_app.thumbTried[i] = 0;
        }
    }
    std::vector<ThumbRequest> requests;
    // Decode visible cards before buffer rows.
    for (int i = L.firstVis; i <= L.lastVis; ++i) {
        if (i < 0 || i >= n) continue;
        if (!g_app.thumbs[i] && !g_app.thumbTried[i])
            requests.push_back({g_thumbGeneration, i, g_app.themes[i].dayImage});
    }
    for (int i = keepA; i <= keepB; ++i) {
        if (i < 0 || i >= n) continue;
        if (i >= L.firstVis && i <= L.lastVis) continue;
        if (!g_app.thumbs[i] && !g_app.thumbTried[i])
            requests.push_back({g_thumbGeneration, i, g_app.themes[i].dayImage});
    }
    g_thumbLoader.submit(g_app.hwnd, kThumbReadyMessage, g_thumbGeneration, std::move(requests));
}

// Appearance: 0=system, 1=light, 2=dark.
static void RefreshPalette() {
    bool dark = g_app.cfg.appearance == 2
                    ? true
                    : (g_app.cfg.appearance == 1 ? false : SystemUsesDarkMode() != 0);
    if (g_app.bgBrush) { g_app.bgBrush->Release(); g_app.bgBrush = nullptr; }
    g_app.pal = MakePalette(dark, SystemAccentColor());
    SetDarkModeAttribute(g_app.hwnd, dark);
}

// ---------------------------------------------------------------------------
// Animation.
// ---------------------------------------------------------------------------
// Use QPC for sub-millisecond animation timing. GetTickCount64 has roughly 15.6 ms granularity,
// which timeBeginPeriod(1) does not improve; interpolating with it skips 2-3 display refreshes
// at a time and creates uneven motion on a 120 Hz screen.
static double qpcMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}
static int g_paintCount = 0;        // WM_PAINT count for the TW_ANIMLOG diagnostic hook.
static double g_lastFrameEnd = 0;   // QPC time at the end of the last frame for TW_ANIMLOG.

static void StartAnim() {
    if (!g_app.gfx.dc() || !IsWindowVisible(g_app.hwnd)) return;
    // Interpolate toggles, segments, and fades using elapsed time; an 8 ms timer is sufficient.
    // Zoom animation instead runs synchronously in RunZoomAnim from StartZoom,
    // paced by DwmFlush so WM_TIMER granularity does not cap it near 64 fps.
    if (SetTimer(g_app.hwnd, TM_ANIM, 8, nullptr)) g_animTimerPrecision.acquire();
}

// WINDOWPLACEMENT rcNormalPosition uses workspace coordinates, while GetWindowRect uses
// screen coordinates. Convert between them to avoid a restore offset,
// such as the 76px taskbar height.
static RECT ScreenToWorkArea(const RECT& r) {
    RECT w = r;
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) {
        w.left -= mi.rcWork.left;  w.right -= mi.rcWork.left;
        w.top -= mi.rcWork.top;    w.bottom -= mi.rcWork.top;
    }
    return w;
}
static RECT WorkAreaToScreen(const RECT& w, HWND h) {
    RECT r = w;
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
        r.left += mi.rcWork.left;  r.right += mi.rcWork.left;
        r.top += mi.rcWork.top;    r.bottom += mi.rcWork.top;
    }
    return r;
}

// Finalize maximize/restore only after the animated rectangle reaches the target.
// Use SetWindowPlacement only to update the saved restore rectangle, and ShowWindow
// to change state in one step. Changing both through SetWindowPlacement can first move
// to rcNormalPosition and then change state, causing a visible jump.
//
// The two directions need opposite ordering because rcNormalPosition changes meaning:
// while maximized, it is only a saved restore position and causes no visible movement;
// while restored, it is the live window position and writing it moves the window.
// Therefore:
// To maximize, first call SW_MAXIMIZE after the animation has reached the work area,
// then write rcNormalPosition while maximized, which changes only saved data.
// Reversing that order flashes back to the old position before maximizing.
// To restore, write rcNormalPosition while maximized, then call SW_RESTORE.
// DWMWA_TRANSITIONS_FORCEDISABLED prevents DWM from adding a second maximize/restore
// animation after the custom transition, which would cause a visible final bounce.
#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#endif

static void ApplyZoomState(bool toMax) {
    g_app.maximized = toMax;
    BOOL noTrans = TRUE;
    DwmSetWindowAttribute(g_app.hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &noTrans, sizeof(noTrans));
    WINDOWPLACEMENT wp;
    wp.length = sizeof(wp);
    if (toMax) {
        ShowWindow(g_app.hwnd, SW_MAXIMIZE);
        if (g_app.normRect.right > g_app.normRect.left && GetWindowPlacement(g_app.hwnd, &wp)) {
            wp.showCmd = SW_SHOWMAXIMIZED;               // Keep the current state so only saved placement data changes.
            wp.flags = 0;
            wp.rcNormalPosition = ScreenToWorkArea(g_app.normRect);
            SetWindowPlacement(g_app.hwnd, &wp);
        }
    } else {
        if (GetWindowPlacement(g_app.hwnd, &wp)) {
            wp.showCmd = IsZoomed(g_app.hwnd) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;  // Keep the current state.
            wp.flags = 0;
            if (g_app.normRect.right > g_app.normRect.left)
                wp.rcNormalPosition = ScreenToWorkArea(g_app.normRect);
            SetWindowPlacement(g_app.hwnd, &wp);
        }
        ShowWindow(g_app.hwnd, SW_RESTORE);
        if (g_app.normRect.right > g_app.normRect.left) {
            // Restore the tracked target immediately to discard intermediate placement frames.
            g_app.zooming = false;
            TrackNormRect();
        }
    }
    noTrans = FALSE;
    DwmSetWindowAttribute(g_app.hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &noTrans, sizeof(noTrans));
    // Use square corners when maximized and rounded corners when restored.
    // Set this explicitly after the state change because an unchanged rectangle may not trigger WM_SIZE.
    int corner = toMax ? 1 /*DONOTROUND*/ : 2 /*ROUND*/;
    DwmSetWindowAttribute(g_app.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    g_app.zooming = false;
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// Remember the normal window rectangle for restore animations; ignore zoom, maximize,
// and minimize states so intermediate/fullscreen rectangles do not overwrite it.
static void TrackNormRect() {
    if (g_app.zooming || g_app.maximized) return;
    if (IsZoomed(g_app.hwnd) || IsIconic(g_app.hwnd)) return;
    RECT r;
    if (GetWindowRect(g_app.hwnd, &r) && r.right > r.left && r.bottom > r.top)
        g_app.normRect = r;
}

// Animate the window rectangle from zoomFrom to zoomTo synchronously.
// Each frame changes the rectangle (WM_SIZE updates layout), repaints immediately,
// then waits for composition with DwmFlush (about 8.3 ms/frame at 120 Hz).
// WM_TIMER's roughly 10-15.6 ms minimum would drop frames on fast displays.
// Starting directly avoids the 15-31 ms first-frame delay of a timer.
// Progress uses QPC; see qpcMs.
static void RunZoomAnim() {
    AnimationTimerPrecision precision;
    precision.acquire();
    float dur = g_app.zoomMs > 40.0f ? g_app.zoomMs : 40.0f;
    g_app.gfx.presentSync = 0;      // Let DwmFlush pace frames; Present must not also wait for vertical sync.
    // TW_ANIMLOG=1 appends per-frame set/paint/flush timings to %TEMP%\tw_anim.log.
    // Check adjacent frame timestamps against the refresh interval, plus the flush duration.
    static FILE* alog = nullptr;
    static bool alogInit = false;
    if (!alogInit) {
        alogInit = true;
        if (_wgetenv(L"TW_ANIMLOG")) {
            wchar_t tmp[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, tmp);
            if (n && n < MAX_PATH - 20) wcscat_s(tmp, L"tw_anim.log");
            alog = _wfopen(tmp, L"a, ccs=UTF-8");
        }
    }
    int frameNo = 0;
    double tStartWall = qpcMs();
    for (;;) {
        double c0 = qpcMs();
        float t = (float)((c0 - g_app.zoomStartMs) / dur);
        if (t > 1.0f) t = 1.0f;
        if (t < 0.0f) t = 0.0f;
        float e = 1.0f - powf(1.0f - t, 3.0f);               // Cubic ease-out: fast initially, slower near the end.
        RECT r;
        r.left   = g_app.zoomFrom.left   + (LONG)((g_app.zoomTo.left   - g_app.zoomFrom.left)   * e);
        r.top    = g_app.zoomFrom.top    + (LONG)((g_app.zoomTo.top    - g_app.zoomFrom.top)    * e);
        r.right  = g_app.zoomFrom.right  + (LONG)((g_app.zoomTo.right  - g_app.zoomFrom.right)  * e);
        r.bottom = g_app.zoomFrom.bottom + (LONG)((g_app.zoomTo.bottom - g_app.zoomFrom.bottom) * e);
        if (r.right - r.left < 1) r.right = r.left + 1;
        if (r.bottom - r.top < 1) r.bottom = r.top + 1;
        int paintBefore = g_paintCount;
        SetWindowPos(g_app.hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        double c1 = qpcMs();
        UpdateWindow(g_app.hwnd);       // Paint this frame immediately; Present runs inside WM_PAINT.
        double c2 = qpcMs();
        DwmFlush();                     // Wait for the next composition cycle to match the display refresh rate.
        double c3 = qpcMs();
        if (alog)
            fwprintf(alog, L"frame %d t=%.4f rect=%ld,%ld,%ld,%ld set=%.2f paint=%.2f(repaint=%d) flush=%.2f total=%.2f gap_us=%.0f\n",
                     frameNo, t, r.left, r.top, r.right, r.bottom,
                     (c1 - c0) * 1000.0, (c2 - c1) * 1000.0, g_paintCount - paintBefore,
                     (c3 - c2) * 1000.0, (c3 - c0) * 1000.0,
                     g_lastFrameEnd > 0 ? (c0 - g_lastFrameEnd) * 1000.0 : 0.0);
        g_lastFrameEnd = c3;
        ++frameNo;
        if (t >= 1.0f) {
            double z0 = qpcMs();
            ApplyZoomState(g_app.zoomToMax);
            if (alog) {
                fwprintf(alog, L"apply %.2f us_total=%.2f frames=%d\n",
                         (qpcMs() - z0) * 1000.0, (qpcMs() - tStartWall) * 1000.0, frameNo);
                fflush(alog);
            }
            break;
        }
    }
    g_app.gfx.presentSync = 1;
}

// Start a smooth transition from the current rectangle to the work area or restore rectangle.
// The 200 ms cubic ease-out approximates Windows 11 maximize motion. Each frame runs
// SetWindowPos -> WM_SIZE -> ComputeLayout so content follows the changing window size.
static void StartZoom(bool toMax) {
    HWND h = g_app.hwnd;
    if (g_app.zooming) return;                       // Ignore new requests while a transition is running.
    RECT cur;
    GetWindowRect(h, &cur);
    RECT target;
    if (toMax) {
        MONITORINFO mi = {sizeof(mi)};
        if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
        target = mi.rcWork;                          // Maximize to the work area, matching WM_GETMINMAXINFO.
    } else {
        // Use our saved restore rectangle; the system record may have changed during animation.
        if (g_app.normRect.right > g_app.normRect.left) {
            target = g_app.normRect;
        } else {
            WINDOWPLACEMENT wp;
            wp.length = sizeof(wp);
            if (!GetWindowPlacement(h, &wp)) return;
            target = WorkAreaToScreen(wp.rcNormalPosition, h);
            g_app.normRect = target;
        }
    }
    if (EqualRect(&cur, &target)) { ApplyZoomState(toMax); return; }
    if (toMax) g_app.normRect = cur;   // At maximize start, the current rectangle is the future restore destination.
    g_app.zoomFrom = cur;
    g_app.zoomTo = target;
    g_app.zoomToMax = toMax;
    g_app.zooming = true;
    g_app.zoomStartMs = qpcMs();
    RunZoomAnim();                     // Start immediately without waiting for WM_TIMER.
}

// show=true fades/slides in; false fades out then hides the window.
static void StartWindowAnim(bool show) {
    g_app.winAnimTarget = show ? 1.0f : 0.0f;
    g_app.winAnimating = true;
    g_app.hideAfterAnim = !show;
    if (show) g_app.winAnim = 0.0f;
    StartAnim();
}

static bool EnsureUiInitialized() {
    if (g_app.gfx.dc()) return true;
    if (!g_app.gfx.init(g_app.hwnd)) {
        MessageBoxW(g_app.hwnd, AppText(L"Graphics initialization failed. DirectX 11 and Windows 10 or later are required.", L"图形初始化失败。需要 DirectX 11 和 Windows 10 或更高版本。"),
                    kAppDisplay, MB_OK | MB_ICONERROR);
        return false;
    }
    RECT rc;
    GetClientRect(g_app.hwnd, &rc);
    float sc = g_app.gfx.scale();
    g_app.winWDip = (rc.right - rc.left) / sc;
    g_app.winHDip = (rc.bottom - rc.top) / sc;
    ComputeLayout(g_app.L);
    EnsureCardVisible(g_app.selIdx, true);
    if (g_app.glass) EnsureGlassBitmap(true);
    return true;
}

static void ShowMainWindow() {
    if (!EnsureUiInitialized()) return;
    if (!IsWindowVisible(g_app.hwnd)) {
        g_app.toggleAnim[0] = g_app.toggleTarget[0];
        g_app.toggleAnim[1] = g_app.toggleTarget[1];
        g_app.segAnim = (float)g_app.segTarget;
        g_app.swAnim = (float)g_app.swTarget;
    }
    if (IsIconic(g_app.hwnd)) {
        SendMessageW(g_app.hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        SetForegroundWindow(g_app.hwnd);
        return;
    }
    ShowWindow(g_app.hwnd, SW_SHOW);
    SetForegroundWindow(g_app.hwnd);
    StartWindowAnim(true);
}

static void HideMainWindow() {
    // On the first close-to-tray action, explain that the app is still running.
    static bool tipped = false;
    if (!tipped && g_app.trayAdded) {
        tipped = true;
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_app.hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_INFO;
        wcscpy_s(nid.szInfoTitle, AppText(L"Still running in the background", L"应用仍在后台运行"));
        wcscpy_s(nid.szInfo, AppText(L"The app is in the system tray and will keep switching wallpapers. Click the tray icon to reopen it, or right-click to quit.", L"应用已最小化到系统托盘，将继续切换壁纸。点击托盘图标可重新打开，右键单击可退出。"));
        nid.hBalloonIcon = (HICON)SendMessageW(g_app.hwnd, WM_GETICON, ICON_BIG, 0);
        nid.dwInfoFlags = nid.hBalloonIcon ? NIIF_USER | NIIF_LARGE_ICON : NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    StartWindowAnim(false);
}

static void StepAnim() {
    bool busy = false;
    // Scale exponential easing by actual elapsed QPC time so motion is frame-rate independent.
    static double lastMs = 0;
    double nowMs = qpcMs();
    float dt = lastMs > 0 ? (float)(nowMs - lastMs) : 15.6f;
    lastMs = nowMs;
    if (dt < 2.0f) dt = 2.0f;
    if (dt > 60.0f) dt = 60.0f;
    auto ease = [&](float k0) { return 1.0f - powf(1.0f - k0, dt / 15.6f); };
    for (int i = 0; i < 2; ++i) {        // Toggle knobs.
        float d = g_app.toggleTarget[i] - g_app.toggleAnim[i];
        if (d > 0.001f || d < -0.001f) {
            g_app.toggleAnim[i] += d * ease(0.35f);
            busy = true;
        } else {
            g_app.toggleAnim[i] = g_app.toggleTarget[i];
        }
    }
    {   // Appearance indicator: slide in about 0.15-0.2 seconds and scale when pressed.
        float d = (float)g_app.segTarget - g_app.segAnim;
        if (d > 0.002f || d < -0.002f) {
            g_app.segAnim += d * ease(0.32f);
            busy = true;
        } else {
            g_app.segAnim = (float)g_app.segTarget;
        }
        float s = g_app.segPressed ? 0.93f : 1.0f;
        float ds = s - g_app.segScale;
        if (ds > 0.002f || ds < -0.002f) {
            g_app.segScale += ds * ease(0.45f);
            busy = true;
        } else {
            g_app.segScale = s;
        }
    }
    {   // Switch-mode indicator uses the same animation as appearance.
        float d = (float)g_app.swTarget - g_app.swAnim;
        if (d > 0.002f || d < -0.002f) {
            g_app.swAnim += d * ease(0.32f);
            busy = true;
        } else {
            g_app.swAnim = (float)g_app.swTarget;
        }
        float s = g_app.swPressed ? 0.93f : 1.0f;
        float ds = s - g_app.swScale;
        if (ds > 0.002f || ds < -0.002f) {
            g_app.swScale += ds * ease(0.45f);
            busy = true;
        } else {
            g_app.swScale = s;
        }
    }
    if (g_app.winAnimating) {
        float d = g_app.winAnimTarget - g_app.winAnim;
        if (d > 0.004f || d < -0.004f) {
            g_app.winAnim += d * ease(0.30f);
            busy = true;
        } else {
            g_app.winAnim = g_app.winAnimTarget;
            g_app.winAnimating = false;
            if (g_app.hideAfterAnim) {
                g_app.hideAfterAnim = false;
                ShowWindow(g_app.hwnd, SW_HIDE);
            }
        }
    }
    {   // The wheel updates scrollTarget; exponential easing gives smooth page scrolling.
        float d = g_app.scrollTarget - g_app.scroll;
        if (d > 0.3f || d < -0.3f) {
            g_app.scroll += d * ease(0.28f);
            busy = true;
        } else {
            g_app.scroll = g_app.scrollTarget;
        }
    }
    {   // Fade the scrollbar in while scrolling and out after about 0.9 seconds; keep the timer active.
        bool recent = (qpcMs() - g_app.lastScrollMs) < 900.0;
        float targetA = (g_app.L.maxScroll > 0.0f && (recent || g_app.sbDragging)) ? 1.0f : 0.0f;
        float da = targetA - g_app.sbAlpha;
        if (da > 0.004f || da < -0.004f) {
            g_app.sbAlpha += da * ease(0.22f);
            busy = true;
        } else {
            g_app.sbAlpha = targetA;
        }
        if (targetA > 0.5f && g_app.sbAlpha > 0.002f) busy = true;   // Wait for fade-out completion before stopping the timer.
    }
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
    if (!busy && !g_app.winAnimating) {
        KillTimer(g_app.hwnd, TM_ANIM);
        g_animTimerPrecision.release();
    }
}

// ---------------------------------------------------------------------------
// Theme selection and application.
// ---------------------------------------------------------------------------
static void SelectTheme(int idx) {
    if (idx < 0 || idx >= (int)g_app.themes.size()) return;
    g_app.selIdx = idx;
    g_app.cfg.theme = g_app.themes[idx].name;
    g_app.cfg.noTheme = false;
    SaveConfig(g_app.cfg);
    ComputeLayout(g_app.L);
    EnsureCardVisible(g_app.selIdx);     // Smoothly scroll lower-row cards into view.
    StartAnim();
    ApplyCurrent(true, true);
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void OpenThemesFolder() {
    std::wstring dir = ThemeDirs().front();
    CreateDirectoryW(dir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Uninstall.
// ---------------------------------------------------------------------------
// Read InstallLocation from the uninstall entry before deleting that registry key.
static std::wstring RegisteredInstallDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     kUninstallKey,
                     L"InstallLocation", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return L"";
    return buf;
}

// Compare directory paths ignoring case and trailing separators.
static bool SameDir(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    auto trim = [](std::wstring s) {
        while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
        return s;
    };
    return _wcsicmp(trim(a).c_str(), trim(b).c_str()) == 0;
}

// Remove the executable's entire directory only in these cases:
// 1. It matches the registered InstallLocation, including a user-selected install path.
// 2. No uninstall entry exists, but it is the default %LOCALAPPDATA%\Programs\Dynamic Wallpapers.
// Portable executables may share Desktop or Downloads with unrelated files.
// If the directory is not a confirmed installation, remove only the executable.
static bool ShouldRemoveInstallDir(const std::wstring& exeDir, const std::wstring& registeredDir) {
    if (exeDir.empty()) return false;
    if (SameDir(exeDir, registeredDir)) return true;
    wchar_t local[MAX_PATH * 2] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) return false;
    return SameDir(exeDir, JoinPath(JoinPath(local, L"Programs"), kAppDisplay));
}

static void DoUninstall() {
    const std::wstring regDir = RegisteredInstallDir();     // Read the uninstall entry before it is removed below.
    const bool removeDir = ShouldRemoveInstallDir(ExeDir(), regDir);
    const bool chinese = InstalledLanguageIsChinese();
    auto tr = [chinese](const wchar_t* english, const wchar_t* zh) { return chinese ? zh : english; };
    const wchar_t* title = tr(L"Dynamic Wallpapers Uninstall", L"卸载 Dynamic Wallpapers");

    // Explain exactly what will be removed; portable installs retain their parent directory.
    const wchar_t* location = removeDir
        ? tr(L"This will remove the application folder.", L"这将删除应用文件夹。")
        : tr(L"This will remove the application file and keep its parent folder.",
             L"这将删除应用文件，但保留其所在文件夹。");
    std::wstring msg = std::wstring(location) + L"\n\n" +
        tr(L"Desktop and Start menu shortcuts, startup entries, settings, and user themes will also be removed."
           L" User data is stored in %APPDATA%\\Dynamic Wallpapers.",
           L"桌面和开始菜单快捷方式、启动项、设置和用户主题也将被删除。"
           L"用户数据位于 %APPDATA%\\Dynamic Wallpapers。");
    if (!InstallDialog(nullptr, title, tr(L"Uninstall Dynamic Wallpapers?", L"确定卸载 Dynamic Wallpapers 吗？"),
                       msg.c_str(), TD_WARNING_ICON, tr(L"Uninstall", L"卸载"), tr(AppText(L"Cancel", L"取消"), L"取消")))
        return;

    // Ask the running instance to quit so its executable can be removed.
    // The helper uses a different window class to avoid matching itself.
    for (int round = 0; round < 3; ++round) {
        bool any = false;
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, kWindowClass, nullptr)) != nullptr) {
            PostMessageW(h, WM_APP + 3, 0, 0);
            any = true;
        }
        // Also ask any legacy process to exit during the migration period.
        while ((h = FindWindowExW(nullptr, h, L"TimeWallMainWindow", nullptr)) != nullptr) {
            PostMessageW(h, WM_APP + 3, 0, 0);
            any = true;
        }
        if (!any) break;
        Sleep(700);
    }
    SetAutoStart(false);
    // Remove shortcuts under both current and legacy names.
    wchar_t appdata[MAX_PATH * 2] = {0};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    for (const wchar_t* nm : { kAppDisplay, kLegacyDisplay }) {
        std::wstring smDir = JoinPath(JoinPath(appdata,
            L"Microsoft\\Windows\\Start Menu\\Programs"), nm);
        DeleteFileW(JoinPath(smDir, std::wstring(nm) + L".lnk").c_str());
        RemoveDirectoryW(smDir.c_str());          // Remove the Start menu folder if it is empty.
    }
    wchar_t desktop[MAX_PATH * 2] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
        DeleteFileW(JoinPath(desktop, std::wstring(kAppDisplay) + L".lnk").c_str());
        DeleteFileW(JoinPath(desktop, std::wstring(kLegacyDisplay) + L".lnk").c_str());
    }
    // Remove current and legacy uninstall registry entries.
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TimeWall");
    // Remove current and legacy settings/user-theme directories.
    std::vector<std::wstring> dirs = { AppDataDir() };
    if (appdata[0]) dirs.push_back(JoinPath(appdata, L"TimeWall"));
    for (const std::wstring& ad : dirs) {
        if (ad.empty()) continue;
        std::wstring cmd = L"cmd.exe /c rmdir /s /q \"" + ad + L"\"";
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    // Show completion before scheduling self-deletion; otherwise the message box keeps
    // the process alive and locks the executable.
    InstallDialog(nullptr, title, tr(L"Uninstall complete", L"卸载完成"),
                  tr(L"The application files will be removed in a few seconds.", L"应用文件将在几秒后移除。"),
                  TD_INFORMATION_ICON, tr(L"OK", L"确定"));

    std::wstring exeDir = ExeDir();
    std::wstring self = ExePath();
    // Delete only the executable unless ShouldRemoveInstallDir confirms the installation directory.
    std::wstring bat = L"cmd.exe /c ping 127.0.0.1 -n 3 > nul & del /f /q \"" + self + L"\"";
    if (removeDir) bat += L" & rmdir /s /q \"" + exeDir + L"\"";
    STARTUPINFOW si2 = {sizeof(si2)};
    si2.dwFlags = STARTF_USESHOWWINDOW;
    si2.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi2 = {};
    if (CreateProcessW(nullptr, &bat[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si2, &pi2)) {
        CloseHandle(pi2.hProcess);
        CloseHandle(pi2.hThread);
    }
}

// ---------------------------------------------------------------------------
// Window procedure.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    // Re-add the tray icon after Explorer restarts.
    if (g_taskbarCreatedMsg && msg == g_taskbarCreatedMsg) {
        g_app.trayAdded = false;
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_NCCALCSIZE:
        // Retain WS_CAPTION behavior but extend the client over the frame on creation and resize.
        // No system caption/border is visible; maximized bounds match the work area.
        return 0;

    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT wr;
        GetWindowRect(h, &wr);
        // Use an 8 DIP resize region on edges/corners for native resizing and Aero Snap.
        int bw = (int)(8 * g_app.gfx.scale());
        bool onL = pt.x < wr.left + bw, onR = pt.x >= wr.right - bw;
        bool onT = pt.y < wr.top + bw,  onB = pt.y >= wr.bottom - bw;
        if (!IsZoomed(h)) {
            if (onT && onL) return HTTOPLEFT;
            if (onT && onR) return HTTOPRIGHT;
            if (onB && onL) return HTBOTTOMLEFT;
            if (onB && onR) return HTBOTTOMRIGHT;
            if (onL) return HTLEFT;
            if (onR) return HTRIGHT;
            if (onT) return HTTOP;
            if (onB) return HTBOTTOM;
        }
        POINT c = pt;
        ScreenToClient(h, &c);
        float s = g_app.gfx.scale();
        float x = c.x / s, y = c.y / s;
        const Layout& L = g_app.L;
        auto in = [&](const D2D1_RECT_F& r) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        };
        if (y < Ui::TitleBarH) {
            // Return HTMAXBUTTON to enable the Windows 11 snap layout flyout.
            if (in(L.captionMax)) return HTMAXBUTTON;
            // Custom minimize/close buttons use client messages.
            if (in(L.captionMin) || in(L.captionClose)) return HTCLIENT;
            return HTCAPTION;        // The remaining caption supports drag, double-click maximize, and edge snapping.
        }
        return HTCLIENT;
    }

    // Maximize hover/press uses nonclient messages from the HTMAXBUTTON region.
    case WM_NCMOUSEMOVE: {
        int id = (w == HTMAXBUTTON) ? CID_MAX : CID_NONE;
        if (id != g_app.hot) {
            g_app.hot = id;
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE | TME_NONCLIENT, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;                       // Delegate native behavior to DefWindowProc.
    }

    case WM_NCMOUSELEAVE:
        if (g_app.hot == CID_MAX) {
            g_app.hot = CID_NONE;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_NCLBUTTONDOWN:
        if (w == HTMAXBUTTON) {
            // Use StartZoom for custom rectangle interpolation on maximize/restore clicks.
            // HTMAXBUTTON is used to retain the native snap layout flyout.
            g_app.pressed = CID_MAX;
            InvalidateRect(h, nullptr, FALSE);
            StartZoom(!g_app.maximized);
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (g_app.pressed == CID_MAX) {
            g_app.pressed = CID_NONE;
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_NCLBUTTONDBLCLK:
        if (w == HTCAPTION) {          // Double-clicking the title bar toggles maximize/restore with animation.
            StartZoom(!g_app.maximized);
            return 0;
        }
        break;

    case WM_GETMINMAXINFO: {
        // Let Windows initialize ptMaxSize/ptMaxPosition first so the taskbar is respected.
        // This custom window needs explicit work-area bounds without extra frame margins.
        // Together with WM_NCCALCSIZE returning zero, outer bounds match the work area,
        // avoiding bright material strips along the top or bottom.
        LRESULT res = DefWindowProcW(h, msg, w, l);
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        UINT dpi = GetDpiForWindow(h);
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(mon, &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        // Minimum width fits three cards; minimum height accommodates all sections.
        int minW = (int)((kPadX * 2 + 3 * (kCardW + 12) + 12) * dpi / 96.0f);
        int minH = (int)((kWinH + 8) * dpi / 96.0f);
        if (mmi->ptMinTrackSize.x < minW) mmi->ptMinTrackSize.x = minW;
        if (mmi->ptMinTrackSize.y < minH) mmi->ptMinTrackSize.y = minH;
        return res;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        ++g_paintCount;                 // TW_ANIMLOG diagnostics.
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        RenderFrame();
        EndPaint(h, &ps);
        return 0;
    }

    case kThumbReadyMessage: {
        const Layout& L = g_app.L;
        for (auto& result : g_thumbLoader.takeResults()) {
            int i = result.request.index;
            if (result.request.generation != g_thumbGeneration || i < 0 ||
                i >= (int)g_app.thumbs.size() || g_app.thumbs[i] ||
                i < L.firstVis - L.perRow || i > L.lastVis + L.perRow) continue;
            g_app.thumbTried[i] = 1;
            if (!result.pixels.empty())
                g_app.thumbs[i] = g_app.gfx.createBitmapFromPixels(
                    result.pixels.data(), result.width, result.height);
        }
        if (IsWindowVisible(h)) InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_SIZE:
        // Do not use minimized icon-placeholder dimensions for layout or graphics buffers.
        if (w == SIZE_MINIMIZED || IsIconic(h)) return 0;
        if (g_app.gfx.dc()) {
            RECT rc;
            GetClientRect(h, &rc);
            UINT dpi = GetDpiForWindow(h);
            g_app.gfx.resize(rc.right - rc.left, rc.bottom - rc.top, dpi);
            // Client size in DIPs, shared by layout and hit testing.
            float sc = dpi / 96.0f;
            g_app.winWDip = (rc.right - rc.left) / sc;
            g_app.winHDip = (rc.bottom - rc.top) / sc;
            // During zoom, use the target state to avoid lagging icons and corners.
            g_app.maximized = g_app.zooming ? g_app.zoomToMax : (IsZoomed(h) != 0);
            // Keep rounded corners during animation, then switch to square corners on maximized completion.
            bool square = g_app.maximized && !g_app.zooming;
            int corner = square ? 1 /*DONOTROUND*/ : 2 /*ROUND*/;
            DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
            if (g_app.bgBrush) {          // Rebuild gradient endpoints for the new height.
                g_app.bgBrush->Release();
                g_app.bgBrush = nullptr;
            }
            ComputeLayout(g_app.L);       // Reflow after every window size change.
            TrackNormRect();              // Remember the normal rectangle for restore animation.
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_MOVE:
        TrackNormRect();                  // Also track moves that do not change size.
        break;

    case WM_DPICHANGED: {
        RECT* r = (RECT*)l;
        UINT dpi = HIWORD(w);
        // Keep the same DIP size across DPI changes; physical dimensions scale accordingly.
        int nw = (int)(g_app.winWDip * dpi / 96.0f + 0.5f);
        int nh = (int)(g_app.winHDip * dpi / 96.0f + 0.5f);
        SetWindowPos(h, nullptr, r->left, r->top, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
        g_app.gfx.resize(nw, nh, dpi);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_SETTINGCHANGE: {
        RefreshPalette();
        CheckExternalWallpaper();
        // React to app-theme changes immediately; the five-second poll is only a fallback.
        if (g_app.cfg.switchMode == 1 && g_app.cfg.autoSwitch) {
            ApplyCurrent(false);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    // Wheel scrolling updates the animation target by 96 DIPs per notch.
    // Scale smaller touchpad/high-resolution deltas proportionally.
    case WM_MOUSEWHEEL: {
        if (AnyDialogOpen()) return 0;              // Do not scroll page content behind a modal dialog.
        const Layout& L = g_app.L;
        if (L.maxScroll <= 0.0f) return 0;          // All content fits; there is nothing to scroll.
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        g_app.scrollTarget -= (float)delta / (float)WHEEL_DELTA * 96.0f;
        ClampScrollTarget();
        MarkScrolled();
        StartAnim();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        float s = g_app.gfx.scale();
        float my = GET_Y_LPARAM(l) / s;
        // Dragging the scrollbar tracks the pointer immediately without easing.
        if (g_app.sbDragging) {
            const Layout& L = g_app.L;
            D2D1_RECT_F th = ScrollThumbRect();
            float travel = L.scrollTrackH - (th.bottom - th.top);
            float t = travel > 0.5f ? (my - g_app.sbDragGrab - L.scrollTrackTop) / travel : 0.0f;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            g_app.scroll = g_app.scrollTarget = t * L.maxScroll;
            MarkScrolled();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        int id = HitTest(GET_X_LPARAM(l) / s, my);
        if (id != g_app.hot) {
            g_app.hot = id;
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_app.hot = CID_NONE;
        g_app.pressed = CID_NONE;
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        float s = g_app.gfx.scale();
        g_app.pressed = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        // Drag the thumb or page by slightly more than half a viewport when clicking the track.
        if (g_app.pressed == CID_SCROLLBAR) {
            D2D1_RECT_F th = ScrollThumbRect();
            g_app.sbDragging = true;
            g_app.sbDragGrab = GET_Y_LPARAM(l) / s - th.top;
            MarkScrolled();
        } else if (g_app.pressed == CID_SCROLLTRACK) {
            float my = GET_Y_LPARAM(l) / s;
            D2D1_RECT_F th = ScrollThumbRect();
            float page = (g_app.winHDip - Ui::TitleBarH) * 0.9f;
            if (my < th.top) g_app.scrollTarget -= page;
            else if (my > th.bottom) g_app.scrollTarget += page;
            ClampScrollTarget();
            MarkScrolled();
            StartAnim();
        }
        // Shrink the appearance indicator on press and restore it on release.
        if (g_app.pressed >= CID_APPEARANCE0 && g_app.pressed <= CID_APPEARANCE2) {
            g_app.segPressed = true;
            StartAnim();
        }
        if (g_app.pressed >= CID_SWITCH0 && g_app.pressed <= CID_SWITCH1) {
            g_app.swPressed = true;
            StartAnim();
        }
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        float s = g_app.gfx.scale();
        int id = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        ReleaseCapture();
        int pressed = g_app.pressed;
        g_app.pressed = CID_NONE;
        if (g_app.sbDragging) {              // Finish dragging the scrollbar.
            g_app.sbDragging = false;
            MarkScrolled();
        }
        if (g_app.segPressed) {              // Restore pressed indicator scale on release.
            g_app.segPressed = false;
            StartAnim();
        }
        if (g_app.swPressed) {
            g_app.swPressed = false;
            StartAnim();
        }
        if (id != pressed || id == CID_NONE) { InvalidateRect(h, nullptr, FALSE); return 0; }

        // Theme control ID = base ID + theme index.
        if (id >= CID_THEME0 && id < CID_THEME0 + kThemeSlots) {
            SelectTheme(id - CID_THEME0);
        } else if (id == CID_DAY_MINUS || id == CID_DAY_PLUS) {
            g_app.cfg.dayStart = (g_app.cfg.dayStart + (id == CID_DAY_PLUS ? 30 : -30) + 1440) % 1440;
            SaveConfig(g_app.cfg);
            if (g_app.cfg.autoSwitch) { CheckExternalWallpaper(); ApplyCurrent(true); }
        } else if (id == CID_NIGHT_MINUS || id == CID_NIGHT_PLUS) {
            g_app.cfg.nightStart = (g_app.cfg.nightStart + (id == CID_NIGHT_PLUS ? 30 : -30) + 1440) % 1440;
            SaveConfig(g_app.cfg);
            if (g_app.cfg.autoSwitch) { CheckExternalWallpaper(); ApplyCurrent(true); }
        } else if (id == CID_AUTO) {
            g_app.cfg.autoSwitch = !g_app.cfg.autoSwitch;
            g_app.toggleTarget[0] = g_app.cfg.autoSwitch ? 1.0f : 0.0f;
            SaveConfig(g_app.cfg);
            if (g_app.cfg.autoSwitch) { CheckExternalWallpaper(); ApplyCurrent(true); }
            StartAnim();
        } else if (id == CID_BOOT) {
            g_app.cfg.startOnBoot = !g_app.cfg.startOnBoot;
            g_app.toggleTarget[1] = g_app.cfg.startOnBoot ? 1.0f : 0.0f;
            SetAutoStart(g_app.cfg.startOnBoot);
            SaveConfig(g_app.cfg);
            StartAnim();
        } else if (id >= CID_THEME_DEL0 && id < CID_THEME_DEL0 + kThemeSlots) {
            OpenDelTheme(id - CID_THEME_DEL0);
        } else if (id == CID_OPENTHEMES) {
            OpenThemesFolder();
        } else if (id == CID_DLG_CANCEL) {
            CloseDelTheme();
        } else if (id == CID_DLG_OK) {
            ConfirmDeleteTheme();
        } else if (id == CID_ADD_THEME) {
            OpenAddTheme();
        } else if (id >= CID_APPEARANCE0 && id <= CID_APPEARANCE2) {
            g_app.cfg.appearance = id - CID_APPEARANCE0;
            g_app.segTarget = g_app.cfg.appearance;
            SaveConfig(g_app.cfg);
            RefreshPalette();
            StartAnim();
        } else if (id >= CID_SWITCH0 && id <= CID_SWITCH1) {
            // Recompute and apply the wallpaper immediately after changing switch mode.
            g_app.cfg.switchMode = id - CID_SWITCH0;
            g_app.swTarget = g_app.cfg.switchMode;
            SaveConfig(g_app.cfg);
            if (g_app.cfg.autoSwitch) { CheckExternalWallpaper(); ApplyCurrent(true); }
            StartAnim();
        } else if (id == CID_OV_CANCEL) {
            CloseAddTheme();
        } else if (id == CID_OV_OK) {
            ConfirmAddTheme();
        } else if (id == CID_OV_LIGHT_SLOT) {
            PickAddThemeImage(true);
        } else if (id == CID_OV_DARK_SLOT) {
            PickAddThemeImage(false);
        } else if (id == CID_OV_NAME) {
            // Clicking the name field sets the caret and clears selection.
            float px = GET_X_LPARAM(l) / g_app.gfx.scale() - (g_app.L.ovName.left + 10);
            EditSetCaretFromX(g_app.gfx, g_app.addUI.name, px);
        } else if (id == CID_REN_NAME) {
            float px = GET_X_LPARAM(l) / g_app.gfx.scale() - (g_app.L.renField.left + 10);
            EditSetCaretFromX(g_app.gfx, g_app.renUI.name, px);
        } else if (id == CID_REN_CANCEL) {
            CloseRenameTheme();
        } else if (id == CID_REN_OK) {
            ConfirmRenameTheme();
        } else if (id >= CID_THEME_REN0 && id < CID_THEME_REN0 + kThemeSlots) {
            OpenRenameTheme(id - CID_THEME_REN0);
        } else if (id == CID_MIN) {
            SendMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        } else if (id == CID_CLOSE) {
            if (g_app.cfg.minimizeToTray) HideMainWindow();
            else DestroyWindow(h);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (w == TM_ANIM) { StepAnim(); return 0; }
        if (w == TM_CARET) {                     // Blink the text editor caret.
            g_app.caretOn = !g_app.caretOn;
            if (!AnyDialogOpen()) KillTimer(h, TM_CARET);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == TM_TICK || w == TM_WALLPAPER) {
            CheckExternalWallpaper();
            ApplyCurrent(false);
            if (IsWindowVisible(h) && !IsIconic(h)) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_APP + 1:      // Another instance requests that this window become visible.
        ShowMainWindow();
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_APP + 4: {    // Handle --set-theme from another instance: reload settings and apply immediately.
        g_app.cfg = LoadConfig();
        g_app.themes = ScanThemes();
        g_app.selIdx = g_app.cfg.noTheme ? -1 : 0;
        for (size_t i = 0; i < g_app.themes.size(); ++i)
            if (g_app.themes[i].name == g_app.cfg.theme) { g_app.selIdx = (int)i; break; }
        ClearThumbs();                    // Invalidate all cached thumbnails after rebuilding the theme list.
        ApplyCurrent(true, true);
        if (g_app.glass) EnsureGlassBitmap(true);
        ShowMainWindow();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case kApplySelectionMessage: // Apply a selection made during a synchronous settings broadcast.
        g_app.selectionQueued = false;
        if (g_app.applyingWallpaper || g_app.readingWallpaper) return 0;
        g_app.selectionDeferred = false;
        ApplyCurrent(true);
        return 0;

    case WM_SYSCOMMAND: {
        const UINT sc = w & 0xFFF0;
        if (sc == SC_MINIMIZE || (sc == SC_RESTORE && IsIconic(h))) {
            // Cancel incomplete fades so native animations do not overlap transparency or hide a restored window.
            const bool repaint = g_app.winAnim < 1.0f;
            g_app.winAnimating = false;
            g_app.hideAfterAnim = false;
            g_app.winAnim = g_app.winAnimTarget = 1.0f;
            if (repaint) {
                InvalidateRect(h, nullptr, FALSE);
                UpdateWindow(h);
            }
            // Leave window rectangles and WINDOWPLACEMENT to Windows for native minimize/restore.
            return DefWindowProcW(h, msg, w, l);
        }
        // Maximize/restore from the system or taskbar menu uses the custom rectangle transition.
        if (sc == SC_MAXIMIZE && !g_app.zooming) {
            StartZoom(true);
            return 0;
        }
        if (sc == SC_RESTORE && !g_app.zooming && !IsIconic(h)) {
            StartZoom(false);
            return 0;
        }
        break;
    }

    case WM_KEYDOWN: {
        // Handle editor keys locally, consuming other keys while a text dialog is open
        // to avoid system beeps or unintended default processing such as Escape.
        TextEdit* ed = ActiveEdit();
        if (ed) {
            if (w == VK_ESCAPE) {
                if (g_app.addUI.active) CloseAddTheme();
                else if (g_app.renUI.active) CloseRenameTheme();
                else if (g_app.delUI.active) CloseDelTheme();
                return 0;
            }
            if (w == VK_RETURN) {                 // Enter confirms directly.
                if (g_app.addUI.active) ConfirmAddTheme();
                else if (g_app.renUI.active) ConfirmRenameTheme();
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (w == 'A')) {   // Ctrl+A selects all.
                EditSelectAll(*ed);
                g_app.caretOn = true;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (w == 'V')) {   // Ctrl+V pastes.
                EditPaste(*ed);
                g_app.caretOn = true;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (w == VK_LEFT || w == VK_RIGHT || w == VK_HOME || w == VK_END || w == VK_BACK ||
                w == VK_DELETE) {
                EditKeyDown(*ed, w, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                g_app.caretOn = true;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        if (w == VK_ESCAPE && AnyDialogOpen()) {  // Escape closes dialogs without a text editor.
            if (g_app.addUI.active) CloseAddTheme();
            if (g_app.delUI.active) CloseDelTheme();
            if (g_app.renUI.active) CloseRenameTheme();
            return 0;
        }
        break;
    }

    case WM_CHAR: {
        // Direct character input; IME composition uses WM_IME_* instead.
        TextEdit* ed = ActiveEdit();
        if (ed) {
            wchar_t c = (wchar_t)w;
            if (c >= 32 && c != 127) {
                EditInsert(*ed, std::wstring(1, c));
                g_app.caretOn = true;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        break;
    }

    // IME composition start, update, and end.
    // Draw preedit text separately and insert committed text into the editor.
    case WM_IME_STARTCOMPOSITION:
        if (ActiveEdit()) {
            ImeFollowEdit();
            return 0;
        }
        break;
    case WM_IME_COMPOSITION:
        if (ActiveEdit()) {
            ImeHandleComposition(l);
            return 0;
        }
        break;
    case WM_IME_ENDCOMPOSITION:
        if (TextEdit* ed = ActiveEdit()) {
            ed->preedit.clear();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_APP + 2:      // Tray icon messages.
        // Left-click toggles the window; right-click opens the tray menu.
        if (l == WM_LBUTTONUP) {
            if (IsWindowVisible(h) && !IsIconic(h)) HideMainWindow();
            else ShowMainWindow();
        } else if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) {
            TrayMenu();
        }
        return 0;

    case WM_APP + 3:      // Installer or uninstaller requests shutdown.
        DestroyWindow(h);
        return 0;

    case WM_DISPLAYCHANGE:
        // The tracker compares only monitors present in both snapshots.
        CheckExternalWallpaper();
        return 0;

    case WM_POWERBROADCAST:
        if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
            CheckExternalWallpaper();
            ApplyCurrent(true);
        }
        return TRUE;

    case WM_CLOSE:
        if (g_app.cfg.minimizeToTray) { HideMainWindow(); return 0; }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        g_thumbLoader.stop();              // Wait for decoding before releasing graphics resources tied to the window.
        KillTimer(h, TM_ANIM);
        g_animTimerPrecision.release();
        RemoveTrayIcon();
        if (g_app.trayIcon) { DestroyIcon(g_app.trayIcon); g_app.trayIcon = nullptr; }
        KillTimer(h, TM_TICK);
        KillTimer(h, TM_WALLPAPER);
        if (g_app.uiLayer) { g_app.uiLayer->Release(); g_app.uiLayer = nullptr; }
        SetAutoStart(g_app.cfg.startOnBoot);
        SaveConfig(g_app.cfg);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------
int wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Per-Monitor V2 DPI awareness for crisp rendering at high scaling factors.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetProcessShutdownParameters(0x3FF, SHUTDOWN_NORETRY);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetCurrentProcessExplicitAppUserModelID(kShellAppId);
    g_appChinese = InstalledLanguageIsChinese();
    g_app.gfx.setTextLocale(g_appChinese);

    // Command-line arguments.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool wantApplyOnly = false, wantShow = false, wantUninstall = false, opaque = false,
         selfGlass = false, wantHidden = false, forceSolid = false;
    int hoverCard = -1;
    std::wstring setTheme, addThemeFolder, addThemeUiFolder, delThemeName;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--apply") wantApplyOnly = true;
        else if (a == L"--show") wantShow = true;
        // The Run registry entry includes --autostart to launch silently in the tray.
        else if (a == L"--autostart" || a == L"--background" || a == L"--tray") wantHidden = true;
        else if (a == L"--uninstall") wantUninstall = true;
        else if (a.rfind(L"--simulate-hour=", 0) == 0)
            g_app.simulateHour = _wtoi(a.substr(16).c_str());
        else if (a.rfind(L"--set-theme=", 0) == 0)
            setTheme = a.substr(12);
        else if (a.rfind(L"--add-theme-ui=", 0) == 0)
            addThemeUiFolder = a.substr(15);      // Test/demo hook: open and prefill the dialog.
        else if (a.rfind(L"--add-theme=", 0) == 0)
            addThemeFolder = a.substr(12);        // Scripted direct theme import.
        else if (a.rfind(L"--delete-theme=", 0) == 0)
            delThemeName = a.substr(15);          // Scripted theme deletion to the Recycle Bin.
        else if (a.rfind(L"--hover-card=", 0) == 0)
            hoverCard = _wtoi(a.substr(13).c_str());   // Test hook: simulate hovering over card N.
        else if (a.rfind(L"--zoom-ms=", 0) == 0)
            g_app.zoomMs = (float)_wtoi(a.substr(10).c_str());  // Test hook: zoom duration.
        else if (a == L"--opaque") opaque = true;    // Disable blur and use an opaque backdrop for diagnostics.
        else if (a == L"--self-glass" || a == L"--no-acrylic")
            selfGlass = true;                          // Force the custom frosted backdrop instead of system material.
        else if (a == L"--acrylic")
            selfGlass = false;                         // Explicitly select system acrylic, the default where supported.
        else if (a == L"--solid")
            forceSolid = true;                         // Force a solid background for diagnostics/testing.
    }
    if (argv) LocalFree(argv);

    // Single instance.
    if (!SingleInstanceAcquire() && !wantApplyOnly && !wantUninstall) {
        // One-shot import/delete actions cannot run through an already-running instance.
        // Exit code 4 tells scripts to close that instance first.
        if (!addThemeFolder.empty() || !delThemeName.empty()) return 4;
        // --set-theme writes settings and asks the running instance to reload and apply immediately.
        if (!setTheme.empty()) {
            Config c = LoadConfig();
            c.theme = setTheme;
            c.noTheme = false;
            SaveConfig(c);
            NotifyExistingInstance(WM_APP + 4);
            return 0;
        }
        NotifyExistingInstance();
        return 0;
    }

    if (wantUninstall) {
        // Use a separate uninstall helper class to avoid confusing FindWindow with the main window.
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInst;
        wc.lpszClassName = L"DynamicWallpapersHelperWindow";
        RegisterClassExW(&wc);
        g_app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr,
                                     nullptr, hInst, nullptr);
        DoUninstall();
        CoUninitialize();
        return 0;
    }

    // Settings and themes.
    g_app.cfg = LoadConfig();
    // The registry is authoritative for startup; an installer may have changed it since config was saved.
    {
        bool regAuto = AutoStartEnabled();
        if (regAuto != g_app.cfg.startOnBoot) {
            g_app.cfg.startOnBoot = regAuto;
            SaveConfig(g_app.cfg);
        }
        // Migrate legacy startup commands to include --autostart.
        if (g_app.cfg.startOnBoot && !AutoStartHasBackgroundFlag()) SetAutoStart(true);
        g_app.toggleTarget[1] = g_app.toggleAnim[1] = regAuto ? 1.0f : 0.0f;
    }
    if (!setTheme.empty()) {
        g_app.cfg.theme = setTheme;
        g_app.cfg.noTheme = false;
        SaveConfig(g_app.cfg);        // Persist a command-line theme selection immediately.
    }
    g_app.themes = ScanThemes();
    if (g_app.cfg.noTheme) g_app.selIdx = -1;
    if (!g_app.cfg.theme.empty()) {
        for (size_t i = 0; i < g_app.themes.size(); ++i)
            if (g_app.themes[i].name == g_app.cfg.theme) { g_app.selIdx = (int)i; break; }
    }
    if (!g_app.cfg.noTheme && !g_app.themes.empty() && g_app.themes[g_app.selIdx].name != g_app.cfg.theme) {
        // If the saved theme disappeared, select the first available theme and save the new selection.
        g_app.cfg.theme = g_app.themes[g_app.selIdx].name;
        SaveConfig(g_app.cfg);
    }
    if (!g_app.cfg.noTheme && !g_app.themes.empty() && g_app.cfg.theme.empty()) {
        g_app.cfg.theme = g_app.themes[0].name;
        SaveConfig(g_app.cfg);
    }

    // --add-theme=<folder> imports images directly for scripts/tests.
    if (!addThemeFolder.empty()) {
        std::wstring name = addThemeFolder;
        size_t sl = name.find_last_of(L"\\/");
        if (sl != std::wstring::npos) name = name.substr(sl + 1);
        while (!name.empty() && (name.back() == L'\\' || name.back() == L'/')) name.pop_back();
        std::vector<std::wstring> imgs = ListImagesIn(addThemeFolder);
        if (!imgs.empty()) {
            std::wstring dayImg, nightImg;
            DiscoverThemeImages(addThemeFolder, name, dayImg, nightImg);
            if (dayImg.empty()) dayImg = imgs.front();
            if (nightImg.empty()) nightImg = imgs.size() > 1 ? imgs.back() : imgs.front();
            std::wstring outName, err;
            // Require two distinct images, one light and one dark.
            if (dayImg == nightImg) {
                g_app.statusMsg = AppText(L"Import failed: choose two different images, one light and one dark.", L"导入失败，请分别为浅色和深色模式选择不同的图片。");
                g_app.statusMsgUntil = GetTickCount64() + 8000;
            } else if (ImportTheme(name, dayImg, nightImg, outName, &err)) {
                g_app.cfg.theme = outName;
                g_app.cfg.noTheme = false;
                SaveConfig(g_app.cfg);
                g_app.themes = ScanThemes();
                for (size_t i = 0; i < g_app.themes.size(); ++i)
                    if (g_app.themes[i].name == outName) g_app.selIdx = (int)i;
                g_app.statusMsg = AppText(L"Theme added: ", L"已添加主题：") + outName;
                g_app.statusMsgUntil = GetTickCount64() + 8000;
            } else {
                g_app.statusMsg = AppText(L"Import failed", L"导入失败") + (err.empty() ? std::wstring(L"") : (L": " + err));
                g_app.statusMsgUntil = GetTickCount64() + 8000;
            }
        }
    }
    if (wantApplyOnly) {                       // Script mode: apply once and exit.
        ApplyCurrent(true);
        CoUninitialize();
        return 0;
    }

    // --delete-theme=<name> moves the theme to the Recycle Bin and exits.
    if (!delThemeName.empty()) {
        const ThemeInfo* t = FindTheme(g_app.themes, delThemeName);
        int rc = 2;                            // 2 = theme not found.
        if (t) {
            std::wstring err;
            rc = DeleteThemeFolder(t->folder, err) ? 0 : 3;
        }
        CoUninitialize();
        return rc;
    }

    // Window creation.
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Use DPI-aware system icon dimensions to avoid blurry upscaling.
    int bigIcon = GetSystemMetrics(SM_CXICON), smallIcon = GetSystemMetrics(SM_CXSMICON);
    if (bigIcon <= 0) bigIcon = 32;
    if (smallIcon <= 0) smallIcon = 16;
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, bigIcon, bigIcon, LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, smallIcon, smallIcon, LR_DEFAULTCOLOR);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    int w = (int)(kWinW * dpi / 96.0f + 0.5f);
    int h = (int)(kWinH * dpi / 96.0f + 0.5f);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_app.hwnd = CreateWindowExW(WS_EX_APPWINDOW
                                     | WS_EX_NOREDIRECTIONBITMAP,  // Required for a transparent acrylic backdrop.
                                 kWindowClass, kAppDisplay,
                                 kMainWindowStyle,
                                 sx, sy, w, h, nullptr, nullptr, hInst, nullptr);
    if (!g_app.hwnd) return 1;

    // Set the window icons before showing it so the taskbar can use the embedded mark.
    if (wc.hIcon) SendMessageW(g_app.hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    if (wc.hIconSm) SendMessageW(g_app.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
    const std::wstring exe = ExePath();
    const std::wstring shellIcon = JoinPath(ExeDir(), kShellIconFile);
    const DWORD iconAttr = GetFileAttributesW(shellIcon.c_str());
    SetShellWindowRelaunch(g_app.hwnd, exe,
                          iconAttr != INVALID_FILE_ATTRIBUTES && !(iconAttr & FILE_ATTRIBUTE_DIRECTORY)
                              ? shellIcon + L",0" : exe + L",-" + std::to_wstring(IDI_TASKBAR_V5),
                          L" --show", IDS_SHELL_TITLE, kShellAppId);

    // Windows 11 rounded corners/shadows and a theme-aware dark title bar.
    int corner = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(g_app.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int backdrop = 2; // DWMSBT_MAINWINDOW (Mica); the app draws its own background for consistency.
    DwmSetWindowAttribute(g_app.hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    RefreshPalette();
    {
        float sc = dpi / 96.0f;
        g_app.winWDip = w / sc;
        g_app.winHDip = h / sc;
    }
    // Choose the backdrop by OS support. All transparent modes require WS_EX_NOREDIRECTIONBITMAP;
    // otherwise the redirection surface is opaque black and hides system materials.
    // Windows 11 22H2+: use DWMWA_SYSTEMBACKDROP_TYPE for live blur behind the window.
    // Windows 10: solid background following the app theme.
    // --self-glass forces custom blur; --solid forces solid fill; --opaque uses an opaque gradient.
    if (!opaque) {
        bool backdrop = false;
        if (!selfGlass && !forceSolid && IsWin11_22H2OrLater()) {
            int backdropAcrylic = 3;            // DWMSBT_TRANSIENTWINDOW (acrylic).
            backdrop = SUCCEEDED(DwmSetWindowAttribute(g_app.hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                                       &backdropAcrylic, sizeof(backdropAcrylic)));
        }
        if (backdrop) {
            g_app.liveGlass = true;
            g_app.glass = false;
            g_app.statusMsg = AppText(L"Background: system acrylic (live blur)", L"背景：系统亚克力（实时模糊）");
        } else if (selfGlass && !forceSolid) {
            g_app.glass = true;
            g_app.liveGlass = false;
            EnsureGlassBitmap(true);
            g_app.statusMsg = AppText(L"Background: custom blur (from wallpaper)", L"背景：自定义壁纸模糊");
        } else {
            g_app.glass = false;
            g_app.liveGlass = false;
            g_app.solidBg = true;
            g_app.statusMsg = AppText(L"Background: solid color (follows app theme)", L"背景：纯色（跟随应用外观）");
        }
        g_app.statusMsgUntil = GetTickCount64() + 5000;
    } else {
        g_app.glass = false;                    // --opaque explicitly requests an opaque backdrop.
        g_app.liveGlass = false;
    }

    // Clear any stale topmost flag left by external tools; normal windows should not stay on top.
    if (GetWindowLongW(g_app.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
        SetWindowPos(g_app.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // Start the appearance indicator at the configured segment without animating at startup.
    g_app.segTarget = g_app.cfg.appearance;
    g_app.segAnim = (float)g_app.cfg.appearance;
    g_app.swTarget = g_app.cfg.switchMode;
    g_app.swAnim = (float)g_app.cfg.switchMode;
    ComputeLayout(g_app.L);
    EnsureCardVisible(g_app.selIdx, true);
    // Detach IME at startup to prevent candidate windows when no editor is active.
    // PrepareTextInput attaches it when a dialog opens; EndTextInput detaches it on close.
    g_app.himc = ImmAssociateContext(g_app.hwnd, nullptr);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    // --autostart keeps switching in the tray without showing a window.
    // The tray icon or --show shortcut opens it; explicit --show overrides background startup.
    const bool showNow = !wantHidden || wantShow;
    if ((showNow || !addThemeUiFolder.empty()) && !EnsureUiInitialized()) return 2;
    g_app.winAnim = showNow ? 0.0f : 1.0f;
    ShowWindow(g_app.hwnd, showNow ? SW_SHOWNORMAL : SW_HIDE);
    UpdateWindow(g_app.hwnd);
    TrackNormRect();                                  // Save the initial restore rectangle.
    SetTimer(g_app.hwnd, TM_TICK, 5000, nullptr);     // Every five seconds: update scheduled wallpaper and resample the frosted backdrop.

    // Fade in on the first visible launch; skip for background startup.
    if (showNow) StartWindowAnim(true);

    // --add-theme-ui=<folder> opens the Add theme dialog for tests/demos,
    // prefilling the first two images to avoid manual file picking.
    if (!addThemeUiFolder.empty()) {
        std::vector<std::wstring> imgs = ListImagesIn(addThemeUiFolder);
        OpenAddTheme();
        if (!imgs.empty()) {
            ID2D1Bitmap* b = g_app.gfx.loadImage(imgs[0], 640);
            if (b) {
                g_app.addUI.lightBmp = b;
                g_app.addUI.lightPath = imgs[0];
            }
        }
        if (imgs.size() > 1) {
            ID2D1Bitmap* b = g_app.gfx.loadImage(imgs[1], 640);
            if (b) {
                g_app.addUI.darkBmp = b;
                g_app.addUI.darkPath = imgs[1];
            }
        }
        std::wstring nm = addThemeUiFolder;
        size_t sl = nm.find_last_of(L"\\/");
        if (sl != std::wstring::npos) nm = nm.substr(sl + 1);
        while (!nm.empty() && (nm.back() == L'\\' || nm.back() == L'/')) nm.pop_back();
        g_app.addUI.name.text = nm;
        g_app.addUI.name.caret = (int)nm.size();
        g_app.addUI.name.clearSel();
    }

    // --hover-card=N shows card N's action buttons for screenshots/tests.
    // Scroll lower rows into view first.
    if (hoverCard >= 0 && hoverCard < (int)g_app.themes.size()) {
        g_app.hot = CID_THEME0 + hoverCard;
        EnsureCardVisible(hoverCard, true);
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
    }

    // Apply the appropriate wallpaper once at startup.
    ApplyCurrent(true);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RemoveTrayIcon();
    g_app.gfx.shutdown();
    ClearThumbs();
    CoUninitialize();
    return 0;
}
