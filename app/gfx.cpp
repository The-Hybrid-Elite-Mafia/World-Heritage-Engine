#include "gfx.hpp"

#include <algorithm>
#include <cmath>

namespace whe::ui {

namespace {

constexpr const wchar_t* kSpriteFiles[] = {
    L"plate_4k.png",
    L"knob_body.png", L"knob_pointer.png", L"knob_big_body.png", L"knob_big_pointer.png",
    L"led_on.png", L"led_off.png",
    L"button_on.png", L"button_off.png",
    L"round_on.png", L"round_off.png",
    L"fader_cap.png",
    L"key_white.png", L"key_white_down.png", L"key_black.png", L"key_black_down.png",
    L"crown.png", L"ghost.png",
};
static_assert(std::size(kSpriteFiles) == static_cast<std::size_t>(Sprite::Count));

D2D1_COLOR_F d2c(Color c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }

}  // namespace

Gfx::~Gfx() { discardTarget(); }

std::wstring Gfx::init(HWND hwnd, const std::filesystem::path& artDir, const std::filesystem::path& brandDir,
                       const std::filesystem::path& fontDir) {
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return L"Direct2D is not available.";
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.GetAddressOf()))))
        return L"Windows Imaging Component is not available.";
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw_.GetAddressOf()))))
        return L"DirectWrite is not available.";

    // Decode every PNG once, convert to premultiplied BGRA, keep it in system memory.
    for (int i = 0; i < static_cast<int>(Sprite::Count); ++i) {
        const bool brand = i == static_cast<int>(Sprite::Crown) || i == static_cast<int>(Sprite::Ghost);
        const auto file = (brand ? brandDir : artDir) / kSpriteFiles[i];
        ComPtr<IWICBitmapDecoder> dec;
        if (FAILED(wic_->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.GetAddressOf()))) {
            if (brand) continue;  // badges are optional art; the faceplate still works without them
            return L"Missing art file: " + file.wstring();
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> conv;
        ComPtr<IWICBitmap> bmp;
        if (FAILED(dec->GetFrame(0, frame.GetAddressOf())) || FAILED(wic_->CreateFormatConverter(conv.GetAddressOf())) ||
            FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
            FAILED(wic_->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, bmp.GetAddressOf())))
            return L"Could not decode art file: " + file.wstring();
        auto& sd = sprites_[i];
        sd.source = bmp;
        bmp->GetSize(&sd.w, &sd.h);
    }

    // Montserrat from the program folder (no install needed). Needs Windows 10 1703+;
    // older systems fall back to Segoe UI.
    ComPtr<IDWriteFactory5> f5;
    if (SUCCEEDED(dw_.As(&f5))) {
        ComPtr<IDWriteFontSetBuilder1> builder;
        if (SUCCEEDED(f5->CreateFontSetBuilder(builder.GetAddressOf()))) {
            int added = 0;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(fontDir, ec)) {
                if (e.path().extension() != L".ttf") continue;
                ComPtr<IDWriteFontFile> ff;
                if (SUCCEEDED(f5->CreateFontFileReference(e.path().c_str(), nullptr, ff.GetAddressOf())) && SUCCEEDED(builder->AddFontFile(ff.Get())))
                    ++added;
            }
            ComPtr<IDWriteFontSet> set;
            ComPtr<IDWriteFontCollection1> coll;
            if (added > 0 && SUCCEEDED(builder->CreateFontSet(set.GetAddressOf())) &&
                SUCCEEDED(f5->CreateFontCollectionFromFontSet(set.Get(), coll.GetAddressOf()))) {
                UINT32 idx = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(coll->FindFamilyName(L"Montserrat", &idx, &exists)) && exists) customFonts_ = coll;
            }
        }
    }
    if (!customFonts_) family_ = L"Segoe UI";

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    pw_ = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    ph_ = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
    if (!createTarget()) return L"Could not create the Direct2D window target.";
    return {};
}

bool Gfx::createTarget() {
    if (rt_) return true;
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    const auto hprops = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(pw_, ph_), D2D1_PRESENT_OPTIONS_NONE);
    if (FAILED(d2d_->CreateHwndRenderTarget(props, hprops, rt_.GetAddressOf()))) return false;
    rt_->SetDpi(96.0f, 96.0f);  // 1 DIP = 1 window pixel; scaling is ours
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush_.GetAddressOf());
    return true;
}

void Gfx::discardTarget() {
    for (auto& s : sprites_) s.sized.clear();
    layer_.Reset();
    layerOk_ = false;
    brush_.Reset();
    cur_ = nullptr;
    rt_.Reset();
}

