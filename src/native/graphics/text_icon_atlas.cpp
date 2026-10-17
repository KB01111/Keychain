#include "graphics/text_icon_atlas.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace keychain {

namespace {

// Custom IDWriteTextRenderer that captures glyph runs and rasterizes grayscale coverage
// via IDWriteFactory2::CreateGlyphRunAnalysis [R15] into a target bitmap buffer.
class GrayscaleGlyphTextRenderer final : public IDWriteTextRenderer {
public:
    GrayscaleGlyphTextRenderer(
        IDWriteFactory2* factory2,
        float dpiScale,
        uint8_t* destBuffer,
        uint32_t destWidth,
        uint32_t destHeight)
        : factory2_(factory2)
        , dpiScale_(dpiScale)
        , destBuffer_(destBuffer)
        , destWidth_(destWidth)
        , destHeight_(destHeight) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IDWritePixelSnapping) ||
            riid == __uuidof(IDWriteTextRenderer)) {
            *ppvObject = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refCount_; }

    // IDWritePixelSnapping
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* isDisabled) override {
        *isDisabled = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
        transform->m11 = 1.0f;
        transform->m12 = 0.0f;
        transform->m21 = 0.0f;
        transform->m22 = 1.0f;
        transform->dx = 0.0f;
        transform->dy = 0.0f;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override {
        *pixelsPerDip = dpiScale_;
        return S_OK;
    }

    // IDWriteTextRenderer
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*,
        FLOAT baselineOriginX,
        FLOAT baselineOriginY,
        DWRITE_MEASURING_MODE measuringMode,
        DWRITE_GLYPH_RUN const* glyphRun,
        DWRITE_GLYPH_RUN_DESCRIPTION const*,
        IUnknown*) override {
        if (!factory2_ || !glyphRun || glyphRun->glyphCount == 0) {
            return S_OK;
        }

        Microsoft::WRL::ComPtr<IDWriteGlyphRunAnalysis> analysis;
        // [R15] Use IDWriteFactory2::CreateGlyphRunAnalysis with grayscale antialiasing
        HRESULT hr = factory2_->CreateGlyphRunAnalysis(
            glyphRun,
            nullptr,
            DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
            measuringMode,
            DWRITE_GRID_FIT_MODE_DEFAULT,
            DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
            baselineOriginX * dpiScale_,
            baselineOriginY * dpiScale_,
            &analysis);

        if (FAILED(hr) || !analysis) {
            return S_OK;
        }

        RECT textureBounds{};
        hr = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &textureBounds);
        // For grayscale antialiasing, DWRITE_TEXTURE_CLEARTYPE_3x1 is always supported across DWrite versions,
        // and when created with DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE or averaged across R,G,B it yields crisp grayscale.
        bool useClearTypeBounds = false;
        if (FAILED(hr) || (textureBounds.right <= textureBounds.left) || (textureBounds.bottom <= textureBounds.top)) {
            hr = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &textureBounds);
            if (FAILED(hr) || (textureBounds.right <= textureBounds.left) || (textureBounds.bottom <= textureBounds.top)) {
                return S_OK;
            }
            useClearTypeBounds = true;
        }

        const int bw = textureBounds.right - textureBounds.left;
        const int bh = textureBounds.bottom - textureBounds.top;
        if (bw <= 0 || bh <= 0 || bw > 2048 || bh > 512) {
            return S_OK;
        }

        if (useClearTypeBounds) {
            std::vector<uint8_t> alphaValues(static_cast<size_t>(bw) * static_cast<size_t>(bh) * 3u, 0);
            hr = analysis->CreateAlphaTexture(
                DWRITE_TEXTURE_CLEARTYPE_3x1,
                &textureBounds,
                alphaValues.data(),
                static_cast<UINT32>(alphaValues.size()));
            if (FAILED(hr)) {
                return S_OK;
            }
            for (int y = 0; y < bh; ++y) {
                const int dstY = textureBounds.top + y;
                if (dstY < 0 || dstY >= static_cast<int>(destHeight_)) continue;
                for (int x = 0; x < bw; ++x) {
                    const int dstX = textureBounds.left + x;
                    if (dstX < 0 || dstX >= static_cast<int>(destWidth_)) continue;
                    const size_t srcIdx = (static_cast<size_t>(y) * bw + x) * 3u;
                    const uint32_t lum = (static_cast<uint32_t>(alphaValues[srcIdx + 0]) +
                                          static_cast<uint32_t>(alphaValues[srcIdx + 1]) +
                                          static_cast<uint32_t>(alphaValues[srcIdx + 2])) / 3u;
                    uint8_t& dstPx = destBuffer_[static_cast<size_t>(dstY) * destWidth_ + dstX];
                    dstPx = static_cast<uint8_t>(std::min<uint32_t>(255u, std::max<uint32_t>(dstPx, lum)));
                }
            }
        } else {
            std::vector<uint8_t> alphaValues(static_cast<size_t>(bw) * static_cast<size_t>(bh), 0);
            hr = analysis->CreateAlphaTexture(
                DWRITE_TEXTURE_ALIASED_1x1,
                &textureBounds,
                alphaValues.data(),
                static_cast<UINT32>(alphaValues.size()));
            if (FAILED(hr)) {
                return S_OK;
            }
            for (int y = 0; y < bh; ++y) {
                const int dstY = textureBounds.top + y;
                if (dstY < 0 || dstY >= static_cast<int>(destHeight_)) continue;
                for (int x = 0; x < bw; ++x) {
                    const int dstX = textureBounds.left + x;
                    if (dstX < 0 || dstX >= static_cast<int>(destWidth_)) continue;
                    const uint8_t cov = alphaValues[static_cast<size_t>(y) * bw + x];
                    uint8_t& dstPx = destBuffer_[static_cast<size_t>(dstY) * destWidth_ + dstX];
                    dstPx = std::max(dstPx, cov);
                }
            }
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(
        void*, FLOAT, FLOAT, DWRITE_UNDERLINE const*, IUnknown*) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(
        void*, FLOAT, FLOAT, DWRITE_STRIKETHROUGH const*, IUnknown*) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawInlineObject(
        void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override {
        return S_OK;
    }

private:
    ULONG refCount_ = 1;
    IDWriteFactory2* factory2_ = nullptr;
    float dpiScale_ = 1.0f;
    uint8_t* destBuffer_ = nullptr;
    uint32_t destWidth_ = 0;
    uint32_t destHeight_ = 0;
};

float SdfSegment(float px, float py, float ax, float ay, float bx, float by) {
    const float pax = px - ax, pay = py - ay;
    const float bax = bx - ax, bay = by - ay;
    const float denom = bax * bax + bay * bay;
    const float h = (denom > 1e-6f) ? std::clamp((pax * bax + pay * bay) / denom, 0.0f, 1.0f) : 0.0f;
    const float dx = pax - bax * h;
    const float dy = pay - bay * h;
    return std::sqrt(dx * dx + dy * dy);
}

float SdfRoundedBox(float px, float py, float cx, float cy, float hx, float hy, float r) {
    const float qx = std::abs(px - cx) - hx + r;
    const float qy = std::abs(py - cy) - hy + r;
    const float outDist = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f));
    const float inDist = std::min(std::max(qx, qy), 0.0f);
    return outDist + inDist - r;
}

} // namespace

