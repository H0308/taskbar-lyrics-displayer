#include "quota/quota_provider.h"

#include "app_info.h"
#include "update/update_service.h"

#include <windows.h>
#include <wincred.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace quota_detail {
namespace {

using json = nlohmann::json;

std::optional<double> numberValue(const json& value) {
    if (value.is_number()) {
        const double number = value.get<double>();
        if (std::isfinite(number))
            return number;
        return std::nullopt;
    }
    if (value.is_string()) {
        try {
            size_t parsed = 0;
            const std::string& text = value.get_ref<const std::string&>();
            const double number = std::stod(text, &parsed);
            if (parsed == text.size() && std::isfinite(number))
                return number;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<int64_t> timestampValue(const json& value) {
    if (value.is_number_integer()) {
        const int64_t stamp = value.get<int64_t>();
        if (stamp <= 0)
            return std::nullopt;
        return stamp;
    }
    if (value.is_number_unsigned()) {
        const uint64_t stamp = value.get<uint64_t>();
        if (stamp > 0 && stamp <= static_cast<uint64_t>(INT64_MAX))
            return static_cast<int64_t>(stamp);
        return std::nullopt;
    }
    if (!value.is_string())
        return std::nullopt;
    const std::string text = value.get<std::string>();
    if (text.size() < 19)
        return std::nullopt;

    std::tm utc{};
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf_s(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute,
                 &second) != 6)
        return std::nullopt;
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;

    int offsetSeconds = 0;
    const size_t zone = text.find_first_of("Zz+-", 19);
    if (zone != std::string::npos && text[zone] != 'Z' && text[zone] != 'z') {
        int offsetHour = 0;
        int offsetMinute = 0;
        if (sscanf_s(text.c_str() + zone + 1, "%2d:%2d", &offsetHour, &offsetMinute) < 1)
            return std::nullopt;
        offsetSeconds = offsetHour * 3600 + offsetMinute * 60;
        if (text[zone] == '-')
            offsetSeconds = -offsetSeconds;
    }
    const __time64_t stamp = _mkgmtime64(&utc);
    if (stamp == -1)
        return std::nullopt;
    return static_cast<int64_t>(stamp) - offsetSeconds;
}

std::wstring currencySymbol(const std::wstring& currency) {
    if (currency == L"CNY")
        return L"¥";
    if (currency == L"USD")
        return L"$";
    return currency + L" ";
}

std::wstring trimTrailingZeros(std::wstring text) {
    if (text.find(L'.') == std::wstring::npos)
        return text;
    while (!text.empty() && text.back() == L'0')
        text.pop_back();
    if (!text.empty() && text.back() == L'.')
        text.pop_back();
    return text;
}

std::string base64UrlDecode(std::string_view input) {
    std::string result;
    int bits = 0;
    int accumulator = 0;
    for (const char character : input) {
        int value = -1;
        if (character >= 'A' && character <= 'Z')
            value = character - 'A';
        else if (character >= 'a' && character <= 'z')
            value = character - 'a' + 26;
        else if (character >= '0' && character <= '9')
            value = character - '0' + 52;
        else if (character == '-' || character == '+')
            value = 62;
        else if (character == '_' || character == '/')
            value = 63;
        else if (character == '=')
            break;
        else
            return {};
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
        }
    }
    return result;
}

// 从 JWT 载荷读取 Codex 账号与过期时间；解析失败时保持空，不影响主流程。
struct JwtClaims {
    std::string subject;
    std::string chatgptUserId;
    std::string chatgptAccountId;
    std::optional<int64_t> expiresAt;
};

JwtClaims readJwtClaims(const std::wstring& token) {
    JwtClaims claims;
    const std::string utf8 = toUtf8(token);
    const size_t first = utf8.find('.');
    if (first == std::string::npos)
        return claims;
    const size_t second = utf8.find('.', first + 1);
    if (second == std::string::npos || second == first + 1)
        return claims;
    const std::string payload =
        base64UrlDecode(std::string_view(utf8).substr(first + 1, second - first - 1));
    if (payload.empty())
        return claims;
    const json body = json::parse(payload, nullptr, false);
    if (body.is_discarded() || !body.is_object())
        return claims;
    if (body.contains("sub") && body["sub"].is_string())
        claims.subject = body["sub"].get<std::string>();
    if (body.contains("exp")) {
        const auto exp = numberValue(body["exp"]);
        if (exp && *exp > 0)
            claims.expiresAt = static_cast<int64_t>(*exp);
    }
    const auto auth = body.find("https://api.openai.com/auth");
    if (auth != body.end() && auth->is_object()) {
        const auto account = auth->find("chatgpt_account_id");
        if (account != auth->end() && account->is_string())
            claims.chatgptAccountId = account->get<std::string>();
        const auto user = auth->find("chatgpt_user_id");
        if (user != auth->end() && user->is_string())
            claims.chatgptUserId = user->get<std::string>();
    }
    return claims;
}

} // namespace

std::string toUtf8(std::wstring_view text) {
    if (text.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
    return result;
}

std::wstring toWide(std::string_view text) {
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
    return result;
}

std::wstring formatPercent(int percent) {
    return std::to_wstring(std::clamp(percent, 0, 100)) + L"%";
}

std::wstring formatFullAmount(const std::wstring& currency, double value) {
    const double magnitude = std::fabs(value);
    wchar_t raw[64]{};
    swprintf_s(raw, L"%.2f", magnitude);
    std::wstring text = raw;
    const size_t dot = text.find(L'.');
    const std::wstring integerPart = text.substr(0, dot);
    const std::wstring decimalPart = dot == std::wstring::npos ? L"00" : text.substr(dot + 1);
    std::wstring grouped;
    for (size_t i = 0; i < integerPart.size(); ++i) {
        if (i > 0 && (integerPart.size() - i) % 3 == 0)
            grouped.push_back(L',');
        grouped.push_back(integerPart[i]);
    }
    return currencySymbol(currency) + (value < 0.0 ? L"-" : L"") + grouped + L"." + decimalPart;
}

std::wstring formatCompactAmount(const std::wstring& currency, double value) {
    const double magnitude = std::fabs(value);
    double scaled = magnitude;
    std::wstring suffix;
    if (magnitude >= 100000000.0) {
        scaled /= 100000000.0;
        suffix = L"亿";
    } else if (magnitude >= 10000.0) {
        scaled /= 10000.0;
        suffix = L"万";
    } else if (magnitude >= 1000.0) {
        scaled /= 1000.0;
        suffix = L"k";
    }
    const int decimals = suffix.empty() ? (magnitude < 100.0 ? 1 : 0)
                                        : (scaled < 10.0 ? 2 : 1);
    // 与单位阈值一致地截断而不是四舍五入，避免 9999.99 显示成“10k”。
    const double factor = std::pow(10.0, static_cast<double>(decimals));
    const double truncated = std::trunc(scaled * factor) / factor;
    wchar_t raw[64]{};
    swprintf_s(raw, L"%.*f", decimals, truncated);
    const std::wstring text = trimTrailingZeros(raw);
    return currencySymbol(currency) + (value < 0.0 ? L"-" : L"") + text + suffix;
}

std::wstring formatResetIn(const std::optional<int64_t>& resetAt, int64_t now) {
    if (!resetAt || *resetAt <= 0)
        return {};
    const int64_t delta = *resetAt - now;
    if (delta <= 60)
        return L"即将重置";
    if (delta < 3600)
        return std::to_wstring(delta / 60) + L" 分钟后重置";
    if (delta < 86400)
        return std::to_wstring(delta / 3600) + L" 小时后重置";
    return std::to_wstring(delta / 86400) + L" 天后重置";
}

std::wstring shortWindowLabel(int periodSeconds) {
    if (periodSeconds == 18000)
        return L"5h";
    if (periodSeconds == 604800)
        return L"周";
    if (periodSeconds == 86400)
        return L"日";
    if (periodSeconds > 0 && periodSeconds % 86400 == 0)
        return std::to_wstring(periodSeconds / 86400) + L"天";
    if (periodSeconds > 0 && periodSeconds % 3600 == 0)
        return std::to_wstring(periodSeconds / 3600) + L"h";
    return L"额度";
}

std::wstring describeWindow(int periodSeconds) {
    if (periodSeconds == 18000)
        return L"5 小时";
    if (periodSeconds == 604800)
        return L"本周";
    if (periodSeconds == 86400)
        return L"每日";
    if (periodSeconds > 0 && periodSeconds % 86400 == 0)
        return std::to_wstring(periodSeconds / 86400) + L" 天";
    if (periodSeconds > 0 && periodSeconds % 3600 == 0)
        return std::to_wstring(periodSeconds / 3600) + L" 小时";
    return L"额度周期";
}

bool parseDeepSeekBalance(const json& body, QuotaSnapshot& snapshot, std::wstring& error) {
    if (!body.is_object()) {
        error = L"余额响应格式无效";
        return false;
    }
    const auto available = body.find("is_available");
    if (available == body.end() || !available->is_boolean()) {
        error = L"余额响应缺少账户状态";
        return false;
    }
    const auto infos = body.find("balance_infos");
    if (infos == body.end() || !infos->is_array() || infos->empty()) {
        error = L"DeepSeek 未返回余额";
        return false;
    }
    std::vector<QuotaBalance> balances;
    for (const auto& row : *infos) {
        if (!row.is_object()) {
            error = L"余额响应格式无效";
            return false;
        }
        const auto currencyIt = row.find("currency");
        if (currencyIt == row.end() || !currencyIt->is_string()) {
            error = L"余额响应缺少币种";
            return false;
        }
        const std::wstring currency = toWide(currencyIt->get<std::string>());
        if (currency != L"CNY" && currency != L"USD") {
            error = L"暂不支持该余额币种";
            return false;
        }
        const auto total = numberValue(row.contains("total_balance") ? row["total_balance"] : json());
        const auto granted =
            numberValue(row.contains("granted_balance") ? row["granted_balance"] : json());
        const auto toppedUp =
            numberValue(row.contains("topped_up_balance") ? row["topped_up_balance"] : json());
        if (!total || !granted || !toppedUp) {
            error = L"余额字段无效，请稍后刷新";
            return false;
        }
        if (std::any_of(balances.begin(), balances.end(), [&](const QuotaBalance& existing) {
                return existing.currency == currency;
            })) {
            error = L"余额币种重复";
            return false;
        }
        balances.push_back({currency, *total, *granted, *toppedUp});
    }
    const auto primary = std::find_if(balances.begin(), balances.end(), [](const QuotaBalance& row) {
        return row.currency == L"CNY";
    });
    snapshot.hasBalance = true;
    snapshot.balance = primary != balances.end() ? *primary : balances.front();
    snapshot.plan = available->get<bool>() ? std::wstring() : L"余额不可用";
    return true;
}

bool parseCodexUsage(const json& body, QuotaSnapshot& snapshot, std::wstring& error) {
    if (!body.is_object()) {
        error = L"额度响应格式无效";
        return false;
    }
    const auto rateLimit = body.find("rate_limit");
    if (rateLimit == body.end() || !rateLimit->is_object()) {
        error = L"服务没有返回额度信息";
        return false;
    }
    if (body.contains("plan_type") && body["plan_type"].is_string())
        snapshot.plan = toWide(body["plan_type"].get<std::string>());

    auto addWindow = [&](const json& value) {
        if (!value.is_object())
            return false;
        const auto usedIt = value.find("used_percent");
        if (usedIt == value.end())
            return false;
        const auto used = numberValue(*usedIt);
        if (!used)
            return false;
        QuotaWindow window;
        if (value.contains("limit_window_seconds")) {
            const auto seconds = numberValue(value["limit_window_seconds"]);
            if (seconds && *seconds > 0 && *seconds < 366.0 * 86400.0)
                window.periodSeconds = static_cast<int>(*seconds);
        }
        window.label = shortWindowLabel(window.periodSeconds);
        window.remainingPercent = std::clamp(static_cast<int>(std::lround(100.0 - *used)), 0, 100);
        if (value.contains("reset_at"))
            window.resetAt = timestampValue(value["reset_at"]);
        snapshot.windows.push_back(std::move(window));
        return true;
    };

    for (const char* key : {"primary_window", "secondary_window"}) {
        const auto window = rateLimit->find(key);
        if (window == rateLimit->end() || window->is_null())
            continue;
        if (!window->is_object() || !addWindow(*window)) {
            error = L"额度字段不完整，暂时无法判断周期";
            snapshot.windows.clear();
            return false;
        }
    }
    if (snapshot.windows.empty()) {
        error = L"服务未返回周期额度";
        return false;
    }
    std::sort(snapshot.windows.begin(), snapshot.windows.end(),
              [](const QuotaWindow& left, const QuotaWindow& right) {
                  return left.periodSeconds < right.periodSeconds;
              });
    if (snapshot.windows.size() > 2)
        snapshot.windows.resize(2);
    return true;
}

bool parseCodexAuthJson(const json& body, CodexCredential& credential, std::wstring& error) {
    if (!body.is_object()) {
        error = L"登录文件不是有效的 JSON";
        return false;
    }
    const json* auth = &body;
    const auto wrapped = body.find("auth_json");
    if (wrapped != body.end() && wrapped->is_object())
        auth = &*wrapped;

    std::wstring token;
    std::wstring accountId;
    const auto tokens = auth->find("tokens");
    if (tokens != auth->end() && tokens->is_object()) {
        if (tokens->contains("access_token") && (*tokens)["access_token"].is_string())
            token = toWide((*tokens)["access_token"].get<std::string>());
        if (tokens->contains("account_id") && (*tokens)["account_id"].is_string())
            accountId = toWide((*tokens)["account_id"].get<std::string>());
    }
    if (token.empty() && auth->contains("access_token") && (*auth)["access_token"].is_string())
        token = toWide((*auth)["access_token"].get<std::string>());
    if (accountId.empty() && auth->contains("account_id") && (*auth)["account_id"].is_string())
        accountId = toWide((*auth)["account_id"].get<std::string>());
    if (token.empty()) {
        error = L"登录文件缺少 access token，请在 Codex 中重新登录";
        return false;
    }

    const JwtClaims claims = readJwtClaims(token);
    if (accountId.empty())
        accountId = toWide(claims.chatgptAccountId);
    const std::wstring userId = toWide(claims.chatgptUserId.empty() ? claims.subject
                                                                    : claims.chatgptUserId);
    if (!accountId.empty() && !userId.empty())
        credential.identity = accountId + L":" + userId;
    else
        credential.identity = accountId + userId;
    credential.accessToken = std::move(token);
    credential.accountId = std::move(accountId);
    credential.expiresAt = claims.expiresAt;
    return true;
}

} // namespace quota_detail

namespace {

using quota_detail::CodexCredential;
using quota_detail::toUtf8;
using quota_detail::toWide;

constexpr char kDeepSeekBalanceUrl[] = "https://api.deepseek.com/user/balance";
constexpr char kCodexUsageUrl[] = "https://chatgpt.com/backend-api/wham/usage";
constexpr size_t kMaxResponseBytes = 1024 * 1024;
constexpr size_t kMaxAuthFileBytes = 4 * 1024 * 1024;

struct HttpResponse {
    CURLcode curlResult = CURLE_FAILED_INIT;
    long statusCode = 0;
    std::string body;
};

size_t curlWrite(char* data, size_t size, size_t count, void* userdata) {
    auto* output = static_cast<std::string*>(userdata);
    const size_t total = size * count;
    if (output->size() + total > kMaxResponseBytes)
        return 0;
    output->append(data, total);
    return total;
}

int curlAbort(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* cancelled = static_cast<const std::atomic<bool>*>(userdata);
    return cancelled && cancelled->load() ? 1 : 0;
}

bool httpGet(const std::string& url, const std::vector<std::string>& headerLines,
             const std::atomic<bool>& cancelled, HttpResponse& response) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return false;
    curl_slist* headers = nullptr;
    for (const auto& line : headerLines)
        headers = curl_slist_append(headers, line.c_str());

