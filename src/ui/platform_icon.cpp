#include "platform_icon.h"
#include "media/player_source.h"

#include <objbase.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace {


std::wstring findProcessImagePath(const std::wstring& processName) {
    if (processName.empty())
        return {};

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring result;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName.c_str()) != 0)
                continue;

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
            if (QueryFullProcessImageNameW(process, 0, path, &length))
                result.assign(path, length);
            CloseHandle(process);
            if (!result.empty())
                break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::wstring fileNameFromPath(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash + 1 < path.size())
        return path.substr(slash + 1);
    return path;
}

const wchar_t* preferredWindowClassForProcess(const std::wstring& processName) {
    if (_wcsicmp(processName.c_str(), L"cloudmusic.exe") == 0)
        return L"OrpheusBrowserHost";
    return nullptr;
}

std::vector<std::wstring> sourceProcessCandidates(const std::wstring& source) {
    if (source.empty())
        return {};

    if (const auto* definition = player_source::find(source)) {
        const auto names = definition->processNames;
        // 原生进程会话直接取自身；桥接器及非进程标识按播放器进程顺序解析。
        if (!names.empty() && _wcsicmp(source.c_str(), names.front()) == 0)
            return {names.front()};
        return {names.begin(), names.end()};
    }

    return {fileNameFromPath(source)};
}

std::vector<DWORD> findProcessIds(const std::wstring& processName) {
    std::vector<DWORD> result;
    if (processName.empty())
        return result;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName.c_str()) == 0)
                result.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::wstring fullPath(const std::wstring& path) {
    if (path.empty())
        return {};
    wchar_t buffer[32768]{};
    const DWORD length = GetFullPathNameW(path.c_str(), _countof(buffer), buffer, nullptr);
    return length > 0 && length < _countof(buffer) ? std::wstring(buffer, length) : path;
}

std::vector<DWORD> findProcessIdsByImagePath(const std::wstring& targetPath) {
    std::vector<DWORD> result;
    const std::wstring target = fullPath(targetPath);
    if (target.empty())
        return result;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(_countof(path));
            const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
            CloseHandle(process);
            if (queried && _wcsicmp(path, target.c_str()) == 0)
                result.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool containsProcessId(const std::vector<DWORD>& processIds, DWORD processId) {
    for (DWORD candidate : processIds) {
        if (candidate == processId)
            return true;
    }
    return false;
}

bool hasWindowClassName(HWND window, const wchar_t* expectedClassName) {
    if (!expectedClassName || !*expectedClassName)
        return false;
    wchar_t className[256]{};
    const int length = GetClassNameW(window, className, static_cast<int>(_countof(className)));
    return length > 0 && _wcsicmp(className, expectedClassName) == 0;
}

bool hasNonTrivialWindowArea(HWND window) {
    if (IsIconic(window))
        return true;

    RECT rect{};
    if (GetWindowRect(window, &rect) && rect.right - rect.left > 1 &&
        rect.bottom - rect.top > 1)
        return true;

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(window, &placement)) {
        const RECT& normalRect = placement.rcNormalPosition;
        if (normalRect.right - normalRect.left > 1 && normalRect.bottom - normalRect.top > 1)
            return true;
    }
    return false;
}

struct WindowSearchContext {
    const std::vector<DWORD>* processIds = nullptr;
    const wchar_t* preferredClassName = nullptr;
    bool allowHiddenPreferredClass = false;
    HWND bestWindow = nullptr;
    int bestRank = 0;
};

