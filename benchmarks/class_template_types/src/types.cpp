#include "types.h"

namespace container {

float Shape::area() const { return cached_area; }
float Shape::perimeter() const { return cached_perimeter; }
void Shape::validate() {
    if (cached_area < 0.0f) cached_area = 0.0f;
    if (cached_perimeter < 0.0f) cached_perimeter = 0.0f;
}

Circle::Circle(float radius) : radius_(radius) {
    cached_area = MathConstants::pi() * radius_ * radius_;
    cached_perimeter = 2.0f * MathConstants::pi() * radius_;
    validate();
}

Circle::~Circle() {}
float Circle::radius() const { return radius_; }

int32_t MathConstants::precision = 6;
float MathConstants::pi() { return 3.14159265f; }
float MathConstants::e() { return 2.71828183f; }

} // namespace container
