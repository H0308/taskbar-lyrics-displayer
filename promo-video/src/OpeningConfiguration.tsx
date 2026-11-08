import {Interactive, interpolate, useCurrentFrame, useVideoConfig} from 'remotion';
import {clamp, ease} from './design';

export const OpeningConfiguration = () => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <div style={{position: 'absolute', left: 120, right: 120, top: 320, height: 255,
    boxSizing: 'border-box', padding: '20px 28px', borderRadius: 22,
    background: 'linear-gradient(130deg, #ffffffdb, #eef7fbc7)', border: '1px solid #ffffff', boxShadow: '0 12px 35px #2854720c',
    opacity: interpolate(frame, [0, 0.6 * fps], [0, 1], clamp),
    translate: `0px ${interpolate(frame, [0, 0.6 * fps], [16, 0], {...clamp, easing: ease})}px`,
  }}>
    <Interactive.Div name="当前演示配置标题" style={{fontSize: 22, color: '#728a9a', marginBottom: 16}}>当前演示已开启以下功能与效果</Interactive.Div>
    <div style={{display: 'grid', gridTemplateColumns: '1fr 1.15fr 0.75fr 1.65fr', gap: 24}}>
      <div>
        <div style={{fontSize: 26, color: '#3285ac', marginBottom: 10}}>歌曲信息</div>
        <Interactive.Div name="演示配置 · 歌曲信息" style={{fontSize: 24, lineHeight: 1.65, color: '#536f80'}}>
          歌曲信息 · 平台图标<br />专辑封面：黑胶唱片
        </Interactive.Div>
      </div>
      <div>
        <div style={{fontSize: 26, color: '#3285ac', marginBottom: 10}}>歌词与跟唱</div>
        <Interactive.Div name="演示配置 · 歌词与跟唱" style={{fontSize: 24, lineHeight: 1.65, color: '#536f80'}}>
          双行歌词 · 已播放色跟随专辑<br />跟唱流光 · 字间涟漪<br />逐字浮波
        </Interactive.Div>
      </div>
      <div>
        <div style={{fontSize: 26, color: '#3285ac', marginBottom: 10}}>资源监视</div>
        <Interactive.Div name="演示配置 · 资源监视" style={{fontSize: 24, lineHeight: 1.65, color: '#536f80'}}>
          内嵌模式<br />显示资源监视
        </Interactive.Div>
      </div>
      <div>
        <div style={{fontSize: 26, color: '#3285ac', marginBottom: 10}}>音频频谱</div>
        <Interactive.Div name="演示配置 · 音频频谱" style={{fontSize: 24, lineHeight: 1.65, color: '#536f80'}}>
          柱状图样式 · 跟随专辑主题色<br />柔光辉晕 · 衰减残影 · 峰值散粒<br />流动光泽 · 频谱渐变色
        </Interactive.Div>
      </div>
    </div>
  </div>;
};
