import type {CSSProperties, ReactNode} from 'react';
import {CanvasImage, Easing, interpolate, useCurrentFrame, useVideoConfig} from 'remotion';
import {clamp} from './design';

const cameraEase = Easing.bezier(0.77, 0, 0.175, 1);

// 镜头在素材内部移动，按真实尺寸约束边界，避免推近后露出空白或拉伸文字。
export const CameraFrame = ({children, sourceWidth, sourceHeight, width, height, focusX, focusY, zoom = 1.7, start = 0.5, end = 2, cleanEdge = false, style}: {
  children: ReactNode; sourceWidth: number; sourceHeight: number; width: number; height: number;
  focusX: number; focusY: number; zoom?: number; start?: number; end?: number; cleanEdge?: boolean; style?: CSSProperties;
}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const progress = interpolate(frame, [start * fps, end * fps], [0, 1], {...clamp, easing: cameraEase});
  const fit = Math.min(width / sourceWidth, height / sourceHeight);
  const scale = 1 + (zoom - 1) * progress;
  const cx = sourceWidth / 2 + (focusX - sourceWidth / 2) * progress;
  const cy = sourceHeight / 2 + (focusY - sourceHeight / 2) * progress;
  const imageWidth = sourceWidth * fit * scale;
  const imageHeight = sourceHeight * fit * scale;
  const x = imageWidth <= width ? (width - imageWidth) / 2 : Math.max(width - imageWidth, Math.min(0, width / 2 - cx * fit * scale));
  const y = imageHeight <= height ? (height - imageHeight) / 2 : Math.max(height - imageHeight, Math.min(0, height / 2 - cy * fit * scale));
  return <div style={{position: 'relative', width, height, overflow: 'hidden', borderRadius: 16, ...style}}>
    <div style={{position: 'absolute', width: sourceWidth * fit, height: sourceHeight * fit, transformOrigin: 'top left', scale, translate: `${x}px ${y}px`, borderRadius: 14, overflow: 'hidden'}}>
      <div style={cleanEdge ? {margin: -3} : undefined}>{children}</div>
    </div>
  </div>;
};

export const DesktopFocus = ({src, edge = 'bottom', start = 0.45, end = 1.75, focusX = 1280, bandHeight = 75, zoom = 1.8, children}: {
  src: string; edge?: 'top' | 'bottom'; start?: number; end?: number; focusX?: number; bandHeight?: number; zoom?: number; children?: ReactNode;
}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const p = interpolate(frame, [start * fps, end * fps], [0, 1], {...clamp, easing: cameraEase});
  return <div style={{position: 'absolute', left: 120, top: 70,
    translate: `0px ${p * (edge === 'top' ? 540 : -330)}px`,
    clipPath: edge === 'top' ? `inset(0px 0px ${(945 - bandHeight) * p}px 0px round 16px)` : `inset(${(945 - bandHeight) * p}px 0px 0px 0px round 16px)`,
  }}>
    <CameraFrame width={1680} height={945} sourceWidth={2560} sourceHeight={1440} focusX={focusX} focusY={edge === 'top' ? 40 : 1400} zoom={zoom} start={start} end={end}>
      <CanvasImage src={src} style={{width: '100%', height: 'auto', display: 'block'}} />
      {children}
    </CameraFrame>
  </div>;
};
