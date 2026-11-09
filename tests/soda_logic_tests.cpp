#include "test_support.h"

#include "lyric/soda_lyric_timing.h"
#include "media/player_source.h"
#include "media/smtc/soda_music_timeline.h"

namespace {
SmtcSnapshot playingSnapshot(int64_t position = 10000, int64_t anchor = 100000) {
    SmtcSnapshot snapshot;
    snapshot.player = SmtcPlayerType::Soda;
    snapshot.status = PlaybackStatus::Playing;
    snapshot.sessionAlive = true;
    snapshot.positionMs = position;
    snapshot.anchorUtcMs = anchor;
    snapshot.durationMs = 180000;
    return snapshot;
}
} // namespace

TEST_CASE("soda_timeline.join_preserves_old_anchor_and_missing_anchor_uses_now") {
    smtc::SodaMusicTimeline timeline;
    auto source = playingSnapshot();
    source.timelineStale = true;
    timeline.initialize(source, 160000);
    EXPECT_EQ(source.anchorUtcMs, 100000);
    EXPECT_FALSE(source.timelineStale);
    EXPECT_EQ(timeline.snapshot(source, 160000).positionMs, 70000);

    timeline.reset();
    source = playingSnapshot(20000, 0);
    timeline.initialize(source, 160000);
    EXPECT_EQ(source.anchorUtcMs, 160000);
    EXPECT_EQ(timeline.snapshot(source, 161000).positionMs, 21000);
}

TEST_CASE("soda_timeline.pause_and_resume_do_not_reuse_pause_duration") {
    smtc::SodaMusicTimeline timeline;
    auto source = playingSnapshot();
    timeline.initialize(source, 100000);
    EXPECT_EQ(timeline.snapshot(source, 105000).positionMs, 15000);

    // 状态通知先到，时间线仍为播放起点的重复采样。
    timeline.updatePlayback(source, PlaybackStatus::Paused, 105000);
    timeline.updateSample(source, 10000, 100000, 180000, 105000);
    EXPECT_EQ(timeline.snapshot(source, 120000).positionMs, 15000);
    timeline.updatePlayback(source, PlaybackStatus::Paused, 125000);
    timeline.updateSample(source, 10000, 100000, 180000, 125000);
    EXPECT_EQ(timeline.snapshot(source, 125000).positionMs, 15000);

    timeline.updatePlayback(source, PlaybackStatus::Playing, 130000);
    timeline.updateSample(source, 10000, 100000, 180000, 130000);
    EXPECT_EQ(timeline.snapshot(source, 131000).positionMs, 16000);
}

TEST_CASE("soda_timeline.pause_seek_and_resume_use_the_new_paused_position") {
    smtc::SodaMusicTimeline timeline;
    auto source = playingSnapshot();
    timeline.initialize(source, 100000);
    timeline.updatePlayback(source, PlaybackStatus::Paused, 105000);
    timeline.updateSample(source, 15000, 105000, 180000, 105000);
    EXPECT_EQ(timeline.snapshot(source, 108000).positionMs, 15000);
    timeline.updateSample(source, 3000, 110000, 180000, 110000);
    EXPECT_EQ(timeline.snapshot(source, 115000).positionMs, 3000);
    timeline.updatePlayback(source, PlaybackStatus::Playing, 120000);
    timeline.updateSample(source, 3000, 110000, 180000, 120000);
    EXPECT_EQ(timeline.snapshot(source, 121000).positionMs, 4000);
}

TEST_CASE("soda_timeline.repeated_and_older_samples_do_not_rewind") {
    smtc::SodaMusicTimeline timeline;
    auto source = playingSnapshot();
    timeline.initialize(source, 100000);
    EXPECT_EQ(timeline.snapshot(source, 105000).positionMs, 15000);
    timeline.updateSample(source, 10000, 100000, 180000, 106000);
    EXPECT_EQ(timeline.snapshot(source, 106000).positionMs, 16000);
    timeline.updateSample(source, 5000, 99000, 180000, 107000);
    EXPECT_EQ(source.anchorUtcMs, 100000);
    EXPECT_EQ(timeline.snapshot(source, 107000).positionMs, 17000);
}

