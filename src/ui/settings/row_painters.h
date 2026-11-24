#pragma once

#include "idle/idle_types.h"
#include "media/generic_smtc_filter.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/settings/artwork.h"
#include "ui/settings/row.h"
#include "ui/settings/slider_editor.h"

#include <windows.h>

#include <span>

namespace settings_ui::row_painters {

/// 绘制单个设置行控件时由窗口借出的只读交互与样式上下文。
struct RowPaintContext {
    SettingsArtwork& artwork;
    SliderEditor& sliderEditor;
    COLORREF floatingCardBackgroundColor = RGB(255, 255, 255);
    std::span<const IdleAppInfo> idleApps;
    std::span<const GenericSmtcAppRule> genericSmtcRules;
    std::span<const GenericSmtcAppRule> customLocalLyricsPlayers;
    UINT dpi = 96;
    bool idleAppDragging = false;
    int idleAppDragIndex = -1;
    int idleAppDropIndex = -1;
    int hoverId = 0;
    int hoverOption = -1;
    int pressedId = 0;
    int pressedOption = -1;
    int focusedId = 0;
    bool focusVisible = false;
};

D2D1_RECT_F sliderValueRect(const Row& row);
D2D1_RECT_F sliderNumberRect(const Row& row);

void drawButton(fluent::FluentDialogSurface::Painter& painter, const RowPaintContext& ctx,
                const Row& row);
void drawToggle(fluent::FluentDialogSurface::Painter& painter, const Row& row,
                bool hovered, bool focused);
void drawSlider(fluent::FluentDialogSurface::Painter& painter, const RowPaintContext& ctx,
                const Row& row);
void drawRadio(fluent::FluentDialogSurface::Painter& painter, const RowPaintContext& ctx,
               Row& row);
void drawModeGrid(fluent::FluentDialogSurface::Painter& painter, const RowPaintContext& ctx,
                  Row& row);
void drawAppList(fluent::FluentDialogSurface::Painter& painter, const RowPaintContext& ctx,
                 const Row& row);

} // namespace settings_ui::row_painters
