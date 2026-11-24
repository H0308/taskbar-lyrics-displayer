#include "media/smtc_monitor.h"
#include "media/player_source.h"

#include "media/smtc/netease_smtc_adapter.h"
#include "media/smtc/qq_music_smtc_adapter.h"
#include "media/smtc/sampled_smtc_adapter.h"
#include "media/smtc/smtc_common.h"
#include "logging/runtime_logger.h"

#include <windows.h>
#include <tlhelp32.h>

#include <winrt/Windows.Media.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

using Session = smtc::Session;
using namespace winrt;
using namespace winrt::Windows::Media::Control;

namespace {

bool processAlive(const wchar_t* executableName) {
    if (!executableName || !*executableName)
        return false;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, executableName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool playerProcessAlive(SmtcPlayerType player) {
    const auto* source = player_source::find(player);
    return source && !source->processNames.empty() &&
           processAlive(source->processNames.front());
}

const wchar_t* playerName(SmtcPlayerType player) noexcept {
    switch (player) {
    case SmtcPlayerType::QQMusic:
        return L"QQMusic";
    case SmtcPlayerType::NetEase:
        return L"NetEase";
    case SmtcPlayerType::Soda:
        return L"Soda Music";
    case SmtcPlayerType::Spotify:
        return L"Spotify";
    case SmtcPlayerType::SaltPlayer:
        return L"Salt Player";
    case SmtcPlayerType::Generic:
        return L"Generic";
    default:
        return L"Unknown";
    }
}

const wchar_t* playbackStatusName(PlaybackStatus status) noexcept {
    switch (status) {
    case PlaybackStatus::Stopped:
        return L"Stopped";
    case PlaybackStatus::Playing:
        return L"Playing";
    case PlaybackStatus::Paused:
        return L"Paused";
    default:
        return L"Other";
    }
}

const wchar_t* asyncStatusName(winrt::Windows::Foundation::AsyncStatus status) noexcept {
    using AsyncStatus = winrt::Windows::Foundation::AsyncStatus;
    switch (status) {
    case AsyncStatus::Started:
        return L"Started";
    case AsyncStatus::Completed:
        return L"Completed";
    case AsyncStatus::Canceled:
        return L"Canceled";
    case AsyncStatus::Error:
        return L"Error";
    default:
        return L"Unknown";
    }
}

void writeSmtcControlLog(const std::wstring& message) noexcept {
    try {
        runtime_log::writef(L"[smtc-control] %s", message.c_str());
    } catch (...) {
    }
}

std::wstring sessionSourceAppUserModelId(const Session& session) {
    if (!session)
        return L"<none>";
    try {
        return session.SourceAppUserModelId().c_str();
    } catch (...) {
        return L"<unavailable>";
    }
}

// ---- 播放模式（顺序/列表/单曲/随机）----
int playModeFrom(int repeatMode, bool shuffle) {
    if (shuffle)
        return 3;
    if (repeatMode == 2)
        return 1;
    if (repeatMode == 1)
        return 2;
    return 0;
}

int repeatModeFor(int mode) {
    switch (mode) {
    case 1: return 2; // 列表循环
    case 2: return 1; // 单曲循环
    case 3: return 2; // 随机：底层仍是列表，靠 shuffle 区分
    default: return 0;
    }
}

bool shuffleFor(int mode) { return mode == 3; }

} // namespace

struct SmtcMonitor::Impl {
    enum class ControlAction { PlayPause, SkipNext, SkipPrevious, CycleRepeat, Seek };
    struct ControlRequest {
        ControlAction action;
        uint64_t sessionGeneration;
        int64_t positionMs = 0;
    };

    struct RefreshSignal {
        std::mutex mtx;
        std::condition_variable cv;
        bool stopping = false;
        bool requested = false;
        bool rebuildWatchers = false;
        bool mediaPropertiesChanged = false;
        std::optional<std::wstring> pendingSourceSelection;
        int pendingSourceSteps = 0;
        std::deque<ControlRequest> pendingControls;
    };

    struct ThumbnailRequest {
        GlobalSystemMediaTransportControlsSessionMediaProperties props{ nullptr };
        uint64_t generation = 0;
        uint64_t sequence = 0;
        std::wstring title;
        std::wstring artist;
        std::wstring album;
        std::wstring source;
    };
    struct SessionWatcher {
        Session session{ nullptr };
        Session::PlaybackInfoChanged_revoker playbackRevoker;
        Session::MediaPropertiesChanged_revoker propsRevoker;
    };

    GlobalSystemMediaTransportControlsSessionManager manager{ nullptr };
    Session session{ nullptr };

    mutable std::mutex mtx;
    // 会话选择、属性和进度适配只由刷新线程执行；封面线程仅提交图片字节。
    SmtcSnapshot snap;
    SmtcSourceSelection sourceSelection;
    SmtcMonitor::ChangeCallback onChange;

    GlobalSystemMediaTransportControlsSessionManager::SessionsChanged_revoker sessionsRevoker;
    GlobalSystemMediaTransportControlsSessionManager::CurrentSessionChanged_revoker
        currentSessionRevoker;
    Session::TimelinePropertiesChanged_revoker timelineRevoker;
    Session::MediaPropertiesChanged_revoker selectedPropsRevoker;

    // 所有媒体会话都保留轻量监听，避免只监听当前选中会话导致切换丢失。
    std::vector<SessionWatcher> sessionWatchers;

    // 新播放器只需实现一个适配器并在这里注册，监控器本身不再增加播放器分支。
    std::vector<std::unique_ptr<smtc::SmtcPlayerAdapter>> adapters;
    mutable std::mutex filterMtx;
    GenericSmtcFilterConfig genericFilter;
    SmtcMonitor::SourceTargetResolver sourceTargetResolver;
    std::vector<std::wstring> observedGenericSourceIds;

    std::shared_ptr<RefreshSignal> refreshSignal = std::make_shared<RefreshSignal>();
    std::thread pollThread;
    std::mutex thumbnailMtx;
    std::condition_variable thumbnailCv;
    bool thumbnailStopping = false;
    ThumbnailRequest thumbnailRequest;
    std::thread thumbnailThread;
    uint64_t thumbnailGeneration = 0;
    bool thumbnailPending = false;
    int64_t thumbnailRetryAtMs = 0;
    uint64_t thumbnailReadSequence = 0;
    uint64_t thumbnailDiagnosticsGeneration = 0;
    int thumbnailReferenceState = -1;
    int64_t sampledPositionMs = 0;
    int64_t sampledDurationMs = 0;
    int64_t sampledAnchorMs = 0;
    int pendingPlayMode_ = -1;
    int64_t pendingPlayModeUntilMs_ = 0;
    bool repeatInitialLogged_ = false;
    bool repeatMissingLogged_ = false;

    // Windows 在其他 SMTC 应用成为当前会话时，可能从枚举结果中漏掉
    // 已暂停的音乐会话。只要该会话所属的音乐播放器仍在运行，就保留暂停快照；
    // 播放器退出后再等待一个稳定窗口，避免生命周期边界的瞬时空列表。
    std::chrono::steady_clock::time_point sessionLossCandidateSince;
    std::chrono::steady_clock::time_point preferredSourceLossSince;
    std::chrono::steady_clock::time_point nextSourceListRefresh;
    static constexpr auto kSessionLossStabilityWindow = std::chrono::milliseconds(1500);

