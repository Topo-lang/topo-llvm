#pragma once
#include <topo/tuple.h>

namespace color {
topo::tuple<int, int, int> rgb_to_hsl(int r, int g, int b);
topo::tuple<int, int, int> hsl_to_rgb(int h, int s, int l);
topo::tuple<int, int, int> adjust_brightness(int r, int g, int b, int factor);

void demo();
int clamp(int val, int lo, int hi);
void normalize(int* val);
} // namespace color
