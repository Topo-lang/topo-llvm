#pragma once

namespace compute {
void run_critical_path();
void process_data(int size);
void run_pipeline();
void cleanup();
void log_metrics();
void critical_helper(int n);
void internal_step();
} // namespace compute
