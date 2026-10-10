#include "agent/classfile.hpp"

#include <algorithm>
#include <stdexcept>

namespace umc {

namespace {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint8_t u1() { need(1); return p_[pos_++]; }
    uint16_t u2() { need(2); uint16_t v = uint16_t(p_[pos_] << 8 | p_[pos_ + 1]); pos_ += 2; return v; }
    uint32_t u4() { uint32_t hi = u2(); return hi << 16 | u2(); }
    std::vector<uint8_t> bytes(size_t k) {
        need(k);
        std::vector<uint8_t> v(p_ + pos_, p_ + pos_ + k);
        pos_ += k;
        return v;
    }
    bool done() const { return pos_ == n_; }

private:
    void need(size_t k) { if (pos_ + k > n_) throw ParseError("truncated class file"); }
    const uint8_t* p_;
    size_t n_, pos_ = 0;
};

struct Writer {
    std::vector<uint8_t> b;
    void u1(unsigned v) { b.push_back(uint8_t(v)); }
    void u2(unsigned v) { u1(v >> 8); u1(v); }
    void u4(uint32_t v) { u2(v >> 16); u2(v & 0xffff); }
    void put(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
};

struct Attr {
    uint16_t name;
    std::vector<uint8_t> data;
};

std::vector<Attr> read_attrs(Reader& r) {
    std::vector<Attr> attrs(r.u2());
    for (auto& a : attrs) {
        a.name = r.u2();
        a.data = r.bytes(r.u4());
    }
    return attrs;
}

void write_attrs(Writer& w, const std::vector<Attr>& attrs) {
    w.u2(unsigned(attrs.size()));
    for (auto& a : attrs) {
        w.u2(a.name);
        w.u4(uint32_t(a.data.size()));
        w.put(a.data);
    }
}

enum : uint8_t {
    kUtf8 = 1, kInt = 3, kFloat = 4, kLong = 5, kDouble = 6, kClass = 7, kString = 8,
    kField = 9, kMethod = 10, kIMethod = 11, kNameType = 12, kHandle = 15, kMethodType = 16,
    kDynamic = 17, kInvokeDynamic = 18, kModule = 19, kPackage = 20,
};

class ConstantPool {
public:
    void read(Reader& r) {
        uint16_t count = r.u2();
        entries_.resize(count);
        for (uint16_t i = 1; i < count; ++i) {
            Entry& e = entries_[i];
            e.tag = r.u1();
            switch (e.tag) {
            case kUtf8: e.raw = r.bytes(r.u2()); break;
            case kClass: case kString: case kMethodType: case kModule: case kPackage: e.raw = r.bytes(2); break;
            case kHandle: e.raw = r.bytes(3); break;
            case kInt: case kFloat: case kField: case kMethod: case kIMethod: case kNameType:
            case kDynamic: case kInvokeDynamic: e.raw = r.bytes(4); break;
            case kLong: case kDouble: e.raw = r.bytes(8); ++i; break;  // takes two slots
            default: throw ParseError("unknown constant pool tag " + std::to_string(e.tag));
            }
        }
    }

    void write(Writer& w) const {
        if (entries_.size() > 0xffff) throw ParseError("constant pool overflow");
        w.u2(unsigned(entries_.size()));
        for (size_t i = 1; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            if (e.tag == 0) continue;  // second slot of a long/double
            w.u1(e.tag);
            if (e.tag == kUtf8) w.u2(unsigned(e.raw.size()));
            w.put(e.raw);
        }
    }

    std::string utf8(uint16_t index) const {
        const Entry& e = at(index);
        if (e.tag != kUtf8) throw ParseError("expected utf8 constant");
        return std::string(e.raw.begin(), e.raw.end());
    }

    std::string class_name(uint16_t index) const {
        const Entry& e = at(index);
        if (e.tag != kClass) throw ParseError("expected class constant");
        return utf8(uint16_t(e.raw[0] << 8 | e.raw[1]));
    }

