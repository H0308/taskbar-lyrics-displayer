#include "test_support.h"

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
