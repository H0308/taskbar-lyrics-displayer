// 图片加载完成再替换，快速切换时只接收最后一次选择。
const screenshotButtons = Array.from(document.querySelectorAll('[data-screen]'));
const screenshotLink = document.getElementById('screenshotLink');
const screenshotImage = document.getElementById('screenshotImage');
const screenshotTitle = document.getElementById('screenshotTitle');
const screenshotDescription = document.getElementById('screenshotDescription');
const taskbarDetail = document.getElementById('taskbarDetail');
let screenshotRequest = 0;

for (const button of screenshotButtons) {
  button.addEventListener('click', async () => {
    const request = ++screenshotRequest;
    const { screen, title, description, detail } = button.dataset;
    const src = `assets/media/${screen}`;
    const nextImage = new Image();
    nextImage.src = src;
    screenshotLink.setAttribute('aria-busy', 'true');
    try {
      await nextImage.decode();
      if (request !== screenshotRequest) return;
      screenshotLink.classList.remove('is-changing');
      screenshotImage.src = src;
      screenshotImage.alt = `${title}：${description}`;
      screenshotImage.width = nextImage.naturalWidth;
      screenshotImage.height = nextImage.naturalHeight;
      screenshotLink.href = src;
      screenshotLink.ariaLabel = `放大查看${title}截图`;
      screenshotLink.dataset.window = String(nextImage.naturalWidth / nextImage.naturalHeight < 1.6);
      screenshotTitle.textContent = title;
      screenshotDescription.textContent = description;
      taskbarDetail.hidden = !detail;
      for (const item of screenshotButtons) item.setAttribute('aria-pressed', String(item === button));
      // 样式刷新只用于重新触发这次用户选择的淡入。
      void screenshotLink.offsetWidth;
      screenshotLink.classList.add('is-changing');
    } catch {
      if (request === screenshotRequest) screenshotDescription.textContent = '截图加载失败，请重新选择。';
    } finally {
      if (request === screenshotRequest) screenshotLink.removeAttribute('aria-busy');
    }
  });
}

const screenshotDialog = document.getElementById('screenshotDialog');
screenshotLink.addEventListener('click', event => {
  event.preventDefault();
  document.getElementById('screenshotFullImage').src = screenshotImage.src;
  document.getElementById('screenshotFullImage').alt = screenshotImage.alt;
  document.getElementById('screenshotDialogTitle').textContent = screenshotTitle.textContent;
  document.getElementById('screenshotOriginal').href = screenshotImage.src;
  screenshotDialog.showModal();
});
screenshotDialog.addEventListener('click', event => {
  if (event.target !== screenshotDialog) return;
  const bounds = screenshotDialog.getBoundingClientRect();
  if (event.clientX < bounds.left || event.clientX > bounds.right ||
      event.clientY < bounds.top || event.clientY > bounds.bottom) screenshotDialog.close();
});

// 同一播放器承载所有录屏，预加载所选视频的一帧作封面，不自动播放。
const demos = {
  lyrics: { title: '逐字与双行歌词', description: '看唱词逐字亮起，长句自然滚动，下一句提前出现在熟悉的位置。', strip: true },
  translation: { title: '原文与翻译', description: '在有可用翻译的歌曲中，同时看见原文与中文释义。', strip: true },
  romanization: { title: '原文与罗马音', description: '切换罗马音辅助歌词，跟着读音一起唱。', strip: true },
  stardust: { title: '星尘跟唱', description: '星星沿着逐字进度移动，在唱词间留下细小的粒子拖尾。', strip: true },
  underline: { title: '笔触下划线', description: '用跟随唱词推进的笔触，标记音乐正在经过的位置。', strip: true },
  media: { title: '媒体控制', description: '展开媒体卡片，查看歌曲进度，使用播放与切歌控制。' },
  search: { title: '手动搜索歌词', description: '查看歌词候选与预览，切换翻译或罗马音，再应用合适的版本。' },
  positioning: { title: '拖动与自动避让', description: '拖动调整位置，观察任务栏内容随可用空间变化而避让。', strip: true },
  immersive: { title: '沉浸模式交互', description: '在沉浸模式中操作控制区域，离开后把空间还给任务栏。', strip: true },
};
const demoButtons = Array.from(document.querySelectorAll('[data-demo]'));
const video = document.getElementById('demoVideo');
const playButton = document.getElementById('demoPlay');
const progress = document.getElementById('demoProgress');
const demoTime = document.getElementById('demoTime');
const demoStatus = document.getElementById('demoStatus');
let selectedDemo = 'lyrics';
let demoRequest = 0;
let showingPreviewFrame = true;
video.parentElement.dataset.strip = 'true';
video.muted = true;

