#include "E2eHarness.h"

#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"

#include <cstdlib>
#include <fstream>

namespace topo::test::e2e {

// ============================================================================
// B1: Functional Correctness Tests
// ============================================================================

using Functional = E2eFixture;

// --- 01: Visibility ---

TEST_F(Functional, Visibility) {
    auto build = topoBuild("visibility");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("visibility", "hello_visibility");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "01_hello_visibility: all assertions passed\n"
                        "...");
}

// --- 02: Stages ---

TEST_F(Functional, Stages) {
    auto build = topoBuild("stages");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("stages", "stages");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "02_stages: all assertions passed\n"
                        "...");
}

// --- 03: MultiFile (merged into 01_hello_visibility) ---

// --- 04: MultiReturn ---

TEST_F(Functional, MultiReturn) {
    auto build = topoBuild("multireturn");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("multireturn", "multi_return");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "04_multi_return: all assertions passed\n"
                        "...");
}

// --- 05: Pipeline ---

TEST_F(Functional, Pipeline) {
    auto build = topoBuild("pipeline");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("pipeline", "pipeline");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "05_pipeline: pipeline result = 125\n"
                        "05_pipeline: all assertions passed\n"
                        "...");
}

// --- 06: SharedLib ---

TEST_F(Functional, SharedLib) {
    // SharedLib is a fixture project — use fixturesDir_
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    auto build = topoBuild("shared_lib");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    // Verify shared library file exists
    auto libPath = sharedLibPath("shared_lib", "hashlib");
    EXPECT_TRUE(fs::exists(libPath)) << "Shared library not found: " << libPath;

    // Build and run the driver
    auto driverBuild = compileDriver("shared_lib",
                                     "driver/main.cpp",
                                     "hashlib_driver",
                                     {"include"},
                                     std::string("hashlib") + std::string(platform::SharedLibSuffix));

    if (driverBuild.exitCode == 0) {
        auto run = runBinary("shared_lib", "hashlib_driver");
        ASSERT_EQ(run.exitCode, 0) << "Driver failed:\n" << run.output;

        assertOutputMatches(run.output,
                            "06_shared_lib: hash1={{NUM}} hash2={{NUM}} combined={{NUM}}\n"
                            "06_shared_lib: all assertions passed");
    }

    projectsDir_ = savedProjects;
}

// --- 95: StaticLib ---

TEST_F(Functional, StaticLib) {
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    auto build = topoBuild("static_lib");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    // Verify static library exists
    fs::path projDir = fixturesDir_ / "static_lib";
    fs::path libPath = projDir / ("mathlib" + std::string(platform::StaticLibSuffix));
    if (!fs::exists(libPath)) libPath = projDir / "build" / ("mathlib" + std::string(platform::StaticLibSuffix));
    EXPECT_TRUE(fs::exists(libPath)) << "Static library not found at: " << libPath;

    // Build driver linking against static lib
    auto driverBuild =
        compileDriver("static_lib", "driver/main.cpp", "mathlib_driver", {"include"}, libPath.filename().string());

    if (driverBuild.exitCode == 0) {
        auto run = runBinary("static_lib", "mathlib_driver");
        ASSERT_EQ(run.exitCode, 0) << "Driver failed:\n" << run.output;
        assertOutputMatches(run.output, "sum=7 prod=30");
    } else {
        // Fallback: link directly with the full path to the static lib
        std::string clangxx = (llvmBinDir_ / ("clang++" + std::string(platform::ExeSuffix))).generic_string();

        std::vector<std::string> args;
        args.push_back("-O2");
        args.push_back("-std=c++17");

        if constexpr (platform::IsMacOS) {
            auto sdkResult = platform::runProcessCapture("xcrun", {"--show-sdk-path"});
            if (sdkResult.exitCode == 0 && !sdkResult.stdoutOutput.empty()) {
                std::string sdkPath = sdkResult.stdoutOutput;
                while (!sdkPath.empty() && (sdkPath.back() == '\n' || sdkPath.back() == '\r'))
                    sdkPath.pop_back();
                args.push_back("-isysroot");
                args.push_back(sdkPath);
            }
        }

        args.push_back("-I" + (projDir / "include").generic_string());
        args.push_back((projDir / "driver/main.cpp").generic_string());
        args.push_back(libPath.generic_string());

        fs::path buildDir = projDir / "build";
        fs::create_directories(buildDir);
        fs::path outputPath = buildDir / ("mathlib_driver" + std::string(platform::ExeSuffix));
        args.push_back("-o");
        args.push_back(outputPath.generic_string());

        auto manualBuild = platform::runProcessCapture(clangxx, args);
        ASSERT_EQ(manualBuild.exitCode, 0) << "Manual driver build failed:\n" << manualBuild.stdoutOutput;

        auto run = runBinary("static_lib", "mathlib_driver");
        ASSERT_EQ(run.exitCode, 0) << "Driver failed:\n" << run.output;
        assertOutputMatches(run.output, "sum=7 prod=30");
    }

    projectsDir_ = savedProjects;
}

