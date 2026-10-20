#include "minitest.h"

#ifndef _WIN32
#include <csignal>
#endif

int main(int argc, char** argv) {
#ifndef _WIN32
    // Writes to closed sockets must not terminate the process (see main.cpp).
    std::signal(SIGPIPE, SIG_IGN);
#endif

    std::string filter;
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg.rfind("--filter=", 0) == 0) {
            filter = arg.substr(9);
        } else if (arg.rfind("--gtest_filter=", 0) == 0) {
            filter = arg.substr(15);
            while (!filter.empty() && filter.front() == '*') filter.erase(filter.begin());
            while (!filter.empty() && filter.back() == '*') filter.pop_back();
        } else {
            filter = arg;
        }
    }
    return minitest::run(filter);
}
