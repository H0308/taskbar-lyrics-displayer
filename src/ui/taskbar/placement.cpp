#include "ui/taskbar/placement.h"
#include <shellapi.h>
#include <algorithm>
#include <cstdlib>
namespace taskbar_detail {
std::vector<std::pair<int, int>> occupiedIntervals(const OccupiedRegions& input) {
    std::vector<std::pair<int, int>> v;
    const bool vertical = input.vertical;
    auto add = [&v, vertical](const RECT& r) {
        const int start = vertical ? r.top : r.left;
        const int end = vertical ? r.bottom : r.right;
        if (end > start)
            v.emplace_back(start, end);
    };
    for (const RECT& r : input.buttons)
        add(r);
    if (input.hasNotify)
        add(input.notifyRect);
    if (input.hasStart)
        add(input.startRect);
    add(input.trafficMonitor);
    for (const RECT& r : input.kugouWindows)
        add(r);
    std::sort(v.begin(), v.end());
    std::vector<std::pair<int, int>> merged;
    for (const auto& p : v) {
        if (!merged.empty() && p.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, p.second);
        else
            merged.push_back(p);
    }
    return merged;
}
TaskbarPlacementStatus calculatePlacement(const PlacementInput& input, WindowPlacement& placement) {
    // 空闲区间 = 任务栏主轴减去占用区间
    struct Span {
        int l, r;
    };
    std::vector<Span> spans;
    const int majorStart = input.vertical ? input.taskbar.top : input.taskbar.left;
    const int majorEnd = input.vertical ? input.taskbar.bottom : input.taskbar.right;
    int cursor = majorStart;
    const auto& occupied = input.occupied;
    for (const auto& o : occupied) {
        if (o.first > cursor)
            spans.push_back({cursor, std::min(o.first, majorEnd)});
        cursor = std::max(cursor, std::min(o.second, majorEnd));
    }
    if (cursor < majorEnd)
        spans.push_back({cursor, majorEnd});
    // 只有确实没有探测到任何占用区间时，才把整条任务栏视为可用；
    // 如果占用区间已经覆盖整个主轴，必须保留“无空闲位置”的结果。
    if (spans.empty() && occupied.empty())
        spans.push_back({majorStart, majorEnd});

    auto usableMajor = [gap = input.gap](const Span& s) { return s.r - s.l - gap * 2; };

    int pxMajor = 0;
    int availableMajor = 0;
    int x = 0;
    int y = 0;
    // align: 0 = 居左，1 = 居右（模式 1/3 为居右）。
    int align = input.positionMode % 2;
    auto place = [&](const Span& s, int w) {
        availableMajor = std::max(1, usableMajor(s));
        pxMajor = std::min(w, std::max(1, majorEnd - majorStart));
        int major;
        if (align == 0) {
            major = s.l + input.gap;
        } else {
            major = s.r - input.gap - pxMajor;
        }
        if (pxMajor <= majorEnd - majorStart)
            major = std::clamp(major, majorStart, majorEnd - pxMajor);
        else
            major = majorStart;
        if (input.vertical) {
            x = input.edge == ABE_LEFT
                    ? input.taskbar.left + input.crossMargin
                    : input.taskbar.right - input.crossMargin - input.crossPixels;
            y = major;
        } else {
            x = major;
            y = input.taskbar.top + input.crossMargin;
        }
    };
    TaskbarPlacementStatus result = TaskbarPlacementStatus::Unavailable;
    if (spans.empty()) {
        if (!input.allowOverlap)
            return TaskbarPlacementStatus::NoSpace;
        const Span full{majorStart, majorEnd};
        // 用户明确选择继续开启时，也沿用压缩后的最小主轴尺寸，
        // 不再恢复为标准最小尺寸，尽量降低对任务栏的遮挡。
        place(full, input.compressedMinMajor);
        result = TaskbarPlacementStatus::ForcedOverlap;
    } else {
        // 目标空白区：模式 0/1 取第一空白区，2/3 取第二空白区（单空白区时二者相同，
        // 天然满足"2、3 自动等同 0、1"）。
        const Span* target = (input.positionMode == 0 || input.positionMode == 1)
                                 ? &spans.front()
                                 : &spans.back();
        if (usableMajor(*target) >= input.compressedMinMajor) {
            // 原位优先：被挤压先收缩长度
            place(*target, std::min(usableMajor(*target), input.maxMajor));
            result = usableMajor(*target) < input.minMajor ? TaskbarPlacementStatus::Compressed
                                                           : TaskbarPlacementStatus::Safe;
        } else {
            // 原位压到安全最小长度仍放不下：就近换到容得下的空闲区。
            const Span* rec = nullptr;
            long long bestScore = 0;
            const long long ref = align == 0 ? target->l : target->r;
            for (const auto& s : spans) {
                if (usableMajor(s) < input.compressedMinMajor)
                    continue;
                const int w = std::min(usableMajor(s), input.maxMajor);
                if (w <= 0)
                    continue;
                const int low = s.l + input.gap;
                const int high = std::max(low, s.r - input.gap - w);
                const int candidate = align == 0 ? low : high;
                const long long score = std::llabs((long long)candidate - ref);
                if (!rec || score < bestScore) {
                    rec = &s;
                    bestScore = score;
                }
            }
            if (rec) {
                place(*rec, std::min(usableMajor(*rec), input.maxMajor));
                result = TaskbarPlacementStatus::Relocated;
            } else if (input.allowOverlap) {
                // 用户已明确允许重叠时，仍把窗口限制在任务栏主轴范围内，
                // 只允许与已探测到的控件相交，不让窗口越出任务栏；尺寸使用
                // 压缩后的最小值，避免强制开启时又恢复为标准尺寸。
                place(*target, input.compressedMinMajor);
                result = TaskbarPlacementStatus::ForcedOverlap;
            } else {
                return TaskbarPlacementStatus::NoSpace;
            }
        }
    }
    if (input.vertical) {
        if (y < input.taskbar.top)
            y = input.taskbar.top;
        if (pxMajor <= majorEnd - majorStart && y + pxMajor > input.taskbar.bottom)
            y = input.taskbar.bottom - pxMajor;
    } else if (x < input.taskbar.left) {
        x = input.taskbar.left;
    }

    placement.x = x;
    placement.y = y;
    placement.width = input.vertical ? input.crossPixels : pxMajor;
    placement.height = input.vertical ? pxMajor : input.crossPixels;
    placement.availableMajor = availableMajor;
    if (placement.width <= 0 || placement.height <= 0)
        return TaskbarPlacementStatus::Unavailable;
    return result;
}
} // namespace taskbar_detail