// --- 07: PerfComparison (merged into 05_pipeline) ---

// --- 08: ClassTemplate ---

TEST_F(Functional, ClassTemplate) {
    auto build = topoBuild("class_template");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("class_template", "class_template");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "...\n"
                        "08_class_template: all assertions passed\n"
                        "...");
}

// --- 09: AuxiliaryTypes ---

TEST_F(Functional, AuxiliaryTypes) {
    auto build = topoBuild("auxiliary_types");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("auxiliary_types", "auxiliary_types");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "09_auxiliary_types: all assertions passed\n"
                        "...");
}

// --- 10: ParallelRuntime ---

TEST_F(Functional, ParallelRuntime) {
    auto build = topoBuild("parallel");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("parallel", "parallel_runtime");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "results: a={{NUM}}, b=42\n"
                        "...");
}

// --- 11: AutoParallel (merged into 10_parallel_runtime) ---

// --- 12: IREmbedding ---

TEST_F(Functional, IREmbedding) {
    auto build = topoBuild("jit");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("jit", "ir_embedding");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "~ processing::run\\(42\\) = .*\n"
                        "...");
}

// --- 13: JITSpecialize (merged into 12_ir_embedding) ---

// --- 14: AdaptiveParallel ---

TEST_F(Functional, AdaptiveParallel) {
    auto build = topoBuild("adaptive");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("adaptive", "adaptive_parallel");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        // Scenario 1+2: Basic adaptive optimization
                        "=== Running pipeline with adaptive optimization ===\n"
                        "  result: {{NUM}}, specializations: {{NUM}}\n"
                        "RESULT_US_FRIENDLY={{NUM}}\n"
                        "RESULT_US_UNFRIENDLY={{NUM}}\n"
                        "  Specializations: {{NUM}}, Deoptimizations: {{NUM}}, Active JIT: {{NUM}}\n"
                        "...\n"
                        // Scenario 3: Multi-pipeline concurrent JIT stress
                        "=== Multi-pipeline concurrent scenario ===\n"
                        "  Before: spec={{NUM}} deopt={{NUM}} active={{NUM}}\n"
                        "...\n"
                        "  After:  spec={{NUM}} deopt={{NUM}} active={{NUM}}\n"
                        "  Stabilization time: {{NUM}} ms\n"
                        "  Pipeline process: result={{NUM}}, valid=yes\n"
                        "  Pipeline analyze: result={{NUM}}, valid=yes\n"
                        "  Pipeline transform: result={{NUM}}, valid=yes\n"
                        "  Pipeline compress: result={{NUM}}, valid=yes\n"
                        "  Rapid dispatch validation: PASS\n"
                        "  Concurrent execution completed without crash\n"
                        "...\n"
                        // Scenario 4: Deoptimization
                        "=== Deoptimization scenario ===\n"
                        "  Step 1 done: spec={{NUM}} deopt={{NUM}} active={{NUM}}\n"
                        "  Step 2 done: spec={{NUM}} deopt={{NUM}} active={{NUM}}\n"
                        "  New deoptimizations during scenario: {{NUM}}\n");
}

// --- 15: RustBasic (build-only) ---

TEST_F(Functional, RustBasic) {
#ifdef _WIN32
    int ret = std::system("cargo --version > NUL 2>&1");
#else
    int ret = std::system("cargo --version > /dev/null 2>&1");
#endif
    if (ret != 0) {
        GTEST_SKIP() << "cargo not available, skipping RustBasic";
    }

    // RustBasic is a fixture project — use fixturesDir_
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    auto build = topoBuild("rust_basic");
    if (!build.spawned) {
        // Spawn failure (missing fixture dir, missing topo-build) is a HARD
        // failure — a skip here would silently drop the fixture coverage
        // (issue e2e-harness-spawn-failure-masks-as-pass-or-skip).
        FAIL() << "topo-build could not be spawned for rust_basic — fixture or "
                  "toolchain missing (not a skip-worthy toolchain limitation)";
    } else if (build.exitCode != 0) {
        GTEST_SKIP() << "Rust build failed (toolchain may be incomplete): exit code "
                      + std::to_string(build.exitCode);
    }
    // lib crate — no binary to run, just verify build succeeds

    projectsDir_ = savedProjects;
}

