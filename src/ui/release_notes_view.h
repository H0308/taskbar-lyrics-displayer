#pragma once

#include "ui/fluent_dialog_surface.h"
#include "ui/markdown_document.h"
#include "update/update_service.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

enum class AboutPanelTab {
    ReleaseNotes,
    Disclaimer,
};

// 关于页的更新日志与免责声明面板逻辑模型。它不创建子窗口，只保存解析结果和滚动位置，
// 由 AboutDialog 的单一绘制表面调用 paint()。
class ReleaseNotesView {
public:
    bool create(HWND parent);
    bool tick(WPARAM timer);
    void stop();
    void reset();

    AboutPanelTab activeTab() const { return activeTab_; }
    void setActiveTab(AboutPanelTab tab);
    void setRelease(update_service::ReleaseInfo release);
    void setMessage(const std::wstring& message);

    void scrollBy(float delta);
    float contentHeight() const { return contentHeight_; }
    bool dragging() const { return dragging_; }
    void endDrag() { dragging_ = false; }
    bool beginScroll(float x, float y);
    void dragTo(float y);

    void paint(fluent::FluentDialogSurface::Painter& painter, const D2D1_RECT_F& rect,
               float scrollbarRight);

private:
    void applyMarkdownStyles(IDWriteTextLayout* layout, const markdown_doc::MarkdownBlock& block,
                             ID2D1SolidColorBrush* linkBrush);
    void drawScrollBar(fluent::FluentDialogSurface::Painter& painter, const D2D1_RECT_F& rect,
                       float scrollbarRight);
    const std::vector<markdown_doc::MarkdownBlock>& ensureDisclaimerBlocks();
    static const std::wstring& emptyMessage();

    AboutPanelTab activeTab_ = AboutPanelTab::ReleaseNotes;
    std::optional<update_service::ReleaseInfo> release_;
    std::vector<markdown_doc::MarkdownBlock> blocks_;
    std::vector<markdown_doc::MarkdownBlock> disclaimerBlocks_;
    std::wstring message_;
    float scrollY_ = 0.0f;
    float contentHeight_ = 0.0f;
    D2D1_RECT_F scrollTrack_{};
    D2D1_RECT_F scrollThumb_{};
    float maxScroll_ = 0.0f;
    float viewHeight_ = 0.0f;
    float dragOffset_ = 0.0f;
    bool dragging_ = false;
    HWND parent_ = nullptr;
    fluent::CompositedScrollView scrollView_;
    bool scrollOnlyPaint_ = false;
    D2D1_RECT_F panelRect_{};
    float scrollbarRight_ = 0.0f;
};