bool TextAndIconAtlas::Initialize() {
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory2),
        reinterpret_cast<IUnknown**>(dwriteFactory_.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !dwriteFactory_) {
        return false;
    }

    // WIC factory is optional for shell icon fallback; procedural icons work regardless
    CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory_));

    glyphPixels_.assign(static_cast<size_t>(kGlyphAtlasWidth) * kGlyphAtlasHeight, 0);
    iconPixels_.assign(static_cast<size_t>(kIconAtlasWidth) * kIconAtlasHeight * 4u, 0);

    BuildProceduralVectorIcons();
    glyphDirty_ = true;
    iconDirty_ = true;
    return true;
}

void TextAndIconAtlas::ClearTextCache() {
    std::fill(glyphPixels_.begin(), glyphPixels_.end(), uint8_t(0));
    glyphShelfX_ = 2;
    glyphShelfY_ = 2;
    glyphShelfRowHeight_ = 0;
    textCache_.clear();
    glyphDirty_ = true;
}

AtlasUvRect TextAndIconAtlas::AllocateIconSlot() {
    const uint32_t cols = kIconAtlasWidth / kIconSlotSizePx;
    const uint32_t maxSlots = cols * (kIconAtlasHeight / kIconSlotSizePx);
    const uint32_t idx = (nextIconSlotIndex_ < maxSlots) ? nextIconSlotIndex_++ : (maxSlots - 1);
    const uint32_t col = idx % cols;
    const uint32_t row = idx / cols;
    const float x = static_cast<float>(col * kIconSlotSizePx);
    const float y = static_cast<float>(row * kIconSlotSizePx);

    AtlasUvRect rect{};
    rect.u0 = (x + 1.0f) / static_cast<float>(kIconAtlasWidth);
    rect.v0 = (y + 1.0f) / static_cast<float>(kIconAtlasHeight);
    rect.u1 = (x + kIconSlotSizePx - 1.0f) / static_cast<float>(kIconAtlasWidth);
    rect.v1 = (y + kIconSlotSizePx - 1.0f) / static_cast<float>(kIconAtlasHeight);
    rect.widthPx = static_cast<float>(kIconSlotSizePx - 2);
    rect.heightPx = static_cast<float>(kIconSlotSizePx - 2);
    return rect;
}

