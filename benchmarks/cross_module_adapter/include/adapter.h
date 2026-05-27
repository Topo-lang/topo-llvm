#pragma once

namespace types {

class Pair {
public:
    explicit Pair(int first, int second) : first_(first), second_(second) {}
    int first() const { return first_; }
    int second() const { return second_; }

private:
    int first_, second_;
};

int pair_sum(const Pair& a, const Pair& b);

} // namespace types

namespace app {

int run_adapter(int a, int b, int c, int d);
int combine(const types::Pair& p);

} // namespace app
