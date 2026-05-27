// visibility_adapter — Visibility rules combined with adapters

#include "codec.h"
#include <cassert>
#include <cstdio>

namespace codec {

int xor_key() {
    return 42;
}

int simple_encode(int input) {
    return input ^ xor_key();
}

int simple_decode(int input) {
    return input ^ xor_key();
}

int SimpleCodec::encode(int input) {
    return simple_encode(input);
}

int SimpleCodec::decode(int input) {
    return simple_decode(input);
}

int roundtrip(int value) {
    int encoded = simple_encode(value);
    return simple_decode(encoded);
}

} // namespace codec

int main() {
    // roundtrip(100) => encode(100) = 100^42 = 78
    //                 => decode(78)  = 78^42  = 100
    int result = codec::roundtrip(100);
    assert(result == 100);

    // Verify encode/decode via class
    codec::SimpleCodec c;
    int encoded = c.encode(255);
    int decoded = c.decode(encoded);
    assert(decoded == 255);

    std::printf("visibility_adapter: result=%d decoded=%d\n", result, decoded);
    std::printf("visibility_adapter: all assertions passed\n");
    return 0;
}