void TextAndIconAtlas::BuildProceduralVectorIcons() {
    const std::vector<std::wstring> names = {
        L"folder", L"download", L"explorer", L"edit",
        L"settings", L"keychain", L"grid", L"shortcut"
    };

    const uint32_t cols = kIconAtlasWidth / kIconSlotSizePx;

    for (size_t i = 0; i < names.size(); ++i) {
        const uint32_t slotIdx = nextIconSlotIndex_++;
        const uint32_t slotX = (slotIdx % cols) * kIconSlotSizePx;
        const uint32_t slotY = (slotIdx / cols) * kIconSlotSizePx;

        for (uint32_t py = 0; py < kIconSlotSizePx; ++py) {
            for (uint32_t px = 0; px < kIconSlotSizePx; ++px) {
                const float x = static_cast<float>(px) + 0.5f;
                const float y = static_cast<float>(py) + 0.5f;
                float sdf = 100.0f;

                if (names[i] == L"folder") {
                    const float body = std::abs(SdfRoundedBox(x, y, 32.0f, 35.0f, 18.0f, 12.0f, 3.0f)) - 1.6f;
                    const float tab = std::abs(SdfRoundedBox(x, y, 22.0f, 21.0f, 8.0f, 3.0f, 2.0f)) - 1.5f;
                    sdf = std::min(body, tab);
                } else if (names[i] == L"download") {
                    const float shaft = SdfSegment(x, y, 32.0f, 16.0f, 32.0f, 38.0f) - 1.8f;
                    const float leftWing = SdfSegment(x, y, 23.0f, 30.0f, 32.0f, 39.0f) - 1.8f;
                    const float rightWing = SdfSegment(x, y, 41.0f, 30.0f, 32.0f, 39.0f) - 1.8f;
                    const float trayBottom = SdfSegment(x, y, 18.0f, 46.0f, 46.0f, 46.0f) - 1.8f;
                    sdf = std::min({shaft, leftWing, rightWing, trayBottom});
                } else if (names[i] == L"explorer") {
                    const float winBox = std::abs(SdfRoundedBox(x, y, 32.0f, 32.0f, 18.0f, 14.0f, 3.0f)) - 1.6f;
                    const float headerBar = SdfSegment(x, y, 15.0f, 25.0f, 49.0f, 25.0f) - 1.3f;
                    sdf = std::min(winBox, headerBar);
                } else if (names[i] == L"edit") {
                    const float pen = std::abs(SdfSegment(x, y, 21.0f, 43.0f, 43.0f, 21.0f) - 3.2f) - 1.3f;
                    const float baseLine = SdfSegment(x, y, 18.0f, 47.0f, 46.0f, 47.0f) - 1.5f;
                    sdf = std::min(pen, baseLine);
                } else if (names[i] == L"settings") {
                    const float dx = x - 32.0f;
                    const float dy = y - 32.0f;
                    const float r = std::hypot(dx, dy);
                    const float angle = std::atan2(dy, dx);
                    const float gearWave = std::cos(angle * 6.0f) * 2.4f;
                    const float outerGear = std::abs(r - (15.5f + gearWave)) - 1.7f;
                    const float innerHole = std::abs(r - 6.5f) - 1.6f;
                    sdf = std::min(outerGear, innerHole);
                } else if (names[i] == L"keychain") {
                    const float dx = x - 32.0f;
                    const float dy = y - 32.0f;
                    const float r = std::hypot(dx, dy);
                    const float outerRing = std::abs(r - 17.5f) - 1.8f;
                    const float innerHub = std::abs(r - 6.5f) - 1.6f;
                    const float spoke1 = SdfSegment(x, y, 32.0f, 15.0f, 32.0f, 25.0f) - 1.3f;
                    const float spoke2 = SdfSegment(x, y, 32.0f, 39.0f, 32.0f, 49.0f) - 1.3f;
                    const float spoke3 = SdfSegment(x, y, 15.0f, 32.0f, 25.0f, 32.0f) - 1.3f;
                    const float spoke4 = SdfSegment(x, y, 39.0f, 32.0f, 49.0f, 32.0f) - 1.3f;
                    sdf = std::min({outerRing, innerHub, spoke1, spoke2, spoke3, spoke4});
                } else if (names[i] == L"grid") {
                    const float b1 = std::abs(SdfRoundedBox(x, y, 23.0f, 23.0f, 6.5f, 6.5f, 2.0f)) - 1.5f;
                    const float b2 = std::abs(SdfRoundedBox(x, y, 41.0f, 23.0f, 6.5f, 6.5f, 2.0f)) - 1.5f;
                    const float b3 = std::abs(SdfRoundedBox(x, y, 23.0f, 41.0f, 6.5f, 6.5f, 2.0f)) - 1.5f;
                    const float b4 = std::abs(SdfRoundedBox(x, y, 41.0f, 41.0f, 6.5f, 6.5f, 2.0f)) - 1.5f;
                    sdf = std::min({b1, b2, b3, b4});
                } else {
                    // shortcut lightning / key
                    const float s1 = SdfSegment(x, y, 35.0f, 16.0f, 24.0f, 33.0f) - 1.8f;
                    const float s2 = SdfSegment(x, y, 24.0f, 33.0f, 38.0f, 33.0f) - 1.8f;
                    const float s3 = SdfSegment(x, y, 38.0f, 33.0f, 28.0f, 48.0f) - 1.8f;
                    sdf = std::min({s1, s2, s3});
                }

                const float coverage = std::clamp(0.5f - sdf, 0.0f, 1.0f);
                const uint8_t val = static_cast<uint8_t>( std::round(coverage * 255.0f));
                const size_t dstIdx = ((static_cast<size_t>(slotY + py) * kIconAtlasWidth) + (slotX + px)) * 4u;
                // Premultiplied BGRA8 white icon so vertex color tints it cleanly
                iconPixels_[dstIdx + 0] = val;
                iconPixels_[dstIdx + 1] = val;
                iconPixels_[dstIdx + 2] = val;
                iconPixels_[dstIdx + 3] = val;
            }
        }

        AtlasUvRect rect{};
        rect.u0 = static_cast<float>(slotX + 1) / static_cast<float>(kIconAtlasWidth);
        rect.v0 = static_cast<float>(slotY + 1) / static_cast<float>(kIconAtlasHeight);
        rect.u1 = static_cast<float>(slotX + kIconSlotSizePx - 1) / static_cast<float>(kIconAtlasWidth);
        rect.v1 = static_cast<float>(slotY + kIconSlotSizePx - 1) / static_cast<float>(kIconAtlasHeight);
        rect.widthPx = static_cast<float>(kIconSlotSizePx - 2);
        rect.heightPx = static_cast<float>(kIconSlotSizePx - 2);
        iconCache_[names[i]] = rect;
    }
}

