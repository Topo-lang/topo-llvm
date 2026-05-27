#include "color.h"
#include <algorithm>

namespace color {

int clamp(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

void normalize(int* val) {
    *val = clamp(*val, 0, 255);
}

// Simplified RGB to HSL (integer approximation, H in [0,360], S/L in [0,100])
topo::tuple<int, int, int> rgb_to_hsl(int r, int g, int b) {
    int maxc = std::max({r, g, b});
    int minc = std::min({r, g, b});
    int l = (maxc + minc) * 100 / 510; // 0-100

    if (maxc == minc) {
        return {0, 0, l};
    }

    int diff = maxc - minc;
    int s = (l > 50) ? diff * 100 / (510 - maxc - minc) : diff * 100 / (maxc + minc);

    int h = 0;
    if (maxc == r) {
        h = 60 * ((g - b) * 100 / diff) / 100;
    } else if (maxc == g) {
        h = 60 * ((b - r) * 100 / diff) / 100 + 120;
    } else {
        h = 60 * ((r - g) * 100 / diff) / 100 + 240;
    }
    if (h < 0) h += 360;

    return {h, s, l};
}

// Simplified HSL to RGB
topo::tuple<int, int, int> hsl_to_rgb(int h, int s, int l) {
    if (s == 0) {
        int v = l * 255 / 100;
        return {v, v, v};
    }

    // Simple approximation: return a proportional value
    int c = (100 - std::abs(2 * l - 100)) * s / 100;
    int x = c * (100 - std::abs((h * 100 / 60) % 200 - 100)) / 100;
    int m = l - c / 2;

    int r1 = 0, g1 = 0, b1 = 0;
    if (h < 60) {
        r1 = c;
        g1 = x;
        b1 = 0;
    } else if (h < 120) {
        r1 = x;
        g1 = c;
        b1 = 0;
    } else if (h < 180) {
        r1 = 0;
        g1 = c;
        b1 = x;
    } else if (h < 240) {
        r1 = 0;
        g1 = x;
        b1 = c;
    } else if (h < 300) {
        r1 = x;
        g1 = 0;
        b1 = c;
    } else {
        r1 = c;
        g1 = 0;
        b1 = x;
    }

    int r = clamp((r1 + m) * 255 / 100, 0, 255);
    int g = clamp((g1 + m) * 255 / 100, 0, 255);
    int b2 = clamp((b1 + m) * 255 / 100, 0, 255);

    return {r, g, b2};
}

topo::tuple<int, int, int> adjust_brightness(int r, int g, int b, int factor) {
    r = clamp(r * factor / 100, 0, 255);
    g = clamp(g * factor / 100, 0, 255);
    b = clamp(b * factor / 100, 0, 255);
    return {r, g, b};
}

void demo() {
    auto [h, s, l] = rgb_to_hsl(128, 64, 32);
    auto [r, g, b] = hsl_to_rgb(h, s, l);
    auto [nr, ng, nb] = adjust_brightness(r, g, b, 120);
    (void)nr;
    (void)ng;
    (void)nb;
}

} // namespace color
