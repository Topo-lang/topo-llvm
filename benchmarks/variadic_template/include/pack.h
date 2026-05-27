#pragma once
#include <cstddef>

namespace pack {

template <typename... Ts>
class Tuple {
public:
    std::size_t count() const { return sizeof...(Ts); }
};

int accumulate(int a, int b);

template <typename T>
int sum_all(T arg) {
    return static_cast<int>(arg);
}

template <typename T, typename... Rest>
int sum_all(T first, Rest... rest) {
    return accumulate(static_cast<int>(first), sum_all(rest...));
}

} // namespace pack