    Impl() {
        adapters.emplace_back(std::make_unique<smtc::QqMusicSmtcAdapter>());
        adapters.emplace_back(std::make_unique<smtc::NeteaseSmtcAdapter>());
        adapters.emplace_back(std::make_unique<smtc::SampledSmtcAdapter>(SmtcPlayerType::Soda));
        adapters.emplace_back(std::make_unique<smtc::SampledSmtcAdapter>(SmtcPlayerType::Spotify));
        adapters.emplace_back(std::make_unique<smtc::SampledSmtcAdapter>(SmtcPlayerType::SaltPlayer));
        // Generic 兜底必须注册在最后：identifySession 按顺序取第一个识别结果，
        // 已知播放器先由专属适配器认领，其余 SMTC 会话统一走通用采样。
        adapters.emplace_back(std::make_unique<smtc::SampledSmtcAdapter>(SmtcPlayerType::Generic));
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(refreshSignal->mtx);
            refreshSignal->stopping = true;
            refreshSignal->pendingControls.clear();
        }
        refreshSignal->cv.notify_all();
        // 先结束轮询线程（其内部会在 MTA 退出前撤销所有 SMTC 事件和会话对象），
        // 再停止封面线程，确保不存在跨套间的 WinRT 析构。
        if (pollThread.joinable())
            pollThread.join();
        {
            std::lock_guard<std::mutex> lk(thumbnailMtx);
            thumbnailStopping = true;
        }
        thumbnailCv.notify_all();
        if (thumbnailThread.joinable())
            thumbnailThread.join();
    }

    static void requestRefresh(const std::shared_ptr<RefreshSignal>& signal,
                               bool rebuildWatchers = false,
                               bool mediaPropertiesChanged = false) {
        {
            std::lock_guard<std::mutex> lk(signal->mtx);
            if (signal->stopping)
                return;
            signal->requested = true;
            signal->rebuildWatchers = signal->rebuildWatchers || rebuildWatchers;
            signal->mediaPropertiesChanged = signal->mediaPropertiesChanged || mediaPropertiesChanged;
        }
        signal->cv.notify_one();
    }

    static void requestControl(const std::shared_ptr<RefreshSignal>& signal,
                               ControlRequest request) {
        {
            std::lock_guard<std::mutex> lk(signal->mtx);
            if (signal->stopping)
                return;
            signal->pendingControls.push_back(request);
        }
        signal->cv.notify_one();
    }

    smtc::SmtcPlayerAdapter* adapterFor(SmtcPlayerType player) const {
        for (const auto& adapter : adapters) {
            if (adapter && adapter->playerType() == player)
                return adapter.get();
        }
        return nullptr;
    }

    std::wstring thumbCacheKey_;
    std::shared_ptr<const std::vector<uint8_t>> thumbCache_;

    static std::wstring thumbnailKey(const SmtcSnapshot& snapshot) {
        return snapshot.sourceAppUserModelId + L'\x1f' + snapshot.neteaseSongId +
               L'\x1f' + snapshot.title + L'\x1f' + snapshot.artist +
               L'\x1f' + snapshot.album;
    }

