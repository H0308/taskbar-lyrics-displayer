import {Video} from '@remotion/media';
import {AbsoluteFill, CanvasImage, Easing, interpolate, Sequence, useCurrentFrame, useVideoConfig} from 'remotion';
import {recording, screenshot} from './assets';
import {clamp, Footer, Header, RecordingNotice, Scene, Surface} from './design';
import {CameraFrame} from './camera';

export const ManualSearch = () => {
  const {fps} = useVideoConfig();
  return <Scene duration={15}>
    <Header chapter="手动搜索歌词" title="为这一首，选对歌词。" />
    <CameraFrame width={1440} height={640} sourceWidth={1152} sourceHeight={840} focusX={650} focusY={470} zoom={1.55} start={2} end={4} cleanEdge style={{position: 'absolute', left: 240, top: 285}}>
        <Video name="手动搜索歌词实录" src={recording('手动搜索歌词')} muted volume={0} trimBefore={2 * fps} durationInFrames={15 * fps} style={{width: '100%', height: 'auto', display: 'block'}} />
    </CameraFrame>
    <div style={{position: 'absolute', top: 950, width: '100%', textAlign: 'center', fontSize: 28, color: '#6b8190'}}>查看候选，预览逐字歌词与辅助歌词。</div>
    <RecordingNotice />
  </Scene>;
};

const SettingsShot = ({image, label, detail, hasNext = true}: {image: string; label: string; detail: string; hasNext?: boolean}) => {
  const frame = useCurrentFrame();
  const {fps, durationInFrames} = useVideoConfig();
  // 图片保留交叠，文字在下一组开始前退场，避免两条描述叠在同一位置。
  const textEnd = durationInFrames - (hasNext ? 0.4 * fps : 1);
  const textOpacity = interpolate(frame, [0, 0.25 * fps, textEnd - 0.25 * fps, textEnd], [0, 1, 1, 0], {...clamp, easing: Easing.inOut(Easing.cubic)});
  return <AbsoluteFill>
    <div style={{position: 'absolute', left: 240, top: 265, right: 240, display: 'flex', justifyContent: 'space-between', alignItems: 'center',
      opacity: textOpacity,
      translate: `0px ${interpolate(frame, [0, 0.25 * fps, textEnd - 0.25 * fps, textEnd], [10, 0, 0, -8], {...clamp, easing: Easing.inOut(Easing.cubic)})}px`,
    }}>
      <div style={{fontSize: 28, color: '#287fa9'}}>{label}</div>
      <div style={{fontSize: 24, color: '#6b8190'}}>{detail}</div>
    </div>
    <Surface style={{position: 'absolute', left: 550, top: 325, width: 820, borderRadius: 14,
      opacity: interpolate(frame, [0, 0.3 * fps], [0, 1], {...clamp, easing: Easing.inOut(Easing.cubic)}),
    }}>
      <div style={{margin: -3}}><CanvasImage src={screenshot(image)} style={{width: '100%', height: 'auto', display: 'block'}} /></div>
    </Surface>
  </AbsoluteFill>;
};

export const SettingsShowcase = () => {
  const {fps} = useVideoConfig();
  return <Scene duration={19}>
    <Header chapter="设置与个性化" title="把细节，调整成喜欢的样子。" />
    <Sequence name="显示设置 · 主题字体与封面" durationInFrames={3.4 * fps} premountFor={fps}><SettingsShot image="设置（显示）1" label="主题、字体与封面" detail="从深浅主题到字体与专辑封面。" /></Sequence>
    <Sequence name="显示设置 · 背景与交互" from={3 * fps} durationInFrames={3.4 * fps} premountFor={fps}><SettingsShot image="设置（显示）2" label="背景与菜单交互" detail="选择背景效果，保留顺手的操作方式。" /></Sequence>
    <Sequence name="展示模式设置" from={6 * fps} durationInFrames={3.4 * fps} premountFor={fps}><SettingsShot image="设置（展示模式）" label="任务栏展示模式" detail="内嵌、沉浸与 Dock，按习惯选择。" /></Sequence>
    <Sequence name="频谱设置" from={9 * fps} durationInFrames={3.4 * fps} premountFor={fps}><SettingsShot image="设置（频谱）" label="频谱样式与颜色" detail="在真实设置界面中选择喜欢的样式。" /></Sequence>
    <Sequence name="歌词设置" from={12 * fps} durationInFrames={2.9 * fps} premountFor={fps}><SettingsShot image="设置（歌词）" label="歌词排版与特效" detail="双行、对齐、颜色与跟唱效果。" /></Sequence>
    <Sequence name="切歌弹窗设置" from={14.5 * fps} durationInFrames={2.9 * fps} premountFor={fps}><SettingsShot image="设置（切歌弹窗）" label="切歌弹窗" detail="调整显示时长、位置与全屏时的行为。" /></Sequence>
    <Sequence name="每日一言与更多设置" from={17 * fps} durationInFrames={2 * fps} premountFor={fps}><SettingsShot image="设置（每日一言与更多）" label="每日一言与快捷启动" detail="安排空闲内容和常用应用。" hasNext={false} /></Sequence>
    <Footer text="真实设置界面" />
  </Scene>;
};

export const About = () => <Scene duration={6}>
  <Header chapter="关于与更新" title="每一次更新，都看得见。" />
  <Surface style={{position: 'absolute', left: 720, top: 305, width: 480, borderRadius: 14}}>
    <div style={{margin: -3}}><CanvasImage src={screenshot('关于窗口')} style={{width: '100%', height: 'auto', display: 'block'}} /></div>
  </Surface>
  <Footer text="版本信息 / 更新日志 / 更新来源" />
</Scene>;
