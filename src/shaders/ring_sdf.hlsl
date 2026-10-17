// Keychain 1.0 - Procedural SDF Midnight Glass Ring Shader (SM 6.0)
// Renders bounded radial sectors, center hub, soft local drop shadow,
// simulated optical glass gradients, fine specular rim, and non-color selection geometry.
// Output is strictly premultiplied alpha (DXGI_ALPHA_MODE_PREMULTIPLIED).

cbuffer RingConstants : register(b0)
{
    float2 ViewportSizePx;       // Total overlay surface dimensions in physical pixels
    float2 RingCenterPx;         // Logical anchor point in physical pixels within the overlay
    float  InnerRadiusPx;        // Inner sector radius (outside dead zone)
    float  OuterRadiusPx;        // Base outer sector radius
    float  DeadZoneRadiusPx;     // Center dead-zone radius
    float  SectorGapPx;          // Half-gap between adjacent sectors in pixels

    uint   SectorCount;          // 6 or 8
    int    SelectedSector;       // -1 if none, else [0..SectorCount-1]
    int    HoveredChildSector;   // Sector currently dwelling for child ring
    uint   Flags;                // Bit 0: HighContrast, Bit 1: ReducedTransparency, Bit 2: LightTheme, Bit 3: InChildRing, Bit 4: ToggleMode

    float  StartAngleRad;        // Fan start angle in radians
    float  ArcSpanRad;           // Fan total angular span (2*PI for full ring, PI for half fan, PI/2 for quarter fan)
    float  OpenScale;            // Spring scale (e.g. 0.88 -> 1.0)
    float  MasterOpacity;        // Overall alpha multiplier (0..1)

    float  HighlightAngleRad;    // Smoothed angular highlight center
    float  HighlightIntensity;   // 0..1 active highlight weight
    float  ConfirmProgress;      // 0..1 confirmation pulse progress
    float  DwellProgress;        // 0..1 child-ring hover dwell progress

    float4 BaseTintColor;        // rgb = glass base tint, a = base opacity
    float4 AccentColor;          // rgb = ice-blue or user accent, a = intensity
    float4 RimColor;             // rgb = specular rim color, a = rim opacity
    float4 ShadowColor;          // rgb = local shadow color, a = shadow opacity

    float4 SectorLiftPx[2];      // Up to 8 float sector radial lift values in pixels (packed as 2 float4s)
    uint4  SectorState[2];       // Up to 8 uint flags per sector: Bit 0 = Disabled, Bit 1 = IsChildRing, Bit 2 = Unapproved
};

struct VSOut
{
    float4 Position : SV_POSITION;
    float2 PixelPos : TEXCOORD0;
};

VSOut VSMain(uint vertexId : SV_VertexID)
{
    // Full-surface triangle strip (4 vertices for bounded overlay quad)
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VSOut o;
    o.Position = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    o.PixelPos = uv * ViewportSizePx;
    return o;
}

float GetSectorLift(uint idx)
{
    if (idx < 4)
        return SectorLiftPx[0][idx];
    else if (idx < 8)
        return SectorLiftPx[1][idx - 4];
    return 0.0f;
}

uint GetSectorFlags(uint idx)
{
    if (idx < 4)
        return SectorState[0][idx];
    else if (idx < 8)
        return SectorState[1][idx - 4];
    return 0u;
}

float WrapAngleZeroToTwoPi(float a)
{
    const float TWO_PI = 6.28318530718f;
    float r = fmod(a, TWO_PI);
    return (r < 0.0f) ? (r + TWO_PI) : r;
}

float ShortestAngleDiff(float a, float b)
{
    const float PI = 3.14159265359f;
    const float TWO_PI = 6.28318530718f;
    float d = fmod(a - b + PI, TWO_PI);
    if (d < 0.0f) d += TWO_PI;
    return d - PI;
}