bool TextAndIconAtlas::TryExtractShellIcon(const std::wstring& targetPath, AtlasUvRect& outRect) {
    if (targetPath.empty()) {
        return false;
    }

    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(targetPath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) || !sfi.hIcon) {
        return false;
    }

    ICONINFO iconInfo{};
    if (!GetIconInfo(sfi.hIcon, &iconInfo)) {
        DestroyIcon(sfi.hIcon);
        return false;
    }

    BITMAP bm{};
    if (GetObjectW(iconInfo.hbmColor, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
        if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
        DestroyIcon(sfi.hIcon);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = -bm.bmHeight; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> rawBgra(static_cast<size_t>(bm.bmWidth) * bm.bmHeight * 4u, 0);
    HDC hdc = GetDC(nullptr);
    const int lines = GetDIBits(hdc, iconInfo.hbmColor, 0, static_cast<UINT>(bm.bmHeight), rawBgra.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);

    if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
    if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
    DestroyIcon(sfi.hIcon);

    if (lines <= 0) {
        return false;
    }

    outRect = AllocateIconSlot();
    const uint32_t slotX = static_cast<uint32_t>(std::round(outRect.u0 * kIconAtlasWidth)) - 1u;
    const uint32_t slotY = static_cast<uint32_t>(std::round(outRect.v0 * kIconAtlasHeight)) - 1u;

    for (uint32_t py = 0; py < kIconSlotSizePx; ++py) {
        const uint32_t srcY = std::min<uint32_t>(bm.bmHeight - 1, (py * static_cast<uint32_t>(bm.bmHeight)) / kIconSlotSizePx);
        for (uint32_t px = 0; px < kIconSlotSizePx; ++px) {
            const uint32_t srcX = std::min<uint32_t>(bm.bmWidth - 1, (px * static_cast<uint32_t>(bm.bmWidth)) / kIconSlotSizePx);
            const size_t srcIdx = (static_cast<size_t>(srcY) * bm.bmWidth + srcX) * 4u;
            const uint8_t b = rawBgra[srcIdx + 0];
            const uint8_t g = rawBgra[srcIdx + 1];
            const uint8_t r = rawBgra[srcIdx + 2];
            const uint8_t a = rawBgra[srcIdx + 3];

            const size_t dstIdx = ((static_cast<size_t>(slotY + py) * kIconAtlasWidth) + (slotX + px)) * 4u;
            // Premultiply RGB by Alpha
            iconPixels_[dstIdx + 0] = static_cast<uint8_t>((static_cast<uint32_t>(b) * a + 127u) / 255u);
            iconPixels_[dstIdx + 1] = static_cast<uint8_t>((static_cast<uint32_t>(g) * a + 127u) / 255u);
            iconPixels_[dstIdx + 2] = static_cast<uint8_t>((static_cast<uint32_t>(r) * a + 127u) / 255u);
            iconPixels_[dstIdx + 3] = a;
        }
    }

    iconDirty_ = true;
    return true;
}

void TextAndIconAtlas::PrecacheProfileIcons(const ConfigDocument& config) {
    for (const auto& action : config.actions) {
        (void)GetIconUv(action);
    }
}

AtlasUvRect TextAndIconAtlas::GetIconUv(const ActionDefinition& action) {
    if (!action.iconGlyph.empty()) {
        const std::wstring glyphKey = Utf8ToWide(action.iconGlyph);
        auto it = iconCache_.find(glyphKey);
        if (it != iconCache_.end()) {
            return it->second;
        }
    }

    if (!action.target.empty() &&
        (action.type == ActionType::LaunchApp || action.type == ActionType::OpenFile)) {
        const std::wstring targetW = Utf8ToWide(action.target);
        const std::wstring cacheKey = L"shell:" + targetW;
        auto it = iconCache_.find(cacheKey);
        if (it != iconCache_.end()) {
            return it->second;
        }
        AtlasUvRect extracted{};
        if (TryExtractShellIcon(targetW, extracted)) {
            iconCache_[cacheKey] = extracted;
            return extracted;
        }
    }

    switch (action.type) {
        case ActionType::OpenFolder:
            return iconCache_[L"folder"];
        case ActionType::OpenChildRing:
            return iconCache_[L"grid"];
        case ActionType::OpenKeychainSettings:
            return iconCache_[L"keychain"];
        case ActionType::WindowsAction:
            return iconCache_[L"settings"];
        case ActionType::SendShortcut:
            return iconCache_[L"shortcut"];
        default:
            return iconCache_[L"explorer"];
    }
}

const CachedTextEntry& TextAndIconAtlas::GetOrRasterizeText(
    const std::wstring& text,
    float fontSizeDip,
    uint32_t dpi,
    bool bold) {
    if (text.empty() || !dwriteFactory_) {
        return emptyTextEntry_;
    }

    const int quantizedFont = static_cast<int>(std::round(fontSizeDip * 10.0f));
    const std::wstring cacheKey =
        std::to_wstring(dpi) + L"|" + std::to_wstring(quantizedFont) + L"|" + (bold ? L"B|" : L"R|") + text;

    auto it = textCache_.find(cacheKey);
    if (it != textCache_.end()) {
        return it->second;
    }

    const float dpiScale = static_cast<float>(dpi) / 96.0f;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormat;
    HRESULT hr = dwriteFactory_->CreateTextFormat(
        L"Segoe UI Variable Text",
        nullptr,
        bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSizeDip,
        L"en-us",
        &textFormat);

    if (FAILED(hr) || !textFormat) {
        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSizeDip,
            L"en-us",
            &textFormat);
        if (FAILED(hr) || !textFormat) {
            return emptyTextEntry_;
        }
    }

    textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
    hr = dwriteFactory_->CreateTextLayout(
        text.c_str(),
        static_cast<UINT32>(text.size()),
        textFormat.Get(),
        320.0f,
        64.0f,
        &textLayout);
    if (FAILED(hr) || !textLayout) {
        return emptyTextEntry_;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(textLayout->GetMetrics(&metrics))) {
        return emptyTextEntry_;
    }

    const uint32_t boxWidthPx = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::ceil((metrics.widthIncludingTrailingWhitespace + 6.0f) * dpiScale)),
        8u,
        kGlyphAtlasWidth - 8u);
    const uint32_t boxHeightPx = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::ceil((metrics.height + 6.0f) * dpiScale)),
        8u,
        128u);

    if (glyphShelfX_ + boxWidthPx + 2 >= kGlyphAtlasWidth) {
        glyphShelfX_ = 2;
        glyphShelfY_ += glyphShelfRowHeight_ + 2;
        glyphShelfRowHeight_ = 0;
    }

    if (glyphShelfY_ + boxHeightPx + 2 >= kGlyphAtlasHeight) {
        // Atlas full: reset shelf
        ClearTextCache();
    }

    std::vector<uint8_t> tempBox(static_cast<size_t>(boxWidthPx) * boxHeightPx, 0);
    GrayscaleGlyphTextRenderer renderer(
        dwriteFactory_.Get(),
        dpiScale,
        tempBox.data(),
        boxWidthPx,
        boxHeightPx);

    textLayout->Draw(nullptr, &renderer, 2.0f, 2.0f);

    for (uint32_t y = 0; y < boxHeightPx; ++y) {
        std::memcpy(
            &glyphPixels_[static_cast<size_t>(glyphShelfY_ + y) * kGlyphAtlasWidth + glyphShelfX_],
            &tempBox[static_cast<size_t>(y) * boxWidthPx],
            boxWidthPx);
    }

    CachedTextEntry entry{};
    entry.uv.u0 = static_cast<float>(glyphShelfX_) / static_cast<float>(kGlyphAtlasWidth);
    entry.uv.v0 = static_cast<float>(glyphShelfY_) / static_cast<float>(kGlyphAtlasHeight);
    entry.uv.u1 = static_cast<float>(glyphShelfX_ + boxWidthPx) / static_cast<float>(kGlyphAtlasWidth);
    entry.uv.v1 = static_cast<float>(glyphShelfY_ + boxHeightPx) / static_cast<float>(kGlyphAtlasHeight);
    entry.uv.widthPx = static_cast<float>(boxWidthPx);
    entry.uv.heightPx = static_cast<float>(boxHeightPx);
    entry.widthPx = static_cast<float>(boxWidthPx);
    entry.heightPx = static_cast<float>(boxHeightPx);

    glyphShelfX_ += boxWidthPx + 2;
    glyphShelfRowHeight_ = std::max(glyphShelfRowHeight_, boxHeightPx);
    glyphDirty_ = true;

    auto [insIt, _] = textCache_.emplace(cacheKey, entry);
    return insIt->second;
}

void TextAndIconAtlas::AppendQuad(
    std::vector<QuadVertex>& vertices,
    float x,
    float y,
    float w,
    float h,
    const AtlasUvRect& uv,
    const Color4F& color,
    uint32_t mode,
    uint32_t flags) {
    const float x0 = x;
    const float y0 = y;
    const float x1 = x + w;
    const float y1 = y + h;

    const QuadVertex v00{{x0, y0}, {uv.u0, uv.v0}, {color.r, color.g, color.b, color.a}, mode, flags};
    const QuadVertex v10{{x1, y0}, {uv.u1, uv.v0}, {color.r, color.g, color.b, color.a}, mode, flags};
    const QuadVertex v11{{x1, y1}, {uv.u1, uv.v1}, {color.r, color.g, color.b, color.a}, mode, flags};
    const QuadVertex v01{{x0, y1}, {uv.u0, uv.v1}, {color.r, color.g, color.b, color.a}, mode, flags};

    vertices.push_back(v00);
    vertices.push_back(v10);
    vertices.push_back(v11);
    vertices.push_back(v00);
    vertices.push_back(v11);
    vertices.push_back(v01);
}

} // namespace keychain
