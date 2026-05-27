#ifndef SCENE_FRIENDLY_H
#define SCENE_FRIENDLY_H

#include <memory>

struct Node {
    int value;
    int extra;
};

// Friendly-only: owned (unique_ptr) — IndirectionPass can promote dereferences.
int process_owned(std::unique_ptr<Node> node);
int run_test();

#endif // SCENE_FRIENDLY_H
