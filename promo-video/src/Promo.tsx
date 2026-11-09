import {Audio} from '@remotion/media';
import {AbsoluteFill, interpolate, Sequence, Track, useCurrentFrame, useVideoConfig} from 'remotion';
import {auxiliaryLyricsDurationInSeconds, music} from './assets';
import {clamp} from './design';
import {Closing, Effects, Idle, Languages, MediaCard, Modes, OpeningLyrics, Positioning, SpectrumScene, Themes} from './Scenes';
import {About, ManualSearch, SettingsShowcase} from './ExtraScenes';
import {Explore} from './Explore';

const Soundtrack = () => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <Audio name="背景音乐 · digital-lemonade" src={music} durationInFrames={180 * fps}
    volume={interpolate(frame, [0, 1.2 * fps, 177 * fps, 180 * fps - 1], [0, 0.85, 0.85, 0], clamp)} />;
};

const SoundtrackContinuation = ({durationInFrames}: {durationInFrames: number}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <Audio name="同一首背景音乐 · 交叠续接" src={music} durationInFrames={durationInFrames}
    volume={interpolate(frame, [0, 3 * fps, durationInFrames - 5 * fps, durationInFrames - 1], [0, 0.85, 0.85, 0], clamp)} />;
};

export const Promo = () => {
  const {fps} = useVideoConfig();
  const auxiliaryLyricsFrames = Math.round(auxiliaryLyricsDurationInSeconds * fps);
  const auxiliaryExtraFrames = auxiliaryLyricsFrames - 6 * fps;
  return <AbsoluteFill
    style={{
      background: '#f3f7fb',
      scale: 1.003
    }}
  >
    <Track name="画面与字幕">
      <Sequence name="桌面聚焦 · 同一段逐字实录持续播放" durationInFrames={20 * fps}><OpeningLyrics /></Sequence>
      <Sequence name="辅助歌词 · 翻译与罗马音同步实录" from={20 * fps} durationInFrames={auxiliaryLyricsFrames}><Languages /></Sequence>
      <Sequence name="手动搜索 · 歌词候选与预览" from={26 * fps + auxiliaryExtraFrames} durationInFrames={15 * fps}><ManualSearch /></Sequence>
      <Sequence name="歌词特效 · 星尘与笔触" from={41 * fps + auxiliaryExtraFrames} durationInFrames={14 * fps}><Effects /></Sequence>
      <Sequence name="频谱 · 五种样式示意" from={55 * fps + auxiliaryExtraFrames} durationInFrames={15 * fps}><SpectrumScene musicStartSeconds={55 + auxiliaryExtraFrames / fps} /></Sequence>
      <Sequence name="外观 · 深浅模式" from={70 * fps + auxiliaryExtraFrames} durationInFrames={8 * fps}><Themes /></Sequence>
      <Sequence name="布局 · 内嵌、沉浸避让、Dock" from={78 * fps + auxiliaryExtraFrames} durationInFrames={16 * fps}><Modes /></Sequence>
      <Sequence name="定位 · 拖动与避让" from={94 * fps + auxiliaryExtraFrames} durationInFrames={12 * fps} premountFor={2 * fps}><Positioning /></Sequence>
      <Sequence name="媒体卡片 · 完整录屏" from={106 * fps + auxiliaryExtraFrames} durationInFrames={18 * fps}><MediaCard /></Sequence>
      <Sequence name="空闲 · 一言与工具" from={124 * fps + auxiliaryExtraFrames} durationInFrames={10 * fps}><Idle /></Sequence>
      <Sequence name="设置 · 七组完整界面" from={134 * fps + auxiliaryExtraFrames} durationInFrames={19 * fps}><SettingsShowcase /></Sequence>
      <Sequence name="关于 · 多播放器支持与更新" from={153 * fps + auxiliaryExtraFrames} durationInFrames={6 * fps}><About /></Sequence>
      <Sequence name="更多内容 · 官网功能卡片" from={159 * fps + auxiliaryExtraFrames} durationInFrames={20 * fps}><Explore /></Sequence>
      <Sequence name="片尾 · 项目介绍与下载入口" from={179 * fps + auxiliaryExtraFrames} durationInFrames={16 * fps}><Closing /></Sequence>
    </Track>
    <Track name="背景音乐"><Soundtrack /></Track>
    <Track name="同曲续接"><Sequence name="背景音乐平滑续接" from={177 * fps} durationInFrames={18 * fps + auxiliaryExtraFrames}><SoundtrackContinuation durationInFrames={18 * fps + auxiliaryExtraFrames} /></Sequence></Track>
  </AbsoluteFill>;
};
