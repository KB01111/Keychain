#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <cstdint>
#include <cmath>
#include <string>
#include <string_view>
#include <memory>
#include <chrono>
#include <algorithm>

namespace keychain {

using Microsoft::WRL::ComPtr;

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kHalfPi = 1.57079632679489661923f;

struct Point2F {
    float x = 0.0f;
    float y = 0.0f;
};

struct Point2I {
    int32_t x = 0;
    int32_t y = 0;
};

struct RectI {
    int32_t left   = 0;
    int32_t top    = 0;
    int32_t right  = 0;
    int32_t bottom = 0;

    constexpr int32_t Width() const noexcept { return right - left; }
    constexpr int32_t Height() const noexcept { return bottom - top; }
    constexpr bool Contains(int32_t px, int32_t py) const noexcept {
        return px >= left && px < right && py >= top && py < bottom;
    }
};

struct Color4F {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// High-resolution monotonic clock abstraction (QPC in production, deterministic in tests)
class IClock {
public:
    virtual ~IClock() = default;
    virtual int64_t NowTicks() const noexcept = 0;
    virtual int64_t Frequency() const noexcept = 0;

    double TicksToSeconds(int64_t deltaTicks) const noexcept {
        const int64_t freq = Frequency();
        return (freq > 0) ? (static_cast<double>(deltaTicks) / static_cast<double>(freq)) : 0.0;
    }

    double TicksToMilliseconds(int64_t deltaTicks) const noexcept {
        return TicksToSeconds(deltaTicks) * 1000.0;
    }

    double TicksToMicroseconds(int64_t deltaTicks) const noexcept {
        return TicksToSeconds(deltaTicks) * 1000000.0;
    }

    int64_t NowMicroseconds() const noexcept {
        return static_cast<int64_t>(TicksToMicroseconds(NowTicks()));
    }
};

class SystemQpcClock final : public IClock {
public:
    SystemQpcClock() noexcept {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        frequency_ = (freq.QuadPart > 0) ? freq.QuadPart : 10000000LL;
    }

    int64_t NowTicks() const noexcept override {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return counter.QuadPart;
    }

    int64_t Frequency() const noexcept override {
        return frequency_;
    }

private:
    int64_t frequency_ = 10000000LL;
};

class DeterministicTestClock final : public IClock {
public:
    explicit DeterministicTestClock(int64_t frequency = 10000000LL, int64_t initialTicks = 10000000LL) noexcept
        : frequency_(frequency), currentTicks_(initialTicks) {}

    int64_t NowTicks() const noexcept override { return currentTicks_; }
    int64_t Frequency() const noexcept override { return frequency_; }

    void AdvanceSeconds(double seconds) noexcept {
        currentTicks_ += static_cast<int64_t>(seconds * static_cast<double>(frequency_));
    }

    void AdvanceMilliseconds(double ms) noexcept {
        AdvanceSeconds(ms * 0.001);
    }

    void SetTicks(int64_t ticks) noexcept { currentTicks_ = ticks; }

private:
    int64_t frequency_;
    int64_t currentTicks_;
};

// RAII wrapper for Win32 HANDLE
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle_(h) {}
    ~UniqueHandle() noexcept { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.Release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    HANDLE Get() const noexcept { return handle_; }
    bool IsValid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return IsValid(); }

    HANDLE Release() noexcept {
        HANDLE h = handle_;
        handle_ = nullptr;
        return h;
    }

    void Reset(HANDLE newHandle = nullptr) noexcept {
        if (IsValid()) {
            CloseHandle(handle_);
        }
        handle_ = newHandle;
    }

private:
    HANDLE handle_ = nullptr;
};

inline std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

inline std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

inline float DipToPhysicalPx(float dip, uint32_t dpi) noexcept {
    return dip * (static_cast<float>(dpi) / 96.0f);
}

inline float PhysicalPxToDip(float px, uint32_t dpi) noexcept {
    return (dpi > 0) ? (px * 96.0f / static_cast<float>(dpi)) : px;
}

inline float WrapAngleMinusPiToPi(float rad) noexcept {
    float r = std::fmod(rad + kPi, kTwoPi);
    if (r < 0.0f) r += kTwoPi;
    return r - kPi;
}

inline float WrapAngleZeroToTwoPi(float rad) noexcept {
    float r = std::fmod(rad, kTwoPi);
    if (r < 0.0f) r += kTwoPi;
    return r;
}

inline float ShortestAngularDelta(float fromRad, float toRad) noexcept {
    return WrapAngleMinusPiToPi(toRad - fromRad);
}

inline Color4F ParseHexColor(std::string_view hex, Color4F fallback = {0.36f, 0.78f, 1.0f, 1.0f}) noexcept {
    if (hex.empty()) return fallback;
    if (hex.front() == '#') hex.remove_prefix(1);
    if (hex.size() != 6 && hex.size() != 8) return fallback;
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto byteAt = [&](size_t offset, uint8_t& out) -> bool {
        int hi = hexVal(hex[offset]);
        int lo = hexVal(hex[offset + 1]);
        if (hi < 0 || lo < 0) return false;
        out = static_cast<uint8_t>((hi << 4) | lo);
        return true;
    };
    uint8_t r = 0, g = 0, b = 0, a = 255;
    if (!byteAt(0, r) || !byteAt(2, g) || !byteAt(4, b)) return fallback;
    if (hex.size() == 8 && !byteAt(6, a)) return fallback;
    return Color4F{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
}

} // namespace keychain
