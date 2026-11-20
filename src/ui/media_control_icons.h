#pragma once

#include <d2d1_1.h>

namespace media_control {

// 任务栏内嵌控件与媒体弹窗共用的播放控制图标几何。
struct Geometry {
    ID2D1PathGeometry* play = nullptr;
    ID2D1PathGeometry* previous = nullptr;
    ID2D1PathGeometry* next = nullptr;
    ID2D1PathGeometry* repeat[3] = {nullptr, nullptr, nullptr}; // 顺序/列表、单曲、随机
    D2D1_RECT_F repeatBounds[3] = {};
    ID2D1PathGeometry* musicNote = nullptr; // Segoe Fluent Icons / MDL2 Audio 音符字形
    D2D1_RECT_F musicNoteBounds = {};
};

void release(Geometry& geometry);
bool create(ID2D1Factory* factory, Geometry& geometry);

// index: 0=上一首，1=播放/暂停，2=下一首。
void draw(ID2D1RenderTarget* target, const Geometry& geometry, int index, bool playing,
          const D2D1_POINT_2F& center, float radius, ID2D1Brush* brush);

// 音量图标（不依赖 Geometry，按 level 现画；接受 ID2D1RenderTarget，
// DCRenderTarget（BitmapRenderer）与 DeviceContext（CompositionRenderer）都可传入）：
// level 0=静音（喇叭+叉），1=无声（仅喇叭），2=小声（一道波），3=大声（两道波）。
void drawVolume(ID2D1RenderTarget* target, const D2D1_POINT_2F& center, float radius,
                ID2D1Brush* brush, int level);

// 播放模式图标：字形直接取自 Windows 自带图标字体（Segoe Fluent Icons，
// 回退 Segoe MDL2 Assets），不是手绘路径；字体缺失时退回矢量兜底画法。
// mode 0 顺序播放 / 2 单曲循环 / 3 随机播放 各自一个字形；
// 列表循环（mode 1）已不再参与轮换，若播放器自行上报则与顺序播放共用字形。
void drawRepeat(ID2D1RenderTarget* target, const Geometry& geometry,
                const D2D1_POINT_2F& center, float radius, ID2D1Brush* brush, int mode);

// 系统 Fluent 图标字体（Segoe Fluent Icons / MDL2 Assets）的原生音符字形（U+E8D6），
// 供无封面占位图与无来源图标兜底复用。持有 Geometry 的绘制方优先使用带 geometry 的重载。
void drawMusicNote(ID2D1RenderTarget* target, const Geometry& geometry,
                   const D2D1_POINT_2F& center, float size, ID2D1Brush* brush);
void drawMusicNote(ID2D1RenderTarget* target, const D2D1_POINT_2F& center, float size,
                   ID2D1Brush* brush);

} // namespace media_control
