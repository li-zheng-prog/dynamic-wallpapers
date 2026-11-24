// ============================================================================
// gfx.cpp - Graphics implementation.
// ============================================================================
#include "gfx.h"
#include <wincodec.h>
#include <d2d1helper.h>
#include <algorithm>

// Compatibility definitions for DWM constants missing from older MinGW SDK headers.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Declare the DComp IID not exported by MinGW headers.
static const GUID kIID_IDCompositionDevice =
    {0xc37ea93a, 0xe7aa, 0x450d, {0xb1, 0x6f, 0x97, 0x46, 0xcb, 0x04, 0x07, 0xf3}};

static bool ColorEq(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static bool FontExists(IDWriteFontCollection* fonts, const std::wstring& name) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    return fonts && !name.empty() && SUCCEEDED(fonts->FindFamilyName(name.c_str(), &index, &exists)) && exists;
}

void Gfx::resolveUiFonts() {
    activeChineseFont_ = FontExists(fonts_, chineseFont_) ? chineseFont_ : L"";
    activeEnglishFont_ = FontExists(fonts_, englishFont_) ? englishFont_ : L"";
}

void Gfx::setUiFonts(const std::wstring& chinese, const std::wstring& english) {
    chineseFont_ = chinese;
    englishFont_ = english;
    resolveUiFonts();
}

void Gfx::refreshFonts() {
    if (!dwrite_) return;
    IDWriteFontCollection* updated = nullptr;
    if (FAILED(dwrite_->GetSystemFontCollection(&updated, TRUE))) return;
    for (auto& f : formats_) f.second->Release();
    formats_.clear();
    if (ellipsis_) { ellipsis_->Release(); ellipsis_ = nullptr; }
    if (fonts_) fonts_->Release();
    fonts_ = updated;
    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    systemUiFont_ = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) &&
                    FontExists(fonts_, metrics.lfMessageFont.lfFaceName)
                    ? metrics.lfMessageFont.lfFaceName : L"Segoe UI";
    resolveUiFonts();
}

static std::wstring FamilyName(IDWriteLocalizedStrings* names, const wchar_t* locale, UINT32 fallback) {
    UINT32 index = 0, length = 0;
    BOOL found = FALSE;
    if (FAILED(names->FindLocaleName(locale, &index, &found)) || !found) index = fallback;
    if (FAILED(names->GetStringLength(index, &length))) return {};
    std::wstring result(length + 1, L'\0');
    if (FAILED(names->GetString(index, result.data(), length + 1))) return {};
    result.resize(length);
    return result;
}

