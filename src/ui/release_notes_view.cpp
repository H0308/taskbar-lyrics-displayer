#include "ui/release_notes_view.h"

#include "ui/font_sizes.h"
#include "ui/fluent_theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

const std::wstring& disclaimerMarkdown() {
    static const std::wstring content =
        L"1. **开源协议适用范围**：本项目的 **Apache-2.0** 许可证仅适用于本仓库中的自研源代码，不涵盖且不构成对任何第三方接口、服务数据、媒体内容、字体、图标或第三方软件的授权。\n\n"
        L"2. **非官方关联与商标声明**：本项目为独立开发的桌面辅助工具，与 QQ 音乐、网易云音乐、汽水音乐、酷狗音乐、Spotify、滴答清单（Dida365）以及 BetterNCM、InfLink-rs、LRCLIB、MusicBrainz 等第三方产品、平台或服务均无任何官方关联、合作或背书。程序中涉及的第三方产品名称、品牌标识及运行时从用户本地客户端读取的图标，其商标权与版权均归原权利方所有。\n\n"
        L"3. **内容权属与接口说明**：本项目自身及仓库不存储、托管或分发任何音乐文件、歌词文本、专辑封面或任务数据。程序仅在用户本地运行时，按需读取本机已播放软件的公开媒体会话、用户本地缓存目录，或向第三方公开接口发起实时展示请求。歌词、专辑封面及文案等内容的著作权归词曲作者、唱片公司及原服务提供方所有；第三方接口的可用性及格式随时可能发生变更，本项目不保证各在线来源长期稳定可用。\n\n"
        L"4. **使用与再分发责任**：任何个人或组织在使用、修改或再分发本项目（包括基于 Apache-2.0 协议进行的各类商业或非商业分发）时，均须自行确保其行为符合所在国家或地区的法律法规以及相关第三方平台的服务条款，且不得以本项目或原作者名义进行虚假宣传或商业背书。因违反第三方服务条款、接口风控或版权争议所产生的任何后果，均由实际使用方或再分发方自行承担。\n\n"
        L"5. **无担保与责任限制**：本软件按「现状（AS IS）」提供，不附带任何明示或暗示的适用性或稳定性担保。程序涉及 Windows 系统媒体控制、音频回环捕获、UI Automation 及任务栏窗口交互等系统级操作，在任何情况下，作者均不对因使用、无法使用、修改或分发本软件而导致的任何直接或间接损失（包括但不限于系统异常、数据丢失、账号受限或商业损失）承担法律责任。";
    return content;
}

} // namespace

bool ReleaseNotesView::create(HWND parent) {
    parent_ = parent;
    return scrollView_.create(parent, 0x6301, [this](float position, bool redraw) {
        scrollY_ = position;
        scrollOnlyPaint_ = !redraw;
        const float scale = fluent::dipScale(GetDpiForWindow(parent_));
        const RECT dirty{
            static_cast<LONG>(std::floor((redraw ? panelRect_.left : scrollbarRight_ - 10.0f) * scale)),
            static_cast<LONG>(std::floor(panelRect_.top * scale)),
            static_cast<LONG>(std::ceil((scrollbarRight_ + 4.0f) * scale)),
            static_cast<LONG>(std::ceil(panelRect_.bottom * scale))};
        InvalidateRect(parent_, &dirty, FALSE);
    });
}

bool ReleaseNotesView::tick(WPARAM timer) {
    return scrollView_.tick(timer);
}

void ReleaseNotesView::stop() {
    scrollY_ = scrollView_.offset();
    scrollView_.stop();
    scrollOnlyPaint_ = false;
}

void ReleaseNotesView::reset() {
    scrollView_.reset();
    parent_ = nullptr;
}

void ReleaseNotesView::setActiveTab(AboutPanelTab tab) {
    if (tab == activeTab_)
        return;
    activeTab_ = tab;
    scrollView_.setOffset(0.0f);
    scrollOnlyPaint_ = false;
    scrollY_ = 0.0f;
}

void ReleaseNotesView::setRelease(update_service::ReleaseInfo release) {
    release_ = std::move(release);
    blocks_ = markdown_doc::parseMarkdown(release_->body);
    message_.clear();
    if (activeTab_ == AboutPanelTab::ReleaseNotes) {
        scrollView_.setOffset(0.0f);
        scrollOnlyPaint_ = false;
        scrollY_ = 0.0f;
    }
}

void ReleaseNotesView::setMessage(const std::wstring& message) {
    release_.reset();
    blocks_.clear();
    message_ = message;
    if (activeTab_ == AboutPanelTab::ReleaseNotes) {
        scrollView_.setOffset(0.0f);
        scrollOnlyPaint_ = false;
        scrollY_ = 0.0f;
    }
}

