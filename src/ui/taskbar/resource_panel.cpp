#include "ui/taskbar/resource_panel.h"
#include "ui/taskbar/animation_clock.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <cstdint>

namespace {
constexpr float kDockResourceZoneGap = 4.0f;
constexpr float kCompactResourceZoneGap = 3.0f;
constexpr float kResourceCellSafetyPadding = 0.0f;
constexpr float kVerticalResourceInset = 2.0f;
constexpr float kVerticalResourceLabelGap = 1.0f;
constexpr float kVerticalResourceGroupGap = 4.0f;
constexpr UINT kResourceSnapshotPollMs = 250;
constexpr ULONGLONG kTransientLabelHoldMs = 900;
constexpr ULONGLONG kTransientLabelFadeMs = 260;
} // namespace
using taskbar_detail::monotonicNowMs;

float ResourcePanel::columnGap() const {
    return dockResourceVisibility_.smallFont ? kCompactResourceZoneGap : kDockResourceZoneGap;
}

float ResourcePanel::width() const {
    if (!dockResourceVisibility_.hasAnyMetric())
        return 0.0f;
    const float gap = columnGap();
    if (compact_) {
        float width = 0.0f;
        int count = 0;
        for (float cellWidth : resourceSingleLineWidths_) {
            if (cellWidth > 0.0f) {
                width += cellWidth;
                ++count;
            }
        }
        return width + std::max(0, count - 1) * gap;
    }
    float width = 0.0f;
    for (const auto& [left, right] : resourcePageColumnWidths_)
        width = std::max(width, left + (right > 0.0f ? gap + right : 0.0f));
    return width;
}

std::vector<ResourcePanel::Reading> ResourcePanel::readings() const {
    std::vector<Reading> result;
    if (dockResourceVisibility_.cpuUsage)
        result.push_back({L"CPU", resourceCpuText_});
    if (dockResourceVisibility_.memoryUsage)
        result.push_back({L"内存", resourceMemoryText_});
    if (dockResourceVisibility_.upload)
        result.push_back({L"上行", resourceUploadText_});
    if (dockResourceVisibility_.download)
        result.push_back({L"下行", resourceDownloadText_});
    if (dockResourceVisibility_.gpuUsage)
        result.push_back({L"GPU", resourceGpuText_});
    if (dockResourceVisibility_.cpuFrequency)
        result.push_back({L"频率", resourceCpuFrequencyText_});
    return result;
}

std::vector<ResourcePanel::DockResourceRow> ResourcePanel::rows() const {
    std::vector<DockResourceCell> cells;
    const bool hideLabels = dockResourceVisibility_.hideLabels;
    for (const auto& reading : readings()) {
        if (hideLabels)
            cells.push_back({reading.value, reading.label});
        else
            cells.push_back({std::wstring(reading.label) + L": " + reading.value, {}});
    }
    std::vector<DockResourceRow> rows;
    if (compact_) {
        for (size_t i = 0; i < cells.size(); i += 2)
            rows.push_back({cells[i], i + 1 < cells.size() ? cells[i + 1] : DockResourceCell{}});
        return rows;
    }
    for (size_t i = 0; i < cells.size(); i += 4) {
        rows.push_back({cells[i], i + 2 < cells.size() ? cells[i + 2] : DockResourceCell{}});
        rows.push_back({i + 1 < cells.size() ? cells[i + 1] : DockResourceCell{},
                        i + 3 < cells.size() ? cells[i + 3] : DockResourceCell{}});
    }
    return rows;
}

size_t ResourcePanel::pageCount() const {
    const size_t perPage = compact_ || vertical_ ? 2 : 4;
    const size_t count = readings().size();
    return (count + perPage - 1) / perPage;
}

std::wstring_view ResourcePanel::shortVerticalValue(std::wstring_view value) {
    if (value.ends_with(L"B/s") || value.ends_with(L"GHz"))
        return value.substr(0, value.find_first_not_of(L"0123456789.") + 1);
    return value;
}

float ResourcePanel::verticalAvailableWidth(float width) {
    return width - 2.0f * kVerticalResourceInset;
}

