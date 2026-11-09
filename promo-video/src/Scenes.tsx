import {Video} from '@remotion/media';
import {AbsoluteFill, CanvasImage, Easing, Freeze, Interactive, interpolate, Sequence, staticFile, useCurrentFrame, useVideoConfig} from 'remotion';
import {auxiliaryLyricsDurationInSeconds, lyricRecording, recording, screenshot} from './assets';
import {clamp, ease, Footer, Header, RecordingNotice, Scene, Strip, Surface} from './design';
import {Spectrum} from './Spectrum';
import {CameraFrame, DesktopFocus} from './camera';
import {OpeningConfiguration} from './OpeningConfiguration';
import {productIntroduction, productName} from './product';

export const OpeningLyrics = () => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const cameraEase = Easing.bezier(0.77, 0, 0.175, 1);
  const focus = interpolate(frame, [2.2 * fps, 5.4 * fps], [0, 1], {...clamp, easing: cameraEase});
  // 坐标对应新桌面截图中的完整产品条；截图和录屏共用同一镜头，避免推近时漂移。
  const lyricX = 1476;
  const lyricY = 1382;
  const desktopScale = 1680 / 2560 * (1 + 0.8 * focus);
  const desktopX = 120 + Math.max(1680 - 2560 * desktopScale, Math.min(0, 840 - (1280 + 580 * focus) * desktopScale));
  const desktopY = 70 - 330 * focus + 945 - 1440 * desktopScale;
  const top = 70 + 540 * focus;
  const bottom = 1015 - 330 * focus;
  return <Scene duration={20}>
    <AbsoluteFill style={{clipPath: `inset(${top}px 120px ${1080 - bottom}px 120px round 16px)`}}>
      <div style={{position: 'absolute', width: 2560, height: 1440, transformOrigin: 'top left', transform: `translate(${desktopX}px, ${desktopY}px) scale(${desktopScale})`}}>
        <CanvasImage src={screenshot('日常桌面主视觉')} style={{width: 2560, height: 1440, display: 'block'}} />
        <Sequence name="聚焦后持续播放 · 唯一逐字实录" from={2.2 * fps} durationInFrames={17.8 * fps} premountFor={fps} style={{width: 2560, height: 1440}}>
          <div style={{position: 'absolute', left: lyricX, top: lyricY, width: 726, height: 54, overflow: 'hidden', borderRadius: 4,
            opacity: interpolate(frame, [2.2 * fps, 2.7 * fps], [0, 1], clamp),
          }}>
            <Video name="推近时开始播放 · 逐字高亮与双行歌词" src={lyricRecording} muted volume={0} trimBefore={8 * fps} durationInFrames={17.8 * fps} style={{width: 726, height: 54, display: 'block'}} />
          </div>
        </Sequence>
      </div>
    </AbsoluteFill>
    <div style={{position: 'absolute', inset: 0, background: 'linear-gradient(180deg, #f3f7fbd9, transparent 45%)', opacity: interpolate(frame, [2 * fps, 5 * fps], [1, 0], clamp)}} />
    <div style={{position: 'absolute', top: 210, left: 180,
      opacity: interpolate(frame, [0, 0.8 * fps, 2.2 * fps, 3.2 * fps], [0, 1, 1, 0], clamp),
    }}>
      <Interactive.Div name="开场标题" style={{fontSize: 86, fontWeight: 500, lineHeight: 1.3}}>音乐，就在任务栏。</Interactive.Div>
      <div style={{fontSize: 29, marginTop: 28, color: '#365b72'}}>把正在播放的一句，留在熟悉的位置。</div>
    </div>
    <div style={{position: 'absolute', top: 140, left: 120, right: 120, textAlign: 'center',
      opacity: interpolate(frame, [4.2 * fps, 5.5 * fps], [0, 1], clamp),
      translate: interpolate(frame, [4.2 * fps, 5.5 * fps], ['0px 16px', '0px 0px'], {...clamp, easing: ease}),
    }}>
      <Interactive.Div name="聚焦任务栏标题" style={{fontSize: 76, fontWeight: 500}}>让音乐，融入桌面。</Interactive.Div>
      <div style={{fontSize: 29, marginTop: 30, color: '#657b8b'}}>歌词、节奏与播放控制，抬眼可见。</div>
    </div>
    <Sequence name="当前演示配置 · 四类功能与效果" from={5.6 * fps} durationInFrames={14.4 * fps} layout="none">
      <OpeningConfiguration />
    </Sequence>
    <div style={{position: 'absolute', top: 810, left: 120, right: 120, display: 'flex', justifyContent: 'center', gap: 18, alignItems: 'center',
      opacity: interpolate(frame, [6 * fps, 7 * fps, 8.8 * fps, 9.6 * fps], [0, 1, 1, 0], clamp),
    }}>
      <CanvasImage src={staticFile('logo-light.png')} style={{width: 48, height: 48}} />
      <Interactive.Div name="产品名称" style={{fontSize: 29}}>{productName}</Interactive.Div>
    </div>
    <Sequence name="当前行说明" from={9.6 * fps} durationInFrames={4.4 * fps} layout="none">
      <div style={{position: 'absolute', top: 815, left: 120, right: 120, textAlign: 'center', fontSize: 30, color: '#6b8190'}}>
        逐字推进，长句自然滚动。
      </div>
    </Sequence>
    <Sequence name="双行歌词说明" from={14 * fps} durationInFrames={6 * fps} layout="none">
      <div style={{position: 'absolute', top: 815, left: 120, right: 120, textAlign: 'center', fontSize: 30, color: '#6b8190'}}>
        这一句，下一句。跟唱更从容。
      </div>
    </Sequence>
  </Scene>;
};

