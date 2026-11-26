#include "test_support.h"

#include "model/display_types.h"
#include "quota/quota_provider.h"
#include "quota/quota_types.h"
#include "settings/app_settings.h"

#include <nlohmann/json.hpp>

#include <string>

namespace {

using nlohmann::json;

std::string base64UrlEncode(const std::string& input) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    int bits = 0;
    unsigned accumulator = 0;
    for (const unsigned char character : input) {
        accumulator = (accumulator << 8) | character;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(kTable[(accumulator >> bits) & 0x3F]);
        }
    }
    if (bits > 0)
        output.push_back(kTable[(accumulator << (6 - bits)) & 0x3F]);
    return output;
}

std::wstring codexToken(const std::string& payloadJson) {
    return quota_detail::toWide("e30." + base64UrlEncode(payloadJson) + ".signature");
}

} // namespace

TEST_CASE("quota.deepseek_balance_parsing") {
    QuotaSnapshot snapshot;
    std::wstring error;
    const json body = {
        {"is_available", true},
        {"balance_infos",
         {{{"currency", "USD"},
           {"total_balance", "1.25"},
           {"granted_balance", "0.25"},
           {"topped_up_balance", "1.00"}},
          {{"currency", "CNY"},
           {"total_balance", "28.60"},
           {"granted_balance", "10.00"},
           {"topped_up_balance", "18.60"}}}},
    };
    EXPECT_TRUE(quota_detail::parseDeepSeekBalance(body, snapshot, error));
    EXPECT_TRUE(snapshot.hasBalance);
    EXPECT_EQ(snapshot.balance.currency, std::wstring(L"CNY"));
    EXPECT_NEAR(snapshot.balance.total, 28.6, 1e-6);
    EXPECT_NEAR(snapshot.balance.granted, 10.0, 1e-6);

    QuotaSnapshot malformed;
    EXPECT_FALSE(quota_detail::parseDeepSeekBalance(json::object(), malformed, error));
    EXPECT_FALSE(quota_detail::parseDeepSeekBalance(
        json{{"is_available", true}, {"balance_infos", json::array()}}, malformed, error));
    EXPECT_FALSE(quota_detail::parseDeepSeekBalance(
        json{{"is_available", true},
             {"balance_infos", {{{"currency", "CNY"},
                                 {"total_balance", "NaN"},
                                 {"granted_balance", "0"},
                                 {"topped_up_balance", "0"}}}}},
        malformed, error));
    EXPECT_FALSE(quota_detail::parseDeepSeekBalance(
        json{{"is_available", true},
             {"balance_infos", {{{"currency", "EUR"},
                                 {"total_balance", "1"},
                                 {"granted_balance", "0"},
                                 {"topped_up_balance", "1"}}}}},
        malformed, error));
}

TEST_CASE("quota.codex_usage_windows") {
    QuotaSnapshot snapshot;
    std::wstring error;
    const json body = {
        {"plan_type", "pro"},
        {"rate_limit",
         {{"primary_window",
           {{"used_percent", 0}, {"limit_window_seconds", 18000}, {"reset_at", 1900000000}}},
          {"secondary_window",
           {{"used_percent", 25},
            {"limit_window_seconds", 604800},
            {"reset_at", "2030-01-01T00:00:00Z"}}}}},
    };
    EXPECT_TRUE(quota_detail::parseCodexUsage(body, snapshot, error));
    EXPECT_EQ(snapshot.plan, std::wstring(L"pro"));
    EXPECT_EQ(snapshot.windows.size(), size_t(2));
    EXPECT_EQ(snapshot.windows[0].label, std::wstring(L"5h"));
    EXPECT_EQ(snapshot.windows[0].remainingPercent, 100);
    EXPECT_TRUE(snapshot.windows[0].resetAt.has_value());
    EXPECT_EQ(snapshot.windows[1].label, std::wstring(L"周"));
    EXPECT_EQ(snapshot.windows[1].remainingPercent, 75);
    EXPECT_TRUE(snapshot.windows[1].resetAt.has_value());

    QuotaSnapshot weekly;
    const json weeklyOnly = {
        {"rate_limit",
         {{"primary_window", nullptr},
          {"secondary_window",
           {{"used_percent", 43}, {"limit_window_seconds", 604800}, {"reset_at", 1900000000}}}}},
    };
    EXPECT_TRUE(quota_detail::parseCodexUsage(weeklyOnly, weekly, error));
    EXPECT_EQ(weekly.windows.size(), size_t(1));
    EXPECT_EQ(weekly.windows[0].label, std::wstring(L"周"));
    EXPECT_EQ(weekly.windows[0].remainingPercent, 57);

    QuotaSnapshot invalid;
    EXPECT_FALSE(quota_detail::parseCodexUsage(json{{"rate_limit", json::object()}}, invalid, error));
    EXPECT_FALSE(quota_detail::parseCodexUsage(
        json{{"rate_limit", {{"primary_window", {{"reset_at", 1900000000}}}}}}, invalid, error));
    EXPECT_FALSE(quota_detail::parseCodexUsage(json::object(), invalid, error));
}

