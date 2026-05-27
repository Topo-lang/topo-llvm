// Separate TU — provides externally-linked "registry" functions.
// The linker sees the symbols but IndirectionPass cannot see into their
// bodies at optimization time, which is the natural escape model we want
// (vs. the volatile g_escape artifact used previously).

#include "scene.h"
#include <vector>

namespace {
std::vector<std::shared_ptr<Texture>> g_registry;

void opaque_sink(Texture* t) {
    // Do nothing functional — just touch the pointer so the optimizer
    // cannot prove no side effect across TUs.
    if (t) t->width ^= 0;
}
}

TextureSink get_sink() { return &opaque_sink; }

void register_texture(std::shared_ptr<Texture> tex) {
    g_registry.push_back(std::move(tex));
}

int drain_registered() {
    int sum = 0;
    for (auto& t : g_registry) {
        if (t) sum += t->width + t->height;
    }
    g_registry.clear();
    return sum;
}
