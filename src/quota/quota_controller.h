#pragma once

#include "quota/quota_provider.h"
#include "quota/quota_types.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct AppSettings;

// 额度结果线程消息载荷：工作线程分配，主线程接管所有权。
struct QuotaRefreshPayload {
    uint64_t generation = 0;
    std::vector<QuotaSnapshot> snapshots;
};

// 设置页显示所需的只读额度状态。
struct QuotaSettingsState {
    bool codexEnabled = true;
    bool deepSeekEnabled = true;
    bool codexLoginDetected = false;
    bool deepSeekKeyConfigured = false;
    bool refreshing = false;
    std::wstring codexStatus;
    std::wstring deepSeekStatus;
};

// AI 额度编排：读取本机凭据、按退避间隔触发后台查询，并把结果转成任务栏文案。
// 所有公开方法都在主线程调用；工作线程只做网络请求和解析。
class QuotaController {
public:
    using PublishCallback = std::function<void()>;

    QuotaController(AppSettings& settings, PublishCallback publish);
    ~QuotaController();

    QuotaController(const QuotaController&) = delete;
    QuotaController& operator=(const QuotaController&) = delete;

    void setTimerWindow(HWND window);
    // 设置加载完成后调用：探测本机登录状态并发出首次刷新。
    void initialize();
    // 退出前调用：置位取消标记并等待在途请求结束。
    void finish();
    void onTick();
    void requestRefresh(bool force);
    // 启用项、API Key 或登录文件变化后调用：丢弃旧代次结果并重新查询。
    void notifySettingsChanged();
    void onRefreshReady(std::unique_ptr<QuotaRefreshPayload> payload);
    // 成功写入凭据管理器返回 true；空字符串表示清除。
    bool saveDeepSeekKey(const std::wstring& key);
    void clearDeepSeekKey();

    QuotaSettingsState settingsState() const;
    std::vector<QuotaReading> panelReadings() const;
    // 任务栏是否会显示额度区（已启用且至少一个服务可用）。
    bool displayable() const;

private:
    bool providerEnabled(QuotaProvider provider) const;
    bool providerConfigured(QuotaProvider provider) const;
    bool refreshConfiguredState();
    void markLoading(const std::vector<QuotaProvider>& providers);
    const QuotaSnapshot* snapshotFor(QuotaProvider provider) const;
    void publish();
    void joinWorker();

    AppSettings& settings_;
    PublishCallback publish_;
    DWORD mainThread_ = GetCurrentThreadId();
    HWND timerWindow_ = nullptr;
    std::array<QuotaSnapshot, 2> snapshots_{};
    std::array<int64_t, 2> nextAttemptAt_{};
    bool codexLoginDetected_ = false;
    bool deepSeekKeyConfigured_ = false;
    bool inFlight_ = false;
    bool pendingRefresh_ = false;
    uint64_t generation_ = 0;
    std::atomic<bool> shutdown_{false};
    std::thread worker_;
};
