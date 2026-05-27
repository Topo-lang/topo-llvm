#include "pipeline.h"

namespace imaging {

int load(int input) {
    // Simulate loading image data
    return input * 2;
}

int decode(int data) {
    // Simulate decoding
    return data + 10;
}

int enhance(int pixels) {
    // Simulate enhancement (brightness adjustment)
    return pixels + 5;
}

int detect(int pixels) {
    // Simulate feature detection
    return pixels * 3;
}

int compose(int enhanced, int detected) {
    // Merge enhanced image with detected features
    return enhanced + detected;
}

} // namespace imaging