BOOL CALLBACK findBestProcessWindow(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<WindowSearchContext*>(parameter);
    if (!context || !context->processIds)
        return FALSE;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!containsProcessId(*context->processIds, processId) ||
        GetWindow(window, GW_OWNER) != nullptr)
        return TRUE;

    const bool visible = IsWindowVisible(window) != FALSE;
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const bool toolWindow =
        (exStyle & WS_EX_TOOLWINDOW) != 0 && (exStyle & WS_EX_APPWINDOW) == 0;
    const bool noActivate = (exStyle & WS_EX_NOACTIVATE) != 0;
    const bool matchesPreferred = hasWindowClassName(window, context->preferredClassName);

    int rank = 0;
    if (matchesPreferred && visible && !toolWindow && !noActivate)
        rank = 5;
    else if (matchesPreferred && visible)
        rank = 4;
    else if (context->allowHiddenPreferredClass && matchesPreferred && !visible &&
             !toolWindow && !noActivate && hasNonTrivialWindowArea(window))
        rank = 3;
    else if (context->allowHiddenPreferredClass && matchesPreferred && !visible)
        rank = 2;
    else if (visible && !toolWindow && !noActivate && hasNonTrivialWindowArea(window))
        rank = 1;

    if (rank > context->bestRank) {
        context->bestWindow = window;
        context->bestRank = rank;
    }

    const int maxPossibleRank = context->preferredClassName ? 5 : 1;
    return context->bestRank >= maxPossibleRank ? FALSE : TRUE;
}

HWND findProcessWindow(const std::vector<DWORD>& processIds, const wchar_t* preferredClassName,
                       bool allowHiddenPreferredClass) {
    if (processIds.empty())
        return nullptr;
    WindowSearchContext context{&processIds, preferredClassName, allowHiddenPreferredClass,
                                nullptr, 0};
    EnumWindows(findBestProcessWindow, reinterpret_cast<LPARAM>(&context));
    return context.bestWindow;
}

