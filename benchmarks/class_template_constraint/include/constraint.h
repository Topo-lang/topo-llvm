#pragma once
// class_template_constraint — micro-benchmark for Step 4:
// constraints + specialization (adapt, sum, clamp, TypeTraits).
//
// Vector is embedded locally (not shared with other splits) because
// traits::sum requires a container type. This is the only inter-phase
// dependency from the original class_template benchmark.

#include <cstdint>
#include <cstddef>

namespace collection {

template <typename T>
class Vector {
public:
    Vector() = default;
    Vector(const Vector& other) : count_(other.count_), capacity_(other.capacity_) {
        if (capacity_ > 0) {
            data_ = new T[capacity_];
            for (size_t i = 0; i < count_; ++i)
                data_[i] = other.data_[i];
        }
    }
    Vector& operator=(const Vector& other) {
        if (this != &other) {
            delete[] data_;
            data_ = nullptr;
            count_ = other.count_;
            capacity_ = other.capacity_;
            if (capacity_ > 0) {
                data_ = new T[capacity_];
                for (size_t i = 0; i < count_; ++i)
                    data_[i] = other.data_[i];
            }
        }
        return *this;
    }
    ~Vector() { delete[] data_; }
    void push_back(const T& value) {
        if (count_ >= capacity_) {
            size_t new_cap = capacity_ == 0 ? 4 : capacity_ * 2;
            T* new_data = new T[new_cap];
            for (size_t i = 0; i < count_; ++i)
                new_data[i] = data_[i];
            delete[] data_;
            data_ = new_data;
            capacity_ = new_cap;
        }
        data_[count_++] = value;
    }
    size_t size() const { return count_; }
    const T& at(size_t i) const { return data_[i]; }

private:
    T* data_ = nullptr;
    size_t count_ = 0;
    size_t capacity_ = 0;
};

} // namespace collection

namespace traits {

double double_add(double a, double b);
double double_multiply(double a, double b);
extern const double double_zero;

template <typename T>
T sum(collection::Vector<T> data) {
    T result{};
    for (size_t i = 0; i < data.size(); ++i) {
        result = result + data.at(i);
    }
    return result;
}

template <typename T>
T clamp(T value, T low, T high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

template <typename T>
class TypeTraits {
public:
    static bool is_integral() { return false; }
    static bool is_floating() { return false; }
};

template <>
class TypeTraits<int32_t> {
public:
    static bool is_integral() { return true; }
    static bool is_floating() { return false; }
    static int32_t max_value() { return 2147483647; }
};

template <typename T>
class TypeTraits<T*> {
public:
    static bool is_pointer() { return true; }
    static size_t pointer_size() { return sizeof(T*); }
};

} // namespace traits
