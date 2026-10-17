// Keychain 1.0 - Glyph & Icon Atlas Premultiplied Quad Shader (SM 6.0)
// Renders DirectWrite grayscale glyph runs (R8_UNORM alpha mask) and cached
// WIC/Shell icons (B8G8R8A8_UNORM premultiplied) onto the transparent overlay.

cbuffer QuadConstants : register(b0)
{
    float2 ViewportSizePx;
    float2 RingCenterPx;
    float  OpenScale;
    float  MasterOpacity;
    float2 Padding;
};

Texture2D<float4> AtlasTexture : register(t0);
SamplerState      LinearSampler : register(s0);

struct VSIn
{
    float2 PosPx : POSITION;
    float2 UV    : TEXCOORD0;
    float4 Color : COLOR0;
    uint   Mode  : BLENDINDICES0; // 0 = Grayscale glyph alpha (R channel), 1 = Premultiplied RGBA icon, 2 = Solid UI badge
};

struct VSOut
{
    float4 Position : SV_POSITION;
    float2 UV       : TEXCOORD0;
    float4 Color    : COLOR0;
    nointerpolation uint Mode : BLENDINDICES0;
};

VSOut VSMain(VSIn input)
{
    // Scale quad positions relative to RingCenterPx so icons/labels track opening spring coherently
    float2 scaledPos = RingCenterPx + (input.PosPx - RingCenterPx) * OpenScale;
    float2 ndc = float2(
        (scaledPos.x / ViewportSizePx.x) * 2.0f - 1.0f,
        1.0f - (scaledPos.y / ViewportSizePx.y) * 2.0f
    );

    VSOut o;
    o.Position = float4(ndc, 0.0f, 1.0f);
    o.UV       = input.UV;
    o.Color    = input.Color;
    o.Mode     = input.Mode;
    return o;
}

float4 PSMain(VSOut input) : SV_TARGET
{
    if (MasterOpacity <= 0.001f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    if (input.Mode == 0u)
    {
        // Grayscale DirectWrite glyph coverage stored in R8/BGRA8 alpha
        float cov = AtlasTexture.Sample(LinearSampler, input.UV).r;
        // Apply slight gamma-correct contrast boost for crisp grayscale text over translucent glass
        cov = pow(saturate(cov), 0.85f);
        float a = cov * input.Color.a * MasterOpacity;
        return float4(input.Color.rgb * a, a);
    }
    else if (input.Mode == 1u)
    {
        // Premultiplied BGRA8 icon from WIC/Shell icon cache
        float4 tex = AtlasTexture.Sample(LinearSampler, input.UV);
        float tintA = input.Color.a * MasterOpacity;
        return float4(tex.rgb * input.Color.rgb * tintA, tex.a * tintA);
    }
    else
    {
        // Procedural crisp pill/badge quad
        float a = input.Color.a * MasterOpacity;
        return float4(input.Color.rgb * a, a);
    }
}
