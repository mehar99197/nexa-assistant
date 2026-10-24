#pragma once

namespace nexa {

class MemoryStore;
class ToolRegistry;
struct MemoryConfig;

// remember, forget and search_memory over the MemoryStore — added while memory is on and removed
// while it is off (not withheld: the fast path and the executor would still find a withheld tool),
// and the store told whether the model may save on its own.
void syncMemoryTools(ToolRegistry &registry, MemoryStore *store, const MemoryConfig &config);

} // namespace nexa
