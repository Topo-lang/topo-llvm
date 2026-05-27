#pragma once
#include <string>
#include <vector>

namespace app {
void startup();
void init_logging();
void init_config();
void load_plugins();
void start_services();
void validate_config(int flags);

// Observable state for verification
const std::vector<std::string>& get_init_log();
} // namespace app
