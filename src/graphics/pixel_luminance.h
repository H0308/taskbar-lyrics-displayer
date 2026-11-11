#pragma once

namespace graphics {
// 输入为紧密排列、每像素四字节的 BGRA 截图；保持窗口原有采样与线性化算法。
float backdropLuminance(const void* pixels, int width, int height);
// pixels 指向待采样区域左上角，rowStridePixels 为原始截图每行的像素数。
float backdropLuminance(const void* pixels, int width, int height, int rowStridePixels);
} // namespace graphics
