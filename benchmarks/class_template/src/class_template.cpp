#include "class_template.h"
#include <cmath>

// ============================================================
// Step 1: Non-template class implementations
// ============================================================

namespace container {

// --- Shape ---

float Shape::area() const {
    return cached_area;
}
float Shape::perimeter() const {
    return cached_perimeter;
}
void Shape::validate() {
    if (cached_area < 0.0f) cached_area = 0.0f;
    if (cached_perimeter < 0.0f) cached_perimeter = 0.0f;
}

// --- Circle ---

Circle::Circle(float radius) : radius_(radius) {
    cached_area = MathConstants::pi() * radius_ * radius_;
    cached_perimeter = 2.0f * MathConstants::pi() * radius_;
    validate();
}

Circle::~Circle() {}

float Circle::radius() const {
    return radius_;
}

// --- MathConstants ---

int32_t MathConstants::precision = 6;

float MathConstants::pi() {
    return 3.14159265f;
}
float MathConstants::e() {
    return 2.71828183f;
}

} // namespace container

// ============================================================
// Step 4: Adapt function implementations
// ============================================================

namespace traits {

double double_add(double a, double b) {
    return a + b;
}
double double_multiply(double a, double b) {
    return a * b;
}
const double double_zero = 0.0;

} // namespace traits

// ============================================================
// Step 5: Comptime function implementations
// ============================================================

namespace meta {

void small_path() {
    std::cout << "  small_path: N <= 4, using compact layout" << std::endl;
}

void large_path() {
    std::cout << "  large_path: N > 4, using expanded layout" << std::endl;
}

} // namespace meta

// ============================================================
// Explicit template instantiations
// ============================================================

// instantiate Vector<Int>;
template class collection::Vector<int32_t>;
// instantiate Vector<Float>;
template class collection::Vector<float>;
// instantiate HashMap<Int, Double>;
template class collection::HashMap<int32_t, double>;
// instantiate Array<Float, 16>;
template class collection::Array<float, 16>;
// instantiate Tuple<Int, Float, Double>;
template class variadic::Tuple<int32_t, float, double>;
