#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <cstdio>

#include <objbase.h>
#include "nlohmann/json.hpp"

#include "actions/action_dispatcher.h"
#include "animation/spring_animator.h"
#include "config/config_schema.h"
#include "diagnostics/diagnostics_manager.h"
#include "gesture/gesture_state_machine.h"
#include "graphics/d3d12_overlay_renderer.h"
#include "input/input_manager.h"
#include "ipc/ipc_server.h"
#include "layout/radial_layout.h"

#include <windows.h>
#include <psapi.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

using namespace keychain;

constexpr RectI kWorkArea{0, 0, 1920, 1080};
constexpr uint32_t kDpi = 96;
constexpr Point2I kAnchor{960, 540};
const HWND kFakeForeground = reinterpret_cast<HWND>(0x1234);
const HWND kOtherForeground = reinterpret_cast<HWND>(0x5678);

using json = nlohmann::json;

std::filesystem::path TempTestDirectory(const char* tag) {
    wchar_t tempRoot[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempRoot);
    auto dir = std::filesystem::path(tempRoot) / (std::string("keychain_tests_") + tag);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::shared_ptr<const ConfigDocument> DefaultTestConfig() {
    return std::make_shared<const ConfigDocument>(ConfigStore::CreateDefaultConfig());
}

// Copies the default config and remaps root-ring sector 0 to the given action id.
std::shared_ptr<const ConfigDocument> ConfigWithSector0Action(const char* actionId) {
    ConfigDocument doc = ConfigStore::CreateDefaultConfig();
    doc.rings[0].sectorActionIds[0] = actionId;
    return std::make_shared<const ConfigDocument>(std::move(doc));
}

RadialLayout MirrorLayout(const ConfigDocument& cfg) {
    return RadialLayout::Compute(
        kAnchor, kWorkArea, kDpi, cfg.rings[0].sectorCount,
        cfg.preferences.ringDiameterDip, cfg.preferences.deadZoneRadiusDip, cfg.preferences.sectorGapDip);
}

Point2I CursorAtSector(const ConfigDocument& cfg, int sector) {
    const RadialLayout layout = MirrorLayout(cfg);
    const Point2F p = layout.sectors[static_cast<size_t>(sector)].screenIconPx;
    return Point2I{static_cast<int32_t>(p.x), static_cast<int32_t>(p.y)};
}

// Drives the gesture machine with deterministic timing and a fixed desktop geometry.
struct GestureHarness {
    std::shared_ptr<DeterministicTestClock> clock = std::make_shared<DeterministicTestClock>();
    std::shared_ptr<const ConfigDocument> config;
    GestureStateMachine sm;

    explicit GestureHarness(std::shared_ptr<const ConfigDocument> cfg)
        : config(std::move(cfg))
        , sm(clock, [](const ActionDefinition& a) { return ActionDispatcher::IsActionTargetAvailable(a); }) {
        sm.SetConfigSnapshotProvider([this]() { return config; });
    }

    GestureStepResult WinDown(uint32_t vk = VK_LWIN, bool capsToggle = false) {
        return sm.OnKeyEvent(vk, true, false, capsToggle, kAnchor, kWorkArea, kDpi, kFakeForeground);
    }

    GestureStepResult WinUp(uint32_t vk = VK_LWIN, bool capsToggle = false, HWND fg = kFakeForeground) {
        return sm.OnKeyEvent(vk, false, false, capsToggle, kAnchor, kWorkArea, kDpi, fg);
    }

    GestureStepResult CapsDown(bool capsToggle = false) {
        return sm.OnKeyEvent(VK_CAPITAL, true, false, capsToggle, kAnchor, kWorkArea, kDpi, kFakeForeground);
    }

    GestureStepResult CapsUp(bool capsToggle = false, Point2I cursor = kAnchor, HWND fg = kFakeForeground) {
        return sm.OnKeyEvent(VK_CAPITAL, false, false, capsToggle, cursor, kWorkArea, kDpi, fg);
    }

    GestureStepResult EscapeDown() {
        return sm.OnKeyEvent(VK_ESCAPE, true, false, false, kAnchor, kWorkArea, kDpi, kFakeForeground);
    }

    void Activate(bool capsToggle = false) {
        WinDown(VK_LWIN, capsToggle);
        CapsDown(capsToggle);
    }

    void SettleClosing() {
        clock->AdvanceMilliseconds(250.0);
        sm.AdvanceTime(clock->NowTicks(), kFakeForeground);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// [Section 3] Activation ordering, Caps ownership, suppression, commit
// ---------------------------------------------------------------------------
TEST_CASE("Gesture: Win-then-Caps activates, Caps-first does not; commit is exactly-once") {
    const bool capsStates[] = {false, true};
    const uint32_t winKeys[] = {VK_LWIN, VK_RWIN};

    for (const bool initialCaps : capsStates) {
        for (const uint32_t winVk : winKeys) {
            GestureHarness h(DefaultTestConfig());

            // Caps-down before Win-down: ordinary Caps Lock behavior, no activation.
            const GestureStepResult rCapsFirst = h.CapsDown(initialCaps);
            CHECK_FALSE(rCapsFirst.consumeInput);
            CHECK(h.sm.GetState() == GestureState::Idle);

            const GestureStepResult rWin = h.WinDown(winVk, initialCaps);
            CHECK(h.sm.GetState() == GestureState::Idle);

            // The physical ordinary Caps press completes (key-up) before the
            // recognized chord begins; hardware always delivers this sequence.
            const GestureStepResult rCapsFirstUp = h.CapsUp(initialCaps);
            CHECK_FALSE(rCapsFirstUp.consumeInput);
            CHECK(h.sm.GetState() == GestureState::Idle);

            // Win then Caps: recognized gesture opens the ring and consumes Caps.
            const GestureStepResult rCaps = h.CapsDown(initialCaps);
            CHECK(rCaps.consumeInput);
            CHECK(rCaps.stateChanged);
            CHECK(h.sm.GetState() == GestureState::Opening);
            CHECK(h.sm.GetCurrentGenerationId() > 0);
            CHECK(h.sm.IsCapsOwnedByGesture());
            CHECK(h.sm.GetPreexistingCapsToggleState() == initialCaps);

            const uint64_t gen = h.sm.GetCurrentGenerationId();

            // Caps repeats are suppressed without changing generation.
            const GestureStepResult rRepeat =
                h.sm.OnKeyEvent(VK_CAPITAL, true, true, initialCaps, kAnchor, kWorkArea, kDpi, kFakeForeground);
            CHECK(rRepeat.consumeInput);
            CHECK(h.sm.GetCurrentGenerationId() == gen);
            CHECK(h.sm.GetState() == GestureState::Opening);

            // Selection with the pointer.
            const Point2I cursor = CursorAtSector(*h.config, 0);
            const GestureStepResult rMove = h.sm.OnPointerMove(cursor);
            CHECK(rMove.selectionChanged);
            CHECK(h.sm.GetSelectedSector() == 0);
            CHECK((h.sm.GetState() == GestureState::Opening || h.sm.GetState() == GestureState::Active));

            // Caps-up commits exactly one action using the latest pointer position.
            const GestureStepResult rCommit = h.CapsUp(initialCaps, cursor);
            CHECK(rCommit.consumeInput);
            REQUIRE(rCommit.immediateCommit.has_value());
            CHECK(rCommit.immediateCommit->action.id == "act_documents");
            CHECK(rCommit.immediateCommit->generationId == gen);
            CHECK(rCommit.immediateCommit->configSnapshot != nullptr);
            CHECK(h.sm.GetState() == GestureState::Closing);

            // No double commit: Win-up and any further Caps-up dispatch nothing.
            const GestureStepResult rWinUp = h.WinUp(winVk, initialCaps);
            CHECK_FALSE(rWinUp.immediateCommit.has_value());
            const GestureStepResult rCapsUp2 = h.CapsUp(initialCaps);
            // The gesture consumed exactly one matching Caps-up (the commit).
            // A further Caps-up is an ordinary key event and must pass through.
            CHECK_FALSE(rCapsUp2.consumeInput);
            CHECK_FALSE(rCapsUp2.immediateCommit.has_value());
            CHECK_FALSE(rCapsUp2.readyDeferredCommit.has_value());
        }
    }
}

// ---------------------------------------------------------------------------
// [Section 3] Cancellation paths
// ---------------------------------------------------------------------------
TEST_CASE("Gesture: Win released first cancels; subsequent Caps-up is consumed without commit") {
    GestureHarness h(DefaultTestConfig());
    h.Activate();
    h.sm.OnPointerMove(CursorAtSector(*h.config, 0));

    const GestureStepResult rWinUp = h.WinUp(VK_LWIN);
    CHECK(rWinUp.cancelReason == CancelReason::WinReleasedFirst);
    CHECK(h.sm.GetState() == GestureState::Closing);

    const GestureStepResult rCapsUp = h.CapsUp();
    CHECK(rCapsUp.consumeInput);
    CHECK_FALSE(rCapsUp.immediateCommit.has_value());
}

TEST_CASE("Gesture: center/dead-zone release cancels") {
    GestureHarness h(DefaultTestConfig());
    h.Activate();
    h.sm.OnPointerMove(CursorAtSector(*h.config, 2));

    const GestureStepResult rCenter = h.sm.OnPointerMove(kAnchor);
    CHECK(h.sm.GetSelectedSector() == -1);

    const GestureStepResult rCapsUp = h.CapsUp(false, kAnchor);
    CHECK(rCapsUp.cancelReason == CancelReason::CenterRelease);
    CHECK_FALSE(rCapsUp.immediateCommit.has_value());
}

TEST_CASE("Gesture: Escape cancels and is consumed") {
    GestureHarness h(DefaultTestConfig());
    h.Activate();
    h.sm.OnPointerMove(CursorAtSector(*h.config, 1));

    const GestureStepResult rEsc = h.EscapeDown();
    CHECK(rEsc.consumeInput);
    CHECK(rEsc.cancelReason == CancelReason::EscapeKey);
    CHECK(h.sm.GetState() == GestureState::Closing);
}

TEST_CASE("Gesture: session lock / desktop loss cancels") {
    GestureHarness h(DefaultTestConfig());
    h.Activate();

    const GestureStepResult r = h.sm.OnSessionOrDesktopLost();
    CHECK(r.cancelReason == CancelReason::SessionLockedOrDesktopLost);
    // System-loss cancels skip the closing animation by design: instant, safe cleanup.
    CHECK(h.sm.GetState() == GestureState::Idle);
}

TEST_CASE("Gesture: queue overflow cancels safely") {
    GestureHarness h(DefaultTestConfig());
    h.Activate();

    const GestureStepResult r = h.sm.OnQueueOverflow();
    CHECK(r.cancelReason == CancelReason::QueueOverflow);
    // Queue overflow fails safely and instantly; no animation is attempted.
    CHECK(h.sm.GetState() == GestureState::Idle);
}

// ---------------------------------------------------------------------------
// [Section 3/7] Child-ring navigation: dwell, right-click back, root right-click
// ---------------------------------------------------------------------------
TEST_CASE("Gesture: child ring dwell navigation and right-click back") {
    const auto config = ConfigWithSector0Action("act_child_tools");
    const Point2I childSectorCursor = CursorAtSector(*config, 0);

    SUBCASE("release before dwell cancels instead of executing") {
        GestureHarness h(config);
        h.Activate();
        h.sm.OnPointerMove(childSectorCursor);
        h.clock->AdvanceMilliseconds(100.0);
        h.sm.AdvanceTime(h.clock->NowTicks(), kFakeForeground);
        CHECK_FALSE(h.sm.IsInChildRing());

        const GestureStepResult rCapsUp = h.CapsUp(false, childSectorCursor);
        CHECK(rCapsUp.cancelReason == CancelReason::ChildRingLinkNotEntered);
        CHECK_FALSE(rCapsUp.immediateCommit.has_value());
    }

    SUBCASE("250ms hover dwell enters child ring; right-click returns to root") {
        GestureHarness h(config);
        h.Activate();
        h.sm.OnPointerMove(childSectorCursor);

        h.clock->AdvanceMilliseconds(260.0);
        const GestureStepResult rDwell = h.sm.AdvanceTime(h.clock->NowTicks(), kFakeForeground);
        CHECK(rDwell.navigatedChildRing);
        CHECK(h.sm.IsInChildRing());
        REQUIRE(h.sm.GetActiveRing() != nullptr);
        CHECK(h.sm.GetActiveRing()->id == "ring_quick_tools");

        // Right-click inside child ring returns to root (consumed, no cancel).
        const GestureStepResult rBack = h.sm.OnMouseButton(true, true, childSectorCursor);
        CHECK(rBack.consumeInput);
        CHECK(rBack.returnedToRootRing);
        CHECK_FALSE(h.sm.IsInChildRing());
        REQUIRE(h.sm.GetActiveRing() != nullptr);
        CHECK(h.sm.GetActiveRing()->id == "ring_root_default");
        CHECK(h.sm.GetState() != GestureState::Idle);
    }

    SUBCASE("right-click at root cancels") {
        GestureHarness h(config);
        h.Activate();

        const GestureStepResult rRight = h.sm.OnMouseButton(true, true, CursorAtSector(*config, 0));
        CHECK(rRight.cancelReason == CancelReason::RightClickRoot);
        CHECK_FALSE(rRight.immediateCommit.has_value());
    }
}

// ---------------------------------------------------------------------------
// [Section 4] Deferred SendShortcut: modifier-release wait, foreground revalidation
// ---------------------------------------------------------------------------
TEST_CASE("Gesture: deferred SendShortcut commits only after activation modifiers release") {
    const auto config = ConfigWithSector0Action("act_copy_chord");
    const Point2I shortcutCursor = CursorAtSector(*config, 0);

    SUBCASE("Caps-up with Win held defers; Win-up with same foreground releases") {
        GestureHarness h(config);
        h.Activate();
        h.sm.OnPointerMove(shortcutCursor);

        const GestureStepResult rCapsUp = h.CapsUp(false, shortcutCursor);
        CHECK_FALSE(rCapsUp.immediateCommit.has_value());
        CHECK(h.sm.GetPendingActionState() == PendingActionState::WaitingForModifierRelease);

        const GestureStepResult rWinUp = h.WinUp(VK_LWIN, false, kFakeForeground);
        REQUIRE(rWinUp.readyDeferredCommit.has_value());
        CHECK(rWinUp.readyDeferredCommit->action.id == "act_copy_chord");
        // The machine transfers dispatch responsibility the moment it hands the
        // committed shortcut back, so the observable state is Dispatched.
        CHECK(h.sm.GetPendingActionState() == PendingActionState::Dispatched);
        CHECK_FALSE(rWinUp.readyDeferredCommit->configSnapshot == nullptr);
    }

    SUBCASE("foreground change before injection cancels") {
        GestureHarness h(config);
        h.Activate();
        h.sm.OnPointerMove(shortcutCursor);
        h.CapsUp(false, shortcutCursor);

        const GestureStepResult rWinUp = h.WinUp(VK_LWIN, false, kOtherForeground);
        CHECK(rWinUp.cancelReason == CancelReason::ForegroundChangedBeforeShortcut);
        CHECK_FALSE(rWinUp.readyDeferredCommit.has_value());
        CHECK(h.sm.GetPendingActionState() == PendingActionState::Cancelled);
    }

    SUBCASE("bounded modifier-release timeout cancels") {
        GestureHarness h(config);
        h.Activate();
        h.sm.OnPointerMove(shortcutCursor);
        h.CapsUp(false, shortcutCursor);

        h.clock->AdvanceMilliseconds(1600.0);
        const GestureStepResult r = h.sm.AdvanceTime(h.clock->NowTicks(), kFakeForeground);
        CHECK(r.cancelReason == CancelReason::ShortcutModifierReleaseTimeout);
        CHECK_FALSE(r.readyDeferredCommit.has_value());
    }
}

// ---------------------------------------------------------------------------
// [Section 3] Immutable configuration snapshot across an active gesture
// ---------------------------------------------------------------------------
TEST_CASE("Gesture: active gesture retains its config revision across profile changes") {
    GestureHarness h(DefaultTestConfig());
    const auto configV2 = ConfigWithSector0Action("act_notepad");

    h.Activate();
    h.sm.OnPointerMove(CursorAtSector(*h.config, 0));

    // Configuration changes mid-gesture (new revision becomes provider result).
    h.config = configV2;

    const GestureStepResult rCommit = h.CapsUp(false, CursorAtSector(*h.config, 0));
    REQUIRE(rCommit.immediateCommit.has_value());
    CHECK(rCommit.immediateCommit->action.id == "act_documents"); // V1 snapshot, not V2
    REQUIRE(rCommit.immediateCommit->configSnapshot != nullptr);
    CHECK(rCommit.immediateCommit->configSnapshot->rings[0].sectorActionIds[0] == "act_documents");
}

// ---------------------------------------------------------------------------
// [Section 3] BoundedInputQueue: coalescing and fail-safe overflow
// ---------------------------------------------------------------------------
TEST_CASE("BoundedInputQueue: coalesces pointer moves and reports overflow") {
    BoundedInputQueue<4> queue;

    InputQueueEvent moveA{};
    moveA.type = InputEventType::PointerMove;
    moveA.cursorScreenPx = {100, 100};
    moveA.receiptQpc = 100;
    CHECK(queue.Push(moveA));

    InputQueueEvent moveB{};
    moveB.type = InputEventType::PointerMove;
    moveB.cursorScreenPx = {250, 260};
    moveB.receiptQpc = 101;
    CHECK(queue.Push(moveB));

    InputQueueEvent out{};
    REQUIRE(queue.Pop(out));
    CHECK(out.type == InputEventType::PointerMove);
    CHECK(out.cursorScreenPx.x == 250);
    CHECK(out.cursorScreenPx.y == 260);
    CHECK(out.receiptQpc == 101);
    CHECK(queue.Size() == 0);

    InputQueueEvent key{};
    key.type = InputEventType::KeyEvent;
    for (int i = 0; i < 4; ++i) {
        CHECK(queue.Push(key));
    }
    // Queue is full; a critical event overflows safely.
    CHECK_FALSE(queue.Push(key));

    InputQueueEvent overflow{};
    REQUIRE(queue.Pop(overflow));
    CHECK(overflow.type == InputEventType::QueueOverflow);

    // Queue is usable again after overflow drain.
    CHECK(queue.Push(key));
}

// ---------------------------------------------------------------------------
// [Section 7] RadialLayout: sectors, dead zone, flick, edge fans, negative coords
// ---------------------------------------------------------------------------
TEST_CASE("RadialLayout: six sectors, dead zone, per-sector hit testing, flick continuity") {
    const RadialLayout layout =
        RadialLayout::Compute(kAnchor, kWorkArea, kDpi, 6);

    CHECK(layout.sectorCount == 6);
    REQUIRE(layout.sectors.size() == 6);
    CHECK(layout.anchorScreenPx.x == kAnchor.x);
    CHECK(layout.anchorScreenPx.y == kAnchor.y);

    const HitTestResult centerHit = layout.HitTest(kAnchor);
    CHECK(centerHit.inDeadZone);
    CHECK(centerHit.selectedSector == -1);

    for (size_t i = 0; i < layout.sectors.size(); ++i) {
        const Point2F sp = layout.sectors[i].screenIconPx;
        const HitTestResult hit = layout.HitTest({static_cast<int32_t>(sp.x), static_cast<int32_t>(sp.y)});
        CHECK_FALSE(hit.inDeadZone);
        CHECK(hit.selectedSector == static_cast<int>(i));
    }

    // Quick flick far beyond the outer radius stays valid (angle-based selection).
    const HitTestResult flick = layout.HitTest({960, 50});
    CHECK(flick.beyondOuter);
    CHECK(flick.selectedSector == 0);
}

TEST_CASE("RadialLayout: eight sectors supported") {
    const RadialLayout layout = RadialLayout::Compute(kAnchor, kWorkArea, kDpi, 8);
    CHECK(layout.sectorCount == 8);
    REQUIRE(layout.sectors.size() == 8);
    for (size_t i = 0; i < layout.sectors.size(); ++i) {
        const Point2F sp = layout.sectors[i].screenIconPx;
        const HitTestResult hit = layout.HitTest({static_cast<int32_t>(sp.x), static_cast<int32_t>(sp.y)});
        CHECK(hit.selectedSector == static_cast<int>(i));
    }
}

TEST_CASE("RadialLayout: edge and corner fans keep actions reachable without moving the anchor") {
    struct FanCase {
        Point2I anchor;
        FanMode expected;
    };
    const FanCase cases[] = {
        {{1910, 540}, FanMode::HalfRight},
        {{10, 540}, FanMode::HalfLeft},
        {{960, 10}, FanMode::HalfTop},
        {{960, 1070}, FanMode::HalfBottom},
        {{10, 10}, FanMode::QuarterTopLeft},
        {{1910, 10}, FanMode::QuarterTopRight},
        {{10, 1070}, FanMode::QuarterBottomLeft},
        {{1910, 1070}, FanMode::QuarterBottomRight},
    };

    for (const auto& c : cases) {
        const RadialLayout layout = RadialLayout::Compute(c.anchor, kWorkArea, kDpi, 6);
        CHECK(layout.fanMode == c.expected);
        CHECK(layout.anchorScreenPx.x == c.anchor.x);
        CHECK(layout.anchorScreenPx.y == c.anchor.y);

        // Every sector icon stays inside the monitor work area.
        for (const SectorGeometry& sec : layout.sectors) {
            CHECK(kWorkArea.Contains(static_cast<int32_t>(sec.screenIconPx.x), static_cast<int32_t>(sec.screenIconPx.y)));
        }

        // Center label position stays inside the work area too.
        CHECK(kWorkArea.Contains(
            static_cast<int32_t>(layout.centerLabelPosPx.x), static_cast<int32_t>(layout.centerLabelPosPx.y)));

        for (size_t i = 0; i < layout.sectors.size(); ++i) {
            const Point2F sp = layout.sectors[i].screenIconPx;
            const HitTestResult hit = layout.HitTest({static_cast<int32_t>(sp.x), static_cast<int32_t>(sp.y)});
            CHECK(hit.selectedSector == static_cast<int>(i));
        }
    }
}

TEST_CASE("RadialLayout: negative desktop coordinates and non-96 DPI") {
    const RectI leftMonitorWorkArea{-1920, -200, 0, 880};
    const Point2I anchor{-960, 340};
    const RadialLayout layout = RadialLayout::Compute(anchor, leftMonitorWorkArea, 144, 6);

    CHECK(layout.dpi == 144);
    CHECK(layout.anchorScreenPx.x == anchor.x);
    CHECK(layout.anchorScreenPx.y == anchor.y);

    const HitTestResult centerHit = layout.HitTest(anchor);
    CHECK(centerHit.inDeadZone);

    for (size_t i = 0; i < layout.sectors.size(); ++i) {
        const Point2F sp = layout.sectors[i].screenIconPx;
        CHECK(sp.x < 0.0f); // entirely on the left monitor
        const HitTestResult hit = layout.HitTest({static_cast<int32_t>(sp.x), static_cast<int32_t>(sp.y)});
        CHECK(hit.selectedSector == static_cast<int>(i));
    }
}

// ---------------------------------------------------------------------------
// [Section 8] Spring animation stability across simulated refresh rates
// ---------------------------------------------------------------------------
TEST_CASE("RingAnimationController: stable and settles at 60/144/240/360/500 Hz") {
    const RadialLayout layout = RadialLayout::Compute(kAnchor, kWorkArea, kDpi, 6);
    const int refreshRates[] = {60, 144, 240, 360, 500};

    for (const int hz : refreshRates) {
        auto clock = std::make_shared<DeterministicTestClock>();
        RingAnimationController anim(clock);
        anim.SetMotionPreferences("Standard", 1.0f, false);
        anim.OnGestureStateChanged(GestureState::Opening, false, -1, layout);

        const double frameMs = 1000.0 / static_cast<double>(hz);
        const int totalFrames = hz * 2;
        RingVisualState vis{};
        bool settled = false;

        for (int f = 0; f < totalFrames; ++f) {
            clock->AdvanceMilliseconds(frameMs);
            vis = anim.Update(clock->NowTicks());
            CHECK(std::isfinite(vis.openScale));
            CHECK(std::isfinite(vis.masterOpacity));
            CHECK(vis.openScale <= 1.08f);
            CHECK(vis.masterOpacity >= -0.001f);
            CHECK(vis.masterOpacity <= 1.001f);
            if (anim.IsSettled()) {
                settled = true;
                break;
            }
        }

        CHECK(settled);
        CHECK(vis.isVisible);
        CHECK(vis.openScale == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(vis.masterOpacity == doctest::Approx(1.0f).epsilon(0.02));

        // Closing with a committed action settles back to hidden.
        anim.OnGestureStateChanged(GestureState::Closing, true, 0, layout);
        for (int f = 0; f < totalFrames; ++f) {
            clock->AdvanceMilliseconds(frameMs);
            vis = anim.Update(clock->NowTicks());
            if (anim.IsSettled()) {
                break;
            }
        }
        CHECK(anim.IsSettled());
        CHECK_FALSE(vis.isVisible);
    }
}

TEST_CASE("RingAnimationController: reduced motion snaps immediately") {
    auto clock = std::make_shared<DeterministicTestClock>();
    const RadialLayout layout = RadialLayout::Compute(kAnchor, kWorkArea, kDpi, 6);

    RingAnimationController anim(clock);
    anim.SetMotionPreferences("Standard", 1.0f, true);
    anim.OnGestureStateChanged(GestureState::Opening, false, -1, layout);

    clock->AdvanceMilliseconds(1.0);
    const RingVisualState vis = anim.Update(clock->NowTicks());
    CHECK(anim.IsSettled());
    CHECK(vis.openScale == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(vis.masterOpacity == doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("RingAnimationController: selection immediacy, shortest-path retarget, no reversal discontinuity") {
    const RadialLayout layout = RadialLayout::Compute(kAnchor, kWorkArea, kDpi, 6);

    // Phase 1: first visible response is immediate. With no highlight shown yet,
    // the very first selection snaps the highlight onto the sector angle [Section 8].
    {
        auto clock = std::make_shared<DeterministicTestClock>();
        RingAnimationController anim(clock);
        anim.SetMotionPreferences("Standard", 1.0f, false);
        anim.OnGestureStateChanged(GestureState::Opening, false, -1, layout);
        anim.OnSelectionChanged(3, layout);
        clock->AdvanceMilliseconds(1000.0 / 240.0);
        const RingVisualState vis = anim.Update(clock->NowTicks());
        CHECK(ShortestAngularDelta(vis.highlightAngleRad, layout.sectors[3].centerAngleRad) ==
              doctest::Approx(0.0f).epsilon(0.0005));
    }

    // Phase 2: with a settled visible highlight, retargets ease along the shortest
    // path with bounded per-frame travel and no discontinuity on rapid reversal.
    {
        auto clock = std::make_shared<DeterministicTestClock>();
        RingAnimationController anim(clock);
        anim.SetMotionPreferences("Standard", 1.0f, false);
        anim.OnGestureStateChanged(GestureState::Opening, false, 1, layout);
        for (int f = 0; f < 48; ++f) { // 200ms at 240Hz lets the highlight become fully visible
            clock->AdvanceMilliseconds(1000.0 / 240.0);
            anim.Update(clock->NowTicks());
        }
        REQUIRE(anim.GetCurrentVisualState().highlightIntensity > 0.5f);

        float prevAngle = anim.GetCurrentVisualState().highlightAngleRad;
        bool reachedTarget = false;
        bool reversedMidTravel = false;
        for (int f = 0; f < 140; ++f) {
            if (f == 20) {
                anim.OnSelectionChanged(5, layout); // far target, must take the shortest path
            } else if (f == 40) {
                reversedMidTravel = true;
                anim.OnSelectionChanged(1, layout); // rapid reversal while still traveling
            }
            clock->AdvanceMilliseconds(1000.0 / 240.0);
            const RingVisualState vis = anim.Update(clock->NowTicks());
            CHECK(std::isfinite(vis.highlightAngleRad));
            const float step = std::fabs(ShortestAngularDelta(prevAngle, vis.highlightAngleRad));
            const std::string stepMsg =
                std::string("per-frame angular travel too large: ") + std::to_string(step) +
                " (intensity " + std::to_string(anim.GetCurrentVisualState().highlightIntensity) + ")";
            CHECK_MESSAGE(step < 0.6f, stepMsg.c_str());
            if (reversedMidTravel &&
                std::fabs(ShortestAngularDelta(vis.highlightAngleRad, layout.sectors[1].centerAngleRad)) < 0.02f) {
                reachedTarget = true;
            }
            prevAngle = vis.highlightAngleRad;
        }
        CHECK(reachedTarget);
        CHECK(std::fabs(ShortestAngularDelta(anim.GetCurrentVisualState().highlightAngleRad,
                                              layout.sectors[1].centerAngleRad)) < 0.02f);
    }
}

// ---------------------------------------------------------------------------
// [Section 11] ConfigStore: defaults, validation, atomic save, recovery, import
// ---------------------------------------------------------------------------
TEST_CASE("ConfigStore: defaults load, validate, save atomically, and recover from corruption") {
    const auto dir = TempTestDirectory("configstore");
    ConfigStore store(dir);

    ValidationResult loadReport;
    const ConfigDocument doc = store.LoadOrRecover(&loadReport);
    CHECK(loadReport.valid);
    CHECK(doc.schemaVersion == kCurrentConfigSchemaVersion);
    CHECK(doc.actions.size() >= 6);
    REQUIRE(!doc.rings.empty());
    CHECK(doc.rings[0].id == "ring_root_default");
    CHECK(doc.rings[0].sectorActionIds.size() == 6);

    // Atomic save with a modified preference.
    ConfigDocument modified = doc;
    modified.preferences.ringDiameterDip = 410.0f;
    const ValidationResult saveResult = store.SaveAtomic(modified);
    CHECK(saveResult.valid);
    const auto snap = store.GetSnapshot();
    CHECK(snap->preferences.ringDiameterDip == 410.0f);
    CHECK(snap->revision > doc.revision);

    // Truncate the primary file; a fresh store must recover from last_good.
    {
        std::ofstream trunc(store.GetConfigFilePath(), std::ios::binary | std::ios::trunc);
        trunc << "{ \"schemaVersion\": 1, \"broken";
    }
    ConfigStore recovered(dir);
    const ConfigDocument doc2 = recovered.LoadOrRecover();
    CHECK(doc2.preferences.ringDiameterDip == 410.0f);

    // Invalid nesting depth: child ring sector pointing back at the root ring.
    ConfigDocument badNesting = doc2;
    ActionDefinition cycle{};
    cycle.id = "act_cycle";
    cycle.type = ActionType::OpenChildRing;
    cycle.childRingId = "ring_root_default";
    badNesting.actions.push_back(cycle);
    RadialRing* childRing = nullptr;
    for (auto& r : badNesting.rings) {
        if (r.isChildRing) {
            childRing = &r;
            break;
        }
    }
    REQUIRE(childRing != nullptr);
    childRing->sectorActionIds[0] = "act_cycle";
    const ValidationResult cycleValidation = ConfigStore::ValidateConfig(badNesting);
    CHECK_FALSE(cycleValidation.valid);

    // Bounded file size enforcement.
    CHECK(kMaxConfigFileBytes == 512 * 1024);
}

TEST_CASE("ConfigStore: imported executable actions stay unapproved until reviewed") {
    ConfigDocument doc = ConfigStore::CreateDefaultConfig();
    ActionDefinition app{};
    app.id = "act_imported_app";
    app.type = ActionType::LaunchApp;
    app.label = "Imported App";
    app.target = "C:\\Windows\\System32\\notepad.exe";
    doc.actions.push_back(app);
    doc.rings[0].sectorActionIds[1] = "act_imported_app";

    const std::string serialized = ConfigStore::SerializeToJson(doc, false);

    ConfigDocument imported;
    ValidationResult val;
    REQUIRE(ConfigStore::ImportConfigFromJson(serialized, imported, val));
    CHECK(val.valid);

    const ActionDefinition* importedApp = imported.FindAction("act_imported_app");
    REQUIRE(importedApp != nullptr);
    CHECK(importedApp->type == ActionType::LaunchApp);
    CHECK_FALSE(importedApp->approved); // unapproved until the user reviews and enables
}

// ---------------------------------------------------------------------------
// [Section 4] ActionDispatcher: chord parsing, dry-run execution, interpreter block
// ---------------------------------------------------------------------------
TEST_CASE("ActionDispatcher: chord parsing and safe execution rules") {
    const ParsedShortcutChord chord = ActionDispatcher::ParseShortcutChord("Ctrl+Shift+Esc");
    CHECK(chord.valid);
    REQUIRE(chord.modifierVks.size() == 2);
    CHECK(chord.primaryVk == VK_ESCAPE);

    CHECK_FALSE(ActionDispatcher::ParseShortcutChord("Not+A+Chord+").valid);

    ActionDispatcher dispatcher;
    dispatcher.SetDryRunForTests(true);

    CommittedAction ok{};
    ok.action = *ConfigStore::CreateDefaultConfig().FindAction("act_documents");
    ok.originalForeground = GetForegroundWindow();
    const ActionExecutionResult okResult = dispatcher.ExecuteSynchronously(ok);
    CHECK(okResult.status == ActionExecutionStatus::Succeeded);
    CHECK(okResult.actionId == "act_documents");

    CommittedAction blocked{};
    blocked.action.id = "act_cmd";
    blocked.action.type = ActionType::LaunchApp;
    blocked.action.label = "cmd";
    blocked.action.target = "C:\\Windows\\System32\\cmd.exe";
    blocked.originalForeground = GetForegroundWindow();
    const ActionExecutionResult blockedResult = dispatcher.ExecuteSynchronously(blocked);
    CHECK(blockedResult.status == ActionExecutionStatus::ShellInterpreterBlocked);

    CommittedAction disabled{};
    disabled.action = *ConfigStore::CreateDefaultConfig().FindAction("act_documents");
    disabled.action.enabled = false;
    disabled.originalForeground = GetForegroundWindow();
    const ActionExecutionResult disabledResult = dispatcher.ExecuteSynchronously(disabled);
    CHECK(disabledResult.status == ActionExecutionStatus::DisabledActionBlocked);
}

// ---------------------------------------------------------------------------
// [Section 11 / R12] IPC: named-pipe protocol round-trip
// ---------------------------------------------------------------------------
TEST_CASE("IPC: length-prefixed JSON round-trip over named pipe") {
    const std::wstring pipeName = IpcServer::GetDefaultPipeName() + L".Test";

    auto config = DefaultTestConfig();
    bool paused = false;
    bool settingsShown = false;

    IpcHandlers handlers{};
    handlers.onGetConfig = [&]() { return config; };
    handlers.onSetPaused = [&](bool p) { paused = p; };
    handlers.onIsPaused = [&]() { return paused; };
    handlers.onShowSettings = [&](std::string&) {
        settingsShown = true;
        return true;
    };

    IpcServer server(std::move(handlers), pipeName);
    REQUIRE(server.Start());

    // Wait for the pipe to exist.
    bool pipeReady = false;
    for (int i = 0; i < 40 && !pipeReady; ++i) {
        if (WaitNamedPipeW(pipeName.c_str(), 50)) {
            pipeReady = true;
        } else {
            Sleep(25);
        }
    }
    REQUIRE(pipeReady);

    std::string response;

    // GetConfig
    REQUIRE(IpcServer::SendRequest(
        pipeName,
        R"({"version":1,"requestId":"42","type":"GetConfig","payload":{}})",
        response));
    {
        const json resp = json::parse(response);
        CHECK(resp["requestId"] == "42");
        CHECK(resp["ok"] == true);
        CHECK(resp["type"] == "GetConfigResponse");
        CHECK(resp["payload"]["schemaVersion"] == kCurrentConfigSchemaVersion);
        CHECK(resp["payload"]["rings"].size() >= 2);
    }

    // Pause / Resume
    REQUIRE(IpcServer::SendRequest(
        pipeName,
        R"({"version":1,"requestId":"7","type":"Pause","payload":{}})",
        response));
    {
        const json resp = json::parse(response);
        CHECK(resp["ok"] == true);
        CHECK(paused);
    }
    REQUIRE(IpcServer::SendRequest(
        pipeName,
        R"({"version":1,"requestId":"8","type":"Resume","payload":{}})",
        response));
    {
        const json resp = json::parse(response);
        CHECK(resp["ok"] == true);
        CHECK_FALSE(paused);
    }

    // ShowSettings
    REQUIRE(IpcServer::SendRequest(
        pipeName,
        R"({"version":1,"requestId":"9","type":"ShowSettings","payload":{}})",
        response));
    {
        const json resp = json::parse(response);
        CHECK(resp["ok"] == true);
        CHECK(settingsShown);
    }

    // Unknown type is rejected
    REQUIRE(IpcServer::SendRequest(
        pipeName,
        R"({"version":1,"requestId":"10","type":"Bogus","payload":{}})",
        response));
    {
        const json resp = json::parse(response);
        CHECK(resp["ok"] == false);
        CHECK(resp["requestId"] == "10");
    }

    // Bad version is rejected
    REQUIRE(IpcServer::SendRequest(
        pipeName,
        R"({"version":99,"requestId":"11","type":"GetConfig","payload":{}})",
        response));
    {
        const json resp = json::parse(response);
        CHECK(resp["ok"] == false);
    }

    server.Stop();
}

TEST_CASE("IPC: direct JSON processing validates ApplyConfig round-trip") {
    auto config = DefaultTestConfig();
    uint64_t appliedRevision = 0;

    IpcHandlers handlers{};
    handlers.onGetConfig = [&]() { return config; };
    handlers.onApplyConfig = [&](const ConfigDocument& newDoc, uint64_t& outRev) {
        const ValidationResult vr = ConfigStore::ValidateConfig(newDoc);
        if (vr.valid) {
            appliedRevision = newDoc.revision + 1;
            outRev = appliedRevision;
        }
        return vr;
    };

    IpcServer server(std::move(handlers), IpcServer::GetDefaultPipeName() + L".DirectTest");

    const ConfigDocument doc = ConfigStore::CreateDefaultConfig();
    const std::string configJson = ConfigStore::SerializeToJson(doc, false);

    json req = {
        {"version", kIpcProtocolVersion},
        {"requestId", "50"},
        {"type", "ApplyConfig"},
        {"payload", {{"config", configJson}}}
    };

    const std::string response = server.ProcessRequestJson(req.dump());
    const json resp = json::parse(response);
    CHECK(resp["ok"] == true);
    CHECK(resp["payload"]["revision"].get<uint64_t>() == appliedRevision);

    // Malformed JSON is rejected without throwing.
    const std::string badResponse = server.ProcessRequestJson("this is not json");
    const json badResp = json::parse(badResponse);
    CHECK(badResp["ok"] == false);
}

// ---------------------------------------------------------------------------
// [Section 12] 1,000-cycle stress test with handle/memory growth checks
// ---------------------------------------------------------------------------
TEST_CASE("Stress: 1000 activation/cancel/commit cycles without resource growth") {
    GestureHarness h(DefaultTestConfig());
    const RadialLayout layout = MirrorLayout(*h.config);
    const Point2I commitCursor = CursorAtSector(*h.config, 0);

    auto animClock = std::make_shared<DeterministicTestClock>();
    RingAnimationController anim(animClock);
    anim.SetMotionPreferences("Standard", 1.0f, false);

    DWORD handlesBefore = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
    PROCESS_MEMORY_COUNTERS memBefore{};
    GetProcessMemoryInfo(GetCurrentProcess(), &memBefore, sizeof(memBefore));

    for (int i = 0; i < 1000; ++i) {
        h.Activate();
        h.sm.OnPointerMove(commitCursor);
        anim.OnGestureStateChanged(GestureState::Opening, false, -1, layout);

        if (i % 2 == 0) {
            const GestureStepResult rEsc = h.EscapeDown();
            CHECK(rEsc.cancelReason == CancelReason::EscapeKey);
            anim.OnGestureStateChanged(GestureState::Closing, false, -1, layout);
        } else {
            const GestureStepResult rCapsUp = h.CapsUp(false, commitCursor);
            REQUIRE(rCapsUp.immediateCommit.has_value());
            CHECK(rCapsUp.immediateCommit->action.id == "act_documents");
            anim.OnGestureStateChanged(GestureState::Closing, true, 0, layout);
        }

        // Drain closing back to Idle, then release remaining physical keys cleanly.
        h.SettleClosing();
        CHECK(h.sm.GetState() == GestureState::Idle);
        if (i % 2 == 0) {
            h.CapsUp(); // Escape path: Caps still physically held, must not commit
        }
        h.WinUp();
    }

    DWORD handlesAfter = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    PROCESS_MEMORY_COUNTERS memAfter{};
    GetProcessMemoryInfo(GetCurrentProcess(), &memAfter, sizeof(memAfter));

    const LONG handleDelta = static_cast<LONG>(handlesAfter) - static_cast<LONG>(handlesBefore);
    const SIZE_T privateDelta =
        (memAfter.WorkingSetSize > memBefore.WorkingSetSize)
            ? memAfter.WorkingSetSize - memBefore.WorkingSetSize
            : 0;

    CHECK(handleDelta <= 8);
    CHECK(privateDelta < 8u * 1024u * 1024u); // less than 8 MiB growth over 1000 cycles
}

// ---------------------------------------------------------------------------
// [Section 5/12] Real D3D12 + DirectComposition transparent frame
// ---------------------------------------------------------------------------
TEST_CASE("Renderer: real DX12 transparent composition frame renders and captures") {
    // The DirectWrite/WIC atlas initializes COM objects on this thread.
    REQUIRE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"KeychainRendererTestWindow";
    REQUIRE(RegisterClassExW(&wc) != 0);

    HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"KeychainRendererTest",
        WS_POPUP,
        100, 100, 480, 480,
        nullptr, nullptr, wc.hInstance, nullptr);
    REQUIRE(hwnd != nullptr);

    auto clock = std::make_shared<SystemQpcClock>();
    DiagnosticsManager diagnostics(clock);
    auto renderer = std::make_unique<D3D12OverlayRenderer>(*clock, diagnostics);

    if (!renderer->Initialize(hwnd, 480, 480)) {
        std::string reasons = "Renderer Initialize failed. Diagnostics:\n";
        for (const auto& e : diagnostics.CaptureSnapshot().recentEntries) {
            reasons += "[";
            reasons += e.category;
            reasons += "] ";
            reasons += e.message;
            reasons += "\n";
        }
        std::fprintf(stderr, "%s\n", reasons.c_str());
        std::fflush(stderr);
        CoUninitialize();
        FAIL("Renderer Initialize failed - diagnostics printed to stderr");
    }
    CHECK(renderer->PrewarmTransparentFrame());

    auto config = DefaultTestConfig();
    renderer->PrecacheConfigAssets(*config, kDpi);

    const RadialLayout layout = RadialLayout::Compute({340, 340}, {0, 0, 1920, 1080}, kDpi, 6);

    RingVisualState vis{};
    vis.isVisible = true;
    vis.openScale = 1.0f;
    vis.masterOpacity = 1.0f;
    vis.highlightIntensity = 1.0f;
    vis.highlightAngleRad = layout.sectors[0].centerAngleRad;
    vis.sectorLiftDip[0] = 4.0f;

    OverlayRenderParams params{};
    params.layout = layout;
    params.visual = vis;
    params.config = config.get();
    params.activeRingId = "ring_root_default";
    params.selectedSector = 0;
    params.sectorAvailable.assign(6, true);

    const FrameTimingResult fr = renderer->RenderFrame(params);
    CHECK(fr.rendered);
    CHECK_FALSE(fr.deviceLost);
    CHECK(fr.cpuWorkMs >= 0.0);

    renderer->WaitForGpuIdle();

    const auto pngPath = TempTestDirectory("renderer") / "overlay_capture.png";
    CHECK(renderer->CaptureCurrentBackBufferToPng(pngPath.wstring()));
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(pngPath, ec);
        CHECK_FALSE(ec);
        CHECK(size > 1000);
    }

    renderer->Shutdown();
    // Destroy COM-owning objects while COM is still initialized on this thread.
    renderer.reset();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
}
