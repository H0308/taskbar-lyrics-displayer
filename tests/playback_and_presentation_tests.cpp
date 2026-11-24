#include "test_support.h"

#include "app/presentation_model.h"
#include "graphics/pixel_luminance.h"
#include "media/playback_controller.h"
#include "media/player_source.h"
#include "media/track_identity.h"
#include "settings/app_settings.h"
#include "util/base64.h"

namespace {
class MessageOnlyTimerWindow {
public:
    MessageOnlyTimerWindow() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"QQMusicLyricTestTimerWindow";
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                wc.hInstance, nullptr);
    }
    ~MessageOnlyTimerWindow() {
        if (hwnd_)
            DestroyWindow(hwnd_);
    }
    HWND hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
};
} // namespace

TEST_CASE("presentation.qq_and_netease_show_track_info_until_first_lyric") {
    for (const auto player : {SmtcPlayerType::QQMusic, SmtcPlayerType::NetEase}) {
        PresentationModel model;
        AppSettings settings;
        PlaybackState playback;
        IdleFrameInput idle;
        SmtcSnapshot snap;
        snap.player = player;
        snap.sessionAlive = true;
        snap.status = PlaybackStatus::Playing;
        snap.title = L"测试歌曲";
        snap.artist = L"测试歌手";
        playback.lyricLoading_ = true;
        EXPECT_EQ(model.update(snap, settings, playback, idle, {}).statusText,
                  std::wstring(L"测试歌曲 - 测试歌手"));

        playback.lyricLoading_ = false;
        playback.currentLyrics_ = {{1000, L"第一句"}, {3000, L"第二句"}};
        snap.positionMs = 950;
        const auto& frame = model.update(snap, settings, playback, idle, {.lyricsChanged = true});
        EXPECT_EQ(frame.currentLine, -1);
        EXPECT_EQ(frame.statusText, std::wstring(L"测试歌曲 - 测试歌手"));
        snap.positionMs = 1000;
        EXPECT_EQ(model.updatePlayback(snap).currentLine, 0);
    }
}

TEST_CASE("track_identity.qq_stale_timeline_and_netease_song_id") {
    SmtcSnapshot qqSnap;
    qqSnap.sessionAlive = true;
    qqSnap.player = SmtcPlayerType::QQMusic;
    qqSnap.title = L"月牙湾";
    qqSnap.artist = L"F.I.R.";
    qqSnap.durationMs = 232012;
    qqSnap.timelineStale = true; // 切歌残留时间线不参与曲目身份计算

    const std::wstring staleKey = makeTrackKey(qqSnap);
    EXPECT_EQ(staleKey, std::wstring(L"qq|月牙湾|F.I.R.|-1"));

    qqSnap.timelineStale = false;
    qqSnap.durationMs = 305991;
    const std::wstring validKey = makeTrackKey(qqSnap);
    EXPECT_EQ(validKey, std::wstring(L"qq|月牙湾|F.I.R.|61"));

    // SMTC Changing/Opened 过渡期标题暂时为空时保留当前会话匹配
    SmtcSnapshot transitionSnap = qqSnap;
    transitionSnap.title.clear();
    EXPECT_TRUE(snapshotMatchesTrackKey(transitionSnap, validKey));

    SmtcSnapshot neteaseSnap;
    neteaseSnap.sessionAlive = true;
    neteaseSnap.player = SmtcPlayerType::NetEase;
    neteaseSnap.neteaseSongId = L"1974443814";
    neteaseSnap.title = L"demo";
    EXPECT_EQ(makeTrackKey(neteaseSnap), std::wstring(L"netease|1974443814"));
}

TEST_CASE("track_identity.soda_and_qq_same_metadata_remain_isolated") {
    SmtcSnapshot qq;
    qq.sessionAlive = true;
    qq.player = SmtcPlayerType::QQMusic;
    qq.title = L"同名歌曲";
    qq.artist = L"同一歌手";
    qq.durationMs = 180000;
    SmtcSnapshot soda = qq;
    soda.player = SmtcPlayerType::Soda;
    const auto qqKey = makeTrackKey(qq);
    const auto sodaKey = makeTrackKey(soda);
    EXPECT_EQ(sodaKey, std::wstring(L"soda|同名歌曲|同一歌手|36"));
    EXPECT_NE(qqKey, sodaKey);
    EXPECT_TRUE(snapshotMatchesTrackKey(soda, sodaKey));
    EXPECT_FALSE(snapshotMatchesTrackKey(soda, qqKey));
    EXPECT_FALSE(snapshotMatchesTrackKey(qq, sodaKey));
    soda.sessionAlive = false;
    EXPECT_FALSE(snapshotMatchesTrackKey(soda, sodaKey));
}