TEST_CASE("quota.codex_auth_file_claims") {
    const std::string payload =
        R"({"exp":1900000000,"sub":"user-1",)"
        R"("https://api.openai.com/auth":{"chatgpt_account_id":"acct-claim",)"
        R"("chatgpt_user_id":"user-claim"}})";
    const std::wstring token = codexToken(payload);
    const json auth = {
        {"auth_mode", "chatgpt"},
        {"tokens", {{"access_token", quota_detail::toUtf8(token)}, {"account_id", "acct-token"}}},
    };
    quota_detail::CodexCredential credential;
    std::wstring error;
    EXPECT_TRUE(quota_detail::parseCodexAuthJson(auth, credential, error));
    EXPECT_EQ(credential.accountId, std::wstring(L"acct-token"));
    EXPECT_EQ(credential.identity, std::wstring(L"acct-token:user-claim"));
    EXPECT_TRUE(credential.expiresAt.has_value());
    EXPECT_EQ(*credential.expiresAt, int64_t(1900000000));

    const json fallback = {{"tokens", {{"access_token", quota_detail::toUtf8(token)}}}};
    quota_detail::CodexCredential fromClaims;
    EXPECT_TRUE(quota_detail::parseCodexAuthJson(fallback, fromClaims, error));
    EXPECT_EQ(fromClaims.accountId, std::wstring(L"acct-claim"));
    EXPECT_EQ(fromClaims.identity, std::wstring(L"acct-claim:user-claim"));

    quota_detail::CodexCredential missing;
    EXPECT_FALSE(quota_detail::parseCodexAuthJson(json{{"tokens", json::object()}}, missing, error));
    EXPECT_FALSE(quota_detail::parseCodexAuthJson(json::array(), missing, error));
}

TEST_CASE("quota.display_formatting") {
    EXPECT_EQ(quota_detail::formatPercent(43), std::wstring(L"43%"));
    EXPECT_EQ(quota_detail::formatPercent(120), std::wstring(L"100%"));
    EXPECT_EQ(quota_detail::formatCompactAmount(L"CNY", 28.60), std::wstring(L"¥28.6"));
    EXPECT_EQ(quota_detail::formatCompactAmount(L"CNY", 999.0), std::wstring(L"¥999"));
    EXPECT_EQ(quota_detail::formatCompactAmount(L"CNY", 1280.5), std::wstring(L"¥1.28k"));
    EXPECT_EQ(quota_detail::formatCompactAmount(L"CNY", 12800.0), std::wstring(L"¥1.28万"));
    EXPECT_EQ(quota_detail::formatCompactAmount(L"CNY", 128000.0), std::wstring(L"¥12.8万"));
    EXPECT_EQ(quota_detail::formatCompactAmount(L"CNY", 9999.99), std::wstring(L"¥9.99k"));
    EXPECT_EQ(quota_detail::formatCompactAmount(L"CNY", 100000000.0), std::wstring(L"¥1亿"));
    EXPECT_EQ(quota_detail::formatFullAmount(L"CNY", 12800.0), std::wstring(L"¥12,800.00"));
    EXPECT_EQ(quota_detail::formatResetIn(std::optional<int64_t>(1000 + 120), 1000),
              std::wstring(L"2 分钟后重置"));
    EXPECT_EQ(quota_detail::formatResetIn(std::optional<int64_t>(1000 + 7200), 1000),
              std::wstring(L"2 小时后重置"));
    EXPECT_EQ(quota_detail::formatResetIn(std::nullopt, 1000), std::wstring());
    EXPECT_EQ(quota_detail::shortWindowLabel(18000), std::wstring(L"5h"));
    EXPECT_EQ(quota_detail::shortWindowLabel(604800), std::wstring(L"周"));
    EXPECT_EQ(quota_detail::shortWindowLabel(172800), std::wstring(L"2天"));
    EXPECT_EQ(quota_detail::describeWindow(18000), std::wstring(L"5 小时"));
    EXPECT_EQ(quota_detail::describeWindow(604800), std::wstring(L"本周"));
}

TEST_CASE("quota.settings_round_trip") {
    AppSettings defaults;
    EXPECT_FALSE(defaults.quotaEnabled_);
    EXPECT_TRUE(defaults.quotaCodexEnabled_);
    EXPECT_TRUE(defaults.quotaDeepSeekEnabled_);

    AppSettings saved;
    saved.quotaEnabled_ = true;
    saved.quotaCodexEnabled_ = false;
    const nlohmann::json document = saved.toJson();
    AppSettings loaded;
    bool migrateLegacyLogDirectory = false;
    loaded.readJson(document, false, migrateLegacyLogDirectory);
    EXPECT_TRUE(loaded.quotaEnabled_);
    EXPECT_FALSE(loaded.quotaCodexEnabled_);
    EXPECT_TRUE(loaded.quotaDeepSeekEnabled_);
}

TEST_CASE("quota.status_visibility_flags") {
    DockResourceVisibility visibility;
    visibility.cpuUsage = false;
    visibility.memoryUsage = false;
    visibility.download = false;
    visibility.upload = false;
    visibility.gpuUsage = false;
    visibility.cpuFrequency = false;
    EXPECT_FALSE(visibility.hasAnyQuota());
    visibility.quotaEnabled = true;
    EXPECT_TRUE(visibility.hasAnyQuota());
    EXPECT_FALSE(visibility.hasAnyMetric());
    visibility.quotaCodex = false;
    visibility.quotaDeepSeek = false;
    EXPECT_FALSE(visibility.hasAnyQuota());
    visibility.quotaDeepSeek = true;
    EXPECT_TRUE(visibility.hasAnyQuota());
}

TEST_CASE("quota.deepseek_key_validation") {
    EXPECT_TRUE(QuotaProviderClient::validDeepSeekKey(L"sk-1234567890"));
    EXPECT_FALSE(QuotaProviderClient::validDeepSeekKey(L""));
    EXPECT_FALSE(QuotaProviderClient::validDeepSeekKey(L"sk 123"));
    EXPECT_FALSE(QuotaProviderClient::validDeepSeekKey(L"sk-123\n"));
    EXPECT_FALSE(QuotaProviderClient::validDeepSeekKey(std::wstring(257, L'a')));
}
