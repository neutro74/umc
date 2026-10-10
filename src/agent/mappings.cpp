#include "agent/mappings.hpp"

#include <fstream>

namespace umc {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

std::string internal(std::string s) {
    for (auto& c : s) if (c == '.') c = '/';
    return s;
}

// "int[]" / "net.minecraft.Foo" -> JVM descriptor
std::string type_desc(std::string t) {
    std::string prefix;
    while (t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0) {
        prefix += '[';
        t.resize(t.size() - 2);
    }
    static const std::unordered_map<std::string, std::string> prims = {
        {"void", "V"}, {"boolean", "Z"}, {"byte", "B"}, {"char", "C"}, {"short", "S"},
        {"int", "I"},  {"long", "J"},    {"float", "F"}, {"double", "D"}};
    auto p = prims.find(t);
    return prefix + (p != prims.end() ? p->second : "L" + internal(t) + ";");
}

// Strips ProGuard line-number decorations: "12:14:void foo(int):30:31" -> "void foo(int)"
std::string strip_lines(std::string s) {
    size_t i = 0;
    for (int colons = 0; colons < 2; ++colons) {
        size_t j = i;
        while (j < s.size() && isdigit(static_cast<unsigned char>(s[j]))) ++j;
        if (j == i || j >= s.size() || s[j] != ':') break;
        i = j + 1;
    }
    s = s.substr(i);
    size_t paren = s.rfind(')');
    if (paren != std::string::npos && paren + 1 < s.size() && s[paren + 1] == ':') s.resize(paren + 1);
    return s;
}

}  // namespace

bool Mappings::load_proguard(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }

    // Descriptors need the full class table, so members are converted after reading everything.
    struct Pending { std::string owner, type, name, args, obf; bool method; };
    std::vector<Pending> pending;
    std::string line, current;

    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t arrow = line.find(" -> ");
        if (arrow == std::string::npos) continue;

        if (line[0] != ' ' && line[0] != '\t') {
            std::string named = internal(trim(line.substr(0, arrow)));
            std::string obf = trim(line.substr(arrow + 4));
            if (!obf.empty() && obf.back() == ':') obf.pop_back();
            obf = internal(obf);
            current = named;
            classes_[named] = ClassMapping{named, obf, {}, {}};
            obf_to_named_[obf] = named;
            continue;
        }
        if (current.empty()) continue;

        // Generated legacy mappings: "!f name -> obf" (type unknown) and "!m (named desc) name -> obf".
        {
            std::string t = trim(line.substr(0, arrow));
            if (t.rfind("!f ", 0) == 0) {
                pending.push_back({current, "?", trim(t.substr(3)), "", trim(line.substr(arrow + 4)), false});
                continue;
            }
            if (t.rfind("!m ", 0) == 0) {
                size_t sp = t.find(' ', 3);
                if (sp != std::string::npos)
                    pending.push_back({current, "!", trim(t.substr(sp + 1)), t.substr(3, sp - 3), trim(line.substr(arrow + 4)), true});
                continue;
            }
        }
        std::string lhs = strip_lines(trim(line.substr(0, arrow)));
        std::string obf = trim(line.substr(arrow + 4));
        size_t space = lhs.find(' ');
        if (space == std::string::npos) continue;
        std::string type = lhs.substr(0, space), rest = lhs.substr(space + 1);
        size_t paren = rest.find('(');
        if (paren == std::string::npos) {
            pending.push_back({current, type, rest, "", obf, false});
        } else {
            std::string args = rest.substr(paren + 1, rest.size() - paren - 2);
            pending.push_back({current, type, rest.substr(0, paren), args, obf, true});
        }
    }

    for (auto& p : pending) {
        MemberMapping m;
        m.name = p.name;
        m.obf_name = p.obf;
        if (p.method && p.type == "!") {
            m.desc = p.args;  // already a JVM descriptor
        } else if (!p.method && p.type == "?") {
            m.desc = "";
        } else if (p.method) {
            std::string desc = "(";
            size_t start = 0;
            while (start < p.args.size()) {
                size_t comma = p.args.find(',', start);
                if (comma == std::string::npos) comma = p.args.size();
                desc += type_desc(trim(p.args.substr(start, comma - start)));
                start = comma + 1;
            }
            m.desc = desc + ")" + type_desc(p.type);
        } else {
            m.desc = type_desc(p.type);
        }
        m.obf_desc = m.desc.empty() ? "" : remap_desc(m.desc);
        auto& cls = classes_[p.owner];
        (p.method ? cls.methods : cls.fields).push_back(std::move(m));
    }

    if (classes_.empty()) {
        error = path + " contains no class mappings";
        return false;
    }
    return true;
}

std::string Mappings::to_obf(const std::string& named) const {
    auto it = classes_.find(named);
    return it == classes_.end() ? named : it->second.obf;
}

std::string Mappings::to_named(const std::string& obf) const {
    auto it = obf_to_named_.find(obf);
    return it == obf_to_named_.end() ? obf : it->second;
}

std::string Mappings::remap_desc(const std::string& desc) const {
    std::string out;
    for (size_t i = 0; i < desc.size(); ++i) {
        out += desc[i];
        if (desc[i] != 'L') continue;
        size_t end = desc.find(';', i);
        if (end == std::string::npos) break;
        out += to_obf(desc.substr(i + 1, end - i - 1)) + ";";
        i = end;
    }
    return out;
}

const ClassMapping* Mappings::by_named(const std::string& named) const {
    auto it = classes_.find(named);
    return it == classes_.end() ? nullptr : &it->second;
}

std::vector<const MemberMapping*> Mappings::methods(const std::string& owner, const std::string& name,
                                                    const std::string& desc) const {
    std::vector<const MemberMapping*> out;
    if (auto* c = by_named(owner)) {
        for (auto& m : c->methods)
            if (m.name == name && (desc.empty() || m.desc == desc)) out.push_back(&m);
    }
    return out;
}

const MemberMapping* Mappings::field(const std::string& owner, const std::string& name) const {
    if (auto* c = by_named(owner))
        for (auto& f : c->fields)
            if (f.name == name) return &f;
    return nullptr;
}

}  // namespace umc
