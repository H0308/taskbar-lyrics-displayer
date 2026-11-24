#include "test_support.h"

#include "lyric/custom_local_lyric_index.h"
#include "lyric/local_lyric_index.h"
#include "lyric/lyric_matching.h"
#include "lyric/sources/kugou_lyric_source.h"
#include "lyric/sources/netease_lyric_source.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

struct TempDirGuard {
    std::filesystem::path path;
    explicit TempDirGuard(const wchar_t* prefix) {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               (std::wstring(prefix) + std::to_wstring(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("lyric_matching.normalization_version_tags_and_scoring") {
    EXPECT_EQ(lyric_detail::normalizeTitle(L"晴天 (Album) feat. 某人"),
              std::wstring(L"晴天"));
    EXPECT_EQ(lyric_detail::normalizeTitle(L"晴天 (Album Version) feat. 某人"),
              std::wstring(L"晴天 album version"));
    const auto liveTags = lyric_detail::extractVersionTags(L"稻香 (Live)");
    EXPECT_EQ(liveTags.size(), 1u);
    EXPECT_EQ(liveTags[0], std::wstring(L"live"));

    const lyric_detail::MatchQuery q{
        L"稻香", L"周杰伦", 223000, lyric_detail::extractVersionTags(L"稻香")};
    const int studioScore = lyric_detail::scoreCandidate(q, L"稻香", L"周杰伦", 223500);
    const int liveScore = lyric_detail::scoreCandidate(q, L"稻香 (Live)", L"周杰伦", 223500);
    const int wrongArtist = lyric_detail::scoreCandidate(q, L"稻香", L"其他歌手", 223500);
    EXPECT_TRUE(studioScore >= lyric_detail::kScoreThreshold);
    EXPECT_EQ(liveScore, 0);
    EXPECT_EQ(wrongArtist, 0);
    EXPECT_TRUE(lyric_detail::titleEndsWithArtist(L"晴天 - 周杰伦", L"周杰伦"));
}

TEST_CASE("lyric_matching.artist_parenthetical_aliases_and_collaborators") {
    EXPECT_EQ(lyric_detail::singerSimilarity(L"陆绮菲（菲菲公主）", L"菲菲公主（陆绮菲）"), 100);
    EXPECT_EQ(lyric_detail::singerSimilarity(L"邓寓君(等什么君)", L"等什么君（邓寓君）"), 100);
    EXPECT_EQ(lyric_detail::singerSimilarity(L"陸綺菲（菲菲公主）", L"菲菲公主(陆绮菲)"), 100);

    const lyric_detail::MatchQuery q{
        L"第57次取消发送", L"陆绮菲（菲菲公主）", 180560, {}};
    EXPECT_TRUE(lyric_detail::scoreCandidate(q, L"第57次取消发送", L"菲菲公主（陆绮菲）", 180000) >=
                lyric_detail::kScoreThreshold);

    EXPECT_EQ(lyric_detail::singerSimilarity(L"仓木麻衣 (くらき まい)", L"仓木麻衣 (Mai Kuraki)"), 95);
    const lyric_detail::MatchQuery kurakiQuery{
        L"渡月橋 ～君 想ふ～", L"仓木麻衣 (くらき まい)", 247893, {}};
    EXPECT_TRUE(lyric_detail::scoreCandidate(
                    kurakiQuery, L"渡月橋 ～君 想ふ～", L"仓木麻衣 (Mai Kuraki)", 247000) >=
                lyric_detail::kScoreThreshold);

    EXPECT_EQ(lyric_detail::singerSimilarity(L"周深 (Charlie Zhou, 卡布叻)", L"卡布叻 (周深)"), 95);

    EXPECT_TRUE(
        lyric_detail::singerSimilarity(L"陆绮菲（菲菲公主）/DJ阿智", L"DJ阿智/菲菲公主") >= 90);
    EXPECT_EQ(lyric_detail::singerSimilarity(
                  L"周杰伦 (Jay Chou) feat. 费玉清", L"Jay Chou / 费玉清 (Fei Yu-Ching)"),
              95);
    EXPECT_EQ(lyric_detail::singerSimilarity(L"周杰伦 (feat. 费玉清)", L"费玉清/周杰伦"), 100);

    const int singleVsCollab =
        lyric_detail::singerSimilarity(L"陆绮菲（菲菲公主）", L"菲菲公主（陆绮菲）/DJ阿智");
    EXPECT_TRUE(singleVsCollab < 100);
    EXPECT_TRUE(singleVsCollab < 80);

    EXPECT_EQ(lyric_detail::singerSimilarity(L"张三（翻唱）", L"李四（翻唱）"), 0);
    EXPECT_EQ(lyric_detail::singerSimilarity(L"张三 (Live)", L"李四 (Live)"), 0);
    EXPECT_EQ(lyric_detail::singerSimilarity(L"张三 (原唱)", L"李四 (原唱)"), 0);
    EXPECT_EQ(lyric_detail::singerSimilarity(L"张三 (CV)", L"李四 (CV)"), 0);
    EXPECT_EQ(lyric_detail::singerSimilarity(L"张三 (合唱)", L"李四 (合唱)"), 0);
    const lyric_detail::MatchQuery coverQuery{L"晴天", L"张三（翻唱）", 220000, {}};
    EXPECT_EQ(lyric_detail::scoreCandidate(coverQuery, L"晴天", L"李四（翻唱）", 220000), 0);
}

TEST_CASE("lyric_sources.yrc_and_krc_parsing") {
    const std::string yrc =
        "\xEF\xBB\xBF"
        "[1000,2000](1000,500,0)你(1500,500,0)好\n"
        "[00:04.50]混入普通行\n";
    const auto yrcLines = lyric_detail::parseYrc(yrc);
    EXPECT_EQ(yrcLines.size(), 2u);
    EXPECT_EQ(yrcLines[0].ms, 1000);
    EXPECT_EQ(yrcLines[0].text, std::wstring(L"你好"));
    EXPECT_EQ(yrcLines[0].chars.size(), 2u);
    EXPECT_EQ(yrcLines[0].chars[1].startMs, 1500);
    EXPECT_EQ(yrcLines[0].chars[1].endMs, 2000);
    EXPECT_EQ(yrcLines[1].ms, 4500);
    EXPECT_EQ(yrcLines[1].text, std::wstring(L"混入普通行"));

    // {"content":[{"type":1,"lyricContent":[["世界"]]},{"type":0,"lyricContent":[["shi","jie"]]}]}
    const std::string langB64 =
        "eyJjb250ZW50IjpbeyJ0eXBlIjoxLCJseXJpY0NvbnRlbnQiOltbIuS4lueVjCJdXX0seyJ0eXBlIjowLCJseXJp"
        "Y0NvbnRlbnQiOltbInNoaSIsImppZSJdXX1dfQ==";
    const std::string krc =
        "[ti:demo]\r\n"
        "[language:" + langB64 + "]\r\n"
        "[1000,1500]<0,500,0>世<500,500,0>界&amp;\r\n";
    std::vector<LyricLine> krcLines;
    EXPECT_TRUE(lyric_detail::parseKrc(krc, krcLines));
    EXPECT_EQ(krcLines.size(), 1u);
    EXPECT_EQ(krcLines[0].ms, 1000);
    EXPECT_EQ(krcLines[0].text, std::wstring(L"世界&"));
    EXPECT_EQ(krcLines[0].chars.size(), 2u);
    EXPECT_EQ(krcLines[0].translation, std::wstring(L"世界"));
    EXPECT_EQ(krcLines[0].romanization, std::wstring(L"shi jie"));
}

TEST_CASE("local_lyric_index.filename_parsing_indexing_and_order_override") {
    std::wstring base;
    lyric_detail::LocalLyricVariant variant = lyric_detail::LocalLyricVariant::Original;
    EXPECT_TRUE(lyric_detail::parseLocalQrcPath(
        L"周杰伦 - 七里香 - 1024 - 七里香_qm.qrc", base, variant));
    EXPECT_EQ(variant, lyric_detail::LocalLyricVariant::Original);
    EXPECT_EQ(base, std::wstring(L"周杰伦 - 七里香 - 1024 - 七里香"));

    EXPECT_TRUE(lyric_detail::parseLocalQrcPath(
        L"周杰伦 - 七里香 - 1024_qmts.qrc", base, variant));
    EXPECT_EQ(variant, lyric_detail::LocalLyricVariant::Translation);

    lyric_detail::LocalLyricMetadata meta;
    EXPECT_TRUE(lyric_detail::parseLocalLyricBaseName(L"周杰伦 - 七里香 - 1024 - 七里香", meta));
    EXPECT_EQ(meta.artist, std::wstring(L"周杰伦"));
    EXPECT_EQ(meta.titles.size(), 1u);
    EXPECT_EQ(meta.titles[0], std::wstring(L"七里香"));

    TempDirGuard root(L"qqmusic_lyric_local_idx_");
    TempDirGuard orderDir(L"qqmusic_lyric_order_");
    {
        std::ofstream f(root.path / L"周杰伦 - 七里香 - 1024 - 七里香_qm.qrc",
                        std::ios::binary);
        f << "[00:01.00]窗外的麻雀\n";
    }
    const auto index =
        lyric_detail::buildLocalLyricIndex(root.path.wstring(), orderDir.path.wstring());
    EXPECT_EQ(index->bundles.size(), 1u);
    const auto matches = lyric_detail::findLocalLyricCandidates(*index, L"七里香", L"周杰伦");
    EXPECT_FALSE(matches.empty());
    EXPECT_EQ(matches.front(), 0u);
    const auto subsetMatches =
        lyric_detail::findLocalLyricCandidates(*index, L"七里香", L"周杰伦 / 费玉清");
    EXPECT_EQ(subsetMatches.size(), 1u);

    const auto& bundle = index->bundles[matches.front()];
    EXPECT_FALSE(lyric_detail::readLocalOrderOverride(bundle));
    EXPECT_TRUE(lyric_detail::writeLocalOrderOverride(bundle, true));
    EXPECT_TRUE(lyric_detail::readLocalOrderOverride(bundle));
    EXPECT_TRUE(lyric_detail::writeLocalOrderOverride(bundle, false));
    EXPECT_FALSE(lyric_detail::readLocalOrderOverride(bundle));
}

TEST_CASE("custom_local_lyric_index.single_level_matching_embedded_audio_and_order") {
    TempDirGuard root(L"qqmusic_lyric_custom_idx_");
    TempDirGuard orderDir(L"qqmusic_lyric_custom_order_");

    // 顶层 .lrc 文件（普通版、AI 翻唱版、含非法文件名替换字符）
    {
        std::ofstream f(root.path / L"周杰伦 - 晴天.lrc", std::ios::binary);
        f << "[00:01.00]原版歌词\n";
    }
    {
        std::ofstream f(root.path / L"晴天 (AI翻唱).lrc", std::ios::binary);
        f << "[00:02.00]AI翻唱歌词\n";
    }
    {
        std::ofstream f(root.path / L"Re_Zero _ Start.lrc", std::ios::binary);
        f << "[00:03.00]特殊字符歌名\n";
    }
    // 深层子目录文件不应被索引（设计约束：单目录不递归）
    std::filesystem::create_directories(root.path / L"sub");
    {
        std::ofstream f(root.path / L"sub" / L"子目录歌曲.lrc", std::ios::binary);
        f << "[00:01.00]不应被扫描\n";
    }

    // 构造一个最小合法 ID3v2.4 + USLT（含 LRC 时间轴）+ 单帧 MPEG 的 MP3，验证音频内嵌提取。
    auto writeMinimalMp3WithUslt = [](const std::filesystem::path& path,
                                      const std::string& lyricUtf8) {
        auto appendSyncSafe = [](std::string& dst, uint32_t value) {
            dst.push_back(static_cast<char>((value >> 21) & 0x7F));
            dst.push_back(static_cast<char>((value >> 14) & 0x7F));
            dst.push_back(static_cast<char>((value >> 7) & 0x7F));
            dst.push_back(static_cast<char>(value & 0x7F));
        };
        std::string payload;
        payload.push_back(static_cast<char>(0x03)); // UTF-8
        payload += "eng";
        payload.push_back('\0'); // 空描述
        payload += lyricUtf8;

        std::string frame = "USLT";
        appendSyncSafe(frame, static_cast<uint32_t>(payload.size()));
        frame.push_back('\0');
        frame.push_back('\0');
        frame += payload;

        std::string fileBytes = "ID3";
        fileBytes.push_back(static_cast<char>(0x04)); // v2.4
        fileBytes.push_back('\0');
        fileBytes.push_back('\0');
        appendSyncSafe(fileBytes, static_cast<uint32_t>(frame.size()));
        fileBytes += frame;

        // 写入 2 个连续的最小 MPEG-1 Layer III 128kbps 44.1kHz 音频帧（每帧 417 字节），
        // 供 TagLib::MPEG::Properties 通过双帧头校验并安静解析音频时长。
        for (int frameIdx = 0; frameIdx < 2; ++frameIdx) {
            fileBytes.push_back(static_cast<char>(0xFF));
            fileBytes.push_back(static_cast<char>(0xFB));
            fileBytes.push_back(static_cast<char>(0x90));
            fileBytes.push_back(static_cast<char>(0x00));
            fileBytes.append(413, '\0');
        }

        std::ofstream out(path, std::ios::binary);
        out.write(fileBytes.data(), static_cast<std::streamsize>(fileBytes.size()));
    };
    writeMinimalMp3WithUslt(root.path / L"测试歌手 - 内嵌歌曲.mp3", "[00:04.50]内嵌同步歌词\n");
    writeMinimalMp3WithUslt(root.path / L"测试歌手 - 静态歌词.mp3", "这是一段没有时间轴的纯文本\n");

    const auto index = lyric_detail::buildCustomLocalLyricIndex(root.path.wstring());
    EXPECT_EQ(index->entries.size(), 5u);

    std::vector<LyricLine> lines;
    SongInfo info;
    // 子目录文件不命中
    EXPECT_FALSE(lyric_detail::lookupCustomLocalLyric(*index, L"子目录歌曲", L"", 0, lines, info));

    // 原版与 AI 翻唱互不串词
    EXPECT_TRUE(lyric_detail::lookupCustomLocalLyric(*index, L"晴天", L"周杰伦", 0, lines, info));
    EXPECT_EQ(info.lyricOrigin, LyricOrigin::LocalLrc);
    EXPECT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].text, std::wstring(L"原版歌词"));

    EXPECT_TRUE(
        lyric_detail::lookupCustomLocalLyric(*index, L"晴天 (AI翻唱)", L"孙燕姿", 0, lines, info));
    EXPECT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].text, std::wstring(L"AI翻唱歌词"));

