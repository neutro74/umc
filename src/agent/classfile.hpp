// Bytecode injection for method hooks.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace umc {

// Given a method's runtime name and descriptor, returns the bridge site id to call, or -1 to skip it.
using SiteResolver = std::function<int(const std::string& name, const std::string& desc)>;

// Rewrites a class so each selected method starts with:
//   Object r = umc.Bridge.enter(site, this, new Object[]{args...});
//   if (r != umc.Bridge.CONTINUE) return (T) r;
// Returns false with an error on malformed input. `patched` receives "name desc" for each change.
bool inject_hooks(const uint8_t* data, size_t len, const SiteResolver& resolve,
                  std::vector<uint8_t>& out, std::vector<std::string>& patched, std::string& error);

}  // namespace umc
