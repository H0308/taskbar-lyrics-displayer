#include "ui/markdown_document.h"

#include <algorithm>
#include <string_view>

namespace markdown_doc {
namespace {

std::wstring trimMarkdown(std::wstring_view value) {
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == L' ' || value[begin] == L'\t'))
        ++begin;
    size_t end = value.size();
    while (end > begin && (value[end - 1] == L' ' || value[end - 1] == L'\t'))
        --end;
    return std::wstring(value.substr(begin, end - begin));
}

bool isMarkdownWord(wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
           (ch >= L'0' && ch <= L'9') || ch == L'_';
}

bool isMarkdownRule(std::wstring_view value) {
    std::wstring compact;
    for (wchar_t ch : value) {
        if (ch != L' ' && ch != L'\t')
            compact.push_back(ch);
    }
    if (compact.size() < 3)
        return false;
    const wchar_t marker = compact.front();
    if (marker != L'-' && marker != L'*' && marker != L'_')
        return false;
    return std::all_of(compact.begin(), compact.end(),
                       [marker](wchar_t ch) { return ch == marker; });
}

bool parseMarkdownHeading(std::wstring_view line, int* level, std::wstring* text) {
    size_t count = 0;
    while (count < line.size() && count < 6 && line[count] == L'#')
        ++count;
    if (count == 0 || count >= line.size() || line[count] != L' ')
        return false;

    std::wstring heading = trimMarkdown(line.substr(count + 1));
    while (!heading.empty() && heading.back() == L'#')
        heading.pop_back();
    *level = static_cast<int>(count);
    *text = trimMarkdown(heading);
    return true;
}

bool parseMarkdownBullet(std::wstring_view line, int* depth, std::wstring* text) {
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == L' ' || line[pos] == L'\t'))
        ++pos;
    if (pos + 1 >= line.size() ||
        (line[pos] != L'-' && line[pos] != L'*' && line[pos] != L'+') ||
        line[pos + 1] != L' ')
        return false;
    *depth = static_cast<int>(pos / 2);
    *text = trimMarkdown(line.substr(pos + 2));
    return true;
}

bool parseMarkdownOrdered(std::wstring_view line, int* depth, int* number,
                          std::wstring* text) {
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == L' ' || line[pos] == L'\t'))
        ++pos;
    const size_t digitBegin = pos;
    int parsedNumber = 0;
    while (pos < line.size() && line[pos] >= L'0' && line[pos] <= L'9') {
        parsedNumber = parsedNumber * 10 + static_cast<int>(line[pos] - L'0');
        ++pos;
    }
    if (pos == digitBegin || pos + 1 >= line.size() ||
        (line[pos] != L'.' && line[pos] != L')') || line[pos + 1] != L' ')
        return false;
    *depth = static_cast<int>(digitBegin / 2);
    *number = parsedNumber;
    *text = trimMarkdown(line.substr(pos + 2));
    return true;
}

void appendMarkdownText(MarkdownBlock& block, std::wstring_view text,
                        const MarkdownSpanStyle& style) {
    if (text.empty())
        return;
    const size_t start = block.text.size();
    block.text.append(text);
    if (!block.spans.empty() && block.spans.back().style == style &&
        block.spans.back().start + block.spans.back().length == start) {
        block.spans.back().length += text.size();
    } else {
        block.spans.push_back(MarkdownSpan{start, text.size(), style});
    }
}

void parseMarkdownInline(std::wstring_view source, MarkdownBlock& block,
                         MarkdownSpanStyle style = {}) {
    size_t plainStart = 0;
    auto flushPlain = [&](size_t end) {
        if (end > plainStart)
            appendMarkdownText(block, source.substr(plainStart, end - plainStart), style);
    };

    size_t i = 0;
    while (i < source.size()) {
        if (source[i] == L'\\' && i + 1 < source.size()) {
            flushPlain(i);
            appendMarkdownText(block, source.substr(i + 1, 1), style);
            i += 2;
            plainStart = i;
            continue;
        }

        if (source[i] == L'`') {
            const size_t close = source.find(L'`', i + 1);
            if (close != std::wstring_view::npos && close > i + 1) {
                flushPlain(i);
                MarkdownSpanStyle codeStyle = style;
                codeStyle.code = true;
                appendMarkdownText(block, source.substr(i + 1, close - i - 1), codeStyle);
                i = close + 1;
                plainStart = i;
                continue;
            }
        }

        std::wstring_view marker;
        if (i + 1 < source.size() &&
            ((source[i] == L'*' && source[i + 1] == L'*') ||
             (source[i] == L'_' && source[i + 1] == L'_') ||
             (source[i] == L'~' && source[i + 1] == L'~'))) {
            marker = source.substr(i, 2);
            const size_t close = source.find(marker, i + 2);
            if (close != std::wstring_view::npos && close > i + 2) {
                flushPlain(i);
                MarkdownSpanStyle nested = style;
                if (marker[0] == L'~')
                    nested.strike = true;
                else
                    nested.bold = true;
                parseMarkdownInline(source.substr(i + 2, close - i - 2), block, nested);
                i = close + 2;
                plainStart = i;
                continue;
            }
        }

        if (source[i] == L'[') {
            const size_t closeLabel = source.find(L"](", i + 1);
            if (closeLabel != std::wstring_view::npos) {
                const size_t closeUrl = source.find(L')', closeLabel + 2);
                if (closeUrl != std::wstring_view::npos && closeUrl > i + 1) {
                    flushPlain(i);
                    MarkdownSpanStyle linkStyle = style;
                    linkStyle.link = true;
                    parseMarkdownInline(source.substr(i + 1, closeLabel - i - 1), block,
                                        linkStyle);
                    i = closeUrl + 1;
                    plainStart = i;
                    continue;
                }
            }
        }

        if (source[i] == L'*' || source[i] == L'_') {
            if (source[i] == L'_' && i > 0 && i + 1 < source.size() &&
                isMarkdownWord(source[i - 1]) && isMarkdownWord(source[i + 1])) {
                ++i;
                continue;
            }
            const size_t close = source.find(source[i], i + 1);
            if (close != std::wstring_view::npos && close > i + 1) {
                flushPlain(i);
                MarkdownSpanStyle nested = style;
                nested.italic = true;
                parseMarkdownInline(source.substr(i + 1, close - i - 1), block, nested);
                i = close + 1;
                plainStart = i;
                continue;
            }
        }
        ++i;
    }
    flushPlain(source.size());
}

} // namespace