TEST_CASE("playback_controller.platform_switch_rejects_inflight_qq_results_before_state_update") {
    MessageOnlyTimerWindow timerWindow;
    EXPECT_TRUE(timerWindow.hwnd() != nullptr);
    SmtcSnapshot current;
    current.sessionAlive = true;
    current.player = SmtcPlayerType::QQMusic;
    current.sourceAppUserModelId = L"QQMusic.exe";
    current.title = L"同名歌曲";
    current.artist = L"同一歌手";
    current.durationMs = 180000;
    current.thumbnail = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1});
    int publishes = 0;
    int covers = 0;
    PlaybackController controller(
        [&] { return current; },
        [&](const SmtcSnapshot&, PresentationUpdate) { ++publishes; },
        [](const SmtcSnapshot&) {}, [&] { ++covers; });
    controller.setTimerWindow(timerWindow.hwnd());
    controller.updateTrack(current);
    controller.cancelLyricDebounce(); // 不触发网络请求，仅验证已有在途结果的接收规则。
    controller.releaseCurrentCover(); // 不让 SMTC 封面优先规则掩盖跨平台结果校验。
    current.thumbnail.reset();
    const auto key = controller.state().currentKey;
    const auto generation = controller.state().requestGeneration_;
    const auto previousCover = controller.state().lastCover_;
    const int beforePublishes = publishes;

    // 会话已切换，但主线程尚未更新曲目状态；标题、歌手和时长完全相同。
    current.player = SmtcPlayerType::Soda;
    current.sourceAppUserModelId = L"汽水音乐";
    auto lyric = std::make_unique<LyricPayload>();
    lyric->key = key;
    lyric->requestGeneration = generation;
    lyric->result.ok = true;
    lyric->result.lines = {{1000, L"旧播放器歌词"}};
    controller.onLyricReady(std::move(lyric));
    EXPECT_TRUE(controller.state().currentLyrics_.empty());
    EXPECT_EQ(publishes, beforePublishes);

    auto cover = std::make_unique<CoverPayload>();
    cover->key = key;
    cover->requestGeneration = generation;
    cover->cover = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{2});
    controller.onCoverReady(std::move(cover));
    EXPECT_EQ(controller.state().lastCover_, previousCover);
    EXPECT_EQ(covers, 0);
    EXPECT_EQ(publishes, beforePublishes);

    // 对照：同一代次、同一播放器的有效封面和歌词仍正常接收。
    current.player = SmtcPlayerType::QQMusic;
    current.sourceAppUserModelId = L"QQMusic.exe";
    cover = std::make_unique<CoverPayload>();
    cover->key = key;
    cover->requestGeneration = generation;
    cover->source = CoverSource::QqApi;
    const auto validCover =
        std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{3});
    cover->cover = validCover;
    current.thumbnail = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{4});
    auto blockedCover = std::make_unique<CoverPayload>(*cover);
    controller.onCoverReady(std::move(blockedCover));
    EXPECT_EQ(controller.state().lastCover_, previousCover);
    EXPECT_EQ(covers, 0);
    EXPECT_EQ(publishes, beforePublishes);
    current.thumbnail.reset();
    controller.onCoverReady(std::move(cover));
    EXPECT_EQ(controller.state().lastCover_, validCover);
    EXPECT_EQ(covers, 1);
    EXPECT_EQ(publishes, beforePublishes + 1);
    lyric = std::make_unique<LyricPayload>();
    lyric->key = key;
    lyric->requestGeneration = generation;
    lyric->result.ok = true;
    lyric->result.lines = {{1000, L"有效歌词"}};
    controller.onLyricReady(std::move(lyric));
    EXPECT_EQ(controller.state().currentLyrics_.size(), 1u);
    EXPECT_EQ(controller.state().currentLyrics_[0].text, std::wstring(L"有效歌词"));
    EXPECT_EQ(publishes, beforePublishes + 2);
}

