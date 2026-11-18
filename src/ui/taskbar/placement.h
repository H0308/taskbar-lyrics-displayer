#pragma once
#include "model/display_types.h"
#include <windows.h>
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
    // 位置模式：0/1 = 第一空白区居左/居右；
    // 2/3 = 第二空白区居左/居右（任务栏只剩一处空白区时自动等同 0/1）。
    int positionMode = 0;
    int gap = 0;
    int minMajor = 0;
    int compressedMinMajor = 0;
    int maxMajor = 0;
    int crossPixels = 0;
    int crossMargin = 0;
    std::vector<std::pair<int, int>> occupied;
};
std::vector<std::pair<int, int>> occupiedIntervals(const OccupiedRegions& input);
TaskbarPlacementStatus calculatePlacement(const PlacementInput& input, WindowPlacement& placement);
} // namespace taskbar_detail