export const Languages = () => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const auxiliaryLyricsFrames = Math.round(auxiliaryLyricsDurationInSeconds * fps);
  const opacity = interpolate(frame, [0, 0.5 * fps, auxiliaryLyricsFrames - 0.4 * fps, auxiliaryLyricsFrames - 1], [0, 1, 1, 0], {...clamp, easing: Easing.inOut(Easing.cubic)});
  const translate = `0px ${interpolate(frame, [0, 0.6 * fps], [18, 0], {...clamp, easing: ease})}px`;
  return <Scene duration={auxiliaryLyricsFrames / fps}>
    <Header chapter="辅助歌词" title="读懂这一句。" detail="翻译或罗马音，按需选择。" />
      <div style={{position: 'absolute', left: 350, top: 430, width: 1220, opacity, translate}}>
        <div style={{fontSize: 27, color: '#4884a6', marginBottom: 30}}>原文 + 翻译</div>
        <Surface><Video name="辅助歌词翻译实录" src={recording('辅助歌词（翻译）')} muted volume={0} durationInFrames={auxiliaryLyricsFrames} style={{width: '100%', height: 'auto', display: 'block'}} /></Surface>
      </div>
      <div style={{position: 'absolute', left: 350, top: 650, width: 1220, opacity, translate}}>
        <div style={{fontSize: 27, color: '#4884a6', marginBottom: 30}}>原文 + 罗马音</div>
        <Surface><Video name="辅助歌词罗马音实录 · 与翻译等长" src={recording('辅助歌词（罗马音）')} muted volume={0} durationInFrames={auxiliaryLyricsFrames} style={{width: '100%', height: 'auto', display: 'block'}} /></Surface>
      </div>
    <div style={{position: 'absolute', left: 120, right: 120, top: 880, textAlign: 'center', fontSize: 29, color: '#6c8090'}}>不同的语言，同一个熟悉的位置。</div>
    <Footer text="翻译与罗马音可按需切换" />
  </Scene>;
};

const EffectShot = ({kind}: {kind: 'star' | 'underline'}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <AbsoluteFill style={{opacity: interpolate(frame, [0, 0.5 * fps], [0, 1], clamp)}}>
    <div style={{position: 'absolute', top: 440, left: 120, right: 120, textAlign: 'center', fontSize: 30, color: '#4384aa'}}>
      {kind === 'star' ? '星尘跟唱' : '笔触下划线'}
    </div>
    <Surface style={{position: 'absolute', left: 380, top: 610, width: 1160, display: 'flex'}}>
      {kind === 'star'
        ? <Video name="星尘跟唱实录" src={recording('星尘跟唱')} muted volume={0} trimBefore={2 * fps} durationInFrames={7 * fps} style={{width: 1160, borderRadius: 8}} />
        : <Video name="笔触下划线实录" src={recording('笔触下划线')} muted volume={0} trimBefore={2 * fps} durationInFrames={7 * fps} style={{width: 1160, borderRadius: 8}} />}
    </Surface>
  </AbsoluteFill>;
};

