// Stress test: hooks every method of every class file under a directory, rewriting in place.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

#include "agent/classfile.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: rewrite_all <dir>\n";
        return 2;
    }
    size_t classes = 0, methods = 0, failed = 0;
    for (auto& e : std::filesystem::recursive_directory_iterator(argv[1])) {
        if (e.path().extension() != ".class") continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
        std::vector<uint8_t> out;
        std::vector<std::string> patched;
        std::string err;
        if (!umc::inject_hooks(data.data(), data.size(), [](auto&, auto&) { return 0; }, out, patched, err)) {
            std::cerr << e.path() << ": " << err << "\n";
            ++failed;
            continue;
        }
        std::ofstream(e.path(), std::ios::binary).write(reinterpret_cast<char*>(out.data()), long(out.size()));
        ++classes;
        methods += patched.size();
    }
    std::cout << "rewrote " << methods << " methods in " << classes << " classes, " << failed << " failed\n";
    return failed ? 1 : 0;
}
