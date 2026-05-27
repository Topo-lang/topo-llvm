#include "class_template.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#ifdef _MSC_VER
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

static bool approx(float a, float b, float eps = 0.001f) {
    return std::fabs(a - b) < eps;
}

BENCH_NOINLINE
static int friendly_bench(int seed) {
    collection::Vector<int32_t> v;
    for (int i = 0; i < 50; ++i)
        v.push_back(seed + i);
    int s = traits::sum(v);

    collection::HashMap<int32_t, double> hm;
    for (int i = 0; i < 20; ++i)
        hm.insert(seed + i, static_cast<double>(i));
    s += static_cast<int>(hm.get(seed));
    return s;
}

BENCH_NOINLINE
static int unfriendly_bench(int seed) {
    container::Circle c(static_cast<float>(seed % 100 + 1));
    float a = c.area();

    collection::Array<float, 16> arr;
    for (int i = 0; i < 16; ++i)
        arr.set(i, static_cast<float>(seed + i));

    collection::Vector<float> vf;
    for (int i = 0; i < 20; ++i)
        vf.push_back(static_cast<float>(seed + i) * 0.5f);

    return static_cast<int>(a) + static_cast<int>(arr.at(seed % 16)) + static_cast<int>(vf.front());
}

int main() {
    std::printf("08_class_template: running tests...\n");

    // ── Step 1: Basic class declarations ──────────────────────

    std::printf("  Step 1: classes + inheritance\n");
    {
        // MathConstants: static members
        assert(approx(container::MathConstants::pi(), 3.14159265f));
        assert(approx(container::MathConstants::e(), 2.71828183f));

        // Circle: inheritance, constructor, destructor
        container::Circle c(5.0f);
        assert(approx(c.radius(), 5.0f));
        assert(approx(c.area(), container::MathConstants::pi() * 25.0f));
        assert(c.perimeter() > 0.0f);

        // Shape base class access through Circle
        const container::Shape& s = c;
        assert(approx(s.area(), c.area()));
    }

    // ── Step 2: Function templates ────────────────────────────

    std::printf("  Step 2: function templates\n");
    {
        // max<int>
        assert(algorithm::max(3, 7) == 7);
        assert(algorithm::max(10, 2) == 10);

        // max<double>
        assert(approx(static_cast<float>(algorithm::max(1.5, 2.5)), 2.5f));

        // convert<int, double>
        int32_t i = algorithm::convert<int32_t>(3.14);
        assert(i == 3);

        // convert<float, int>
        float f = algorithm::convert<float>(42);
        assert(approx(f, 42.0f));

        // fill_array<int, 8>
        algorithm::fill_array<int, 8>(99);

        // normalize (fn block: stage<1> max(x, y))
        assert(algorithm::normalize(3, 5) == 5);
        assert(algorithm::normalize(10, 2) == 10);
    }

    // ── Step 3: Class templates + CRTP ────────────────────────

    std::printf("  Step 3: class templates + CRTP\n");
    {
        // Vector<int>
        collection::Vector<int32_t> vi;
        assert(vi.empty());
        assert(vi.size() == 0);
        vi.push_back(10);
        vi.push_back(20);
        vi.push_back(30);
        assert(vi.size() == 3);
        assert(vi.front() == 10);
        assert(vi.back() == 30);
        assert(!vi.empty());

        // Vector copy
        collection::Vector<int32_t> vi2 = vi;
        assert(vi2.size() == 3);
        assert(vi2.front() == 10);

        // Vector<float>
        collection::Vector<float> vf;
        vf.push_back(1.5f);
        vf.push_back(2.5f);
        assert(vf.size() == 2);
        assert(approx(vf.front(), 1.5f));

        // HashMap<int, double>
        collection::HashMap<int32_t, double> hm;
        hm.insert(1, 100.0);
        hm.insert(2, 200.0);
        hm.insert(3, 300.0);
        assert(hm.size() == 3);
        assert(hm.contains(2));
        assert(!hm.contains(99));
        assert(approx(static_cast<float>(hm.get(1)), 100.0f));
        // overwrite existing key
        hm.insert(1, 999.0);
        assert(approx(static_cast<float>(hm.get(1)), 999.0f));
        assert(hm.size() == 3);

        // Array<float, 16>
        collection::Array<float, 16> arr;
        {
            int32_t cap = collection::Array<float, 16>::capacity();
            assert(cap == 16);
        }
        arr.set(0, 3.14f);
        arr.set(15, 2.72f);
        assert(approx(arr.at(0), 3.14f));
        assert(approx(arr.at(15), 2.72f));

        // CRTP: Point : Printable<Point>
        collection::Point pt(3.0f, 4.0f);
        assert(approx(pt.x(), 3.0f));
        assert(approx(pt.y(), 4.0f));
        std::printf("    CRTP print: ");
        pt.println();
    }

    // ── Step 4: Constraints + Specialization ──────────────────

    std::printf("  Step 4: constraints + specialization\n");
    {
        // adapt Numeric for Double
        assert(approx(static_cast<float>(traits::double_add(1.5, 2.5)), 4.0f));
        assert(approx(static_cast<float>(traits::double_multiply(3.0, 4.0)), 12.0f));
        assert(approx(static_cast<float>(traits::double_zero), 0.0f));

        // constrained template: sum
        collection::Vector<int32_t> data;
        data.push_back(10);
        data.push_back(20);
        data.push_back(30);
        data.push_back(40);
        assert(traits::sum(data) == 100);

        // constrained template: clamp
        assert(traits::clamp(5, 0, 10) == 5);
        assert(traits::clamp(-5, 0, 10) == 0);
        assert(traits::clamp(15, 0, 10) == 10);

        // Full specialization: TypeTraits<int32_t>
        assert(traits::TypeTraits<int32_t>::is_integral() == true);
        assert(traits::TypeTraits<int32_t>::is_floating() == false);
        assert(traits::TypeTraits<int32_t>::max_value() == 2147483647);

        // Primary template: TypeTraits<double>
        assert(traits::TypeTraits<double>::is_integral() == false);
        assert(traits::TypeTraits<double>::is_floating() == false);

        // Partial specialization: TypeTraits<int*>
        assert(traits::TypeTraits<int32_t*>::is_pointer() == true);
        assert(traits::TypeTraits<int32_t*>::pointer_size() == sizeof(int32_t*));
    }

    // ── Step 5: Comptime + TypeFn + Template Template Params ──

    std::printf("  Step 5: comptime + typefn + TTP\n");
    {
        // comptime if — both paths callable
        meta::small_path();
        meta::large_path();

        // comptime fn factorial
        static_assert(meta::factorial(0) == 1, "0! == 1");
        static_assert(meta::factorial(1) == 1, "1! == 1");
        static_assert(meta::factorial(5) == 120, "5! == 120");
        static_assert(meta::factorial(10) == 3628800, "10! == 3628800");

        // typefn Wider: type trait
        static_assert(sizeof(meta::Wider<float>::type) == sizeof(double), "Wider<float> == double");
        static_assert(sizeof(meta::Wider<int32_t>::type) == sizeof(size_t), "Wider<int> == size_t");

        // Template template parameter: Stack<int, Vector>
        meta::Stack<int32_t, collection::Vector> stack;
        assert(stack.empty());
        stack.push(42);
        assert(!stack.empty());
        assert(stack.top() == 42);
        stack.push(99);
        assert(stack.top() == 99);
    }

    // ── Step 6: Variadic templates ────────────────────────────

    std::printf("  Step 6: variadic templates\n");
    {
        // print_all with mixed types
        std::printf("    print_all: ");
        variadic::print_all(42, 3.14, "hello");

        // Tuple component count
        variadic::Tuple<int32_t, float, double> t3;
        assert(t3.component_count() == 3);

        variadic::Tuple<int32_t> t1;
        assert(t1.component_count() == 1);

        variadic::Tuple<> t0;
        assert(t0.component_count() == 0);
    }

    std::printf("08_class_template: all assertions passed\n");

    // --- Benchmark ---
    constexpr int ROUNDS = 7, WARMUP = 50, ITERS = 50000;

    auto bench = [](int rounds, int iters, auto&& work) -> long long {
        std::vector<long long> samples;
        for (int r = 0; r < rounds; ++r) {
            auto start = std::chrono::steady_clock::now();
            volatile int sink = 0;
            for (int it = 0; it < iters; ++it)
                sink = work(it);
            (void)sink;
            auto end = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        }
        std::sort(samples.begin(), samples.end());
        return samples[rounds / 2];
    };

    for (int i = 0; i < WARMUP; ++i) {
        friendly_bench(i);
        unfriendly_bench(i);
    }
    std::printf("RESULT_US_FRIENDLY=%lld\n", bench(ROUNDS, ITERS, friendly_bench));
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", bench(ROUNDS, ITERS, unfriendly_bench));
    return 0;
}