// --- 16: Ownership ---

TEST_F(Functional, Ownership) {
    auto build = topoBuild("ownership");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("ownership", "ownership");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "16_ownership: all assertions passed\n"
                        "...");
}

// --- 20: Incremental Build ---

TEST_F(Functional, IncrementalBuild) {
    namespace fs = std::filesystem;

    // Incremental is a fixture project — use fixturesDir_
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    // Clean any prior cache
    fs::path projDir = projectsDir_ / "incremental";
    fs::path cacheDir = projDir / ".topo-cache";
    {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }

    // First build: full build, creates cache
    auto build1 = topoBuild("incremental");
    ASSERT_EQ(build1.exitCode, 0) << "First build failed:\n" << build1.output;

    // Verify cache directory was created
    EXPECT_TRUE(fs::exists(cacheDir)) << ".topo-cache/ should exist after build";
    EXPECT_TRUE(fs::exists(cacheDir / "manifest.json"));
    EXPECT_TRUE(fs::exists(cacheDir / "symbols.json"));
    EXPECT_TRUE(fs::exists(cacheDir / "visibility.json"));

    // Verify binary works
    auto run1 = runBinary("incremental", "incremental_test");
    ASSERT_EQ(run1.exitCode, 0) << "Binary failed:\n" << run1.output;
    assertOutputMatches(run1.output, "43");

    // Second build: no changes, should use cache
    auto build2 = topoBuild("incremental");
    ASSERT_EQ(build2.exitCode, 0) << "Second build failed:\n" << build2.output;

    // Verify binary still works after cache-hit build
    auto run2 = runBinary("incremental", "incremental_test");
    ASSERT_EQ(run2.exitCode, 0) << "Binary failed after cache-hit build:\n" << run2.output;
    assertOutputMatches(run2.output, "43");

    // Cleanup cache
    {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }

    projectsDir_ = savedProjects;
}

// --- 20a: Incremental Build — .topo file change ---

TEST_F(Functional, IncrementalBuild_TopoFileChange) {
    namespace fs = std::filesystem;

    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    fs::path projDir = projectsDir_ / "incremental";
    fs::path cacheDir = projDir / ".topo-cache";
    fs::path topoFile = projDir / "topo" / "main.topo";

    // Clean cache
    {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }

    // Save original .topo content
    std::string originalTopo;
    {
        std::ifstream f(topoFile);
        originalTopo.assign(std::istreambuf_iterator<char>(f), {});
    }

    // Full build
    auto build1 = topoBuild("incremental");
    ASSERT_EQ(build1.exitCode, 0) << "First build failed:\n" << build1.output;

    auto run1 = runBinary("incremental", "incremental_test");
    ASSERT_EQ(run1.exitCode, 0) << "Binary failed:\n" << run1.output;
    assertOutputMatches(run1.output, "43");

    // Modify .topo: add a comment (changes mtime, triggers cache invalidation)
    {
        std::ofstream f(topoFile);
        f << "// modified for incremental test\n" << originalTopo;
    }

    // Rebuild — should succeed (cache invalidated by .topo mtime change)
    auto build2 = topoBuild("incremental");

    // Restore original before any assertions that could abort
    {
        std::ofstream f(topoFile);
        f << originalTopo;
    }
    {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }
    projectsDir_ = savedProjects;

    ASSERT_EQ(build2.exitCode, 0) << "Rebuild after .topo change failed:\n" << build2.output;
}

// --- 20b: Incremental Build — Topo.toml change ---

