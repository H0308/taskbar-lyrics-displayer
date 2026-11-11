#pragma once

#include <d2d1_1.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>

// 城市彩蛋的矢量资源；只借用本次绘制的上下文与画刷。
class LyricCityLandmarks {
public:
    enum class City { Shanghai, NewYork, Paris, Tokyo, London, Beijing,
                      Guangzhou, Wuhan, Sydney, Singapore, SanFrancisco, LosAngeles, Miami, Count };
    static constexpr size_t kCityCount = static_cast<size_t>(City::Count);
    void clear();
    void draw(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
              City city, D2D1_RECT_F slot, float opacity);

private:
    // 主轮廓、立面、装饰和细节线；按城市惰性创建，随设备缓存清理。
    using Artwork = std::array<Microsoft::WRL::ComPtr<ID2D1PathGeometry>, 4>;
    bool ensureArtwork(ID2D1DeviceContext* target, City city);
    bool ensureRockstarShapes(ID2D1Factory* factory);
    std::array<Artwork, kCityCount> artworks_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> rockstarMark_, rockstarStar_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> viceCityScript_;
};