void ReleaseNotesView::scrollBy(float delta) {
    scrollView_.scrollBy(delta);
}

bool ReleaseNotesView::beginScroll(float x, float y) {
    scrollY_ = scrollView_.offset();
    scrollView_.stop();
    if (maxScroll_ <= 0.0f || x < scrollTrack_.left - 4.5f ||
        x > scrollTrack_.right + 4.5f || y < scrollTrack_.top || y > scrollTrack_.bottom)
        return false;
    if (y >= scrollThumb_.top && y <= scrollThumb_.bottom) {
        dragging_ = true;
        dragOffset_ = y - scrollThumb_.top;
    } else {
        scrollY_ = std::clamp(scrollY_ + (y < scrollThumb_.top ? -viewHeight_ : viewHeight_),
                              0.0f, maxScroll_);
    }
    return true;
}

void ReleaseNotesView::dragTo(float y) {
    const float travel = scrollTrack_.bottom - scrollTrack_.top -
                         (scrollThumb_.bottom - scrollThumb_.top);
    if (dragging_ && travel > 0.0f)
        scrollY_ = std::clamp((y - scrollTrack_.top - dragOffset_) / travel,
                              0.0f, 1.0f) * maxScroll_;
}

void ReleaseNotesView::applyMarkdownStyles(IDWriteTextLayout* layout,
                                           const markdown_doc::MarkdownBlock& block,
                                           ID2D1SolidColorBrush* linkBrush) {
    for (const auto& span : block.spans) {
        if (span.length == 0)
            continue;

        const DWRITE_TEXT_RANGE range{
            static_cast<UINT32>(block.prefix.size() + span.start),
            static_cast<UINT32>(span.length)};
        if (span.style.bold)
            layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
        if (span.style.italic)
            layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        if (span.style.strike)
            layout->SetStrikethrough(TRUE, range);
        if (span.style.code) {
            layout->SetFontFamilyName(L"Consolas", range);
            layout->SetFontSize(fluent::font_sizes::code, range);
        }
        if (span.style.link) {
            layout->SetUnderline(TRUE, range);
            if (linkBrush)
                layout->SetDrawingEffect(linkBrush, range);
        }
    }
}