TEST_CASE("presentation_model.scene_resolution_lead_time_and_transition_retention") {
    PresentationModel model;
    AppSettings settings;
    PlaybackState playback;
    playback.currentKey = L"qq|月牙湾|F.I.R.|61";
    playback.currentTitle = L"月牙湾";
    playback.currentArtist = L"F.I.R.";
    playback.currentLyrics_ = {
        {1000, L"第一句"},
        {2000, L"第二句"},
    };

    SmtcSnapshot snap;
    snap.sessionAlive = true;
    snap.player = SmtcPlayerType::QQMusic;
    snap.title = L"月牙湾";
    snap.artist = L"F.I.R.";
    snap.album = L"爱‧歌姬";
    snap.durationMs = 305991;
    snap.positionMs = 1920; // +100ms 预读后达到 2020ms，应选中第二句（index=1）
    snap.status = PlaybackStatus::Playing;
    snap.canRepeat = true;
    snap.repeatMode = 1;

    IdleFrameInput idle;
    const auto& frame1 = model.update(snap, settings, playback, idle,
                                      {.animateTransition = true, .lyricsChanged = true});
    EXPECT_EQ(frame1.scene, DisplayScene::Lyrics);
    EXPECT_EQ(frame1.currentLine, 1);
    EXPECT_EQ(frame1.media.artist, std::wstring(L"F.I.R. · 爱‧歌姬"));
    EXPECT_TRUE(frame1.media.canRepeat);
    EXPECT_EQ(frame1.media.repeatMode, 2);
    EXPECT_EQ(SmtcSnapshot::nextPlayMode(0), 2);
    EXPECT_EQ(SmtcSnapshot::nextPlayMode(1), 2);
    EXPECT_EQ(SmtcSnapshot::nextPlayMode(2), 3);
    EXPECT_EQ(SmtcSnapshot::nextPlayMode(3), 0);

    // SMTC 过渡事件标题与歌手暂时为空时，保留上一帧的媒体标题与歌手，防止闪空
    SmtcSnapshot transient = snap;
    transient.title.clear();
    transient.artist.clear();
    transient.album.clear();
    const auto& frame2 = model.update(transient, settings, playback, idle, {});
    EXPECT_EQ(frame2.media.title, std::wstring(L"月牙湾"));
    EXPECT_EQ(frame2.media.artist, std::wstring(L"F.I.R. · 爱‧歌姬"));
    EXPECT_TRUE(frame2.media.canRepeat);
    EXPECT_EQ(frame2.media.repeatMode, 2);

    // 高频播放进度补丁与专辑主题色更新
    snap.positionMs = 500;
    const PlaybackPatch patch = model.updatePlayback(snap);
    EXPECT_EQ(patch.currentLine, -1);
    EXPECT_EQ(patch.lineSelectionPositionMs, 500);
    snap.positionMs = 1000;
    const PlaybackPatch firstLine = model.updatePlayback(snap);
    EXPECT_EQ(firstLine.currentLine, 0);
    EXPECT_EQ(firstLine.lineSelectionPositionMs, 1100);

    const auto& colored = model.updateAlbumColor(RGB(120, 104, 88));
    EXPECT_TRUE(colored.media.hasDominantColor);
    EXPECT_EQ(colored.media.dominantColor, RGB(120, 104, 88));
}

