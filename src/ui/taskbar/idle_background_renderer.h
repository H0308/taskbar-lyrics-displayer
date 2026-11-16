#pragma once

#include "model/display_types.h"

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

namespace taskbar_detail {

/// 绘制任务栏闲置语录动态背景所需的只读帧参数与画刷借用。
struct IdleQuoteBackgroundContext {
    ID2D1DeviceContext* rt = nullptr;
    IdleQuoteBackground style = IdleQuoteBackground::None;
    float width = 0.0f;
    float height = 0.0f;
    D2D1_RECT_F region{};
    ULONGLONG frameNowMs = 0;
    ID2D1SolidColorBrush* brushWarm = nullptr;
    ID2D1SolidColorBrush* brushCool = nullptr;
    ID2D1SolidColorBrush* brushAccent = nullptr;
    ID2D1SolidColorBrush* fallbackTextBrush = nullptr;
    IDWriteTextFormat* binaryFontFormat = nullptr;
};

void drawIdleQuoteBackground(const IdleQuoteBackgroundContext& ctx);

} // namespace taskbar_detail