export const Effects = () => {
  const {fps} = useVideoConfig();
  return <Scene duration={14}>
    <Header chapter="歌词特效" title="每一句，都有自己的表情。" />
    <Sequence name="星尘跟唱" durationInFrames={7 * fps}><EffectShot kind="star" /></Sequence>
    <Sequence name="笔触下划线" from={7 * fps} durationInFrames={7 * fps}><EffectShot kind="underline" /></Sequence>
    <div style={{position: 'absolute', top: 815, left: 120, right: 120, textAlign: 'center', fontSize: 28, color: '#6b8190'}}>星尘 · 流光 · 笔触 · 涟漪 · 浮波</div>
    <Footer text="按喜好选择歌词特效" />
  </Scene>;
};

export const SpectrumScene = ({musicStartSeconds}: {musicStartSeconds: number}) => <Scene duration={15}>
  <Header chapter="音频频谱" title="把节奏，展开来看。" />
  <Spectrum musicStartSeconds={musicStartSeconds} />
</Scene>;

export const Themes = () => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const p = interpolate(frame, [3.6 * fps, 4.4 * fps], [0, 1], clamp);
  return <Scene duration={8}>
  <Header chapter="外观" title="深浅之间，都很合拍。" detail="歌词颜色、字体与背景，组合成喜欢的模样。" />
  <div style={{position: 'absolute', left: 350, top: 610, opacity: 1 - p}}>
    <Strip src={screenshot('内嵌（浅色模式）')} width={1220} />
  </div>
  <div style={{position: 'absolute', left: 350, top: 610, opacity: p}}>
    <Strip src={screenshot('内嵌（深色模式）')} width={1220} />
  </div>
  <div style={{position: 'absolute', top: 815, width: '100%', textAlign: 'center', fontSize: 29, color: '#6b8190'}}>{p < 0.5 ? '浅色，清透自然。' : '深色，安静陪伴。'}</div>
  <Footer text="任务栏内容与悬浮窗可分别设置主题" />
</Scene>;
};

const ModeShot = ({title, description, image, fadeOutAt}: {title: string; description: string; image: string; fadeOutAt?: number}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <AbsoluteFill style={{opacity: interpolate(frame, [0, 0.5 * fps], [0, 1], clamp)
    * (fadeOutAt === undefined ? 1 : interpolate(frame, [fadeOutAt * fps, (fadeOutAt + 0.5) * fps], [1, 0], {...clamp, easing: Easing.inOut(Easing.cubic)})),
  }}>
    <DesktopFocus src={screenshot(image)} edge={image.includes('在上') ? 'top' : 'bottom'} focusX={image === '日常桌面主视觉' ? 1860 : 1280} bandHeight={image.includes('在下') ? 80 : image.includes('在上') ? 40 : 75} zoom={image === '日常桌面主视觉' ? 1.8 : 1} />
    <div style={{position: 'absolute', top: 100, width: '100%', textAlign: 'center', opacity: interpolate(frame, [0.7 * fps, 1.5 * fps], [0, 1], clamp)}}>
      <div style={{fontSize: 22, color: '#4384aa'}}>展示模式</div>
      <div style={{fontSize: 68, marginTop: 20}}>{title}</div>
    </div>
    <div style={{position: 'absolute', top: 815, width: '100%', textAlign: 'center', fontSize: 29, color: '#6b8190', opacity: interpolate(frame, [1.2 * fps, 1.8 * fps], [0, 1], clamp)}}>{description}</div>
  </AbsoluteFill>;
};