TEST_CASE("playback_controller.stale_payload_rejection_and_duration_completion") {
    MessageOnlyTimerWindow timerWindow;
    SmtcSnapshot currentSnap;
    currentSnap.sessionAlive = true;
    currentSnap.player = SmtcPlayerType::QQMusic;
    currentSnap.sourceAppUserModelId = L"QQMusic.exe";
    currentSnap.title = L"歌曲A";
    currentSnap.artist = L"歌手A";
    currentSnap.durationMs = 240000;
    currentSnap.timelineStale = true; // 首次到达时时间线仍是上一首残留
    auto thumbA = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3, 4});
    currentSnap.thumbnail = thumbA;

    int publishCount = 0;
    int coverChangedCount = 0;
    PlaybackController controller(
        [&] { return currentSnap; },
        [&](const SmtcSnapshot&, PresentationUpdate) { ++publishCount; },
        [&](const SmtcSnapshot&) {},
        [&] { ++coverChangedCount; });
    controller.setTimerWindow(timerWindow.hwnd());

    // 1. 歌曲 A 首次以残留时间线进入（duration 视为 0），随后补上真实时长
    const TrackChange firstChange = controller.updateTrack(currentSnap);
    EXPECT_TRUE(firstChange.trackChanged);
    EXPECT_FALSE(firstChange.durationOnly);
    EXPECT_TRUE(firstChange.songIdentityChanged);
    EXPECT_EQ(controller.state().currentKey, std::wstring(L"qq|歌曲A|歌手A|-1"));
    EXPECT_EQ(controller.state().lastCover_, thumbA);

    currentSnap.timelineStale = false;
    currentSnap.durationMs = 200000;
    const TrackChange durationCompletion = controller.updateTrack(currentSnap);
    EXPECT_TRUE(durationCompletion.trackChanged);
    EXPECT_TRUE(durationCompletion.durationOnly);
    EXPECT_FALSE(durationCompletion.songIdentityChanged);
    EXPECT_EQ(controller.state().currentKey, std::wstring(L"qq|歌曲A|歌手A|40"));
    EXPECT_EQ(controller.state().lastCover_, thumbA); // 同曲时长补齐不清空已有封面

    // 2. 手动应用歌词会递增 requestGeneration_，使旧代次的异步歌词结果失效
    const uint64_t staleGeneration = controller.state().requestGeneration_;
    LyricResult manualResult;
    manualResult.ok = true;
    manualResult.fromManual = true;
    manualResult.lines = {{1000, L"手动歌词行", {}, L"翻译行", {}}};
    controller.applyManualLyrics(manualResult, currentSnap);
    EXPECT_TRUE(controller.state().requestGeneration_ > staleGeneration);
    EXPECT_TRUE(controller.state().currentLyricsFromManual_);
    EXPECT_TRUE(controller.state().currentHasTranslation_);
    EXPECT_FALSE(controller.state().currentHasRomanization_);

    auto staleSameTrackPayload = std::make_unique<LyricPayload>();
    staleSameTrackPayload->key = controller.state().currentKey;
    staleSameTrackPayload->requestGeneration = staleGeneration;
    staleSameTrackPayload->result.ok = true;
    staleSameTrackPayload->result.lines = {{1000, L"过期的自动歌词"}};
    controller.onLyricReady(std::move(staleSameTrackPayload));
    EXPECT_EQ(controller.state().currentLyrics_[0].text, std::wstring(L"手动歌词行"));

    // 3. 切换到歌曲 B 后，歌曲 A 的迟到歌词与封面结果必须被丢弃
    const std::wstring keyA = controller.state().currentKey;
    const uint64_t genBeforeSwitch = controller.state().requestGeneration_;
    currentSnap.title = L"歌曲B";
    currentSnap.artist = L"歌手B";
    currentSnap.durationMs = 180000;
    currentSnap.thumbnail.reset();
    controller.updateTrack(currentSnap);
    controller.cancelLyricDebounce();
    EXPECT_EQ(controller.state().currentKey, std::wstring(L"qq|歌曲B|歌手B|36"));

    auto lateLyricA = std::make_unique<LyricPayload>();
    lateLyricA->key = keyA;
    lateLyricA->requestGeneration = genBeforeSwitch;
    lateLyricA->result.ok = true;
    lateLyricA->result.lines = {{1000, L"歌曲A的迟到歌词"}};
    controller.onLyricReady(std::move(lateLyricA));
    EXPECT_TRUE(controller.state().currentLyrics_.empty());

    auto lateCoverA = std::make_unique<CoverPayload>();
    lateCoverA->key = keyA;
    lateCoverA->requestGeneration = genBeforeSwitch;
    lateCoverA->source = CoverSource::QqApi;
    lateCoverA->cover = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{9, 9});
    controller.onCoverReady(std::move(lateCoverA));
    EXPECT_TRUE(controller.state().lastCover_ == nullptr);

    // 4. 当前曲目 B 的有效代次封面正常接收；但若 SMTC 已提供缩略图，API 封面不得覆盖 SMTC
    auto validCoverB = std::make_unique<CoverPayload>();
    validCoverB->key = controller.state().currentKey;
    validCoverB->requestGeneration = controller.state().requestGeneration_;
    validCoverB->source = CoverSource::QqApi;
    auto apiCoverBytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{7, 8});
    validCoverB->cover = apiCoverBytes;
    controller.onCoverReady(std::move(validCoverB));
    EXPECT_EQ(controller.state().lastCover_, apiCoverBytes);
    EXPECT_EQ(controller.state().lastCoverSource_, CoverSource::QqApi);
}

