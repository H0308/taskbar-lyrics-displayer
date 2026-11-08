import {Composition} from 'remotion';
import {Promo} from './Promo';
import {auxiliaryLyricsDurationInSeconds} from './assets';

const durationInSeconds = 195 + auxiliaryLyricsDurationInSeconds - 6;

export const RemotionRoot = () => (
  <>
    <Composition id="TaskbarLyrics-Promo-V1" component={Promo} durationInFrames={Math.round(durationInSeconds * 30)} fps={30} width={1920} height={1080} />
    <Composition id="TaskbarLyrics-Promo-60fps" component={Promo} durationInFrames={Math.round(durationInSeconds * 60)} fps={60} width={1920} height={1080} />
  </>
);