    const std::string proxy = update_service::systemProxyForUrl(toWide(url));
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlAbort);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    response.curlResult = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    if (headers)
        curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response.curlResult == CURLE_OK;
}

bool readFile(const std::wstring& path, std::string& content) {
    std::error_code error;
    const auto size = std::filesystem::file_size(std::filesystem::path(path), error);
    if (error || size > kMaxAuthFileBytes)
        return false;
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input)
        return false;
    content.assign(static_cast<size_t>(size), '\0');
    if (size > 0 && !input.read(content.data(), static_cast<std::streamsize>(size)))
        return false;
    if (content.starts_with("\xEF\xBB\xBF"))
        content.erase(0, 3);
    return true;
}

constexpr wchar_t kDeepSeekCredentialTarget[] = L"QQMusicLyric.DeepSeek.ApiKey";

std::wstring userProfileDirectory() {
    wchar_t buffer[32768]{};
    const DWORD length =
        GetEnvironmentVariableW(L"USERPROFILE", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
        return {};
    return buffer;
}

QuotaSnapshot failureSnapshot(QuotaProvider provider, std::wstring error, bool authRequired,
                              int retryAfterSeconds) {
    QuotaSnapshot snapshot;
    snapshot.provider = provider;
    snapshot.status = QuotaStatus::Error;
    snapshot.error = std::move(error);
    snapshot.authRequired = authRequired;
    snapshot.retryAfterSeconds = retryAfterSeconds;
    return snapshot;
}

std::wstring statusErrorText(long status, const wchar_t* service) {
    wchar_t text[160]{};
    swprintf_s(text, L"%s 暂时不可用（HTTP %ld）", service, status);
    return text;
}

bool readCredentialBlob(const std::wstring& target, std::string& blob) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential) || !credential)
        return false;
    if (credential->CredentialBlob && credential->CredentialBlobSize > 0)
        blob.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                    credential->CredentialBlobSize);
    CredFree(credential);
    return !blob.empty();
}

