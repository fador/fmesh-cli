#include "tui/command.h"
#include "tui/input_line.h"
#include "tui/window_manager.h"
#include "tui/window.h"
#include "mesh/mesh_service.h"
#include "mesh/node_db.h"
#include "store/database.h"

#include "minitest.h"

#include <algorithm>

using namespace meshcli;

namespace {
    // Minimal MeshService with a connected device for command tests.
    struct TestMeshService : public MeshService {
        // Expose ability to inject a DeviceRuntime directly.
        using MeshService::devices_for_test;
        using MeshService::devices_mu_for_test;
    };
} // namespace

// -- InputLine edge cases --

TEST(InputLineRobust, MaxLineLength) {
    InputLine input;
    std::string out;
    // Fill with printable chars up to and beyond kMaxLineSize.
    for (size_t i = 0; i < InputLine::kMaxLineSize + 100; ++i) {
        input.handle_key('x', out);
    }
    EXPECT_TRUE(out.empty()); // never submitted
    EXPECT_EQ(input.buf().size(), InputLine::kMaxLineSize);
}

TEST(InputLineRobust, EmptySubmitDoesNothing) {
    InputLine input;
    std::string out;
    EXPECT_FALSE(input.handle_key('\n', out));
    EXPECT_TRUE(out.empty());
}

TEST(InputLineRobust, BackspaceAtStart) {
    InputLine input;
    std::string out;
    input.handle_key('a', out);
    input.handle_key('b', out);
    input.handle_key(KEY_BACKSPACE, out);
    input.handle_key(KEY_BACKSPACE, out);
    input.handle_key(KEY_BACKSPACE, out); // extra backspace
    EXPECT_EQ(input.buf(), "");
}

TEST(InputLineRobust, DeleteAtEnd) {
    InputLine input;
    std::string out;
    input.handle_key('a', out);
    input.handle_key(KEY_DC, out); // delete when cursor at end
    EXPECT_EQ(input.buf(), "a");
}

TEST(InputLineRobust, CtrlWDeletesWord) {
    InputLine input;
    std::string out;
    input.handle_key('h', out);
    input.handle_key('i', out);
    input.handle_key(' ', out);
    input.handle_key('t', out);
    input.handle_key('h', out);
    // buf is "hi th", cursor at end
    input.handle_key(23, out); // Ctrl-W
    EXPECT_EQ(input.buf(), "hi ");
}

TEST(InputLineRobust, CtrlU) {
    InputLine input;
    std::string out;
    input.handle_key('t', out);
    input.handle_key('e', out);
    input.handle_key('s', out);
    input.handle_key('t', out);
    input.handle_key(21, out); // Ctrl-U
    EXPECT_TRUE(input.buf().empty());
    EXPECT_EQ(input.cursor(), 0u);
}

TEST(InputLineRobust, CtrlK) {
    InputLine input;
    std::string out;
    input.handle_key('a', out);
    input.handle_key('b', out);
    // Move cursor to start (KEY_HOME)
    input.handle_key(KEY_HOME, out);
    input.handle_key(11, out); // Ctrl-K kills to end
    EXPECT_TRUE(input.buf().empty());
}

TEST(InputLineRobust, CtrlA) {
    InputLine input;
    std::string out;
    input.handle_key('x', out);
    input.handle_key('y', out);
    // Move cursor to 1
    input.handle_key(KEY_LEFT, out);
    input.handle_key(1, out); // Ctrl-A = home
    EXPECT_EQ(input.cursor(), 0u);
}

TEST(InputLineRobust, CtrlE) {
    InputLine input;
    std::string out;
    input.handle_key('x', out);
    input.handle_key('y', out);
    input.handle_key(1, out); // Ctrl-A
    EXPECT_EQ(input.cursor(), 0u);
    input.handle_key(5, out); // Ctrl-E = end
    EXPECT_EQ(input.cursor(), 2u);
}

TEST(InputLineRobust, ArrowNavigation) {
    InputLine input;
    std::string out;
    input.handle_key('a', out);
    input.handle_key('b', out);
    input.handle_key('c', out);
    input.handle_key(KEY_LEFT, out);
    EXPECT_EQ(input.cursor(), 2u);
    input.handle_key(KEY_LEFT, out);
    EXPECT_EQ(input.cursor(), 1u);
    input.handle_key(KEY_RIGHT, out);
    EXPECT_EQ(input.cursor(), 2u);
    input.handle_key(KEY_HOME, out);
    EXPECT_EQ(input.cursor(), 0u);
    input.handle_key(KEY_END, out);
    EXPECT_EQ(input.cursor(), 3u);
    // Left from start should not go negative
    input.handle_key(KEY_LEFT, out);
    EXPECT_EQ(input.cursor(), 2u);
}

