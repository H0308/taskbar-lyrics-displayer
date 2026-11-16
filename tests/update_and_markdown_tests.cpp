#include "test_support.h"

#include "ui/markdown_document.h"
#include "update/update_service.h"

TEST_CASE("update_service.version_parsing_and_comparison") {
    const auto v1 = update_service::parseVersion(L"v2.15.0");
    const auto v2 = update_service::parseVersion(L"2.15.1");
    const auto v3 = update_service::parseVersion(L"V2.15");
    EXPECT_TRUE(v1.has_value());
    EXPECT_TRUE(v2.has_value());
    EXPECT_TRUE(v3.has_value());
    EXPECT_EQ(update_service::compareVersions(*v1, *v3), 0);
    EXPECT_TRUE(update_service::compareVersions(*v1, *v2) < 0);
    EXPECT_TRUE(update_service::compareVersions(*v2, *v1) > 0);

    EXPECT_FALSE(update_service::parseVersion(L"").has_value());
    EXPECT_FALSE(update_service::parseVersion(L"v").has_value());
    EXPECT_FALSE(update_service::parseVersion(L"2..1").has_value());
    EXPECT_FALSE(update_service::parseVersion(L"2.15.0-rc1").has_value());

    EXPECT_EQ(update_service::displayVersion(L"v2.15.0"), std::wstring(L"2.15.0"));
    EXPECT_EQ(update_service::displayVersion(L"2.15.0"), std::wstring(L"2.15.0"));
    EXPECT_EQ(update_service::displayReleaseDate(L"2026-11-16T10:00:00Z"),
              std::wstring(L"2026-11-16"));
}

TEST_CASE("update_service.proxy_selection_and_release_json_parsing") {
    EXPECT_EQ(update_service::selectHttpsProxy(L"http=127.0.0.1:7890;https=127.0.0.1:7891"),
              std::wstring(L"127.0.0.1:7891"));
    EXPECT_EQ(update_service::selectHttpsProxy(L"socks=127.0.0.1:1080"),
              std::wstring(L"socks5h://127.0.0.1:1080"));
    EXPECT_EQ(update_service::proxyUrlForCurl(L"127.0.0.1:7890"),
              std::string("http://127.0.0.1:7890"));
    EXPECT_EQ(update_service::proxyUrlForCurl(L"socks5h://127.0.0.1:1080"),
              std::string("socks5h://127.0.0.1:1080"));

    const auto noRelease = update_service::parseLatestReleaseResponse(
        1, update_service::UpdateSource::GitHub, 404, "", L"2.15.0");
    EXPECT_EQ(noRelease.state, update_service::UpdateState::NoRelease);

    const std::string body = R"({
        "tag_name": "v2.16.0",
        "published_at": "2026-11-16T12:00:00Z",
        "body": "## 更新说明\n- 新增特性",
        "assets": [
            {
                "name": "other.zip",
                "browser_download_url": "https://example.com/other.zip",
                "size": 100
            },
            {
                "name": "WindowsTaskbarLyrics-2.16.0-Setup.exe",
                "browser_download_url": "https://example.com/WindowsTaskbarLyrics-2.16.0-Setup.exe",
                "size": 1234567
            }
        ]
    })";
    const auto available = update_service::parseLatestReleaseResponse(
        42, update_service::UpdateSource::Gitee, 200, body, L"2.15.0");
    EXPECT_EQ(available.requestId, 42ull);
    EXPECT_EQ(available.state, update_service::UpdateState::Available);
    EXPECT_EQ(available.version, std::wstring(L"v2.16.0"));
    EXPECT_EQ(available.releaseUrl,
              std::wstring(update_service::updateSourceReleasePage(
                  update_service::UpdateSource::Gitee)));
    EXPECT_TRUE(available.release.has_value());
    EXPECT_EQ(available.release->installerName,
              std::wstring(L"WindowsTaskbarLyrics-2.16.0-Setup.exe"));
    EXPECT_EQ(available.release->installerSize, 1234567);

    const auto current = update_service::parseLatestReleaseResponse(
        43, update_service::UpdateSource::GitHub, 200, body, L"2.16.0");
    EXPECT_EQ(current.state, update_service::UpdateState::Current);
}

TEST_CASE("markdown_document.blocks_and_inline_styles") {
    const std::wstring md =
        L"# 主标题\n"
        L"\n"
        L"正文包含 **粗体**、*斜体*、~~删除~~、`snake_case` 与 [链接](https://example.com)。\n"
        L"\n"
        L"- [x] 已完成任务\n"
        L"  1. 有序子项\n"
        L"> 引用内容\n"
        L"---\n"
        L"```cpp\n"
        L"int x = 1;\n"
        L"```\n";

    const auto blocks = markdown_doc::parseMarkdown(md);
    EXPECT_EQ(blocks.size(), 7u);
    EXPECT_EQ(blocks[0].kind, markdown_doc::MarkdownBlockKind::Heading);
    EXPECT_EQ(blocks[0].level, 1);
    EXPECT_EQ(blocks[0].text, std::wstring(L"主标题"));

    EXPECT_EQ(blocks[1].kind, markdown_doc::MarkdownBlockKind::Paragraph);
    bool sawBold = false;
    bool sawItalic = false;
    bool sawStrike = false;
    bool sawCode = false;
    bool sawLink = false;
    for (const auto& span : blocks[1].spans) {
        sawBold |= span.style.bold;
        sawItalic |= span.style.italic;
        sawStrike |= span.style.strike;
        sawCode |= span.style.code;
        sawLink |= span.style.link;
    }
    EXPECT_TRUE(sawBold);
    EXPECT_TRUE(sawItalic);
    EXPECT_TRUE(sawStrike);
    EXPECT_TRUE(sawCode);
    EXPECT_TRUE(sawLink);
    EXPECT_TRUE(blocks[1].text.find(L"snake_case") != std::wstring::npos);

    EXPECT_EQ(blocks[2].kind, markdown_doc::MarkdownBlockKind::Bullet);
    EXPECT_EQ(blocks[2].prefix, std::wstring(L"☑ "));
    EXPECT_EQ(blocks[2].text, std::wstring(L"已完成任务"));

    EXPECT_EQ(blocks[3].kind, markdown_doc::MarkdownBlockKind::Ordered);
    EXPECT_EQ(blocks[3].number, 1);
    EXPECT_EQ(blocks[3].prefix, std::wstring(L"  1. "));

    EXPECT_EQ(blocks[4].kind, markdown_doc::MarkdownBlockKind::Quote);
    EXPECT_EQ(blocks[5].kind, markdown_doc::MarkdownBlockKind::Rule);
    EXPECT_EQ(blocks[6].kind, markdown_doc::MarkdownBlockKind::Code);
    EXPECT_EQ(blocks[6].text, std::wstring(L"int x = 1;"));
}
