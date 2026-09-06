#include "tui/window.h"
#include "tui/window_manager.h"
#include "mesh/mesh_service.h"

#include "minitest.h"

using namespace meshcli;

TEST(Window, AppendAndScroll) {
    Window w(WindowTarget{"d", "status", 0}, "status");
    w.append_line({"line 1", 0, false});
    w.append_line({"line 2", 0, false});
    EXPECT_EQ(w.lines().size(), 2u);
    EXPECT_EQ(w.scroll_offset(), 0);
    w.scroll_by(1);
    EXPECT_EQ(w.scroll_offset(), 1);
    w.scroll_by(-5);   // clamps to 0
    EXPECT_EQ(w.scroll_offset(), 0);
}

TEST(Window, ActivityMarks) {
    Window w(WindowTarget{"d", "channel", 0}, "#primary");
    EXPECT_EQ(w.activity(), 0);
    w.bump_activity(1);
    EXPECT_EQ(w.activity(), 1);
    EXPECT_EQ(w.unread(), 1);
    w.bump_activity(2);
    EXPECT_EQ(w.activity(), 2);
    w.mark_read();
    EXPECT_EQ(w.activity(), 0);
    EXPECT_EQ(w.unread(), 0);
}

TEST(WindowManager, StatusWindowIsFirst) {
    MeshService svc;
    WindowManager wm(svc);
    EXPECT_EQ(wm.windows().size(), 1u);
    EXPECT_EQ(wm.current_index(), 1);
    EXPECT_EQ(wm.windows()[0]->title(), "status");
}

TEST(WindowManager, EnsureChannelCreatesWindow) {
    MeshService svc;
    WindowManager wm(svc);
    int idx = wm.ensure_channel("dev1", 0, "primary");
    EXPECT_EQ(idx, 2);
    idx = wm.ensure_channel("dev1", 0, "primary");
    EXPECT_EQ(idx, 2);  // idempotent
    idx = wm.ensure_channel("dev1", 1, "chat");
    EXPECT_EQ(idx, 3);
    EXPECT_EQ(wm.windows().size(), 3u);
}

TEST(WindowManager, EnsureDmCreatesWindow) {
    MeshService svc;
    WindowManager wm(svc);
    int idx = wm.ensure_dm("dev1", 0xdeadbeef, "Bob");
    EXPECT_EQ(idx, 2);
    EXPECT_EQ(wm.windows()[1]->title(), "Bob");
    // Same node -> same window.
    idx = wm.ensure_dm("dev1", 0xdeadbeef, "Bob2");
    EXPECT_EQ(idx, 2);
}

TEST(WindowManager, CurrentTargetStatusIsNull) {
    MeshService svc;
    WindowManager wm(svc);
    EXPECT_EQ(wm.current_target(), nullptr);
    wm.ensure_channel("dev1", 0, "primary");
    wm.select(2);
    auto* t = wm.current_target();
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind, "channel");
    EXPECT_EQ(t->target, 0u);
}

TEST(WindowManager, SelectRelativeWraps) {
    MeshService svc;
    WindowManager wm(svc);
    wm.ensure_channel("d", 0, "a");   // 2
    wm.ensure_channel("d", 1, "b");   // 3
    wm.select(1);
    wm.select_relative(-1);
    EXPECT_EQ(wm.current_index(), 3);
    wm.select_relative(1);
    EXPECT_EQ(wm.current_index(), 1);
}

TEST(WrapText, FitsInWidth) {
    auto res = wrap_text("hello world", 20);
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0], "hello world");

    res = wrap_text("12345", 5);
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0], "12345");
}

TEST(WrapText, WordBoundaryWrap) {
    std::string text = "the quick brown fox jumps over the lazy dog";
    auto res = wrap_text(text, 20);
    // Line 0: "the quick brown fox" (19 chars <= 20)
    // Continuation line: indented by 2 spaces ("  ")
    // Line 1: "  jumps over the" (16 chars <= 20)
    // Line 2: "  lazy dog" (10 chars <= 20)
    ASSERT_EQ(res.size(), 3u);
    EXPECT_EQ(res[0], "the quick brown fox");
    EXPECT_EQ(res[1], "  jumps over the");
    EXPECT_EQ(res[2], "  lazy dog");
}

TEST(WrapText, LongTokenHardWrap) {
    std::string text = "http://example.com/very/long/url/without/spaces";
    auto res = wrap_text(text, 16);
    // First line 16 chars: "http://example.c"
    // Next line: indent "  " (2 chars) + 14 chars = 16 chars: "  om/very/long/u"
    // Next line: indent "  " + 14 chars: "  rl/without/spa"
    // Next line: indent "  " + 3 chars: "  ces"
    ASSERT_GT(res.size(), 1u);
    for (const auto& line : res) {
        EXPECT_TRUE(static_cast<int>(line.size()) <= 16);
    }
}

TEST(WrapText, MultilineWithNewlines) {
    std::string text = "line one\nline two is longer and should wrap";
    auto res = wrap_text(text, 20);
    ASSERT_GT(res.size(), 2u);
    EXPECT_EQ(res[0], "line one");
    EXPECT_EQ(res[1], "line two is longer");
    EXPECT_EQ(res[2], "  and should wrap");
}

TEST(WrapText, EdgeCases) {
    auto res = wrap_text("", 20);
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0], "");

    res = wrap_text("test", 0);
    EXPECT_EQ(res.size(), 0u);
}

