// operator_pipeline — Pipeline with operator-overloaded types

#include "math_vec.h"
#include <cassert>
#include <cstdio>

namespace math {

Vec2 make_vec(int a, int b) {
    return Vec2(a, b);
}

Vec2 add_offset(Vec2 v) {
    Vec2 offset(10, 20);
    return v + offset;
}

int extract(Vec2 v) {
    return v.x() + v.y();
}

int pipeline_sum(int a, int b) {
    auto v = make_vec(a, b);
    auto shifted = add_offset(v);
    return extract(shifted);
}

} // namespace math

int main() {
    // pipeline_sum(3, 4) => make_vec(3,4) => Vec2(3,4)
    //   => add_offset => Vec2(3,4) + Vec2(10,20) = Vec2(13,24)
    //   => extract => 13 + 24 = 37
    int result = math::pipeline_sum(3, 4);
    assert(result == 37);

    std::printf("operator_pipeline: result=%d\n", result);
    std::printf("operator_pipeline: all assertions passed\n");
    return 0;
}
