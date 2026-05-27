#include "scene.h"

int process_owned(std::unique_ptr<Node> node) {
    // Multiple dereferences — IndirectionPass should promote to single load.
    int a = node->value;
    int b = node->extra;
    int c = node->value + node->extra;
    return a + b + c;
}
