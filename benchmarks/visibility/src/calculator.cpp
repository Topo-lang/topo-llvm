#include "calculator.h"
#include <iostream>

namespace calc::core {

static int last_result = 0;

int add(int a, int b) {
    int r = a + b;
    log_operation(r);
    return r;
}

int subtract(int a, int b) {
    int r = a - b;
    log_operation(r);
    return r;
}

const int& get_last_result() {
    return last_result;
}

void log_operation(int result) {
    last_result = result;
}

void reset_state() {
    impl::set(&last_result, 0);
}

namespace impl {
void set(int* out, int val) {
    *out = val;
}
} // namespace impl

} // namespace calc::core