void Gfx::resize(UINT pixelW, UINT pixelH) {
    pw_ = std::max<UINT>(1, pixelW);
    ph_ = std::max<UINT>(1, pixelH);
    for (auto& s : sprites_) s.sized.clear();  // art is re-sampled for the new window size
    layer_.Reset();
    layerOk_ = false;
    if (rt_) rt_->Resize(D2D1::SizeU(pw_, ph_));
}

void Gfx::setView(float scale, float offsetX, float offsetY) noexcept {
    if (std::abs(scale - scale_) > 1e-6f) {
        for (auto& s : sprites_) s.sized.clear();
        layerOk_ = false;
    }
    if (std::abs(offsetX - ox_) > 0.01f || std::abs(offsetY - oy_) > 0.01f) layerOk_ = false;
    scale_ = scale;
    ox_ = offsetX;
    oy_ = offsetY;
}

bool Gfx::beginFrame() {
    if (!createTarget()) return false;
    cur_ = rt_.Get();
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void Gfx::endFrame() {
    while (clipDepth_ > 0) popClip();
    if (rt_->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) discardTarget();
}

bool Gfx::beginLayer() {
    if (!createTarget()) return false;
    if (!layer_) {
        // Same pixel size as the window; shares bitmaps and brushes with the window target.
        if (FAILED(rt_->CreateCompatibleRenderTarget(D2D1::SizeF(static_cast<float>(pw_), static_cast<float>(ph_)), D2D1::SizeU(pw_, ph_),
                                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                     D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, layer_.GetAddressOf())))
            return false;
        layer_->SetDpi(96.0f, 96.0f);
        layer_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    cur_ = layer_.Get();
    layer_->BeginDraw();
    layer_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void Gfx::endLayer() {
    while (clipDepth_ > 0) popClip();
    const HRESULT hr = layer_->EndDraw();
    cur_ = rt_.Get();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        discardTarget();
        return;
    }
    layerOk_ = SUCCEEDED(hr);
}

void Gfx::drawLayer() {
    if (!layer_) return;
    ComPtr<ID2D1Bitmap> bmp;
    if (FAILED(layer_->GetBitmap(bmp.GetAddressOf()))) return;
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->DrawBitmap(bmp.Get(), D2D1::RectF(0, 0, static_cast<float>(pw_), static_cast<float>(ph_)), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

D2D1_RECT_F Gfx::px(const Rect& r) const noexcept {
    return D2D1::RectF(ox_ + r.x * scale_, oy_ + r.y * scale_, ox_ + (r.x + r.w) * scale_, oy_ + (r.y + r.h) * scale_);
}

ID2D1SolidColorBrush* Gfx::brush(Color c) {
    brush_->SetColor(d2c(c));
    return brush_.Get();
}

void Gfx::clear(Color c) {
    cur_->SetTransform(D2D1::Matrix3x2F::Identity());
    cur_->Clear(d2c(c));
}

ID2D1Bitmap* Gfx::bitmapAt(Sprite s, UINT pw, UINT ph) {
    auto& sd = sprites_[static_cast<int>(s)];
    if (!sd.source) return nullptr;
    pw = std::clamp<UINT>(pw, 1, 8192);
    ph = std::clamp<UINT>(ph, 1, 8192);
    const auto key = std::make_pair(pw, ph);
    if (auto it = sd.sized.find(key); it != sd.sized.end()) return it->second.Get();
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic_->CreateBitmapScaler(scaler.GetAddressOf()))) return nullptr;
    // Down-sampling on the CPU: high-quality cubic (Windows 10+), Fant otherwise.
    // WICBitmapInterpolationModeHighQualityCubic = 4 (Windows 10 SDK; spelled out for older headers).
    if (FAILED(scaler->Initialize(sd.source.Get(), pw, ph, static_cast<WICBitmapInterpolationMode>(4))) &&
        FAILED(scaler->Initialize(sd.source.Get(), pw, ph, WICBitmapInterpolationModeFant)))
        return nullptr;
    ComPtr<ID2D1Bitmap> bmp;
    const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(rt_->CreateBitmapFromWicBitmap(scaler.Get(), props, bmp.GetAddressOf()))) return nullptr;
    auto* raw = bmp.Get();
    sd.sized.emplace(key, std::move(bmp));
    return raw;
}

void Gfx::sprite(Sprite s, const Rect& r, float rotationDeg, float opacity) {
    const D2D1_RECT_F d = px(r);
    const UINT pw = static_cast<UINT>(std::lround(d.right - d.left));
    const UINT ph = static_cast<UINT>(std::lround(d.bottom - d.top));
    ID2D1Bitmap* bmp = bitmapAt(s, pw, ph);
    if (!bmp) return;
    const D2D1_RECT_F dst = D2D1::RectF(d.left, d.top, d.left + static_cast<float>(pw), d.top + static_cast<float>(ph));
    if (rotationDeg != 0.0f)
        cur_->SetTransform(D2D1::Matrix3x2F::Rotation(rotationDeg, D2D1::Point2F((dst.left + dst.right) * 0.5f, (dst.top + dst.bottom) * 0.5f)));
    else
        cur_->SetTransform(D2D1::Matrix3x2F::Identity());
    cur_->DrawBitmap(bmp, dst, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    cur_->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Gfx::nineSlice(Sprite s, const Rect& r, float sliceSrc, float border, float outset) {
    auto& sd = sprites_[static_cast<int>(s)];
    if (!sd.source) return;
    const float k = border * scale_ / sliceSrc;  // source px -> window px
    const UINT bw = static_cast<UINT>(std::max(1L, std::lround(sd.w * k)));
    const UINT bh = static_cast<UINT>(std::max(1L, std::lround(sd.h * k)));
    ID2D1Bitmap* bmp = bitmapAt(s, bw, bh);
    if (!bmp) return;
    const float sb = sliceSrc * static_cast<float>(bw) / static_cast<float>(sd.w);  // slice in bitmap px
    D2D1_RECT_F d = px(Rect{r.x - outset, r.y - outset, r.w + 2 * outset, r.h + 2 * outset});
    const float bpx = border * scale_;
    const float xs[4] = {d.left, d.left + bpx, d.right - bpx, d.right};
    const float ys[4] = {d.top, d.top + bpx, d.bottom - bpx, d.bottom};
    const float sx[4] = {0, sb, static_cast<float>(bw) - sb, static_cast<float>(bw)};
    const float sy[4] = {0, sb, static_cast<float>(bh) - sb, static_cast<float>(bh)};
    cur_->SetTransform(D2D1::Matrix3x2F::Identity());
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            if (xs[i + 1] <= xs[i] || ys[j + 1] <= ys[j]) continue;
            cur_->DrawBitmap(bmp, D2D1::RectF(xs[i], ys[j], xs[i + 1], ys[j + 1]), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                            D2D1::RectF(sx[i], sy[j], sx[i + 1], sy[j + 1]));
        }
}

void Gfx::fillRect(const Rect& r, Color c, float radius) {
    cur_->SetTransform(D2D1::Matrix3x2F::Identity());
    const auto d = px(r);
    if (radius > 0) cur_->FillRoundedRectangle(D2D1::RoundedRect(d, radius * scale_, radius * scale_), brush(c));
    else cur_->FillRectangle(d, brush(c));
}

void Gfx::strokeRect(const Rect& r, Color c, float width, float radius) {
    cur_->SetTransform(D2D1::Matrix3x2F::Identity());
    const auto d = px(r);
    if (radius > 0) cur_->DrawRoundedRectangle(D2D1::RoundedRect(d, radius * scale_, radius * scale_), brush(c), width * scale_);
    else cur_->DrawRectangle(d, brush(c), width * scale_);
}

void Gfx::fillGradientV(const Rect& r, Color top, Color bottom, float radius) {
    cur_->SetTransform(D2D1::Matrix3x2F::Identity());
    const auto d = px(r);
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, d2c(top)}, {1.0f, d2c(bottom)}};
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (FAILED(cur_->CreateGradientStopCollection(stops, 2, coll.GetAddressOf()))) return;
    if (FAILED(cur_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(d.left, d.top), D2D1::Point2F(d.left, d.bottom)),
                                              coll.Get(), lg.GetAddressOf())))
        return;
    if (radius > 0) cur_->FillRoundedRectangle(D2D1::RoundedRect(d, radius * scale_, radius * scale_), lg.Get());
    else cur_->FillRectangle(d, lg.Get());
}

