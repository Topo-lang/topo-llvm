#pragma once
// class_template_types — micro-benchmark for Step 1 only:
// non-template class declarations (inheritance, constructor, static members).

#include <cstdint>

namespace container {

class Shape {
public:
    float area() const;
    float perimeter() const;

protected:
    void validate();
    float cached_area = 0.0f;
    float cached_perimeter = 0.0f;
};

class Circle : public Shape {
public:
    explicit Circle(float radius);
    ~Circle();
    float radius() const;

private:
    float radius_;
};

class MathConstants {
public:
    static float pi();
    static float e();

private:
    static int32_t precision;
};

} // namespace container