TEST_F(Functional, IncrementalBuild_TopoTomlChange) {
    namespace fs = std::filesystem;

    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    fs::path projDir = projectsDir_ / "incremental";
    fs::path cacheDir = projDir / ".topo-cache";
    fs::path tomlFile = projDir / "Topo.toml";

    // Clean cache
    {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }

    // Save original Topo.toml content
    std::string originalToml;
    {
        std::ifstream f(tomlFile);
        originalToml.assign(std::istreambuf_iterator<char>(f), {});
    }

    // Full build
    auto build1 = topoBuild("incremental");
    ASSERT_EQ(build1.exitCode, 0) << "First build failed:\n" << build1.output;

    auto run1 = runBinary("incremental", "incremental_test");
    ASSERT_EQ(run1.exitCode, 0) << "Binary failed:\n" << run1.output;
    assertOutputMatches(run1.output, "43");

    // Modify Topo.toml: change C++ standard
    {
        std::ofstream f(tomlFile);
        f << "[project]\n"
          << "name = \"incremental_test\"\n"
          << "\n"
          << "[topo]\n"
          << "root = \"topo/main.topo\"\n"
          << "\n"
          << "[build]\n"
          << "sources = [\"src/*.cpp\"]\n"
          << "include = [\"include\"]\n"
          << "standard = \"c++20\"\n"
          << "output = \"incremental_test\"\n";
    }

    // Rebuild — should succeed (config fingerprint changed → full rebuild)
    auto build2 = topoBuild("incremental");

    // Restore original before any assertions that could abort
    {
        std::ofstream f(tomlFile);
        f << originalToml;
    }
    {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }
    projectsDir_ = savedProjects;

    ASSERT_EQ(build2.exitCode, 0) << "Rebuild after Topo.toml change failed:\n" << build2.output;
}

// --- 20c: Incremental Build — cache deleted ---

TEST_F(Functional, IncrementalBuild_CacheDeleted) {
    namespace fs = std::filesystem;

    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    fs::path projDir = projectsDir_ / "incremental";
    fs::path cacheDir = projDir / ".topo-cache";

    // Clean cache, do full build
    {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }

    auto build1 = topoBuild("incremental");
    ASSERT_EQ(build1.exitCode, 0) << "First build failed:\n" << build1.output;

    // Verify cache exists
    ASSERT_TRUE(fs::exists(cacheDir)) << ".topo-cache/ should exist after build";

    // Delete cache entirely
    {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }
    ASSERT_FALSE(fs::exists(cacheDir)) << ".topo-cache/ should be gone";

    // Rebuild — should gracefully fall back to full build
    auto build2 = topoBuild("incremental");
    ASSERT_EQ(build2.exitCode, 0) << "Rebuild after cache deletion failed:\n" << build2.output;

    // Binary should still work
    auto run2 = runBinary("incremental", "incremental_test");
    ASSERT_EQ(run2.exitCode, 0) << "Binary failed after cache deletion:\n" << run2.output;
    assertOutputMatches(run2.output, "43");

    // Cleanup
    {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }

    projectsDir_ = savedProjects;
}

// --- 20d: Incremental Build — corrupted manifest ---

TEST_F(Functional, IncrementalBuild_CorruptedManifest) {
    namespace fs = std::filesystem;

    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    fs::path projDir = projectsDir_ / "incremental";
    fs::path cacheDir = projDir / ".topo-cache";
    fs::path manifestFile = cacheDir / "manifest.json";

    // Clean cache, do full build
    {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }

    auto build1 = topoBuild("incremental");
    ASSERT_EQ(build1.exitCode, 0) << "First build failed:\n" << build1.output;

    // Verify manifest exists
    ASSERT_TRUE(fs::exists(manifestFile)) << "manifest.json should exist after build";

    // Corrupt manifest with invalid JSON
    {
        std::ofstream f(manifestFile);
        f << "{broken";
    }

    // Rebuild — should gracefully fall back to full build
    auto build2 = topoBuild("incremental");
    ASSERT_EQ(build2.exitCode, 0) << "Rebuild after manifest corruption failed:\n" << build2.output;

    // Binary should still work
    auto run2 = runBinary("incremental", "incremental_test");
    ASSERT_EQ(run2.exitCode, 0) << "Binary failed after manifest corruption:\n" << run2.output;
    assertOutputMatches(run2.output, "43");

    // Cleanup
    {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }

    projectsDir_ = savedProjects;
}

// --- 23: Indirection ---

TEST_F(Functional, Indirection) {
    auto build = topoBuild("indirection");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("indirection", "indirection");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    // Correctness lines still present; benchmark results follow
    assertOutputMatches(run.output,
                        "23_indirection: result=120\n"
                        "23_indirection: all assertions passed\n"
                        "...");
}

// --- 24: LoopParallel ---