TEST_CASE("utility_logic.base64_and_backdrop_luminance") {
    EXPECT_EQ(base64Decode("5pyI54mZ5rm+"), std::string("\xE6\x9C\x88\xE7\x89\x99\xE6\xB9\xBE"));
    EXPECT_EQ(base64Decode("aGVs\r\nbG8="), std::string("hello"));
    EXPECT_TRUE(base64Decode("").empty());

    EXPECT_NEAR(graphics::backdropLuminance(nullptr, 10, 10), 0.0, 1e-6);
    const uint8_t blackPixels[16] = {};
    EXPECT_NEAR(graphics::backdropLuminance(blackPixels, 2, 2), 0.0, 1e-6);
    const uint8_t whitePixels[16] = {
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255,
    };
    EXPECT_NEAR(graphics::backdropLuminance(whitePixels, 2, 2), 1.0, 1e-4);

    // 亮暗混合背景施加暗区惩罚后，等效亮度应明显低于线性均值并保持在切换白字阈值以下
    const uint8_t halfWhiteHalfBlackPixels[16] = {
        255, 255, 255, 255, 255, 255, 255, 255,
        0,   0,   0,   255, 0,   0,   0,   255,
    };
    const float mixedLuminance = graphics::backdropLuminance(halfWhiteHalfBlackPixels, 2, 2);
    EXPECT_TRUE(mixedLuminance > 0.0f && mixedLuminance < 0.15f);
}

TEST_CASE("track_identity.salt_player_and_spotify_isolate_album_and_platform") {
    EXPECT_TRUE(player_source::usesMetadataLyricPath(SmtcPlayerType::Spotify));
    EXPECT_TRUE(player_source::usesMetadataLyricPath(SmtcPlayerType::SaltPlayer));
    EXPECT_FALSE(player_source::usesMetadataLyricPath(SmtcPlayerType::QQMusic));
    EXPECT_FALSE(player_source::usesMetadataLyricPath(SmtcPlayerType::NetEase));
    EXPECT_FALSE(player_source::usesMetadataLyricPath(SmtcPlayerType::Soda));
    EXPECT_FALSE(player_source::usesMetadataLyricPath(SmtcPlayerType::Unknown));

    SmtcSnapshot saltSnap;
    saltSnap.sessionAlive = true;
    saltSnap.player = SmtcPlayerType::SaltPlayer;
    saltSnap.title = L"歌曲";
    saltSnap.artist = L"歌手";
    saltSnap.album = L"专辑A";
    saltSnap.durationMs = 200000;

    SmtcSnapshot saltAlbumB = saltSnap;
    saltAlbumB.album = L"专辑B";

    SmtcSnapshot spotifySnap = saltSnap;
    spotifySnap.player = SmtcPlayerType::Spotify;

    const std::wstring saltKey = makeTrackKey(saltSnap);
    const std::wstring saltAlbumBKey = makeTrackKey(saltAlbumB);
    const std::wstring spotifyKey = makeTrackKey(spotifySnap);
    EXPECT_EQ(saltKey, std::wstring(L"saltplayer|歌曲|歌手|40|专辑A"));
    EXPECT_NE(saltKey, saltAlbumBKey);
    EXPECT_NE(saltKey, spotifyKey);

    EXPECT_TRUE(snapshotMatchesTrackKey(saltSnap, saltKey));
    EXPECT_FALSE(snapshotMatchesTrackKey(spotifySnap, saltKey));

    SmtcSnapshot transientSalt = saltSnap;
    transientSalt.title.clear();
    EXPECT_TRUE(snapshotMatchesTrackKey(transientSalt, saltKey));
    EXPECT_FALSE(snapshotMatchesTrackKey(transientSalt, spotifyKey));

    SmtcSnapshot transientSpotify = spotifySnap;
    transientSpotify.title.clear();
    EXPECT_FALSE(snapshotMatchesTrackKey(transientSpotify, saltKey));

    saltSnap.sessionAlive = false;
    EXPECT_FALSE(snapshotMatchesTrackKey(saltSnap, saltKey));
}