std::vector<MarkdownBlock> parseMarkdown(const std::wstring& markdown) {
    std::vector<MarkdownBlock> blocks;
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= markdown.size()) {
        size_t end = markdown.find(L'\n', start);
        if (end == std::wstring::npos)
            end = markdown.size();
        std::wstring line = markdown.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (end == markdown.size())
            break;
        start = end + 1;
    }

    std::wstring paragraph;
    bool inFence = false;
    MarkdownBlock codeBlock;
    auto flushParagraph = [&] {
        if (paragraph.empty())
            return;
        MarkdownBlock block;
        block.kind = MarkdownBlockKind::Paragraph;
        parseMarkdownInline(paragraph, block);
        blocks.push_back(std::move(block));
        paragraph.clear();
    };
    auto addInlineBlock = [&](MarkdownBlockKind kind, const std::wstring& text,
                              const std::wstring& prefix, int level, int number) {
        MarkdownBlock block;
        block.kind = kind;
        block.prefix = prefix;
        block.level = level;
        block.number = number;
        parseMarkdownInline(text, block);
        blocks.push_back(std::move(block));
    };

    for (const auto& line : lines) {
        const std::wstring trimmed = trimMarkdown(line);
        const bool fenceLine = trimmed.rfind(L"```", 0) == 0 || trimmed.rfind(L"~~~", 0) == 0;
        if (inFence) {
            if (fenceLine) {
                blocks.push_back(std::move(codeBlock));
                codeBlock = MarkdownBlock{};
                inFence = false;
            } else {
                if (!codeBlock.text.empty())
                    codeBlock.text.push_back(L'\n');
                codeBlock.text += line;
            }
            continue;
        }
        if (fenceLine) {
            flushParagraph();
            inFence = true;
            codeBlock = MarkdownBlock{};
            codeBlock.kind = MarkdownBlockKind::Code;
            continue;
        }
        if (trimmed.empty()) {
            flushParagraph();
            continue;
        }

        if (trimmed.front() == L'>') {
            flushParagraph();
            const std::wstring quote = trimMarkdown(trimmed.substr(1));
            addInlineBlock(MarkdownBlockKind::Quote, quote, L"│ ", 0, 0);
            continue;
        }

        int headingLevel = 0;
        std::wstring headingText;
        if (parseMarkdownHeading(trimmed, &headingLevel, &headingText)) {
            flushParagraph();
            addInlineBlock(MarkdownBlockKind::Heading, headingText, L"", headingLevel, 0);
            continue;
        }
        if (isMarkdownRule(trimmed)) {
            flushParagraph();
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::Rule;
            blocks.push_back(std::move(block));
            continue;
        }

        int depth = 0;
        std::wstring listText;
        if (parseMarkdownBullet(line, &depth, &listText)) {
            flushParagraph();
            std::wstring prefix(static_cast<size_t>(depth) * 2, L' ');
            if (listText.size() >= 3 && listText[0] == L'[' && listText[2] == L']' &&
                (listText[1] == L' ' || listText[1] == L'x' || listText[1] == L'X')) {
                prefix += listText[1] == L' ' ? L"☐ " : L"☑ ";
                listText = trimMarkdown(listText.substr(3));
            } else {
                prefix += L"• ";
            }
            addInlineBlock(MarkdownBlockKind::Bullet, listText, prefix, depth, 0);
            continue;
        }

        int number = 0;
        if (parseMarkdownOrdered(line, &depth, &number, &listText)) {
            flushParagraph();
            std::wstring prefix(static_cast<size_t>(depth) * 2, L' ');
            prefix += std::to_wstring(number) + L". ";
            addInlineBlock(MarkdownBlockKind::Ordered, listText, prefix, depth, number);
            continue;
        }

        if (!paragraph.empty())
            paragraph.push_back(L' ');
        paragraph += trimmed;
    }
    flushParagraph();
    if (inFence)
        blocks.push_back(std::move(codeBlock));
    return blocks;
}

} // namespace markdown_doc
