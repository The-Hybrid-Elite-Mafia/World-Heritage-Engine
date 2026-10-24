// Gfx — the program's drawing layer, built only on Windows' own APIs:
// Direct2D (GPU drawing), WIC (PNG decoding and high-quality resampling) and
// DirectWrite (text). No frameworks.
//
// Art handling (the GPU budget rule): every rendered PNG is decoded ONCE into
// system memory at its full 4K-art size. What goes to the GPU is always a copy
// resampled on the CPU to the exact pixel size it occupies in the window
// (WIC high-quality cubic / Fant). A 4K plate in a 1366x768 window costs a
// 1366x768 texture, never the 4K original. Resizing the window rebuilds those
// copies.
//
// All drawing calls take coordinates in layout pixels (the 1920x1080 design
// space); setView() maps layout pixels to the window.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace whe::ui {

using Microsoft::WRL::ComPtr;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] bool contains(float px, float py) const noexcept { return px >= x && py >= y && px < x + w && py < y + h; }
    [[nodiscard]] float cx() const noexcept { return x + w * 0.5f; }
    [[nodiscard]] float cy() const noexcept { return y + h * 0.5f; }
};

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
};

enum class Align { Left, Center, Right };

struct TextStyle {
    float size = 16.0f;
    int weight = 700;          // 500 medium, 600 semibold, 700 bold, 800 extra-bold
    Color color{0.89f, 0.89f, 0.91f, 1};
    Align align = Align::Left;
    bool glow = false;         // cyan halo, for numbers behind glass and lit labels
    bool engrave = false;      // dark top / light bottom edge, for labels cut into the plate
    bool wrap = false;         // wrap inside the rect (otherwise one line, clipped)
    float lineSpacing = 0.0f;  // 0 = font default
    Color glowColor{0.247f, 0.847f, 0.918f, 1};
};

enum class Sprite : int {
    Plate,
    KnobBody, KnobPointer, KnobBigBody, KnobBigPointer,
    LedOn, LedOff,
    ButtonOn, ButtonOff,
    RoundOn, RoundOff,
    FaderCap,
    KeyWhite, KeyWhiteDown, KeyBlack, KeyBlackDown,
    Crown, Ghost,
    Count
};

class Gfx {
public:
    Gfx() = default;
    ~Gfx();
    Gfx(const Gfx&) = delete;
    Gfx& operator=(const Gfx&) = delete;

    // artDir holds the rendered PNGs, fontDir the Montserrat .ttf files.
    // Returns an empty string on success, else a readable error.
    std::wstring init(HWND hwnd, const std::filesystem::path& artDir, const std::filesystem::path& brandDir,
                      const std::filesystem::path& fontDir);
    void resize(UINT pixelW, UINT pixelH);

    // Layout -> window mapping (uniform scale + letterbox offset), in window pixels.
    void setView(float scale, float offsetX, float offsetY) noexcept;
    [[nodiscard]] float scale() const noexcept { return scale_; }

    bool beginFrame();   // false if the render target is unavailable
    void endFrame();     // handles device loss (rebuilds on the next frame)

    // Cached layer: everything that does not change every frame is drawn once
    // into an offscreen bitmap and reused, so meters can animate cheaply (this
    // matters over remote desktop). Call begin/end outside a frame.
    bool beginLayer();
    void endLayer();
    void drawLayer();    // inside a frame
    [[nodiscard]] bool layerValid() const noexcept { return layer_ != nullptr && layerOk_; }
    void invalidateLayer() noexcept { layerOk_ = false; }

    // ---- drawing, all in layout pixels ----
    void clear(Color c);
    void sprite(Sprite s, const Rect& r, float rotationDeg = 0.0f, float opacity = 1.0f);
    // 9-slice: the sprite's outer `sliceSrc` source pixels form the frame; `border`
    // is the frame thickness in layout px; `outset` grows the drawn area beyond r.
    void nineSlice(Sprite s, const Rect& r, float sliceSrc, float border, float outset);
    void fillRect(const Rect& r, Color c, float radius = 0.0f);
    void strokeRect(const Rect& r, Color c, float width, float radius = 0.0f);
    void fillGradientV(const Rect& r, Color top, Color bottom, float radius = 0.0f);
    void line(float x0, float y0, float x1, float y1, Color c, float width);
    void fillCircle(float cx, float cy, float r, Color inner, Color outer);  // radial shade
    void strokeCircle(float cx, float cy, float r, Color c, float width);
    void text(std::wstring_view s, const Rect& r, const TextStyle& st);
    [[nodiscard]] float textWidth(std::wstring_view s, const TextStyle& st);
    void pushClip(const Rect& r);
    void popClip();

    [[nodiscard]] bool fontLoaded() const noexcept { return customFonts_ != nullptr; }

private:
    struct SpriteData {
        ComPtr<IWICBitmapSource> source;  // full-size decoded art (system memory)
        UINT w = 0, h = 0;
        std::map<std::pair<UINT, UINT>, ComPtr<ID2D1Bitmap>> sized;  // GPU copies at exact pixel sizes
    };
    bool createTarget();
    void discardTarget();
    ID2D1Bitmap* bitmapAt(Sprite s, UINT pw, UINT ph);
    IDWriteTextFormat* format(const TextStyle& st);
    ID2D1SolidColorBrush* brush(Color c);
    [[nodiscard]] D2D1_RECT_F px(const Rect& r) const noexcept;

    HWND hwnd_ = nullptr;
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<IDWriteFontCollection> customFonts_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1BitmapRenderTarget> layer_;
    ID2D1RenderTarget* cur_ = nullptr;  // where drawing goes: the window or the layer
    bool layerOk_ = false;
    ComPtr<ID2D1SolidColorBrush> brush_;
    SpriteData sprites_[static_cast<int>(Sprite::Count)];
    std::map<std::tuple<int, int, int, bool>, ComPtr<IDWriteTextFormat>> formats_;
    std::wstring family_ = L"Montserrat";
    float scale_ = 1.0f, ox_ = 0.0f, oy_ = 0.0f;
    UINT pw_ = 1, ph_ = 1;
    int clipDepth_ = 0;
};

}  // namespace whe::ui
