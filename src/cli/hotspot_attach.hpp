// HotSpot's Unix attach protocol (what jcmd uses), shared by Linux and macOS.
#pragma once

#include <string>
#include <vector>

namespace umc::cli {

struct AttachTarget {
    int pid;                               // pid to signal
    std::string socket;                    // .java_pid<ns_pid> as seen from here
    std::vector<std::string> triggers;     // candidate .attach_pid<ns_pid> files, tried in order
};

bool hotspot_load(const AttachTarget& t, const std::string& library, const std::string& options,
                  std::string& message);

}  // namespace umc::cli
