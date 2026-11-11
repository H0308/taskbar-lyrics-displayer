#include "ui/taskbar/lyric_city_landmarks.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <utility>

namespace {
constexpr float kPi = 3.14159265358979323846f;

// 以 100 为基准高度构建多轮廓矢量，宽幅场景沿横轴展开。
class LandmarkPath {
public:
    explicit LandmarkPath(ID2D1Factory* factory) {
        if (SUCCEEDED(factory->CreatePathGeometry(geometry_.GetAddressOf())) &&
            SUCCEEDED(geometry_->Open(sink_.GetAddressOf())))
            sink_->SetFillMode(D2D1_FILL_MODE_WINDING);
    }
    void polygon(std::initializer_list<D2D1_POINT_2F> points, bool filled = true) {
        if (!sink_ || points.size() < 2)
            return;
        auto point = points.begin();
        sink_->BeginFigure(*point++, filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (; point != points.end(); ++point)
            sink_->AddLine(*point);
        sink_->EndFigure(filled ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    }
    void rect(float left, float top, float right, float bottom) {
        polygon({{left, top}, {right, top}, {right, bottom}, {left, bottom}});
    }
    void curve(D2D1_POINT_2F start, std::initializer_list<D2D1_BEZIER_SEGMENT> curves,
               bool filled = true) {
        if (!sink_)
            return;
        sink_->BeginFigure(start, filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (const auto& curve : curves)
            sink_->AddBezier(curve);
        sink_->EndFigure(filled ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    }
    void ellipse(float x, float y, float rx, float ry) {
        constexpr float k = 0.55228475f;
        curve({x + rx, y}, {
            {{x + rx, y + k * ry}, {x + k * rx, y + ry}, {x, y + ry}},
            {{x - k * rx, y + ry}, {x - rx, y + k * ry}, {x - rx, y}},
            {{x - rx, y - k * ry}, {x - k * rx, y - ry}, {x, y - ry}},
            {{x + k * rx, y - ry}, {x + rx, y - k * ry}, {x + rx, y}},
        });
    }
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> finish() {
        if (!sink_ || FAILED(sink_->Close()))
            return {};
        sink_.Reset();
        return std::move(geometry_);
    }

private:
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry_;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink_;
};

void latticeTower(LandmarkPath& body, LandmarkPath& light, LandmarkPath& lines, bool tokyo) {
    // 弯曲塔腿、底部拱洞、两级平台和逐层收窄的交叉桁架。
    const float cx = tokyo ? 39.0f : 50.0f;
    const float half = tokyo ? 27.0f : 33.0f;
    body.curve({cx - half, 94}, {
        {{cx - 17, 72}, {cx - 7, 45}, {cx - 2, 14}},
        {{cx - 1, 11}, {cx + 1, 11}, {cx + 2, 14}},
        {{cx + 7, 45}, {cx + 17, 72}, {cx + half, 94}},
        {{cx + half - 4, 94}, {cx + half - 8, 94}, {cx + half - 12, 94}},
        {{cx + 12, 72}, {cx - 12, 72}, {cx - half + 12, 94}},
    });
    body.rect(cx - 1, 3, cx + 1, 17);
    body.rect(cx - 19, 66, cx + 19, 71);
    body.rect(cx - 9, 39, cx + 9, 44);
    light.rect(cx - 17, 67, cx + 17, 69);
    light.rect(cx - 8, 40, cx + 8, 43);
    if (tokyo) {
        light.rect(cx - 2, 13, cx + 2, 22);
        light.rect(cx - 4, 27, cx + 4, 32);
        light.polygon({{cx - 9, 47}, {cx + 9, 47}, {cx + 12, 54}, {cx - 12, 54}});
    }
    for (int level = 0; level < 6; ++level) {
        const float y = 23.0f + level * 7.0f;
        const float r0 = 2.0f + level * 1.65f;
        const float r1 = r0 + 1.65f;
        lines.polygon({{cx - r0, y}, {cx + r1, y + 7}, {cx - r1, y + 7},
                       {cx + r0, y}}, false);
    }
    lines.polygon({{cx - 20, 75}, {cx - 9, 68}, {cx - 11, 83},
                   {cx - 25, 85}}, false);
    lines.polygon({{cx + 20, 75}, {cx + 9, 68}, {cx + 11, 83},
                   {cx + 25, 85}}, false);
}
void rockstarBadge(LandmarkPath& badge) {
    badge.curve({158, 7}, {
        {{153, 7}, {151, 9}, {151, 14}},
        {{151, 23}, {151, 35}, {151, 42}},
        {{151, 47}, {154, 49}, {159, 49}},
        {{169, 49}, {184, 49}, {189, 49}},
        {{194, 49}, {196, 46}, {196, 41}},
        {{196, 32}, {196, 19}, {196, 14}},
        {{196, 9}, {194, 7}, {189, 7}},
    });
}

void viceCityTitle(LandmarkPath& outline, LandmarkPath& fill) {
    // 固定矢量粗体字样，不依赖本机是否安装游戏字体。
    struct Letter { char character; std::array<uint8_t, 7> rows; };
    static constexpr Letter alphabet[] = {
        {'G', {14, 17, 16, 23, 17, 17, 14}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
        {'A', {14, 17, 17, 31, 17, 17, 17}}, {'N', {17, 25, 25, 21, 19, 19, 17}},
        {'D', {30, 17, 17, 17, 17, 17, 30}}, {'T', {31, 4, 4, 4, 4, 4, 4}},
        {'H', {17, 17, 17, 31, 17, 17, 17}}, {'E', {31, 16, 16, 30, 16, 16, 31}},
        {'F', {31, 16, 16, 30, 16, 16, 16}}, {'U', {17, 17, 17, 17, 17, 17, 14}},
        {'O', {14, 17, 17, 17, 17, 17, 14}},
    };
    constexpr const char* words[] = {"GRAND", "THEFT", "AUTO"};
    for (size_t row = 0; row < 3; ++row) {
        const float top = 4.0f + static_cast<float>(row) * 18;
        const float left = row == 2 ? 49.0f : 42.0f;
        for (size_t letter = 0; words[row][letter] != '\0'; ++letter) {
            const auto glyph = std::find_if(std::begin(alphabet), std::end(alphabet),
                [&](const Letter& candidate) { return candidate.character == words[row][letter]; });
            if (glyph == std::end(alphabet))
                continue;
            for (size_t y = 0; y < glyph->rows.size(); ++y)
                for (int x = 0; x < 5; ++x) {
                    if ((glyph->rows[y] & (1u << (4 - x))) == 0)
                        continue;
                    const float gx = left + static_cast<float>(letter) * 14 + x * 2.3f;
                    const float gy = top + static_cast<float>(y) * 2.3f;
                    outline.rect(gx - 0.9f, gy - 0.9f, gx + 3.2f, gy + 3.2f);
                    fill.rect(gx, gy, gx + 2.3f, gy + 2.3f);
                }
        }
    }
}
} // namespace

void LyricCityLandmarks::clear() {
    artworks_ = {};
    rockstarMark_.Reset();
    rockstarStar_.Reset();
    viceCityScript_.Reset();
}

bool LyricCityLandmarks::ensureRockstarShapes(ID2D1Factory* factory) {
    if (rockstarMark_ && rockstarStar_)
        return true;
    LandmarkPath mark(factory), star(factory);
    mark.polygon({{157, 39}, {162, 14}, {175, 14}, {179, 17}, {178, 25},
                  {174, 29}, {179, 39}, {171, 39}, {166, 30}, {163, 30}, {161, 39}});
    mark.polygon({{165, 24}, {171, 24}, {173, 22}, {173, 20}, {166, 20}});
    std::array<D2D1_POINT_2F, 10> points;
    for (size_t point = 0; point < points.size(); ++point) {
        const float angle = -kPi * 0.5f + static_cast<float>(point) * kPi / 5;
        const float radius = point % 2 == 0 ? 7.5f : 3.0f;
        points[point] = {184 + std::cos(angle) * radius, 37 + std::sin(angle) * radius};
    }
    star.polygon({points[0], points[1], points[2], points[3], points[4], points[5],
                  points[6], points[7], points[8], points[9]});
    auto markShape = mark.finish();
    auto starShape = star.finish();
    if (!markShape || !starShape)
        return false;
    rockstarMark_ = std::move(markShape);
    rockstarStar_ = std::move(starShape);
    return true;
}

bool LyricCityLandmarks::ensureArtwork(ID2D1DeviceContext* target, City city) {
    const auto index = static_cast<size_t>(city);
    if (index >= artworks_.size())
        return false;
    if (artworks_[index][0])
        return true;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    if (!factory)
        return false;
    LandmarkPath body(factory.Get()), face(factory.Get()), accent(factory.Get()), detail(factory.Get());
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> viceCityScript;
    switch (city) {
    case City::Shanghai:
        // 横向陆家嘴天际线：东方明珠双球与支腿为前景，三件套在右侧错落展开。
        // 上海单独采用 156×100 的画布，避免四座地标挤进同一个方形。
        body.rect(26, 27, 30, 72);
        body.rect(27, 13, 29, 29);
        body.ellipse(28, 29, 3, 3);
        body.polygon({{25, 69}, {29, 70}, {20, 91}, {13, 91}});
        body.polygon({{28, 70}, {32, 69}, {44, 91}, {37, 91}});
        body.rect(26, 71, 30, 91);
        body.ellipse(28, 63, 12, 11);
        body.ellipse(28, 41, 8, 7.5f);
        accent.ellipse(28, 63, 10.5f, 9.5f);
        accent.ellipse(28, 41, 6.5f, 6);
        detail.ellipse(26, 63, 5.5f, 9);
        detail.ellipse(27, 41, 3.5f, 6);
        detail.polygon({{18, 61}, {38, 61}}, false);
        detail.polygon({{19, 66}, {37, 66}}, false);
        detail.polygon({{22, 41}, {34, 41}}, false);
        // 较低的滨江楼群衔接东方明珠与高楼，保留各座建筑的完整轮廓。
        body.rect(48, 75, 57, 91);
        body.rect(58, 68, 66, 91);
        body.rect(67, 78, 73, 91);
        body.rect(76, 53, 94, 91);
        for (int tier = 0; tier < 6; ++tier) {
            const float inset = tier * 1.2f;
            body.rect(77 + inset, 50 - tier * 4.0f, 93 - inset, 54 - tier * 4.0f);
        }
        body.polygon({{83, 30}, {85, 19}, {87, 30}});
        body.polygon({{100, 91}, {102, 22}, {118, 17}, {121, 91}});
        // 与外轮廓反向，保留开瓶器顶部真正透明的梯形洞口。
        body.polygon({{106, 34}, {114, 34}, {114, 24}, {106, 27}});
        body.curve({128, 91}, {
            {{124, 64}, {129, 30}, {133, 10}},
            {{135, 3}, {146, 2}, {148, 9}},
            {{155, 34}, {153, 65}, {151, 91}},
        });
        face.curve({132, 90}, {
            {{138, 65}, {146, 40}, {144, 10}},
            {{151, 30}, {150, 69}, {147, 90}},
        });
        for (int floor = 0; floor < 9; ++floor) {
            const float y = 54.0f + floor * 4.0f;
            detail.polygon({{78, y}, {92, y}}, false);
            detail.polygon({{103, y}, {118, y}}, false);
        }
        face.polygon({{104, 39}, {106, 90}, {108, 90}, {107, 39}});
        body.rect(5, 91, 154, 93);
        for (int row = 0; row < 2; ++row)
            detail.polygon({{10.0f + row * 7, 96.0f + row * 3},
                            {147.0f - row * 9, 96.0f + row * 3}}, false);
        break;
    case City::NewYork:
        // 火炬在画面左侧，另一手持书板；长袍、七芒冠和阶梯基座。
        body.polygon({{36, 78}, {41, 52}, {34, 43}, {27, 26}, {21, 27},
                      {26, 47}, {35, 59}, {30, 78}});
        body.polygon({{40, 48}, {49, 43}, {58, 49}, {64, 73}, {60, 79},
                      {65, 81}, {32, 81}});
        body.ellipse(49, 37, 6, 8);
        for (int ray = 0; ray < 7; ++ray) {
            const float a = kPi + ray * kPi / 6;
            const float x = 49 + 7 * std::cos(a);
            const float y = 35 + 7 * std::sin(a);
            body.polygon({{x - 1, y + 2}, {49 + 14 * std::cos(a), 35 + 14 * std::sin(a)},
                          {x + 1, y + 2}});
        }
        body.rect(22, 16, 27, 29);
        face.rect(36, 82, 62, 88);
        face.polygon({{34, 88}, {64, 88}, {68, 95}, {30, 95}});
        accent.curve({23, 17}, {
            {{15, 12}, {25, 7}, {24, 2}},
            {{34, 11}, {29, 14}, {26, 17}},
        });
        face.polygon({{54, 50}, {63, 48}, {68, 64}, {58, 67}});
        detail.polygon({{44, 52}, {39, 76}, {48, 65}, {47, 78}}, false);
        detail.polygon({{50, 47}, {52, 59}, {57, 77}}, false);
        detail.polygon({{45, 35}, {53, 35}}, false);
        break;
    case City::Paris:
        latticeTower(body, accent, detail, false);
        break;
    case City::Tokyo:
        latticeTower(body, face, detail, true);
        // 樱枝与五瓣花簇留在塔侧，避免遮住塔身。
        detail.polygon({{97, 94}, {87, 71}, {81, 53}, {72, 42}}, false);
        detail.polygon({{86, 70}, {97, 53}}, false);
        detail.polygon({{81, 55}, {93, 36}}, false);
        for (const auto center : {D2D1::Point2F(73, 42), D2D1::Point2F(92, 36),
                                 D2D1::Point2F(96, 54), D2D1::Point2F(83, 61)}) {
            for (int petal = 0; petal < 5; ++petal) {
                const float angle = petal * 2 * kPi / 5;
                accent.ellipse(center.x + 3 * std::sin(angle), center.y - 3 * std::cos(angle),
                               2.6f, 2.6f);
            }
            face.ellipse(center.x, center.y, 1.4f, 1.4f);
        }
        break;
    case City::London:
        body.rect(31, 40, 69, 94);
        body.rect(28, 34, 72, 40);
        body.polygon({{32, 34}, {39, 22}, {43, 18}, {47, 7}, {53, 7},
                      {57, 18}, {61, 22}, {68, 34}});
        body.rect(49, 2, 51, 10);
        for (const float x : {30.0f, 65.0f})
            body.polygon({{x, 35}, {x + 2.5f, 24}, {x + 5, 35}});
        face.rect(36, 65, 64, 93);
        accent.ellipse(50, 51, 12, 12);
        for (int hour = 0; hour < 12; ++hour) {
            const float angle = hour * kPi / 6;
            detail.polygon({{50 + 9 * std::sin(angle), 51 - 9 * std::cos(angle)},
                            {50 + 10.5f * std::sin(angle), 51 - 10.5f * std::cos(angle)}}, false);
        }
        detail.polygon({{50, 43}, {50, 51}, {56, 54}}, false);
        for (int column = 0; column < 5; ++column)
            detail.polygon({{39.0f + column * 5.5f, 69}, {39.0f + column * 5.5f, 91}}, false);
        detail.polygon({{37, 23}, {63, 23}}, false);
        break;
    case City::Beijing:
        // 横向双地标：左侧祈年殿，右侧天安门；各自保留完整轮廓。
        accent.rect(10, 88, 90, 94);
        accent.rect(16, 83, 84, 87);
        accent.rect(22, 78, 78, 82);
        body.rect(26, 55, 74, 78);
        body.rect(32, 38, 68, 53);
        body.rect(40, 26, 60, 37);
        for (int tier = 0; tier < 3; ++tier) {
            const float y = 31.0f + tier * 18;
            const float r = 20.0f + tier * 12;
            face.curve({50 - r, y}, {
                {{50 - r + 9, y + 4}, {43, y - 7}, {50, y - 13}},
                {{57, y - 7}, {50 + r - 9, y + 4}, {50 + r, y}},
                {{50 + r - 2, y + 9}, {50 - r + 2, y + 9}, {50 - r, y}},
            });
            detail.polygon({{50 - r + 3, y + 5}, {50 + r - 3, y + 5}}, false);
        }
        accent.ellipse(50, 13, 3, 4);
        for (int column = 0; column < 7; ++column)
            accent.rect(28.0f + column * 7, 72, 30.0f + column * 7, 78);
        // 天安门：重檐庑殿顶、红色城台、五个拱门与城楼栏杆。
        body.rect(105, 70, 193, 94);
        body.rect(114, 56, 184, 70);
        body.rect(123, 40, 175, 52);
        accent.curve({116, 35}, {
            {{124, 41}, {134, 34}, {149, 31}},
            {{164, 34}, {174, 41}, {182, 35}},
            {{179, 43}, {173, 46}, {168, 46}},
            {{154, 47}, {133, 47}, {123, 46}},
            {{120, 43}, {117, 40}, {116, 35}},
        });
        accent.curve({104, 51}, {
            {{114, 58}, {134, 49}, {149, 45}},
            {{164, 49}, {184, 58}, {194, 51}},
            {{190, 60}, {184, 63}, {176, 63}},
            {{159, 64}, {134, 64}, {122, 63}},
            {{114, 63}, {108, 60}, {104, 51}},
        });
        for (int gate = 0; gate < 5; ++gate) {
            const float x = 117.0f + gate * 16;
            const float radius = gate == 2 ? 5.0f : 3.6f;
            const float spring = gate == 2 ? 81.0f : 84.0f;
            // 反向轮廓形成真正透空的拱门，不用背景色盖住已有绘制。
            body.curve({x - radius, 94}, {
                {{x - radius, 94}, {x + radius, 94}, {x + radius, 94}},
                {{x + radius, 90}, {x + radius, 87}, {x + radius, spring}},
                {{x + radius, spring - 7}, {x - radius, spring - 7}, {x - radius, spring}},
            });
        }
        accent.rect(101, 94, 197, 97);
        detail.polygon({{112, 68}, {186, 68}}, false);
        detail.polygon({{123, 43}, {175, 43}}, false);
        for (int column = 0; column < 9; ++column) {
            const float x = 117.0f + column * 8;
            detail.polygon({{x, 64}, {x, 70}}, false);
        }
        // 小尺寸仅保留中央画像框与两侧题额的构图，不绘制不可辨认的文字。
        accent.rect(145, 70, 153, 79);
        body.rect(146.5f, 71.5f, 151.5f, 77.5f);
        detail.polygon({{115, 75}, {137, 75}}, false);
        detail.polygon({{161, 75}, {183, 75}}, false);
        break;
    case City::Guangzhou:
        // 小蛮腰：外侧网格向中部收紧，顶部观景层偏心。
        body.curve({29, 94}, {
            {{38, 76}, {46, 57}, {42, 43}},
            {{40, 28}, {38, 19}, {39, 14}},
            {{45, 10}, {63, 11}, {66, 16}},
            {{62, 37}, {52, 55}, {57, 72}},
            {{59, 81}, {64, 90}, {69, 94}},
        });
        face.curve({34, 93}, {
            {{49, 66}, {52, 43}, {44, 15}},
            {{48, 13}, {53, 13}, {56, 14}},
            {{48, 43}, {46, 67}, {63, 93}},
        });
        accent.ellipse(52, 15, 14, 3);
        body.rect(52, 2, 54, 14);
        for (int row = 0; row < 9; ++row) {
            const float y = 21.0f + row * 8;
            const float r = 7.0f + std::fabs(y - 54) * 0.17f;
            detail.polygon({{49 - r, y}, {49 + r, y + 4}}, false);
            detail.polygon({{49 + r, y}, {49 - r, y + 4}}, false);
        }
        break;
    case City::Wuhan:
        // 黄鹤楼五层飞檐，每层檐角上翘，楼身逐层收分。
        face.rect(15, 89, 85, 95);
        for (int tier = 0; tier < 5; ++tier) {
            const float y = 22.0f + tier * 15;
            const float r = 15.0f + tier * 6;
            body.rect(50 - r + 6, y, 50 + r - 6, y + 11);
            accent.curve({50 - r - 3, y - 5}, {
                {{50 - r + 2, y + 4}, {40, y - 4}, {50, y - 11}},
                {{60, y - 4}, {50 + r - 2, y + 4}, {50 + r + 3, y - 5}},
                {{50 + r, y + 7}, {50 - r, y + 7}, {50 - r - 3, y - 5}},
            });
            detail.polygon({{50 - r + 7, y + 8}, {50 + r - 7, y + 8}}, false);
            for (const float dx : {-0.5f, 0.0f, 0.5f})
                face.rect(49 + dx * r, y + 5, 51 + dx * r, y + 11);
        }
        body.rect(49, 4, 51, 11);
        break;
    case City::Sydney:
        // 多组互相咬合的白色帆壳，底座与海面分开绘制。
        body.polygon({{5, 82}, {89, 75}, {97, 85}, {13, 94}});
        for (int sail = 0; sail < 4; ++sail) {
            const float x = 10.0f + sail * 19;
            const float peak = sail == 1 ? 17.0f : 30.0f + sail * 5;
            face.curve({x, 79}, {
                {{x + 4, 59}, {x + 5, peak + 10}, {x + 10, peak}},
                {{x + 13, 50}, {x + 23, 64}, {x + 28, 78}},
            });
            detail.curve({x + 10, peak + 2}, {
                {{x + 9, 48}, {x + 17, 67}, {x + 24, 77}},
            }, false);
        }
        for (int row = 0; row < 3; ++row)
            accent.rect(10.0f + row * 4, 95.0f + row * 1.5f,
                        91.0f - row * 4, 95.7f + row * 1.5f);
        break;
    case City::Singapore:
        // 鱼尾狮的鬃毛、狮口、鱼鳞和向侧面喷出的弧形水柱。
        body.curve({36, 94}, {
            {{50, 81}, {49, 72}, {44, 59}},
            {{34, 49}, {35, 27}, {48, 23}},
            {{58, 16}, {72, 24}, {70, 33}},
            {{72, 37}, {78, 37}, {78, 43}},
            {{75, 48}, {66, 46}, {63, 51}},
            {{59, 61}, {70, 73}, {64, 89}},
            {{69, 91}, {73, 93}, {75, 95}},
        });
        face.curve({40, 90}, {
            {{53, 80}, {55, 66}, {49, 55}},
            {{60, 66}, {63, 78}, {59, 89}},
        });
        face.ellipse(58, 34, 6, 7);
        // 水柱是窄的填充轮廓，避免把开放曲线当面填充。
        accent.curve({76, 43}, {
            {{90, 42}, {94, 65}, {97, 79}},
            {{92, 64}, {87, 46}, {76, 45}},
        });
        detail.ellipse(63, 33, 1.5f, 1.5f);
        detail.polygon({{49, 26}, {43, 33}, {47, 40}, {43, 48}, {49, 55}}, false);
        for (int row = 0; row < 4; ++row) {
            const float y = 65.0f + row * 6;
            detail.curve({49, y}, {{{54, y + 6}, {58, y + 6}, {63, y}}}, false);
        }
        accent.rect(30, 95, 80, 98);
        break;
    case City::SanFrancisco:
        // 宽幅双塔悬索桥；海湾、桥面和主缆保留不同层次。
        face.rect(2, 85, 198, 88);
        face.rect(10, 92, 190, 94);
        face.rect(25, 98, 176, 100);
        body.rect(2, 67, 198, 72);
        for (const float center : {56.0f, 144.0f}) {
            body.rect(center - 8, 14, center - 4, 87);
            body.rect(center + 4, 14, center + 8, 87);
            body.rect(center - 9, 84, center + 9, 89);
            for (const float y : {20.0f, 35.0f, 50.0f}) {
                body.rect(center - 6, y, center + 6, y + 3);
                detail.polygon({{center - 4, y + 3}, {center + 4, y + 13},
                                {center - 4, y + 13}, {center + 4, y + 3}}, false);
            }
            accent.rect(center - 8, 10, center - 4, 14);
            accent.rect(center + 4, 10, center + 8, 14);
        }
        body.curve({2, 60}, {
            {{20, 56}, {38, 30}, {56, 16}},
            {{85.3333f, 61}, {114.6667f, 61}, {144, 16}},
            {{162, 31}, {180, 57}, {198, 60}},
            {{180, 59}, {162, 34}, {144, 19}},
            {{114.6667f, 64}, {85.3333f, 64}, {56, 19}},
            {{38, 33}, {20, 59}, {2, 62}},
        });
        for (int hanger = 0; hanger < 23; ++hanger) {
            const float x = 8.0f + hanger * 8;
            float y;
            if (x < 56) {
                const float t = (x - 2) / 54;
                const float s = 1 - t;
                y = 60 * s * s * s + 168 * s * s * t + 90 * s * t * t + 16 * t * t * t;
            } else if (x > 144) {
                const float t = (x - 144) / 54;
                const float s = 1 - t;
                y = 16 * s * s * s + 93 * s * s * t + 171 * s * t * t + 60 * t * t * t;
            } else {
                const float t = (x - 56) / 88;
                y = 16 + 135 * t * (1 - t);
            }
            body.rect(x, y + 1, x + 0.9f, 67);
        }
        accent.rect(3, 67, 197, 68.5f);
        for (int panel = 0; panel < 16; ++panel) {
            const float x = 4.0f + panel * 12;
            detail.polygon({{x, 73}, {x + 6, 78}, {x + 12, 73}}, false);
        }
        break;
    case City::LosAngeles: {
        // 洛圣都明信片：落日、VINEWOOD 山坡、棕榈树、城区和前景跑车。
        face.ellipse(40, 26, 19, 19);
        face.curve({3, 67}, {
            {{15, 54}, {23, 51}, {32, 50}},
            {{42, 41}, {53, 46}, {65, 53}},
            {{72, 59}, {82, 63}, {91, 67}},
        });
        for (int building = 0; building < 7; ++building) {
            const float x = 68.0f + building * 9;
            const float top = 55.0f - (building % 4) * 8;
            body.rect(x, top, x + 7, 80);
            detail.polygon({{x + 2, top + 5}, {x + 5, top + 5}}, false);
            detail.polygon({{x + 2, top + 11}, {x + 5, top + 11}}, false);
        }
        body.rect(96, 21, 98, 32);
        for (const auto palm : {D2D1::Point2F(17, 34), D2D1::Point2F(140, 27)}) {
            body.polygon({{palm.x - 2, 82}, {palm.x, palm.y},
                          {palm.x + 2, palm.y}, {palm.x + 4, 82}});
            for (const float direction : {-1.0f, 1.0f}) {
                body.curve(palm, {
                    {{palm.x + direction * 6, palm.y - 12},
                     {palm.x + direction * 15, palm.y - 6},
                     {palm.x + direction * 17, palm.y + 2}},
                    {{palm.x + direction * 9, palm.y - 2},
                     {palm.x + direction * 5, palm.y - 3}, palm},
                });
                body.curve(palm, {
                    {{palm.x + direction * 9, palm.y - 2},
                     {palm.x + direction * 14, palm.y + 4},
                     {palm.x + direction * 12, palm.y + 11}},
                    {{palm.x + direction * 8, palm.y + 4},
                     {palm.x + direction * 4, palm.y + 2}, palm},
                });
            }
        }
        // 山坡字牌采用固定矢量笔画，不依赖运行环境的字体。
        constexpr char sign[] = "VINEWOOD";
        for (size_t letter = 0; letter < 8; ++letter) {
            const float x = 23.0f + static_cast<float>(letter) * 4.8f;
            const float y = 40.0f - static_cast<float>(letter) * 0.25f;
            switch (sign[letter]) {
            case 'V': detail.polygon({{x, y}, {x + 1.8f, y + 6}, {x + 3.6f, y}}, false); break;
            case 'I': detail.polygon({{x + 1.8f, y}, {x + 1.8f, y + 6}}, false); break;
            case 'N': detail.polygon({{x, y + 6}, {x, y}, {x + 3.6f, y + 6}, {x + 3.6f, y}}, false); break;
            case 'E':
                detail.polygon({{x + 3.6f, y}, {x, y}, {x, y + 6}, {x + 3.6f, y + 6}}, false);
                detail.polygon({{x, y + 3}, {x + 3, y + 3}}, false);
                break;
            case 'W': detail.polygon({{x, y}, {x + 0.8f, y + 6}, {x + 1.8f, y + 2},
                                     {x + 2.8f, y + 6}, {x + 3.6f, y}}, false); break;
            case 'O': detail.polygon({{x, y}, {x + 3.6f, y}, {x + 3.6f, y + 6},
                                     {x, y + 6}, {x, y}}, false); break;
            case 'D': detail.polygon({{x, y + 6}, {x, y}, {x + 2.8f, y},
                                     {x + 3.6f, y + 3}, {x + 2.8f, y + 6}, {x, y + 6}}, false); break;
            }
        }
        body.polygon({{2, 84}, {196, 84}, {200, 98}, {0, 98}});
        for (int dash = 0; dash < 5; ++dash)
            detail.polygon({{8.0f + dash * 39, 94}, {25.0f + dash * 39, 94}}, false);
        accent.polygon({{51, 88}, {54, 79}, {65, 76}, {76, 65}, {105, 65},
                        {119, 77}, {132, 80}, {135, 88}});
        body.ellipse(68, 89, 7, 7);
        body.ellipse(119, 89, 7, 7);
        // 轮胎在车身之后补画，车窗以浅色划出低矮车顶。
        detail.polygon({{70, 76}, {79, 68}, {102, 68}, {112, 76}, {70, 76}}, false);
        detail.polygon({{90, 68}, {91, 76}}, false);
        detail.polygon({{55, 81}, {63, 81}}, false);
        detail.polygon({{124, 82}, {131, 82}}, false);
        // 独立黄色圆角徽标，黑色斜体 R 与白色黑边五角星保持清楚的层次。
        rockstarBadge(accent);
        break;
    }
    case City::Miami: {
        viceCityTitle(body, face);
        rockstarBadge(accent);
        LandmarkPath script(factory.Get());
        // 手写连笔 Vice City：大写 V/C、小写连笔、i 的点和 y 的长尾。
        script.curve({10, 55}, {
            {{14, 58}, {14, 70}, {18, 75}},
            {{23, 69}, {27, 54}, {33, 50}},
        }, false);
        script.curve({36, 60}, {
            {{33, 65}, {30, 78}, {41, 69}},
        }, false);
        script.ellipse(38, 53, 1.2f, 1.2f);
        script.curve({53, 60}, {
            {{43, 54}, {38, 66}, {42, 72}},
            {{45, 76}, {51, 72}, {55, 68}},
        }, false);
        script.curve({55, 65}, {
            {{65, 66}, {67, 56}, {60, 58}},
            {{49, 61}, {50, 78}, {66, 70}},
        }, false);
        script.curve({96, 55}, {
            {{87, 43}, {76, 54}, {75, 65}},
            {{73, 78}, {89, 76}, {97, 66}},
        }, false);
        script.curve({103, 59}, {
            {{99, 67}, {96, 77}, {108, 68}},
        }, false);
        script.ellipse(105, 52, 1.2f, 1.2f);
        script.curve({117, 49}, {
            {{111, 57}, {105, 80}, {119, 67}},
        }, false);
        script.curve({107, 59}, {
            {{111, 57}, {116, 58}, {123, 56}},
        }, false);
        script.curve({125, 58}, {
            {{120, 65}, {120, 78}, {131, 65}},
            {{133, 62}, {135, 58}, {136, 56}},
            {{134, 71}, {129, 87}, {120, 88}},
            {{109, 88}, {120, 76}, {141, 68}},
        }, false);
        // 细长的收笔下划线与原作粉色霓虹基调一致。
        script.curve({17, 86}, {{{43, 80}, {72, 82}, {105, 79}}}, false);
        viceCityScript = script.finish();
        break;
    }
    case City::Count:
        return false;
    }
    Artwork artwork = {body.finish(), face.finish(), accent.finish(), detail.finish()};
    if (std::any_of(artwork.begin(), artwork.end(), [](const auto& path) { return !path; }))
        return false;
    if ((city == City::LosAngeles || city == City::Miami) && !ensureRockstarShapes(factory.Get()))
        return false;
    if (city == City::Miami) {
        if (!viceCityScript)
            return false;
        viceCityScript_ = std::move(viceCityScript);
    }
    artworks_[index] = std::move(artwork);
    return true;
}

void LyricCityLandmarks::draw(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                             City city, D2D1_RECT_F slot, float opacity) {
    const float width = slot.right - slot.left;
    const float height = slot.bottom - slot.top;
    if (!target || !brush || width <= 0 || height <= 0 || opacity <= 0 ||
        !ensureArtwork(target, city))
        return;
    static constexpr std::array<std::array<uint32_t, 4>, kCityCount> colors = {{
        {{0x49657A, 0x90B8C5, 0xD998B0, 0xD0E4EB}}, // 上海：东方明珠的粉红球体
        {{0x72AB95, 0x527D70, 0xF4C860, 0x31574A}}, // 纽约
        {{0x96714C, 0xBD9563, 0xEBCB91, 0xE3C29A}}, // 巴黎
        {{0xDB593B, 0xFFF0D9, 0xF3AFC8, 0x854C4C}}, // 东京
        {{0xA58654, 0xC4A874, 0xFFF4CF, 0x5E4A31}}, // 伦敦
        {{0xAD493C, 0x426E9B, 0xEACB84, 0xBCD5E5}}, // 北京
        {{0x8E74AE, 0xC3A4D8, 0xE4DCF3, 0xE2D3EE}}, // 广州
        {{0xA85036, 0xE6CAA0, 0xDAB061, 0xF6DEB3}}, // 武汉
        {{0x96785A, 0xF4EBD8, 0x74B5C9, 0xB1A38E}}, // 悉尼
        {{0xD9E5DE, 0xABC5BF, 0x88C8D6, 0x577F78}}, // 新加坡
        {{0xC44E35, 0x7BB7C6, 0xEDB685, 0xE7A087}}, // 旧金山
        {{0x293943, 0xE9A28B, 0xF9BE32, 0xF2E6DA}}, // 洛杉矶／洛圣都
        {{0x22202B, 0xFFF6FA, 0xF9BE32, 0xF08EB8}}, // 迈阿密／罪恶都市
    }};
    const auto index = static_cast<size_t>(city);
    const auto& artwork = artworks_[index];
    D2D1_MATRIX_3X2_F previous;
    target->GetTransform(&previous);
    const auto previousColor = brush->GetColor();
    const float aspect = city == City::Shanghai ? 1.56f
        : (city == City::Beijing || city == City::SanFrancisco || city == City::LosAngeles ||
           city == City::Miami)
            ? 2.0f : 1.0f;
    const float size = std::min({width / aspect, height * 0.86f, 62.0f});
    const float scale = size / 100.0f;
    const float x = (slot.left + slot.right - size * aspect) * 0.5f;
    const float y = slot.top + (height - size) * 0.5f;
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                         D2D1::Matrix3x2F::Translation(x, y) * previous);
    for (size_t layer = 0; layer < artwork.size(); ++layer) {
        brush->SetColor(D2D1::ColorF(colors[index][layer], opacity));
        if (layer < 3)
            target->FillGeometry(artwork[layer].Get(), brush);
        else
            target->DrawGeometry(artwork[layer].Get(), brush, std::max(1.15f, 0.40f / scale));
    }
    if (city == City::Miami) {
        brush->SetColor(D2D1::ColorF(0x22202B, opacity));
        target->DrawGeometry(viceCityScript_.Get(), brush, 7.5f);
        brush->SetColor(D2D1::ColorF(0xFFF6FA, opacity));
        target->DrawGeometry(viceCityScript_.Get(), brush, 5.5f);
        brush->SetColor(D2D1::ColorF(0xF08EB8, opacity));
        target->DrawGeometry(viceCityScript_.Get(), brush, 3.2f);
    }
    if (city == City::LosAngeles || city == City::Miami) {
        brush->SetColor(D2D1::ColorF(0x17191D, opacity));
        if (city == City::LosAngeles)
            for (const float wheelX : {68.0f, 119.0f})
                target->FillEllipse(D2D1::Ellipse({wheelX, 89}, 6, 6), brush);
        target->FillGeometry(rockstarMark_.Get(), brush);
        brush->SetColor(D2D1::ColorF(D2D1::ColorF::White, opacity));
        target->FillGeometry(rockstarStar_.Get(), brush);
        brush->SetColor(D2D1::ColorF(0x17191D, opacity));
        target->DrawGeometry(rockstarStar_.Get(), brush, 1.4f);
    }
    if (city == City::LosAngeles) {
        // 徽标下方保留 GTA V 的绿色罗马数字，增强第五代的识别度。
        brush->SetColor(D2D1::ColorF(0xF3EDDC, opacity));
        target->DrawLine({164, 57}, {176, 74}, brush, 7.0f);
        target->DrawLine({176, 74}, {189, 57}, brush, 7.0f);
        brush->SetColor(D2D1::ColorF(0x3F8757, opacity));
        target->DrawLine({164, 57}, {176, 74}, brush, 4.5f);
        target->DrawLine({176, 74}, {189, 57}, brush, 4.5f);
    }
    target->SetTransform(previous);
    brush->SetColor(previousColor);
}
