#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class GenericSmtcFilterMode {
    All = 0,       // 识别全部白名单外 SMTC 应用
    AllowList = 1, // 仅识别白名单内的通用 SMTC 应用
    BlockList = 2, // 排除黑名单内的通用 SMTC 应用
};

inline constexpr size_t kMaxGenericSmtcRules = 32;

// 单条通用 SMTC 过滤规则（可来自自动发现的 SMTC 会话，或用户选择的本地 EXE）。
struct GenericSmtcAppRule {
    std::wstring sourceId;          // 原始 SMTC SourceAppUserModelId（选本地 EXE 时可为空）
    std::wstring processName;       // 可执行文件名（如 msedge.exe），用于跨 AUMID 匹配
    std::wstring packageFamilyName; // UWP/MSIX PackageFamilyName（可选）
    std::wstring displayName;       // 持久化展示名，应用未运行时仍保持可读

    // 以下字段仅用于设置界面图标绘制，不写入配置文件。
    std::shared_ptr<const std::vector<uint8_t>> iconPixels;
    unsigned int iconWidth = 0;
    unsigned int iconHeight = 0;
};

struct GenericSmtcFilterConfig {
    bool enabled = false;
    GenericSmtcFilterMode mode = GenericSmtcFilterMode::All;
    std::vector<GenericSmtcAppRule> allowList;
    std::vector<GenericSmtcAppRule> blockList;
};
