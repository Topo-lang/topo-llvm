#include "E2eHarness.h"

#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

// ThreadSanitizer backstop lane.
//
// Topo's parallel optimization license rests on CHECKED declarations. Check now
// runs on every build by default; the one remaining hole is a user who
// explicitly opts out (check = "off" / --no-check) while declaring
// [parallel] mode = "force" over host code that is NOT actually parallel-safe.
// In that case TopoParallelPass spawns genuinely-conflicting work concurrently
// with no static diagnostic anywhere. This suite is the chosen dynamic
// backstop: the known-wrong `tsan_race` fixture must TRIP ThreadSanitizer,
// proving the runtime detector catches what the checker was told not to look
// at — and the serial inverse leg must run TSan-clean, proving the race comes
// from the parallel transform trusting the wrong declaration, not from the
// fixture itself.
//
// TSan is injected into BOTH the compile-to-IR and the link clang invocations
// via CCC_OVERRIDE_OPTIONS (a clang-driver knob honoured by every clang call in
// the build), because a plain-cpp Topo.toml has no clang-flags passthrough:
// the [build.cpp] flags array is parsed only for [build].language = "mixed"
// (topo-cli Config.cpp), so a flags entry on this cpp project would be silently
// dropped at both steps. -g is already supplied by the driver's default dev
// build mode, so the report carries source locations.
//
// Linux + macOS only: clang ships no ThreadSanitizer runtime for the
// Windows/MinGW target, so -fsanitize=thread would fail to compile/link there.
// The Windows leg skips with that reason. macOS arm64 (brew llvm@22) is
// included — the race was verified to trip 80/80 there during bring-up.

namespace topo::test::e2e {

// These helpers are referenced only by the non-Windows test bodies (TSan is
// unavailable in the Windows/MinGW toolchain — see the per-test skip), so guard
// them to keep the Windows compile free of unused-symbol diagnostics.
#ifndef _WIN32
namespace {

constexpr const char* kRaceNeedle = "WARNING: ThreadSanitizer: data race";

// Recursively copy the source fixture tree into `dst` (removing any prior
// copy). Each test gets a fresh private working copy so the forced + serial
// legs never collide on Topo.toml / .topo-cache state under `ctest -j N`.
void copyFixtureTree(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst.parent_path(), ec);
    fs::copy(src, dst, fs::copy_options::recursive, ec);
}

bool fileContains(const fs::path& path, const std::string& needle) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str().find(needle) != std::string::npos;
}

} // namespace
#endif // !_WIN32

// Derives from E2eFixture purely to inherit SetUp()'s backend-tool PATH
// injection (so the imported topo-build resolves topo-build-llvm-cpp) and the
// resolved `topoBuildExe_`. The benchmark/fixture project state is irrelevant
// here — every build runs in a private temp copy.
class TsanBackstop : public E2eFixture {
protected:
    fs::path fixtureSrcDir() const {
#ifdef TOPO_E2E_TSAN_FIXTURE_DIR
        return fs::path(TOPO_E2E_TSAN_FIXTURE_DIR);
#else
        return {};
#endif
    }

#ifndef _WIN32
    // Build the fixture copy at `workDir` with topo-build, injecting
    // -fsanitize=thread into every clang invocation. `tomlVariant` (when
    // non-empty) is copied over Topo.toml before the build (the serial leg uses
    // "Topo-base.toml"). Returns the merged stdout+stderr build log; `outBin`
    // and `outIR` receive the produced binary + dumped-IR paths.
    RunResult buildWithTsan(const fs::path& workDir, const std::string& tomlVariant,
                            const std::string& outputName, fs::path& outBin, fs::path& outIR) {
        if (!tomlVariant.empty()) {
            std::error_code ec;
            fs::copy_file(workDir / tomlVariant, workDir / "Topo.toml",
                          fs::copy_options::overwrite_existing, ec);
            // Drop any cache produced by a prior leg so the toml swap takes.
            fs::remove_all(workDir / ".topo-cache", ec);
        }

        // CCC_OVERRIDE_OPTIONS edits every clang command line the build spawns.
        // Leading '#' silences clang's "### …" override notice; '+' appends the
        // flag — so the compile-to-IR step AND the link step both get
        // -fsanitize=thread (TSan needs it at both).
        const char* savedCcc = std::getenv("CCC_OVERRIDE_OPTIONS");
        std::string savedCccStr = savedCcc ? savedCcc : "";
        setenv("CCC_OVERRIDE_OPTIONS", "# +-fsanitize=thread", 1);

        // --dump-ir → <output>.ll (parallelization witness). --no-check is the
        // explicit opt-out this backstop exists for; Topo.toml also sets
        // check = "off", so the declaration enters the build UNVERIFIED.
        auto r = platform::runProcessCapture(topoBuildExe_.generic_string(),
                                             {"--dump-ir", "--no-check"},
                                             workDir.generic_string());

        if (savedCcc) {
            setenv("CCC_OVERRIDE_OPTIONS", savedCccStr.c_str(), 1);
        } else {
            unsetenv("CCC_OVERRIDE_OPTIONS");
        }

        outBin = workDir / (outputName + std::string(platform::ExeSuffix));
        outIR = workDir / (outputName + ".ll");
        return RunResult{r.exitCode, r.stdoutOutput + r.stderrOutput};
    }

