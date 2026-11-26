#include "quota/quota_controller.h"

#include "app/messages.h"
#include "logging/runtime_logger.h"
#include "settings/app_settings.h"

#include <curl/curl.h>

#include <algorithm>
#include <ctime>
#include <cwctype>
#include <utility>

namespace {

constexpr int kQuotaMinimumRetrySeconds = 30;

size_t providerIndex(QuotaProvider provider) {
    return static_cast<size_t>(provider);
}

int64_t nowSeconds() {
    return static_cast<int64_t>(std::time(nullptr));
}

std::wstring trimmedKey(const std::wstring& text) {
    std::wstring key = text;
    key.erase(std::remove_if(key.begin(), key.end(),
                             [](wchar_t character) { return iswspace(character) != 0; }),
              key.end());
    return key;
}

} // namespace

QuotaController::QuotaController(AppSettings& settings, PublishCallback publish)
    : settings_(settings), publish_(std::move(publish)) {
    for (size_t index = 0; index < snapshots_.size(); ++index) {
        snapshots_[index].provider = static_cast<QuotaProvider>(index);
        snapshots_[index].status = QuotaStatus::NotConfigured;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

QuotaController::~QuotaController() {
    finish();
}

void QuotaController::setTimerWindow(HWND window) {
    timerWindow_ = window;
}

void QuotaController::initialize() {
    refreshConfiguredState();
    if (settings_.quotaEnabled_)
        requestRefresh(true);
    else
        publish();
}

void QuotaController::finish() {
    shutdown_.store(true);
    joinWorker();
    inFlight_ = false;
    // 工作线程可能在退出前已投递结果；窗口销毁后不再处理，这里回收未取走的载荷。
    MSG message{};
    while (PeekMessageW(&message, nullptr, kMsgQuotaReady, kMsgQuotaReady, PM_REMOVE))
        delete reinterpret_cast<QuotaRefreshPayload*>(message.lParam);
}

void QuotaController::joinWorker() {
    if (worker_.joinable())
        worker_.join();
}

bool QuotaController::providerEnabled(QuotaProvider provider) const {
    return provider == QuotaProvider::Codex ? settings_.quotaCodexEnabled_
                                            : settings_.quotaDeepSeekEnabled_;
}

bool QuotaController::providerConfigured(QuotaProvider provider) const {
    return provider == QuotaProvider::Codex ? codexLoginDetected_ : deepSeekKeyConfigured_;
}

bool QuotaController::refreshConfiguredState() {
    const bool codex = QuotaProviderClient::codexLoginConfigured();
    const bool deepSeek = QuotaProviderClient::deepSeekKeyConfigured();
    const bool changed = codex != codexLoginDetected_ || deepSeek != deepSeekKeyConfigured_;
    codexLoginDetected_ = codex;
    deepSeekKeyConfigured_ = deepSeek;
    return changed;
}

const QuotaSnapshot* QuotaController::snapshotFor(QuotaProvider provider) const {
    const size_t index = providerIndex(provider);
    return index < snapshots_.size() ? &snapshots_[index] : nullptr;
}

bool QuotaController::displayable() const {
    if (!settings_.quotaEnabled_)
        return false;
    return (settings_.quotaCodexEnabled_ && codexLoginDetected_) ||
           (settings_.quotaDeepSeekEnabled_ && deepSeekKeyConfigured_);
}

void QuotaController::markLoading(const std::vector<QuotaProvider>& providers) {
    for (const QuotaProvider provider : providers) {
        QuotaSnapshot& snapshot = snapshots_[providerIndex(provider)];
        if (snapshot.status == QuotaStatus::Ready)
            continue; // 刷新期间保留上次成功结果，避免任务栏闪空
        snapshot.provider = provider;
        snapshot.status = QuotaStatus::Loading;
        snapshot.error.clear();
        snapshot.authRequired = false;
    }
}

void QuotaController::requestRefresh(bool force) {
    if (shutdown_.load())
        return;
    if (!settings_.quotaEnabled_) {
        publish();
        return;
    }
    refreshConfiguredState();

    std::vector<QuotaProvider> providers;
    const int64_t now = nowSeconds();
    for (const QuotaProvider provider : {QuotaProvider::Codex, QuotaProvider::DeepSeek}) {
        if (!providerEnabled(provider) || !providerConfigured(provider))
            continue;
        if (force || now >= nextAttemptAt_[providerIndex(provider)])
            providers.push_back(provider);
    }
    if (providers.empty()) {
        if (force)
            publish();
        return;
    }
    if (inFlight_) {
        pendingRefresh_ = true;
        return;
    }

    joinWorker();
    inFlight_ = true;
    ++generation_;
    const uint64_t generation = generation_;
    markLoading(providers);
    runtime_log::writef(L"[quota] refresh start generation=%llu providers=%llu",
                        static_cast<unsigned long long>(generation),
                        static_cast<unsigned long long>(providers.size()));

    worker_ = std::thread([this, providers, generation]() {
        QuotaProviderClient client;
        std::vector<QuotaSnapshot> results;
        results.reserve(providers.size());
        for (const QuotaProvider provider : providers) {
            if (shutdown_.load())
                return;
            QuotaSnapshot snapshot;
            if (provider == QuotaProvider::Codex) {
                snapshot = client.fetchCodex(shutdown_);
            } else {
                std::wstring key;
                if (QuotaProviderClient::loadDeepSeekKey(key)) {
                    snapshot = client.fetchDeepSeek(key, shutdown_);
                } else {
                    snapshot.provider = QuotaProvider::DeepSeek;
                    snapshot.status = QuotaStatus::NotConfigured;
                }
            }
            if (shutdown_.load())
                return;
            results.push_back(std::move(snapshot));
        }
        if (shutdown_.load())
            return;
        auto payload = std::make_unique<QuotaRefreshPayload>();
        payload->generation = generation;
        payload->snapshots = std::move(results);
        QuotaRefreshPayload* raw = payload.release();
        if (!PostThreadMessageW(mainThread_, kMsgQuotaReady, 0, reinterpret_cast<LPARAM>(raw)))
            delete raw;
    });
    publish();
}

void QuotaController::onRefreshReady(std::unique_ptr<QuotaRefreshPayload> payload) {
    inFlight_ = false;
    if (!payload)
        return;
    if (shutdown_.load())
        return;

    const bool stale = payload->generation != generation_;
    if (!stale) {
        const int64_t now = nowSeconds();
        for (auto& snapshot : payload->snapshots) {
            const size_t index = providerIndex(snapshot.provider);
            nextAttemptAt_[index] =
                now + std::max(kQuotaMinimumRetrySeconds, snapshot.retryAfterSeconds);
            snapshots_[index] = std::move(snapshot);
            const auto& stored = snapshots_[index];
            runtime_log::writef(L"[quota] refresh result provider=%s status=%s windows=%llu",
                                stored.provider == QuotaProvider::Codex ? L"codex" : L"deepseek",
                                stored.status == QuotaStatus::Ready  ? L"ready"
                                : stored.status == QuotaStatus::Error ? L"error"
                                                                      : L"idle",
                                static_cast<unsigned long long>(stored.windows.size()));
        }
    }
    const bool needsRefresh = stale || pendingRefresh_;
    pendingRefresh_ = false;
    publish();
    if (needsRefresh)
        requestRefresh(true);
}

void QuotaController::onTick() {
    if (shutdown_.load() || timerWindow_ == nullptr || !settings_.quotaEnabled_)
        return;
    const bool configuredChanged = refreshConfiguredState();
    if (configuredChanged)
        publish();
    if (!providerConfigured(QuotaProvider::Codex) && !providerConfigured(QuotaProvider::DeepSeek))
        return;
    requestRefresh(false);
}

void QuotaController::notifySettingsChanged() {
    ++generation_; // 在途结果属于旧配置，返回后丢弃
    nextAttemptAt_.fill(0);
    refreshConfiguredState();
    requestRefresh(true);
    publish();
}

bool QuotaController::saveDeepSeekKey(const std::wstring& key) {
    const std::wstring cleaned = trimmedKey(key);
    if (cleaned.empty()) {
        QuotaProviderClient::clearDeepSeekKey();
        runtime_log::writef(L"[quota] deepseek key cleared");
        notifySettingsChanged();
        return true;
    }
    if (!QuotaProviderClient::validDeepSeekKey(cleaned))
        return false;
    if (!QuotaProviderClient::saveDeepSeekKey(cleaned)) {
        runtime_log::writef(L"[quota] deepseek key save failed");
        return false;
    }
    runtime_log::writef(L"[quota] deepseek key saved");
    notifySettingsChanged();
    return true;
}

void QuotaController::clearDeepSeekKey() {
    QuotaProviderClient::clearDeepSeekKey();
    runtime_log::writef(L"[quota] deepseek key cleared");
    notifySettingsChanged();
}

void QuotaController::publish() {
    if (publish_)
        publish_();
}

QuotaSettingsState QuotaController::settingsState() const {
    QuotaSettingsState state;
    state.codexEnabled = settings_.quotaCodexEnabled_;
    state.deepSeekEnabled = settings_.quotaDeepSeekEnabled_;
    state.codexLoginDetected = codexLoginDetected_;
    state.deepSeekKeyConfigured = deepSeekKeyConfigured_;
    state.refreshing = inFlight_;

    const QuotaSnapshot* codex = snapshotFor(QuotaProvider::Codex);
    if (!codexLoginDetected_) {
        state.codexStatus = L"未检测到 Codex 登录，请先在 Codex 中登录后刷新。";
    } else if (codex) {
        if (codex->status == QuotaStatus::Ready) {
            const int64_t now = nowSeconds();
            std::wstring text = L"已登录";
            if (!codex->plan.empty())
                text += L" · 套餐 " + codex->plan;
            for (const auto& window : codex->windows) {
                text += L" · " + quota_detail::describeWindow(window.periodSeconds) + L" " +
                        quota_detail::formatPercent(window.remainingPercent);
                const std::wstring reset = quota_detail::formatResetIn(window.resetAt, now);
                if (!reset.empty())
                    text += L"（" + reset + L"）";
            }
            state.codexStatus = std::move(text);
        } else if (codex->status == QuotaStatus::Error) {
            state.codexStatus = codex->error;
        } else {
            state.codexStatus = L"已检测到本机登录 · 正在刷新…";
        }
    }

    const QuotaSnapshot* deepSeek = snapshotFor(QuotaProvider::DeepSeek);
    if (!deepSeekKeyConfigured_) {
        state.deepSeekStatus = L"未设置 API Key，余额不会显示。";
    } else if (deepSeek) {
        if (deepSeek->status == QuotaStatus::Ready && deepSeek->hasBalance) {
            std::wstring text =
                L"余额 " + quota_detail::formatFullAmount(deepSeek->balance.currency,
                                                          deepSeek->balance.total);
            const bool hasGranted = deepSeek->balance.granted > 0.0;
            const bool hasToppedUp = deepSeek->balance.toppedUp > 0.0;
            if (hasGranted || hasToppedUp) {
                const std::wstring& currency = deepSeek->balance.currency;
                text += L"（";
                if (hasToppedUp) {
                    text += L"充值 ";
                    text += quota_detail::formatFullAmount(currency, deepSeek->balance.toppedUp);
                }
                if (hasToppedUp && hasGranted)
                    text += L"，";
                if (hasGranted) {
                    text += L"赠送 ";
                    text += quota_detail::formatFullAmount(currency, deepSeek->balance.granted);
                }
                text += L"）";
            }
            if (!deepSeek->plan.empty())
                text += L" · " + deepSeek->plan;
            state.deepSeekStatus = std::move(text);
        } else if (deepSeek->status == QuotaStatus::Error) {
            state.deepSeekStatus = deepSeek->error;
        } else {
            state.deepSeekStatus = L"已设置 API Key · 正在刷新…";
        }
    }
    return state;
}

std::vector<QuotaReading> QuotaController::panelReadings() const {
    std::vector<QuotaReading> readings;
    if (!settings_.quotaEnabled_)
        return readings;

    if (settings_.quotaCodexEnabled_ && codexLoginDetected_) {
        const QuotaSnapshot* codex = snapshotFor(QuotaProvider::Codex);
        if (codex && codex->status == QuotaStatus::Ready && !codex->windows.empty()) {
            // 两个周期合并到一格，避免 Codex 独占两行把其他状态挤出第一页。
            const QuotaWindow& first = codex->windows.front();
            if (codex->windows.size() == 1) {
                readings.push_back({L"Codex " + first.label,
                                    quota_detail::formatPercent(first.remainingPercent)});
            } else {
                const QuotaWindow& second = codex->windows[1];
                const std::wstring label =
                    first.label == second.label
                        ? L"Codex"
                        : L"Codex " + first.label + L"/" + second.label;
                readings.push_back(
                    {label, quota_detail::formatPercent(first.remainingPercent) + L"/" +
                                quota_detail::formatPercent(second.remainingPercent)});
            }
        } else if (codex && codex->status == QuotaStatus::Error) {
            readings.push_back({L"Codex", codex->authRequired ? L"需登录" : L"重试"});
        } else {
            readings.push_back({L"Codex", L"…"});
        }
    }

    if (settings_.quotaDeepSeekEnabled_ && deepSeekKeyConfigured_) {
        const QuotaSnapshot* deepSeek = snapshotFor(QuotaProvider::DeepSeek);
        if (deepSeek && deepSeek->status == QuotaStatus::Ready && deepSeek->hasBalance) {
            readings.push_back({L"DeepSeek",
                                quota_detail::formatCompactAmount(deepSeek->balance.currency,
                                                                  deepSeek->balance.total)});
        } else if (deepSeek && deepSeek->status == QuotaStatus::Error) {
            readings.push_back({L"DeepSeek", deepSeek->authRequired ? L"需登录" : L"重试"});
        } else {
            readings.push_back({L"DeepSeek", L"…"});
        }
    }
    return readings;
}
