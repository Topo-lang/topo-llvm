#include "scene.h"

int process_shared(std::shared_ptr<Texture> tex) {
    int w = tex->width;
    int h = tex->height;
    int area = tex->width * tex->height;
    return w + h + area;
}

int observe_weak(std::weak_ptr<Texture> tex) {
    auto locked = tex.lock();
    if (!locked) return -1;
    return locked->width + locked->height;
}
