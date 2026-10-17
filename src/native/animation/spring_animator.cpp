#include "animation/spring_animator.h"
#include <cmath>
#include <algorithm>

namespace keychain {

SpringValue::SpringValue(float initialValue, SpringConfig cfg) noexcept
    : config_(cfg)
    , value_(initialValue)
    , target_(initialValue)
    , velocity_(0.0f)
    , settled_(true) {}

void SpringValue::SnapTo(float val) noexcept {
    value_                   = val;
    target_                  = val;
    velocity_                = 0.0f;
    elapsedSinceRetargetSec_ = 0.0;
    settled_                 = true;
}

void SpringValue::SetTarget(float newTarget, float minBound, float maxBound) noexcept {
    minBound_ = minBound;
    maxBound_ = maxBound;
    if (std::abs(newTarget - target_) > 1e-6f) {
        // Velocity-preserving retargeting: keep velocity_ intact!
        target_                  = newTarget;
        elapsedSinceRetargetSec_ = 0.0;
        settled_                 = false;
    }
}

void SpringValue::SetTargetAngleShortestPath(float targetAngleRad) noexcept {
    // Rebase current value into [-PI, PI] and adjust target via shortest angular delta
    const float wrappedCurrent = WrapAngleMinusPiToPi(value_);
    const float shortestDelta  = ShortestAngularDelta(wrappedCurrent, targetAngleRad);
    value_                     = wrappedCurrent;
    const float adjustedTarget = wrappedCurrent + shortestDelta;
    SetTarget(adjustedTarget, -kTwoPi * 2.0f, kTwoPi * 2.0f);
}

void SpringValue::Step(double dtSeconds, bool reducedMotion) noexcept {
    if (settled_) {
        return;
    }

    if (reducedMotion || dtSeconds <= 0.0 || config_.nominalDurationMs <= 1.0f) {
        SnapTo(target_);
        return;
    }

    // Clamp large frame gaps (e.g. debugger pause or sleep) to prevent numerical explosion
    const double clampedDt = std::min(dtSeconds, 0.1);
    elapsedSinceRetargetSec_ += clampedDt;

    // Derive natural frequency omega0 so 98% settling occurs in nominalDurationMs
    const double durationSec = std::max(static_cast<double>(config_.nominalDurationMs) * 0.001, 0.010);
    const double zeta        = std::clamp(static_cast<double>(config_.dampingRatio), 0.4, 1.5);
    const double omega0      = 4.6 / (durationSec * std::min(zeta, 1.0));

    // Sub-step at <= 1ms for unconditional stability across 60/144/240/360/500 Hz and irregular dt
    const int steps = std::max(1, static_cast<int>(std::ceil(clampedDt / 0.001)));
    const double h  = clampedDt / static_cast<double>(steps);

    double x = static_cast<double>(value_);
    double v = static_cast<double>(velocity_);
    const double tgt = static_cast<double>(target_);

    for (int i = 0; i < steps; ++i) {
        // Implicit Euler / Symplectic integration of x'' + 2*zeta*omega0*x' + omega0^2*(x - tgt) = 0
        const double denom = 1.0 + 2.0 * zeta * omega0 * h + (omega0 * omega0) * (h * h);
        const double nextX = (x + h * (v + 2.0 * zeta * omega0 * x + (omega0 * omega0) * h * tgt)) / denom;
        const double nextV = (v - (omega0 * omega0) * h * (x - tgt)) / denom;
        x = nextX;
        v = nextV;
    }

    value_    = static_cast<float>(x);
    velocity_ = static_cast<float>(v);

    // Enforce bounded overshoot
    if (value_ < minBound_) {
        value_ = minBound_;
        if (velocity_ < 0.0f) velocity_ = 0.0f;
    } else if (value_ > maxBound_) {
        value_ = maxBound_;
        if (velocity_ > 0.0f) velocity_ = 0.0f;
    }

    // Stall recovery and rest detection
    const float posErr = std::abs(value_ - target_);
    const float velMag = std::abs(velocity_);
    const double maxStallSec = durationSec * 2.4;

    if ((posErr <= config_.restPosEpsilon && velMag <= config_.restVelEpsilon) ||
        elapsedSinceRetargetSec_ >= maxStallSec) {
        SnapTo(target_);
    }
}

RingAnimationController::RingAnimationController(std::shared_ptr<IClock> clock)
    : clock_(std::move(clock))
    , openScaleSpring_(0.88f, SpringConfig{140.0f, 0.86f, 0.02f, 0.0008f, 0.005f})
    , opacitySpring_(0.0f, SpringConfig{110.0f, 0.95f, 0.0f, 0.001f, 0.005f})
    , highlightAngleSpring_(-kHalfPi, SpringConfig{70.0f, 0.90f, 0.02f, 0.001f, 0.01f})
    , highlightIntensitySpring_(0.0f, SpringConfig{70.0f, 0.95f, 0.0f, 0.002f, 0.01f}) {
    if (!clock_) {
        clock_ = std::make_shared<SystemQpcClock>();
    }
    for (auto& s : sectorLiftSprings_) {
        s = SpringValue(0.0f, SpringConfig{70.0f, 0.88f, 0.4f, 0.01f, 0.05f});
    }
}

void RingAnimationController::SetMotionPreferences(
    std::string_view preset,
    float speedMultiplier,
    bool reducedMotion
) noexcept {
    speedMultiplier_ = std::clamp(speedMultiplier, 0.25f, 4.0f);
    reducedMotion_   = reducedMotion;

    float damping = 0.86f;
    float presetScale = 1.0f;
    if (preset == "Snappy") {
        damping = 0.92f;
        presetScale = 0.78f;
    } else if (preset == "Gentle") {
        damping = 0.82f;
        presetScale = 1.22f;
    }

    const float timeScale = presetScale / speedMultiplier_;
    openScaleSpring_.Configure(SpringConfig{140.0f * timeScale, damping, 0.02f, 0.0008f, 0.005f});
    opacitySpring_.Configure(SpringConfig{110.0f * timeScale, 0.95f, 0.0f, 0.001f, 0.005f});
    highlightAngleSpring_.Configure(SpringConfig{70.0f * timeScale, damping, 0.02f, 0.001f, 0.01f});
    highlightIntensitySpring_.Configure(SpringConfig{70.0f * timeScale, 0.95f, 0.0f, 0.002f, 0.01f});
    for (auto& s : sectorLiftSprings_) {
        s.Configure(SpringConfig{70.0f * timeScale, damping, 0.4f, 0.01f, 0.05f});
    }
}

void RingAnimationController::OnGestureStateChanged(
    GestureState newState,
    bool committed,
    int selectedSector,
    const RadialLayout& layout
) noexcept {
    const GestureState prevState = trackedState_;
    trackedState_ = newState;

    if (newState == GestureState::Opening) {
        closingElapsedSec_ = 0.0;
        closingCommitted_  = false;
        // If starting from Idle, start scale at 0.88; if retargeting from Closing, preserve current scale & velocity!
        if (prevState == GestureState::Idle || prevState == GestureState::Suspended) {
            if (reducedMotion_) {
                openScaleSpring_.SnapTo(1.0f);
                opacitySpring_.SnapTo(1.0f);
            } else {
                openScaleSpring_.SnapTo(0.88f);
                opacitySpring_.SnapTo(0.25f); // Immediate first-frame visibility without waiting for long reveal
                openScaleSpring_.SetTarget(1.0f, 0.85f, 1.02f);
                opacitySpring_.SetTarget(1.0f, 0.0f, 1.0f);
            }
        } else {
            openScaleSpring_.SetTarget(1.0f, 0.85f, 1.02f);
            opacitySpring_.SetTarget(1.0f, 0.0f, 1.0f);
        }
        OnSelectionChanged(selectedSector, layout);
    } else if (newState == GestureState::Active) {
        openScaleSpring_.SetTarget(1.0f, 0.85f, 1.02f);
        opacitySpring_.SetTarget(1.0f, 0.0f, 1.0f);
        OnSelectionChanged(selectedSector, layout);
    } else if (newState == GestureState::Closing) {
        closingCommitted_  = committed;
        closingElapsedSec_ = 0.0;
        if (reducedMotion_) {
            openScaleSpring_.SnapTo(committed ? 1.02f : 0.92f);
            opacitySpring_.SnapTo(0.0f);
            for (auto& s : sectorLiftSprings_) s.SnapTo(0.0f);
        } else {
            const float closeMs = (committed ? 90.0f : 80.0f) / speedMultiplier_;
            openScaleSpring_.Configure(SpringConfig{closeMs, 0.95f, 0.01f, 0.001f, 0.005f});
            opacitySpring_.Configure(SpringConfig{closeMs, 0.98f, 0.0f, 0.001f, 0.005f});
            openScaleSpring_.SetTarget(committed ? 1.03f : 0.92f, 0.85f, 1.05f);
            opacitySpring_.SetTarget(0.0f, 0.0f, 1.0f);
            if (!committed) {
                for (auto& s : sectorLiftSprings_) s.SetTarget(0.0f, 0.0f, 6.0f);
                highlightIntensitySpring_.SetTarget(0.0f, 0.0f, 1.0f);
            }
        }
    } else if (newState == GestureState::Idle || newState == GestureState::Suspended) {
        openScaleSpring_.SnapTo(0.88f);
        opacitySpring_.SnapTo(0.0f);
        highlightIntensitySpring_.SnapTo(0.0f);
        for (auto& s : sectorLiftSprings_) s.SnapTo(0.0f);
        closingElapsedSec_ = 0.0;
        currentVisual_.isVisible = false;
        currentVisual_.isSettled = true;
    }
}

void RingAnimationController::OnSelectionChanged(
    int selectedSector,
    const RadialLayout& layout
) noexcept {
    constexpr float kTargetLiftDip = 4.2f; // 3-5 DIP lift per specification

    for (size_t i = 0; i < sectorLiftSprings_.size(); ++i) {
        const float tgt = (static_cast<int>(i) == selectedSector) ? kTargetLiftDip : 0.0f;
        if (reducedMotion_) {
            sectorLiftSprings_[i].SnapTo(tgt);
        } else {
            sectorLiftSprings_[i].SetTarget(tgt, 0.0f, 5.5f);
        }
    }

    if (selectedSector >= 0 && static_cast<size_t>(selectedSector) < layout.sectors.size()) {
        const float secAngle = layout.sectors[static_cast<size_t>(selectedSector)].centerAngleRad;
        if (reducedMotion_ || highlightIntensitySpring_.GetValue() < 0.05f) {
            highlightAngleSpring_.SnapTo(WrapAngleMinusPiToPi(secAngle));
        } else {
            highlightAngleSpring_.SetTargetAngleShortestPath(secAngle);
        }
        if (reducedMotion_) {
            highlightIntensitySpring_.SnapTo(1.0f);
        } else {
            highlightIntensitySpring_.SetTarget(1.0f, 0.0f, 1.0f);
        }
    } else {
        if (reducedMotion_) {
            highlightIntensitySpring_.SnapTo(0.0f);
        } else {
            highlightIntensitySpring_.SetTarget(0.0f, 0.0f, 1.0f);
        }
    }
}

RingVisualState RingAnimationController::Update(int64_t nowTicks) noexcept {
    double dtSec = 0.0;
    if (hasLastTicks_) {
        dtSec = std::max(0.0, clock_->TicksToSeconds(nowTicks - lastUpdateTicks_));
    }
    lastUpdateTicks_ = nowTicks;
    hasLastTicks_    = true;

    if (trackedState_ == GestureState::Idle || trackedState_ == GestureState::Suspended) {
        currentVisual_           = RingVisualState{};
        currentVisual_.isVisible = false;
        currentVisual_.isSettled = true;
        return currentVisual_;
    }

    openScaleSpring_.Step(dtSec, reducedMotion_);
    opacitySpring_.Step(dtSec, reducedMotion_);
    highlightAngleSpring_.Step(dtSec, reducedMotion_);
    highlightIntensitySpring_.Step(dtSec, reducedMotion_);

    bool allLiftsSettled = true;
    for (size_t i = 0; i < sectorLiftSprings_.size(); ++i) {
        sectorLiftSprings_[i].Step(dtSec, reducedMotion_);
        currentVisual_.sectorLiftDip[i] = sectorLiftSprings_[i].GetValue();
        if (!sectorLiftSprings_[i].IsSettled()) {
            allLiftsSettled = false;
        }
    }

    if (trackedState_ == GestureState::Closing) {
        closingElapsedSec_ += dtSec;
        const double nominalCloseSec = (closingCommitted_ ? 0.090 : 0.080) / static_cast<double>(speedMultiplier_);
        const float progress = reducedMotion_
            ? 1.0f
            : static_cast<float>(std::clamp(closingElapsedSec_ / std::max(nominalCloseSec, 0.010), 0.0, 1.0));
        currentVisual_.confirmProgress = closingCommitted_ ? progress : 0.0f;
    } else {
        currentVisual_.confirmProgress = 0.0f;
    }

    currentVisual_.openScale          = openScaleSpring_.GetValue();
    currentVisual_.masterOpacity      = opacitySpring_.GetValue();
    currentVisual_.highlightAngleRad  = WrapAngleMinusPiToPi(highlightAngleSpring_.GetValue());
    currentVisual_.highlightIntensity = highlightIntensitySpring_.GetValue();
    currentVisual_.isVisible          = (currentVisual_.masterOpacity > 0.002f);

    currentVisual_.isSettled =
        openScaleSpring_.IsSettled() &&
        opacitySpring_.IsSettled() &&
        highlightAngleSpring_.IsSettled() &&
        highlightIntensitySpring_.IsSettled() &&
        allLiftsSettled &&
        (trackedState_ != GestureState::Closing || currentVisual_.masterOpacity <= 0.002f);

    return currentVisual_;
}

} // namespace keychain
