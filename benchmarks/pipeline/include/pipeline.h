#pragma once
#include <topo/pipeline.h>

namespace imaging {
int process(int input);

int load(int input);
int decode(int data);
int enhance(int pixels);
int detect(int pixels);
int compose(int enhanced, int detected);
} // namespace imaging
