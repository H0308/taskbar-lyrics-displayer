#pragma once
#include "model/display_types.h"
#include "monitor/resource_monitor.h"
#include "quota/quota_types.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

// 任务栏右侧状态区：系统资源采样、应用层推送的 AI 额度行、列宽与分页。
// 绘图目标、字体和画刷只在 draw 调用期间借用；额度数据按值复制，不反向读取应用配置。
class ResourcePanel {
public:
    const DockResourceVisibility& visibility() const { return dockResourceVisibility_; }
    void setVisibility(const DockResourceVisibility& value) {
        dockResourceVisibility_ = value;
        if (!value.hideLabels)
            clearTransientLabels();
        if (!value.hasAnyMetric() && resourceMonitorRunning_)
            stop();
        resetPage();
    }
    void setLayout(bool compact, bool vertical) {
        const bool twoPerPage = compact || vertical;
        if (twoPerPage != (compact_ || vertical_))
            dockResourcePage_ = twoPerPage ? dockResourcePage_ * 2 : dockResourcePage_ / 2;
        compact_ = compact;
        vertical_ = vertical;
    }
    void resetPage() { dockResourcePage_ = 0; }
    // AI 额度行由应用层从额度快照推导；空列表表示不显示额度。
    void setQuotaReadings(std::vector<QuotaReading> readings);
    const std::vector<QuotaReading>& quotaReadings() const { return quotaReadings_; }
    // 状态区是否有任何可绘制内容（系统指标或已启用的额度行）。
    bool hasAnyContent() const;
    void clampPage();
    void triggerTransientLabels(ULONGLONG now);
    void clearTransientLabels();
    bool transientLabelsFading(ULONGLONG now, bool animate) const;
    bool tickTransientLabels(ULONGLONG now, bool animate);
    void start();
    void stop();
    bool refresh();
    float width() const;
    size_t pageCount() const;
    bool advancePage(int steps);
    bool updateColumnWidths(IDWriteFactory* dwrite, IDWriteTextFormat* format);
    float measureVerticalStackedWidth(IDWriteFactory* dwrite, IDWriteTextFormat* format) const;
    static float verticalAvailableWidth(float width);
    float verticalHeight(float width) const;
    void drawVertical(ID2D1DeviceContext* target, IDWriteTextFormat* format, ID2D1Brush* brush,
                      const D2D1_RECT_F& area) const;
    void draw(ID2D1DeviceContext* target, IDWriteTextFormat* format, ID2D1Brush* brush, float x,
              float height, float width) const;

private:
    enum class ReadingKind {
        Cpu,
        Memory,
        Upload,
        Download,
        Gpu,
        CpuFrequency,
        Quota,
    };
    struct Reading {
        std::wstring label;
        std::wstring value;
        ReadingKind kind = ReadingKind::Cpu;
    };
    struct DockResourceCell {
        std::wstring value;
        std::wstring label;
    };
    struct DockResourceRow {
        DockResourceCell left, right;
    };
    struct ColumnMetrics {
        std::vector<std::array<float, 2>> pageColumnWidths;
        std::array<float, 2> singleLineWidths{};
        float verticalInlineWidth = 0.0f;
        float verticalStackedWidth = 0.0f;
        float verticalMinWidth = 0.0f;
        float verticalLineHeight = 0.0f;
    };
    std::vector<Reading> readings() const;
    std::vector<DockResourceRow> rows() const;
    ColumnMetrics measureColumnMetrics(IDWriteFactory* dwrite, IDWriteTextFormat* format) const;
    static std::wstring_view shortVerticalValue(std::wstring_view value);
    float columnGap() const;
    static std::wstring formatResourcePercent(double percent);
    static std::wstring formatNetworkRate(uint64_t bytesPerSecond);
    static std::wstring formatFrequency(bool available, double gigahertz);
    static float measureResourceTextWidth(IDWriteFactory* dwrite, IDWriteTextFormat* format,
                                          const std::wstring& text, float* lineHeight = nullptr);
    system_monitor::ResourceMonitor resourceMonitor_;
    std::vector<QuotaReading> quotaReadings_;
    uint64_t resourceSnapshotRevision_ = 0;
    ULONGLONG nextResourceSnapshotPollMs_ = 0;
    std::wstring resourceCpuText_ = L"—";
    std::wstring resourceMemoryText_ = L"—";
    std::wstring resourceDownloadText_ = L"—";
    std::wstring resourceUploadText_ = L"—";
    std::wstring resourceGpuText_ = L"—";
    std::wstring resourceCpuFrequencyText_ = L"—";
    DockResourceVisibility dockResourceVisibility_{};
    size_t dockResourcePage_ = 0;
    std::vector<std::array<float, 2>> resourcePageColumnWidths_{{76.0f, 100.0f}};
    std::array<float, 2> resourceSingleLineWidths_{};
    float verticalInlineWidth_ = 0.0f;
    float verticalStackedWidth_ = 0.0f;
    float verticalMinWidth_ = 0.0f;
    float verticalLineHeight_ = 0.0f;
    bool resourceMonitorRunning_ = false;
    bool compact_ = false;
    bool vertical_ = false;
    bool transientLabelActive_ = false;
    ULONGLONG transientLabelStartMs_ = 0;
    float transientLabelAlpha_ = 0.0f;
};
