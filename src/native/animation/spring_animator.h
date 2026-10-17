#pragma once

#include "common/types.h"
#include "gesture/gesture_state_machine.h"
#include <array>
#include <memory>

namespace keychain {

struct SpringConfig {
    float nominalDurationMs = 140.0f;
    float dampingRatio      = 0.86f; // Slightly underdamped/critically damped with minimal overshoot
    float maxOvershoot      = 0.03f; // Max overshoot beyond target range
    float restPosEpsilon    = 0.001f;
    float restVelEpsilon    = 0.008f;
};

class SpringValue {
public:
    SpringValue() noexcept = default;
    explicit SpringValue(float initialValue, SpringConfig cfg = {}) noexcept;

    void Configure(SpringConfig cfg) noexcept { config_ = cfg; }
    void SnapTo(float val) noexcept;
    void SetTarget(float newTarget, float minBound = -1e6f, float maxBound = 1e6f) noexcept;
    void SetTargetAngleShortestPath(float targetAngleRad) noexcept;

    void Step(double dtSeconds, bool reducedMotion = false) noexcept;

    float GetValue() const noexcept { return value_; }
    float GetTarget() const noexcept { return target_; }
    float GetVelocity() const noexcept { return velocity_; }
    bool  IsSettled() const noexcept { return settled_; }

private:
    SpringConfig config_{};
    float  value_            = 0.0f;
    float  target_           = 0.0f;
    float  velocity_         = 0.0f;
    float  minBound_         = -1e6f;
    float  maxBound_         = 1e6f;
    double elapsedSinceRetargetSec_ = 0.0;
    bool   settled_          = true;
};

struct RingVisualState {
    float                openScale          = 1.0f;
    float                masterOpacity      = 0.0f;
    float                highlightAngleRad  = 0.0f;
    float                highlightIntensity = 0.0f;
    float                confirmProgress    = 0.0f;
    std::array<float, 8> sectorLiftDip      = {};
    bool                 isVisible          = false;
    bool                 isSettled          = true;
};

class RingAnimationController {
public:
    explicit RingAnimationController(std::shared_ptr<IClock> clock = std::make_shared<SystemQpcClock>());

    void SetMotionPreferences(std::string_view preset, float speedMultiplier, bool reducedMotion) noexcept;

    void OnGestureStateChanged(
        GestureState newState,
        bool committed,
        int selectedSector,
        const RadialLayout& layout
    ) noexcept;

    void OnSelectionChanged(
        int selectedSector,
        const RadialLayout& layout
    ) noexcept;

    RingVisualState Update(int64_t nowTicks) noexcept;
    const RingVisualState& GetCurrentVisualState() const noexcept { return currentVisual_; }
    bool IsSettled() const noexcept { return currentVisual_.isSettled; }

private:
    std::shared_ptr<IClock> clock_;
    int64_t lastUpdateTicks_ = 0;
    bool hasLastTicks_       = false;

    float speedMultiplier_   = 1.0f;
    bool  reducedMotion_     = false;
    bool  closingCommitted_  = false;
    GestureState trackedState_ = GestureState::Idle;

    SpringValue openScaleSpring_;
    SpringValue opacitySpring_;
    SpringValue highlightAngleSpring_;
    SpringValue highlightIntensitySpring_;
    std::array<SpringValue, 8> sectorLiftSprings_;

    double closingElapsedSec_ = 0.0;
    RingVisualState currentVisual_{};
};

} // namespace keychain