float4 PSMain(VSOut input) : SV_TARGET
{
    const float PI = 3.14159265359f;
    const float TWO_PI = 6.28318530718f;

    bool isHighContrast = (Flags & 1u) != 0u;
    bool isReducedTrans = (Flags & 2u) != 0u;
    bool isLightTheme   = (Flags & 4u) != 0u;
    bool isInChildRing  = (Flags & 8u) != 0u;

    float scale = max(OpenScale, 0.001f);
    float2 rel = (input.PixelPos - RingCenterPx) / scale;
    float dist = length(rel);

    // Early out well outside maximum possible ring + shadow extent to guarantee pure zero transparent pixels
    float maxReach = OuterRadiusPx + 28.0f;
    if (dist > maxReach || MasterOpacity <= 0.001f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float angle = atan2(rel.y, rel.x);
    float relAngle = WrapAngleZeroToTwoPi(angle - StartAngleRad);
    bool isFullCircle = (ArcSpanRad >= TWO_PI - 0.01f);

    uint count = clamp(SectorCount, 1u, 8u);
    float sectorSpan = ArcSpanRad / float(count);

    // Determine sector index and angular distance to sector boundaries
    int sectorIdx = -1;
    float localAngle = 0.0f;
    float angularEdgeDistPx = -100.0f;

    if (isFullCircle || (relAngle >= 0.0f && relAngle <= ArcSpanRad))
    {
        uint rawIdx = min(uint(floor(relAngle / sectorSpan)), count - 1u);
        sectorIdx = int(rawIdx);
        float sectorCenterAngle = (float(rawIdx) + 0.5f) * sectorSpan;
        localAngle = abs(relAngle - sectorCenterAngle);
        float halfSpan = 0.5f * sectorSpan;
        // Convert angular margin to linear arc distance at current radius
        angularEdgeDistPx = ( localAngle - halfSpan ) * max(dist, InnerRadiusPx) + SectorGapPx;
    }
    else
    {
        // Outside fan span: compute angular distance to fan start/end
        float dStart = abs(ShortestAngleDiff(angle, StartAngleRad));
        float dEnd   = abs(ShortestAngleDiff(angle, StartAngleRad + ArcSpanRad));
        angularEdgeDistPx = min(dStart, dEnd) * max(dist, InnerRadiusPx) + SectorGapPx;
    }

    float lift = (sectorIdx >= 0) ? GetSectorLift(uint(sectorIdx)) : 0.0f;
    uint sFlags = (sectorIdx >= 0) ? GetSectorFlags(uint(sectorIdx)) : 0u;
    bool isDisabled  = (sFlags & 1u) != 0u;
    bool isChildLink = (sFlags & 2u) != 0u;
    bool isSelected  = (sectorIdx >= 0 && sectorIdx == SelectedSector);

    // Radial bounds with per-sector lift
    float secInnerR = InnerRadiusPx + lift * 0.35f;
    float secOuterR = OuterRadiusPx + lift;
    float radialDistPx = max(secInnerR - dist, dist - secOuterR);

    // Smooth chamfered intersection of radial and angular SDFs (rounded corners r=6px)
    float cornerR = 6.0f;
    float2 d2 = float2(radialDistPx + cornerR, angularEdgeDistPx + cornerR);
    float sectorSdf = length(max(d2, 0.0f)) + min(max(d2.x, d2.y), 0.0f) - cornerR;

    // Center hub SDF (inside dead-zone)
    float hubRadius = DeadZoneRadiusPx * 0.86f;
    float hubSdf = dist - hubRadius;

    // 1. Soft local shadow behind ring sectors and center hub (omitted in high contrast)
    float shadowAlpha = 0.0f;
    if (!isHighContrast)
    {
        float2 shadowRel = rel - float2(0.0f, 4.0f);
        float shadowDist = length(shadowRel);
        float shadowRadial = max(InnerRadiusPx - shadowDist, shadowDist - (OuterRadiusPx + 2.0f));
        float shadowRingSdf = max(shadowRadial, angularEdgeDistPx - 2.0f);
        float shadowHubSdf = shadowDist - hubRadius;
        float combinedShadowSdf = min(shadowRingSdf, shadowHubSdf);
        shadowAlpha = saturate(1.0f - smoothstep(-2.0f, 14.0f, combinedShadowSdf)) * ShadowColor.a;
    }

    // 2. Sector fill and optical glass shading
    float sectorCoverage = saturate(0.5f - sectorSdf);
    float4 finalPremul = float4(ShadowColor.rgb * shadowAlpha, shadowAlpha);

    if (sectorCoverage > 0.001f && sectorIdx >= 0)
    {
        float radialT = saturate((dist - secInnerR) / max(secOuterR - secInnerR, 1.0f));
        // Top-left directional lighting term for optical glass feel
        float2 normDir = normalize(rel + float2(0.0001f, 0.0001f));
        float lightGrad = saturate(dot(normDir, normalize(float2(-0.45f, -0.89f))) * 0.5f + 0.5f);

        float3 glassRgb = BaseTintColor.rgb;
        float glassAlpha = BaseTintColor.a;

        if (isReducedTrans || isHighContrast)
        {
            glassAlpha = 1.0f;
        }
        else
        {
            // Simulated optical glass depth gradient
            glassRgb += (isLightTheme ? float3(0.06f, 0.07f, 0.09f) : float3(0.045f, 0.055f, 0.075f)) * lightGrad;
            glassRgb += (isLightTheme ? -0.03f : 0.03f) * (1.0f - radialT);
        }

        // Smooth shortest-path angular highlight
        float angDiff = abs(ShortestAngleDiff(angle, HighlightAngleRad));
        float angGlow = exp(-angDiff * angDiff * 3.5f) * HighlightIntensity;

        if (isSelected && !isDisabled)
        {
            float selMix = isHighContrast ? 0.45f : 0.28f;
            glassRgb = lerp(glassRgb, AccentColor.rgb, selMix + 0.12f * radialT);
            glassAlpha = saturate(glassAlpha + 0.12f);

            // Confirmation ripple on commit
            if (ConfirmProgress > 0.001f)
            {
                float wave = 1.0f - abs(radialT - ConfirmProgress) * 4.0f;
                glassRgb += AccentColor.rgb * saturate(wave) * 0.35f;
            }
        }
        else if (!isDisabled && angGlow > 0.01f && !isHighContrast)
        {
            glassRgb = lerp(glassRgb, AccentColor.rgb, angGlow * 0.14f);
        }

        if (isDisabled)
        {
            glassAlpha *= 0.52f;
            glassRgb *= 0.65f;
        }

        // Fine specular rim & non-color-only selection border
        // Selected sectors get a thicker 2.6px outer/inner accent border + geometric outer notch
        float rimWidth = isSelected ? 2.4f : 1.15f;
        float rimFactor = saturate(1.0f - abs(sectorSdf + rimWidth * 0.5f) / (rimWidth * 0.65f));

        // Non-color-only selection indicator: crisp inner arc bar + outer tick mark on selected sector
        float nonColorIndicator = 0.0f;
        if (isSelected)
        {
            float innerBarDist = abs(dist - (secInnerR + 4.5f)) - 1.6f;
            if (localAngle < sectorSpan * 0.28f && innerBarDist < 1.0f)
            {
                nonColorIndicator = saturate(0.5f - innerBarDist);
            }
        }

        // Child-ring outer double-arc indicator & dwell progress arc
        float childArcIndicator = 0.0f;
        if (isChildLink)
        {
            float outerArcDist = abs(dist - (secOuterR - 5.5f)) - 1.2f;
            if (localAngle < sectorSpan * 0.22f && outerArcDist < 1.0f)
            {
                childArcIndicator = saturate(0.5f - outerArcDist);
            }
            if (sectorIdx == HoveredChildSector && DwellProgress > 0.01f)
            {
                float dwellSpan = sectorSpan * 0.35f * saturate(DwellProgress);
                if (localAngle <= dwellSpan && abs(dist - (secOuterR - 2.5f)) < 2.0f)
                {
                    childArcIndicator = 1.0f;
                }
            }
        }

        float3 rimRgb = isSelected ? lerp(RimColor.rgb, AccentColor.rgb, 0.75f) : (RimColor.rgb * (0.65f + 0.35f * lightGrad));
        float rimAlpha = isSelected ? 0.95f : (RimColor.a * (0.55f + 0.45f * lightGrad));
        if (isHighContrast)
        {
            rimRgb = isSelected ? AccentColor.rgb : RimColor.rgb;
            rimAlpha = 1.0f;
        }

        glassRgb = lerp(glassRgb, rimRgb, rimFactor * rimAlpha);
        float indicatorMix = max(nonColorIndicator, childArcIndicator * 0.85f);
        glassRgb = lerp(glassRgb, float3(0.96f, 0.98f, 1.0f), indicatorMix);
        glassAlpha = max(glassAlpha, max(rimFactor * rimAlpha, indicatorMix));

        float a = sectorCoverage * glassAlpha;
        float4 secPremul = float4(glassRgb * a, a);
        finalPremul = secPremul + finalPremul * (1.0f - secPremul.a);
    }

    // 3. Center hub medallion (displays active label, subring indicator, and cancel zone)
    float hubCoverage = saturate(0.5f - hubSdf);
    if (hubCoverage > 0.001f)
    {
        float3 hubRgb = BaseTintColor.rgb * 0.92f;
        float hubAlpha = (isReducedTrans || isHighContrast) ? 1.0f : saturate(BaseTintColor.a + 0.08f);
        float hubRim = saturate(1.0f - abs(hubSdf + 0.8f) / 1.1f);
        float3 hubRimRgb = isInChildRing ? AccentColor.rgb : RimColor.rgb;
        hubRgb = lerp(hubRgb, hubRimRgb, hubRim * 0.7f);
        float a = hubCoverage * hubAlpha;
        float4 hubPremul = float4(hubRgb * a, a);
        finalPremul = hubPremul + finalPremul * (1.0f - hubPremul.a);
    }

    return finalPremul * MasterOpacity;
}
