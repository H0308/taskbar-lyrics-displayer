#include "ui/text_edit_buffer.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <utility>

namespace fluent {

void TextEditBuffer::reset(std::wstring initialText, bool selectAllText) {
    text = std::move(initialText);
    caret = text.size();
    anchor = selectAllText ? 0 : caret;
    undoText_.clear();
    undoCaret_ = 0;
    undoAnchor_ = 0;
    canUndo_ = false;
}

size_t TextEditBuffer::selectionStart() const {
    return std::min(caret, anchor);
}

size_t TextEditBuffer::selectionEnd() const {
    return std::max(caret, anchor);
}

bool TextEditBuffer::hasSelection() const {
    return caret != anchor;
}

std::wstring TextEditBuffer::selectedText() const {
    const size_t start = selectionStart();
    const size_t end = selectionEnd();
    return text.substr(start, end - start);
}

void TextEditBuffer::selectAll() {
    anchor = 0;
    caret = text.size();
}

void TextEditBuffer::moveCaret(size_t next, bool extend) {
    next = std::min(next, text.size());
    if (extend)
        caret = next;
    else
        caret = anchor = next;
}

bool TextEditBuffer::replaceSelection(const std::wstring& inserted,
                                      const TextEditOptions& options) {
    if (options.digitsOnly &&
        inserted.find_first_not_of(L"0123456789") != std::wstring::npos) {
        return false;
    }

    const size_t start = selectionStart();
    const size_t end = selectionEnd();

    std::wstring accepted;
    if (options.decimalOnly) {
        bool hasDot = false;
        for (size_t i = 0; i < text.size(); ++i) {
            if ((i < start || i >= end) && text[i] == L'.') {
                hasDot = true;
                break;
            }
        }
        accepted.reserve(inserted.size());
        for (wchar_t ch : inserted) {
            if (std::iswdigit(ch)) {
                accepted.push_back(ch);
            } else if (ch == L'.' && !hasDot) {
                accepted.push_back(ch);
                hasDot = true;
            }
        }
    } else if (options.printableOnly) {
        accepted.reserve(inserted.size());
        for (wchar_t ch : inserted) {
            if (ch >= L' ' && ch != 0x7f)
                accepted.push_back(ch);
        }
    } else {
        accepted = inserted;
    }

    if (accepted.empty() && (start == end || !inserted.empty()))
        return false;

    if (options.maxLength > 0 &&
        text.size() - (end - start) + accepted.size() > options.maxLength) {
        return false;
    }

    if (options.allowUndo) {
        undoText_ = text;
        undoCaret_ = caret;
        undoAnchor_ = anchor;
        canUndo_ = true;
    }

    text.replace(start, end - start, accepted);
    caret = anchor = start + accepted.size();
    return true;
}

bool TextEditBuffer::deleteBackward(const TextEditOptions& options) {
    if (!hasSelection()) {
        if (caret == 0)
            return false;
        anchor = caret - 1;
    }
    return replaceSelection(L"", options);
}

bool TextEditBuffer::deleteForward(const TextEditOptions& options) {
    if (!hasSelection()) {
        if (caret >= text.size())
            return false;
        anchor = caret + 1;
    }
    return replaceSelection(L"", options);
}

bool TextEditBuffer::undo() {
    if (!canUndo_)
        return false;
    std::swap(text, undoText_);
    std::swap(caret, undoCaret_);
    std::swap(anchor, undoAnchor_);
    return true;
}

bool TextEditBuffer::copySelection(HWND hwnd) const {
    const size_t start = selectionStart();
    const size_t end = selectionEnd();
    if (start == end || !hwnd || !OpenClipboard(hwnd))
        return false;

    const std::wstring fragment = text.substr(start, end - start);
    const size_t bytes = (fragment.size() + 1) * sizeof(wchar_t);
    bool copied = false;
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* data = GlobalLock(memory)) {
            std::memcpy(data, fragment.c_str(), bytes);
            GlobalUnlock(memory);
            EmptyClipboard();
            copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!copied)
            GlobalFree(memory);
    }
    CloseClipboard();
    return copied;
}

std::wstring TextEditBuffer::readClipboardText(HWND hwnd) {
    std::wstring result;
    if (!hwnd || !OpenClipboard(hwnd))
        return result;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* value = static_cast<const wchar_t*>(GlobalLock(data))) {
            result = value;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return result;
}

bool TextEditBuffer::handleKey(HWND hwnd, WPARAM key, bool ctrl, bool shift,
                               const TextEditOptions& options, bool* textModified) {
    if (textModified)
        *textModified = false;

    if (ctrl && (key == 'A' || key == 'a')) {
        selectAll();
        return true;
    }
    if ((ctrl && (key == 'C' || key == 'c' || key == 'X' || key == 'x' || key == VK_INSERT)) ||
        (shift && key == VK_DELETE)) {
        const bool isCut = (key == 'X' || key == 'x' || key == VK_DELETE);
        if (copySelection(hwnd) && isCut) {
            if (replaceSelection(L"", options) && textModified)
                *textModified = true;
        }
        return true;
    }
    if ((ctrl && (key == 'V' || key == 'v')) || (shift && key == VK_INSERT)) {
        const std::wstring clip = readClipboardText(hwnd);
        if (!clip.empty() && replaceSelection(clip, options) && textModified)
            *textModified = true;
        return true;
    }
    if (options.allowUndo && ctrl && (key == 'Z' || key == 'z' || key == 'Y' || key == 'y')) {
        if (undo() && textModified)
            *textModified = true;
        return true;
    }

    switch (key) {
    case VK_LEFT:
        if (!shift && hasSelection())
            moveCaret(selectionStart(), false);
        else if (caret > 0)
            moveCaret(caret - 1, shift);
        else if (!shift)
            anchor = caret;
        return true;
    case VK_RIGHT:
        if (!shift && hasSelection())
            moveCaret(selectionEnd(), false);
        else if (caret < text.size())
            moveCaret(caret + 1, shift);
        else if (!shift)
            anchor = caret;
        return true;
    case VK_HOME:
        moveCaret(0, shift);
        return true;
    case VK_END:
        moveCaret(text.size(), shift);
        return true;
    case VK_BACK:
        if (deleteBackward(options) && textModified)
            *textModified = true;
        return true;
    case VK_DELETE:
        if (deleteForward(options) && textModified)
            *textModified = true;
        return true;
    default:
        return false;
    }
}

} // namespace fluent
