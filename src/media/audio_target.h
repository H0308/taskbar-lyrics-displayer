#pragma once

#include <string>
#include <vector>

// 音频会话与进程级回环捕获的匹配目标：
// 桌面应用按可执行文件名匹配，打包应用（UWP/MSIX）同时可按 PackageFamilyName 匹配。
struct AudioSessionTarget {
    std::vector<std::wstring> processNames;
    std::wstring packageFamilyName;

    bool empty() const noexcept {
        return processNames.empty() && packageFamilyName.empty();
    }

    bool operator==(const AudioSessionTarget&) const = default;
};