void Gfx::line(float x0, float y0, float x1, float y1, Color c, float width) {
    cur_->SetTransform(D2D1::Matrix3x2F::Identity());
    cur_->DrawLine(D2D1::Point2F(ox_ + x0 * scale_, oy_ + y0 * scale_), D2D1::Point2F(ox_ + x1 * scale_, oy_ + y1 * scale_), brush(c), width * scale_);
}

void Gfx::fillCircle(float cx, float cy, float r, Color inner, Color outer) {
    cur_->SetTransform(D2D1::Matrix3x2F::Identity());
    const D2D1_POINT_2F c = D2D1::Point2F(ox_ + cx * scale_, oy_ + cy * scale_);
    const float pr = r * scale_;
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, d2c(inner)}, {1.0f, d2c(outer)}};
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1RadialGradientBrush> rg;
    if (FAILED(cur_->CreateGradientStopCollection(stops, 2, coll.GetAddressOf()))) return;
    // Light from the top-left, like the rendered parts.
    if (FAILED(cur_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, D2D1::Point2F(-pr * 0.3f, -pr * 0.3f), pr * 1.2f, pr * 1.2f),
                                               coll.Get(), rg.GetAddressOf())))
        return;
    cur_->FillEllipse(D2D1::Ellipse(c, pr, pr), rg.Get());
}