export const Modes = () => {
  const {fps} = useVideoConfig();
  return <Scene duration={16}>
    <Sequence name="内嵌模式" durationInFrames={3.5 * fps}><ModeShot title="内嵌" description="融入现有任务栏，让熟悉的桌面多一句歌词。" image="日常桌面主视觉" fadeOutAt={3} /></Sequence>
    <Sequence name="沉浸全貌 → 竖向聚焦与自动让位" from={3 * fps} durationInFrames={7 * fps}><ImmersiveShot /></Sequence>
    <Sequence name="顶部 Dock" from={10 * fps} durationInFrames={3 * fps}><ModeShot title="顶部 Dock" description="独立的音乐栏，把歌词留在桌面上方。" image="Dock全貌（Dock在上）" /></Sequence>
    <Sequence name="底部 Dock" from={13 * fps} durationInFrames={3 * fps}><ModeShot title="底部 Dock" description="选择顶部或底部，按照自己的习惯摆放。" image="Dock全貌（Dock在下）" /></Sequence>
    <Footer text="内嵌 / 沉浸 / Dock" />
  </Scene>;
};

const ImmersiveShot = () => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <AbsoluteFill style={{opacity: interpolate(frame, [0, 0.5 * fps], [0, 1], {...clamp, easing: Easing.inOut(Easing.cubic)})}}>
    <DesktopFocus src={screenshot('沉浸模式全貌')} zoom={1} bandHeight={157.5}>
      <Sequence name="聚焦将结束时开始 · 沉浸避让实录" from={1.4 * fps} durationInFrames={5.5 * fps} premountFor={fps} style={{width: 1680, height: 945}}>
        <Video name="沉浸条淡出与恢复" src={recording('沉浸模式交互')} muted volume={0} durationInFrames={5.5 * fps} style={{position: 'absolute', bottom: 0, width: 1680, height: 157.5,
          opacity: interpolate(frame, [1.4 * fps, 1.8 * fps], [0, 1], clamp),
        }} />
      </Sequence>
    </DesktopFocus>
    <div style={{opacity: interpolate(frame, [fps, 1.8 * fps], [0, 1], clamp)}}>
      <Header chapter="沉浸模式 · 自动避让" title="需要任务栏时，自然让开。" />
      <div style={{position: 'absolute', top: 815, width: '100%', textAlign: 'center', fontSize: 29, color: '#6b8190'}}>鼠标进入相应区域，让出任务栏；离开后恢复。</div>
    </div>
  </AbsoluteFill>;
};

export const Positioning = () => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const entryFrames = Math.round(0.65 * fps);
  const playbackFrames = 12 * fps - entryFrames;
  return <Scene duration={12}>
    <Header chapter="桌面融合" title="换个位置，依然顺手。" detail="拖动换位 · 锚点吸附 · 空间避让" />
    <CameraFrame width={1680} height={160} sourceWidth={2560} sourceHeight={150} focusX={950} focusY={120} zoom={1.7} start={2} end={4} style={{position: 'absolute', left: 120, top: 540,
      opacity: interpolate(frame, [0, 0.65 * fps, 11.7 * fps, 12 * fps - 1], [0, 1, 1, 0], {...clamp, easing: Easing.inOut(Easing.cubic)}),
      translate: `0px ${interpolate(frame, [0, 0.65 * fps], [16, 0], {...clamp, easing: ease})}px`,
    }}>
      <Sequence name="切页淡入 · 保持录屏首帧" durationInFrames={entryFrames} layout="none">
        <Freeze frame={0}>
          <Video name="定位录屏首帧" src={recording('拖动换位与自动避让')} muted volume={0} style={{width: '100%', height: 'auto', display: 'block'}} />
        </Freeze>
      </Sequence>
      <Sequence name="淡入完成后 · 从头播放拖动与避让" from={entryFrames} durationInFrames={playbackFrames} premountFor={2 * fps} style={{width: '100%', height: '100%'}}>
        {/* 开头不在淡入期间消耗；将原有 15 秒素材完整安排进剩余展示时间。 */}
        <Video name="拖动与空间适应" src={recording('拖动换位与自动避让')} muted volume={0} playbackRate={15 * fps / playbackFrames} trimAfter={15 * fps} style={{width: '100%', height: 'auto', display: 'block'}} />
      </Sequence>
    </CameraFrame>
    <div style={{position: 'absolute', width: '100%', textAlign: 'center', top: 825, fontSize: 30, color: '#6b8190'}}>为音乐找到位置，也为任务栏保留空间。</div>
    <RecordingNotice />
  </Scene>;
};

