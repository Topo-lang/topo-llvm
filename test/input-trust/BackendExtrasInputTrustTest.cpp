// topo-build-llvm-{cpp,rust,mixed} per-value backendExtras validation tests.
//
// Spawns each LLVM backend binary with hand-crafted BackendRequest JSON to
// assert that wrong-typed backendExtras values are rejected before any
// compile step runs. Diagnostic shape pinned:
//   `error: backendExtras.<key>: expected <type>, got <actual>`
// Mirrors the topo-jvm `BackendExtrasInputTrustTest.cpp` pattern: every
// backend tool must reject malformed backendExtras uniformly rather than
// silently coercing or crashing in a downstream compile step.

#include "topo/Platform/Process.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

#ifdef _WIN32
int testPid() { return _getpid(); }
#else
int testPid() { return getpid(); }
#endif

class LlvmBackendExtrasInputTrust : public ::testing::Test {
protected:
    fs::path testDir;

    void SetUp() override {
        testDir = fs::temp_directory_path() /
                  ("topo-llvm-extras-trust_" + std::to_string(testPid()) + "_" +
                   std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(testDir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(testDir, ec);
    }

    /// Build a minimal-but-deserializable BackendRequest with the given
    /// `language` label and `backendExtras` payload.
    json makeRequest(const char* language, const json& backendExtras) const {
        json j = json::object();
        j["outputPath"] = (testDir / "out.bin").string();
        j["tempDir"] = (testDir / "tmp").string();
        j["language"] = language;
        j["config"] = json::object();
        j["topoMetadata"] = json::object();
        j["visibilityEntries"] = json::array();
        j["backendExtras"] = backendExtras;
        return j;
    }

    topo::platform::CapturedProcessResult invoke(const std::string& exe,
                                                 const json& req) const {
        fs::path reqPath = testDir / "request.json";
        std::ofstream(reqPath) << req.dump();
        return topo::platform::runProcessCapture(exe, {reqPath.string()}, false);
    }
};

} // namespace

// --- topo-build-llvm-cpp ----------------------------------------

TEST_F(LlvmBackendExtrasInputTrust, CppHostCompilerPathMustBeString) {
    json extras = json::object();
    extras["hostCompilerPath"] = 42;
    auto result = invoke(TOPO_BUILD_LLVM_CPP_EXE, makeRequest("cpp", extras));

    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.stderrOutput.find("backendExtras.hostCompilerPath"),
              std::string::npos)
        << "expected diagnostic mentioning 'backendExtras.hostCompilerPath'; "
        << "stderr was:\n" << result.stderrOutput;
    EXPECT_NE(result.stderrOutput.find("expected string"), std::string::npos)
        << "expected 'expected string' phrase; stderr was:\n"
        << result.stderrOutput;
}

TEST_F(LlvmBackendExtrasInputTrust, CppStandardMustBeString) {
    json extras = json::object();
    extras["standard"] = 17;
    auto result = invoke(TOPO_BUILD_LLVM_CPP_EXE, makeRequest("cpp", extras));

    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.stderrOutput.find("backendExtras.standard"),
              std::string::npos)
        << "expected diagnostic mentioning 'backendExtras.standard'; "
        << "stderr was:\n" << result.stderrOutput;
}

// --- topo-build-llvm-rust ---------------------------------------

TEST_F(LlvmBackendExtrasInputTrust, RustCargoPathMustBeString) {
    json extras = json::object();
    extras["cargoPath"] = json::array({"cargo"}); // arrays are not strings
    auto result = invoke(TOPO_BUILD_LLVM_RUST_EXE, makeRequest("rust", extras));

    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.stderrOutput.find("backendExtras.cargoPath"),
              std::string::npos)
        << "expected diagnostic mentioning 'backendExtras.cargoPath'; "
        << "stderr was:\n" << result.stderrOutput;
}

TEST_F(LlvmBackendExtrasInputTrust, RustHostCompilerPathMustBeString) {
    json extras = json::object();
    extras["hostCompilerPath"] = true;
    auto result = invoke(TOPO_BUILD_LLVM_RUST_EXE, makeRequest("rust", extras));

    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.stderrOutput.find("backendExtras.hostCompilerPath"),
              std::string::npos)
        << "expected diagnostic mentioning 'backendExtras.hostCompilerPath'; "
        << "stderr was:\n" << result.stderrOutput;
}

// --- topo-build-llvm-mixed --------------------------------------

TEST_F(LlvmBackendExtrasInputTrust, MixedMixedConfigMustBeObject) {
    json extras = json::object();
    extras["mixedConfig"] = "not-an-object";
    auto result = invoke(TOPO_BUILD_LLVM_MIXED_EXE, makeRequest("mixed", extras));

    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.stderrOutput.find("backendExtras.mixedConfig"),
              std::string::npos)
        << "expected diagnostic mentioning 'backendExtras.mixedConfig'; "
        << "stderr was:\n" << result.stderrOutput;
    EXPECT_NE(result.stderrOutput.find("expected object"), std::string::npos)
        << "expected 'expected object' phrase; stderr was:\n"
        << result.stderrOutput;
}

TEST_F(LlvmBackendExtrasInputTrust, MixedCppSourcesEntriesMustBeStrings) {
    json mixedCfg = json::object();
    mixedCfg["cppSources"] = json::array({"ok.cpp", 99}); // second elem is int
    json extras = json::object();
    extras["mixedConfig"] = mixedCfg;
    auto result = invoke(TOPO_BUILD_LLVM_MIXED_EXE, makeRequest("mixed", extras));

    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.stderrOutput.find("mixedConfig.cppSources"),
              std::string::npos)
        << "expected diagnostic mentioning 'mixedConfig.cppSources'; "
        << "stderr was:\n" << result.stderrOutput;
}

TEST_F(LlvmBackendExtrasInputTrust, MixedRustManifestMustBeString) {
    json mixedCfg = json::object();
    mixedCfg["rustManifest"] = 12345;
    json extras = json::object();
    extras["mixedConfig"] = mixedCfg;
    auto result = invoke(TOPO_BUILD_LLVM_MIXED_EXE, makeRequest("mixed", extras));

    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.stderrOutput.find("mixedConfig.rustManifest"),
              std::string::npos)
        << "expected diagnostic mentioning 'mixedConfig.rustManifest'; "
        << "stderr was:\n" << result.stderrOutput;
}
