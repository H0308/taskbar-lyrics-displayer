#include "update/startup_update_check.h"

#include "logging/runtime_logger.h"
#include "ui/ui_dispatcher.h"

struct StartupUpdateCheck::Impl {
    UiDispatcher dispatcher;
    update_service::UpdateSource source = update_service::UpdateSource::GitHub;
    std::optional<update_service::UpdateCheckResult> result;
};

StartupUpdateCheck::StartupUpdateCheck(
    bool useGiteeUpdateSource, std::function<void(const std::wstring&)> onAvailable)
    : impl_(std::make_unique<Impl>()) {
    impl_->source = useGiteeUpdateSource ? update_service::UpdateSource::Gitee
                                         : update_service::UpdateSource::GitHub;
    const uint64_t request = update_service::nextRequestId();
    runtime_log::writef(L"[action][update] check-start source=%s request=%llu",
                        update_service::updateSourceName(impl_->source),
                        static_cast<unsigned long long>(request));
    std::function<void(update_service::UpdateCheckResult)> receive =
        [this, onAvailable = std::move(onAvailable)](update_service::UpdateCheckResult result) {
            runtime_log::writef(L"[action][update] check-result state=%s version=%s source=%s",
                                update_service::updateStateName(result.state),
                                result.version.c_str(),
                                update_service::updateSourceName(impl_->source));
            if (result.state != update_service::UpdateState::Available)
                return;
            const std::wstring version = update_service::displayVersion(result.version);
            // 仅保留新版本结果；普通结果和网络响应在本次回调结束后释放。
            impl_->result = std::move(result);
            if (onAvailable)
                onAvailable(version);
        };
    update_service::requestLatestRelease(request, impl_->source,
                                         impl_->dispatcher.callback(std::move(receive)));
}

StartupUpdateCheck::~StartupUpdateCheck() {
    // 后台请求只持有派发器的弱引用，关闭队列后不能再调用 App 或本对象。
    impl_->dispatcher.close();
}

std::optional<update_service::UpdateCheckResult> StartupUpdateCheck::takeAvailableResult(
    update_service::UpdateSource source) {
    if (impl_->source != source || !impl_->result)
        return std::nullopt;
    auto taken = std::move(impl_->result);
    impl_->result.reset();
    return taken;
}
