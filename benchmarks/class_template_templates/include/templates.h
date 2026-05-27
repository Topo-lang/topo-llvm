#pragma once
// class_template_templates — micro-benchmark for Step 2 (function templates)
// + Step 3 class templates (Vector, HashMap, Array). Step 3's CRTP subset
// lives in class_template_crtp.

#include <cstdint>
#include <cstddef>

namespace algorithm {

template <typename T>
T max(T a, T b) { return a > b ? a : b; }

template <typename T, typename U>
T convert(U source) { return static_cast<T>(source); }

template <typename T, int N>
void fill_array(T value) {
    T arr[N];
    for (int i = 0; i < N; ++i)
        arr[i] = value;
    (void)arr;
}

template <typename T>
T normalize(T x, T y) { return algorithm::max(x, y); }

} // namespace algorithm

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
    const T& at(size_t i) const { return data_[i]; }

private:
    T* data_ = nullptr;
    size_t count_ = 0;
    size_t capacity_ = 0;
};

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

template <typename T, int N>
class Array {
public:
    T at(int32_t index) const { return storage_[index]; }
    void set(int32_t index, T value) { storage_[index] = value; }
    static int32_t capacity() { return N; }

private:
    T storage_[N]{};
};

} // namespace collection