bool activateWindow(HWND window) {
    if (!window || !IsWindow(window))
        return false;

    DWORD targetProcessId = 0;
    GetWindowThreadProcessId(window, &targetProcessId);
    if (targetProcessId != 0)
        AllowSetForegroundWindow(targetProcessId);

    const bool wasVisible = IsWindowVisible(window) != FALSE;
    if (IsIconic(window)) {
        ShowWindow(window, SW_RESTORE);
    } else if (!wasVisible) {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        const bool wasMaximized =
            IsZoomed(window) != FALSE ||
            (GetWindowPlacement(window, &placement) && placement.showCmd == SW_SHOWMAXIMIZED);
        ShowWindow(window, wasMaximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    } else {
        ShowWindow(window, SW_SHOW);
    }

    HWND focusTarget = window;
    HWND popup = GetLastActivePopup(window);
    if (popup && popup != window && IsWindowVisible(popup))
        focusTarget = popup;

    BringWindowToTop(focusTarget);
    SetForegroundWindow(focusTarget);
    return true;
}

bool activateExistingWindow(const std::vector<DWORD>& processIds,
                            const wchar_t* preferredClassName = nullptr,
                            bool allowHiddenPreferredClass = false) {
    HWND window = findProcessWindow(processIds, preferredClassName, allowHiddenPreferredClass);
    return activateWindow(window);
}

bool executePath(const std::wstring& path) {
    if (path.empty())
        return false;
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                                           SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool executeUri(const std::wstring& uri) {
    if (uri.empty())
        return false;
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr,
                                           SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool activateAppUserModelId(const std::wstring& appUserModelId) {
    if (appUserModelId.empty())
        return false;

    IApplicationActivationManager* manager = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                  CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(hr) || !manager)
        return false;

    DWORD processId = 0;
    hr = manager->ActivateApplication(appUserModelId.c_str(), nullptr, AO_NONE, &processId);
    manager->Release();
    return SUCCEEDED(hr);
}

std::wstring resolveSourceIconPath(const std::wstring& sourceAppUserModelId) {
    if (sourceAppUserModelId.empty())
        return {};

    for (const auto& candidate : sourceProcessCandidates(sourceAppUserModelId)) {
        const std::wstring path = findProcessImagePath(candidate);
        if (!path.empty())
            return path;
    }

    DWORD attributes = GetFileAttributesW(sourceAppUserModelId.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return sourceAppUserModelId;
    return {};
}

void clearIconBlackMatte(std::vector<BYTE>& pixels, UINT width, UINT height) {
    if (pixels.empty() || width == 0 || height == 0)
        return;

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<BYTE> cleared(pixelCount, 0);
    std::vector<size_t> pending;
    pending.reserve(pixelCount);

    auto nearBlack = [&](size_t index) {
        const BYTE* p = pixels.data() + index * 4;
        return p[3] != 0 && p[0] <= 48 && p[1] <= 48 && p[2] <= 48;
    };
    auto enqueue = [&](size_t index) {
        if (!cleared[index] && nearBlack(index)) {
            cleared[index] = 1;
            pending.push_back(index);
        }
    };

    for (UINT x = 0; x < width; ++x) {
        enqueue(x);
        enqueue(static_cast<size_t>(height - 1) * width + x);
    }
    for (UINT y = 0; y < height; ++y) {
        enqueue(static_cast<size_t>(y) * width);
        enqueue(static_cast<size_t>(y) * width + width - 1);
    }

    while (!pending.empty()) {
        const size_t index = pending.back();
        pending.pop_back();
        const UINT x = static_cast<UINT>(index % width);
        const UINT y = static_cast<UINT>(index / width);
        if (x > 0)
            enqueue(index - 1);
        if (x + 1 < width)
            enqueue(index + 1);
        if (y > 0)
            enqueue(index - width);
        if (y + 1 < height)
            enqueue(index + width);
    }

    for (size_t i = 0; i < pixelCount; ++i) {
        BYTE* p = pixels.data() + i * 4;
        if (cleared[i] || p[3] == 0)
            p[0] = p[1] = p[2] = p[3] = 0;
    }
}

bool readIconPixelsFromHicon(HICON icon, std::vector<BYTE>& pixels, UINT& width,
                             UINT& height) {
    if (!icon)
        return false;

    ICONINFO iconInfo{};
    if (!GetIconInfo(icon, &iconInfo))
        return false;

    HDC screenDc = GetDC(nullptr);
    bool ok = false;
    do {
        if (!screenDc || !iconInfo.hbmColor)
            break;

        BITMAP colorBitmap{};
        if (GetObjectW(iconInfo.hbmColor, sizeof(colorBitmap), &colorBitmap) !=
            sizeof(colorBitmap))
            break;
        if (colorBitmap.bmWidth <= 0 || colorBitmap.bmHeight <= 0)
            break;

        const UINT iconWidth = static_cast<UINT>(colorBitmap.bmWidth);
        const UINT iconHeight = static_cast<UINT>(colorBitmap.bmHeight);
        std::vector<BYTE> colorPixels(static_cast<size_t>(iconWidth) * iconHeight * 4);

        BITMAPINFO colorInfo{};
        colorInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        colorInfo.bmiHeader.biWidth = static_cast<LONG>(iconWidth);
        colorInfo.bmiHeader.biHeight = -static_cast<LONG>(iconHeight);
        colorInfo.bmiHeader.biPlanes = 1;
        colorInfo.bmiHeader.biBitCount = 32;
        colorInfo.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(screenDc, iconInfo.hbmColor, 0, iconHeight, colorPixels.data(),
                      &colorInfo, DIB_RGB_COLORS) != static_cast<int>(iconHeight))
            break;

        // 32 位图标的颜色位图通常自带 alpha。旧式图标可能把 alpha 全置 0，
        // 此时改用 AND mask；不能直接让 GDI+ FromHICON 合成，否则透明列可能
        // 被转换成不透明白色（例如 VS Code 图标中的竖条）。
        bool hasColorAlpha = colorBitmap.bmBitsPixel == 32;
        if (hasColorAlpha) {
            hasColorAlpha = false;
            for (size_t i = 3; i < colorPixels.size(); i += 4) {
                if (colorPixels[i] != 0) {
                    hasColorAlpha = true;
                    break;
                }
            }
        }

        std::vector<BYTE> maskPixels;
        UINT maskStride = 0;
        bool hasMask = false;
        if (iconInfo.hbmMask) {
            BITMAP maskBitmap{};
            if (GetObjectW(iconInfo.hbmMask, sizeof(maskBitmap), &maskBitmap) ==
                    sizeof(maskBitmap) &&
                maskBitmap.bmWidth >= static_cast<LONG>(iconWidth) &&
                maskBitmap.bmHeight >= static_cast<LONG>(iconHeight)) {
                maskStride = ((iconWidth + 31u) / 32u) * 4u;
                maskPixels.resize(static_cast<size_t>(maskStride) * iconHeight);
                // 1 位 DIB 在 DIB_RGB_COLORS 模式下需要两个 RGBQUAD 调色板项。
                // BITMAPINFO 自带的 bmiColors[1] 不够，直接传入会覆盖栈内存。
                struct MaskBitmapInfo {
                    BITMAPINFOHEADER bmiHeader;
                    RGBQUAD bmiColors[2];
                } maskInfo{};
                maskInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                maskInfo.bmiHeader.biWidth = static_cast<LONG>(iconWidth);
                maskInfo.bmiHeader.biHeight = -static_cast<LONG>(iconHeight);
                maskInfo.bmiHeader.biPlanes = 1;
                maskInfo.bmiHeader.biBitCount = 1;
                maskInfo.bmiHeader.biCompression = BI_RGB;
                maskInfo.bmiHeader.biClrUsed = 2;
                hasMask = GetDIBits(screenDc, iconInfo.hbmMask, 0, iconHeight,
                                    maskPixels.data(),
                                    reinterpret_cast<BITMAPINFO*>(&maskInfo),
                                    DIB_RGB_COLORS) == static_cast<int>(iconHeight);
            }
        }
        const bool useMask = hasMask && !hasColorAlpha;

        auto premultiply = [](BYTE value, BYTE alpha) -> BYTE {
            return static_cast<BYTE>((static_cast<unsigned>(value) * alpha + 127u) / 255u);
        };
        for (UINT y = 0; y < iconHeight; ++y) {
            for (UINT x = 0; x < iconWidth; ++x) {
                BYTE* pixel = colorPixels.data() +
                              (static_cast<size_t>(y) * iconWidth + x) * 4;
                BYTE alpha = hasColorAlpha ? pixel[3] : 255;
                if (useMask) {
                    const BYTE maskByte = maskPixels[static_cast<size_t>(y) * maskStride + x / 8];
                    if ((maskByte & (0x80u >> (x & 7u))) != 0)
                        alpha = 0;
                }
                if (alpha == 0) {
                    pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
                    continue;
                }
                pixel[0] = premultiply(pixel[0], alpha);
                pixel[1] = premultiply(pixel[1], alpha);
                pixel[2] = premultiply(pixel[2], alpha);
                pixel[3] = alpha;
            }
        }

        pixels = std::move(colorPixels);
        width = iconWidth;
        height = iconHeight;
        ok = true;
    } while (false);

    if (screenDc)
        ReleaseDC(nullptr, screenDc);
    if (iconInfo.hbmColor)
        DeleteObject(iconInfo.hbmColor);
    if (iconInfo.hbmMask)
        DeleteObject(iconInfo.hbmMask);
    return ok;
}

bool readIconPixels(HICON icon, std::vector<BYTE>& pixels, UINT& width, UINT& height) {
    if (!icon)
        return false;

    if (readIconPixelsFromHicon(icon, pixels, width, height)) {
        clearIconBlackMatte(pixels, width, height);
        return true;
    }

    std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromHICON(icon));
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok)
        return false;

    width = bitmap->GetWidth();
    height = bitmap->GetHeight();
    if (width == 0 || height == 0)
        return false;

    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) !=
        Gdiplus::Ok)
        return false;

    pixels.resize(static_cast<size_t>(width) * height * 4);
    const BYTE* base = static_cast<const BYTE*>(data.Scan0);
    const LONG stride = data.Stride;
    const LONG rowStride = stride >= 0 ? stride : -stride;
    for (UINT y = 0; y < height; ++y) {
        const UINT sourceRow = stride >= 0 ? y : height - 1 - y;
        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
                    base + static_cast<size_t>(sourceRow) * rowStride,
                    static_cast<size_t>(width) * 4);
    }
    bitmap->UnlockBits(&data);
    clearIconBlackMatte(pixels, width, height);
    return true;
}