float ResourcePanel::verticalHeight(float width) const {
    const size_t count = readings().size();
    if (count == 0)
        return 0.0f;
    const float available = verticalAvailableWidth(width);
    if (verticalLineHeight_ <= 0.0f || available < verticalMinWidth_)
        return 0.0f;
    const size_t items = std::min<size_t>(2, count);
    const float gaps = items > 1 ? kVerticalResourceGroupGap : 0.0f;
    if (dockResourceVisibility_.hideLabels)
        return static_cast<float>(items) * verticalLineHeight_ + gaps;
    if (available < verticalInlineWidth_)
        return static_cast<float>(items) * (2.0f * verticalLineHeight_ + kVerticalResourceLabelGap) +
               gaps;
    return static_cast<float>(items) * verticalLineHeight_;
}

void ResourcePanel::drawVertical(ID2D1DeviceContext* rt, IDWriteTextFormat* format,
                                 ID2D1Brush* brush, const D2D1_RECT_F& area) const {
    if (!rt || !format || !brush || area.bottom <= area.top)
        return;

    const auto values = readings();
    if (values.empty())
        return;
    const size_t first = std::min(dockResourcePage_, (values.size() - 1) / 2) * 2;
    const bool hideLabels = dockResourceVisibility_.hideLabels;
    const bool stacked =
        !hideLabels && verticalAvailableWidth(area.right - area.left) < verticalInlineWidth_;
    float top = area.top;
    const auto previousAlignment = format->GetTextAlignment();
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    // 堆叠和隐藏标签模式文本均已水平居中且窗口外侧已有任务栏边距，不再二次内缩裁剪。
    const float inset = (stacked || hideLabels) ? 0.0f : kVerticalResourceInset;
    const auto drawText = [&](std::wstring_view text, float y, float opacityScale) {
        if (text.empty() || opacityScale <= 0.001f)
            return;
        const float previousOpacity = brush->GetOpacity();
        if (opacityScale < 0.999f)
            brush->SetOpacity(previousOpacity * opacityScale);
        rt->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format,
                      D2D1::RectF(area.left + inset, y,
                                  area.right - inset, y + verticalLineHeight_),
                      brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        if (opacityScale < 0.999f)
            brush->SetOpacity(previousOpacity);
    };
    for (size_t index = first; index < std::min(first + 2, values.size()); ++index) {
        const auto& reading = values[index];
        if (hideLabels) {
            if (index != first)
                top += kVerticalResourceGroupGap;
            const float labelAlpha = transientLabelAlpha_;
            if (labelAlpha < 0.999f)
                drawText(shortVerticalValue(reading.value), top, 1.0f - labelAlpha);
            if (labelAlpha > 0.001f)
                drawText(reading.label, top, labelAlpha);
            top += verticalLineHeight_;
        } else if (stacked) {
            if (index != first)
                top += kVerticalResourceGroupGap;
            drawText(reading.label, top, 1.0f);
            top += verticalLineHeight_ + kVerticalResourceLabelGap;
            drawText(shortVerticalValue(reading.value), top, 1.0f);
            top += verticalLineHeight_;
        } else {
            drawText(std::wstring(reading.label) + L" " + reading.value, top, 1.0f);
            top += verticalLineHeight_;
        }
    }
    format->SetTextAlignment(previousAlignment);
}

std::wstring ResourcePanel::formatResourcePercent(double percent) {
    wchar_t text[16]{};
    swprintf_s(text, L"%.0f%%", std::clamp(percent, 0.0, 100.0));
    return text;
}

