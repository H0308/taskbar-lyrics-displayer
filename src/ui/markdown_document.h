#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace markdown_doc {

struct MarkdownSpanStyle {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    bool link = false;

    bool operator==(const MarkdownSpanStyle& other) const {
        return bold == other.bold && italic == other.italic && strike == other.strike &&
               code == other.code && link == other.link;
    }
};

struct MarkdownSpan {
    size_t start = 0;
    size_t length = 0;
    MarkdownSpanStyle style;
};

enum class MarkdownBlockKind {
    Paragraph,
    Heading,
    Bullet,
    Ordered,
    Quote,
    Code,
    Rule,
};

struct MarkdownBlock {
    MarkdownBlockKind kind = MarkdownBlockKind::Paragraph;
    int level = 0;
    int number = 0;
    std::wstring prefix;
    std::wstring text;
    std::vector<MarkdownSpan> spans;
};

// 将轻量 Markdown 文本解析为块与行内样式片段列表，供渲染层排版使用。
std::vector<MarkdownBlock> parseMarkdown(const std::wstring& markdown);

} // namespace markdown_doc
