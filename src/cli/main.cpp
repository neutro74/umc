#include <cstdlib>
#include <iostream>

#include "cli/dispatch.hpp"
#include "common/platform.hpp"

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    int pid = 0;
    if (args.size() >= 2 && args[0] == "-p") {
        pid = std::atoi(args[1].c_str());
        args.erase(args.begin(), args.begin() + 2);
    }
    // A bare `umc` in a terminal opens the interactive shell.
    if (args.empty() && umc::plat::stdin_is_tty()) args.push_back("shell");
    return umc::cli::dispatch(std::move(args), pid);
}
