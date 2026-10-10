// Mojang ProGuard mappings (named -> obfuscated). Class names are stored in internal form (a/b/C).
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace umc {

struct MemberMapping {
    std::string name, desc;          // named
    std::string obf_name, obf_desc;  // runtime
};

struct ClassMapping {
    std::string named, obf;
    std::vector<MemberMapping> fields, methods;
};

class Mappings {
public:
    bool load_proguard(const std::string& path, std::string& error);
    bool empty() const { return classes_.empty(); }
    size_t size() const { return classes_.size(); }

    // Unknown names pass through unchanged, which keeps unobfuscated versions working.
    std::string to_obf(const std::string& named) const;
    std::string to_named(const std::string& obf) const;
    std::string remap_desc(const std::string& named_desc) const;

    const ClassMapping* by_named(const std::string& named) const;
    // Empty desc returns every overload with that name.
    std::vector<const MemberMapping*> methods(const std::string& owner, const std::string& name,
                                              const std::string& desc) const;
    const MemberMapping* field(const std::string& owner, const std::string& name) const;

private:
    std::unordered_map<std::string, ClassMapping> classes_;
    std::unordered_map<std::string, std::string> obf_to_named_;
};

}  // namespace umc
