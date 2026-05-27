#include "processing.h"

namespace processing {

int decode(int raw) {
    return raw ^ 0xFF;
}
int transform(int data) {
    return data * 3 + 1;
}
int encode(int data) {
    return data ^ 0xFF;
}

} // namespace processing