std::wstring ResourcePanel::formatNetworkRate(uint64_t bytesPerSecond) {
    static constexpr const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s"};
    double value = static_cast<double>(bytesPerSecond);
    size_t unit = 0;
    while (std::round(value) >= 1000.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    if (unit + 1 == std::size(units))
        value = std::min(value, 999.0);

    const double roundedTenths = std::round(value * 10.0) / 10.0;
    wchar_t text[32]{};
    if (unit == 0 || roundedTenths >= 100.0)
        swprintf_s(text, L"%.0f%ls", std::round(value), units[unit]);
    else
        swprintf_s(text, L"%.1f%ls", roundedTenths, units[unit]);
    return text;
}

std::wstring ResourcePanel::formatFrequency(bool available, double gigahertz) {
    if (!available)
        return L"—";
    wchar_t text[24]{};
    swprintf_s(text, L"%.1fGHz", std::clamp(gigahertz, 0.0, 99.0));
    return text;
}

void ResourcePanel::start() {
    resourceCpuText_ = L"—";
    resourceMemoryText_ = L"—";
    resourceDownloadText_ = L"—";
    resourceUploadText_ = L"—";
    resourceGpuText_ = L"—";
    resourceCpuFrequencyText_ = L"—";
    resourceSnapshotRevision_ = UINT64_MAX;
    nextResourceSnapshotPollMs_ = 0;
    resourceMonitor_.start();
}

void ResourcePanel::stop() {
    resourceMonitor_.stop();
    nextResourceSnapshotPollMs_ = 0;
    clearTransientLabels();
}

void ResourcePanel::triggerTransientLabels(ULONGLONG now) {
    if (!dockResourceVisibility_.hideLabels)
        return;
    transientLabelActive_ = true;
    transientLabelStartMs_ = now != 0 ? now : monotonicNowMs();
    transientLabelAlpha_ = 1.0f;
}

void ResourcePanel::clearTransientLabels() {
    transientLabelActive_ = false;
    transientLabelStartMs_ = 0;
    transientLabelAlpha_ = 0.0f;
}

bool ResourcePanel::transientLabelsFading(ULONGLONG now, bool animate) const {
    if (!transientLabelActive_ || !animate)
        return false;
    const ULONGLONG elapsed = now >= transientLabelStartMs_ ? now - transientLabelStartMs_ : 0;
    return elapsed >= kTransientLabelHoldMs &&
           elapsed < kTransientLabelHoldMs + kTransientLabelFadeMs;
}

bool ResourcePanel::tickTransientLabels(ULONGLONG now, bool animate) {
    if (!transientLabelActive_)
        return false;
    const ULONGLONG elapsed = now >= transientLabelStartMs_ ? now - transientLabelStartMs_ : 0;
    const ULONGLONG fadeMs = animate ? kTransientLabelFadeMs : 0;
    if (elapsed >= kTransientLabelHoldMs + fadeMs) {
        transientLabelActive_ = false;
        transientLabelStartMs_ = 0;
        const bool changed = transientLabelAlpha_ > 0.0f;
        transientLabelAlpha_ = 0.0f;
        return changed;
    }
    float nextAlpha = 1.0f;
    if (elapsed >= kTransientLabelHoldMs && fadeMs > 0) {
        const float t =
            static_cast<float>(elapsed - kTransientLabelHoldMs) / static_cast<float>(fadeMs);
        nextAlpha = 1.0f - t * t * (3.0f - 2.0f * t);
    }
    if (std::abs(nextAlpha - transientLabelAlpha_) > 0.002f) {
        transientLabelAlpha_ = nextAlpha;
        return true;
    }
    return false;
}

bool ResourcePanel::refresh() {
    const ULONGLONG now = monotonicNowMs();
    if (now < nextResourceSnapshotPollMs_)
        return false;
    nextResourceSnapshotPollMs_ = now + kResourceSnapshotPollMs;

    const system_monitor::ResourceSnapshot snapshot = resourceMonitor_.snapshot();
    if (snapshot.revision == resourceSnapshotRevision_)
        return false;
    resourceSnapshotRevision_ = snapshot.revision;
    resourceCpuText_ =
        snapshot.cpuAvailable ? ResourcePanel::formatResourcePercent(snapshot.cpuPercent) : L"—";
    resourceMemoryText_ = snapshot.memoryAvailable
                              ? ResourcePanel::formatResourcePercent(snapshot.memoryPercent)
                              : L"—";
    resourceDownloadText_ = snapshot.networkAvailable
                                ? ResourcePanel::formatNetworkRate(snapshot.downloadBytesPerSecond)
                                : L"—";
    resourceUploadText_ = snapshot.networkAvailable
                              ? ResourcePanel::formatNetworkRate(snapshot.uploadBytesPerSecond)
                              : L"—";
    resourceGpuText_ =
        snapshot.gpuAvailable ? ResourcePanel::formatResourcePercent(snapshot.gpuPercent) : L"—";
    resourceCpuFrequencyText_ =
        ResourcePanel::formatFrequency(snapshot.cpuFrequencyAvailable, snapshot.cpuFrequencyGHz);
    return true;
}

void ResourcePanel::draw(ID2D1DeviceContext* rt, IDWriteTextFormat* format, ID2D1Brush* brush,
                         float x, float h, float width) const {
    if (!rt || !format || width <= 0.0f || !brush)
        return;

    const auto rows = ResourcePanel::rows();
    const size_t pageCount = compact_ ? rows.size() : rows.size() / 2;
    if (rows.empty() || pageCount == 0)
        return;
    const size_t page = std::min(dockResourcePage_, pageCount - 1);
    const float gap = columnGap();
    const bool hideLabels = dockResourceVisibility_.hideLabels;

    auto drawText = [&](const std::wstring& text, const D2D1_RECT_F& rect,
                        D2D1_DRAW_TEXT_OPTIONS options, float opacityScale) {
        if (text.empty() || opacityScale <= 0.001f)
            return;
        const float previousOpacity = brush->GetOpacity();
        if (opacityScale < 0.999f)
            brush->SetOpacity(previousOpacity * opacityScale);
        rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush, options,
                      DWRITE_MEASURING_MODE_NATURAL);
        if (opacityScale < 0.999f)
            brush->SetOpacity(previousOpacity);
    };
    auto drawCell = [&](const DockResourceCell& cell, float left, float rowTop, float cellW,
                        float cellH, D2D1_DRAW_TEXT_OPTIONS options) {
        if (cell.value.empty() && cell.label.empty())
            return;
        if (cellW <= 0.0f || cellH <= 0.0f)
            return;
        const D2D1_RECT_F rect = D2D1::RectF(left, rowTop, left + cellW, rowTop + cellH);
        if (!hideLabels) {
            drawText(cell.value, rect, options, 1.0f);
            return;
        }
        const float labelAlpha = transientLabelAlpha_;
        if (labelAlpha < 0.999f)
            drawText(cell.value, rect, options, 1.0f - labelAlpha);
        if (labelAlpha > 0.001f)
            drawText(cell.label, rect, options, labelAlpha);
    };

    if (compact_) {
        const std::array<const DockResourceCell*, 2> cells{&rows[page].left, &rows[page].right};
        rt->PushAxisAlignedClip(D2D1::RectF(x, 0.0f, x + width, h), D2D1_ANTIALIAS_MODE_ALIASED);
        float cellX = x;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (cells[i]->value.empty() && cells[i]->label.empty())
                continue;
            const float cellRight = std::min(x + width, cellX + resourceSingleLineWidths_[i]);
            if (cellRight <= cellX)
                break;
            drawCell(*cells[i], cellX, 0.0f, cellRight - cellX, h, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            cellX += resourceSingleLineWidths_[i] + gap;
        }
        rt->PopAxisAlignedClip();
        return;
    }

    if (resourcePageColumnWidths_.empty())
        return;
    std::array<DockResourceRow, 2> visibleRows{};
    visibleRows[0] = rows[page * 2];
    visibleRows[1] = rows[page * 2 + 1];
    const float maxBlockH = dockResourceVisibility_.smallFont ? 36.0f : 38.0f;
    const float blockH = std::min(maxBlockH, std::max(1.0f, h - 2.0f));
    const float top = std::max(0.0f, (h - blockH) * 0.5f);
    const float rowH = blockH * 0.5f;
    const auto& columnWidths =
        resourcePageColumnWidths_[std::min(page, resourcePageColumnWidths_.size() - 1)];
    const float leftColumnW = std::min(columnWidths[0], width);
    const float rightColumnX = x + leftColumnW + gap;
    const float rightColumnW = width - leftColumnW - gap;
    const D2D1_DRAW_TEXT_OPTIONS drawOptions = width >= ResourcePanel::width()
                                                   ? D2D1_DRAW_TEXT_OPTIONS_NONE
                                                   : D2D1_DRAW_TEXT_OPTIONS_CLIP;

    const bool singleRow =
        visibleRows[1].left.value.empty() && visibleRows[1].left.label.empty() &&
        visibleRows[1].right.value.empty() && visibleRows[1].right.label.empty();
    for (size_t row = 0; row < (singleRow ? 1u : visibleRows.size()); ++row) {
        const auto& current = visibleRows[row];
        const float rowTop = singleRow ? top + (blockH - rowH) * 0.5f : top + row * rowH;
        if (current.right.value.empty() && current.right.label.empty()) {
            drawCell(current.left, x, rowTop, width, rowH, drawOptions);
        } else {
            drawCell(current.left, x, rowTop, leftColumnW, rowH, drawOptions);
            drawCell(current.right, rightColumnX, rowTop, rightColumnW, rowH, drawOptions);
        }
    }
}

