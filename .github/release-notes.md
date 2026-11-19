## Windows任务栏歌词 v2.17.0
把 QQ 音乐、网易云、汽水音乐、Spotify和椒盐音乐的歌词嵌入 Windows 10/11 任务栏——滚动歌词、每日一言、快速打开、滴答清单今日任务、逐字高亮、播放控制、音频频谱，支持 Windows 11 任务栏上、下、左、右四边布局。程序本体为 C++ 原生实现，无 Electron、WebView 和 .NET/WPF 运行时，秉持工具软件应尽可能少占内存的原则。

### ✨ 新增功能

- 新增椒盐音乐支持，测试版本：微软商店版本1.19.3 @stinaCN

### 📦 下载与使用

下载软件安装包 `WindowsTaskbarLyrics-2.17.0-Setup.exe` 安装使用，安装向导提供简体中文界面。

如果安装前检测到 WindowsTaskbarLyrics 正在运行，请先通过系统托盘图标退出软件，再点击安装程序中的「重试」继续安装。发布包包含前置依赖时，安装程序会自动安装 Microsoft Visual C++ 运行库。

首次使用滴答清单今日任务时，在软件设置中的「滴答清单」项填写 [滴答清单网页版](https://dida365.com/webapp/)「设置 → 账号与安全 → API 口令」中创建的普通用户 API 口令，然后开启自动同步。任务详情通过网页打开，任务完成操作可直接在任务卡片中进行。

也可以在软件的「关于」窗口中选择 GitHub 或 Gitee 更新源，检查并安装新版本。

> 网易云音乐需要额外下载 BetterNCM 和 InfLink-rs 插件，具体下载方式可以参考 [README](https://github.com/H0308/taskbar-lyrics-displayer?tab=readme-ov-file)。

### ⚠️ 注意事项

首次运行如被 SmartScreen 拦截，点击「更多信息」→「仍要运行」即可

### 🔧 自行构建

Visual Studio 2022 + vcpkg + CMake + Ninja，详见 [README](https://github.com/H0308/taskbar-lyrics-displayer?tab=readme-ov-file)。