    uint16_t utf8_index(const std::string& s) {
        return find_or_add(kUtf8, std::vector<uint8_t>(s.begin(), s.end()));
    }
    uint16_t class_index(const std::string& name) { return find_or_add(kClass, be16(utf8_index(name))); }
    uint16_t name_type(const std::string& name, const std::string& desc) {
        return find_or_add(kNameType, be16(utf8_index(name), utf8_index(desc)));
    }
    uint16_t method_ref(const std::string& cls, const std::string& name, const std::string& desc) {
        return find_or_add(kMethod, be16(class_index(cls), name_type(name, desc)));
    }
    uint16_t field_ref(const std::string& cls, const std::string& name, const std::string& desc) {
        return find_or_add(kField, be16(class_index(cls), name_type(name, desc)));
    }

private:
    struct Entry {
        uint8_t tag = 0;
        std::vector<uint8_t> raw;
    };

    const Entry& at(uint16_t i) const {
        if (i == 0 || i >= entries_.size()) throw ParseError("bad constant pool index");
        return entries_[i];
    }

    static std::vector<uint8_t> be16(uint16_t a) { return {uint8_t(a >> 8), uint8_t(a)}; }
    static std::vector<uint8_t> be16(uint16_t a, uint16_t b) {
        return {uint8_t(a >> 8), uint8_t(a), uint8_t(b >> 8), uint8_t(b)};
    }

    uint16_t find_or_add(uint8_t tag, std::vector<uint8_t> raw) {
        for (size_t i = 1; i < entries_.size(); ++i)
            if (entries_[i].tag == tag && entries_[i].raw == raw) return uint16_t(i);
        if (entries_.size() >= 0xffff) throw ParseError("constant pool overflow");
        entries_.push_back({tag, std::move(raw)});
        return uint16_t(entries_.size() - 1);
    }

