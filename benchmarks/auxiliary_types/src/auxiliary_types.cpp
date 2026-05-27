#include "auxiliary_types.h"
#include <topo/array.h>
#include <topo/span.h>
#include <topo/slot.h>

namespace aux {

int sum_array(int* data, int size) {
    topo::span<int> s(data, static_cast<std::size_t>(size));
    int total = 0;
    for (int x : s)
        total += x;
    return total;
}

float average(float* data, int size) {
    topo::span<float> s(data, static_cast<std::size_t>(size));
    float total = 0.0f;
    for (float x : s)
        total += x;
    return s.empty() ? 0.0f : total / static_cast<float>(s.size());
}

int find_or_default(int* data, int size, int target, int fallback) {
    topo::span<int> s(data, static_cast<std::size_t>(size));
    topo::slot<int> result;
    for (int x : s) {
        if (x == target) {
            result.emplace(x);
            break;
        }
    }
    return result.value_or(fallback);
}

} // namespace aux
