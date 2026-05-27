#ifndef SCENE_H
#define SCENE_H

#include <memory>

struct Node {
    int value;
    int extra;
};

struct Texture {
    int width;
    int height;
};

// Declarations matching .topo ownership qualifiers:
//   owned Node   → std::unique_ptr<Node>
//   shared Texture → std::shared_ptr<Texture>
//   weak Texture   → std::weak_ptr<Texture>

int process_owned(std::unique_ptr<Node> node);
int process_shared(std::shared_ptr<Texture> tex);
int observe_weak(std::weak_ptr<Texture> tex);
int run_test();

#endif // SCENE_H