bool writeCredentialBlob(const std::wstring& target, const std::string& blob) {
    if (blob.empty() || blob.size() > MAXDWORD)
        return false;
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(blob.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) != FALSE;
}

} // namespace

std::wstring QuotaProviderClient::codexAuthPath() {
    wchar_t home[32768]{};
    const DWORD homeLength =
        GetEnvironmentVariableW(L"CODEX_HOME", home, static_cast<DWORD>(std::size(home)));
    std::wstring directory;
    if (homeLength > 0 && homeLength < std::size(home)) {
        directory = home;
    } else {
        const std::wstring profile = userProfileDirectory();
        if (profile.empty())
            return {};
        directory = profile + L"\\.codex";
    }
    if (directory.empty())
        return {};
    if (directory.back() == L'\\' || directory.back() == L'/')
        return directory + L"auth.json";
    return directory + L"\\auth.json";
}

bool QuotaProviderClient::codexLoginConfigured() {
    const std::wstring path = codexAuthPath();
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool QuotaProviderClient::loadDeepSeekKey(std::wstring& key) {
    key.clear();
    std::string blob;
    if (!readCredentialBlob(kDeepSeekCredentialTarget, blob))
        return false;
    key = toWide(blob);
    return !key.empty();
}

bool QuotaProviderClient::saveDeepSeekKey(const std::wstring& key) {
    return writeCredentialBlob(kDeepSeekCredentialTarget, toUtf8(key));
}

void QuotaProviderClient::clearDeepSeekKey() {
    CredDeleteW(kDeepSeekCredentialTarget, CRED_TYPE_GENERIC, 0);
}

bool QuotaProviderClient::deepSeekKeyConfigured() {
    std::wstring key;
    return loadDeepSeekKey(key);
}

bool QuotaProviderClient::validDeepSeekKey(std::wstring_view key) {
    if (key.empty() || key.size() > 256)
        return false;
    return std::none_of(key.begin(), key.end(), [](wchar_t character) {
        return iswspace(character) != 0 || iswcntrl(character) != 0;
    });
}

QuotaSnapshot QuotaProviderClient::fetchDeepSeek(const std::wstring& apiKey,
                                                 const CancelFlag& cancelled) const {
    if (!validDeepSeekKey(apiKey))
        return failureSnapshot(QuotaProvider::DeepSeek, L"请输入有效的 DeepSeek API Key", true, 0);

    HttpResponse response;
    const std::vector<std::string> headers = {
        "Accept: application/json",
        "Authorization: Bearer " + toUtf8(apiKey),
        "User-Agent: QQMusicLyric/" + toUtf8(app_info::kVersion),
    };
    if (!httpGet(kDeepSeekBalanceUrl, headers, cancelled, response)) {
        if (cancelled.load())
            return failureSnapshot(QuotaProvider::DeepSeek, L"已取消", false, 0);
        return failureSnapshot(QuotaProvider::DeepSeek,
                               L"无法更新 DeepSeek 余额，请检查网络或代理", false, 120);
    }
    if (response.statusCode == 401 || response.statusCode == 403)
        return failureSnapshot(QuotaProvider::DeepSeek,
                               L"DeepSeek 认证失败，请检查 API Key", true, 3600);
    if (response.statusCode == 429)
        return failureSnapshot(QuotaProvider::DeepSeek, L"请求过于频繁，稍后自动重试", false, 180);
    if (response.statusCode < 200 || response.statusCode >= 300)
        return failureSnapshot(QuotaProvider::DeepSeek,
                               statusErrorText(response.statusCode, L"DeepSeek 余额服务"), false,
                               120);

    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    QuotaSnapshot snapshot;
    snapshot.provider = QuotaProvider::DeepSeek;
    std::wstring error;
    if (body.is_discarded() || !quota_detail::parseDeepSeekBalance(body, snapshot, error))
        return failureSnapshot(
            QuotaProvider::DeepSeek, error.empty() ? L"DeepSeek 余额响应无效" : error, false, 120);
    snapshot.status = QuotaStatus::Ready;
    snapshot.retryAfterSeconds = 300;
    return snapshot;
}

QuotaSnapshot QuotaProviderClient::fetchCodex(const CancelFlag& cancelled) const {
    const std::wstring path = codexAuthPath();
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        QuotaSnapshot snapshot;
        snapshot.provider = QuotaProvider::Codex;
        snapshot.status = QuotaStatus::NotConfigured;
        return snapshot;
    }

    std::string content;
    if (!readFile(path, content))
        return failureSnapshot(QuotaProvider::Codex, L"无法读取 Codex 登录文件，请重新登录", true,
                               3600);
    const nlohmann::json auth = nlohmann::json::parse(content, nullptr, false);
    CodexCredential credential;
    std::wstring error;
    if (auth.is_discarded() || !quota_detail::parseCodexAuthJson(auth, credential, error))
        return failureSnapshot(QuotaProvider::Codex,
                               error.empty() ? L"Codex 登录文件无法解析" : error, true, 3600);

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (credential.expiresAt && *credential.expiresAt <= now)
        return failureSnapshot(QuotaProvider::Codex,
                               L"Codex 登录已过期，请在 Codex 中重新登录", true, 3600);

    std::vector<std::string> headers = {
        "Accept: application/json",
        "Authorization: Bearer " + toUtf8(credential.accessToken),
        "User-Agent: codex-cli/" + toUtf8(app_info::kVersion),
    };
    const std::string accountId = toUtf8(credential.accountId);
    if (!accountId.empty())
        headers.push_back("ChatGPT-Account-Id: " + accountId);

    HttpResponse response;
    if (!httpGet(kCodexUsageUrl, headers, cancelled, response)) {
        if (cancelled.load())
            return failureSnapshot(QuotaProvider::Codex, L"已取消", false, 0);
        return failureSnapshot(QuotaProvider::Codex,
                               L"无法连接 Codex 额度服务，请检查网络或代理", false, 120);
    }
    if (response.statusCode == 401 || response.statusCode == 403)
        return failureSnapshot(QuotaProvider::Codex,
                               L"Codex 登录已失效，请在 Codex 中重新登录", true, 3600);
    if (response.statusCode == 429)
        return failureSnapshot(QuotaProvider::Codex, L"请求过于频繁，稍后自动重试", false, 180);
    if (response.statusCode < 200 || response.statusCode >= 300)
        return failureSnapshot(QuotaProvider::Codex,
                               statusErrorText(response.statusCode, L"Codex 额度服务"), false, 120);

    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    QuotaSnapshot snapshot;
    snapshot.provider = QuotaProvider::Codex;
    if (body.is_discarded() || !quota_detail::parseCodexUsage(body, snapshot, error))
        return failureSnapshot(
            QuotaProvider::Codex, error.empty() ? L"Codex 额度响应无效" : error, false, 120);
    snapshot.status = QuotaStatus::Ready;
    snapshot.retryAfterSeconds = 300;
    return snapshot;
}