void ReleaseNotesView::paint(fluent::FluentDialogSurface::Painter& painter, const D2D1_RECT_F& rect,
                             float scrollbarRight) {
    if (rect.left != panelRect_.left || rect.top != panelRect_.top ||
        rect.right != panelRect_.right || rect.bottom != panelRect_.bottom)
        scrollOnlyPaint_ = false;
    panelRect_ = rect;
    scrollbarRight_ = scrollbarRight;
    if (scrollOnlyPaint_) {
        scrollOnlyPaint_ = false;
        drawScrollBar(painter, rect, scrollbarRight);
        return;
    }
    scrollTrack_ = {};
    scrollThumb_ = {};
    maxScroll_ = 0.0f;
    ID2D1RenderTarget* rt = painter.target();
    if (!rt)
        return;

    const auto& palette = fluent::palette();
    const float wDip = std::max(0.0f, rect.right - rect.left);
    const float hDip = std::max(0.0f, rect.bottom - rect.top);
    const D2D1_RECT_F panelRect = D2D1::RectF(
        rect.left + 0.5f, rect.top + 0.5f, std::max(rect.left + 0.5f, rect.right - 0.5f),
        std::max(rect.top + 0.5f, rect.bottom - 0.5f));
    painter.fillRoundRect(palette.cardFill, panelRect, fluent::metrics::cardRadius);
    painter.strokeRoundRect(palette.cardStroke, panelRect, 1.0f,
                            fluent::metrics::cardRadius);

    const bool showDisclaimer = activeTab_ == AboutPanelTab::Disclaimer;
    auto* bodyFormat = painter.textFormat(fluent::font_sizes::label, 400);
    if (bodyFormat)
        bodyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (!showDisclaimer && !release_) {
        scrollView_.layout({}, 0.0f, GetDpiForWindow(parent_));
        const std::wstring& message = message_.empty() ? emptyMessage() : message_;
        painter.drawText(message, bodyFormat,
                         D2D1::RectF(rect.left + 16.0f, rect.top + 12.0f,
                                     std::max(rect.left + 16.0f, rect.right - 16.0f),
                                     std::max(rect.top + 12.0f, rect.bottom - 12.0f)),
                         palette.textSecondary);
        contentHeight_ = hDip;
        return;
    }

    auto* factory = painter.dwrite();
    auto* headerFormat = painter.textFormat(fluent::font_sizes::body, 600);
    auto* codeFormat = painter.textFormat(fluent::font_sizes::code, 400, false, false, L"Consolas");
    auto* heading1Format = painter.textFormat(fluent::font_sizes::heading, 700);
    auto* heading2Format = painter.textFormat(fluent::font_sizes::subheading, 700);
    auto* heading3Format = painter.textFormat(fluent::font_sizes::body, 600);
    ID2D1SolidColorBrush* linkBrush = painter.createBrush(palette.accent);
    if (!factory || !bodyFormat || !headerFormat || !codeFormat || !heading1Format ||
        !heading2Format || !heading3Format)
        return;

    std::wstring header;
    if (showDisclaimer) {
        header = L"免责声明  ·  Apache-2.0";
    } else {
        const auto& release = *release_;
        header = update_service::displayVersion(release.version);
        if (!release.publishedAt.empty())
            header += L"  ·  " + update_service::displayReleaseDate(release.publishedAt);
    }
    painter.drawText(header, headerFormat,
                     D2D1::RectF(rect.left + 16.0f, rect.top + 8.0f,
                                 std::max(rect.left + 16.0f, rect.right - 16.0f),
                                 rect.top + 31.0f),
                     palette.text);
    if (auto* br = painter.brush(palette.separator)) {
        rt->DrawLine(D2D1::Point2F(rect.left + 16.0f, rect.top + 39.0f),
                     D2D1::Point2F(std::max(rect.left + 16.0f, rect.right - 16.0f),
                                   rect.top + 39.0f),
                     br, 1.0f);
    }

    const auto& activeBlocks = showDisclaimer ? ensureDisclaimerBlocks() : blocks_;
    const float bodyTop = rect.top + 48.0f;
    const float bodyViewHeight = std::max(0.0f, hDip - 48.0f);
    if (activeBlocks.empty()) {
        scrollView_.layout({}, 0.0f, GetDpiForWindow(parent_));
        painter.drawText(L"暂无发布说明", bodyFormat,
                         D2D1::RectF(rect.left + 16.0f, bodyTop,
                                     std::max(rect.left + 16.0f, rect.right - 16.0f),
                                     std::max(bodyTop + 18.0f, rect.bottom - 12.0f)),
                         palette.textSecondary);
        contentHeight_ = bodyViewHeight;
        return;
    }

    codeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    headerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    heading1Format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    heading2Format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    heading3Format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    struct RenderedBlock {
        const markdown_doc::MarkdownBlock* block = nullptr;
        IDWriteTextLayout* layout = nullptr;
        float height = 0.0f;
        float gap = 0.0f;
    };

    using Kind = markdown_doc::MarkdownBlockKind;
    std::vector<RenderedBlock> rendered;
    rendered.reserve(activeBlocks.size());
    float totalHeight = 0.0f;
    for (const auto& block : activeBlocks) {
        RenderedBlock item;
        item.block = &block;
        if (block.kind == Kind::Rule) {
            item.height = 12.0f;
            item.gap = 8.0f;
            rendered.push_back(item);
            totalHeight += item.height + item.gap;
            continue;
        }

        IDWriteTextFormat* format = bodyFormat;
        if (block.kind == Kind::Code) {
            format = codeFormat;
        } else if (block.kind == Kind::Heading) {
            format = block.level <= 1 ? heading1Format
                                      : block.level == 2 ? heading2Format : heading3Format;
        }
        const std::wstring displayText = block.prefix + block.text;
        const float textWidth = block.kind == Kind::Code
                                    ? std::max(1.0f, wDip - 48.0f)
                                    : std::max(1.0f, wDip - 32.0f);
        item.layout = painter.textLayout(displayText, format, textWidth, 100000.0f);
        if (item.layout) {
            DWRITE_TEXT_METRICS textMetrics{};
            item.layout->GetMetrics(&textMetrics);
            item.height = std::max(18.0f, textMetrics.height);
            if (block.kind == Kind::Code)
                item.height += 12.0f;
            if (block.kind != Kind::Code)
                applyMarkdownStyles(item.layout, block, linkBrush);
        } else {
            item.height = block.kind == Kind::Code ? 30.0f : 18.0f;
        }

        item.gap = block.kind == Kind::Code
                       ? 12.0f
                       : block.kind == Kind::Heading ? 8.0f : 6.0f;
        rendered.push_back(item);
        totalHeight += item.height + item.gap;
    }

    if (!rendered.empty())
        totalHeight = std::max(0.0f, totalHeight - rendered.back().gap);
    contentHeight_ = totalHeight;
    const float maxScroll = std::max(0.0f, contentHeight_ - bodyViewHeight);
    maxScroll_ = maxScroll;
    viewHeight_ = bodyViewHeight;
    scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll);

    scrollView_.layout(D2D1::RectF(rect.left + 2.0f, bodyTop,
                                   rect.right - 2.0f, rect.bottom - 2.0f),
                       std::max(0.0f, contentHeight_ - 2.0f), GetDpiForWindow(parent_));
    if (!scrollView_.animating())
        scrollView_.setOffset(scrollY_);
    scrollY_ = scrollView_.offset();
    scrollView_.paint([&](fluent::FluentDialogSurface::Painter& scrollPainter,
                          const D2D1_RECT_F& paintBounds, float position) {
        auto* scrollRt = scrollPainter.target();
        auto* scrollLinkBrush = scrollPainter.createBrush(palette.accent);
        scrollPainter.fillRoundRect(palette.cardFill, paintBounds, 0.0f);
        float y = bodyTop - position;
        for (const auto& item : rendered) {
            const auto& block = *item.block;
            if (block.kind == Kind::Rule) {
                if (y + item.height >= paintBounds.top && y <= paintBounds.bottom) {
                    if (auto* br = scrollPainter.brush(palette.separator)) {
                        const float lineY = y + item.height / 2.0f;
                        scrollRt->DrawLine(
                            D2D1::Point2F(rect.left + 16.0f, lineY),
                            D2D1::Point2F(std::max(rect.left + 16.0f, rect.right - 16.0f), lineY),
                            br, 1.0f);
                    }
                }
            } else if (item.layout && y + item.height >= paintBounds.top && y <= paintBounds.bottom) {
                applyMarkdownStyles(item.layout, block, scrollLinkBrush);
                if (block.kind == Kind::Code) {
                    const D2D1_RECT_F codeRect = D2D1::RectF(
                        rect.left + 16.0f, y, std::max(rect.left + 16.0f, rect.right - 16.0f),
                        y + item.height);
                    scrollPainter.fillRoundRect(palette.cardFillSolid, codeRect, 6.0f);
                    scrollPainter.strokeRoundRect(palette.cardStroke, codeRect, 1.0f, 6.0f);
                    scrollPainter.drawTextLayout(item.layout,
                                                 D2D1::Point2F(rect.left + 24.0f, y + 6.0f),
                                                 palette.text);
                } else {
                    scrollPainter.drawTextLayout(
                        item.layout, D2D1::Point2F(rect.left + 16.0f, y),
                        block.kind == Kind::Quote ? palette.textSecondary : palette.text);
                }
            }
            y += item.height + item.gap;
        }
    });
    drawScrollBar(painter, rect, scrollbarRight);
}

