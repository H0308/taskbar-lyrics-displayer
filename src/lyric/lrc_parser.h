#pragma once

#include "lyric_types.h"

#include <cstdint>
#include <string>
#include <vector>

struct ParsedLrcDocument {
    std::vector<LyricLine> lines;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    // 已应用到时间轴上的位移（与软件时间调整约定一致：负值表示提前，正值表示延后）。
    int64_t appliedOffsetMs = 0;
    bool hasOffset = false;
};

// 解析 UTF-8 LRC 文本；支持一行多个时间戳，并忽略 [ti:]/[ar:] 等元数据行。
std::vector<LyricLine> parseLrcText(const std::string& content);

// 解析单个 LRC 时间戳，供 YRC 的普通 LRC 回退行复用。
bool parseLrcTimestamp(const std::string& text, int64_t& milliseconds);

// 将可能为 UTF-8/UTF-16/GB18030/ANSI 的本地字节流解码为 UTF-8 文本。
std::string decodeLocalLyricBytesToUtf8(const std::string& rawBytes);

// 解析本地或内嵌 LRC：支持自动编码探测、[ti:]/[ar:]/[al:]/[offset:]、
// 增强型词级时间轴（<mm:ss.xx>）、冒号毫秒与同时间戳双语行合并。
ParsedLrcDocument parseLocalLrcBytes(const std::string& rawBytes);
ParsedLrcDocument parseLocalLrcUtf8(const std::string& utf8Content);