    void startThumbnailReader() {
        thumbnailThread = std::thread([this] {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (const winrt::hresult_error& error) {
                runtime_log::writef(L"[cover][smtc] reader-init-failed hresult=0x%08X reason=\"%s\"",
                                   static_cast<unsigned int>(error.code().value), error.message().c_str());
                return;
            } catch (...) {
                runtime_log::writef(L"[cover][smtc] reader-init-failed reason=unknown");
                return;
            }
            for (;;) {
                ThumbnailRequest request;
                {
                    std::unique_lock<std::mutex> lk(thumbnailMtx);
                    thumbnailCv.wait(lk, [this] {
                        return thumbnailStopping || thumbnailRequest.props;
                    });
                    if (thumbnailStopping) {
                        thumbnailRequest = {};
                        break;
                    }
                    request = std::move(thumbnailRequest);
                    thumbnailRequest = {};
                }
                runtime_log::writef(
                    L"[cover][smtc] read-started sequence=%llu generation=%llu source=\"%s\" "
                    L"title=\"%s\" artist=\"%s\" album=\"%s\"",
                    static_cast<unsigned long long>(request.sequence),
                    static_cast<unsigned long long>(request.generation), request.source.c_str(),
                    request.title.c_str(), request.artist.c_str(), request.album.c_str());
                auto thumbnail = smtc::readThumbnail(request.props, request.sequence);
                const size_t bytes = thumbnail ? thumbnail->size() : 0;
                bool changed = false;
                uint64_t currentGeneration = 0;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    currentGeneration = thumbnailGeneration;
                    if (request.generation == thumbnailGeneration) {
                        thumbnailPending = false;
                        thumbnailRetryAtMs = smtc::nowUtcMs() + 1000;
                        if (thumbnail) {
                            thumbCache_ = std::move(thumbnail);
                            snap.thumbnail = thumbCache_;
                            changed = true;
                        }
                    }
                }
                if (request.generation != currentGeneration) {
                    runtime_log::writef(
                        L"[cover][smtc] read-discarded sequence=%llu generation=%llu current-generation=%llu "
                        L"title=\"%s\" bytes=%zu reason=track-or-session-changed",
                        static_cast<unsigned long long>(request.sequence),
                        static_cast<unsigned long long>(request.generation),
                        static_cast<unsigned long long>(currentGeneration), request.title.c_str(), bytes);
                } else {
                    runtime_log::writef(
                        L"[cover][smtc] %s sequence=%llu generation=%llu title=\"%s\" bytes=%zu retry-after=%dms",
                        changed ? L"read-committed" : L"retry-scheduled",
                        static_cast<unsigned long long>(request.sequence),
                        static_cast<unsigned long long>(request.generation), request.title.c_str(),
                        bytes, changed ? 0 : 1000);
                }
                if (changed)
                    notify();
            }
            winrt::uninit_apartment();
        });
    }

    void clearPendingThumbnail(uint64_t generation) {
        std::lock_guard<std::mutex> lk(mtx);
        if (thumbnailGeneration == generation)
            thumbnailPending = false;
    }

    // 在快照锁外访问 WinRT 缩略图引用，再单独入队给封面线程。
    void enqueueThumbnailCheck(
        const GlobalSystemMediaTransportControlsSessionMediaProperties& props,
        uint64_t generation, const std::wstring& source,
        const std::wstring& title, const std::wstring& artist,
        const std::wstring& album) {
        bool queued = false;
        try {
            const bool hasThumbnail = static_cast<bool>(props.Thumbnail());
            if (thumbnailDiagnosticsGeneration != generation ||
                thumbnailReferenceState != static_cast<int>(hasThumbnail)) {
                thumbnailDiagnosticsGeneration = generation;
                thumbnailReferenceState = static_cast<int>(hasThumbnail);
                runtime_log::writef(
                    L"[cover][smtc] thumbnail-%s generation=%llu source=\"%s\" title=\"%s\" artist=\"%s\" album=\"%s\"",
                    hasThumbnail ? L"provided" : L"missing",
                    static_cast<unsigned long long>(generation), source.c_str(),
                    title.c_str(), artist.c_str(), album.c_str());
            }
            if (!hasThumbnail) {
                clearPendingThumbnail(generation);
                return;
            }
            const uint64_t sequence = ++thumbnailReadSequence;
            {
                std::lock_guard<std::mutex> lk(thumbnailMtx);
                if (thumbnailStopping) {
                    clearPendingThumbnail(generation);
                    return;
                }
                if (thumbnailRequest.props) {
                    runtime_log::writef(
                        L"[cover][smtc] read-discarded sequence=%llu generation=%llu title=\"%s\" reason=request-replaced-before-read",
                        static_cast<unsigned long long>(thumbnailRequest.sequence),
                        static_cast<unsigned long long>(thumbnailRequest.generation), thumbnailRequest.title.c_str());
                }
                thumbnailRequest = { props, generation, sequence,
                                     title, artist, album, source };
                queued = true;
                runtime_log::writef(L"[cover][smtc] read-queued sequence=%llu generation=%llu title=\"%s\"",
                                   static_cast<unsigned long long>(sequence),
                                   static_cast<unsigned long long>(generation), title.c_str());
            }
            thumbnailCv.notify_one();
        } catch (const winrt::hresult_error& error) {
            if (!queued)
                clearPendingThumbnail(generation);
            if (thumbnailDiagnosticsGeneration != generation || thumbnailReferenceState != -2) {
                thumbnailDiagnosticsGeneration = generation;
                thumbnailReferenceState = -2;
                runtime_log::writef(
                    L"[cover][smtc] request-failed generation=%llu title=\"%s\" hresult=0x%08X reason=\"%s\"",
                    static_cast<unsigned long long>(generation), title.c_str(),
                    static_cast<unsigned int>(error.code().value), error.message().c_str());
            }
        } catch (...) {
            if (!queued)
                clearPendingThumbnail(generation);
            if (thumbnailDiagnosticsGeneration != generation || thumbnailReferenceState != -2) {
                thumbnailDiagnosticsGeneration = generation;
                thumbnailReferenceState = -2;
                runtime_log::writef(L"[cover][smtc] request-failed generation=%llu title=\"%s\" reason=unknown",
                                   static_cast<unsigned long long>(generation), title.c_str());
            }
        }
    }

    void recordObservedGenericSession(const Session& candidate) {
        if (!candidate)
            return;
        try {
            const std::wstring sourceId = candidate.SourceAppUserModelId().c_str();
            if (sourceId.empty() || player_source::find(sourceId) != nullptr)
                return;
            using Status =
                winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
            auto info = candidate.GetPlaybackInfo();
            if (!info || info.PlaybackStatus() == Status::Closed)
                return;
            std::lock_guard<std::mutex> lk(filterMtx);
            for (auto it = observedGenericSourceIds.begin(); it != observedGenericSourceIds.end();
                 ++it) {
                if (_wcsicmp(it->c_str(), sourceId.c_str()) == 0) {
                    if (it != observedGenericSourceIds.begin())
                        std::rotate(observedGenericSourceIds.begin(), it, it + 1);
                    return;
                }
            }
            observedGenericSourceIds.insert(observedGenericSourceIds.begin(), sourceId);
            if (observedGenericSourceIds.size() > kMaxGenericSmtcRules)
                observedGenericSourceIds.resize(kMaxGenericSmtcRules);
        } catch (...) {
        }
    }

    static bool matchesGenericRule(const std::wstring& sourceId,
                                   const AudioSessionTarget& resolvedTarget,
                                   const GenericSmtcAppRule& rule) {
        if (!rule.sourceId.empty() && _wcsicmp(sourceId.c_str(), rule.sourceId.c_str()) == 0)
            return true;
        if (!rule.packageFamilyName.empty() && !resolvedTarget.packageFamilyName.empty() &&
            _wcsicmp(resolvedTarget.packageFamilyName.c_str(), rule.packageFamilyName.c_str()) == 0)
            return true;
        if (!rule.processName.empty()) {
            for (const auto& proc : resolvedTarget.processNames) {
                if (_wcsicmp(proc.c_str(), rule.processName.c_str()) == 0)
                    return true;
            }
            if (_wcsicmp(sourceId.c_str(), rule.processName.c_str()) == 0)
                return true;
        }
        return false;
    }

    bool isGenericSourceAllowed(const std::wstring& sourceId) const {
        if (sourceId.empty())
            return false;
        GenericSmtcFilterConfig config;
        SmtcMonitor::SourceTargetResolver resolver;
        {
            std::lock_guard<std::mutex> lk(filterMtx);
            config = genericFilter;
            resolver = sourceTargetResolver;
        }
        if (!config.enabled)
            return false;
        if (config.mode == GenericSmtcFilterMode::All)
            return true;

        const auto& rules = config.mode == GenericSmtcFilterMode::AllowList ? config.allowList
                                                                            : config.blockList;
        if (rules.empty())
            return config.mode == GenericSmtcFilterMode::BlockList;

        AudioSessionTarget resolvedTarget;
        if (resolver)
            resolvedTarget = resolver(sourceId);
        const bool matched = std::any_of(rules.begin(), rules.end(), [&](const auto& rule) {
            return matchesGenericRule(sourceId, resolvedTarget, rule);
        });
        return config.mode == GenericSmtcFilterMode::AllowList ? matched : !matched;
    }

    smtc::SmtcSessionIdentity identifySession(const Session& candidate) const {
        for (const auto& adapter : adapters) {
            if (!adapter)
                continue;
            auto identity = adapter->identifySession(candidate);
            if (identity.player == SmtcPlayerType::Unknown)
                continue;
            if (identity.player == SmtcPlayerType::Generic &&
                !isGenericSourceAllowed(identity.sourceAppUserModelId))
                continue;
            return identity;
        }
        return {};
    }

    void notify() {
        if (onChange)
            onChange();
    }

    bool canSwitchSource() const {
        std::lock_guard<std::mutex> lk(mtx);
        return sourceSelection.available.size() > 1;
    }

    void advanceSourceRequest(std::optional<std::wstring>& request, int steps) const {
        if (steps == 0)
            return;
        std::lock_guard<std::mutex> lk(mtx);
        const auto& sources = sourceSelection.available;
        const int count = static_cast<int>(sources.size());
        if (count < 2)
            return;

        const auto& preferredId = request ? *request : sourceSelection.preferredAppUserModelId;
        const auto& sourceId = preferredId.empty() ? snap.sourceAppUserModelId : preferredId;
        const auto current = std::find_if(sources.begin(), sources.end(), [&](const auto& source) {
            return _wcsicmp(source.appUserModelId.c_str(), sourceId.c_str()) == 0;
        });
        const int index = current != sources.end() ? static_cast<int>(current - sources.begin())
                                                   : (steps > 0 ? -1 : 0);
        const int next = (index + steps % count + count) % count;
        request = sources[next].appUserModelId;
    }

    // 控制请求投递到后台 MTA 轮询线程执行，避免跨套间操作 WinRT 会话，
    // 完成回调只记录异步结果，不阻塞轮询消息调度。
    void logControlSession(const wchar_t* action, const wchar_t* api,
                           const Session& target, const SmtcSnapshot& snapshot) {
        Session systemCurrent{ nullptr };
        int targetIndex = -1;
        int currentIndex = -1;
        int sessionCount = -1;
        const wchar_t* managerQuery = manager ? L"ok" : L"unavailable";

        if (manager) {
            try {
                systemCurrent = manager.GetCurrentSession();
                auto sessions = manager.GetSessions();
                if (sessions) {
                    sessionCount = static_cast<int>(sessions.Size());
                    for (uint32_t i = 0; i < sessions.Size(); ++i) {
                        const Session candidate = sessions.GetAt(i);
                        if (target && targetIndex < 0 && smtc::sameSession(target, candidate))
                            targetIndex = static_cast<int>(i);
                        if (systemCurrent && currentIndex < 0 &&
                            smtc::sameSession(systemCurrent, candidate))
                            currentIndex = static_cast<int>(i);
                    }
                }
            } catch (...) {
                managerQuery = L"error";
            }
        }

        std::wostringstream line;
        line << L"phase=dispatch action=" << action << L" api=" << api
             << L" player=" << playerName(snapshot.player)
             << L" selectedSource=" << sessionSourceAppUserModelId(target)
             << L" selectedListed=" << (targetIndex >= 0 ? 1 : 0)
             << L" selectedIndex=" << targetIndex
             << L" cachedStatus=" << playbackStatusName(snapshot.status)
             << L" controls(prev=" << (snapshot.canPrev ? 1 : 0)
             << L",playPause=" << (snapshot.canPlayPause ? 1 : 0)
             << L",next=" << (snapshot.canNext ? 1 : 0) << L")"
             << L" systemCurrentSource=" << sessionSourceAppUserModelId(systemCurrent)
             << L" currentListed=" << (currentIndex >= 0 ? 1 : 0)
             << L" currentIndex=" << currentIndex
             << L" sameSession=" << (target && systemCurrent &&
                                           smtc::sameSession(target, systemCurrent) ? 1 : 0)
             << L" sessionCount=" << sessionCount << L" managerQuery=" << managerQuery;
        writeSmtcControlLog(line.str());
    }

    template <typename StartOperation>
    void startControlOperation(const wchar_t* action, const wchar_t* api,
                               const Session& target, const SmtcSnapshot& snapshot,
                               StartOperation&& start) {
        try {
            logControlSession(action, api, target, snapshot);
        } catch (...) {
            // 诊断日志异常不能阻止实际控制请求。
        }
        if (!target) {
            std::wostringstream line;
            line << L"phase=operation-skipped action=" << action << L" api=" << api
                 << L" reason=no-selected-session";
            writeSmtcControlLog(line.str());
            return;
        }

        try {
            auto operation = start();
            if (!operation) {
                std::wostringstream line;
                line << L"phase=operation-start action=" << action << L" api=" << api
                     << L" result=no-operation";
                writeSmtcControlLog(line.str());
                return;
            }

            {
                std::wostringstream line;
                line << L"phase=operation-start action=" << action << L" api=" << api
                     << L" result=started";
                writeSmtcControlLog(line.str());
            }

            const std::wstring actionCopy(action);
            const std::wstring apiCopy(api);
            operation.Completed([actionCopy, apiCopy, signal = refreshSignal](
                                    auto&& completedOperation, auto status) {
                using AsyncStatus = winrt::Windows::Foundation::AsyncStatus;
                if (status != AsyncStatus::Completed) {
                    std::wostringstream line;
                    line << L"phase=operation-complete action=" << actionCopy
                         << L" api=" << apiCopy << L" asyncStatus="
                         << asyncStatusName(status) << L"(" << static_cast<int>(status) << L")";
                    writeSmtcControlLog(line.str());
                    return;
                }

                try {
                    const bool succeeded = completedOperation.GetResults();
                    std::wostringstream line;
                    line << L"phase=operation-complete action=" << actionCopy
                         << L" api=" << apiCopy << L" asyncStatus=Completed result="
                         << (succeeded ? L"true" : L"false");
                    writeSmtcControlLog(line.str());
                    if (succeeded)
                        requestRefresh(signal);
                } catch (const winrt::hresult_error& error) {
                    std::wostringstream line;
                    line << L"phase=operation-complete action=" << actionCopy
                         << L" api=" << apiCopy << L" asyncStatus=Completed result=exception hresult=0x"
                         << std::hex << static_cast<uint32_t>(error.code());
                    writeSmtcControlLog(line.str());
                } catch (...) {
                    std::wostringstream line;
                    line << L"phase=operation-complete action=" << actionCopy
                         << L" api=" << apiCopy << L" asyncStatus=Completed result=unknown-exception";
                    writeSmtcControlLog(line.str());
                }
            });
        } catch (const winrt::hresult_error& error) {
            std::wostringstream line;
            line << L"phase=operation-start action=" << action << L" api=" << api
                 << L" result=exception hresult=0x" << std::hex
                 << static_cast<uint32_t>(error.code());
            writeSmtcControlLog(line.str());
        } catch (...) {
            std::wostringstream line;
            line << L"phase=operation-start action=" << action << L" api=" << api
                 << L" result=unknown-exception";
            writeSmtcControlLog(line.str());
        }
    }

    void refreshAll(bool mediaPropertiesChanged) {
        Session currentSession{ nullptr };
        SmtcSnapshot previous;
        {
            std::lock_guard<std::mutex> lk(mtx);
            currentSession = session;
            previous = snap;
        }
        if (!currentSession)
            return;

        bool playbackChanged = false;
        smtc::PlaybackSample phase1Playback;
        smtc::TimelineSample phase1Timeline;
        try {
            if (auto info = currentSession.GetPlaybackInfo()) {
                phase1Playback = smtc::readPlaybackSample(info);
                if (phase1Playback.valid && previous.sessionAlive &&
                    phase1Playback.status != previous.status) {
                    phase1Timeline = smtc::readTimelineSample(
                        currentSession.GetTimelineProperties());
                }
            }
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (phase1Playback.valid && snap.sessionAlive) {
                if (phase1Playback.status != snap.status) {
                    if (auto* adapter = adapterFor(snap.player))
                        adapter->refreshPlayback(phase1Playback, phase1Timeline,
                                                 snap, smtc::nowUtcMs());
                    snap.status = phase1Playback.status;
                }
                smtc::applyPlaybackControls(phase1Playback, snap);
                playbackChanged = snap.status != previous.status ||
                                  snap.canPrev != previous.canPrev ||
                                  snap.canPlayPause != previous.canPlayPause ||
                                  snap.canNext != previous.canNext ||
                                  snap.canSeek != previous.canSeek ||
                                  snap.canRepeat != previous.canRepeat;
            }
            previous = snap;
        }
        if (playbackChanged)
            notify();

        SmtcSnapshot next = previous;
        if (!next.sessionAlive) {
            const auto identity = identifySession(currentSession);
            next.player = identity.player;
            next.neteaseSongId = identity.neteaseSongId;
            next.sourceAppUserModelId = identity.sourceAppUserModelId;
            next.enhancedSmtc = identity.enhancedSmtc;
            next.sessionAlive = identity.player != SmtcPlayerType::Unknown;
        }
        auto* adapter = adapterFor(next.player);
        if (!adapter)
            return;

        GlobalSystemMediaTransportControlsSessionMediaProperties props{ nullptr };
        try {
            auto operation = currentSession.TryGetMediaPropertiesAsync();
            if (operation.wait_for(std::chrono::seconds(1)) ==
                winrt::Windows::Foundation::AsyncStatus::Completed)
                props = operation.GetResults();
            else
                operation.Cancel();
            if (props) {
                next.title = props.Title().c_str();
                next.artist = props.Artist().c_str();
                next.album = props.AlbumTitle().c_str();
                if (player_source::usesMetadataLyricPath(next.player))
                    next.albumArtist = props.AlbumArtist().c_str();
                if (next.player == SmtcPlayerType::NetEase) {
                    for (const auto& genre : props.Genres()) {
                        const std::wstring value = genre.c_str();
                        if (value.starts_with(L"NCM-") && value.size() > 4 &&
                            value.find_first_not_of(L"0123456789", 4) == std::wstring::npos) {
                            next.neteaseSongId = value.substr(4);
                            break;
                        }
                    }
                }
            }
        } catch (...) {
        }

        const bool trackChanged = !previous.sessionAlive ||
                                  next.title != previous.title ||
                                  next.artist != previous.artist ||
                                  next.neteaseSongId != previous.neteaseSongId;
        smtc::TimelineSample timelineSample;
        smtc::PlaybackSample playbackSample;
        try {
            auto timeline = currentSession.GetTimelineProperties();
            auto info = currentSession.GetPlaybackInfo();
            timelineSample = smtc::readTimelineSample(timeline);
            playbackSample = smtc::readPlaybackSample(info);
            const bool repeatSupported = playbackSample.canRepeat &&
                                         next.player != SmtcPlayerType::QQMusic &&
                                         next.player != SmtcPlayerType::Soda;
            if (!repeatSupported) {
                pendingPlayMode_ = -1;
                pendingPlayModeUntilMs_ = 0;
                next.repeatMode = 0;
                next.shuffle = false;
            } else {
                int reportedRepeat = -1; // 本次上报的原始循环模式（-1 表示播放器没报）
                bool reportedShuffle = false;
                if (info) {
                    // 循环模式由播放器上报，可能为空（不支持时），保持上一次的值。
                    if (auto mode = info.AutoRepeatMode()) {
                        const int value = static_cast<int>(mode.Value());
                        if (!repeatInitialLogged_) {
                            repeatInitialLogged_ = true;
                            runtime_log::writef(L"[smtc] repeat initial=%d (0 关/1 单曲/2 列表)",
                                                value);
                        }
                        next.repeatMode = value;
                        reportedRepeat = value;
                        if (auto shuffleActive = info.IsShuffleActive()) {
                            next.shuffle = shuffleActive.Value();
                            reportedShuffle = shuffleActive.Value();
                        } else {
                            reportedShuffle = next.shuffle;
                        }
                    }
                }
                {
                    // 刚切换过模式时，上报值可能还是旧的：以本地权威值稳定界面。
                    // 网易云是异步生效的（实测 1~2 秒才回读到新值），所以不能立刻判死。
                    // 注意：判定必须用 reportedRepeat 这份"播放器原始上报值"，
                    // 不能用 next.repeatMode —— next 是从上一帧继承来的，里面已经含
                    // 本地的覆盖值，用它判断会自己确认自己，第一帧就把预期值清掉。
                    const int pending = pendingPlayMode_;
                    const bool pendingValid =
                        pending >= 0 && smtc::nowUtcMs() < pendingPlayModeUntilMs_;
                    if (pendingValid) {
                        const bool confirmed =
                            reportedRepeat >= 0 &&
                            playModeFrom(reportedRepeat, reportedShuffle) == pending;
                        if (confirmed) {
                            // 播放器真的跟上来了，停止覆盖。
                            pendingPlayMode_ = -1;
                            pendingPlayModeUntilMs_ = 0;
                        } else {
                            next.repeatMode = repeatModeFor(pending);
                            next.shuffle = shuffleFor(pending);
                        }
                    } else if (pending >= 0) {
                        // 窗口到期仍未生效：放掉本地值并留痕，界面回到真实状态。
                        pendingPlayMode_ = -1;
                        pendingPlayModeUntilMs_ = 0;
                        const int actualMode = playModeFrom(next.repeatMode, next.shuffle);
                        std::wostringstream reject;
                        reject << L"phase=operation-rejected action=play-mode wanted=" << pending
                               << L" actual=" << actualMode;
                        writeSmtcControlLog(reject.str());
                    }
                }
                if (info && !info.AutoRepeatMode()) {
                    if (!repeatMissingLogged_) {
                        repeatMissingLogged_ = true;
                        runtime_log::writef(L"[smtc] repeat unsupported: AutoRepeatMode is null");
                    }
                }
            }
        } catch (...) {
        }

        bool changed = false;
        bool needThumbnailCheck = false;
        uint64_t evalGeneration = 0;
        std::wstring evalSource;
        std::wstring evalTitle;
        std::wstring evalArtist;
        std::wstring evalAlbum;
        {
            std::lock_guard<std::mutex> lk(mtx);
            const int64_t now = smtc::nowUtcMs();
            if (timelineSample.valid && playbackSample.valid) {
                if (trackChanged) {
                    next.positionMs = timelineSample.positionMs;
                    next.durationMs = timelineSample.durationMs;
                    next.anchorUtcMs = timelineSample.anchorUtcMs > 0
                                           ? timelineSample.anchorUtcMs
                                           : now;
                    next.timelineStale = false;
                    adapter->prepareInitialSnapshot(next);
                    next.status = playbackSample.status;
                } else {
                    if (playbackSample.status != next.status)
                        adapter->refreshPlayback(playbackSample, timelineSample, next, now);
                    if (next.timelineStale ||
                        timelineSample.positionMs != sampledPositionMs ||
                        timelineSample.durationMs != sampledDurationMs ||
                        timelineSample.anchorUtcMs != sampledAnchorMs) {
                        adapter->refreshTimeline(playbackSample, timelineSample, next, now);
                    }
                }
                smtc::applyPlaybackControls(playbackSample, next);
                sampledPositionMs = timelineSample.positionMs;
                sampledDurationMs = timelineSample.durationMs;
                sampledAnchorMs = timelineSample.anchorUtcMs;
            }

            const std::wstring key = thumbnailKey(next);
            // 歌名可能先更新，封面稍后通过同曲目的属性事件补齐。
            // 属性更新也必须开启新读取代次，不能因已缓存图片而永久跳过。
            if (key != thumbCacheKey_ || mediaPropertiesChanged) {
                thumbCacheKey_ = key;
                thumbCache_.reset();
                ++thumbnailGeneration;
                thumbnailPending = false;
                thumbnailRetryAtMs = 0;
            }
            next.thumbnail = thumbCache_;
            changed = next.sessionAlive != snap.sessionAlive ||
                      next.player != snap.player || next.title != snap.title ||
                      next.artist != snap.artist || next.album != snap.album ||
                      next.albumArtist != snap.albumArtist ||
                      next.neteaseSongId != snap.neteaseSongId ||
                      next.status != snap.status || next.durationMs != snap.durationMs ||
                      next.positionMs != snap.positionMs || next.anchorUtcMs != snap.anchorUtcMs ||
                      next.timelineStale != snap.timelineStale ||
                      next.thumbnail != snap.thumbnail || next.canPrev != snap.canPrev ||
                      next.canPlayPause != snap.canPlayPause || next.canNext != snap.canNext ||
                      next.canSeek != snap.canSeek ||
                      next.canRepeat != snap.canRepeat ||
                      next.repeatMode != snap.repeatMode || next.shuffle != snap.shuffle;
            if (next.repeatMode != snap.repeatMode || next.shuffle != snap.shuffle)
                runtime_log::writef(L"[smtc] play-mode=%d (0 顺序/1 列表/2 单曲/3 随机)", next.playMode());
            snap = std::move(next);
            if (props && !thumbCache_ && !thumbnailPending &&
                now >= thumbnailRetryAtMs) {
                needThumbnailCheck = true;
                thumbnailPending = true;
                evalGeneration = thumbnailGeneration;
                evalSource = snap.sourceAppUserModelId;
                evalTitle = snap.title;
                evalArtist = snap.artist;
                evalAlbum = snap.album;
            }
        }
        if (needThumbnailCheck)
            enqueueThumbnailCheck(props, evalGeneration, evalSource, evalTitle, evalArtist, evalAlbum);
        if (changed)
            notify();
    }

    static bool isResponsivePlaybackStatus(
        GlobalSystemMediaTransportControlsSessionPlaybackStatus status) noexcept {
        using Status = GlobalSystemMediaTransportControlsSessionPlaybackStatus;
        // QQ 音乐切歌时会先短暂上报 Stopped，再提交下一首的媒体属性。
        // 只要 PlaybackInfo 仍可读取，Stopped 表示会话仍在，而不是会话已撤销。
        return status == Status::Opened || status == Status::Changing ||
               status == Status::Playing || status == Status::Paused ||
               status == Status::Stopped;
    }

    void attach(const Session& selected) {
        try {
            timelineRevoker.revoke();
        } catch (...) {
            // 旧会话刚结束时撤销事件也可能返回 HRESULT；旧会话已经失效，
            // 不能让异常穿过 SMTC 事件回调。
        }
        try {
            selectedPropsRevoker.revoke();
        } catch (...) {
            // 媒体属性监听与时间线监听分别释放，避免单次撤销失败保留旧监听。
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            session = selected;
            const auto generation = snap.sessionGeneration + 1;
            snap = {};
            snap.sessionGeneration = generation;
            thumbCacheKey_.clear();
            thumbCache_.reset();
            ++thumbnailGeneration;
            thumbnailPending = false;
            thumbnailRetryAtMs = 0;
            sampledPositionMs = 0;
            sampledDurationMs = 0;
            sampledAnchorMs = 0;
            pendingPlayMode_ = -1;
            pendingPlayModeUntilMs_ = 0;
            repeatInitialLogged_ = false;
            repeatMissingLogged_ = false;
            for (auto& adapter : adapters) {
                if (adapter)
                    adapter->reset();
            }
        }
        if (!selected) {
            notify();
            return;
        }

        try {
            timelineRevoker = selected.TimelinePropertiesChanged(winrt::auto_revoke,
                [signal = refreshSignal](auto&&, auto&&) { requestRefresh(signal); });
        } catch (...) {
            // 会话可能在绑定监听前已经被播放器撤销；下一次系统事件或后台健康检查
            // 会重新尝试绑定。
        }
        try {
            selectedPropsRevoker = selected.MediaPropertiesChanged(winrt::auto_revoke,
                [signal = refreshSignal](auto&&, auto&&) { requestRefresh(signal, false, true); });
        } catch (...) {
            // 会话可能在绑定监听前已被撤销，属性读取仍由刷新线程完成。
        }
    }

    void watchSessions(const auto& sessions) {
        sessionWatchers.clear();
        for (auto const& watched : sessions) {
            if (!watched)
                continue;
            try {
                SessionWatcher watcher;
                watcher.session = watched;
                watcher.playbackRevoker = watched.PlaybackInfoChanged(winrt::auto_revoke,
                    [signal = refreshSignal](auto&&, auto&&) { requestRefresh(signal); });
                watcher.propsRevoker = watched.MediaPropertiesChanged(winrt::auto_revoke,
                    [signal = refreshSignal](auto&&, auto&&) { requestRefresh(signal); });
                sessionWatchers.push_back(std::move(watcher));
            } catch (...) {
                // 单个已失效会话绑定失败不应阻断其他会话的监听。
            }
        }
    }

    void updateSession(bool rebuildWatchers, const std::optional<std::wstring>& sourceRequest) {
        Session found{ nullptr };
        Session preferred{ nullptr };
        Session selectedSession{ nullptr };
        bool selectedSessionAlive = false;
        SmtcPlayerType selectedPlayer = SmtcPlayerType::Unknown;
        std::wstring selectedSourceId;
        std::wstring preferredSourceId;
        {
            std::lock_guard<std::mutex> lk(mtx);
            selectedSession = session;
            selectedSessionAlive = snap.sessionAlive;
            selectedPlayer = snap.player;
            selectedSourceId = snap.sourceAppUserModelId;
            if (sourceRequest)
                sourceSelection.preferredAppUserModelId = *sourceRequest;
            preferredSourceId = sourceSelection.preferredAppUserModelId;
        }
        const bool selectedGenericDisallowed =
            selectedPlayer == SmtcPlayerType::Generic && !isGenericSourceAllowed(selectedSourceId);
        const bool selectedMatchesPreference =
            selectedSessionAlive && !selectedGenericDisallowed && !preferredSourceId.empty() &&
            _wcsicmp(selectedSourceId.c_str(), preferredSourceId.c_str()) == 0;
        bool selectedPreferenceEnded = false;
        if (sourceRequest)
            preferredSourceLossSince = {};
        if (manager) {
            try {
                // Windows 已经维护了“用户当前最可能想控制”的媒体会话。
                auto current = manager.GetCurrentSession();
                auto sessions = manager.GetSessions();
                // GetCurrentSession() 在播放器退出的边界上可能短暂返回已经断开的
                // 旧会话。先用会话列表确认它仍然存在，再读取媒体属性，避免对
                // 已失效的网易云会话继续调用 TryGetMediaPropertiesAsync()。
                bool currentListed = false;
                if (current && sessions) {
                    for (auto const& candidate : sessions) {
                        if (smtc::sameSession(current, candidate)) {
                            currentListed = true;
                            break;
                        }
                    }
                }

                smtc::SmtcSessionIdentity currentIdentity;
                using Status = GlobalSystemMediaTransportControlsSessionPlaybackStatus;
                Status currentStatus = Status::Closed;
                if (current && currentListed) {
                    currentIdentity = identifySession(current);
                    try {
                        if (auto info = current.GetPlaybackInfo())
                            currentStatus = info.PlaybackStatus();
                    } catch (...) {
                    }
                    if (currentIdentity.player != SmtcPlayerType::Unknown &&
                        currentStatus == Status::Playing)
                        found = current;
                }

                Session firstIdentified{ nullptr };
                bool selectedResponsive = false;
                if (sessions) {
                    const auto now = std::chrono::steady_clock::now();
                    const bool refreshSourceList = rebuildWatchers || sourceRequest ||
                                                   now >= nextSourceListRefresh;
                    std::vector<SmtcPlaybackSource> available;
                    bool preferredPlaying = false;
                    bool preferredIsSelected = false;
                    for (auto const& candidate : sessions)
                        recordObservedGenericSession(candidate);
                    if (rebuildWatchers)
                        watchSessions(sessions);
                    for (auto const& candidate : sessions) {
                        if (!candidate)
                            continue;
                        const bool isCurrent = currentListed && smtc::sameSession(candidate, current);
                        const bool isSelected = smtc::sameSession(candidate, selectedSession);
                        const bool isSelectedPreference = isSelected && selectedMatchesPreference;
                        if (!refreshSourceList && found) {
                            if (preferredSourceId.empty() || preferredIsSelected)
                                break;
                            // 自动候选已找到，此轮只需继续查找手动指定的来源。
                            if (!isSelectedPreference &&
                                _wcsicmp(sessionSourceAppUserModelId(candidate).c_str(),
                                         preferredSourceId.c_str()) != 0)
                                continue;
                        }
                        auto identity = isCurrent ? currentIdentity : identifySession(candidate);
                        if (identity.player == SmtcPlayerType::Unknown &&
                            (!isSelected || !selectedSessionAlive || selectedGenericDisallowed))
                            continue;

                        Status status = currentStatus;
                        if (!isCurrent) {
                            status = Status::Closed;
                            try {
                                if (auto info = candidate.GetPlaybackInfo())
                                    status = info.PlaybackStatus();
                            } catch (...) {
                            }
                        }
                        if (!isResponsivePlaybackStatus(status))
                            continue;
                        if (isSelected)
                            selectedResponsive = true;

                        const bool playing = status == Status::Playing;
                        if (identity.player != SmtcPlayerType::Unknown) {
                            if (!firstIdentified)
                                firstIdentified = candidate;
                            // 当前会话可能暂停，而另一个受支持会话正在播放。
                            if (!found && playing)
                                found = candidate;
                        } else if (isSelectedPreference ||
                                   status == Status::Opened || status == Status::Changing) {
                            // 切歌时同一会话的播放状态可读，但媒体属性可能暂时缺失。
                            identity.player = selectedPlayer;
                            identity.sourceAppUserModelId = selectedSourceId;
                        }
                        if (identity.player == SmtcPlayerType::Unknown ||
                            identity.sourceAppUserModelId.empty())
                            continue;

                        const auto sameSource = [&](const std::wstring& id) {
                            return _wcsicmp(id.c_str(), identity.sourceAppUserModelId.c_str()) == 0;
                        };
                        if (!preferredSourceId.empty() && sameSource(preferredSourceId) &&
                            (!preferred || isSelected ||
                             (!preferredIsSelected && playing && !preferredPlaying))) {
                            preferred = candidate;
                            preferredPlaying = playing;
                            preferredIsSelected = isSelected;
                        }
                        if (refreshSourceList &&
                            std::none_of(available.begin(), available.end(), [&](const auto& source) {
                                return sameSource(source.appUserModelId);
                            }))
                            available.push_back({std::move(identity.sourceAppUserModelId),
                                                 identity.player});
                    }
                    bool waitingForPreferred = false;
                    if (preferred) {
                        found = preferred;
                        preferredSourceLossSince = {};
                    } else if (!preferredSourceId.empty()) {
                        const auto preferredPlayer = player_source::resolvePlayerType(preferredSourceId);
                        const bool allowed = preferredPlayer != SmtcPlayerType::Generic ||
                                             isGenericSourceAllowed(preferredSourceId);
                        if (allowed) {
                            const auto checkedAt = std::chrono::steady_clock::now();
                            if (preferredSourceLossSince == std::chrono::steady_clock::time_point{})
                                preferredSourceLossSince = checkedAt;
                            waitingForPreferred =
                                checkedAt - preferredSourceLossSince < kSessionLossStabilityWindow;
                            // 部分播放器运行中会把暂停或切歌的会话暂时移出列表。
                            if (!waitingForPreferred && selectedMatchesPreference &&
                                playerProcessAlive(selectedPlayer)) {
                                waitingForPreferred = true;
                                preferredSourceLossSince = checkedAt;
                            }
                        }
                        if (waitingForPreferred) {
                            if (refreshSourceList)
                                available.push_back({preferredSourceId, preferredPlayer});
                        } else {
                            selectedPreferenceEnded = selectedMatchesPreference;
                            preferredSourceId.clear();
                            preferredSourceLossSince = {};
                            nextSourceListRefresh = {};
                        }
                    }
                    if (refreshSourceList) {
                        std::sort(available.begin(), available.end(), [](const auto& a, const auto& b) {
                            return _wcsicmp(a.appUserModelId.c_str(), b.appUserModelId.c_str()) < 0;
                        });
                    }
                    {
                        std::lock_guard<std::mutex> lk(mtx);
                        sourceSelection.preferredAppUserModelId = preferredSourceId;
                        if (refreshSourceList)
                            sourceSelection.available = std::move(available);
                    }
                    // 会话增减和手动切换立即刷新菜单；健康检查补足无事件的状态变化。
                    if (refreshSourceList)
                        nextSourceListRefresh = std::chrono::steady_clock::now() +
                                                std::chrono::seconds(1);
                    if (waitingForPreferred) {
                        sessionLossCandidateSince = {};
                        return;
                    }
                }
                // 暂停或停止不是切换理由：已选中的会话仍存在时保持选中。否则网易云暂停后，
                // Windows 当前会话若指向另一个暂停的播放器（如 QQ 音乐），显示会跳走，
                // 后续控制按钮也会作用到错误的会话上。当通用 SMTC 开关关闭或当前应用被规则排除时不再保留。
                if (!found && selectedSessionAlive && !selectedGenericDisallowed &&
                    selectedResponsive)
                    found = selectedSession;

                if (!found && currentListed && isResponsivePlaybackStatus(currentStatus) &&
                    currentIdentity.player != SmtcPlayerType::Unknown)
                    found = current;

                // 初次绑定或手动来源已结束时，最后才用列表中第一个可用会话。
                if (!found && (!selectedSessionAlive || selectedPreferenceEnded))
                    found = firstIdentified;
            } catch (...) {
            }
        }

        // 枚举失败时无法确认手动来源已经消失。
        if (!preferredSourceId.empty() && !preferred) {
            preferredSourceLossSince = {};
            return;
        }

        if (!found && selectedSessionAlive && selectedSession && !selectedGenericDisallowed &&
            !selectedPreferenceEnded) {
            // 汽水、Spotify 和椒盐音乐的展示以 SMTC 会话为准，会话消失后不能仅凭进程维持展示。
            // 仍经过下面的稳定窗口，短暂的会话重建不会立即清空展示。
            if (selectedPlayer != SmtcPlayerType::Soda &&
                !player_source::usesMetadataLyricPath(selectedPlayer) &&
                playerProcessAlive(selectedPlayer)) {
                sessionLossCandidateSince = {};
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (sessionLossCandidateSince == std::chrono::steady_clock::time_point{})
                sessionLossCandidateSince = now;
            if (now - sessionLossCandidateSince < kSessionLossStabilityWindow)
                return;
            sessionLossCandidateSince = {};
        } else {
            sessionLossCandidateSince = {};
        }

        Session currentSession{ nullptr };
        {
            std::lock_guard<std::mutex> lk(mtx);
            currentSession = session;
        }
        if (!smtc::sameSession(currentSession, found)) {
            try {
                attach(found);
            } catch (...) {
                // 会话切换与属性读取发生在播放器生命周期边界，失败时保持
                // 当前快照，等待下一次系统媒体会话事件或后台健康检查重试。
            }
        }
    }

    bool isStopping() const {
        std::lock_guard<std::mutex> lk(refreshSignal->mtx);
        return refreshSignal->stopping;
    }

    void initializeManagerOnPollThread() {
        try {
            auto managerOp = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
            if (!managerOp)
                return;
            while (!isStopping()) {
                const auto status = managerOp.wait_for(std::chrono::milliseconds(100));
                if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
                    manager = managerOp.GetResults();
                    break;
                }
                if (status != winrt::Windows::Foundation::AsyncStatus::Started)
                    return;
            }
            if (isStopping()) {
                if (managerOp.Status() == winrt::Windows::Foundation::AsyncStatus::Started)
                    managerOp.Cancel();
                return;
            }
            if (!manager)
                return;
            sessionsRevoker = manager.SessionsChanged(winrt::auto_revoke,
                [signal = refreshSignal](auto&&, auto&&) {
                    Impl::requestRefresh(signal, true);
                });
            currentSessionRevoker = manager.CurrentSessionChanged(winrt::auto_revoke,
                [signal = refreshSignal](auto&&, auto&&) {
                    Impl::requestRefresh(signal);
                });
        } catch (...) {
            // 没有可用媒体会话或系统媒体控制能力时，监控器保持空快照，
            // 不让启动阶段的 HRESULT 终止后台线程。
            manager = nullptr;
        }
    }

    void executeControl(ControlRequest request) {
        Session current{ nullptr };
        SmtcSnapshot snapshotCopy;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!snap.sessionAlive || request.sessionGeneration != snap.sessionGeneration)
                return;
            if (!sourceSelection.preferredAppUserModelId.empty() &&
                _wcsicmp(sourceSelection.preferredAppUserModelId.c_str(),
                         snap.sourceAppUserModelId.c_str()) != 0)
                return;
            current = session;
            snapshotCopy = snap;
        }
        switch (request.action) {
        case ControlAction::Seek:
            if (snapshotCopy.canSeek && !snapshotCopy.timelineStale && snapshotCopy.durationMs > 0) {
                const auto positionMs =
                    std::clamp(request.positionMs, int64_t{0}, snapshotCopy.durationMs);
                startControlOperation(L"seek", L"TryChangePlaybackPositionAsync", current,
                                      snapshotCopy, [current, positionMs] {
                    const auto timeline = current.GetTimelineProperties();
                    auto ticks = positionMs * 10000;
                    if (timeline && timeline.MaxSeekTime() > timeline.MinSeekTime()) {
                        ticks = std::clamp<int64_t>(ticks, timeline.MinSeekTime().count(),
                                                   timeline.MaxSeekTime().count());
                    }
                    return current.TryChangePlaybackPositionAsync(ticks);
                });
            }
            break;
        case ControlAction::PlayPause:
            startControlOperation(L"play-pause", L"TryTogglePlayPauseAsync", current, snapshotCopy,
                                  [current] { return current.TryTogglePlayPauseAsync(); });
            break;
        case ControlAction::SkipNext:
            startControlOperation(L"next", L"TrySkipNextAsync", current, snapshotCopy,
                                  [current] { return current.TrySkipNextAsync(); });
            break;
        case ControlAction::SkipPrevious:
            startControlOperation(L"previous", L"TrySkipPreviousAsync", current, snapshotCopy,
                                  [current] { return current.TrySkipPreviousAsync(); });
            break;
        case ControlAction::CycleRepeat: {
            // 只保留三档：顺序播放(0) → 单曲循环(2) → 随机播放(3) → 顺序播放。
            // 列表循环(1) 不再是独立档位；播放器自己上报列表循环时按顺序播放处理。
            // 基准优先取本地权威模式：播放器上报滞后时，仍能保证每次点击前进一档。
            const int curMode = snapshotCopy.playMode();
            const int pending = pendingPlayMode_;
            const bool pendingValid = pending >= 0 && smtc::nowUtcMs() < pendingPlayModeUntilMs_;
            const int base = pendingValid ? pending : curMode;
            const int nextMode = SmtcSnapshot::nextPlayMode(base);

            const auto repeatMode =
                static_cast<winrt::Windows::Media::MediaPlaybackAutoRepeatMode>(
                    repeatModeFor(nextMode));
            const bool wantShuffle = shuffleFor(nextMode);

            if (!current || !snapshotCopy.canRepeat) {
                writeSmtcControlLog(
                    !current ? L"phase=operation-skipped action=play-mode reason=no-session"
                             : L"phase=operation-skipped action=play-mode reason=unsupported");
                break;
            }
            // 先把本地权威值定下来，后续上报即使滞后也不会影响下一次点击。
            pendingPlayMode_ = nextMode;
            // 网易云写入后 1~2 秒才回读得到新值，窗口给到 3.5 秒足够覆盖；
            // 一旦回读到目标值会提前撤掉覆盖（见刷新路径）。
            pendingPlayModeUntilMs_ = smtc::nowUtcMs() + 3500;
            try {
                // 带超时等待异步调用，避免播放器未响应时阻塞 SMTC 轮询线程。
                auto waitControl = [](auto&& op) -> bool {
                    if (!op)
                        return false;
                    const auto status = op.wait_for(std::chrono::milliseconds(1500));
                    if (status != winrt::Windows::Foundation::AsyncStatus::Completed) {
                        if (status == winrt::Windows::Foundation::AsyncStatus::Started)
                            op.Cancel();
                        return false;
                    }
                    op.GetResults();
                    return true;
                };
                // 两个维度按顺序落地，避免并发互相覆盖。
                // 顺序有讲究：开随机时先设循环再开随机；关随机时先关随机再设循环。
                // 反过来做的话，网易云会把刚设好的循环又按"随机"改回去。
                const bool completed =
                    wantShuffle
                        ? (waitControl(current.TryChangeAutoRepeatModeAsync(repeatMode)) &&
                           waitControl(current.TryChangeShuffleActiveAsync(true)))
                        : (waitControl(current.TryChangeShuffleActiveAsync(false)) &&
                           waitControl(current.TryChangeAutoRepeatModeAsync(repeatMode)));
                if (!completed) {
                    pendingPlayMode_ = -1;
                    pendingPlayModeUntilMs_ = 0;
                    writeSmtcControlLog(
                        L"phase=operation-failed action=play-mode reason=timeout-or-canceled");
                    break;
                }
                // 回读校验：确认真的落下去了，失败也留痕，方便排查播放器不支持的情况。
                int readRepeat = -1;
                bool readShuffle = false;
                if (auto verify = current.GetPlaybackInfo()) {
                    if (auto rm = verify.AutoRepeatMode())
                        readRepeat = static_cast<int>(rm.Value());
                    if (auto sh = verify.IsShuffleActive())
                        readShuffle = sh.Value();
                }
                std::wostringstream line;
                line << L"phase=operation-complete action=play-mode from=" << base
                     << L" mode=" << nextMode << L" readRepeat=" << readRepeat
                     << L" readShuffle=" << (readShuffle ? 1 : 0)
                     << L" (readback right after call, 网易云异步生效可能仍是旧值)";
                writeSmtcControlLog(line.str());
                requestRefresh(refreshSignal);
            } catch (...) {
                pendingPlayMode_ = -1;
                pendingPlayModeUntilMs_ = 0;
                writeSmtcControlLog(L"phase=operation-failed action=play-mode");
            }
            break;
        }
        }
    }

    void shutdownWinrtOnPollThread() {
        auto safeRevoke = [](auto& revoker) {
            try {
                revoker.revoke();
            } catch (...) {
            }
        };
        safeRevoke(timelineRevoker);
        safeRevoke(selectedPropsRevoker);
        safeRevoke(currentSessionRevoker);
        safeRevoke(sessionsRevoker);
        try {
            sessionWatchers.clear();
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            session = nullptr;
        }
        manager = nullptr;
    }

    void startPolling() {
        pollThread = std::thread([this] {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (...) {
                return;
            }
            initializeManagerOnPollThread();
            requestRefresh(refreshSignal, true);
            for (;;) {
                bool rebuildWatchers = false;
                bool mediaPropertiesChanged = false;
                std::deque<ControlRequest> controls;
                std::optional<std::wstring> sourceRequest;
                int sourceSteps = 0;
                {
                    std::unique_lock<std::mutex> lk(refreshSignal->mtx);
                    refreshSignal->cv.wait_for(lk, std::chrono::milliseconds(100), [this] {
                        return refreshSignal->stopping || refreshSignal->requested ||
                               !refreshSignal->pendingControls.empty();
                    });
                    if (refreshSignal->stopping)
                        break;
                    rebuildWatchers = refreshSignal->rebuildWatchers;
                    mediaPropertiesChanged = refreshSignal->mediaPropertiesChanged;
                    refreshSignal->requested = false;
                    refreshSignal->rebuildWatchers = false;
                    refreshSignal->mediaPropertiesChanged = false;
                    controls.swap(refreshSignal->pendingControls);
                    sourceRequest.swap(refreshSignal->pendingSourceSelection);
                    sourceSteps = std::exchange(refreshSignal->pendingSourceSteps, 0);
                }
                advanceSourceRequest(sourceRequest, sourceSteps);
                const auto dispatchControls = [&] {
                    for (const auto action : controls)
                        executeControl(action);
                };
                if (!sourceRequest)
                    dispatchControls();
                try {
                    const bool couldSwitchSource = canSwitchSource();
                    updateSession(rebuildWatchers, sourceRequest);
                    refreshAll(mediaPropertiesChanged);
                    if (canSwitchSource() != couldSwitchSource)
                        notify();
                    if (sourceRequest) {
                        std::wstring activeSourceId;
                        {
                            std::lock_guard<std::mutex> lk(mtx);
                            activeSourceId = snap.sourceAppUserModelId;
                        }
                        if (sourceRequest->empty() ||
                            _wcsicmp(sourceRequest->c_str(), activeSourceId.c_str()) == 0)
                            dispatchControls();
                    }
                } catch (...) {
                }
            }
            shutdownWinrtOnPollThread();
            winrt::uninit_apartment();
        });
    }
};