std::vector<FontFamilyOption> Gfx::installedFonts() const {
    std::vector<FontFamilyOption> result;
    if (!fonts_) return result;
    const wchar_t* locale = chineseText_ ? L"zh-CN" : L"en-US";
    for (UINT32 i = 0; i < fonts_->GetFontFamilyCount(); ++i) {
        IDWriteFontFamily* family = nullptr;
        if (FAILED(fonts_->GetFontFamily(i, &family))) continue;
        IDWriteLocalizedStrings* names = nullptr;
        if (SUCCEEDED(family->GetFamilyNames(&names))) {
            auto name = FamilyName(names, L"en-US", 0);
            UINT32 english = 0;
            BOOL found = FALSE;
            if (FAILED(names->FindLocaleName(L"en-US", &english, &found)) || !found) english = 0;
            auto label = FamilyName(names, locale, english);
            if (!name.empty()) result.push_back({name, label.empty() ? name : label});
            names->Release();
        }
        family->Release();
    }
    std::stable_sort(result.begin(), result.end(), [locale](const auto& a, const auto& b) {
        return CompareStringEx(locale, NORM_IGNORECASE, a.label.c_str(), -1, b.label.c_str(), -1,
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return result;
}

bool Gfx::init(HWND hwnd) {
    hwnd_ = hwnd;
    dpi_ = GetDpiForWindow(hwnd);
    if (!dpi_) dpi_ = 96;

    // On failure, release local DXGI objects and initialized members through bail.
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* dxgiFactory = nullptr;
    auto bail = [&]() {
        if (dxgiFactory) dxgiFactory->Release();
        if (adapter) adapter->Release();
        if (dxgiDevice) dxgiDevice->Release();
        shutdown();
        return false;
    };

    // D3D11 device; BGRA support is required for Direct2D interoperability.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION, &d3d_, &level, nullptr)))
        return bail();

    if (FAILED(d3d_->QueryInterface(IID_IDXGIDevice, (void**)&dxgiDevice))) return bail();

    // ---- D2D / DWrite / WIC ----
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 IID_ID2D1Factory1, nullptr, (void**)&factory_)))
        return bail();
    if (FAILED(factory_->CreateDevice(dxgiDevice, &d2dDevice_)))
        return bail();
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_)))
        return bail();
    // Use grayscale antialiasing to avoid ClearType color fringes on transparent backgrounds.
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc_->SetDpi((float)dpi_, (float)dpi_);

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)&dwrite_)))
        return bail();
    refreshFonts();
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     __uuidof(IWICImagingFactory), (void**)&wic_);

    // Composition swap chain with premultiplied alpha.
    if (FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter) return bail();
    if (FAILED(adapter->GetParent(IID_IDXGIFactory2, (void**)&dxgiFactory)) || !dxgiFactory)
        return bail();

    RECT rc; GetClientRect(hwnd_, &rc);
    w_ = (UINT)(rc.right - rc.left);
    h_ = (UINT)(rc.bottom - rc.top);
    if (w_ == 0) w_ = 1;
    if (h_ == 0) h_ = 1;

    // Allocate a monitor-sized buffer once and change only the visible area during animations.
    // Resizing buffers and rebuilding the target each frame costs 4-13 ms, too much at 120 Hz.
    // DWM clips the oversized buffer to the window bounds.
    bufW_ = w_; bufH_ = h_;
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
        UINT monW = (UINT)(mi.rcMonitor.right - mi.rcMonitor.left);
        UINT monH = (UINT)(mi.rcMonitor.bottom - mi.rcMonitor.top);
        if (monW > bufW_) bufW_ = monW;
        if (monH > bufH_) bufH_ = monH;
    }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = bufW_;
    sd.Height = bufH_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(dxgiFactory->CreateSwapChainForComposition(d3d_, &sd, nullptr, &swap_)))
        return bail();

    // Attach the swap chain to the window through the DComp visual tree.
    if (FAILED(DCompositionCreateDevice(dxgiDevice, kIID_IDCompositionDevice, (void**)&dcomp_)))
        return bail();
    if (FAILED(dcomp_->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget_))) return bail();
    if (FAILED(dcomp_->CreateVisual(&visual_))) return bail();
    visual_->SetContent(swap_);
    dcompTarget_->SetRoot(visual_);
    dcomp_->Commit();

    if (!createTargetFromSwapChain()) return bail();

    dxgiFactory->Release();
    adapter->Release();
    dxgiDevice->Release();
    return true;
}

bool Gfx::createTargetFromSwapChain() {
    IDXGISurface* surface = nullptr;
    if (FAILED(swap_->GetBuffer(0, IID_IDXGISurface, (void**)&surface))) return false;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        (float)dpi_, (float)dpi_);
    HRESULT hr = dc_->CreateBitmapFromDxgiSurface(surface, &bp, &target_);
    surface->Release();
    if (FAILED(hr)) return false;
    dc_->SetTarget(target_);
    return true;
}

void Gfx::resize(UINT physicalW, UINT physicalH, UINT dpi) {
    if (!swap_) return;
    if (physicalW == 0) physicalW = 1;
    if (physicalH == 0) physicalH = 1;
    if (physicalW == w_ && physicalH == h_ && dpi == dpi_) return;

    bool dpiChanged = (dpi != dpi_);
    w_ = physicalW; h_ = physicalH;
    dpi_ = dpi;
    bool growBuffer = w_ > bufW_ || h_ > bufH_;

    // Normally, update only the window dimensions: its visible area starts at the buffer origin.
    // Reallocate only when the window exceeds the buffer, for example after changing monitors.
    // DPI changes update coordinate mapping without reallocating GPU memory.
    if (growBuffer || dpiChanged) {
        if (target_) { dc_->SetTarget(nullptr); target_->Release(); target_ = nullptr; }
    }
    if (growBuffer) {
        UINT nw = (w_ > bufW_) ? w_ : bufW_;
        UINT nh = (h_ > bufH_) ? h_ : bufH_;
        if (SUCCEEDED(swap_->ResizeBuffers(0, nw, nh, DXGI_FORMAT_UNKNOWN, 0))) {
            bufW_ = nw; bufH_ = nh;
        }
        // Recreate the target even if ResizeBuffers fails, using the old buffer if necessary.
        // A temporarily mismatched size is preferable to leaving begin() without a target.
    }
    if (growBuffer || dpiChanged) {
        // Target DPI is set at creation; rebuild the view while reusing swap-chain pixels.
        createTargetFromSwapChain();
        dc_->SetDpi((float)dpi_, (float)dpi_);
    }
}

