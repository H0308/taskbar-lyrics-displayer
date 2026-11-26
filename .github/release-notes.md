## Windows任务栏歌词 v2.25.0
把 QQ 音乐、网易云、汽水音乐、Spotify和椒盐音乐的歌词嵌入 Windows 10/11 任务栏——滚动歌词、每日一言、快速打开、滴答清单今日任务、逐字高亮、播放控制、音频频谱，支持 Windows 11 任务栏上、下、左、右四边布局。程序本体为 C++ 原生实现，无 Electron、WebView 和 .NET/WPF 运行时，秉持工具软件应尽可能少占内存的原则。

### 新增功能

- 任务栏状态区新增 **AI 额度**：可在资源监视旁显示 Codex 的 5 小时与每周剩余额度，以及 DeepSeek 账户余额（包含充值金额与赠送金额）。
- Codex 直接读取本机 `~/.codex/auth.json` 登录状态（支持 `CODEX_HOME`），不改写其他客户端配置；DeepSeek 使用 API Key，Key 保存在 Windows 凭据管理器，不写入配置文件或运行日志，查询只请求对应服务的官方接口。
- 额度行与资源监视共用位置、紧凑显示、隐藏标签和滚轮翻页；Codex 两个周期合并为一格，并把 CPU／内存排在第一页，避免额度行挤掉内存显示。
- 设置窗口新增「AI 额度」页，可分别开关 Codex 和 DeepSeek、设置或清除 DeepSeek API Key、立即刷新；成功结果约每 5 分钟自动刷新，失败按退避间隔重试。

### 下载与使用

下载软件安装包 `WindowsTaskbarLyrics-2.25.0-Setup.exe` 安装使用，安装向导提供简体中文界面。

如果安装前检测到 WindowsTaskbarLyrics 正在运行，请先通过系统托盘图标退出软件，再点击安装程序中的「重试」继续安装。发布包包含前置依赖时，安装程序会自动安装 Microsoft Visual C++ 运行库。

首次使用滴答清单今日任务时，在软件设置中的「滴答清单」项填写 [滴答清单网页版](https://dida365.com/webapp/)「设置 → 账号与安全 → API 口令」中创建的普通用户 API 口令，然后开启自动同步。任务详情通过网页打开，任务完成操作可直接在任务卡片中进行。

也可以在软件的「关于」窗口中选择 GitHub 或 Gitee 更新源，检查并安装新版本。

> 网易云音乐需要额外下载 BetterNCM 和 InfLink-rs 插件，具体下载方式可以参考 [README](https://github.com/H0308/taskbar-lyrics-displayer?tab=readme-ov-file)。

### 注意事项

首次运行如被 SmartScreen 拦截，点击「更多信息」→「仍要运行」即可

### 自行构建

Visual Studio 2022 + vcpkg + CMake + Ninja，详见 [README](https://github.com/H0308/taskbar-lyrics-displayer?tab=readme-ov-file)。
