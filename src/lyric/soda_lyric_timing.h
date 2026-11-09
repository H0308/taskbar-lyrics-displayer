#pragma once

#include "lyric/lyric_types.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace soda_lyrics {

// SMTC 对试听片段从零计时，汽水歌词仍使用完整歌曲时间轴。
// 只有播放时长与接口明确提供的试听长度相符时，才采用试听起点。
inline int64_t previewOffset(int64_t fullDurationMs, int64_t previewStartMs,
                            int64_t previewDurationMs, int64_t playbackDurationMs) {
    constexpr int64_t kDurationToleranceMs = 1000;
    if (playbackDurationMs <= 0 || previewStartMs <= 0 || previewDurationMs <= 0 ||
        fullDurationMs <= 0 || previewStartMs >= fullDurationMs ||
        previewDurationMs > fullDurationMs - previewStartMs + kDurationToleranceMs)
        return 0;
    if (std::llabs(fullDurationMs - playbackDurationMs) <= kDurationToleranceMs)
        return 0;
    return std::llabs(previewDurationMs - playbackDurationMs) <= kDurationToleranceMs
               ? previewStartMs : 0;
}

inline void alignPreview(std::vector<LyricLine>& lines, int64_t offsetMs,
                        int64_t playbackDurationMs, int64_t fullDurationMs) {
    if (offsetMs <= 0)
        return;
    std::vector<LyricLine> aligned;
    const int64_t endMs = offsetMs + playbackDurationMs;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto& line = lines[i];
        const int64_t lineEndMs = line.chars.empty()
                                     ? (i + 1 < lines.size() ? lines[i + 1].ms : fullDurationMs)
                                     : line.chars.back().endMs;
        if (lineEndMs <= offsetMs || line.ms >= endMs)
            continue;
        if (!line.chars.empty()) {
            std::erase_if(line.chars, [offsetMs, endMs](const LyricChar& word) {
                return word.endMs <= offsetMs || word.startMs >= endMs;
            });
            if (line.chars.empty())
                continue;
            line.text.clear();
            for (auto& word : line.chars) {
                word.startMs = std::max<int64_t>(0, word.startMs - offsetMs);
                word.endMs = std::min(playbackDurationMs, word.endMs - offsetMs);
                line.text += word.text;
            }
        }
        line.ms = std::max<int64_t>(0, line.ms - offsetMs);
        aligned.push_back(std::move(line));
    }
    lines = std::move(aligned);
}

} // namespace soda_lyrics
