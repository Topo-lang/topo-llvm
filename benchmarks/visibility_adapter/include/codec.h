#pragma once

namespace codec {

class SimpleCodec {
public:
    int encode(int input);
    int decode(int input);
};

int roundtrip(int value);
int simple_encode(int input);
int simple_decode(int input);
int xor_key();

} // namespace codec