    std::vector<Entry> entries_;
};

// Splits "(IJLfoo;[I)V" into parameter descriptors and the return descriptor.
void split_desc(const std::string& desc, std::vector<std::string>& params, std::string& ret) {
    size_t i = 1;
    auto one = [&]() {
        size_t start = i;
        while (i < desc.size() && desc[i] == '[') ++i;
        if (i >= desc.size()) throw ParseError("bad descriptor " + desc);
        if (desc[i] == 'L') {
            i = desc.find(';', i);
            if (i == std::string::npos) throw ParseError("bad descriptor " + desc);
        }
        ++i;
        return desc.substr(start, i - start);
    };
    if (desc.empty() || desc[0] != '(') throw ParseError("bad descriptor " + desc);
    while (i < desc.size() && desc[i] != ')') params.push_back(one());
    ++i;
    ret = one();
}

// StackMapTable verification_type_info
struct VType {
    uint8_t tag;
    uint16_t data = 0;  // cpool index for Object, bytecode offset for Uninitialized
};

struct Frame {
    int offset;  // absolute
    enum Kind { Same, SameLocals1, Chop, Append, Full } kind;
    int k = 0;   // chop count
    std::vector<VType> locals, stack;
};

VType read_vtype(Reader& r) {
    VType v{r.u1()};
    if (v.tag == 7 || v.tag == 8) v.data = r.u2();
    return v;
}

void write_vtype(Writer& w, const VType& v) {
    w.u1(v.tag);
    if (v.tag == 7 || v.tag == 8) w.u2(v.data);
}

std::vector<Frame> read_frames(const std::vector<uint8_t>& data) {
    Reader r(data.data(), data.size());
    std::vector<Frame> frames(r.u2());
    int prev = -1;
    for (auto& f : frames) {
        uint8_t t = r.u1();
        int delta;
        if (t < 64) { f.kind = Frame::Same; delta = t; }
        else if (t < 128) { f.kind = Frame::SameLocals1; delta = t - 64; f.stack.push_back(read_vtype(r)); }
        else if (t == 247) { f.kind = Frame::SameLocals1; delta = r.u2(); f.stack.push_back(read_vtype(r)); }
        else if (t >= 248 && t <= 250) { f.kind = Frame::Chop; f.k = 251 - t; delta = r.u2(); }
        else if (t == 251) { f.kind = Frame::Same; delta = r.u2(); }
        else if (t >= 252 && t <= 254) {
            f.kind = Frame::Append;
            delta = r.u2();
            for (int i = 0; i < t - 251; ++i) f.locals.push_back(read_vtype(r));
        } else if (t == 255) {
            f.kind = Frame::Full;
            delta = r.u2();
            f.locals.resize(r.u2());
            for (auto& v : f.locals) v = read_vtype(r);
            f.stack.resize(r.u2());
            for (auto& v : f.stack) v = read_vtype(r);
        } else {
            throw ParseError("bad stack map frame type " + std::to_string(t));
        }
        f.offset = prev + delta + 1;
        prev = f.offset;
    }
    return frames;
}

std::vector<uint8_t> write_frames(const std::vector<Frame>& frames) {
    Writer w;
    w.u2(unsigned(frames.size()));
    int prev = -1;
    for (auto& f : frames) {
        int delta = f.offset - prev - 1;
        prev = f.offset;
        switch (f.kind) {
        case Frame::Same:
            if (delta < 64) w.u1(unsigned(delta)); else { w.u1(251); w.u2(unsigned(delta)); }
            break;
        case Frame::SameLocals1:
            if (delta < 64) w.u1(unsigned(64 + delta)); else { w.u1(247); w.u2(unsigned(delta)); }
            write_vtype(w, f.stack[0]);
            break;
        case Frame::Chop:
            w.u1(unsigned(251 - f.k)); w.u2(unsigned(delta));
            break;
        case Frame::Append:
            w.u1(unsigned(251 + f.locals.size())); w.u2(unsigned(delta));
            for (auto& v : f.locals) write_vtype(w, v);
            break;
        case Frame::Full:
            w.u1(255); w.u2(unsigned(delta));
            w.u2(unsigned(f.locals.size()));
            for (auto& v : f.locals) write_vtype(w, v);
            w.u2(unsigned(f.stack.size()));
            for (auto& v : f.stack) write_vtype(w, v);
            break;
        }
    }
    return w.b;
}

struct Method {
    uint16_t access, name, desc;
    std::vector<Attr> attrs;
};

constexpr uint16_t ACC_STATIC = 0x0008;
constexpr const char* kBridge = "umc/Bridge";
constexpr const char* kObject = "java/lang/Object";

// Rewrites one Code attribute in place.
void patch_code(Attr& code_attr, ConstantPool& cp, const std::string& this_class, bool is_static,
                const std::string& desc, int site, uint16_t major) {
    Reader r(code_attr.data.data(), code_attr.data.size());
    uint16_t max_stack = r.u2(), max_locals = r.u2();
    std::vector<uint8_t> code = r.bytes(r.u4());
    struct Handler { uint16_t start, end, handler, type; };
    std::vector<Handler> handlers(r.u2());
    for (auto& h : handlers) h = {r.u2(), r.u2(), r.u2(), r.u2()};
    std::vector<Attr> attrs = read_attrs(r);

    std::vector<std::string> params;
    std::string ret;
    split_desc(desc, params, ret);

    Writer p;
    p.u1(0x11); p.u2(unsigned(site));        // sipush site
    p.u1(is_static ? 0x01 : 0x2a);           // aconst_null / aload_0
    p.u1(0x11); p.u2(unsigned(params.size()));
    p.u1(0xbd); p.u2(cp.class_index(kObject));  // anewarray Object

    int slot = is_static ? 0 : 1;
    for (size_t i = 0; i < params.size(); ++i) {
        const std::string& t = params[i];
        p.u1(0x59);                         // dup
        p.u1(0x11); p.u2(unsigned(i));      // sipush index
        uint8_t load;
        const char* box = nullptr;
        const char* box_desc = nullptr;
        switch (t[0]) {
        case 'Z': load = 0x15; box = "java/lang/Boolean";   box_desc = "(Z)Ljava/lang/Boolean;"; break;
        case 'B': load = 0x15; box = "java/lang/Byte";      box_desc = "(B)Ljava/lang/Byte;"; break;
        case 'C': load = 0x15; box = "java/lang/Character"; box_desc = "(C)Ljava/lang/Character;"; break;
        case 'S': load = 0x15; box = "java/lang/Short";     box_desc = "(S)Ljava/lang/Short;"; break;
        case 'I': load = 0x15; box = "java/lang/Integer";   box_desc = "(I)Ljava/lang/Integer;"; break;
        case 'J': load = 0x16; box = "java/lang/Long";      box_desc = "(J)Ljava/lang/Long;"; break;
        case 'F': load = 0x17; box = "java/lang/Float";     box_desc = "(F)Ljava/lang/Float;"; break;
        case 'D': load = 0x18; box = "java/lang/Double";    box_desc = "(D)Ljava/lang/Double;"; break;
        default: load = 0x19;
        }
        if (slot > 0xff) { p.u1(0xc4); p.u1(load); p.u2(unsigned(slot)); }  // wide
        else { p.u1(load); p.u1(unsigned(slot)); }
        if (box) { p.u1(0xb8); p.u2(cp.method_ref(box, "valueOf", box_desc)); }
        p.u1(0x53);                         // aastore
        slot += (t[0] == 'J' || t[0] == 'D') ? 2 : 1;
    }

    p.u1(0xb8); p.u2(cp.method_ref(kBridge, "enter", "(ILjava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;"));
    p.u1(0x59);                                                   // dup
    p.u1(0xb2); p.u2(cp.field_ref(kBridge, "CONTINUE", "Ljava/lang/Object;"));
    size_t branch_pc = p.b.size();
    p.u1(0xa6); p.u2(0);                                          // if_acmpne cancel (patched below)
    p.u1(0x57);                                                   // pop
    // tableswitch/lookupswitch padding depends on 4-byte alignment from the code start.
    while (p.b.size() % 4) p.u1(0x00);

    const size_t shift = p.b.size();
    const size_t cancel_pc = shift + code.size();
    long jump = long(cancel_pc) - long(branch_pc);
    if (jump > 0x7fff) throw ParseError("method too large to hook");
    p.b[branch_pc + 1] = uint8_t(jump >> 8);
    p.b[branch_pc + 2] = uint8_t(jump);

    Writer c;
    switch (ret[0]) {
    case 'V': c.u1(0x57); c.u1(0xb1); break;  // pop, return
    case 'Z': case 'B': case 'C': case 'S': case 'I':
        c.u1(0xb8); c.u2(cp.method_ref(kBridge, "asInt", "(Ljava/lang/Object;)I")); c.u1(0xac); break;
    case 'J': c.u1(0xb8); c.u2(cp.method_ref(kBridge, "asLong", "(Ljava/lang/Object;)J")); c.u1(0xad); break;
    case 'F': c.u1(0xb8); c.u2(cp.method_ref(kBridge, "asFloat", "(Ljava/lang/Object;)F")); c.u1(0xae); break;
    case 'D': c.u1(0xb8); c.u2(cp.method_ref(kBridge, "asDouble", "(Ljava/lang/Object;)D")); c.u1(0xaf); break;
    default: {
        std::string cls = ret[0] == 'L' ? ret.substr(1, ret.size() - 2) : ret;
        c.u1(0xc0); c.u2(cp.class_index(cls)); c.u1(0xb0);  // checkcast, areturn
    }
    }

    std::vector<uint8_t> new_code = p.b;
    new_code.insert(new_code.end(), code.begin(), code.end());
    new_code.insert(new_code.end(), c.b.begin(), c.b.end());
    if (new_code.size() > 65535) throw ParseError("method too large to hook");

    auto shifted = [&](uint16_t pc) { return uint16_t(pc + shift); };
    for (auto& h : handlers) { h.start = shifted(h.start); h.end = shifted(h.end); h.handler = shifted(h.handler); }

    // Frame for the cancel block: the method's entry locals with the bridge result on the stack.
    Frame cancel_frame;
    cancel_frame.offset = int(cancel_pc);
    cancel_frame.kind = Frame::Full;
    if (!is_static) cancel_frame.locals.push_back({7, cp.class_index(this_class)});
    for (auto& t : params) {
        switch (t[0]) {
        case 'Z': case 'B': case 'C': case 'S': case 'I': cancel_frame.locals.push_back({1}); break;
        case 'F': cancel_frame.locals.push_back({2}); break;
        case 'D': cancel_frame.locals.push_back({3}); break;
        case 'J': cancel_frame.locals.push_back({4}); break;
        default: cancel_frame.locals.push_back({7, cp.class_index(t[0] == 'L' ? t.substr(1, t.size() - 2) : t)});
        }
    }
    cancel_frame.stack.push_back({7, cp.class_index(kObject)});

    bool has_frames = false;
    std::vector<Attr> kept;
    for (auto& a : attrs) {
        std::string name = cp.utf8(a.name);
        if (name == "StackMapTable") {
            auto frames = read_frames(a.data);
            for (auto& f : frames) {
                f.offset += int(shift);
                for (auto* list : {&f.locals, &f.stack})
                    for (auto& v : *list) if (v.tag == 8) v.data = uint16_t(v.data + shift);
            }
            frames.push_back(cancel_frame);
            a.data = write_frames(frames);
            has_frames = true;
            kept.push_back(std::move(a));
        } else if (name == "LineNumberTable" || name == "LocalVariableTable" || name == "LocalVariableTypeTable") {
            size_t entry = name == "LineNumberTable" ? 4 : 10;
            for (size_t off = 2; off + entry <= a.data.size(); off += entry) {
                uint16_t pc = uint16_t(a.data[off] << 8 | a.data[off + 1]);
                pc = shifted(pc);
                a.data[off] = uint8_t(pc >> 8);
                a.data[off + 1] = uint8_t(pc);
            }
            kept.push_back(std::move(a));
        }
        // Other code attributes (type annotations, etc.) hold offsets we don't track, so they're dropped.
    }
    // Java 6 class files may verify without frames; Java 7+ require them.
    if (!has_frames && major >= 51) kept.push_back({cp.utf8_index("StackMapTable"), write_frames({cancel_frame})});

    Writer w;
    w.u2(std::max<unsigned>(max_stack, 8));
    w.u2(max_locals);
    w.u4(uint32_t(new_code.size()));
    w.put(new_code);
    w.u2(unsigned(handlers.size()));
    for (auto& h : handlers) { w.u2(h.start); w.u2(h.end); w.u2(h.handler); w.u2(h.type); }
    write_attrs(w, kept);
    code_attr.data = std::move(w.b);
}

}  // namespace

bool inject_hooks(const uint8_t* data, size_t len, const SiteResolver& resolve, std::vector<uint8_t>& out,
                  std::vector<std::string>& patched, std::string& error) {
    try {
        Reader r(data, len);
        if (r.u4() != 0xCAFEBABE) throw ParseError("not a class file");
        uint16_t minor = r.u2(), major = r.u2();
        ConstantPool cp;
        cp.read(r);
        uint16_t access = r.u2(), this_idx = r.u2(), super_idx = r.u2();
        std::vector<uint16_t> interfaces(r.u2());
        for (auto& i : interfaces) i = r.u2();

        struct Field { uint16_t access, name, desc; std::vector<Attr> attrs; };
        std::vector<Field> fields(r.u2());
        for (auto& f : fields) { f.access = r.u2(); f.name = r.u2(); f.desc = r.u2(); f.attrs = read_attrs(r); }
        std::vector<Method> methods(r.u2());
        for (auto& m : methods) { m.access = r.u2(); m.name = r.u2(); m.desc = r.u2(); m.attrs = read_attrs(r); }
        std::vector<Attr> class_attrs = read_attrs(r);
        if (!r.done()) throw ParseError("trailing bytes after class file");

        const std::string this_class = cp.class_name(this_idx);
        for (auto& m : methods) {
            std::string name = cp.utf8(m.name), desc = cp.utf8(m.desc);
            if (name == "<init>" || name == "<clinit>") continue;
            int site = resolve(name, desc);
            if (site < 0) continue;
            if (site > 0x7fff) throw ParseError("too many hook sites");
            for (auto& a : m.attrs) {
                if (cp.utf8(a.name) != "Code") continue;
                patch_code(a, cp, this_class, m.access & ACC_STATIC, desc, site, major);
                patched.push_back(name + " " + desc);
            }
        }

        Writer w;
        w.u4(0xCAFEBABE);
        w.u2(minor);
        w.u2(major);
        cp.write(w);
        w.u2(access); w.u2(this_idx); w.u2(super_idx);
        w.u2(unsigned(interfaces.size()));
        for (auto i : interfaces) w.u2(i);
        w.u2(unsigned(fields.size()));
        for (auto& f : fields) { w.u2(f.access); w.u2(f.name); w.u2(f.desc); write_attrs(w, f.attrs); }
        w.u2(unsigned(methods.size()));
        for (auto& m : methods) { w.u2(m.access); w.u2(m.name); w.u2(m.desc); write_attrs(w, m.attrs); }
        write_attrs(w, class_attrs);
        out = std::move(w.b);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace umc