TEST_CASE("playback_controller.salt_player_debounce_duration_update_and_transient_title") {
    MessageOnlyTimerWindow timerWindow;
    EXPECT_TRUE(timerWindow.hwnd() != nullptr);

    auto thumbA = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
    auto thumbB = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{4, 5, 6});
    SmtcSnapshot currentSnap;
    currentSnap.sessionAlive = true;
    currentSnap.player = SmtcPlayerType::SaltPlayer;
    currentSnap.sourceAppUserModelId =
        L"Sakawish.SaltPlayerforWindows_q65q631pyh094!SaltPlayerforWindows";
    currentSnap.title = L"歌曲A";
    currentSnap.artist = L"歌手A";
    currentSnap.album = L"专辑1";
    currentSnap.durationMs = 200000;
    currentSnap.status = PlaybackStatus::Playing;
    currentSnap.thumbnail = thumbA;

    PlaybackController controller(
        [&] { return currentSnap; }, [](const SmtcSnapshot&, PresentationUpdate) {},
        [](const SmtcSnapshot&) {}, [] {});
    controller.setTimerWindow(timerWindow.hwnd());

    // 1. 首次进入椒盐音乐曲目 A：挂载 200ms 防抖并保留封面
    const TrackChange first = controller.updateTrack(currentSnap);
    EXPECT_TRUE(first.trackChanged);
    EXPECT_FALSE(first.durationOnly);
    EXPECT_TRUE(first.songIdentityChanged);
    EXPECT_TRUE(controller.state().lyricLoading_);
    EXPECT_TRUE(controller.state().lyricDebounceArmed_);
    EXPECT_EQ(controller.state().lastCover_, thumbA);

    // 2. 旧歌名 + 新时长：识别为仅时长更新，保留封面、刷新代次并继续挂起防抖
    const uint64_t genBeforeDuration = controller.state().requestGeneration_;
    currentSnap.durationMs = 215000;
    const TrackChange durationUpdate = controller.updateTrack(currentSnap);
    EXPECT_TRUE(durationUpdate.trackChanged);
    EXPECT_TRUE(durationUpdate.durationOnly);
    EXPECT_FALSE(durationUpdate.songIdentityChanged);
    EXPECT_TRUE(controller.state().lyricLoading_);
    EXPECT_TRUE(controller.state().lyricDebounceArmed_);
    EXPECT_EQ(controller.state().lastCover_, thumbA);
    EXPECT_TRUE(controller.state().requestGeneration_ > genBeforeDuration);

    // 3. 新歌名 B 到达并清空封面：识别为完整切歌并释放旧封面
    currentSnap.title = L"歌曲B";
    currentSnap.artist = L"歌手B";
    currentSnap.thumbnail.reset();
    const TrackChange songBChange = controller.updateTrack(currentSnap);
    EXPECT_TRUE(songBChange.trackChanged);
    EXPECT_FALSE(songBChange.durationOnly);
    EXPECT_TRUE(songBChange.songIdentityChanged);
    EXPECT_TRUE(controller.state().lastCover_ == nullptr);
    EXPECT_TRUE(controller.state().lyricDebounceArmed_);
    EXPECT_TRUE(controller.state().lyricLoading_);

    // 4. 同歌名歌手但专辑变化：不视为 durationOnly
    currentSnap.album = L"专辑2";
    currentSnap.thumbnail = thumbB;
    const TrackChange albumChange = controller.updateTrack(currentSnap);
    EXPECT_TRUE(albumChange.trackChanged);
    EXPECT_FALSE(albumChange.durationOnly);
    EXPECT_FALSE(albumChange.songIdentityChanged);
    EXPECT_EQ(controller.state().lastCover_, thumbB);
    EXPECT_EQ(controller.state().lastSmtcThumbnail, thumbB);
    EXPECT_TRUE(controller.state().lyricDebounceArmed_);

    PresentationModel model;
    AppSettings settings;
    IdleFrameInput idle;
    EXPECT_EQ(model.update(currentSnap, settings, controller.state(), idle, {}).statusText,
              std::wstring(L"歌曲B - 歌手B"));

    // 5. 过渡期空标题触发防抖回调：续挂防抖，不递增 requestGeneration_
    const uint64_t genBeforeTransient = controller.state().requestGeneration_;
    currentSnap.title.clear();
    controller.onLyricDebounce();
    EXPECT_TRUE(controller.state().lyricDebounceArmed_);
    EXPECT_TRUE(controller.state().lyricLoading_);
    EXPECT_EQ(controller.state().requestGeneration_, genBeforeTransient);

    // 6. 恢复标题并手动应用歌词：取消防抖且后续重复 onLyricDebounce 为无操作
    currentSnap.title = L"歌曲B";
    LyricResult manual;
    manual.ok = true;
    manual.fromManual = true;
    manual.lines = {{1000, L"手动歌词"}};
    controller.applyManualLyrics(manual, currentSnap);
    EXPECT_FALSE(controller.state().lyricDebounceArmed_);
    EXPECT_FALSE(controller.state().lyricLoading_);
    EXPECT_TRUE(controller.state().currentLyricsFromManual_);
    const uint64_t genAfterManual = controller.state().requestGeneration_;
    controller.onLyricDebounce();
    EXPECT_EQ(controller.state().requestGeneration_, genAfterManual);
    EXPECT_FALSE(controller.state().lyricLoading_);

    // 7. 无歌词与会话关闭文案
    PlaybackState emptyState;
    emptyState.lastPlayer_ = SmtcPlayerType::SaltPlayer;
    emptyState.currentKey = controller.state().currentKey;
    emptyState.currentTitle = L"歌曲B";
    emptyState.currentArtist = L"歌手B";
    EXPECT_EQ(model.update(currentSnap, settings, emptyState, idle, {}).statusText,
              std::wstring(L"歌曲B - 歌手B"));
    SmtcSnapshot closedSnap = currentSnap;
    closedSnap.sessionAlive = false;
    IdleFrameInput noIdle;
    noIdle.presentation.showText = false;
    noIdle.presentation.quickStartEnabled = false;
    EXPECT_EQ(model.update(closedSnap, settings, controller.state(), noIdle, {}).statusText,
              std::wstring(L"椒盐音乐未运行"));

    // 8. 沉浸模式无歌曲且关闭全部空闲内容时，依然进入空闲常驻状态并清空中间占位文案
    IdleFrameInput immersiveEmptyIdle;
    immersiveEmptyIdle.presentation.showText = false;
    immersiveEmptyIdle.presentation.quickStartEnabled = false;
    immersiveEmptyIdle.presentation.todayTasksEnabled = false;
    immersiveEmptyIdle.keepIdleVisible = true;
    const auto& immersiveIdleFrame =
        model.update(closedSnap, settings, controller.state(), immersiveEmptyIdle, {});
    EXPECT_EQ(immersiveIdleFrame.scene, DisplayScene::Idle);
    EXPECT_TRUE(immersiveIdleFrame.visible);
    EXPECT_TRUE(immersiveIdleFrame.statusText.empty());

    immersiveEmptyIdle.presentation.quickStartEnabled = true;
    const auto& immersiveQuickFrame =
        model.update(closedSnap, settings, controller.state(), immersiveEmptyIdle, {});
    EXPECT_EQ(immersiveQuickFrame.scene, DisplayScene::Idle);
    EXPECT_TRUE(immersiveQuickFrame.visible);
    EXPECT_TRUE(immersiveQuickFrame.statusText.empty());

    // 9. 多屏不同模式时，frameForView 按各自屏幕的空闲常驻与资源监视状态独立派生帧
    IdleFrameInput embeddedEmptyIdle;
    embeddedEmptyIdle.presentation.showText = false;
    embeddedEmptyIdle.presentation.quickStartEnabled = false;
    embeddedEmptyIdle.presentation.todayTasksEnabled = false;
    const auto embeddedHidden =
        model.frameForView(closedSnap, controller.state(), embeddedEmptyIdle);
    EXPECT_EQ(embeddedHidden.scene, DisplayScene::NoPlayback);
    EXPECT_FALSE(embeddedHidden.visible);

    embeddedEmptyIdle.presentation.quickStartEnabled = true;
    const auto embeddedQuick =
        model.frameForView(closedSnap, controller.state(), embeddedEmptyIdle);
    EXPECT_EQ(embeddedQuick.scene, DisplayScene::Idle);
    EXPECT_TRUE(embeddedQuick.visible);
    EXPECT_EQ(embeddedQuick.statusText, std::wstring(L"快捷启动"));
}
