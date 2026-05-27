#include "app.h"
#include <iostream>

namespace app {

static std::vector<std::string> init_log;
void init_logging() {
    init_log.push_back("logging");
    volatile int acc = 0;
    for (int i = 0; i < 100; ++i)
        acc = acc ^ (acc << 1) ^ (i * 3 + 1);
}

void init_config() {
    init_log.push_back("config");
    validate_config(0);
    volatile int acc = 0;
    for (int i = 0; i < 100; ++i)
        acc = acc ^ (acc << 1) ^ (i * 7 + 3);
}

void load_plugins() {
    init_log.push_back("plugins");
    volatile int acc = 0;
    for (int i = 0; i < 100; ++i)
        acc = acc ^ (acc << 1) ^ (i * 11 + 5);
}

void start_services() {
    init_log.push_back("services");
    volatile int acc = 0;
    for (int i = 0; i < 100; ++i)
        acc = acc ^ (acc << 1) ^ (i * 13 + 7);
}

void validate_config(int flags) {
    (void)flags;
}

void startup() {
    init_log.clear();
    init_logging();
    init_config();
    load_plugins();
    start_services();
}

const std::vector<std::string>& get_init_log() {
    return init_log;
}

} // namespace app
