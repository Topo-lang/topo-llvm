#include "scene.h"

int process_owned(std::unique_ptr<Node> node) {
    // Multiple dereferences — IndirectionPass should promote to single load
    int a = node->value;
    int b = node->extra;
    int c = node->value + node->extra;
    return a + b + c;
}

int process_shared(std::shared_ptr<Texture> tex) {
    // Multiple dereferences of shared_ptr data
    int w = tex->width;
    int h = tex->height;
    int area = tex->width * tex->height;
    return w + h + area;
}

int observe_weak(std::weak_ptr<Texture> tex) {
    // weak_ptr must be locked before use — no optimization expected
    auto locked = tex.lock();
    if (!locked) return -1;
    return locked->width + locked->height;
}