bool readIconPixelsFromPath(const std::wstring& path, std::vector<BYTE>& pixels, UINT& width,
                            UINT& height) {
    if (path.empty())
        return false;

    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    ExtractIconExW(path.c_str(), 0, &largeIcon, &smallIcon, 1);
    HICON icon = largeIcon ? largeIcon : smallIcon;
    HICON shellIcon = nullptr;
    if (!icon) {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON))
            shellIcon = info.hIcon;
        icon = shellIcon;
    }

    const bool ok = readIconPixels(icon, pixels, width, height);
    if (largeIcon)
        DestroyIcon(largeIcon);
    if (smallIcon)
        DestroyIcon(smallIcon);
    if (shellIcon)
        DestroyIcon(shellIcon);
    return ok;
}

std::wstring fallbackExeName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t start = slash == std::wstring::npos ? 0 : slash + 1;
    std::wstring name = path.substr(start);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos)
        name.resize(dot);
    return name;
}

} // namespace

namespace platform_icon {

bool readHiconPixels(HICON icon, std::vector<BYTE>& pixels, UINT& width, UINT& height) {
    return readIconPixels(icon, pixels, width, height);
}

bool readSourceIconPixels(const std::wstring& sourceAppUserModelId,
                          std::vector<BYTE>& pixels, UINT& width, UINT& height) {
    const std::wstring path = resolveSourceIconPath(sourceAppUserModelId);
    return readIconPixelsFromPath(path, pixels, width, height);
}

bool readExeIconPixels(const std::wstring& path, std::vector<BYTE>& pixels, UINT& width,
                       UINT& height) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    return readIconPixelsFromPath(path, pixels, width, height);
}