void Gfx::strokeCircle(float cx, float cy, float r, Color c, float width) {
    cur_->SetTransform(D2D1::Matrix3x2F::Identity());
    cur_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ox_ + cx * scale_, oy_ + cy * scale_), r * scale_, r * scale_), brush(c), width * scale_);
}

IDWriteTextFormat* Gfx::format(const TextStyle& st) {
    const int sizeKey = static_cast<int>(std::lround(st.size * 4.0f));
    const auto key = std::make_tuple(sizeKey, st.weight * 4 + static_cast<int>(st.align), static_cast<int>(st.lineSpacing * 4.0f), st.wrap);
    if (auto it = formats_.find(key); it != formats_.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(dw_->CreateTextFormat(family_.c_str(), customFonts_.Get(), static_cast<DWRITE_FONT_WEIGHT>(st.weight), DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, st.size, L"en-us", f.GetAddressOf())))
        return nullptr;
    f->SetTextAlignment(st.align == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING
                        : st.align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                     : DWRITE_TEXT_ALIGNMENT_TRAILING);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    f->SetWordWrapping(st.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (st.lineSpacing > 0) f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, st.lineSpacing, st.lineSpacing * 0.8f);
    auto* raw = f.Get();
    formats_.emplace(key, std::move(f));
    return raw;
}

void Gfx::text(std::wstring_view s, const Rect& r, const TextStyle& st) {
    if (s.empty()) return;
    IDWriteTextFormat* f = format(st);
    if (!f) return;
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(dw_->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), f, r.w, std::max(r.h, st.size * 1.4f), lay.GetAddressOf()))) return;
    if (!st.wrap) {
        // One-line text that does not fit ends in "…" instead of running past its box.
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dw_->CreateEllipsisTrimmingSign(f, ellipsis.GetAddressOf()))) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            lay->SetTrimming(&trim, ellipsis.Get());
        }
    }
    // Text is laid out in layout pixels and drawn through the view transform (vector, sharp at any size).
    const auto view = D2D1::Matrix3x2F::Scale(scale_, scale_) * D2D1::Matrix3x2F::Translation(ox_, oy_);
    cur_->SetTransform(view);
    if (st.glow) {
        Color g = st.glowColor;
        const float rings[2][2] = {{1.6f, 0.20f}, {3.4f, 0.07f}};
        for (const auto& ring : rings) {
            g.a = ring[1];
            ID2D1SolidColorBrush* b = brush(g);
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * 0.785398f;
                cur_->DrawTextLayout(D2D1::Point2F(r.x + ring[0] * std::cos(a), r.y + ring[0] * std::sin(a)), lay.Get(), b);
            }
        }
    }
    if (st.engrave) {
        cur_->DrawTextLayout(D2D1::Point2F(r.x, r.y - 1.0f), lay.Get(), brush(Color{0, 0, 0, 0.9f}));
        cur_->DrawTextLayout(D2D1::Point2F(r.x, r.y + 1.0f), lay.Get(), brush(Color{1, 1, 1, 0.10f}));
    }
    cur_->DrawTextLayout(D2D1::Point2F(r.x, r.y), lay.Get(), brush(st.color));
    cur_->SetTransform(D2D1::Matrix3x2F::Identity());
}

float Gfx::textWidth(std::wstring_view s, const TextStyle& st) {
    IDWriteTextFormat* f = format(TextStyle{st.size, st.weight, st.color, Align::Left, false, false, false, 0.0f, st.glowColor});
    if (!f || s.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(dw_->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), f, 10000.0f, st.size * 2.0f, lay.GetAddressOf()))) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void Gfx::pushClip(const Rect& r) {
    cur_->SetTransform(D2D1::Matrix3x2F::Identity());
    cur_->PushAxisAlignedClip(px(r), D2D1_ANTIALIAS_MODE_ALIASED);
    ++clipDepth_;
}

void Gfx::popClip() {
    if (clipDepth_ <= 0) return;
    cur_->PopAxisAlignedClip();
    --clipDepth_;
}

}  // namespace whe::ui