void Gfx::shutdown() {
    for (auto& b : brushes_) if (b.second) b.second->Release();
    brushes_.clear();
    for (auto& f : formats_) if (f.second) f.second->Release();
    formats_.clear();
    if (target_) { target_->Release(); target_ = nullptr; }
    if (visual_) { visual_->Release(); visual_ = nullptr; }
    if (dcompTarget_) { dcompTarget_->Release(); dcompTarget_ = nullptr; }
    if (dcomp_) { dcomp_->Release(); dcomp_ = nullptr; }
    if (wic_) { wic_->Release(); wic_ = nullptr; }
    if (ellipsis_) { ellipsis_->Release(); ellipsis_ = nullptr; }
    if (fonts_) { fonts_->Release(); fonts_ = nullptr; }
    if (dwrite_) { dwrite_->Release(); dwrite_ = nullptr; }
    if (dc_) { dc_->Release(); dc_ = nullptr; }
    if (d2dDevice_) { d2dDevice_->Release(); d2dDevice_ = nullptr; }
    if (factory_) { factory_->Release(); factory_ = nullptr; }
    if (swap_) { swap_->Release(); swap_ = nullptr; }
    if (d3d_) { d3d_->Release(); d3d_ = nullptr; }
}

bool Gfx::begin() {
    if (!dc_ || !target_) return false;
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void Gfx::end() {
    HRESULT hr = dc_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // Recreate the target after device loss.
        if (target_) { dc_->SetTarget(nullptr); target_->Release(); target_ = nullptr; }
        createTargetFromSwapChain();
        return;
    }
    DXGI_PRESENT_PARAMETERS pp = {};
    swap_->Present1(presentSync, 0, &pp);
}

ID2D1SolidColorBrush* Gfx::brush(const D2D1_COLOR_F& color) {
    for (auto& b : brushes_)
        if (ColorEq(b.first, color)) return b.second;
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(dc_->CreateSolidColorBrush(color, &b))) {
        brushes_.push_back({color, b});
        if (brushes_.size() > 64) {   // Bound the cache to prevent unlimited growth.
            brushes_.front().second->Release();
            brushes_.erase(brushes_.begin());
        }
    }
    return b;
}

IDWriteTextFormat* Gfx::format(const wchar_t* families, float dipSize,
                               DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                               bool wrap, const wchar_t* locale) {
    if (!dwrite_) return nullptr;
    if (!locale) locale = chineseText_ ? L"zh-CN" : L"en-US";
    FmtKey key{families ? families : L"", locale, dipSize, (int)weight, (int)align, wrap};
    for (auto& f : formats_)
        if (f.first.fam == key.fam && f.first.locale == key.locale &&
            f.first.size == key.size && f.first.weight == key.weight &&
            f.first.align == key.align && f.first.wrap == key.wrap)
            return f.second;

    // CreateTextFormat can succeed for a missing family; check the collection before choosing.
    const std::wstring list = families ? families : systemUiFont_;
    IDWriteTextFormat* fmt = nullptr;
    size_t start = 0;
    auto create = [&](const wchar_t* family) {
        return dwrite_->CreateTextFormat(family, fonts_, weight, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, dipSize, locale, &fmt);
    };
    while (true) {
        size_t comma = list.find(L',', start);
        std::wstring one = list.substr(start, comma == std::wstring::npos ? comma : comma - start);
        if (FontExists(fonts_, one) && SUCCEEDED(create(one.c_str())))
            break;
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    if (!fmt) create(L"Segoe UI");
    if (!fmt) return nullptr;
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    // Use the Windows 11 default line-spacing behavior.
    fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_DEFAULT, 0, 0);
    formats_.push_back({key, fmt});
    return fmt;
}