bool readExeDisplayName(const std::wstring& path, std::wstring& name) {
    name.clear();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size > 0) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
            struct Translation {
                WORD language;
                WORD codePage;
            };
            Translation* translations = nullptr;
            UINT translationBytes = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                               reinterpret_cast<void**>(&translations), &translationBytes) &&
                translations && translationBytes >= sizeof(Translation)) {
                const UINT count = translationBytes / sizeof(Translation);
                for (UINT i = 0; i < count; ++i) {
                    wchar_t key[128]{};
                    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                               translations[i].language, translations[i].codePage);
                    wchar_t* description = nullptr;
                    UINT descriptionLength = 0;
                    if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&description),
                                       &descriptionLength) &&
                        description && descriptionLength > 1) {
                        name.assign(description, descriptionLength - 1);
                        return true;
                    }
                }
            }
        }
    }

    name = fallbackExeName(path);
    return !name.empty();
}

bool launchSourceApp(const std::wstring& sourceAppUserModelId) {
    const auto candidates = sourceProcessCandidates(sourceAppUserModelId);
    for (const auto& candidate : candidates) {
        const auto processIds = findProcessIds(candidate);
        const wchar_t* preferredClass = preferredWindowClassForProcess(candidate);
        if (!processIds.empty() &&
            activateExistingWindow(processIds, preferredClass, preferredClass != nullptr))
            return true;
    }

    // 当前桌面播放器的 SMTC 来源通常是 *.exe；如果找不到可激活的已有窗口，
    // 再使用已运行进程的真实路径启动/唤醒单实例程序。
    for (const auto& candidate : candidates) {
        if (executePath(findProcessImagePath(candidate)))
            return true;
    }

    // 对真正的 AUMID 保留系统激活路径，兼容以后接入的打包播放器。
    if (activateAppUserModelId(sourceAppUserModelId))
        return true;

    return executePath(resolveSourceIconPath(sourceAppUserModelId));
}

bool launchConfiguredExe(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;

    const auto processIds = findProcessIdsByImagePath(path);
    const wchar_t* preferredClass = preferredWindowClassForProcess(fileNameFromPath(path));
    if (!processIds.empty() &&
        activateExistingWindow(processIds, preferredClass, preferredClass != nullptr))
        return true;
    return executePath(path);
}

bool launchUri(const std::wstring& uri) {
    return executeUri(uri);
}

} // namespace platform_icon
