// topo-prof — Performance analysis CLI for Topo pipelines
//
// Subcommands:
//   analyze  — Static TTI cost analysis of pipeline stages
//   profile  — Read runtime sampling data and compare with TTI estimates
//   hints    — Compare declared data-aware hints vs runtime sampling data
//
// This file is a THIN SHIM. The analyze / profile / hints
// orchestration moved into topo-core/lib/Profile (zero-LLVM,
// topo::profile::ProfileEngine), and the LLVM-bound static TTI estimation
// moved into topo-llvm/lib/Profile (topo::profile::llvm_backend::
// LlvmTTIProvider). This shim only parses argv, wires the LLVM TTI provider
// into the core profile path, and forwards std::cout/std::cerr — so the CLI
// surface, output bytes and exit codes are identical to before.

#include "Profile/Llvm/LlvmTTIProvider.h"
#include "topo/Profile/ProfileEngine.h"

#include <iostream>
#include <string>

static void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " <command> [options]\n"
              << "\nCommands:\n"
              << "  analyze   Static TTI cost analysis\n"
              << "  profile   Runtime sampling data analysis\n"
              << "  hints     Compare declared data-aware hints vs runtime\n"
              << "\nAnalyze options:\n"
              << "  --project <dir>     Project directory (default: .)\n"
              << "  --focus <type>      Focus: parallel, pipeline (default: all)\n"
              << "  --format <fmt>      Output format: text, json (default: text)\n"
              << "\nProfile options:\n"
              << "  --samples <path>    Runtime samples JSON file (required)\n"
              << "  --project <dir>     Project directory for TTI comparison\n"
              << "  --binary <path>     Binary path (recorded in report)\n"
              << "  --format <fmt>      Output format: text, json (default: text)\n"
              << "  --output <path>     Output report path (default: stdout)\n"
              << "\nHints options:\n"
              << "  --samples <path>    Runtime samples JSON file (required)\n"
              << "  --project <dir>     Project directory (default: .)\n"
              << "  --format <fmt>      Output format: text, json (default: text)\n"
              << "  --output <path>     Output report path (default: stdout)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string command = argv[1];

    if (command == "analyze") {
        std::string projectDir = ".";
        std::string focus;
        std::string format = "text";

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--project" && i + 1 < argc)
                projectDir = argv[++i];
            else if (arg == "--focus" && i + 1 < argc)
                focus = argv[++i];
            else if (arg == "--format" && i + 1 < argc)
                format = argv[++i];
        }

        return topo::profile::runAnalyze(projectDir, focus, format, std::cout, std::cerr);
    }

    if (command == "profile") {
        std::string samplesPath;
        std::string projectDir;
        std::string binaryPath;
        std::string format = "text";
        std::string outputPath;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--samples" && i + 1 < argc)
                samplesPath = argv[++i];
            else if (arg == "--project" && i + 1 < argc)
                projectDir = argv[++i];
            else if (arg == "--binary" && i + 1 < argc)
                binaryPath = argv[++i];
            else if (arg == "--format" && i + 1 < argc)
                format = argv[++i];
            else if (arg == "--output" && i + 1 < argc)
                outputPath = argv[++i];
        }

        if (samplesPath.empty()) {
            std::cerr << "error: --samples is required for profile command\n";
            return 1;
        }

        topo::profile::llvm_backend::LlvmTTIProvider tti;
        return topo::profile::runProfile(samplesPath, projectDir, binaryPath, format, outputPath,
                                         &tti, std::cout, std::cerr);
    }

    if (command == "hints") {
        std::string samplesPath;
        std::string projectDir = ".";
        std::string format = "text";
        std::string outputPath;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--samples" && i + 1 < argc)
                samplesPath = argv[++i];
            else if (arg == "--project" && i + 1 < argc)
                projectDir = argv[++i];
            else if (arg == "--format" && i + 1 < argc)
                format = argv[++i];
            else if (arg == "--output" && i + 1 < argc)
                outputPath = argv[++i];
        }

        if (samplesPath.empty()) {
            std::cerr << "error: --samples is required for hints command\n";
            return 1;
        }

        return topo::profile::runHints(samplesPath, projectDir, format, outputPath,
                                       std::cout, std::cerr);
    }

    std::cerr << "error: unknown command '" << command << "'\n";
    printUsage(argv[0]);
    return 1;
}
