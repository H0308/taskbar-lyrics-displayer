#pragma once

#include "quota/quota_types.h"

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

namespace quota_detail {

// Codex 本机登录文件解析结果。令牌只在请求期间使用，不写入日志或配置。
struct CodexCredential {
    std::wstring accessToken;
    std::wstring accountId;
    std::wstring identity;
    std::optional<int64_t> expiresAt; // Unix 秒

    bool valid() const { return !accessToken.empty(); }
};

// 解析 DeepSeek /user/balance 响应；成功时写入 windows 之外的余额信息。
bool parseDeepSeekBalance(const nlohmann::json& body, QuotaSnapshot& snapshot,
                          std::wstring& error);
// 解析 Codex /backend-api/wham/usage 响应，保留 5 小时与每周两个主窗口。
bool parseCodexUsage(const nlohmann::json& body, QuotaSnapshot& snapshot, std::wstring& error);
// 解析 ~/.codex/auth.json：tokens.access_token、tokens.account_id 与 JWT 声明。
bool parseCodexAuthJson(const nlohmann::json& body, CodexCredential& credential,
                        std::wstring& error);

std::wstring formatPercent(int percent);
// 完整金额（含千分位与两位小数），用于设置页说明。
std::wstring formatFullAmount(const std::wstring& currency, double value);
// 紧凑金额（¥28.6 / ¥1.28k / ¥1.28万 / ¥1亿），用于任务栏。
std::wstring formatCompactAmount(const std::wstring& currency, double value);
std::wstring formatResetIn(const std::optional<int64_t>& resetAt, int64_t now);
// 任务栏短标签：5h / 周 / 日 / N天。
std::wstring shortWindowLabel(int periodSeconds);
// 设置页说明：5 小时 / 本周 / 每日 / N 天 / N 小时。
std::wstring describeWindow(int periodSeconds);

std::string toUtf8(std::wstring_view text);
std::wstring toWide(std::string_view text);

} // namespace quota_detail

// 同步查询单个服务；调用方负责工作线程、取消与结果投递。
class QuotaProviderClient {
public:
    using CancelFlag = std::atomic<bool>;

    QuotaSnapshot fetchCodex(const CancelFlag& cancelled) const;
    QuotaSnapshot fetchDeepSeek(const std::wstring& apiKey, const CancelFlag& cancelled) const;

    // DeepSeek API Key 保存在 Windows 凭据管理器（不进入 settings.json）。
    static bool loadDeepSeekKey(std::wstring& key);
    static bool saveDeepSeekKey(const std::wstring& key);
    static void clearDeepSeekKey();
    static bool deepSeekKeyConfigured();
    static bool validDeepSeekKey(std::wstring_view key);

    // Codex 直接读取 ~/.codex/auth.json（可用 CODEX_HOME 覆盖目录），不再单独授权。
    static std::wstring codexAuthPath();
    static bool codexLoginConfigured();
};
