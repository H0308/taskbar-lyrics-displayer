#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// AI 服务额度：目前覆盖 Codex 订阅额度与 DeepSeek 账户余额。
// 这里只表达解析后的展示数据，网络请求、凭据与刷新节奏由 provider/controller 负责。
enum class QuotaProvider {
    Codex,
    DeepSeek,
};

enum class QuotaStatus {
    NotConfigured, // 未检测到登录文件或 API Key，不显示也不请求
    Loading,
    Ready,
    Error,
};

// 一个订阅额度周期（如 Codex 的 5 小时窗口、每周窗口）。
struct QuotaWindow {
    std::wstring label;        // 任务栏使用的短标签："5h"、"周"
    int periodSeconds = 0;     // 周期长度，用于排序与说明文案
    int remainingPercent = 0;  // 剩余百分比 0~100
    std::optional<int64_t> resetAt; // Unix 秒；未知时为空
};

struct QuotaBalance {
    std::wstring currency; // "CNY" / "USD"
    double total = 0.0;
    double granted = 0.0;
    double toppedUp = 0.0;
};

// 单个服务的最近一次查询结果；跨线程按值复制，不共享引用。
struct QuotaSnapshot {
    QuotaProvider provider = QuotaProvider::Codex;
    QuotaStatus status = QuotaStatus::NotConfigured;
    std::wstring plan;
    std::vector<QuotaWindow> windows;
    bool hasBalance = false;
    QuotaBalance balance;
    std::wstring error;
    bool authRequired = false;
    int retryAfterSeconds = 300; // 失败后的自动重试间隔；认证失败不自动重试
};

// 任务栏状态区中的一行额度文案，由应用层从快照推导。
struct QuotaReading {
    std::wstring label;
    std::wstring value;

    bool operator==(const QuotaReading&) const = default;
};
