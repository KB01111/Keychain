#pragma once

#include "common/types.h"
#include "config/config_schema.h"

#include <dwrite_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace keychain {

struct AtlasUvRect {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float widthPx = 0.0f;
    float heightPx = 0.0f;
};

struct CachedTextEntry {
    AtlasUvRect uv{};
    float widthPx = 0.0f;
    float heightPx = 0.0f;
};

struct QuadVertex {
    float posPx[2];
    float uv[2];
    float color[4];
    uint32_t mode;
    uint32_t flags;
};

// [Section 5, R15] DirectWrite shaping/layout and grayscale glyph-atlas path for labels;
// cached by font, DPI, and content. Plus procedural vector & shell icon atlas.
class TextAndIconAtlas {
public:
    static constexpr uint32_t kGlyphAtlasWidth = 1024;
    static constexpr uint32_t kGlyphAtlasHeight = 1024;
    static constexpr uint32_t kIconAtlasWidth = 512;
    static constexpr uint32_t kIconAtlasHeight = 512;
    static constexpr uint32_t kIconSlotSizePx = 64;

    TextAndIconAtlas() = default;
    ~TextAndIconAtlas() = default;

    bool Initialize();

    // Pre-warms and caches procedural and shell icons outside the critical gesture path
    void PrecacheProfileIcons(const ConfigDocument& config);

    // Shapes and rasterizes a grayscale label via IDWriteFactory2::CreateGlyphRunAnalysis
    // Returns cached UV rect in the R8_UNORM glyph atlas
    const CachedTextEntry& GetOrRasterizeText(
        const std::wstring& text,
        float fontSizeDip,
        uint32_t dpi,
        bool bold = false);

    // Returns UV rect in the RGBA8 premultiplied icon atlas
    AtlasUvRect GetIconUv(const ActionDefinition& action);

    const std::vector<uint8_t>& GlyphAtlasPixels() const noexcept { return glyphPixels_; }
    const std::vector<uint8_t>& IconAtlasPixels() const noexcept { return iconPixels_; }

    bool IsGlyphAtlasDirty() const noexcept { return glyphDirty_; }
    void ClearGlyphAtlasDirty() noexcept { glyphDirty_ = false; }

    bool IsIconAtlasDirty() const noexcept { return iconDirty_; }
    void ClearIconAtlasDirty() noexcept { iconDirty_ = false; }

    void ClearTextCache();

    // Appends 6 vertices (2 triangles) for a quad
    static void AppendQuad(
        std::vector<QuadVertex>& vertices,
        float x,
        float y,
        float w,
        float h,
        const AtlasUvRect& uv,
        const Color4F& color,
        uint32_t mode,
        uint32_t flags = 0);

private:
    void BuildProceduralVectorIcons();
    AtlasUvRect AllocateIconSlot();
    bool TryExtractShellIcon(const std::wstring& targetPath, AtlasUvRect& outRect);

    Microsoft::WRL::ComPtr<IDWriteFactory2> dwriteFactory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;

    std::vector<uint8_t> glyphPixels_;
    uint32_t glyphShelfX_ = 2;
    uint32_t glyphShelfY_ = 2;
    uint32_t glyphShelfRowHeight_ = 0;
    bool glyphDirty_ = false;

    std::vector<uint8_t> iconPixels_;
    uint32_t nextIconSlotIndex_ = 0;
    bool iconDirty_ = false;

    std::unordered_map<std::wstring, CachedTextEntry> textCache_;
    std::unordered_map<std::wstring, AtlasUvRect> iconCache_;
    CachedTextEntry emptyTextEntry_{};
};

} // namespace keychain
