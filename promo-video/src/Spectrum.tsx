import {useWindowedAudioData, visualizeAudio} from '@remotion/media-utils';
import {Easing, interpolate, useCurrentFrame, useVideoConfig} from 'remotion';
import {music} from './assets';
import {clamp} from './design';

const labels = ['默认', '柱状图', '梦幻波浪', '分段电平', '底缘频谱带'];

// 按产品设置示意图保留：12 频段、5:3 柱宽间距、三层填充波面、七格电平及底缘窄带。
export const Spectrum = ({musicStartSeconds}: {musicStartSeconds: number}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const musicFrame = frame + musicStartSeconds * fps;
  const {audioData, dataOffsetInSeconds} = useWindowedAudioData({src: music, frame: musicFrame, fps, windowInSeconds: 30});
  const frequencies = audioData ? visualizeAudio({audioData, fps, frame: musicFrame, dataOffsetInSeconds, numberOfSamples: 256, optimizeFor: 'speed'}) : [];
  const levels = Array.from({length: 12}, (_, i) => {
    const bin = Math.round(2 * Math.pow(1.35, i));
    const value = ((frequencies[bin] ?? 0) + (frequencies[bin + 1] ?? 0)) / 2;
    return Math.max(0.07, Math.min(0.95, Math.log10(1 + value * 110) / 1.5));
  });
  const active = Math.min(4, Math.floor(frame / (3 * fps)));
  const visibility = interpolate(frame, [0, 0.65 * fps, 14.5 * fps, 15 * fps - 1], [0, 1, 1, 0], {...clamp, easing: Easing.inOut(Easing.cubic)});
  const w = 1080;
  const h = 250;
  const barWidth = w / (12 * 5 + 11 * 3) * 5;
  const step = barWidth * 1.6;
  const bass = levels.reduce((sum, value) => sum + value, 0) / 12;
  const wave = (layer: number) => {
    const points = Array.from({length: 100}, (_, i) => {
      const x = i / 99 * w;
      const profile = 0.18 + 0.82 * (0.5 + 0.5 * Math.sin(i / 99 * Math.PI * 2.24 + frame / fps * 30 / 28 + layer * 0.7));
      return `${i === 0 ? 'M' : 'L'} ${x} ${h - 12 - profile * (75 + bass * 130) * (0.6 + layer * 0.2)}`;
    }).join(' ');
    return `${points} L ${w} ${h} L 0 ${h} Z`;
  };
  return <>
    <div style={{position: 'absolute', top: 450, left: 420, width: 1080, height: 250, opacity: visibility, scale: 0.97 + visibility * 0.03, translate: `0px ${16 * (1 - visibility)}px`}}>
      <svg width={w} height={h} viewBox={`0 0 ${w} ${h}`} style={{overflow: 'visible'}}>
        <defs><linearGradient id="spectrum-fill" x1="0" y1="1" x2="0" y2="0"><stop offset="0%" stopColor="#1a719a" /><stop offset="100%" stopColor="#83c9ea" /></linearGradient></defs>
        {active === 2 ? [0, 1, 2].map(layer => <path key={layer} d={wave(layer)} fill="#328fbe" opacity={0.2 + layer * 0.12} />) : active === 4 ? <>
          <text x="0" y="80" fontSize="36" fill="#334e63">让音乐，留在任务栏</text>
          <text x="0" y="126" fontSize="24" fill="#8ba0af">底缘展开，轻盈呈现</text>
          {Array.from({length: 48}, (_, i) => <rect key={i} x={i * w / 48} y={h - (8 + levels[i % 12] * 43)} width={w / 72} height={8 + levels[i % 12] * 43} rx="2" fill="url(#spectrum-fill)" />)}
        </> : levels.map((level, i) => {
          const x = i * step;
          const height = 28 + level * 210;
          if (active === 3) return <g key={i}>
            {Array.from({length: 7}, (_, cell) => <rect key={cell} x={x} y={h - cell * 33 - 20} width={barWidth} height="20" rx="3" fill="#2d8ab7" opacity={cell < Math.ceil(level * 7) ? 0.85 : 0.1} />)}
            <rect x={x} y={h - Math.min(7, Math.ceil(level * 7) + 1) * 33} width={barWidth} height="3" fill="#267da8" />
          </g>;
          return <rect key={i} x={x} y={active === 0 ? (h - height) / 2 : h - height} width={barWidth} height={height} rx={barWidth / 2} fill="url(#spectrum-fill)" />;
        })}
      </svg>
    </div>
    <div style={{position: 'absolute', top: 800, left: 240, right: 240, display: 'flex', justifyContent: 'center', gap: 18, opacity: visibility}}>
      {labels.map((label, i) => <div key={label} style={{padding: '14px 27px', borderRadius: 30, fontSize: 26, color: i === active ? 'white' : '#738896', background: i === active ? '#267fae' : '#e7eff5'}}>{label}</div>)}
    </div>
    <div style={{position: 'absolute', bottom: 67, right: 120, fontSize: 24, color: '#5a7385'}}>频谱效果为动画示意，实际显示以产品为准。</div>
  </>;
};
