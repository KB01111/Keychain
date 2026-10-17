#include "layout/radial_layout.h"
#include <cmath>
#include <algorithm>

namespace keychain {

RadialLayout RadialLayout::Compute(
    Point2I anchorScreenPx,
    RectI monitorWorkAreaPx,
    uint32_t dpi,
    uint32_t sectorCount,
    float ringDiameterDip,
    float deadZoneRadiusDip,
    float sectorGapDip
) {
    RadialLayout layout;
    layout.anchorScreenPx    = anchorScreenPx;
    layout.monitorWorkAreaPx = monitorWorkAreaPx;
    layout.dpi               = (dpi > 0) ? dpi : 96u;
    layout.sectorCount       = (sectorCount == 8) ? 8u : 6u;

    const float clampedDiameterDip = std::clamp(ringDiameterDip, 240.0f, 640.0f);
    const float clampedDeadZoneDip = std::clamp(deadZoneRadiusDip, 20.0f, 100.0f);
    const float clampedGapDip      = std::clamp(sectorGapDip, 1.0f, 10.0f);

    layout.outerRadiusPx    = DipToPhysicalPx(clampedDiameterDip * 0.5f, layout.dpi);
    layout.deadZoneRadiusPx = DipToPhysicalPx(clampedDeadZoneDip, layout.dpi);
    layout.innerRadiusPx    = layout.deadZoneRadiusPx + DipToPhysicalPx(10.0f, layout.dpi);
    layout.sectorGapPx      = DipToPhysicalPx(clampedGapDip, layout.dpi);

    // Determine edge/corner proximity while keeping anchorScreenPx strictly as the logical center
    const float edgeThresholdPx = layout.outerRadiusPx * 0.78f;
    const bool hasValidWorkArea = (monitorWorkAreaPx.Width() > 0 && monitorWorkAreaPx.Height() > 0);

    bool nearLeft   = false;
    bool nearRight  = false;
    bool nearTop    = false;
    bool nearBottom = false;

    if (hasValidWorkArea) {
        nearLeft   = (static_cast<float>(anchorScreenPx.x - monitorWorkAreaPx.left)   < edgeThresholdPx);
        nearRight  = (static_cast<float>(monitorWorkAreaPx.right - anchorScreenPx.x)  < edgeThresholdPx);
        nearTop    = (static_cast<float>(anchorScreenPx.y - monitorWorkAreaPx.top)    < edgeThresholdPx);
        nearBottom = (static_cast<float>(monitorWorkAreaPx.bottom - anchorScreenPx.y) < edgeThresholdPx);
    }

    if (nearLeft && nearTop) {
        layout.fanMode       = FanMode::QuarterTopLeft;
        layout.startAngleRad = 0.0f;
        layout.arcSpanRad    = kHalfPi;
    } else if (nearRight && nearTop) {
        layout.fanMode       = FanMode::QuarterTopRight;
        layout.startAngleRad = kHalfPi;
        layout.arcSpanRad    = kHalfPi;
    } else if (nearRight && nearBottom) {
        layout.fanMode       = FanMode::QuarterBottomRight;
        layout.startAngleRad = -kPi;
        layout.arcSpanRad    = kHalfPi;
    } else if (nearLeft && nearBottom) {
        layout.fanMode       = FanMode::QuarterBottomLeft;
        layout.startAngleRad = -kHalfPi;
        layout.arcSpanRad    = kHalfPi;
    } else if (nearLeft) {
        layout.fanMode       = FanMode::HalfLeft;
        layout.startAngleRad = -kHalfPi;
        layout.arcSpanRad    = kPi;
    } else if (nearRight) {
        layout.fanMode       = FanMode::HalfRight;
        layout.startAngleRad = kHalfPi;
        layout.arcSpanRad    = kPi;
    } else if (nearTop) {
        layout.fanMode       = FanMode::HalfTop;
        layout.startAngleRad = 0.0f;
        layout.arcSpanRad    = kPi;
    } else if (nearBottom) {
        layout.fanMode       = FanMode::HalfBottom;
        layout.startAngleRad = -kPi;
        layout.arcSpanRad    = kPi;
    } else {
        layout.fanMode       = FanMode::FullRing;
        const float spanPerSector = kTwoPi / static_cast<float>(layout.sectorCount);
        // Center sector 0 at top (-PI/2)
        layout.startAngleRad = -kHalfPi - spanPerSector * 0.5f;
        layout.arcSpanRad    = kTwoPi;
    }

    // Bounded overlay window rect centered around anchorScreenPx with margin for lift & soft shadow
    const int32_t paddingPx = static_cast<int32_t>(std::ceil(DipToPhysicalPx(44.0f, layout.dpi)));
    const int32_t halfExtentPx = static_cast<int32_t>(std::ceil(layout.outerRadiusPx)) + paddingPx;

    layout.overlayBoundsPx = RectI{
        anchorScreenPx.x - halfExtentPx,
        anchorScreenPx.y - halfExtentPx,
        anchorScreenPx.x + halfExtentPx,
        anchorScreenPx.y + halfExtentPx
    };

    layout.anchorInOverlayPx = Point2F{
        static_cast<float>(anchorScreenPx.x - layout.overlayBoundsPx.left),
        static_cast<float>(anchorScreenPx.y - layout.overlayBoundsPx.top)
    };

    // Place center/selected-action label inside monitor work area
    float labelScreenX = static_cast<float>(anchorScreenPx.x);
    float labelScreenY = static_cast<float>(anchorScreenPx.y);
    const float labelMarginX = DipToPhysicalPx(90.0f, layout.dpi);
    const float labelMarginY = DipToPhysicalPx(36.0f, layout.dpi);

    if (hasValidWorkArea) {
        labelScreenX = std::clamp(
            labelScreenX,
            static_cast<float>(monitorWorkAreaPx.left) + labelMarginX,
            static_cast<float>(monitorWorkAreaPx.right) - labelMarginX
        );
        labelScreenY = std::clamp(
            labelScreenY,
            static_cast<float>(monitorWorkAreaPx.top) + labelMarginY,
            static_cast<float>(monitorWorkAreaPx.bottom) - labelMarginY
        );
    }

    layout.centerLabelPosPx = Point2F{
        labelScreenX - static_cast<float>(layout.overlayBoundsPx.left),
        labelScreenY - static_cast<float>(layout.overlayBoundsPx.top)
    };

    // Build sector geometries
    const float sectorSpan = layout.arcSpanRad / static_cast<float>(layout.sectorCount);
    const float midRadiusPx = (layout.innerRadiusPx + layout.outerRadiusPx) * 0.5f;

    layout.sectors.resize(layout.sectorCount);
    for (uint32_t i = 0; i < layout.sectorCount; ++i) {
        SectorGeometry& sec = layout.sectors[i];
        sec.index          = i;
        sec.startAngleRad  = layout.startAngleRad + static_cast<float>(i) * sectorSpan;
        sec.endAngleRad    = sec.startAngleRad + sectorSpan;
        sec.centerAngleRad = sec.startAngleRad + sectorSpan * 0.5f;

        const float cosA = std::cos(sec.centerAngleRad);
        const float sinA = std::sin(sec.centerAngleRad);
        sec.iconCenterPx = Point2F{
            layout.anchorInOverlayPx.x + cosA * midRadiusPx,
            layout.anchorInOverlayPx.y + sinA * midRadiusPx
        };
        sec.screenIconPx = Point2F{
            static_cast<float>(anchorScreenPx.x) + cosA * midRadiusPx,
            static_cast<float>(anchorScreenPx.y) + sinA * midRadiusPx
        };
    }

    return layout;
}

HitTestResult RadialLayout::HitTest(Point2I cursorScreenPx, int previousSelectedSector) const noexcept {
    HitTestResult result;
    const float dx = static_cast<float>(cursorScreenPx.x - anchorScreenPx.x);
    const float dy = static_cast<float>(cursorScreenPx.y - anchorScreenPx.y);
    result.distancePx = std::hypot(dx, dy);

    // Returning to the dead zone clears selection immediately
    if (result.distancePx <= deadZoneRadiusPx) {
        result.inDeadZone     = true;
        result.beyondOuter    = false;
        result.selectedSector = -1;
        result.angleRad       = 0.0f;
        return result;
    }

    result.inDeadZone  = false;
    result.beyondOuter = (result.distancePx > outerRadiusPx);
    result.angleRad    = std::atan2(dy, dx);

    const float sectorSpan = arcSpanRad / static_cast<float>(sectorCount);
    const bool isFullRing = (fanMode == FanMode::FullRing);

    // Check angular hysteresis around previousSelectedSector if valid
    if (previousSelectedSector >= 0 && static_cast<uint32_t>(previousSelectedSector) < sectorCount) {
        const auto& prevSec = sectors[static_cast<size_t>(previousSelectedSector)];
        const float deltaFromCenter = std::abs(ShortestAngularDelta(prevSec.centerAngleRad, result.angleRad));
        const float halfSpanWithHysteresis = (sectorSpan * 0.5f) + hysteresisRad;
        if (deltaFromCenter <= halfSpanWithHysteresis) {
            result.selectedSector = previousSelectedSector;
            return result;
        }
    }

    float relAngle = WrapAngleZeroToTwoPi(result.angleRad - startAngleRad);

    if (!isFullRing) {
        // For edge/corner half and quarter fans, allow slight angular margin at fan ends,
        // and reject cursor angles pointing behind the inward-facing fan.
        constexpr float kFanEndGraceRad = 0.26f; // ~15 degrees
        if (relAngle > arcSpanRad) {
            if (relAngle <= arcSpanRad + kFanEndGraceRad) {
                result.selectedSector = static_cast<int>(sectorCount - 1);
                return result;
            }
            if (relAngle >= kTwoPi - kFanEndGraceRad) {
                result.selectedSector = 0;
                return result;
            }
            result.selectedSector = -1;
            return result;
        }
    }

    // Deterministic tie-breaking via floor clamping
    uint32_t rawIdx = static_cast<uint32_t>(std::floor(relAngle / sectorSpan));
    if (rawIdx >= sectorCount) {
        rawIdx = sectorCount - 1;
    }
    result.selectedSector = static_cast<int>(rawIdx);
    return result;
}

} // namespace keychain
