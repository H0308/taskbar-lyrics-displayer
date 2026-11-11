#pragma once

// 界面字号统一使用 DIP；绘制、文本测量和原生输入控件共用这些定义。
// 修改这里即可调整所有引用窗口，无需修改字体创建或 DPI 换算逻辑。
namespace fluent::font_sizes {

inline constexpr float body = 15.0f;    // 正文、输入框
inline constexpr float button = 15.0f;  // 按钮
inline constexpr float label = 14.0f;   // 标签、选项
inline constexpr float caption = 13.0f; // 说明、次要文字
inline constexpr float smallText = 12.0f; // 小字提示
inline constexpr float section = 14.0f; // 设置分组标题
inline constexpr float title = 21.0f;   // 普通窗口标题
inline constexpr float brandTitle = 22.0f;
inline constexpr float largeTitle = 23.0f;
inline constexpr float emphasis = 16.0f;
inline constexpr float subheading = 17.0f;
inline constexpr float heading = 19.0f;
inline constexpr float code = 13.0f;

namespace settings {
inline constexpr float shortcut = 11.5f;
inline constexpr float searchTrigger = 14.5f;
inline constexpr float navigationFooter = 13.5f;
inline constexpr float option = 13.5f;
inline constexpr float artwork = 15.0f;
inline constexpr float artworkDigits = 12.0f;
} // namespace settings

namespace search {
inline constexpr float input = 15.5f;
inline constexpr float result = 14.5f;
inline constexpr float footer = 12.5f;
} // namespace search

namespace fontPicker {
inline constexpr float familySample = 16.0f;
} // namespace fontPicker

namespace log {
inline constexpr float primaryValue = 17.0f;
inline constexpr float value = 16.0f;
} // namespace log

namespace mediaPopup {
inline constexpr float source = 12.0f;
inline constexpr float time = 12.0f;
inline constexpr float title = 18.0f;
inline constexpr float artist = 13.0f;
inline constexpr float icon = 14.0f;
inline constexpr float idleHeader = 13.0f;
inline constexpr float idleQuote = 15.0f;
inline constexpr float idleSource = 12.0f;
inline constexpr float idleApp = 11.5f;
} // namespace mediaPopup

namespace songToast {
inline constexpr float title = 14.0f;
inline constexpr float artist = 12.5f;
} // namespace songToast

// 任务栏字号受安全空间和用户配置约束，只集中原有基准，不随界面字号放大。
namespace taskbar {
inline constexpr float mainScale = 1.18f;
inline constexpr float previewScale = 0.86f;
inline constexpr float minimum = 9.0f;
inline constexpr float maximum = 18.0f;
inline constexpr float base = 12.0f;
inline constexpr float resource = 13.5f;
inline constexpr float clock = 13.5f;
inline constexpr float date = 11.0f;
inline constexpr float dragPreview = 11.0f;
inline constexpr float pickerInitial = 16.0f;
inline constexpr float colorPreview = 14.0f;
} // namespace taskbar

// 宠物的睡眠文字属于图案，保留原有比例。
namespace dockPet {
inline constexpr float sleep[] = {9.0f, 11.0f, 13.5f};
} // namespace dockPet

} // namespace fluent::font_sizes
