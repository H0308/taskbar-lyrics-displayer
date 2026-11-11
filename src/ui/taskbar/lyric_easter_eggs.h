#pragma once

#include "lyric/lyric_types.h"
#include "ui/taskbar/lyric_city_landmarks.h"
#include <d2d1_1.h>
#include <wrl/client.h>
#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

// 拥有关键词时间点和装饰轮廓；画刷和设备上下文仅在本次绘制期间借用。
class LyricEasterEggs {
public:
    // 仅清理布局和设备缓存，保留已触发动画。
    void clear();
    void resetPlayback(int64_t positionMs);
    void resetTriggerPosition(int64_t positionMs); // seek 重新定位触发游标，不改变动画进度。
    bool active() const;
    void draw(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
              const LyricLine& line, int lineIndex, D2D1_RECT_F viewport,
              int64_t positionMs, int64_t emitFromMs, bool playing);

private:
    enum class Kind {
        Cloud, Flowers, Rain, Snow, Wind, Stars, Sea, Butterflies,
        ChinaHongKongFlags, ChinaMacauFlags, City, Count
    };
    enum class Flag { China, HongKong, Macau, Count };
    static constexpr size_t kKindCount = static_cast<size_t>(Kind::Count);
    static constexpr int64_t kDurationMs = 2400;
    static float fade(float age);
    struct Cue {
        Kind kind;
        int64_t startMs;
        size_t textOffset;
        LyricCityLandmarks::City city = LyricCityLandmarks::City::Count;
    };
    struct Animation {
        uint64_t startedMs;
        uint32_t seed;
    };
    const std::vector<Cue>& cues(const LyricLine& line, int lineIndex);
    bool ensureShapes(ID2D1DeviceContext* target);
    bool ensureFlagBitmaps(ID2D1DeviceContext* target);
    void drawClouds(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                    D2D1_RECT_F viewport, float age, float opacity, uint32_t seed);
    void drawFlowers(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                     D2D1_RECT_F viewport, float age, float opacity, uint32_t seed);
    void drawRain(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                  D2D1_RECT_F viewport, float age, float opacity, uint32_t seed);
    void drawSnow(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                  D2D1_RECT_F viewport, float age, float opacity, uint32_t seed);
    void drawWind(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                  D2D1_RECT_F viewport, float age, float opacity, uint32_t seed);
    void drawStars(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                   D2D1_RECT_F viewport, float age, float opacity, uint32_t seed);
    void drawSea(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                 D2D1_RECT_F viewport, float age, float opacity, uint32_t seed);
    void drawButterflies(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                         D2D1_RECT_F viewport, float age, float opacity, uint32_t seed);
    void drawChinaRegionalFlags(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                                D2D1_RECT_F viewport, float age, float opacity, Flag regional);
    std::unordered_map<int, std::vector<Cue>> cues_;
    // 每类最多一个实例；布局缓存失效不影响已触发动画的独立时间轴。
    std::array<std::optional<Animation>, kKindCount> animations_{};
    std::array<int64_t, kKindCount> emittedThroughMs_ = [] {
        std::array<int64_t, kKindCount> positions;
        positions.fill(-1);
        return positions;
    }();
    LyricCityLandmarks cities_;
    std::array<std::optional<Animation>, LyricCityLandmarks::kCityCount> cityAnimations_{};
    std::array<int64_t, LyricCityLandmarks::kCityCount> cityEmittedThroughMs_ = [] {
        std::array<int64_t, LyricCityLandmarks::kCityCount> positions;
        positions.fill(-1);
        return positions;
    }();
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> cloud_, stem_, leaf_, petal_, tulip_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> sparkle_, wave_, butterflyWing_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> flagStar_, bauhiniaPetal_, bauhiniaVein_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> lotusPetal_, macauBridge_;
    std::array<Microsoft::WRL::ComPtr<ID2D1Bitmap>, static_cast<size_t>(Flag::Count)> flagBitmaps_;
};