TEST(InputLineRobust, HistoryNavigates) {
    InputLine input;
    std::string out;
    // Submit line 1
    input.handle_key('f', out); input.handle_key('o', out); input.handle_key('o', out);
    EXPECT_TRUE(input.handle_key('\n', out));
    EXPECT_EQ(out, "foo");
    // Submit line 2
    input.handle_key('b', out); input.handle_key('a', out); input.handle_key('r', out);
    EXPECT_TRUE(input.handle_key('\n', out));
    EXPECT_EQ(out, "bar");
    // Up twice
    input.handle_key(KEY_UP, out);
    EXPECT_EQ(input.buf(), "bar");
    input.handle_key(KEY_UP, out);
    EXPECT_EQ(input.buf(), "foo");
    // Down
    input.handle_key(KEY_DOWN, out);
    EXPECT_EQ(input.buf(), "bar");
    // Down past end clears
    input.handle_key(KEY_DOWN, out);
    EXPECT_TRUE(input.buf().empty());
}

// -- Command edge cases --

struct CmdFixture {
    MeshService svc;
    WindowManager wm;
    std::vector<std::string> status_lines;
    int status_color = 0;

    CmdFixture() : wm(svc) {
        SetUp();
    }

    void SetUp() {
        status_lines.clear();
        // Inject a fake device into the service.
        DeviceRuntime rt;
        rt.display_name = "dev1";
        rt.my_node_num = 0x1234u;
        rt.config_complete = true;
        auto db = std::make_unique<NodeDb>();
        Node me; me.node_num = 0x1234u; me.long_name = "MyNode"; me.short_name = "MN";
        db->upsert_node(me);
        Node peer; peer.node_num = 0x5678u; peer.long_name = "Bob"; peer.short_name = "B";
        db->upsert_node(peer);
        rt.db = std::move(db);
        {
            std::lock_guard<std::mutex> lock(svc.devices_mu_for_test());
            svc.devices_for_test()["/test/dev1"] = std::make_shared<DeviceRuntime>(std::move(rt));
        }
    }

    void TearDown() {}

    CommandResult exec(const std::string& cmd) {
        CommandDispatcher disp(svc, wm, [this](const std::string& s, int c) {
            status_lines.push_back(s);
            status_color = c;
        }, "");
        return disp.execute(cmd);
    }
};

TEST_F(CmdFixture, UnknownCommand) {
    auto res = exec("/bogus");
    ASSERT_FALSE(status_lines.empty());
    EXPECT_FALSE(std::string::npos == status_lines.back().find("Unknown command"));
}

TEST_F(CmdFixture, EmptyCommand) {
    auto res = exec("");
    EXPECT_FALSE(res.quit);
}

TEST_F(CmdFixture, ChannelCommandNoArgs) {
    auto res = exec("/channel");
    ASSERT_FALSE(status_lines.empty());
    EXPECT_FALSE(std::string::npos == status_lines.back().find("Usage"));
}

TEST_F(CmdFixture, WhoisNoArgs) {
    auto res = exec("/whois");
    ASSERT_FALSE(status_lines.empty());
    EXPECT_FALSE(std::string::npos == status_lines.back().find("Usage"));
}

TEST_F(CmdFixture, WhoisInvalidNode) {
    auto res = exec("/whois NonexistentNode12345");
    ASSERT_FALSE(status_lines.empty());
    EXPECT_FALSE(std::string::npos == status_lines.back().find("No node matched"));
}

TEST_F(CmdFixture, QueryNoArgs) {
    auto res = exec("/query");
    ASSERT_FALSE(status_lines.empty());
    EXPECT_FALSE(std::string::npos == status_lines.back().find("Usage"));
}

TEST_F(CmdFixture, RawNegativeCount) {
    auto res = exec("/raw -5");
    // Should either fail or clamp; at minimum should not crash.
    EXPECT_FALSE(res.quit);
}

TEST_F(CmdFixture, RawLargeCount) {
    auto res = exec("/raw 999999");
    EXPECT_FALSE(res.quit); // should clamp to 50
}

TEST_F(CmdFixture, LastlogNoArgs) {
    auto res = exec("/lastlog");
    ASSERT_FALSE(status_lines.empty());
    EXPECT_FALSE(std::string::npos == status_lines.back().find("Usage"));
}

TEST_F(CmdFixture, MeNoArgs) {
    // /me with no text from status window
    auto res = exec("/me");
    EXPECT_FALSE(res.quit);
}

TEST_F(CmdFixture, MsgNoArgs) {
    auto res = exec("/msg");
    ASSERT_FALSE(status_lines.empty());
    EXPECT_FALSE(std::string::npos == status_lines.back().find("Usage"));
}