void ReleaseNotesView::drawScrollBar(fluent::FluentDialogSurface::Painter& painter,
                                     const D2D1_RECT_F& rect, float scrollbarRight) {
    const auto& palette = fluent::palette();
    const float bodyTop = rect.top + 48.0f;
    const float bodyViewHeight = std::max(0.0f, rect.bottom - bodyTop);
    const float maxScroll = maxScroll_;
    if (contentHeight_ > bodyViewHeight && bodyViewHeight > 0.0f) {
        const float trackTop = bodyTop + 8.0f;
        const float trackHeight = std::max(0.0f, bodyViewHeight - 16.0f);
        const float thumbHeight =
            std::min(trackHeight, std::max(24.0f, trackHeight * bodyViewHeight / contentHeight_));
        const float thumbTravel = std::max(0.0f, trackHeight - thumbHeight);
        const float thumbTop =
            trackTop + (maxScroll > 0.0f ? scrollY_ / maxScroll * thumbTravel : 0.0f);
        scrollTrack_ = D2D1::RectF(scrollbarRight - 3.0f, trackTop,
                                   scrollbarRight, trackTop + trackHeight);
        scrollThumb_ = D2D1::RectF(scrollTrack_.left, thumbTop,
                                   scrollTrack_.right, thumbTop + thumbHeight);
        painter.fillRoundRect(palette.textSecondary, scrollThumb_, 1.5f);
    }
}

const std::vector<markdown_doc::MarkdownBlock>& ReleaseNotesView::ensureDisclaimerBlocks() {
    if (disclaimerBlocks_.empty())
        disclaimerBlocks_ = markdown_doc::parseMarkdown(disclaimerMarkdown());
    return disclaimerBlocks_;
}

const std::wstring& ReleaseNotesView::emptyMessage() {
    static const std::wstring message = L"暂无更新日志";
    return message;
}