SmtcMonitor::SmtcMonitor() : impl_(std::make_unique<Impl>()) {}
SmtcMonitor::~SmtcMonitor() = default;

void SmtcMonitor::start(ChangeCallback onChange) {
    writeSmtcControlLog(L"phase=monitor-start");
    impl_->onChange = std::move(onChange);
    impl_->startThumbnailReader();
    impl_->startPolling();
}

void SmtcMonitor::setSourceTargetResolver(SourceTargetResolver resolver) {
    std::lock_guard<std::mutex> lk(impl_->filterMtx);
    impl_->sourceTargetResolver = std::move(resolver);
}

void SmtcMonitor::setGenericSmtcFilter(GenericSmtcFilterConfig config) {
    {
        std::lock_guard<std::mutex> lk(impl_->filterMtx);
        impl_->genericFilter = std::move(config);
    }
    Impl::requestRefresh(impl_->refreshSignal);
}

void SmtcMonitor::setGenericSmtcEnabled(bool enabled) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(impl_->filterMtx);
        changed = impl_->genericFilter.enabled != enabled;
        impl_->genericFilter.enabled = enabled;
    }
    if (changed)
        Impl::requestRefresh(impl_->refreshSignal);
}

std::vector<std::wstring> SmtcMonitor::observedGenericSources() const {
    std::lock_guard<std::mutex> lk(impl_->filterMtx);
    return impl_->observedGenericSourceIds;
}