TEST_F(Functional, LoopParallel) {
    auto build = topoBuild("loop_parallel");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("loop_parallel", "loop_parallel");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "24_loop_parallel: result={{NUM}}\n"
                        "24_loop_parallel: all assertions passed\n"
                        "...");
}

// --- 25: LifetimeArena ---

TEST_F(Functional, LifetimeArena) {
    auto build = topoBuild("lifetime");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("lifetime", "lifetime_arena");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    // Correctness lines still present; benchmark results follow
    assertOutputMatches(run.output,
                        "25_lifetime_arena: result=37745100\n"
                        "25_lifetime_arena: all assertions passed\n"
                        "...");
}

// --- 26: Hints ---

TEST_F(Functional, Hints) {
    auto build = topoBuild("hints");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("hints", "hints");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "26_hints: n=100 sum={{NUM}}\n"
                        "26_hints: all assertions passed\n"
                        "...");
}

// --- 27: RustRuntime (merged into 17_rust_basic — Rust benchmark deferred) ---

// --- 28: Observability ---

TEST_F(Functional, Observability) {
    auto build = topoBuild("observability");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("observability", "observability");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "...\n"
                        "28_observability: result=-539764182\n"
                        "28_observability: all assertions passed\n"
                        "...");
}

// --- 29: DataLayout (correctness via 22_data_layout_perf) ---
// Project 29 merged into 22. Correctness covered by 22's output validation.

TEST_F(Functional, DataLayout) {
    auto build = topoBuild("data_layout");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("data_layout", "data_layout_perf");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    // 22 now outputs benchmark results; verify it completes successfully
    assertOutputMatches(run.output,
                        "...\n"
                        "22_data_layout_perf: done");
}

// --- 90: ConfigValidation (adaptive missing deps) ---

TEST_F(Functional, ConfigValidation_AdaptiveMissingDeps) {
    // ConfigValidation is a fixture project — use fixturesDir_
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    auto build = topoBuild("config_validation");
    ASSERT_TRUE(build.spawned) << "topo-build could not be spawned — validation contract not exercised:\n" << build.output;
    EXPECT_NE(build.exitCode, 0) << "topo-build should fail when [adaptive] enabled=true "
                                    "but embed_ir and parallel.instrument are missing";

    projectsDir_ = savedProjects;
}

// --- 91: Config All Off ---

TEST_F(Functional, ConfigAllOff) {
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    auto build = topoBuild("config_all_off");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("config_all_off", "config_all_off");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;
    assertOutputMatches(run.output, "30");

    projectsDir_ = savedProjects;
}

// --- 92: Config Parallel + Lifetime ---

TEST_F(Functional, ConfigParallelLifetime) {
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    auto build = topoBuild("config_parallel_lifetime");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("config_parallel_lifetime", "config_parallel_lifetime");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;
    assertOutputMatches(run.output, "42");

    projectsDir_ = savedProjects;
}

// --- 93: Error - .topo syntax error ---

TEST_F(Functional, ErrorTopoSyntax) {
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    auto build = topoBuild("error_topo_syntax");
    ASSERT_TRUE(build.spawned) << "topo-build could not be spawned — syntax-error contract not exercised:\n" << build.output;
    EXPECT_NE(build.exitCode, 0) << "topo-build should fail on .topo syntax error";

    projectsDir_ = savedProjects;
}

// --- 94: Error - invalid Topo.toml field ---

TEST_F(Functional, ErrorInvalidToml) {
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    auto build = topoBuild("error_invalid_toml");
    ASSERT_TRUE(build.spawned) << "topo-build could not be spawned — invalid-toml contract not exercised:\n" << build.output;
    EXPECT_NE(build.exitCode, 0) << "topo-build should fail on invalid output_type 'banana'";

    projectsDir_ = savedProjects;
}

// --- 96/97: ConfigAllOn + ConfigParallelAdaptive removed ---
// Blocked by #063: LLVM backend OOM (60G+ on 24G device)
// These fixtures require adaptive/JIT features that exceed available memory.
// Re-add after #063 is resolved.

// --- 98: Error - link failure (missing symbol) ---

TEST_F(Functional, ErrorLinkFailure) {
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    auto build = topoBuild("error_link_failure");
    ASSERT_TRUE(build.spawned) << "topo-build could not be spawned — link-failure contract not exercised:\n" << build.output;
    EXPECT_NE(build.exitCode, 0) << "topo-build should fail when declared public function has no implementation";

    projectsDir_ = savedProjects;
}

