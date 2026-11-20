#pragma once

#include "lyric/lyric_types.h"

#include <cstdint>
#include <vector>

namespace lyric_timing {

// 为缺少逐字时间轴（chars 为空）的歌词行按估算时长和词元权重生成模拟逐字时间轴；
// 已有原生逐字时间轴的行保持原样。返回新副本，不修改原始歌词数据。
std::vector<LyricLine> buildSimulatedKaraokeLines(const std::vector<LyricLine>& lines,
                                                  int64_t trackDurationMs);

// 为指定单行合成模拟逐字词元；若行文本为空或全为空白返回空数组。
std::vector<LyricChar> synthesizeSimulatedKaraokeChars(const std::vector<LyricLine>& lines,
                                                       size_t lineIndex,
                                                       int64_t trackDurationMs);

} // namespace lyric_timing