    // Windows 非法文件名字符等价归一化
    EXPECT_TRUE(
        lyric_detail::lookupCustomLocalLyric(*index, L"Re:Zero / Start", L"", 0, lines, info));
    EXPECT_EQ(lines[0].text, std::wstring(L"特殊字符歌名"));

    // 音频内嵌同步歌词可正常读取，纯静态文本歌词被过滤
    EXPECT_TRUE(
        lyric_detail::lookupCustomLocalLyric(*index, L"内嵌歌曲", L"测试歌手", 0, lines, info));
    EXPECT_EQ(info.lyricOrigin, LyricOrigin::LocalAudio);
    EXPECT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].ms, 4500);
    EXPECT_EQ(lines[0].text, std::wstring(L"内嵌同步歌词"));

    EXPECT_FALSE(
        lyric_detail::lookupCustomLocalLyric(*index, L"静态歌词", L"测试歌手", 0, lines, info));

    // 手动搜索候选包含本地 LRC 与本地音频
    const auto searchHits = lyric_detail::searchCustomLocalCandidates(*index, L"晴天", L"周杰伦");
    EXPECT_FALSE(searchHits.empty());
    EXPECT_EQ(searchHits.front().source, LyricSource::LocalLrc);

    // 跨播放器单曲版本顺序记忆读写
    const auto orderPath = lyric_detail::customLocalOrderPath(
        orderDir.path.wstring(), L"generic|晴天|周杰伦|40|", L"晴天", L"周杰伦",
        SmtcPlayerType::Generic);
    EXPECT_FALSE(lyric_detail::readCustomLocalOrderOverride(orderPath));
    EXPECT_TRUE(lyric_detail::writeCustomLocalOrderOverride(orderPath, true));
    EXPECT_TRUE(lyric_detail::readCustomLocalOrderOverride(orderPath));
    EXPECT_TRUE(lyric_detail::writeCustomLocalOrderOverride(orderPath, false));
    EXPECT_FALSE(lyric_detail::readCustomLocalOrderOverride(orderPath));

    // 指定播放器范围匹配：内置播放器（含商店版/桌面版）、本地 EXE 与通用 SMTC 应用
    std::vector<GenericSmtcAppRule> playerRules;
    playerRules.push_back(lyric_detail::makeBuiltinPlayerRule(SmtcPlayerType::SaltPlayer));
    GenericSmtcAppRule exeRule;
    exeRule.processName = L"foobar2000.exe";
    exeRule.displayName = L"foobar2000";
    playerRules.push_back(exeRule);
    GenericSmtcAppRule uwpRule;
    uwpRule.sourceId = L"Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic";
    uwpRule.packageFamilyName = L"Microsoft.ZuneMusic_8wekyb3d8bbwe";
    uwpRule.displayName = L"媒体播放器";
    playerRules.push_back(uwpRule);

    EXPECT_TRUE(lyric_detail::matchesCustomLocalPlayerRules(
        SmtcPlayerType::SaltPlayer, L"Sakawish.SaltPlayerforWindows_abc123!SaltPlayerforWindows",
        {}, playerRules));
    EXPECT_FALSE(lyric_detail::matchesCustomLocalPlayerRules(
        SmtcPlayerType::QQMusic, L"QQMusic.exe", {}, playerRules));

    AudioSessionTarget foobarTarget;
    foobarTarget.processNames = {L"foobar2000.exe"};
    EXPECT_TRUE(lyric_detail::matchesCustomLocalPlayerRules(
        SmtcPlayerType::Generic, L"foobar2000.exe", foobarTarget, playerRules));

    AudioSessionTarget uwpTarget;
    uwpTarget.packageFamilyName = L"Microsoft.ZuneMusic_8wekyb3d8bbwe";
    EXPECT_TRUE(lyric_detail::matchesCustomLocalPlayerRules(
        SmtcPlayerType::Generic, L"Microsoft.ZuneMusic_8wekyb3d8bbwe!App", uwpTarget, playerRules));

    LyricProvider provider;
    provider.setCustomLocalLyricsConfig(true, root.path.wstring());
    EXPECT_TRUE(provider.isCustomLocalEnabledFor(SmtcPlayerType::QQMusic, L"QQMusic.exe"));
    provider.setCustomLocalLyricsScope(CustomLocalLyricsScope::SelectedPlayers, {});
    EXPECT_FALSE(provider.isCustomLocalEnabledFor(SmtcPlayerType::QQMusic, L"QQMusic.exe"));
    provider.setCustomLocalLyricsScope(CustomLocalLyricsScope::SelectedPlayers, playerRules);
    EXPECT_TRUE(provider.isCustomLocalEnabledFor(SmtcPlayerType::SaltPlayer,
                                                 L"Salt Player for Windows.exe"));
    EXPECT_FALSE(provider.isCustomLocalEnabledFor(SmtcPlayerType::QQMusic, L"QQMusic.exe"));
}
