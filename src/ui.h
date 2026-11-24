// ============================================================================
// ui.h - WinUI 3 style tokens and control drawing.
//
// Custom D2D controls without Windows App SDK dependencies: rounded corners (4/8),
// translucent layers, accent colors, Segoe fonts, and grayscale text. Shared by app and installer.
// Colors and dimensions are centralized in MakePalette() and the constants below.
// ============================================================================
#pragma once
#include "gfx.h"
#include <string>

// A null family selects system UI typography in Gfx.
#define FONT_UI      nullptr
#define FONT_ICON    L"Segoe MDL2 Assets"

// Dimensions in DIPs.
namespace Ui {
    const float RadiusCard    = 8.0f;
    const float RadiusControl = 4.0f;
    const float ToggleW       = 40.0f;
    const float ToggleH       = 20.0f;
    const float ToggleKnob    = 12.0f;
    const float ButtonH       = 34.0f;
    const float TitleBarH     = 40.0f;
}

// Approximate WinUI 3 palette tokens in sRGB plus alpha, independent of DPI.
struct Palette {
    bool dark = false;
    D2D1_COLOR_F windowBgTop, windowBgBottom;   // Mica-style window gradient.
    D2D1_COLOR_F textPrimary, textSecondary, textTertiary, textOnAccent, textDisabled;
    D2D1_COLOR_F cardFill, cardFillHover, cardStroke, cardStrokeSelected;
    D2D1_COLOR_F ctrlFill, ctrlFillHover, ctrlFillPressed, ctrlStroke, ctrlStrokeHover;
    D2D1_COLOR_F accent, accentHover, accentPressed, accentDisabled;
    D2D1_COLOR_F toggleOffFill, toggleOffFillHover, toggleOffKnob;
    D2D1_COLOR_F divider, closeHover, captionHover;
    D2D1_COLOR_F shadowTint;
    D2D1_COLOR_F nightDot, dayDot;      // Status dots: blue-purple for night, golden for day.
    D2D1_COLOR_F accentText;            // Accent text is brightened in dark mode to maintain contrast.
};

// Build the palette from light/dark mode and the system accent color.
Palette MakePalette(bool dark, const D2D1_COLOR_F& accent);

// Read the system app theme and accent color.
bool SystemUsesDarkMode();
D2D1_COLOR_F SystemAccentColor();

// Color helpers.
D2D1_COLOR_F Col(float r, float g, float b, float a = 1.0f);
D2D1_COLOR_F ColHex(unsigned rgb, float a = 1.0f);        // 0xRRGGBB
D2D1_COLOR_F ColAlpha(const D2D1_COLOR_F& c, float a);    // Change alpha.
D2D1_COLOR_F ColBlend(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t);
D2D1_COLOR_F ColLighten(const D2D1_COLOR_F& c, float t);  // Blend toward white.

// Controls use DIP rectangles; callers supply hover/pressed states from hit testing.
void UiSectionTitle(Gfx& g, float x, float y, float w, const std::wstring& title,
                    const Palette& p, bool divider = true);
void UiButton(Gfx& g, const D2D1_RECT_F& r, const std::wstring& label, const Palette& p,
              bool primary, bool hovered, bool pressed, bool enabled = true,
              const wchar_t* iconGlyph = nullptr);
// Caption button actions: minimize, maximize, restore, close.
namespace Ui {
    enum CaptionBtn { CAP_MIN = 0, CAP_CLOSE = 1, CAP_MAX = 2, CAP_RESTORE = 3 };
}
void UiCaptionButton(Gfx& g, const D2D1_RECT_F& r, int mode, bool hovered, bool pressed,
                     const Palette& p);
// animT interpolates the track, knob position, and knob color: 0=off, 1=on.
void UiToggle(Gfx& g, float x, float y, bool on, float animT, bool hovered, const Palette& p);
void UiCheckbox(Gfx& g, float x, float y, const std::wstring& label, bool checked,
                bool hovered, const Palette& p);
void UiStepper(Gfx& g, const D2D1_RECT_F& r, const std::wstring& value, const Palette& p,
               int hoverPart, int pressPart);      // hoverPart: -1=none, 0=decrease, 1=increase.
void UiThemeCard(Gfx& g, const D2D1_RECT_F& r, ID2D1Bitmap* thumb, const std::wstring& name,
                 const std::wstring& sub, bool selected, bool hovered, const Palette& p,
                 bool showDelete = false, bool delHovered = false, bool renHovered = false);
// Delete/rename hit rectangles share geometry with UiThemeCard rendering.
D2D1_RECT_F UiThemeCardDeleteRect(const D2D1_RECT_F& card);
D2D1_RECT_F UiThemeCardRenameRect(const D2D1_RECT_F& card);
void UiCard(Gfx& g, const D2D1_RECT_F& r, const Palette& p, bool hovered = false,
            bool selected = false);
void UiProgress(Gfx& g, const D2D1_RECT_F& r, float progress, const Palette& p);
void UiStatusDot(Gfx& g, float cx, float cy, bool night, const Palette& p);
// Secondary accent text action such as Add theme, with an optional leading icon.
void UiTextButton(Gfx& g, const D2D1_RECT_F& r, const std::wstring& label, const Palette& p,
                  bool hovered, bool pressed, const wchar_t* glyph = nullptr,
                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_TRAILING);
// Equal-width segments; fill the selected segment with the accent color.
// anim: floating-point indicator position in 0..items-1; selected when stationary.
// pressScale: scale around the indicator center, 1.0 at rest.
void UiSegmented(Gfx& g, const D2D1_RECT_F& r, const std::vector<std::wstring>& items,
                 int selected, int hotSeg, const Palette& p, float anim = -1.0f,
                 float pressScale = 1.0f, const wchar_t* const* locales = nullptr);
void UiComboBox(Gfx& g, const D2D1_RECT_F& r, const std::wstring& value, const Palette& p,
                bool hovered, bool pressed, bool focused, bool open);
void UiComboPopup(Gfx& g, const D2D1_RECT_F& r, const Palette& p);
void UiComboItem(Gfx& g, const D2D1_RECT_F& r, const std::wstring& label, const Palette& p,
                 bool selected, bool highlighted);
// Dim the entire window behind a modal dialog.
void UiDim(Gfx& g, float w, float h, const Palette& p);
