#include "ui/settings/lyric_section.h"
#include "ui/settings/ids.h"
#include "ui/settings/section_metrics.h"
#include <algorithm>

namespace settings_ui {
std::vector<Row> createLyricRows(const SettingsState& state) {
    std::vector<Row> rows;
    RowBuilder builder(rows);
    const bool vertical = state.verticalTaskbar;
    const bool minimal = state.renderMode == kRenderModeMinimal;
    builder.addHeader(L"排版");
    builder.addToggle(kIdDoubleLine, L"双行歌词", vertical ? false : state.doubleLineLyrics)
        .enabled = !vertical;
    builder.addRadio(kIdAlignment, L"歌词对齐", nullptr, {L"左对齐", L"居中", L"右对齐"},
                     vertical ? 0 : state.lyricAlignment, !vertical && state.taskbarViewMode == 0,
                     kRowH);
    builder.addHeader(L"颜色与效果");
    builder.addButton(kIdFontColor, L"歌词字体颜色与效果", nullptr, L"打开…");
    builder.addToggle(kIdFollowAlbum, L"歌词已播放颜色跟随专辑", state.followAlbum);
    Row& gradient = builder.addRow(
        kIdLyricPlayedGradient, ControlKind::Toggle, L"歌词已播放色渐变",
        L"以歌词已播放色为基色，由底部深色连续过渡到顶部浅色。", 40.0f, kRowTallH);
    gradient.checked = state.lyricPlayedGradient;
    Row& simulateKaraoke = builder.addRow(
        kIdSimulateWordByWord, ControlKind::Toggle, L"逐行歌词模拟逐字效果",
        L"对无逐字时间轴的逐行歌词，按当前行估算时长模拟逐字推进，并联动歌词特效；极简模式下停用。",
        40.0f, kRowTallH);
    simulateKaraoke.checked = state.simulateWordByWordLyrics;
    simulateKaraoke.enabled = !minimal;
    builder.addHeader(L"歌词特效");
    const wchar_t* lyricEffectNames[kLyricEffectCount] = {L"星尘跟唱", L"跟唱流光", L"笔触下划线",
                                                          L"字间涟漪", L"逐字浮波", L"歌词彩蛋"};
    for (int i = 0; i < kLyricEffectCount; ++i) {
        Row& effect = builder.addRow(
            kIdLyricEffectFirst + i, ControlKind::Toggle, lyricEffectNames[i],
            i == 0
                ? L"仅单行逐字歌词生效，歌词下沉、星星跳动旋转并带流星粒子拖尾；与逐字浮波及翻译/罗马音互斥。"
                : i == static_cast<int>(LyricEffect::WordWave)
                    ? L"支持单行和双行逐字歌词，整行长波轻柔起伏，逐字下沉后缓慢浮起；与星尘跟唱互斥。"
                    : i == static_cast<int>(LyricEffect::EasterEggs)
                        ? L"仅横向逐字歌词生效，小任务栏及极简模式下停用。"
                        : L"需要逐字歌词，仅作用于当前演唱行。",
            40.0f, kRowTallH);
        effect.checked = (state.lyricEffects & (1u << i)) != 0;
        effect.enabled = !minimal && (i != 0 || vertical || !state.doubleLineLyrics);
        if (i == static_cast<int>(LyricEffect::WordWave) ||
            i == static_cast<int>(LyricEffect::EasterEggs))
            effect.enabled = !minimal && !vertical && !state.compactTaskbar;
    }
    builder.addHeader(L"翻译与罗马音");
    builder.addToggle(kIdSecondaryOn, L"开启翻译/罗马音", state.secondaryEnabled);
    const wchar_t* secondaryHint =
        state.secondaryAvailability == SecondaryLyricAvailability::Loading
            ? L"正在检查翻译和罗马音…"
        : state.secondaryAvailability == SecondaryLyricAvailability::Unavailable
            ? L"当前歌曲无翻译或罗马音"
            : L"";
    builder.addRadio(kIdSecondaryType, L"辅助歌词类型", secondaryHint, {L"翻译", L"罗马音"},
                     state.preferRomanization ? 1 : 0,
                     state.secondaryEnabled &&
                         state.secondaryAvailability == SecondaryLyricAvailability::Available,
                     *secondaryHint ? kRowTallH : kRowH);
    builder.addHeader(L"本地歌词");
    Row& customLocalEnabled = builder.addRow(
        kIdCustomLocalLyricsEnabled, ControlKind::Toggle, L"使用自定义本地歌词",
        L"优先从指定目录匹配 .lrc 与音频内嵌歌词（不递归子目录），可设置全部应用或仅指定播放器生效。",
        40.0f, kRowTallH);
    customLocalEnabled.checked = state.customLocalLyricsEnabled;
    const std::wstring customLocalPathHint = state.customLocalLyricsPath.empty()
                                                 ? std::wstring(L"未配置")
                                                 : state.customLocalLyricsPath;
    Row& customLocalPath = builder.addButton(kIdCustomLocalLyricsPath, L"本地歌词目录",
                                             customLocalPathHint.c_str(), L"选择文件夹…", kRowTallH);
    customLocalPath.enabled = state.customLocalLyricsEnabled;
    const int customScope = std::clamp(state.customLocalLyricsScope, 0, 1);
    builder.addRadio(kIdCustomLocalLyricsScope, L"生效范围", nullptr,
                     {L"全部应用", L"仅指定播放器"}, customScope,
                     state.customLocalLyricsEnabled, kRowH);
    const wchar_t* playersHint =
        customScope == 1
            ? L"仅当以下播放器处于播放状态时，优先读取自定义本地歌词目录。"
            : L"切换为“仅指定播放器”后可指定需要读取本地歌词的播放器。";
    const size_t visiblePlayers =
        customScope == 1 ? state.customLocalLyricsPlayers.size() : 0;
    Row& customPlayers = builder.addRow(
        kIdCustomLocalLyricsPlayers, ControlKind::AppList, L"指定播放器", playersHint, 0.0f,
        kIdleAppsBaseH + static_cast<float>(std::max<size_t>(1, visiblePlayers)) * kIdleAppItemH);
    customPlayers.height = customPlayers.minHeight;
    customPlayers.enabled = state.customLocalLyricsEnabled && customScope == 1;
    builder.addToggle(kIdQqLocalLyricsEnabled, L"使用 QQ 音乐本地歌词", state.qqLocalLyricsEnabled);
    const std::wstring localPathHint =
        state.qqLocalLyricsPath.empty() ? std::wstring(L"未配置") : state.qqLocalLyricsPath;
    Row& localPath = builder.addButton(kIdQqLocalLyricsPath, L"QQ 音乐本地歌词目录",
                                       localPathHint.c_str(), L"选择文件夹…", kRowTallH);
    localPath.enabled = state.qqLocalLyricsEnabled;
    Row& persistOrder = builder.addRow(
        kIdQqLocalLyricsPersistOrder, ControlKind::Toggle, L"记住本地歌词版本选择",
        L"记住每首歌切换后的本地/在线版本；关闭后不保存新记录，但仍读取已有记录", 40.0f, kRowTallH);
    persistOrder.checked = state.qqLocalLyricsPersistOrder;
    persistOrder.enabled = state.customLocalLyricsEnabled || state.qqLocalLyricsEnabled;

    updateLyricRows(rows, state);
    return rows;
}

void updateLyricRows(std::vector<Row>& rows, const SettingsState& s) {
    auto findRow = [&](int id) { return settings_ui::findRow(rows, id); };
    const bool vertical = s.verticalTaskbar;
    const bool minimal = s.renderMode == kRenderModeMinimal;
    if (auto* row = findRow(kIdFollowAlbum))
        row->checked = s.followAlbum;
    if (auto* row = findRow(kIdLyricPlayedGradient))
        row->checked = s.lyricPlayedGradient;
    if (auto* row = findRow(kIdSimulateWordByWord)) {
        row->checked = s.simulateWordByWordLyrics;
        row->enabled = !minimal;
    }
    if (auto* row = findRow(kIdDoubleLine)) {
        row->checked = vertical ? false : s.doubleLineLyrics;
        row->enabled = !vertical;
    }
    for (int i = 0; i < kLyricEffectCount; ++i) {
        if (auto* row = findRow(kIdLyricEffectFirst + i)) {
            row->checked = (s.lyricEffects & (1u << i)) != 0;
            row->enabled = !minimal && (i != 0 || vertical || !s.doubleLineLyrics);
            if (i == static_cast<int>(LyricEffect::WordWave) ||
                i == static_cast<int>(LyricEffect::EasterEggs))
                row->enabled = !minimal && !vertical && !s.compactTaskbar;
        }
    }
    if (auto* row = findRow(kIdAlignment)) {
        row->selected = vertical ? 0 : s.lyricAlignment;
        row->enabled = !vertical && s.taskbarViewMode == 0;
    }
    if (auto* row = findRow(kIdSecondaryOn))
        row->checked = s.secondaryEnabled;
    if (auto* row = findRow(kIdSecondaryType)) {
        row->hint = s.secondaryAvailability == SecondaryLyricAvailability::Loading
                        ? L"正在检查翻译和罗马音…"
                    : s.secondaryAvailability == SecondaryLyricAvailability::Unavailable
                        ? L"当前歌曲无翻译或罗马音"
                        : L"";
        row->showHint = !row->hint.empty();
        row->minHeight = row->showHint ? kRowTallH : kRowH;
        row->height = row->minHeight;
        row->selected = s.preferRomanization ? 1 : 0;
        row->enabled = s.secondaryEnabled &&
                       s.secondaryAvailability == SecondaryLyricAvailability::Available;
    }
    if (auto* row = findRow(kIdCustomLocalLyricsEnabled))
        row->checked = s.customLocalLyricsEnabled;
    if (auto* row = findRow(kIdCustomLocalLyricsPath)) {
        row->hint =
            s.customLocalLyricsPath.empty() ? std::wstring(L"未配置") : s.customLocalLyricsPath;
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
        row->enabled = s.customLocalLyricsEnabled;
    }
    const int customScope = std::clamp(s.customLocalLyricsScope, 0, 1);
    if (auto* row = findRow(kIdCustomLocalLyricsScope)) {
        row->selected = customScope;
        row->enabled = s.customLocalLyricsEnabled;
    }
    if (auto* row = findRow(kIdCustomLocalLyricsPlayers)) {
        row->hint = customScope == 1
                        ? L"仅当以下播放器处于播放状态时，优先读取自定义本地歌词目录。"
                        : L"切换为“仅指定播放器”后可指定需要读取本地歌词的播放器。";
        row->showHint = true;
        const size_t visibleCount = customScope == 1 ? s.customLocalLyricsPlayers.size() : 0;
        row->minHeight =
            kIdleAppsBaseH + static_cast<float>(std::max<size_t>(1, visibleCount)) * kIdleAppItemH;
        row->height = row->minHeight;
        row->enabled = s.customLocalLyricsEnabled && customScope == 1;
    }
    if (auto* row = findRow(kIdQqLocalLyricsEnabled))
        row->checked = s.qqLocalLyricsEnabled;
    if (auto* row = findRow(kIdQqLocalLyricsPersistOrder)) {
        row->checked = s.qqLocalLyricsPersistOrder;
        row->enabled = s.customLocalLyricsEnabled || s.qqLocalLyricsEnabled;
    }
    if (auto* row = findRow(kIdQqLocalLyricsPath)) {
        row->hint = s.qqLocalLyricsPath.empty() ? std::wstring(L"未配置") : s.qqLocalLyricsPath;
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
        row->enabled = s.qqLocalLyricsEnabled;
    }

    updateCompactLyricRows(rows, s);
}

void updateCompactLyricRows(std::vector<Row>& rows, const SettingsState& state) {
    auto findRow = [&](int id) { return settings_ui::findRow(rows, id); };
    const wchar_t* compactHint = L"小任务栏下暂时停用，恢复正常大小后自动恢复；已保留开关偏好。";
    for (int id : {kIdDoubleLine, kIdSecondaryOn}) {
        if (auto* row = findRow(id)) {
            row->hint = state.compactTaskbar ? compactHint : L"";
            row->showHint = state.compactTaskbar;
            row->minHeight = row->showHint ? kRowTallH : kRowH;
            row->height = row->minHeight;
            if (state.compactTaskbar)
                row->enabled = false;
        }
    }
    if (auto* row = findRow(kIdLyricEffectFirst)) {
        row->hint =
            state.compactTaskbar
                ? compactHint
                : L"仅单行逐字歌词生效，歌词下沉、星星跳动旋转并带流星粒子拖尾；与逐字浮波及翻译/罗马音互斥。";
        if (state.compactTaskbar)
            row->enabled = false;
    }
    if (state.compactTaskbar) {
        if (auto* row = findRow(kIdSecondaryType))
            row->enabled = false;
    }
}

bool handleLyricCommand(int id, Row& value, std::vector<Row>& rows, SettingsState& state,
                        const SettingsActions& actions, int option) {
    Row* row = &value;
    auto findRow = [&](int rowId) { return settings_ui::findRow(rows, rowId); };
    switch (id) {
    case kIdFontColor:
        if (actions.onFontColorEffect)
            actions.onFontColorEffect();
        break;
    case kIdLyricPlayedGradient:
        row->checked = !row->checked;
        state.lyricPlayedGradient = row->checked;
        if (actions.onLyricPlayedGradient)
            actions.onLyricPlayedGradient(row->checked);
        break;
    case kIdSimulateWordByWord:
        row->checked = !row->checked;
        state.simulateWordByWordLyrics = row->checked;
        if (actions.onSimulateWordByWordLyrics)
            actions.onSimulateWordByWordLyrics(row->checked);
        break;
    case kIdFollowAlbum:
        row->checked = !row->checked;
        if (actions.onFollowAlbum)
            actions.onFollowAlbum(row->checked);
        break;
    case kIdLyricEffectFirst:
    case kIdLyricEffectFirst + 1:
    case kIdLyricEffectFirst + 2:
    case kIdLyricEffectFirst + 3:
    case kIdLyricEffectFirst + 4:
    case kIdLyricEffectFirst + 5:
        row->checked = !row->checked;
        state.lyricEffects = withLyricEffect(state.lyricEffects,
            static_cast<LyricEffect>(row->id - kIdLyricEffectFirst), row->checked);
        updateLyricRows(rows, state);
        if (actions.onLyricEffect)
            actions.onLyricEffect(row->id - kIdLyricEffectFirst, row->checked);
        break;
    case kIdDoubleLine:
        row->checked = !row->checked;
        state.doubleLineLyrics = row->checked;
        updateLyricRows(rows, state);
        if (actions.onDoubleLineLyrics)
            actions.onDoubleLineLyrics(row->checked);
        break;
    case kIdAlignment:
        if (actions.onLyricAlignment)
            actions.onLyricAlignment(row->selected);
        break;
    case kIdSecondaryOn:
        row->checked = !row->checked;
        if (actions.onSecondaryEnabled)
            actions.onSecondaryEnabled(row->checked);
        if (auto* type = findRow(kIdSecondaryType))
            type->enabled = row->checked &&
                            state.secondaryAvailability == SecondaryLyricAvailability::Available;
        break;
    case kIdSecondaryType:
        if (actions.onPreferRomanization)
            actions.onPreferRomanization(row->selected == 1);
        break;
    case kIdCustomLocalLyricsEnabled:
        row->checked = !row->checked;
        state.customLocalLyricsEnabled = row->checked;
        updateLyricRows(rows, state);
        if (actions.onCustomLocalLyricsEnabled)
            actions.onCustomLocalLyricsEnabled(row->checked);
        break;
    case kIdCustomLocalLyricsPath:
        if (actions.onPickCustomLocalLyricsPath)
            actions.onPickCustomLocalLyricsPath();
        break;
    case kIdCustomLocalLyricsScope:
        state.customLocalLyricsScope = std::clamp(row->selected, 0, 1);
        updateLyricRows(rows, state);
        if (actions.onCustomLocalLyricsScope)
            actions.onCustomLocalLyricsScope(state.customLocalLyricsScope);
        break;
    case kIdCustomLocalLyricsPlayers:
        if (option >= 0) {
            if (static_cast<size_t>(option) < state.customLocalLyricsPlayers.size() &&
                actions.onRemoveCustomLocalLyricsPlayer)
                actions.onRemoveCustomLocalLyricsPlayer(option);
        } else if (state.customLocalLyricsPlayers.size() < kMaxGenericSmtcRules &&
                   actions.onAddCustomLocalLyricsPlayer) {
            actions.onAddCustomLocalLyricsPlayer();
        }
        break;
    case kIdQqLocalLyricsEnabled:
        row->checked = !row->checked;
        state.qqLocalLyricsEnabled = row->checked;
        if (auto* persist = findRow(kIdQqLocalLyricsPersistOrder))
            persist->enabled = state.customLocalLyricsEnabled || state.qqLocalLyricsEnabled;
        if (auto* path = findRow(kIdQqLocalLyricsPath))
            path->enabled = row->checked;
        if (actions.onQqLocalLyricsEnabled)
            actions.onQqLocalLyricsEnabled(row->checked);
        break;
    case kIdQqLocalLyricsPersistOrder:
        row->checked = !row->checked;
        if (actions.onQqLocalLyricsPersistOrder)
            actions.onQqLocalLyricsPersistOrder(row->checked);
        break;
    case kIdQqLocalLyricsPath:
        if (actions.onPickQqLocalLyricsPath)
            actions.onPickQqLocalLyricsPath();
        break;
    default:
        return false;
    }
    return true;
}
} // namespace settings_ui