    // Run `bin` under TSan with deterministic first-error halt. `halt_on_error`
    // makes TSan stop + report on the first race (and is non-zero exit on every
    // platform: macOS aborts → 134, Linux exits 66 by default), so the assert
    // keys on "non-zero + race report", not a fixed code.
    platform::CapturedProcessResult runUnderTsan(const fs::path& bin) {
        const char* savedTsan = std::getenv("TSAN_OPTIONS");
        std::string savedTsanStr = savedTsan ? savedTsan : "";
        setenv("TSAN_OPTIONS", "halt_on_error=1", 1);

        // 180s deadline: the racy build aborts within ~1-2s; a clean serial run
        // finishes in well under a second. The cap only guards a hang.
        auto r = platform::runProcessCaptureWithTimeout(bin.generic_string(), {}, 180000);

        if (savedTsan) {
            setenv("TSAN_OPTIONS", savedTsanStr.c_str(), 1);
        } else {
            unsetenv("TSAN_OPTIONS");
        }
        return r;
    }
#endif // !_WIN32
};

// Forced-parallel build over the wrong declaration must spawn the racy work
// concurrently (TopoParallelPass fired → topo_task_spawn in the IR) and TSan
// must report the data race with a non-zero exit.
TEST_F(TsanBackstop, ForcedParallelTripsTSan) {
#ifdef _WIN32
    GTEST_SKIP() << "ThreadSanitizer has no runtime in the Windows/MinGW clang "
                    "toolchain; -fsanitize=thread cannot build there.";
#else
    fs::path src = fixtureSrcDir();
    ASSERT_FALSE(src.empty()) << "TOPO_E2E_TSAN_FIXTURE_DIR not defined";
    ASSERT_TRUE(fs::exists(src)) << "fixture dir missing: " << src;

    fs::path workDir = fs::temp_directory_path() / "topo_tsan_forced";
    copyFixtureTree(src, workDir);

    fs::path bin, ir;
    auto build = buildWithTsan(workDir, /*tomlVariant=*/"", "tsan_race", bin, ir);
    ASSERT_EQ(build.exitCode, 0) << "topo-build (forced) failed:\n" << build.output;
    ASSERT_TRUE(fs::exists(bin)) << "forced binary not produced: " << bin;

    // Parallelization witness: a vacuous fixture that never parallelized would
    // 'pass' by never racing. The dumped IR must show the spawn rewrite.
    ASSERT_TRUE(fs::exists(ir)) << "dumped IR missing: " << ir;
    EXPECT_TRUE(fileContains(ir, "topo_task_spawn_ret"))
        << "TopoParallelPass did not parallelize the forced build — the race "
           "guard would be vacuous. IR: " << ir;
    EXPECT_TRUE(fileContains(ir, "!topo.fired.TopoParallelPass"))
        << "TopoParallelPass fired-marker absent in " << ir;

    auto run = runUnderTsan(bin);
    EXPECT_NE(run.exitCode, 0)
        << "Forced-parallel racy build exited 0 under TSan — the dynamic "
           "backstop did not trip.\nstdout:\n" << run.stdoutOutput
        << "\nstderr:\n" << run.stderrOutput;
    EXPECT_NE(run.stderrOutput.find(kRaceNeedle), std::string::npos)
        << "TSan emitted no data-race report.\nstdout:\n" << run.stdoutOutput
        << "\nstderr:\n" << run.stderrOutput;
#endif
}

// Inverse guard: the SAME sources + the same (wrong) declaration, built with
// [parallel] mode = "off", stay sequential on one thread and run TSan-clean
// with a zero exit. Proves the race is created by the parallel transform
// trusting the declaration, not by the fixture being inherently racy.
TEST_F(TsanBackstop, SerialBuildRunsClean) {
#ifdef _WIN32
    GTEST_SKIP() << "ThreadSanitizer has no runtime in the Windows/MinGW clang "
                    "toolchain; -fsanitize=thread cannot build there.";
#else
    fs::path src = fixtureSrcDir();
    ASSERT_FALSE(src.empty()) << "TOPO_E2E_TSAN_FIXTURE_DIR not defined";
    ASSERT_TRUE(fs::exists(src)) << "fixture dir missing: " << src;

    fs::path workDir = fs::temp_directory_path() / "topo_tsan_serial";
    copyFixtureTree(src, workDir);

    fs::path bin, ir;
    auto build = buildWithTsan(workDir, /*tomlVariant=*/"Topo-base.toml", "tsan_race_base", bin, ir);
    ASSERT_EQ(build.exitCode, 0) << "topo-build (serial) failed:\n" << build.output;
    ASSERT_TRUE(fs::exists(bin)) << "serial binary not produced: " << bin;

    // mode = "off" → TopoParallelPass must NOT have rewritten any call to the
    // spawn runtime; otherwise the "clean" result would not prove the inverse.
    ASSERT_TRUE(fs::exists(ir)) << "dumped IR missing: " << ir;
    EXPECT_FALSE(fileContains(ir, "topo_task_spawn_ret"))
        << "Serial (mode=off) build unexpectedly parallelized — inverse guard "
           "invalid. IR: " << ir;

    auto run = runUnderTsan(bin);
    EXPECT_EQ(run.exitCode, 0)
        << "Serial build did not run clean under TSan.\nstdout:\n"
        << run.stdoutOutput << "\nstderr:\n" << run.stderrOutput;
    EXPECT_EQ(run.stderrOutput.find(kRaceNeedle), std::string::npos)
        << "Serial build reported a data race — the fixture is racy "
           "independent of the parallel transform.\nstderr:\n" << run.stderrOutput;
#endif
}

} // namespace topo::test::e2e
