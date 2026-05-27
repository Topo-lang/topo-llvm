#pragma once

namespace math {

class Vec2 {
public:
    explicit Vec2(int x, int y) : x_(x), y_(y) {}
    int x() const { return x_; }
    int y() const { return y_; }
    Vec2 operator+(const Vec2& rhs) const { return Vec2(x_ + rhs.x_, y_ + rhs.y_); }

private:
    int x_, y_;
};

int pipeline_sum(int a, int b);

Vec2 make_vec(int a, int b);
Vec2 add_offset(Vec2 v);
int extract(Vec2 v);

} // namespace math
