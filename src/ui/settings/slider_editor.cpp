#include "ui/settings/slider_editor.h"
#include <algorithm>
#include <cmath>
#include <climits>
#include <cwctype>

namespace settings_ui {
namespace {
constexpr fluent::TextEditOptions kSliderEditOptions{
    .digitsOnly = true,
    .printableOnly = false,
    .maxLength = 3,
    .allowUndo = true,
};

bool parseBoundedInteger(const std::wstring& text, int minValue, int maxValue, int& value) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::iswspace(text[begin]))
        ++begin;
    while (end > begin && std::iswspace(text[end - 1]))
        --end;
    if (begin == end)
        return false;

    int parsed = 0;
    for (size_t i = begin; i < end; ++i) {
        const wchar_t ch = text[i];
        if (ch < L'0' || ch > L'9')
            return false;
        const int digit = static_cast<int>(ch - L'0');
        if (parsed > (INT_MAX - digit) / 10)
            parsed = INT_MAX;
        else
            parsed = parsed * 10 + digit;
    }
    value = std::clamp(parsed, minValue, maxValue);
    return true;
}

} // namespace
void SliderEditor::resetCaret() {
    state_.caretVisible = true;
    KillTimer(owner_.hwnd, kSliderCaretTimer);
    const UINT blink = GetCaretBlinkTime();
    if (blink != INFINITE && blink > 0)
        SetTimer(owner_.hwnd, kSliderCaretTimer, blink, nullptr);
    owner_.invalidate();
}

void SliderEditor::hide() {
    state_.open = false;
    state_.rowId = 0;
    state_.selecting = false;
    KillTimer(owner_.hwnd, kSliderCaretTimer);
    if (GetCapture() == owner_.hwnd)
        ReleaseCapture();
    owner_.invalidate();
}

void SliderEditor::cancel() {
    if (state_.open)
        hide();
}

void SliderEditor::commit() {
    if (!state_.open || state_.committing)
        return;
    state_.committing = true;
    Row* row = owner_.findRow(state_.rowId);
    if (row && row->enabled) {
        int value = row->value;
        if (parseBoundedInteger(state_.buffer.text, row->minValue, row->maxValue, value) &&
            value != row->value) {
            row->value = value;
            owner_.apply(row->id);
        }
    }
    hide();
    state_.committing = false;
}

void SliderEditor::layout() {
    if (!state_.open)
        return;
    const Row* row = owner_.findRow(state_.rowId);
    if (!row || row->kind != ControlKind::Slider || !row->enabled ||
        row->cardRect.right <= row->cardRect.left || row->cardRect.bottom <= owner_.viewport.top ||
        row->cardRect.top >= owner_.viewport.bottom)
        hide();
}

void SliderEditor::open(Row& row) {
    if (!row.enabled)
        return;
    if (state_.open && state_.rowId != row.id)
        commit();
    owner_.stopScroll();
    state_.rowId = row.id;
    state_.open = true;
    state_.buffer.reset(std::to_wstring(row.value), true);
    owner_.focusedId = row.id;
    SetFocus(owner_.hwnd);
    resetCaret();
}

void SliderEditor::replaceSelection(const std::wstring& inserted) {
    state_.buffer.replaceSelection(inserted, kSliderEditOptions);
    resetCaret();
}

bool SliderEditor::copySelection() {
    return state_.buffer.copySelection(owner_.hwnd);
}

bool SliderEditor::handleKey(WPARAM key) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (key == VK_TAB || (ctrl && key == 'F')) {
        commit();
        return false;
    }
    if (key == VK_RETURN) {
        commit();
        return true;
    }
    if (key == VK_ESCAPE) {
        cancel();
        return true;
    }
    state_.buffer.handleKey(owner_.hwnd, key, ctrl, shift, kSliderEditOptions);
    resetCaret();
    return true;
}

void SliderEditor::placeCaret(float pointerX, bool extend) {
    size_t nearest = 0;
    for (size_t i = 1; i <= state_.buffer.text.size(); ++i)
        if (std::fabs(pointerX - state_.caretStops[i]) <
            std::fabs(pointerX - state_.caretStops[nearest]))
            nearest = i;
    state_.buffer.moveCaret(nearest, extend);
    resetCaret();
}

} // namespace settings_ui
