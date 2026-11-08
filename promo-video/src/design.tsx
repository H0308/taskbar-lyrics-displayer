import type {CSSProperties, ReactNode} from 'react';
import {AbsoluteFill, CanvasImage, Easing, Interactive, interpolate, useCurrentFrame, useVideoConfig} from 'remotion';

export const font = '"TsangerJinKai01"';
export const ease = Easing.bezier(0.16, 1, 0.3, 1);
export const clamp = {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'} as const;

export const Backdrop = ({dark = false, tint = '#d8effd'}: {dark?: boolean; tint?: string}) => (
  <AbsoluteFill style={{
    background: dark ? '#111a26' : '#f3f7fb',
    backgroundImage: dark
      ? 'radial-gradient(ellipse at 80% 20%, #18384b 0%, transparent 65%)'
      : `radial-gradient(ellipse at 65% 45%, ${tint} 0%, transparent 65%), radial-gradient(ellipse at 4% 94%, #e3e9fa 0%, transparent 60%)`,
  }} />
);

export const Scene = ({children, duration, dark = false, tint}: {children: ReactNode; duration: number; dark?: boolean; tint?: string}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <AbsoluteFill style={{fontFamily: font, color: dark ? '#f1f6fa' : '#172635'}}>
    <Backdrop dark={dark} tint={tint} />
    <AbsoluteFill style={{
      opacity: interpolate(frame, [0, 0.3 * fps, (duration - 0.25) * fps, duration * fps - 1], [0, 1, 1, 0], clamp),
    }}>{children}</AbsoluteFill>
  </AbsoluteFill>;
};

export const Header = ({chapter, title, detail, dark = false}: {chapter: string; title: string; detail?: string; dark?: boolean}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <div style={{position: 'absolute', top: 100, left: 120, right: 120, textAlign: 'center'}}>
    <Interactive.Div name="章节" style={{fontSize: 22, letterSpacing: 3, color: dark ? '#8dc8f0' : '#4384aa', fontWeight: 400}}>{chapter}</Interactive.Div>
    <Interactive.Div name="主标题" style={{fontSize: 68, fontWeight: 500, letterSpacing: -1, lineHeight: 1.3, marginTop: 20,
      opacity: interpolate(frame, [4, 0.8 * fps], [0, 1], clamp),
      translate: interpolate(frame, [4, fps], ['0px 18px', '0px 0px'], {...clamp, easing: ease}),
    }}>{title}</Interactive.Div>
    {detail ? <Interactive.Div name="说明" style={{fontSize: 28, color: dark ? '#a8bacb' : '#647786', marginTop: 20, lineHeight: 1.6}}>{detail}</Interactive.Div> : null}
  </div>;
};

export const Footer = ({text, dark = false}: {text: string; dark?: boolean}) => (
  <div style={{position: 'absolute', left: 120, right: 120, bottom: 55, display: 'flex', justifyContent: 'flex-end', fontSize: 20, color: dark ? '#91a7b9' : '#718493'}}>
    <span>{text}</span>
  </div>
);

export const RecordingNotice = () => (
  <div style={{position: 'absolute', left: 120, bottom: 55, fontSize: 18, color: '#718493'}}>
    录屏效果仅供参考，请以实际产品效果为准
  </div>
);

export const Surface = ({children, style, dark = false}: {children: ReactNode; style?: CSSProperties; dark?: boolean}) => (
  <div style={{borderRadius: 8, boxShadow: dark ? '0 10px 28px #0002' : '0 10px 28px #24415d12', overflow: 'hidden', ...style}}>{children}</div>
);

export const Tag = ({children, dark = false}: {children: ReactNode; dark?: boolean}) => (
  <span style={{display: 'inline-flex', alignItems: 'center', gap: 10, border: `1px solid ${dark ? '#4e6276' : '#ccdee9'}`, color: dark ? '#c2d7e7' : '#4b6c82', borderRadius: 30, padding: '12px 24px', fontSize: 25}}>{children}</span>
);

export const Strip = ({src, width = 1160}: {src: string; width?: number}) => (
  <CanvasImage src={src} style={{width, height: 'auto', display: 'block', borderRadius: 8, boxShadow: '0 12px 40px #17364a18'}} />
);
