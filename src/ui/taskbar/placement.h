#pragma once
#include "model/display_types.h"
#include <windows.h>
#include <string>
#include <vector>
#include <span>
#include <utility>
namespace taskbar_detail {
struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int availableMajor = 0;
};
struct OccupiedRegions {
    bool vertical = false;
    bool hasNotify = false;
    bool hasStart = false;
    RECT notifyRect{};
    RECT startRect{};
    RECT trafficMonitor{};
    std::span<const RECT> buttons;
    std::span<const RECT> kugouWindows;
};
struct PlacementInput {
    RECT taskbar{};
    UINT edge = 0;
    bool vertical = false;
    bool allowOverlap = false;
    int positionMode = 0;
    double anchorRatio = 1.0;
    TaskbarAnchorEdge anchorEdge = TaskbarAnchorEdge::End;
    int gap = 0;
    int minMajor = 0;
    int compressedMinMajor = 0;
    int maxMajor = 0;
    int crossPixels = 0;
    int crossMargin = 0;
    // 同一个空白区内拆出“靠起始端/靠末端”两个可区分锚点所需的最小主轴位移（px）；<= 0 时取默认值。
    int minDistinctAnchorOffset = 0;
    std::vector<std::pair<int, int>> occupied;
};
struct TaskbarAnchorSlot {
    TaskbarAnchorSpec spec;
    WindowPlacement anchorPlacement;
    WindowPlacement previewPlacement;
    int spanStart = 0;
    int spanEnd = 0;
    int spanIndex = 0;
    int spanCount = 0;
    bool spanHasBothEdges = false;
};
std::vector<std::pair<int, int>> occupiedIntervals(const OccupiedRegions& input);
TaskbarPlacementStatus calculatePlacement(const PlacementInput& input, WindowPlacement& placement);
WindowPlacement dragPreview(const WindowPlacement& anchor, TaskbarAnchorEdge edge, bool vertical,
                            int desiredMajor);
WindowPlacement dragPreview(const WindowPlacement& anchor, int mode, bool vertical,
                            int desiredMajor);
std::vector<TaskbarAnchorSlot> collectAvailableAnchors(const PlacementInput& input,
                                                       int desiredMajor = 0);
int findActiveAnchorIndex(const std::vector<TaskbarAnchorSlot>& anchors,
                          const TaskbarAnchorSpec& preferred,
                          const WindowPlacement& currentPlacement, bool vertical);
int selectDragAnchorIndex(const std::vector<TaskbarAnchorSlot>& anchors, int effectiveMajor,
                          bool vertical, int currentIndex, int hysteresisPx);
std::wstring formatTaskbarAnchorLabel(const std::vector<TaskbarAnchorOption>& options,
                                      size_t index);
} // namespace taskbar_detail
