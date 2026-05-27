#pragma once
// class_template_crtp — micro-benchmark for CRTP (Step 3 subset).
#include <iostream>

namespace collection {

template <typename Derived>
class Printable {
public:
    void print() const { static_cast<const Derived*>(this)->print_impl(); }
    void println() const {
        print();
        std::cout << std::endl;
    }
    int score() const { return static_cast<const Derived*>(this)->score_impl(); }
};

class Point : public Printable<Point> {
    friend class Printable<Point>;

public:
    explicit Point(float x, float y) : x_(x), y_(y) {}
    float x() const { return x_; }
    float y() const { return y_; }

private:
    void print_impl() const { std::cout << "Point(" << x_ << ", " << y_ << ")"; }
    int score_impl() const { return static_cast<int>(x_ + y_); }
    float x_;
    float y_;
};

class Counter : public Printable<Counter> {
    friend class Printable<Counter>;

public:
    explicit Counter(int start) : value_(start) {}
    void bump() { ++value_; }
    int value() const { return value_; }

private:
    void print_impl() const { std::cout << "Counter(" << value_ << ")"; }
    int score_impl() const { return value_ * 2; }
    int value_;
};

} // namespace collection