TEST_CASE("soda_timeline.small_clock_correction_stays_continuous_but_seek_can_rewind") {
    smtc::SodaMusicTimeline timeline;
    auto source = playingSnapshot();
    timeline.initialize(source, 100000);
    EXPECT_EQ(timeline.snapshot(source, 105000).positionMs, 15000);

    // 新采样只比展示进度少 100ms，不应出现视觉倒退。
    timeline.updateSample(source, 14900, 105000, 180000, 105000);
    EXPECT_EQ(timeline.snapshot(source, 105000).positionMs, 15000);
    EXPECT_EQ(timeline.snapshot(source, 105200).positionMs, 15100);

    timeline.updateSample(source, 3000, 106000, 180000, 106000);
    EXPECT_EQ(timeline.snapshot(source, 106000).positionMs, 3000);
    timeline.updateSample(source, 90000, 107000, 180000, 107000);
    EXPECT_EQ(timeline.snapshot(source, 107000).positionMs, 90000);
}

TEST_CASE("soda_timeline.duration_clamp_and_reset_release_previous_song_progress") {
    smtc::SodaMusicTimeline timeline;
    auto source = playingSnapshot(179000);
    timeline.initialize(source, 100000);
    EXPECT_EQ(timeline.snapshot(source, 105000).positionMs, 180000);
    timeline.reset();
    source = playingSnapshot(0, 106000);
    timeline.initialize(source, 106000);
    EXPECT_EQ(timeline.snapshot(source, 107000).positionMs, 1000);
    timeline.updateSample(source, -500, 108000, 180000, 108000);
    EXPECT_EQ(timeline.snapshot(source, 108000).positionMs, 0);
}

TEST_CASE("soda_lyrics.offset_only_for_matching_preview_duration") {
    using soda_lyrics::previewOffset;
    EXPECT_EQ(previewOffset(180000, 60000, 30000, 30000), 60000);
    EXPECT_EQ(previewOffset(180000, 60000, 30000, 31000), 60000);
    EXPECT_EQ(previewOffset(180000, 60000, 30000, 31001), 0);
    EXPECT_EQ(previewOffset(180000, 60000, 30000, 180000), 0);
    EXPECT_EQ(previewOffset(180000, 60000, 30000, 179000), 0);
    EXPECT_EQ(previewOffset(30000, 1000, 29000, 29000), 0); // 完整时长判定优先。
    EXPECT_EQ(previewOffset(180000, 60000, 30000, 0), 0);
    EXPECT_EQ(previewOffset(180000, 0, 30000, 30000), 0);
    EXPECT_EQ(previewOffset(180000, 180000, 30000, 30000), 0);
    EXPECT_EQ(previewOffset(180000, 170000, 30000, 30000), 0);
    EXPECT_EQ(previewOffset(0, 60000, 30000, 30000), 0);
    EXPECT_EQ(previewOffset(180000, 60000, 0, 30000), 0);
}

TEST_CASE("soda_lyrics.preview_clips_words_at_both_boundaries_and_preserves_secondary_text") {
    std::vector<LyricLine> lines{
        {8000, L"前", {{8000, 10000, L"前"}}},
        {9000, L"甲乙丙丁戊", {{9000, 10000, L"甲"}, {9500, 10500, L"乙"},
                              {10500, 11500, L"丙"}, {11500, 12500, L"丁"},
                              {12000, 13000, L"戊"}}, L"翻译", L"romanization"},
        {12000, L"后", {{12000, 13000, L"后"}}},
    };
    soda_lyrics::alignPreview(lines, 10000, 2000, 180000);
    EXPECT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].ms, 0);
    EXPECT_EQ(lines[0].text, std::wstring(L"乙丙丁"));
    EXPECT_EQ(lines[0].chars.size(), 3u);
    EXPECT_EQ(lines[0].chars[0].startMs, 0);
    EXPECT_EQ(lines[0].chars[0].endMs, 500);
    EXPECT_EQ(lines[0].chars[1].startMs, 500);
    EXPECT_EQ(lines[0].chars[1].endMs, 1500);
    EXPECT_EQ(lines[0].chars[2].startMs, 1500);
    EXPECT_EQ(lines[0].chars[2].endMs, 2000);
    EXPECT_EQ(lines[0].translation, std::wstring(L"翻译"));
    EXPECT_EQ(lines[0].romanization, std::wstring(L"romanization"));
}

