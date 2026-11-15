#pragma once

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <system_error>
#include <thread>

namespace taskbar_detail {

class FrameScheduler {
public:
    ~FrameScheduler() { stop(); }

    bool start(HWND window, UINT message, double intervalMs) {
        stop();
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
        changed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        stopped_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!timer_ || !changed_ || !stopped_) {
            stop();
            return false;
        }
        intervalMs_.store(intervalMs);
        try {
            worker_ = std::thread([this, window, message] {
                using Clock = std::chrono::steady_clock;
                auto next = Clock::now();
                const HANDLE handles[]{stopped_, changed_, timer_};
                for (;;) {
                    const auto period = std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double, std::milli>(intervalMs_.load()));
                    next += period;
                    const auto now = Clock::now();
                    if (next <= now)
                        next = now + period;
                    LARGE_INTEGER due{};
                    due.QuadPart = -std::max<LONGLONG>(1,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(next - now).count() / 100);
                    if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
                        return;
                    const DWORD result = WaitForMultipleObjects(3, handles, FALSE, INFINITE);
                    if (result == WAIT_OBJECT_0 + 1) {
                        next = Clock::now();
                        continue;
                    }
                    if (result != WAIT_OBJECT_0 + 2)
                        return;
                    if (!pending_.exchange(true) && !PostMessageW(window, message, 0, 0))
                        pending_.store(false);
                }
            });
        } catch (const std::system_error&) {
            stop();
            return false;
        }
        return true;
    }

    void setInterval(double intervalMs) {
        if (intervalMs_.exchange(intervalMs) != intervalMs && changed_)
            SetEvent(changed_);
    }

    void acknowledge() { pending_.store(false); }

    void stop() {
        if (stopped_)
            SetEvent(stopped_);
        if (worker_.joinable())
            worker_.join();
        if (timer_)
            CloseHandle(timer_);
        if (changed_)
            CloseHandle(changed_);
        if (stopped_)
            CloseHandle(stopped_);
        timer_ = changed_ = stopped_ = nullptr;
        pending_.store(false);
    }

private:
    HANDLE timer_ = nullptr;
    HANDLE changed_ = nullptr;
    HANDLE stopped_ = nullptr;
    std::thread worker_;
    std::atomic<double> intervalMs_{33.0};
    std::atomic<bool> pending_{false};
};

}