TEST_F(CmdFixture, TextFromStatusBlocked) {
    // Switch to status window (index 1)
    wm.select(1);
    auto res = exec("hello world");
    ASSERT_FALSE(status_lines.empty());
    EXPECT_FALSE(std::string::npos == status_lines.back().find("Cannot send text"));
}

TEST_F(CmdFixture, TextInChannelSends) {
    // Create a channel window and switch to it
    wm.ensure_channel("/test/dev1", 0, "Channel0");
    wm.select(static_cast<int>(wm.windows().size()));
    auto res = exec("hello channel");
    EXPECT_FALSE(res.quit);
}

TEST_F(CmdFixture, QuitFlags) {
    auto res = exec("/quit");
    EXPECT_TRUE(res.quit);
}

TEST_F(CmdFixture, ExitAlias) {
    auto res = exec("/exit");
    EXPECT_TRUE(res.quit);
}

TEST_F(CmdFixture, SlashOnly) {
    auto res = exec("/");
    EXPECT_FALSE(res.quit);
}

// -- Window manager edge cases --

TEST(WindowManagerEdge, CurrentTargetOnStatusWindow) {
    MeshService svc;
    WindowManager wm(svc);
    // Window 1 is the status window.
    EXPECT_EQ(wm.current_target(), nullptr);
}

TEST(WindowManagerEdge, SelectInvalidIndex) {
    MeshService svc;
    WindowManager wm(svc);
    auto* before = wm.current_window();
    wm.select(0);
    EXPECT_EQ(wm.current_window(), before);
    wm.select(999);
    EXPECT_EQ(wm.current_window(), before);
    wm.select(-1);
    EXPECT_EQ(wm.current_window(), before);
}

TEST(WindowManagerEdge, SelectRelativeWrapsEnd) {
    MeshService svc;
    WindowManager wm(svc);
    int n = static_cast<int>(wm.windows().size());
    // select past last wraps to first
    wm.select_relative(10);
    EXPECT_EQ(wm.current_index(), 1);
    // select before first wraps to last
    wm.select_relative(-2);
    EXPECT_EQ(wm.current_index(), n);
}

TEST(WindowManagerEdge, NextActiveWithNoActivity) {
    MeshService svc;
    WindowManager wm(svc);
    // No windows have activity; should stay on current.
    int before = wm.current_index();
    wm.select_next_active();
    EXPECT_EQ(wm.current_index(), before);
}

// -- Window edge cases --

TEST(WindowRobust, ScrollbackCap) {
    Window w(WindowTarget{"dev", "channel", 1}, "#test");
    // Append more than 5000 lines (the internal cap).
    for (int i = 0; i < 6000; ++i) {
        Line l{"msg " + std::to_string(i), 2, false};
        w.append_line(l);
    }
    ASSERT_TRUE(w.lines().size() <= 5000u);
}

TEST(WindowRobust, ScrollClamp) {
    Window w(WindowTarget{"dev", "channel", 1}, "#test");
    for (int i = 0; i < 100; ++i)
        w.append_line(Line{"msg " + std::to_string(i), 2, false});
    // Scroll before start
    w.scroll_by(-200);
    ASSERT_EQ(w.scroll_offset(), 0);
    // Scroll past end
    w.scroll_by(300);
    ASSERT_EQ(w.scroll_offset(), 100);
    // Scroll back to bottom
    w.scroll_to_bottom();
    ASSERT_EQ(w.scroll_offset(), 0);
}

TEST(WindowRobust, ClearAndReappend) {
    Window w(WindowTarget{"dev", "dm", 1}, "Bob");
    w.append_line(Line{"hello", 3, false});
    w.append_line(Line{"world", 3, false});
    w.clear();
    EXPECT_TRUE(w.lines().empty());
    w.append_line(Line{"after clear", 3, false});
    EXPECT_EQ(w.lines().size(), 1u);
}

// -- MeshService edge cases without real transport --

TEST(MeshServiceEdge, SendTextNoDevices) {
    MeshService svc;
    auto pid = svc.send_text("nonexistent", 0xFFFFFFFFu, 0, "hello", false);
    EXPECT_EQ(pid, 0u);
}

TEST(MeshServiceEdge, DeviceIdsEmpty) {
    MeshService svc;
    EXPECT_TRUE(svc.device_ids().empty());
}

TEST(MeshServiceEdge, DbForUnknownDevice) {
    MeshService svc;
    EXPECT_EQ(svc.db_for("nonexistent"), nullptr);
}

TEST(MeshServiceEdge, FirmwareForUnknownDevice) {
    MeshService svc;
    EXPECT_TRUE(svc.firmware_for("nonexistent").empty());
}

TEST(MeshServiceEdge, ConfigLinesForUnknownDevice) {
    MeshService svc;
    EXPECT_TRUE(svc.config_lines_for("nonexistent").empty());
}