TEST_CASE("soda_lyrics.lrc_preview_keeps_overlapping_lines_and_last_line") {
    std::vector<LyricLine> lines{
        {8000, L"片段前"}, {9000, L"跨起点"}, {11000, L"片段内"}, {12000, L"片段后"},
    };
    soda_lyrics::alignPreview(lines, 10000, 2000, 180000);
    EXPECT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].ms, 0);
    EXPECT_EQ(lines[0].text, std::wstring(L"跨起点"));
    EXPECT_EQ(lines[1].ms, 1000);
    EXPECT_TRUE(lines[0].chars.empty());

    lines = {{9000, L"末行"}};
    soda_lyrics::alignPreview(lines, 10000, 2000, 180000);
    EXPECT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].ms, 0);
}

TEST_CASE("soda_lyrics.no_offset_preserves_full_song_and_outside_preview_is_empty") {
    std::vector<LyricLine> lines{{1000, L"原文", {{1000, 2000, L"原文"}}, L"翻译"}};
    soda_lyrics::alignPreview(lines, 0, 180000, 180000);
    EXPECT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].ms, 1000);
    EXPECT_EQ(lines[0].chars[0].startMs, 1000);
    EXPECT_EQ(lines[0].chars[0].endMs, 2000);
    EXPECT_EQ(lines[0].translation, std::wstring(L"翻译"));
    soda_lyrics::alignPreview(lines, 10000, 2000, 180000);
    EXPECT_TRUE(lines.empty());
    soda_lyrics::alignPreview(lines, 10000, 2000, 180000);
    EXPECT_TRUE(lines.empty());
}

TEST_CASE("player_source.session_aliases_process_mapping_and_unknown_source") {
    struct SourceCase {
        const wchar_t* session;
        SmtcPlayerType player;
        const wchar_t* process;
    };
    const SourceCase cases[]{
        {L"QQMusic.exe", SmtcPlayerType::QQMusic, L"QQMusic.exe"},
        {L"Tencent.QQMusic", SmtcPlayerType::QQMusic, L"QQMusic.exe"},
        {L"cloudmusic.exe", SmtcPlayerType::NetEase, L"cloudmusic.exe"},
        {L"NETEASEBRIDGE.EXE", SmtcPlayerType::NetEase, L"cloudmusic.exe"},
        {L"汽水音乐", SmtcPlayerType::Soda, L"SodaMusic.exe"},
    };
    for (const auto& item : cases) {
        const auto* source = player_source::find(std::wstring(item.session));
        EXPECT_TRUE(source != nullptr);
        EXPECT_EQ(source->player, item.player);
        EXPECT_EQ(source, player_source::find(item.player));
        EXPECT_FALSE(source->processNames.empty());
        EXPECT_EQ(std::wstring(source->processNames.front()), std::wstring(item.process));
    }
    const auto* netease = player_source::find(SmtcPlayerType::NetEase);
    EXPECT_TRUE(netease != nullptr);
    EXPECT_EQ(netease->processNames.size(), 2u);
    EXPECT_EQ(std::wstring(netease->processNames[1]), std::wstring(L"NeteaseBridge.exe"));
    EXPECT_TRUE(player_source::find(std::wstring(L"UnknownPlayer.exe")) == nullptr);
    EXPECT_TRUE(player_source::find(std::wstring()) == nullptr);
    EXPECT_TRUE(player_source::find(SmtcPlayerType::Unknown) == nullptr);
}
