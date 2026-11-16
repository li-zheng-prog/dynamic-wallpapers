// ============================================================================
// gfx.h - Rendering with D3D11, DirectComposition, Direct2D, DirectWrite, and WIC.
//
// Rendering only; application logic lives elsewhere.
// The DComp swap chain supports premultiplied per-pixel alpha for rounded windows,
// translucent layers, and system acrylic backgrounds.
// Coordinates use DIPs (logical pixels at 96 DPI). resize() sets the window DPI,
// and D2D scales automatically for sharp text and consistent control sizes.
// ============================================================================
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <d3d11.h>
#include <dcomp.h>
#include <string>
#include <vector>

class Gfx {
public:
    // Create the D3D11 device, D2D context, swap chain, and DComp visual tree.
    // Call after window creation and before the first frame.
    bool init(HWND hwnd);

    // Handle size and DPI changes; physicalW/H are client dimensions in physical pixels.
    void resize(UINT physicalW, UINT physicalH, UINT dpi);

    void shutdown();

    // Frame sequence: begin(), draw with dc(), end().
    bool begin();
    void end();

    // Present sync interval: 1=wait for refresh (normal use, less power, no tearing).
    // 0=do not wait; DwmFlush paces zoom animations to avoid an extra frame of latency.
    int presentSync = 1;

    ID2D1DeviceContext* dc() const { return dc_; }
    float scale() const { return dpi_ / 96.0f; }

    // Cache brushes and text formats instead of recreating them each frame.
    ID2D1SolidColorBrush* brush(const D2D1_COLOR_F& color);
    // Try comma-separated font families in order.
    // Control text uses NEAR paragraph alignment and textOptical for ink-based centering.
    IDWriteTextFormat* format(const wchar_t* families, float dipSize,
                              DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                              bool wrap = false);
    // Decode JPG/PNG/BMP; use WIC scaling when maxWidthPx>0 to save memory.
    ID2D1Bitmap* loadImage(const std::wstring& path, UINT maxWidthPx);
    // Decode executable RCDATA resources so the logo needs no external file.
    ID2D1Bitmap* loadImageFromResource(int resId, UINT maxWidthPx);
    // Create a premultiplied B8G8R8A8 bitmap, used for frosted-background noise.
    ID2D1Bitmap* createBitmapFromPixels(const void* premultipliedBGRA, UINT w, UINT h);

    // Drawing helpers check for null resources and fail silently.
    void fillRound(const D2D1_ROUNDED_RECT& r, const D2D1_COLOR_F& c);
    void strokeRound(const D2D1_ROUNDED_RECT& r, const D2D1_COLOR_F& c, float w = 1.0f);
    void text(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
              const D2D1_COLOR_F& c);
    // Center actual glyph ink instead of the line box; font descender space can otherwise
    // make text appear too high. Used for labels inside controls.
    void textOptical(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                     const D2D1_COLOR_F& c,
                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    // Measure ink width for positioning adjacent text blocks.
    float textWidth(const std::wstring& s, IDWriteTextFormat* fmt);
    // Clipping for rounded images and similar shapes.
    void pushClip(const D2D1_ROUNDED_RECT& r);
    void popClip();

private:
    bool createTargetFromSwapChain();

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    UINT w_ = 0, h_ = 0;            // Current window size in physical pixels: the visible buffer area.
    UINT bufW_ = 0, bufH_ = 0;      // Monitor-sized swap-chain/target buffers, reused throughout animations.

    ID3D11Device* d3d_ = nullptr;
    IDXGISwapChain1* swap_ = nullptr;
    ID2D1Factory1* factory_ = nullptr;
    ID2D1Device* d2dDevice_ = nullptr;
    ID2D1DeviceContext* dc_ = nullptr;
    ID2D1Bitmap1* target_ = nullptr;
    IDCompositionDevice* dcomp_ = nullptr;
    IDCompositionTarget* dcompTarget_ = nullptr;
    IDCompositionVisual* visual_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    IWICImagingFactory* wic_ = nullptr;

    std::vector<std::pair<D2D1_COLOR_F, ID2D1SolidColorBrush*>> brushes_;
    struct FmtKey { std::wstring fam; float size; int weight; int align; bool wrap; };
    std::vector<std::pair<FmtKey, IDWriteTextFormat*>> formats_;
};
