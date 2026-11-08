import {staticFile} from 'remotion';

export const screenshot = (name: string) => staticFile(`screenshots/${name}.png`);
export const recording = (name: string) => staticFile(`screenrecords/${name}.mp4`);
export const music = staticFile('music/digital-lemonade.mp3');
// 以较短的翻译录屏为准；用秒记录，避免导出帧率改变时缩短素材时长。
export const auxiliaryLyricsDurationInSeconds = 292 / 30;
export const lyricRecording = recording('逐字高亮、歌词转场和双行歌词（默认歌词开启了逐字浮波、跟唱流光、字间涟漪效果，频谱选择柱状图，开启除背景频谱外的所有特效）');
