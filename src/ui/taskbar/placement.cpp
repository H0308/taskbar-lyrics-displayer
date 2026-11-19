#include "ui/taskbar/placement.h"
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
namespace taskbar_detail {
namespace {
struct Span {
    int l = 0;
    int r = 0;
};

int majorStart(const PlacementInput& input) {
    return input.vertical ? input.taskbar.top : input.taskbar.left;
}

int majorEnd(const PlacementInput& input) {
    return input.vertical ? input.taskbar.bottom : input.taskbar.right;
}

int usableMajor(const Span& s, int gap) { return s.r - s.l - gap * 2; }

int placementMajorStart(const WindowPlacement& placement, bool vertical) {
    return vertical ? placement.y : placement.x;
}

int placementMajorLength(const WindowPlacement& placement, bool vertical) {
    return vertical ? placement.height : placement.width;
}

bool sameGeometry(const WindowPlacement& a, const WindowPlacement& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

TaskbarAnchorEdge resolveAnchorEdge(const PlacementInput& input) {
    if (input.positionMode == 1)
        return TaskbarAnchorEdge::Start;
    if (input.positionMode == 2)
        return input.anchorEdge;
    return TaskbarAnchorEdge::End;
}

double edgeRatio(const Span& s, TaskbarAnchorEdge edge, int mStart, int mEnd) {
    const int total = mEnd - mStart;
    if (total <= 0)
        return 0.0;
    const int coord = edge == TaskbarAnchorEdge::Start ? s.l : s.r;
    return std::clamp(static_cast<double>(coord - mStart) / static_cast<double>(total), 0.0, 1.0);
}

double roundedEdgeRatio(const Span& s, TaskbarAnchorEdge edge, int mStart, int mEnd) {
    return std::round(edgeRatio(s, edge, mStart, mEnd) * 10000.0) / 10000.0;
}

// 过滤控件与边距之间的微小缝隙，避免把图标间隙误判为独立可用空白区。
std::vector<Span> freeSpans(const PlacementInput& input) {
    std::vector<Span> spans;
    const int mStart = majorStart(input);
    const int mEnd = majorEnd(input);
    if (mEnd <= mStart)
        return spans;

    const int halfCross = input.crossPixels > 0 ? input.crossPixels / 2 : 8;
    const int maxNoise =
        std::max(8, input.compressedMinMajor > 0 ? input.compressedMinMajor / 2 : 24);
    const int noisePx = std::clamp(halfCross, 8, maxNoise);

    int cursor = mStart;
    for (const auto& o : input.occupied) {
        if (o.first > cursor) {
            const int spanEnd = std::min(o.first, mEnd);
            if (spanEnd - cursor >= noisePx)
                spans.push_back({cursor, spanEnd});
        }
        cursor = std::max(cursor, std::min(o.second, mEnd));
    }
    if (cursor < mEnd && mEnd - cursor >= noisePx)
        spans.push_back({cursor, mEnd});
    // 只有确实没有探测到任何占用区间时，才把整条任务栏视为可用；
    // 如果占用区间已经覆盖整个主轴，必须保留“无空闲位置”的结果。
    if (spans.empty() && input.occupied.empty())
        spans.push_back({mStart, mEnd});
    return spans;
}

WindowPlacement placeInSpan(const PlacementInput& input, const Span& s, TaskbarAnchorEdge edge,
                            int w) {
    const int mStart = majorStart(input);
    const int mEnd = majorEnd(input);
    const int total = std::max(1, mEnd - mStart);
    const int availableMajor = std::max(1, usableMajor(s, input.gap));
    const int pxMajor = std::min(w, total);
    int major = edge == TaskbarAnchorEdge::Start ? s.l + input.gap : s.r - input.gap - pxMajor;
    if (pxMajor <= mEnd - mStart)
        major = std::clamp(major, mStart, mEnd - pxMajor);
    else
        major = mStart;

    int x = 0;
    int y = 0;
    if (input.vertical) {
        x = input.edge == ABE_LEFT
                ? input.taskbar.left + input.crossMargin
                : input.taskbar.right - input.crossMargin - input.crossPixels;
        y = major;
        if (y < input.taskbar.top)
            y = input.taskbar.top;
        if (pxMajor <= mEnd - mStart && y + pxMajor > input.taskbar.bottom)
            y = input.taskbar.bottom - pxMajor;
    } else {
        x = major;
        y = input.taskbar.top + input.crossMargin;
        if (x < input.taskbar.left)
            x = input.taskbar.left;
    }

    WindowPlacement placement;
    placement.x = x;
    placement.y = y;
    placement.width = input.vertical ? input.crossPixels : pxMajor;
    placement.height = input.vertical ? pxMajor : input.crossPixels;
    placement.availableMajor = availableMajor;
    return placement;
}

long long previewCenterDist2(const TaskbarAnchorSlot& slot, int effectiveMajor, bool vertical) {
    const int start = placementMajorStart(slot.previewPlacement, vertical);
    const int length = placementMajorLength(slot.previewPlacement, vertical);
    return std::llabs(static_cast<long long>(effectiveMajor) * 2 -
                      (static_cast<long long>(start) * 2 + length));
}
} // namespace

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

int previewMajorPixels(const WindowPlacement& anchor, bool vertical, int desiredMajor) {
    const int anchorMajor = anchor.availableMajor > 0 ? anchor.availableMajor
                                                      : (vertical ? anchor.height : anchor.width);
    const int currentMajor =
        desiredMajor > 0 ? desiredMajor : (vertical ? anchor.height : anchor.width);
    return std::clamp(currentMajor, 1, std::max(1, anchorMajor));
}

WindowPlacement dragPreview(const WindowPlacement& anchor, TaskbarAnchorEdge edge, bool vertical,
                            int desiredMajor) {
    WindowPlacement preview = anchor;
    const int previewMajor = previewMajorPixels(anchor, vertical, desiredMajor);
    if (vertical) {
        preview.height = previewMajor;
        if (edge == TaskbarAnchorEdge::End)
            preview.y = anchor.y + anchor.height - preview.height;
    } else {
        preview.width = previewMajor;
        if (edge == TaskbarAnchorEdge::End)
            preview.x = anchor.x + anchor.width - preview.width;
    }
    return preview;
}

WindowPlacement dragPreview(const WindowPlacement& anchor, int mode, bool vertical,
                            int desiredMajor) {
    return dragPreview(anchor, mode == 1 ? TaskbarAnchorEdge::Start : TaskbarAnchorEdge::End,
                       vertical, desiredMajor);
}

TaskbarPlacementStatus calculatePlacement(const PlacementInput& input, WindowPlacement& placement) {
    const int mStart = majorStart(input);
    const int mEnd = majorEnd(input);
    if (mEnd <= mStart || input.crossPixels <= 0)
        return TaskbarPlacementStatus::Unavailable;

    const std::vector<Span> spans = freeSpans(input);
    const TaskbarAnchorEdge align = resolveAnchorEdge(input);
    TaskbarPlacementStatus result = TaskbarPlacementStatus::Unavailable;

    if (spans.empty()) {
        if (!input.allowOverlap)
            return TaskbarPlacementStatus::NoSpace;
        const Span full{mStart, mEnd};
        // 用户明确选择继续开启时，也沿用压缩后的最小主轴尺寸，
        // 不再恢复为标准最小尺寸，尽量降低对任务栏的遮挡。
        placement = placeInSpan(input, full, align, input.compressedMinMajor);
        result = TaskbarPlacementStatus::ForcedOverlap;
    } else {
        const Span* target = &spans.back();
        if (input.positionMode == 1) {
            target = &spans.front();
        } else if (input.positionMode == 2) {
            const double targetRatio = std::clamp(input.anchorRatio, 0.0, 1.0);
            double bestDiff = 0.0;
            target = nullptr;
            for (const auto& s : spans) {
                const double diff = std::abs(edgeRatio(s, align, mStart, mEnd) - targetRatio);
                if (!target || diff < bestDiff) {
                    target = &s;
                    bestDiff = diff;
                }
            }
        }

        const int targetUsable = usableMajor(*target, input.gap);
        if (targetUsable >= input.compressedMinMajor) {
            // 原位优先：被挤压先收缩长度
            placement = placeInSpan(input, *target, align, std::min(targetUsable, input.maxMajor));
            result = targetUsable < input.minMajor ? TaskbarPlacementStatus::Compressed
                                                   : TaskbarPlacementStatus::Safe;
        } else {
            // 原位压到安全最小长度仍放不下：换到容得下的最大空闲区。
            const Span* best = nullptr;
            for (const auto& s : spans) {
                if (usableMajor(s, input.gap) >= input.compressedMinMajor &&
                    (!best || s.r - s.l > best->r - best->l))
                    best = &s;
            }
            if (best) {
                const int bestUsable = usableMajor(*best, input.gap);
                placement =
                    placeInSpan(input, *best, align, std::min(bestUsable, input.maxMajor));
                result = TaskbarPlacementStatus::Relocated;
            } else if (input.allowOverlap) {
                // 用户已明确允许重叠时，仍把窗口限制在任务栏主轴范围内，
                // 只允许与已探测到的控件相交，不让窗口越出任务栏；尺寸使用
                // 压缩后的最小值，避免强制开启时又恢复为标准尺寸。
                placement = placeInSpan(input, *target, align, input.compressedMinMajor);
                result = TaskbarPlacementStatus::ForcedOverlap;
            } else {
                return TaskbarPlacementStatus::NoSpace;
            }
        }
    }

    if (placement.width <= 0 || placement.height <= 0)
        return TaskbarPlacementStatus::Unavailable;
    return result;
}

std::vector<TaskbarAnchorSlot> collectAvailableAnchors(const PlacementInput& input,
                                                       int desiredMajor) {
    std::vector<TaskbarAnchorSlot> out;
    const int mStart = majorStart(input);
    const int mEnd = majorEnd(input);
    if (mEnd <= mStart || input.crossPixels <= 0)
        return out;

    const int minDistinct = input.minDistinctAnchorOffset > 0 ? input.minDistinctAnchorOffset : 32;
    auto pushSlot = [&](const Span& s, int spanIndex, int spanCount, bool hasBothEdges,
                        const TaskbarAnchorSpec& spec, const WindowPlacement& anchorPl) {
        if (anchorPl.width <= 0 || anchorPl.height <= 0)
            return;
        TaskbarAnchorSlot slot;
        slot.spec = spec;
        slot.anchorPlacement = anchorPl;
        slot.previewPlacement = dragPreview(anchorPl, spec.edge, input.vertical, desiredMajor);
        slot.spanStart = s.l;
        slot.spanEnd = s.r;
        slot.spanIndex = spanIndex;
        slot.spanCount = spanCount;
        slot.spanHasBothEdges = hasBothEdges;
        out.push_back(slot);
    };

    const std::vector<Span> spans = freeSpans(input);
    std::vector<Span> usableSpans;
    for (const auto& s : spans) {
        const int avail = usableMajor(s, input.gap);
        if (avail >= input.compressedMinMajor && std::min(avail, input.maxMajor) > 0)
            usableSpans.push_back(s);
    }

    if (usableSpans.empty()) {
        if (!input.allowOverlap)
            return out;
        const Span full{mStart, mEnd};
        const int overlapMajor = std::max(1, input.compressedMinMajor);
        const WindowPlacement startPl =
            placeInSpan(input, full, TaskbarAnchorEdge::Start, overlapMajor);
        const WindowPlacement endPl =
            placeInSpan(input, full, TaskbarAnchorEdge::End, overlapMajor);
        const int startMajor = placementMajorStart(startPl, input.vertical);
        const int endMajor = placementMajorStart(endPl, input.vertical);
        const bool hasBothEdges = (endMajor - startMajor) >= minDistinct;
        if (hasBothEdges)
            pushSlot(full, 0, 1, true, TaskbarAnchorSpec::outerStart(), startPl);
        pushSlot(full, 0, 1, hasBothEdges, TaskbarAnchorSpec::outerEnd(), endPl);
        return out;
    }

    const size_t k = usableSpans.size();
    for (size_t i = 0; i < k; ++i) {
        const Span& s = usableSpans[i];
        const int w = std::min(usableMajor(s, input.gap), input.maxMajor);
        const WindowPlacement startPl = placeInSpan(input, s, TaskbarAnchorEdge::Start, w);
        const WindowPlacement endPl = placeInSpan(input, s, TaskbarAnchorEdge::End, w);
        const int startMajor = placementMajorStart(startPl, input.vertical);
        const int endMajor = placementMajorStart(endPl, input.vertical);
        const bool hasBothEdges = (endMajor - startMajor) >= minDistinct;
        const bool isFrontSpan = s.l == spans.front().l && s.r == spans.front().r;
        const bool isBackSpan = s.l == spans.back().l && s.r == spans.back().r;
        const int spanIndex = static_cast<int>(i);
        const int spanCount = static_cast<int>(k);

        if (hasBothEdges) {
            const TaskbarAnchorSpec startSpec =
                (i == 0 && isFrontSpan)
                    ? TaskbarAnchorSpec::outerStart()
                    : TaskbarAnchorSpec::relative(
                          roundedEdgeRatio(s, TaskbarAnchorEdge::Start, mStart, mEnd),
                          TaskbarAnchorEdge::Start);
            const TaskbarAnchorSpec endSpec =
                (i + 1 == k && isBackSpan)
                    ? TaskbarAnchorSpec::outerEnd()
                    : TaskbarAnchorSpec::relative(
                          roundedEdgeRatio(s, TaskbarAnchorEdge::End, mStart, mEnd),
                          TaskbarAnchorEdge::End);
            pushSlot(s, spanIndex, spanCount, true, startSpec, startPl);
            pushSlot(s, spanIndex, spanCount, true, endSpec, endPl);
        } else if (i + 1 == k && isBackSpan && !(i == 0 && isFrontSpan && k > 1)) {
            pushSlot(s, spanIndex, spanCount, false, TaskbarAnchorSpec::outerEnd(), endPl);
        } else if (i == 0 && isFrontSpan) {
            pushSlot(s, spanIndex, spanCount, false, TaskbarAnchorSpec::outerStart(), startPl);
        } else {
            pushSlot(s, spanIndex, spanCount, false,
                     TaskbarAnchorSpec::relative(
                         roundedEdgeRatio(s, TaskbarAnchorEdge::Start, mStart, mEnd),
                         TaskbarAnchorEdge::Start),
                     startPl);
        }
    }
    return out;
}

int findActiveAnchorIndex(const std::vector<TaskbarAnchorSlot>& anchors,
                          const TaskbarAnchorSpec& preferred,
                          const WindowPlacement& currentPlacement, bool vertical) {
    if (anchors.empty())
        return -1;
    const bool hasCurrent = currentPlacement.width > 0 && currentPlacement.height > 0;
    if (hasCurrent) {
        for (size_t i = 0; i < anchors.size(); ++i) {
            if (anchors[i].spec == preferred &&
                sameGeometry(anchors[i].anchorPlacement, currentPlacement))
                return static_cast<int>(i);
        }
    }

    int bestIndex = 0;
    int bestDistance = 0;
    bool bestSpecMatch = false;
    bool bestEdgeMatch = false;
    const int currentMajor = placementMajorStart(currentPlacement, vertical);
    for (size_t i = 0; i < anchors.size(); ++i) {
        const auto& slot = anchors[i];
        const int distance =
            hasCurrent ? std::abs(placementMajorStart(slot.anchorPlacement, vertical) - currentMajor)
                       : 0;
        const bool specMatch = slot.spec == preferred;
        const bool edgeMatch = slot.spec.edge == preferred.edge;
        const bool better =
            i == 0 || distance < bestDistance ||
            (distance == bestDistance &&
             (specMatch != bestSpecMatch ? specMatch : (edgeMatch && !bestEdgeMatch)));
        if (better) {
            bestIndex = static_cast<int>(i);
            bestDistance = distance;
            bestSpecMatch = specMatch;
            bestEdgeMatch = edgeMatch;
        }
    }
    return bestIndex;
}

int selectDragAnchorIndex(const std::vector<TaskbarAnchorSlot>& anchors, int effectiveMajor,
                          bool vertical, int currentIndex, int hysteresisPx) {
    if (anchors.empty())
        return -1;
    if (anchors.size() == 1)
        return 0;

    const int hysteresis = std::max(0, hysteresisPx);
    for (size_t i = 0; i < anchors.size(); ++i) {
        const auto& slot = anchors[i];
        if (effectiveMajor < slot.spanStart || effectiveMajor > slot.spanEnd)
            continue;
        int firstIdx = static_cast<int>(i);
        int secondIdx = -1;
        for (size_t j = i + 1; j < anchors.size(); ++j) {
            if (anchors[j].spanIndex == slot.spanIndex) {
                secondIdx = static_cast<int>(j);
                break;
            }
        }
        if (secondIdx < 0)
            return firstIdx;
        const long long mid = (static_cast<long long>(slot.spanStart) + slot.spanEnd) / 2;
        if (currentIndex == firstIdx && effectiveMajor <= mid + hysteresis)
            return firstIdx;
        if (currentIndex == secondIdx && effectiveMajor >= mid - hysteresis)
            return secondIdx;
        return effectiveMajor < mid ? firstIdx : secondIdx;
    }

    int bestIdx = 0;
    long long bestDist2 = previewCenterDist2(anchors[0], effectiveMajor, vertical);
    for (size_t i = 1; i < anchors.size(); ++i) {
        const long long dist2 = previewCenterDist2(anchors[i], effectiveMajor, vertical);
        if (dist2 < bestDist2) {
            bestDist2 = dist2;
            bestIdx = static_cast<int>(i);
        }
    }
    if (currentIndex >= 0 && currentIndex < static_cast<int>(anchors.size()) &&
        currentIndex != bestIdx) {
        const long long currentDist2 =
            previewCenterDist2(anchors[static_cast<size_t>(currentIndex)], effectiveMajor, vertical);
        if (bestDist2 + static_cast<long long>(hysteresis) * 2 >= currentDist2)
            return currentIndex;
    }
    return bestIdx;
}

std::wstring formatTaskbarAnchorLabel(const std::vector<TaskbarAnchorOption>& options,
                                      size_t index) {
    if (index >= options.size())
        return {};
    const auto& opt = options[index];
    const bool v = opt.vertical;
    const wchar_t* edgeText =
        opt.spec.edge == TaskbarAnchorEdge::Start ? (v ? L"靠上" : L"靠左")
                                                  : (v ? L"靠下" : L"靠右");
    const wchar_t* startArea = v ? L"上方空白区" : L"左侧空白区";
    const wchar_t* endArea = v ? L"下方空白区" : L"右侧空白区";

    std::wstring areaText;
    if (opt.spanCount == 1) {
        if (!opt.spanHasBothEdges ||
            (options.front().spec.mode == 1 && options.back().spec.mode == 0)) {
            areaText = L"空白区";
        } else if (options.front().spec.mode == 1) {
            areaText = startArea;
        } else if (options.back().spec.mode == 0) {
            areaText = endArea;
        } else {
            areaText = L"中间空白区";
        }
    } else if (opt.spanCount == 2) {
        areaText = opt.spanIndex == 0 ? startArea : endArea;
    } else if (opt.spanCount == 3) {
        areaText = opt.spanIndex == 0 ? startArea : (opt.spanIndex == 1 ? L"中间空白区" : endArea);
    } else {
        areaText = L"空白区 " + std::to_wstring(opt.spanIndex + 1);
    }

    if (!opt.spanHasBothEdges)
        return areaText;
    areaText += L" · ";
    areaText += edgeText;
    return areaText;
}
} // namespace taskbar_detail
