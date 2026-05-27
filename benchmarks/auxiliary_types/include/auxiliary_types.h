#pragma once

namespace aux {
int sum_array(int* data, int size);
float average(float* data, int size);
int find_or_default(int* data, int size, int target, int fallback);
} // namespace aux