// Convert an open WIC decoder to a D2D bitmap; shared by file and resource loading.
static ID2D1Bitmap* BitmapFromDecoder(IWICImagingFactory* wic, ID2D1DeviceContext* dc,
                                       IWICBitmapDecoder* dec, UINT maxWidthPx) {
    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(dec->GetFrame(0, &frame))) return nullptr;

    IWICBitmapSource* src = frame;
    IWICBitmapScaler* scaler = nullptr;
    if (maxWidthPx > 0) {
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        if (w > maxWidthPx) {
            if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(frame, maxWidthPx, maxWidthPx * h / w,
                                             WICBitmapInterpolationModeFant)))
                src = scaler;
        }
    }
    IWICFormatConverter* conv = nullptr;
    ID2D1Bitmap* bmp = nullptr;
    if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
        dc->CreateBitmapFromWicBitmap(conv, nullptr, &bmp);
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    frame->Release();
    return bmp;
}

ID2D1Bitmap* Gfx::loadImage(const std::wstring& path, UINT maxWidthPx) {
    if (!wic_ || !dc_) return nullptr;
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnDemand, &dec)))
        return nullptr;
    ID2D1Bitmap* bmp = BitmapFromDecoder(wic_, dc_, dec, maxWidthPx);
    dec->Release();
    return bmp;
}

// Decode the title-bar logo from RCDATA using an HGLOBAL stream passed to WIC.
// No temporary files or external image dependencies are needed.
ID2D1Bitmap* Gfx::loadImageFromResource(int resId, UINT maxWidthPx) {
    if (!wic_ || !dc_) return nullptr;
    HRSRC res = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!res) return nullptr;
    DWORD size = SizeofResource(nullptr, res);
    HGLOBAL hGlb = LoadResource(nullptr, res);
    void* data = hGlb ? LockResource(hGlb) : nullptr;
    if (!data || !size) return nullptr;

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return nullptr;
    ULONG written = 0;
    if (FAILED(stream->Write(data, size, &written))) { stream->Release(); return nullptr; }
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(wic_->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) {
        stream->Release();
        return nullptr;
    }
    ID2D1Bitmap* bmp = BitmapFromDecoder(wic_, dc_, dec, maxWidthPx);
    dec->Release();
    stream->Release();
    return bmp;
}

void Gfx::fillRound(const D2D1_ROUNDED_RECT& r, const D2D1_COLOR_F& c) {
    ID2D1SolidColorBrush* b = brush(c);
    if (b) dc_->FillRoundedRectangle(r, b);
}

// Create a bitmap from premultiplied B8G8R8A8 pixels for frosted-background noise.
ID2D1Bitmap* Gfx::createBitmapFromPixels(const void* pixels, UINT w, UINT h) {
    if (!dc_ || !pixels || !w || !h) return nullptr;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ID2D1Bitmap1* bmp = nullptr;
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w, h), pixels, w * 4, props, &bmp))) return nullptr;
    return bmp;
}

void Gfx::strokeRound(const D2D1_ROUNDED_RECT& r, const D2D1_COLOR_F& c, float w) {
    ID2D1SolidColorBrush* b = brush(c);
    if (b) dc_->DrawRoundedRectangle(r, b, w);
}

// Punctuation used specifically with Chinese belongs to the same run as Han text.
static int UiScript(UINT32 c) {
    if ((c >= 0x2E80 && c <= 0x2FDF) || (c >= 0x3000 && c <= 0x303F) ||
        (c >= 0x31C0 && c <= 0x31EF) || (c >= 0x3400 && c <= 0x4DBF) ||
        (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE10 && c <= 0xFE4F) || (c >= 0xFF01 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x20000 && c <= 0x2FA1F) ||
        (c >= 0x30000 && c <= 0x323AF)) return 1;
    if ((c >= 0x20 && c <= 0x7E) || (c >= 0xC0 && c <= 0x24F) ||
        (c >= 0x1E00 && c <= 0x1EFF)) return 2;
    return 0;
}

