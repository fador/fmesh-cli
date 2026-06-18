#pragma once

#include <functional>
#include <string>
#include <vector>

namespace meshcli {

class MeshService;
class WindowManager;

struct CommandResult {
    bool handled = true;
    bool quit = false;
    std::string message;       // status-window message on success/failure
    int color_pair = 0;
};

// Parses and executes a /command line (or routes plain text to send).
// Bound to the current window's device + target so e.g. plain text in a
// channel window broadcasts on that channel.
class CommandDispatcher {
public:
    using StatusSink = std::function<void(const std::string&, int)>;

    CommandDispatcher(MeshService& service, WindowManager& wm, StatusSink status,
                      std::string& active_device,
                      std::function<void()> on_scan = {},
                      std::function<bool(const std::string&)> on_set_theme = {});

    // Execute a full input line (may be a command or plain text). Returns
    // a CommandResult (quit flag etc.).
    CommandResult execute(const std::string& line);

private:
    MeshService& service_;
    WindowManager& wm_;
    StatusSink status_;
    std::string& active_device_;
    std::function<void()> on_scan_;
    std::function<bool(const std::string&)> on_set_theme_;

    void cmd_help();
    void cmd_list();
    void cmd_nodes();
    void cmd_query(const std::vector<std::string>& args);
    void cmd_msg(const std::vector<std::string>& args);
    void cmd_close();
    void cmd_window(const std::vector<std::string>& args);
    void cmd_channel(const std::vector<std::string>& args);
    void cmd_clear();
    void cmd_info();
    void cmd_quit(CommandResult& res);
    void cmd_reconnect(const std::vector<std::string>& args);
    void cmd_device(const std::vector<std::string>& args);
    void cmd_me(const std::vector<std::string>& args);
    void cmd_config(const std::vector<std::string>& args);
    void cmd_whois(const std::vector<std::string>& args);
    void cmd_raw(const std::vector<std::string>& args);
    void cmd_stats();
    void cmd_topic();
    void cmd_lastlog(const std::vector<std::string>& args);
    void cmd_connect(const std::vector<std::string>& args);
    void cmd_disconnect(const std::vector<std::string>& args);
    void cmd_scan();
    void cmd_theme(const std::vector<std::string>& args);
};

} // namespace meshcli
