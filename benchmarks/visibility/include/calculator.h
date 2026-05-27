#pragma once

namespace calc::core {
int add(int a, int b);
int subtract(int a, int b);
const int& get_last_result();
void log_operation(int result);
void reset_state();

namespace impl {
void set(int* out, int val);
}
} // namespace calc::core
