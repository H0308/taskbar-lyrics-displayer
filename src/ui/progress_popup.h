#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <windows.h>

// 任务栏内嵌控件进度按钮悬停时弹出的单行磨砂进度滑块浮窗。
// 独立的非激活置顶弹出窗，只消费宿主推送的播放进度并通过回调上报跳转结果。
class ProgressPopup {
public:
    ProgressPopup();
    ~ProgressPopup();

    ProgressPopup(const ProgressPopup&) = delete;
    ProgressPopup& operator=(const ProgressPopup&) = delete;

    bool create(HINSTANCE inst);
    void destroy();

    void setProgress(uint64_t sessionGeneration, int64_t positionMs, int64_t durationMs,
                     bool available);
    void seekBy(int64_t deltaMs);
    // 点击/拖动松手或滚轮微调后触发跳转（sessionGeneration, positionMs）
    void setCallback(std::function<void(uint64_t, int64_t)> cb);

    // 横向任务栏显示在锚点上方/下方；侧边任务栏贴着任务栏左右显示，并按锚点垂直居中
    void showNear(const RECT& anchorRect, bool verticalTaskbar, bool popupOnRight);
    void hide();
    bool isVisible() const;

    // 锚点（进度按钮）悬停状态，配合浮窗自身悬停决定延迟隐藏
    void onAnchorEnter();
    void onAnchorLeave();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
