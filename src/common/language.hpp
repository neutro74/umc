#pragma once
#include <string>
#include "common/script_meta.hpp"
namespace umc::lang {
constexpr size_t max_source_bytes = 256 * 1024;
struct Compiled { ScriptMeta meta; std::string code; };
// Parses the whole file. Only compiler-generated code may enter the embedded VM.
bool compile(const std::string& source, Compiled& out, std::string& error);
std::string quote(const std::string& text);
}
