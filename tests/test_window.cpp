#include "tui/window.h"
#include "tui/window_manager.h"
#include "mesh/mesh_service.h"

#include <gtest/gtest.h>

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