SmtcSourceSelection SmtcMonitor::sourceSelection() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->sourceSelection;
}

void SmtcMonitor::selectSource(std::wstring appUserModelId) {
    auto& signal = *impl_->refreshSignal;
    {
        std::lock_guard<std::mutex> lk(signal.mtx);
        if (signal.stopping)
            return;
        signal.pendingSourceSelection = std::move(appUserModelId);
        signal.pendingSourceSteps = 0;
        signal.pendingControls.clear();
        signal.requested = true;
    }
    signal.cv.notify_one();
}

void SmtcMonitor::cycleSource(int steps) {
    auto& signal = *impl_->refreshSignal;
    {
        std::lock_guard<std::mutex> lk(signal.mtx);
        if (signal.stopping)
            return;
        signal.pendingSourceSteps += steps;
        signal.pendingControls.clear();
        signal.requested = true;
    }
    signal.cv.notify_one();
}

SmtcSnapshot SmtcMonitor::snapshot() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    SmtcSnapshot source = impl_->snap;
    source.canSwitchSource = impl_->sourceSelection.available.size() > 1;
    if (!source.sessionAlive)
        return source;
    if (auto* adapter = impl_->adapterFor(source.player))
        return adapter->snapshot(source, smtc::nowUtcMs());
    return source;
}

void SmtcMonitor::playPause(uint64_t sessionGeneration) {
    Impl::requestControl(impl_->refreshSignal, {Impl::ControlAction::PlayPause, sessionGeneration});
}

void SmtcMonitor::seek(uint64_t sessionGeneration, int64_t positionMs) {
    Impl::requestControl(impl_->refreshSignal,
                         {Impl::ControlAction::Seek, sessionGeneration, positionMs});
}

void SmtcMonitor::skipNext(uint64_t sessionGeneration) {
    Impl::requestControl(impl_->refreshSignal, {Impl::ControlAction::SkipNext, sessionGeneration});
}

void SmtcMonitor::skipPrevious(uint64_t sessionGeneration) {
    Impl::requestControl(impl_->refreshSignal, {Impl::ControlAction::SkipPrevious, sessionGeneration});
}

void SmtcMonitor::cycleRepeatMode(uint64_t sessionGeneration) {
    Impl::requestControl(impl_->refreshSignal, {Impl::ControlAction::CycleRepeat, sessionGeneration});
}
