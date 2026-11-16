#pragma once

#include "update/update_service.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

// 启动检查只接收网络结果，不创建关于窗口或绘图资源。
class StartupUpdateCheck {
public:
    StartupUpdateCheck(bool useGiteeUpdateSource,
                       std::function<void(const std::wstring&)> onAvailable);
    ~StartupUpdateCheck();
    StartupUpdateCheck(const StartupUpdateCheck&) = delete;
    StartupUpdateCheck& operator=(const StartupUpdateCheck&) = delete;

    // 若已检查到与指定更新源匹配的新版本，则移交并消费该结果供关于窗口复用。
    std::optional<update_service::UpdateCheckResult> takeAvailableResult(
        update_service::UpdateSource source);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