export const MediaCard = () => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <Scene duration={18} tint="#e7dbf3">
    <Header chapter="媒体卡片" title="音乐控制，就在手边。" />
    <Surface style={{position: 'absolute', top: 320, left: 410, width: 1100, height: 1100 * 396 / 758, borderRadius: 24,
      opacity: interpolate(frame, [0.4 * fps, 1.1 * fps, 17.1 * fps, 17.6 * fps], [0, 1, 1, 0], {...clamp, easing: Easing.inOut(Easing.cubic)}),
      translate: `0px ${interpolate(frame, [0.4 * fps, 1.1 * fps], [24, 0], {...clamp, easing: ease})}px`,
    }}>
      <Sequence name="完整媒体卡片录屏 · 原速连续播放" from={0.4 * fps} durationInFrames={17.2 * fps} premountFor={fps}>
        <Video name="媒体卡片播放操作" src={recording('媒体卡片交互')} muted volume={0} style={{width: '100%', height: 'auto', display: 'block'}} />
      </Sequence>
    </Surface>
    <div style={{position: 'absolute', top: 920, width: '100%', textAlign: 'center', fontSize: 28, color: '#6b8190'}}>封面、进度与播放操作，在一张卡片中展开。</div>
    <RecordingNotice />
  </Scene>;
};

const IdleShot = ({image, title, description}: {image: string; title: string; description: string}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <AbsoluteFill style={{opacity: interpolate(frame, [0, 0.5 * fps], [0, 1], clamp)}}>
    <div style={{position: 'absolute', top: 850, width: '100%', textAlign: 'center'}}>
      <div style={{fontSize: 33}}>{title}</div>
      <div style={{fontSize: 26, color: '#6b8190', marginTop: 18}}>{description}</div>
    </div>
    <CanvasImage src={screenshot(image)} style={{position: 'absolute', left: 570, top: 340, width: 780, height: 'auto', borderRadius: 24, boxShadow: '0 30px 70px #21486926', translate: `0px ${interpolate(frame, [0, fps], [35, 0], {...clamp, easing: ease})}px`}} />
  </AbsoluteFill>;
};

export const Idle = () => {
  const {fps} = useVideoConfig();
  return <Scene duration={10}>
    <Header chapter="空闲时光" title="音乐之外，也有日常。" />
    <Sequence name="每日一言与快捷启动" durationInFrames={5 * fps}><IdleShot image="每日一言与快捷启动" title="读一句，打开常用应用。" description="每日一言与快捷启动，放在同一张轻盈卡片里。" /></Sequence>
    <Sequence name="今日任务" from={5 * fps} durationInFrames={5 * fps}><IdleShot image="每日一言与今日任务" title="今天的事，抬眼可见。" description="接入滴答清单，在卡片中查看今日任务。" /></Sequence>
    <Footer text="每日一言 / 快捷启动 / 今日任务" />
  </Scene>;
};

export const Closing = () => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <Scene duration={16}>
    <div style={{position: 'absolute', top: 95, left: 0, right: 0, textAlign: 'center'}}>
      <CanvasImage src={staticFile('logo-light.png')} style={{width: 82, height: 82}} />
      <Interactive.Div name="片尾标题" style={{fontSize: 68, fontWeight: 500, marginTop: 24, letterSpacing: -2}}>把喜欢的音乐，留在任务栏。</Interactive.Div>
      <Interactive.Div name="片尾产品名称" style={{fontSize: 30, color: '#657e90', marginTop: 20}}>{productName}</Interactive.Div>
    </div>
    <div style={{position: 'absolute', left: 235, right: 235, top: 440, fontSize: 30, lineHeight: 1.85, color: '#516c7e',
      opacity: interpolate(frame, [0.5 * fps, 1.3 * fps], [0, 1], clamp),
    }}>
      <Interactive.Div name="项目介绍">{productIntroduction}</Interactive.Div>
    </div>
    <div style={{position: 'absolute', top: 870, width: '100%', textAlign: 'center'}}>
      <Interactive.Div name="下载地址" style={{fontSize: 39, color: '#287fa9', fontWeight: 600}}>www.taskbar-lyrics.top</Interactive.Div>
      <div style={{fontSize: 24, marginTop: 20, color: '#788e9d'}}>了解更多 · 获取下载</div>
    </div>
    <Footer text="音乐 / digital-lemonade" />
  </Scene>;
};