function formatDemoTime(seconds) {
  const value = Number.isFinite(seconds) ? Math.floor(seconds) : 0;
  return `${Math.floor(value / 60)}:${String(value % 60).padStart(2, '0')}`;
}

function updateDemoProgress() {
  const duration = video.duration;
  progress.disabled = !Number.isFinite(duration) || duration <= 0;
  progress.value = progress.disabled ? 0 : video.currentTime / duration * 100;
  progress.style.setProperty('--progress', `${progress.value}%`);
  const current = formatDemoTime(video.currentTime);
  const total = formatDemoTime(duration);
  demoTime.textContent = progress.disabled ? '0:00' : `${current} / ${total}`;
  progress.setAttribute('aria-valuetext', `${current}，共 ${total}`);
}

function updateDemoButton() {
  const state = video.ended ? 'ended' : video.paused ? 'paused' : 'playing';
  const label = state === 'ended' ? '重新播放' : state === 'playing' ? '暂停' : '播放示例';
  playButton.dataset.state = state;
  playButton.setAttribute('aria-label', label);
  playButton.title = label;
}

for (const button of demoButtons) {
  button.addEventListener('click', () => {
    if (selectedDemo === button.dataset.demo) return;
    ++demoRequest;
    selectedDemo = button.dataset.demo;
    const demo = demos[selectedDemo];
    video.pause();
    showingPreviewFrame = true;
    video.src = `assets/media/${selectedDemo}.mp4`;
    video.load();
    video.parentElement.dataset.strip = String(Boolean(demo.strip));
    video.ariaLabel = `${demo.title}功能录屏`;
    document.getElementById('demoTitle').textContent = demo.title;
    document.getElementById('demoDescription').textContent = demo.description;
    demoStatus.textContent = '';
    updateDemoButton();
    updateDemoProgress();
    for (const item of demoButtons) item.setAttribute('aria-pressed', String(item === button));
  });
}

playButton.addEventListener('click', async () => {
  if (!video.paused) { video.pause(); return; }
  const request = ++demoRequest;
  if (video.error) video.load();
  // 封面只用于预览，第一次播放仍从录屏起点开始。
  if ((showingPreviewFrame || video.ended) && video.readyState >= 1) video.currentTime = 0;
  showingPreviewFrame = false;
  video.muted = true;
  demoStatus.textContent = '正在加载录屏…';
  try {
    await video.play();
    if (request === demoRequest) demoStatus.textContent = '';
  } catch (error) {
    if (request === demoRequest && error.name !== 'AbortError') {
      demoStatus.textContent = '录屏未能播放，请点击播放重试。';
    }
  }
});
video.addEventListener('play', updateDemoButton);
video.addEventListener('pause', updateDemoButton);
video.addEventListener('ended', updateDemoButton);
video.addEventListener('playing', () => { demoStatus.textContent = ''; });
video.addEventListener('waiting', () => { demoStatus.textContent = '正在缓冲…'; });
video.addEventListener('error', () => { demoStatus.textContent = '录屏未能加载，请点击播放重试。'; });
function prepareDemoPreview() {
  // 轻微 seek 促使浏览器解码实际画面，避免仅加载元数据时出现黑色占位。
  if (showingPreviewFrame && video.paused && Number.isFinite(video.duration)) {
    video.currentTime = Math.min(0.1, video.duration / 2);
  }
  updateDemoProgress();
}
video.addEventListener('loadedmetadata', prepareDemoPreview);
if (video.readyState >= 1) prepareDemoPreview();
video.addEventListener('timeupdate', updateDemoProgress);
progress.addEventListener('input', () => {
  showingPreviewFrame = false;
  if (Number.isFinite(video.duration)) video.currentTime = Number(progress.value) / 100 * video.duration;
});
// 切换浏览器标签页或离开示例区域时停止播放，不在后台继续运行。
document.addEventListener('visibilitychange', () => { if (document.hidden) video.pause(); });
new IntersectionObserver(entries => {
  if (!entries[0].isIntersecting) video.pause();
}).observe(video);
