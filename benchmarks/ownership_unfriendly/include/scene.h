#ifndef SCENE_UNFRIENDLY_H
#define SCENE_UNFRIENDLY_H

#include <memory>
#include <vector>

struct Texture {
    int width;
    int height;
};

// Unfriendly paths: shared (refcount) + weak + natural escape through an
// opaque function pointer and external registry. No volatile artifact.

int process_shared(std::shared_ptr<Texture> tex);
int observe_weak(std::weak_ptr<Texture> tex);

// External function pointer — unknown body at this TU — forces the pointer
// to be treated as escaped by conservative alias analysis.
using TextureSink = void (*)(Texture*);
extern TextureSink get_sink();

// Registry function — calls an externally-linked function that stashes the
// pointer in a TU-external container. Natural escape path.
void register_texture(std::shared_ptr<Texture> tex);
int drain_registered();

int run_test();

#endif // SCENE_UNFRIENDLY_H
