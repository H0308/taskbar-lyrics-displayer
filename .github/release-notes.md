## Windows任务栏歌词 v2.16.1
把 QQ 音乐、网易云、汽水音乐和Spotify的歌词嵌入 Windows 10/11 任务栏——滚动歌词、每日一言、快速打开、滴答清单今日任务、逐字高亮、播放控制、音频频谱，支持 Windows 11 任务栏上、下、左、右四边布局。程序本体为 C++ 原生实现，无 Electron、WebView 和 .NET/WPF 运行时，秉持工具软件应尽可能少占内存的原则。

### 🚀 功能优化与修复

- 优化内嵌控件显示效果，支持切换到常驻显示 @hyrinx
- 优化任务栏位置设置，支持更多空闲位置 @hyrinx
- 优化应用全屏判定，减少部分应用误判的情况
- 修复全屏模式退出后，悬浮卡片设置页没有恢复可设置状态的问题
- 修复QQ音乐开启黑胶唱片时，从暂停到再播放唱片的转动会恢复到原点重新开始的问题
- 修复歌手艺名与本名顺序导致歌词匹配失败的问题
- 修复网易云音乐最小化后，点击图标或者封面后开启betterncm弹窗的问题 @hyrinx

### 📦 下载与使用

下载软件安装包 `WindowsTaskbarLyrics-2.16.1-Setup.exe` 安装使用，安装向导提供简体中文界面。

如果安装前检测到 WindowsTaskbarLyrics 正在运行，请先通过系统托盘图标退出软件，再点击安装程序中的「重试」继续安装。发布包包含前置依赖时，安装程序会自动安装 Microsoft Visual C++ 运行库。

首次使用滴答清单今日任务时，在软件设置中的「滴答清单」项填写 [滴答清单网页版](https://dida365.com/webapp/)「设置 → 账号与安全 → API 口令」中创建的普通用户 API 口令，然后开启自动同步。任务详情通过网页打开，任务完成操作可直接在任务卡片中进行。

也可以在软件的「关于」窗口中选择 GitHub 或 Gitee 更新源，检查并安装新版本。

> 网易云音乐需要额外下载 BetterNCM 和 InfLink-rs 插件，具体下载方式可以参考 [README](https://github.com/H0308/taskbar-lyrics-displayer?tab=readme-ov-file)。

### ⚠️ 注意事项

首次运行如被 SmartScreen 拦截，点击「更多信息」→「仍要运行」即可

### 🔧 自行构建

Visual Studio 2022 + vcpkg + CMake + Ninja，详见 [README](https://github.com/H0308/taskbar-lyrics-displayer?tab=readme-ov-file)。
