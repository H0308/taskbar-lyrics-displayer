#include "test_support.h"

#include "ui/text_edit_buffer.h"

TEST_CASE("text_edit_buffer.selection_digits_limit_and_undo") {
    fluent::TextEditBuffer buffer;
    constexpr fluent::TextEditOptions sliderOpts{
        .digitsOnly = true,
        .printableOnly = false,
        .maxLength = 3,
        .allowUndo = true,
    };

    buffer.reset(L"48", true);
    EXPECT_EQ(buffer.text, std::wstring(L"48"));
    EXPECT_EQ(buffer.selectionStart(), 0u);
    EXPECT_EQ(buffer.selectionEnd(), 2u);
    EXPECT_TRUE(buffer.hasSelection());
    EXPECT_EQ(buffer.selectedText(), std::wstring(L"48"));

    EXPECT_FALSE(buffer.replaceSelection(L"12a", sliderOpts));
    EXPECT_EQ(buffer.text, std::wstring(L"48"));

    EXPECT_FALSE(buffer.replaceSelection(L"1234", sliderOpts));
    EXPECT_EQ(buffer.text, std::wstring(L"48"));

    EXPECT_TRUE(buffer.replaceSelection(L"120", sliderOpts));
    EXPECT_EQ(buffer.text, std::wstring(L"120"));
    EXPECT_EQ(buffer.caret, 3u);
    EXPECT_FALSE(buffer.hasSelection());

    EXPECT_TRUE(buffer.undo());
    EXPECT_EQ(buffer.text, std::wstring(L"48"));
    EXPECT_EQ(buffer.anchor, 0u);
    EXPECT_EQ(buffer.caret, 2u);

    EXPECT_TRUE(buffer.undo());
    EXPECT_EQ(buffer.text, std::wstring(L"120"));
}

TEST_CASE("text_edit_buffer.printable_filtering_caret_navigation_and_deletion") {
    fluent::TextEditBuffer buffer;
    constexpr fluent::TextEditOptions searchOpts{
        .digitsOnly = false,
        .printableOnly = true,
        .maxLength = 0,
        .allowUndo = false,
    };

    buffer.reset(L"晴天", false);
    EXPECT_TRUE(buffer.replaceSelection(L" \r\n(Live)\x7f", searchOpts));
    EXPECT_EQ(buffer.text, std::wstring(L"晴天 (Live)"));
    EXPECT_EQ(buffer.caret, 9u);

    bool modified = false;
    EXPECT_TRUE(buffer.handleKey(nullptr, VK_LEFT, false, true, searchOpts, &modified));
    EXPECT_FALSE(modified);
    EXPECT_EQ(buffer.caret, 8u);
    EXPECT_EQ(buffer.anchor, 9u);
    EXPECT_EQ(buffer.selectedText(), std::wstring(L")"));

    EXPECT_TRUE(buffer.handleKey(nullptr, VK_LEFT, false, false, searchOpts, &modified));
    EXPECT_EQ(buffer.caret, 8u);
    EXPECT_EQ(buffer.anchor, 8u);

    EXPECT_TRUE(buffer.handleKey(nullptr, VK_HOME, false, true, searchOpts, &modified));
    EXPECT_EQ(buffer.caret, 0u);
    EXPECT_EQ(buffer.anchor, 8u);

    EXPECT_TRUE(buffer.handleKey(nullptr, VK_DELETE, false, false, searchOpts, &modified));
    EXPECT_TRUE(modified);
    EXPECT_EQ(buffer.text, std::wstring(L")"));
    EXPECT_EQ(buffer.caret, 0u);

    EXPECT_TRUE(buffer.handleKey(nullptr, VK_END, false, false, searchOpts, &modified));
    EXPECT_TRUE(buffer.handleKey(nullptr, VK_BACK, false, false, searchOpts, &modified));
    EXPECT_TRUE(modified);
    EXPECT_TRUE(buffer.text.empty());
    EXPECT_FALSE(buffer.deleteBackward(searchOpts));
    EXPECT_FALSE(buffer.deleteForward(searchOpts));
}

TEST_CASE("text_edit_buffer.decimal_filtering_allows_single_dot") {
    fluent::TextEditBuffer buffer;
    constexpr fluent::TextEditOptions sizeOpts{
        .digitsOnly = false,
        .decimalOnly = true,
        .printableOnly = false,
        .maxLength = 0,
        .allowUndo = false,
    };

    buffer.reset(L"14", false);
    EXPECT_TRUE(buffer.replaceSelection(L".5a.8", sizeOpts));
    EXPECT_EQ(buffer.text, std::wstring(L"14.58"));

    // 光标在末尾插入第二个小数点应被忽略，不修改原文本。
    EXPECT_FALSE(buffer.replaceSelection(L".", sizeOpts));
    EXPECT_EQ(buffer.text, std::wstring(L"14.58"));

    // 选区覆盖原有小数点时，允许重新输入一个小数点。
    buffer.selectAll();
    EXPECT_TRUE(buffer.replaceSelection(L"18.25", sizeOpts));
    EXPECT_EQ(buffer.text, std::wstring(L"18.25"));
}
