#pragma once
#include <cstdint>
#include <cstddef>
#include <iostream>
#include <utility>

// ============================================================
// Step 1: Basic class declarations
// ============================================================

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

// ============================================================
// Step 2: Function templates
// ============================================================

namespace algorithm {

template <typename T>
T max(T a, T b) {
    return a > b ? a : b;
}

template <typename T, typename U>
T convert(U source) {
    return static_cast<T>(source);
}

template <typename T, int N>
void fill_array(T value) {
    // Non-type template param demo: compile-time array size
    T arr[N];
    for (int i = 0; i < N; ++i)
        arr[i] = value;
    (void)arr; // used for instantiation verification
}

// fn normalize { stage<1> max(x, y); }
template <typename T>
T normalize(T x, T y) {
    return algorithm::max(x, y);
}

} // namespace algorithm

// ============================================================
// Step 3: Class templates + CRTP
// ============================================================

namespace collection {

// --- Vector<T> ---

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
            count_ = other.count_;
            capacity_ = other.capacity_;
            data_ = nullptr;
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

    T front() const { return data_[0]; }
    T back() const { return data_[count_ - 1]; }
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    // Internal accessor for iteration
    const T& at(size_t i) const { return data_[i]; }

private:
    T* data_ = nullptr;
    size_t count_ = 0;
    size_t capacity_ = 0;
};

// --- HashMap<K, V> ---

template <typename K, typename V>
class HashMap {
    static constexpr size_t kMaxEntries = 256;
    struct Entry {
        K key;
        V value;
        bool occupied = false;
    };

public:
    void insert(K key, V value) {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].key == key) {
                entries_[i].value = value;
                return;
            }
        }
        if (count_ < kMaxEntries) {
            entries_[count_] = {key, value, true};
            ++count_;
        }
    }

    V get(const K& key) const {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].key == key) return entries_[i].value;
        }
        return V{};
    }

    bool contains(const K& key) const {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].key == key) return true;
        }
        return false;
    }

    size_t size() const { return count_; }

private:
    Entry entries_[kMaxEntries]{};
    size_t count_ = 0;
};

// --- Array<T, N> ---

template <typename T, int N>
class Array {
public:
    T at(int32_t index) const { return storage_[index]; }
    void set(int32_t index, T value) { storage_[index] = value; }
    static int32_t capacity() { return N; }

private:
    T storage_[N]{};
};

// --- Printable<Derived> (CRTP) ---

template <typename Derived>
class Printable {
public:
    void print() const { static_cast<const Derived*>(this)->print_impl(); }
    void println() const {
        print();
        std::cout << std::endl;
    }
};

// --- Point : Printable<Point> ---

class Point : public Printable<Point> {
    friend class Printable<Point>;

public:
    explicit Point(float x, float y) : x_(x), y_(y) {}
    float x() const { return x_; }
    float y() const { return y_; }

private:
    void print_impl() const { std::cout << "Point(" << x_ << ", " << y_ << ")"; }
    float x_;
    float y_;
};

} // namespace collection

// ============================================================
// Step 4: Constraints + Specialization
// ============================================================

namespace traits {

// Adapt functions for Double (adapt Numeric for Double)
double double_add(double a, double b);
double double_multiply(double a, double b);
extern const double double_zero;

// Constrained template: sum
template <typename T>
T sum(collection::Vector<T> data) {
    T result{};
    for (size_t i = 0; i < data.size(); ++i) {
        result = result + data.at(i);
    }
    return result;
}

// Constrained template: clamp
template <typename T>
T clamp(T value, T low, T high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// Primary template
template <typename T>
class TypeTraits {
public:
    static bool is_integral() { return false; }
    static bool is_floating() { return false; }
};

// Full specialization for int32_t (Int)
template <>
class TypeTraits<int32_t> {
public:
    static bool is_integral() { return true; }
    static bool is_floating() { return false; }
    static int32_t max_value() { return 2147483647; }
};

// Partial specialization for pointer types
template <typename T>
class TypeTraits<T*> {
public:
    static bool is_pointer() { return true; }
    static size_t pointer_size() { return sizeof(T*); }
};

} // namespace traits

// ============================================================
// Step 5: Comptime + TypeFn + Template Template Params
// ============================================================

namespace meta {

// comptime if — both paths exist in C++
void small_path();
void large_path();

// comptime fn factorial — constexpr in C++
constexpr int32_t factorial(int32_t n) {
    return n <= 1 ? 1 : n * factorial(n - 1);
}

// typefn Wider — type trait in C++
template <typename T>
struct Wider {
    using type = double;
};
template <>
struct Wider<float> {
    using type = double;
};
template <>
struct Wider<int32_t> {
    using type = size_t;
};

// Template template parameter: Stack<T, Container>
template <typename T, template <typename> class Container>
class Stack {
public:
    void push(const T& value) { storage_.push_back(value); }

    T pop() {
        T val = storage_.back();
        // simplified: no actual pop_back on Vector, just track size
        return val;
    }

    T top() const { return storage_.back(); }
    bool empty() const { return storage_.empty(); }

private:
    Container<T> storage_;
};

} // namespace meta

// ============================================================
// Step 6: Variadic templates
// ============================================================

namespace variadic {

// Variadic function template
template <typename... Ts>
void print_all(Ts... args) {
    // C++17 fold expression
    ((std::cout << args << " "), ...);
    std::cout << std::endl;
}

// Variadic class template
template <typename... Components>
class Tuple {
public:
    size_t component_count() const { return sizeof...(Components); }
};

} // namespace variadic
