#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

namespace fluent {

struct TextEditOptions {
    bool digitsOnly = false;
    bool decimalOnly = false;
    bool printableOnly = false;
    size_t maxLength = 0;
    bool allowUndo = false;
};

/// 单行 D2D 文本框共用的纯编辑状态与剪贴板/导航操作缓冲区。
class TextEditBuffer {
public:
    std::wstring text;
    size_t caret = 0;
    size_t anchor = 0;

    void reset(std::wstring initialText = {}, bool selectAll = false);

    size_t selectionStart() const;
    size_t selectionEnd() const;
    bool hasSelection() const;
    std::wstring selectedText() const;

    void selectAll();
    void moveCaret(size_t next, bool extend);
    bool replaceSelection(const std::wstring& inserted, const TextEditOptions& options = {});
    bool deleteBackward(const TextEditOptions& options = {});
    bool deleteForward(const TextEditOptions& options = {});
    bool undo();

    bool copySelection(HWND hwnd) const;
    static std::wstring readClipboardText(HWND hwnd);

    bool handleKey(HWND hwnd, WPARAM key, bool ctrl, bool shift,
                   const TextEditOptions& options = {}, bool* textModified = nullptr);

private:
    std::wstring undoText_;
    size_t undoCaret_ = 0;
    size_t undoAnchor_ = 0;
    bool canUndo_ = false;
};

} // namespace fluent
