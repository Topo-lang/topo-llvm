#pragma once
// class_template_variadic — Step 6 micro-benchmark:
// variadic function + class templates.

#include <cstdint>
#include <cstddef>
#include <iostream>

namespace variadic {

template <typename... Ts>
void print_all(Ts... args) {
    ((std::cout << args << " "), ...);
    std::cout << std::endl;
}

template <typename... Ts>
int sum_ints(Ts... args) {
    return (... + static_cast<int>(args));
}

template <typename... Components>
class Tuple {
public:
    size_t component_count() const { return sizeof...(Components); }
};

} // namespace variadic
