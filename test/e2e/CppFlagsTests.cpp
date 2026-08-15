#include "E2eHarness.h"

#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"

#include <sstream>
#include <string>
#include <vector>

// [build.cpp].flags acceptance lane (plain cpp).
//
// Topo.toml's [build.cpp].flags is the documented free-form clang++
// passthrough; it must reach BOTH clang invocations of a plain-cpp build —
// the compile-to-IR step and the link step. The cpp_flags fixture refuses to
// compile without -DTOPO_E2E_FLAGS_PROOF (#ifndef/#error), so a successful
// build is the compile proof; the link proof reads the --verbose command echo
// and asserts the token on the logged link line (the invocation naming the
// output binary). Asserting only token-on-line keeps the check stable across
// platform-specific link arguments (-isysroot on macOS, -Wl,… on Windows).

namespace topo::test::e2e {

namespace {

constexpr const char* kFlagToken = "-DTOPO_E2E_FLAGS_PROOF";
constexpr const char* kOutputName = "flags_probe";

// Recursively copy the fixture into a private working copy (fresh per test,
// so the two legs never collide on cache state under `ctest -j N`).
void copyFixtureTree(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst.parent_path(), ec);
    fs::copy(src, dst, fs::copy_options::recursive, ec);
}

// True when some logged command line carries the flag token AND names the
// output binary — i.e. the LINK invocation got the flag. Compile lines name
// only source / IR files, and the work dirs below avoid the output name as a
// path substring, so the output name discriminates the link line.
bool linkLineHasFlag(const std::string& log) {
    std::istringstream ss(log);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find(kFlagToken) == std::string::npos) continue;
        if (line.find(kOutputName) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// Derives from E2eFixture purely to inherit SetUp()'s backend-tool PATH
// injection (so the imported topo-build resolves topo-build-llvm-cpp) and the
// resolved topoBuildExe_ — same rationale as TsanBackstop.
class CppFlags : public E2eFixture {
protected:
    fs::path fixtureSrcDir() const {
#ifdef TOPO_E2E_CPP_FLAGS_FIXTURE_DIR
        return fs::path(TOPO_E2E_CPP_FLAGS_FIXTURE_DIR);
#else
        return {};
#endif
    }

    RunResult buildFixture(const fs::path& workDir, bool verbose, fs::path& outBin) {
        std::vector<std::string> args;
        if (verbose) args.push_back("--verbose");
        auto r = platform::runProcessCapture(topoBuildExe_.generic_string(), args,
                                             workDir.generic_string());
        outBin = workDir / (std::string(kOutputName) + std::string(platform::ExeSuffix));
        return RunResult{r.exitCode, r.stdoutOutput + r.stderrOutput};
    }
};

// Compile proof: the fixture #errors without the flag, so a successful build
// plus a zero-exit run mean [build.cpp].flags reached the compile step.
TEST_F(CppFlags, FlagReachesCompile) {
    fs::path src = fixtureSrcDir();
    ASSERT_FALSE(src.empty()) << "TOPO_E2E_CPP_FLAGS_FIXTURE_DIR not defined";
    ASSERT_TRUE(fs::exists(src)) << "fixture dir missing: " << src;

    // Work-dir name deliberately avoids kOutputName (see linkLineHasFlag).
    fs::path workDir = fs::temp_directory_path() / "topo_e2e_flagsproof_compile";
    copyFixtureTree(src, workDir);

    fs::path bin;
    auto build = buildFixture(workDir, /*verbose=*/false, bin);
    ASSERT_EQ(build.exitCode, 0)
        << "topo-build failed — [build.cpp].flags dropped before the compile "
           "step?\n" << build.output;
    ASSERT_TRUE(fs::exists(bin)) << "binary not produced: " << bin;

    auto run = platform::runProcessCapture(bin.generic_string(), {});
    EXPECT_EQ(run.exitCode, 0)
        << "fixture binary failed\nstdout:\n" << run.stdoutOutput
        << "\nstderr:\n" << run.stderrOutput;
}

// Link proof: --verbose echoes every spawned command line; the flag token
// must appear on the link invocation (the line naming the output binary).
TEST_F(CppFlags, FlagReachesLink) {
    fs::path src = fixtureSrcDir();
    ASSERT_FALSE(src.empty()) << "TOPO_E2E_CPP_FLAGS_FIXTURE_DIR not defined";
    ASSERT_TRUE(fs::exists(src)) << "fixture dir missing: " << src;

    fs::path workDir = fs::temp_directory_path() / "topo_e2e_flagsproof_link";
    copyFixtureTree(src, workDir);

    fs::path bin;
    auto build = buildFixture(workDir, /*verbose=*/true, bin);
    ASSERT_EQ(build.exitCode, 0) << "topo-build --verbose failed:\n" << build.output;

    EXPECT_TRUE(linkLineHasFlag(build.output))
        << "no logged command names '" << kOutputName << "' and carries "
        << kFlagToken << " — flags reached compile but not link.\nbuild log:\n"
        << build.output;
}

} // namespace topo::test::e2e
