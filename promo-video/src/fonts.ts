import {cancelRender, continueRender, delayRender} from 'remotion';

// 显式加载本机安装的仓耳今楷 01 W04；字体就绪后才允许预览和帧渲染。
if (typeof document !== 'undefined') {
  const handle = delayRender('加载仓耳今楷');
  const face = new FontFace('TsangerJinKai01', 'local("TsangerJinKai01-W04")', {
    weight: '400',
    style: 'normal',
    display: 'block',
  });
  face.load().then((loaded) => {
    document.fonts.add(loaded);
    continueRender(handle);
  }).catch((error: unknown) => {
    cancelRender(error instanceof Error ? error : new Error(String(error)));
  });
}