// --- 99: Checker Completeness (pass) ---

TEST_F(Functional, CheckerCompletenessPass) {
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    auto build = topoBuild("checker_completeness_pass");
    ASSERT_EQ(build.exitCode, 0) << "topo-build should succeed when all declarations match:\n" << build.output;

    auto run = runBinary("checker_completeness_pass", "checker_completeness_pass");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    projectsDir_ = savedProjects;
}

// --- 100: Checker Completeness (violation) ---

TEST_F(Functional, CheckerCompletenessViolation) {
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    auto build = topoBuild("checker_completeness_violation");
    ASSERT_TRUE(build.spawned) << "topo-build could not be spawned — completeness-violation contract not exercised:\n" << build.output;
    EXPECT_NE(build.exitCode, 0) << "topo-build should fail when .topo declares a function missing from host code";

    projectsDir_ = savedProjects;
}

// --- Spawn-failure regression ---
//
// A spawn failure (missing exe, missing fixture cwd) surfaces as
// exitCode -1 + empty output from runProcessCapture — the same in-band
// value a real tool exit could produce. The harness must flag it
// explicitly (RunResult::spawned == false) so consumers hard-fail instead
// of passing vacuously (EXPECT_NE(exitCode, 0)) or skipping (RustBasic).
// Issue: e2e-harness-spawn-failure-masks-as-pass-or-skip.

TEST_F(Functional, SpawnFailureDetected) {
    auto savedProjects = projectsDir_;
    projectsDir_ = fixturesDir_;

    // Point topoBuildExe_ at a nonexistent executable — spawn fails before
    // any fixture is touched.
    auto savedExe = topoBuildExe_;
    topoBuildExe_ = savedExe.parent_path() / "topo-build-does-not-exist";

    auto missingExe = topoBuild("checker_completeness_pass");
    EXPECT_FALSE(missingExe.spawned)
        << "a missing topo-build executable must be reported as a spawn "
           "failure, not as a legitimate exit code";
    EXPECT_EQ(missingExe.exitCode, -1);

    // Missing project dir → spawn fails on the working-directory change.
    topoBuildExe_ = savedExe;
    auto missingDir = topoBuild("no_such_fixture_project");
    EXPECT_FALSE(missingDir.spawned)
        << "a missing project directory must be reported as a spawn failure";

    projectsDir_ = savedProjects;
}

// --- B3A: OperatorPipeline ---

TEST_F(Functional, OperatorPipeline) {
    auto build = topoBuild("operator_pipeline");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("operator_pipeline", "operator_pipeline");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "operator_pipeline: result=37\n"
                        "operator_pipeline: all assertions passed\n"
                        "...");
}

// --- B3A: CrossModuleAdapter ---

TEST_F(Functional, CrossModuleAdapter) {
    auto build = topoBuild("cross_module_adapter");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("cross_module_adapter", "cross_module_adapter");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "cross_module_adapter: result=56\n"
                        "cross_module_adapter: all assertions passed\n"
                        "...");
}

// --- B3A: VariadicTemplate ---

TEST_F(Functional, VariadicTemplate) {
    auto build = topoBuild("variadic_template");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("variadic_template", "variadic_template");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "variadic_template: count={{NUM}} result={{NUM}}\n"
                        "variadic_template: all assertions passed\n"
                        "...");
}

// --- B3A: VisibilityAdapter ---

TEST_F(Functional, VisibilityAdapter) {
    auto build = topoBuild("visibility_adapter");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("visibility_adapter", "visibility_adapter");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "visibility_adapter: result={{NUM}} decoded={{NUM}}\n"
                        "visibility_adapter: all assertions passed\n"
                        "...");
}

// --- B3A: Priority ---

TEST_F(Functional, Priority) {
    auto build = topoBuild("priority");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("priority", "priority");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "priority_e2e: all assertions passed\n"
                        "...");
}

// --- B3A: MixedLang (C++/Rust) ---

TEST_F(Functional, MixedLang) {
    auto build = topoBuild("mixed_lang");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto run = runBinary("mixed_lang", "mixed_lang");
    ASSERT_EQ(run.exitCode, 0) << "Binary failed:\n" << run.output;

    assertOutputMatches(run.output,
                        "mixed_lang: compute(10, 20) = {{NUM}}\n"
                        "mixed_lang: all assertions passed\n"
                        "...");
}

} // namespace topo::test::e2e
