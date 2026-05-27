#pragma once
// class_template_comptime — Step 5 micro-benchmark:
// constexpr factorial, typefn Wider, template-template-parameter Stack.
//
// A trivial local Vector is provided for Stack<T, Vector>.

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
    T back() const { return data_[count_ - 1]; }
    bool empty() const { return count_ == 0; }
    size_t size() const { return count_; }

private:
    T* data_ = nullptr;
    size_t count_ = 0;
    size_t capacity_ = 0;
};

} // namespace collection

namespace meta {

void small_path();
void large_path();

constexpr int32_t factorial(int32_t n) {
    return n <= 1 ? 1 : n * factorial(n - 1);
}

template <typename T>
struct Wider { using type = double; };
template <>
struct Wider<float> { using type = double; };
template <>
struct Wider<int32_t> { using type = size_t; };

template <typename T, template <typename> class Container>
class Stack {
public:
    void push(const T& value) { storage_.push_back(value); }
    T top() const { return storage_.back(); }
    bool empty() const { return storage_.empty(); }

private:
    Container<T> storage_;
};

} // namespace meta