float ResourcePanel::measureResourceTextWidth(IDWriteFactory* dwrite, IDWriteTextFormat* format,
                                              const std::wstring& text, float* lineHeight) {
    if (!dwrite || !format || text.empty())
        return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                        0.0f, 0.0f, &layout)) ||
        !layout)
        return 0.0f;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    float measuredWidth = 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(layout->GetMetrics(&metrics))) {
        measuredWidth = std::max(measuredWidth, metrics.widthIncludingTrailingWhitespace);
        if (lineHeight)
            *lineHeight = std::max(*lineHeight, metrics.height);
    }
    DWRITE_OVERHANG_METRICS overhangs{};
    if (SUCCEEDED(layout->GetOverhangMetrics(&overhangs)))
        measuredWidth = std::max(measuredWidth, overhangs.right);
    layout->Release();
    return measuredWidth;
}

ResourcePanel::ColumnMetrics ResourcePanel::measureColumnMetrics(IDWriteFactory* dwrite,
                                                                 IDWriteTextFormat* format) const {
    ColumnMetrics result;
    if (!dwrite || !format)
        return result;

    wchar_t widestDigit = L'8';
    float widestDigitWidth = -1.0f;
    for (wchar_t digit = L'0'; digit <= L'9'; ++digit) {
        const float digitWidth =
            ResourcePanel::measureResourceTextWidth(dwrite, format, std::wstring(1, digit));
        if (digitWidth > widestDigitWidth) {
            widestDigitWidth = digitWidth;
            widestDigit = digit;
        }
    }

    const std::wstring singleDigit(1, widestDigit);
    const std::wstring twoDigits = singleDigit + singleDigit;
    const std::wstring threeDigits = twoDigits + singleDigit;
    const std::wstring decimalDigits = twoDigits + L"." + singleDigit;

    const auto values = readings();
    if (values.empty())
        return result;
    float verticalInlineWidth = 0.0f;
    float verticalStackedWidth = 0.0f;
    float verticalMinWidth = 0.0f;
    float verticalLineHeight = 0.0f;
    size_t cellIndex = 0;
    auto measureCell = [&](const std::vector<std::wstring>& samples) {
        const size_t indexInPage = cellIndex % 4;
        if (indexInPage == 0)
            result.pageColumnWidths.push_back({});
        float& columnWidth = result.pageColumnWidths.back()[indexInPage / 2];
        const bool hideLabels = dockResourceVisibility_.hideLabels;
        const auto& reading = values[cellIndex];
        const std::wstring label = reading.label;
        const std::wstring prefix = label + L": ";
        const float labelWidth = measureResourceTextWidth(dwrite, format, label, &verticalLineHeight);
        verticalMinWidth = std::max(verticalMinWidth, labelWidth);
        verticalStackedWidth = std::max(verticalStackedWidth, labelWidth);
        if (hideLabels) {
            columnWidth = std::max(columnWidth, labelWidth);
            result.singleLineWidths[cellIndex % 2] =
                std::max(result.singleLineWidths[cellIndex % 2], labelWidth);
        }
        for (const auto& sample : samples) {
            const float measured = measureResourceTextWidth(
                dwrite, format, hideLabels ? sample : prefix + sample, &verticalLineHeight);
            columnWidth = std::max(columnWidth, measured);
            result.singleLineWidths[cellIndex % 2] =
                std::max(result.singleLineWidths[cellIndex % 2], measured);
            verticalInlineWidth = std::max(
                verticalInlineWidth, measureResourceTextWidth(
                    dwrite, format, label + L" " + sample, &verticalLineHeight));
            verticalStackedWidth = std::max(
                verticalStackedWidth, measureResourceTextWidth(
                    dwrite, format, std::wstring(shortVerticalValue(sample))));
        }
        ++cellIndex;
    };
    if (dockResourceVisibility_.cpuUsage)
        measureCell({L"100%", twoDigits + L"%"});
    if (dockResourceVisibility_.memoryUsage)
        measureCell({L"100%", twoDigits + L"%"});
    static constexpr const wchar_t* rateUnits[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s"};
    auto measureRate = [&] {
        std::vector<std::wstring> samples;
        for (size_t i = 0; i < std::size(rateUnits); ++i) {
            if (i > 0)
                samples.push_back(decimalDigits + rateUnits[i]);
            samples.push_back(threeDigits + rateUnits[i]);
        }
        measureCell(samples);
    };
    if (dockResourceVisibility_.upload)
        measureRate();
    if (dockResourceVisibility_.download)
        measureRate();
    if (dockResourceVisibility_.gpuUsage)
        measureCell({L"100%", twoDigits + L"%"});
    if (dockResourceVisibility_.cpuFrequency)
        measureCell({decimalDigits + L"GHz"});

    for (auto& [left, right] : result.pageColumnWidths) {
        left = std::max(1.0f, std::ceil(left) + kResourceCellSafetyPadding);
        if (right > 0.0f)
            right = std::ceil(right) + kResourceCellSafetyPadding;
    }
    for (float& cellWidth : result.singleLineWidths)
        if (cellWidth > 0.0f)
            cellWidth = std::ceil(cellWidth) + kResourceCellSafetyPadding;

    result.verticalInlineWidth = std::ceil(verticalInlineWidth);
    result.verticalStackedWidth = verticalStackedWidth;
    result.verticalMinWidth = std::ceil(verticalMinWidth);
    result.verticalLineHeight = std::ceil(verticalLineHeight);
    return result;
}

float ResourcePanel::measureVerticalStackedWidth(IDWriteFactory* dwrite,
                                                 IDWriteTextFormat* format) const {
    return measureColumnMetrics(dwrite, format).verticalStackedWidth;
}

bool ResourcePanel::updateColumnWidths(IDWriteFactory* dwrite, IDWriteTextFormat* format) {
    if (!dwrite || !format)
        return false;

    ColumnMetrics measured = measureColumnMetrics(dwrite, format);
    const bool changed = measured.pageColumnWidths != resourcePageColumnWidths_ ||
                         measured.singleLineWidths != resourceSingleLineWidths_ ||
                         measured.verticalInlineWidth != verticalInlineWidth_ ||
                         measured.verticalStackedWidth != verticalStackedWidth_ ||
                         measured.verticalMinWidth != verticalMinWidth_ ||
                         measured.verticalLineHeight != verticalLineHeight_;
    resourcePageColumnWidths_.swap(measured.pageColumnWidths);
    resourceSingleLineWidths_ = measured.singleLineWidths;
    verticalInlineWidth_ = measured.verticalInlineWidth;
    verticalStackedWidth_ = measured.verticalStackedWidth;
    verticalMinWidth_ = measured.verticalMinWidth;
    verticalLineHeight_ = measured.verticalLineHeight;
    return changed;
}

bool ResourcePanel::advancePage(int steps) {
    const size_t pageCount = ResourcePanel::pageCount();
    if (pageCount <= 1 || steps == 0)
        return false;

    const long long count = static_cast<long long>(pageCount);
    long long next = static_cast<long long>(dockResourcePage_) + steps;
    next %= count;
    if (next < 0)
        next += count;
    dockResourcePage_ = static_cast<size_t>(next);
    if (dockResourceVisibility_.hideLabels)
        triggerTransientLabels(monotonicNowMs());
    return true;
}