IDWriteTextLayout* Gfx::textLayout(const std::wstring& s, IDWriteTextFormat* fmt, float w, float h) {
    IDWriteTextLayout* layout = nullptr;
    if (!dwrite_ || !fmt || FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, w, h, &layout)))
        return nullptr;
    const auto f = std::find_if(formats_.begin(), formats_.end(), [fmt](const auto& item) { return item.second == fmt; });
    if (f == formats_.end() || !f->first.fam.empty()) return layout;

    UINT32 start = 0;
    int script = 0;
    auto setRun = [&](UINT32 end) {
        if (end == start || script == 0) return;
        DWRITE_TEXT_RANGE range = {start, end - start};
        const auto& font = script == 1 ? activeChineseFont_ : activeEnglishFont_;
        if (!font.empty()) layout->SetFontFamilyName(font.c_str(), range);
        if (script == 1) layout->SetLocaleName(L"zh-CN", range);
    };
    for (UINT32 i = 0; i < s.size();) {
        const UINT32 at = i;
        UINT32 c = s[i++];
        if (c >= 0xD800 && c <= 0xDBFF && i < s.size() && s[i] >= 0xDC00 && s[i] <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i++] - 0xDC00);
        // Keep combining marks and variation selectors with their base glyph.
        const int next = ((c >= 0x300 && c <= 0x36F) || (c >= 0xFE00 && c <= 0xFE0F) ||
                          (c >= 0xE0100 && c <= 0xE01EF)) ? script : UiScript(c);
        if (next != script) { setRun(at); start = at; script = next; }
    }
    setRun((UINT32)s.size());
    return layout;
}

void Gfx::text(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
               const D2D1_COLOR_F& c) {
    if (!fmt || s.empty()) return;
    ID2D1SolidColorBrush* b = brush(c);
    if (!b || r.right <= r.left || r.bottom <= r.top) return;
    if (auto* layout = textLayout(s, fmt, r.right - r.left, r.bottom - r.top)) {
        dc_->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        layout->Release();
    }
}

float Gfx::textWidth(const std::wstring& s, IDWriteTextFormat* fmt) {
    if (!fmt || s.empty()) return 0;
    auto* layout = textLayout(s, fmt, 4000.0f, 100.0f);
    if (!layout) return 0;
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TEXT_METRICS tm = {};
    layout->GetMetrics(&tm);
    layout->Release();
    return tm.widthIncludingTrailingWhitespace;
}

// Center the glyph ink vertically: overhang is relative to the layout box, not text height.
//   inkTop    = -om.top
// inkBottom = box height + om.bottom; a negative bottom overhang lies inside the box.
// Adding tm.height instead can produce a large negative bound and move text offscreen.
void Gfx::textOptical(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                      const D2D1_COLOR_F& c, DWRITE_TEXT_ALIGNMENT align, bool ellipsis) {
    if (!fmt || s.empty()) return;
    ID2D1SolidColorBrush* b = brush(c);
    if (!b) return;
    float boxW = r.right - r.left;
    if (boxW <= 0) return;
    const float kBoxH = 1000.0f;
    auto* layout = textLayout(s, fmt, boxW, kBoxH);
    if (!layout) return;
    if (ellipsis) {
        if (!ellipsis_) dwrite_->CreateEllipsisTrimmingSign(fmt, &ellipsis_);
        const DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis_);
    }
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TEXT_METRICS tm = {};
    DWRITE_OVERHANG_METRICS om = {};
    layout->GetMetrics(&tm);
    layout->GetOverhangMetrics(&om);
    float inkTop = -om.top;
    float inkBottom = kBoxH + om.bottom;
    float x = r.left;
    float textW = tm.widthIncludingTrailingWhitespace;
    if (align == DWRITE_TEXT_ALIGNMENT_CENTER)        x = r.left + (boxW - textW) / 2.0f;
    else if (align == DWRITE_TEXT_ALIGNMENT_TRAILING) x = r.right - textW;
    // Align the ink center with the control center.
    float y = (r.top + r.bottom) / 2.0f - (inkTop + inkBottom) / 2.0f;
    dc_->DrawTextLayout(D2D1::Point2F(x, y), layout, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

void Gfx::pushClip(const D2D1_ROUNDED_RECT& r) {
    dc_->PushAxisAlignedClip(r.rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

void Gfx::popClip() { dc_->PopAxisAlignedClip(); }
