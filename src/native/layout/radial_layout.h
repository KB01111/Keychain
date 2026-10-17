#pragma once

#include "common/types.h"
#include <vector>
#include <cstdint>

namespace keychain {

enum class FanMode {
    FullRing,
    HalfLeft,
    HalfRight,
    HalfTop,
    HalfBottom,
    QuarterTopLeft,
    QuarterTopRight,
    QuarterBottomLeft,
    QuarterBottomRight
};

struct SectorGeometry {
    uint32_t index          = 0;
    float    startAngleRad  = 0.0f;
    float    endAngleRad    = 0.0f;
    float    centerAngleRad = 0.0f;
    Point2F  iconCenterPx   = {}; // Relative to overlay window origin
    Point2F  screenIconPx   = {}; // In virtual desktop screen coordinates
};

struct HitTestResult {
    bool  inDeadZone     = true;
    bool  beyondOuter    = false;
    int   selectedSector = -1;
    float distancePx     = 0.0f;
    float angleRad       = 0.0f;
};

struct RadialLayout {
    Point2I  anchorScreenPx     = {}; // Logical gesture anchor in desktop screen coordinates (never moved)
    RectI    monitorWorkAreaPx  = {};
    RectI    overlayBoundsPx    = {}; // Bounded popup window rect in screen coordinates
    Point2F  anchorInOverlayPx  = {}; // Anchor position relative to overlayBoundsPx top-left
    Point2F  centerLabelPosPx   = {}; // Center/selected-action label position clamped inside work area

    uint32_t dpi                = 96;
    uint32_t sectorCount        = 6;
    FanMode  fanMode            = FanMode::FullRing;

    float    innerRadiusPx      = 0.0f;
    float    outerRadiusPx      = 0.0f;
    float    deadZoneRadiusPx   = 0.0f;
    float    sectorGapPx        = 0.0f;
    float    startAngleRad      = 0.0f;
    float    arcSpanRad         = kTwoPi;
    float    hysteresisRad      = 0.0610865f; // ~3.5 degrees

    std::vector<SectorGeometry> sectors;

    static RadialLayout Compute(
        Point2I anchorScreenPx,
        RectI monitorWorkAreaPx,
        uint32_t dpi,
        uint32_t sectorCount,
        float ringDiameterDip = 380.0f,
        float deadZoneRadiusDip = 44.0f,
        float sectorGapDip = 3.5f
    );

    HitTestResult HitTest(Point2I cursorScreenPx, int previousSelectedSector = -1) const noexcept;
};

} // namespace keychain
