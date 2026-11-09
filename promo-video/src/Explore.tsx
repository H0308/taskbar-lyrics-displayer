import {AbsoluteFill, Easing, Interactive, interpolate, Sequence, useCurrentFrame, useVideoConfig} from 'remotion';
import {clamp, ease, Footer, Header, Scene} from './design';

const ExploreCard = ({title, detail, label, right = false, duration}: {
  title: string; detail: string; label: string; right?: boolean; duration: number;
}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const delay = right ? 0.15 : 0;
  return <div style={{position: 'absolute', left: right ? 990 : 230, top: 405, width: 700, height: 390,
    boxSizing: 'border-box', padding: '48px 50px', borderRadius: 28,
    background: 'linear-gradient(140deg, #ffffffeb, #edf7fbd9)', border: '1px solid #ffffff', boxShadow: '0 22px 60px #2b567714',
    opacity: interpolate(frame, [delay * fps, (delay + 0.45) * fps, (duration - 0.5) * fps, duration * fps - 1], [0, 1, 1, 0], clamp),
    translate: `0px ${interpolate(frame, [delay * fps, (delay + 0.65) * fps, (duration - 0.5) * fps, duration * fps - 1], [28, 0, 0, -16], {...clamp, easing: ease})}px`,
  }}>
    <div style={{fontSize: 22, color: '#4384aa', letterSpacing: 2}}>{label}</div>
    <Interactive.Div name={title} style={{fontSize: 43, marginTop: 30, color: '#203a4b'}}>{title}</Interactive.Div>
    <Interactive.Div name={`${title}说明`} style={{fontSize: 29, lineHeight: 1.8, color: '#647e8f', marginTop: 24}}>{detail}</Interactive.Div>
  </div>;
};

const ExplorePair = ({leftTitle, leftDetail, rightTitle, rightDetail, label, duration}: {
  leftTitle: string; leftDetail: string; rightTitle: string; rightDetail: string; label: string; duration: number;
}) => <AbsoluteFill>
  <ExploreCard title={leftTitle} detail={leftDetail} label={label} duration={duration} />
  <ExploreCard title={rightTitle} detail={rightDetail} label={label} duration={duration} right />
</AbsoluteFill>;

export const Explore = () => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <Scene duration={20}>
    <Header chapter="继续发现" title="更多内容，等待探索。" detail="还有一些细节，留给日常慢慢发现。" />
    <Sequence name="更多 · 本地歌词与任务栏适配" durationInFrames={5.4 * fps}>
      <ExplorePair label="歌词与桌面" duration={5.4}
        leftTitle="本地歌词匹配" leftDetail="QQ 音乐可索引本地 QRC 缓存，本地与在线歌词随时切换。"
        rightTitle="竖向与小任务栏适配" rightDetail="竖向任务栏自动切换竖排歌词；小任务栏自动调整显示，恢复正常大小后还原偏好。" />
    </Sequence>
    <Sequence name="更多 · 性能与自启动" from={5 * fps} durationInFrames={5.4 * fps}>
      <ExplorePair label="轻一点，省心一点" duration={5.4}
        leftTitle="四档性能模式" leftDetail="正常、低渲染、完全停止、极简，按使用需要选择。"
        rightTitle="开机自启动" rightDetail="无需管理员权限，登录 Windows 后自动运行。" />
    </Sequence>
    <Sequence name="更多 · 资源监视与日志" from={10 * fps} durationInFrames={5.4 * fps}>
      <ExplorePair label="信息，抬眼可见" duration={5.4}
        leftTitle="任务栏资源监视" leftDetail="查看 CPU、内存与可选的网络、GPU、CPU 频率指标，滚轮翻页。"
        rightTitle="运行日志面板" rightDetail="查看播放状态、歌词来源、封面加载与资源占用。" />
    </Sequence>
    <Sequence name="更多 · 宠物与动态背景" from={15 * fps} durationInFrames={5 * fps}>
      <ExplorePair label="给桌面一点趣味" duration={5}
        leftTitle="伴听宠物" leftDetail="播放时伴听、暂停时休息；Dock 与沉浸模式中，空闲时还能漫游。"
        rightTitle="动态背景" rightDetail="落叶、星星、二进制与流光粒子，还可叠加专辑色进度背景。" />
    </Sequence>
    <div style={{position: 'absolute', top: 885, width: '100%', textAlign: 'center', color: '#7290a1', fontSize: 26,
      opacity: interpolate(frame, [0, fps], [0, 1], {...clamp, easing: Easing.out(Easing.cubic)}),
    }}>打开设置，发现属于自己的使用方式。</div>
    <Footer text="更多功能与使用说明 · www.taskbar-lyrics.top" />
  </Scene>;
};
