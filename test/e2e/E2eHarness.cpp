#include "E2eHarness.h"

#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>

namespace topo::test::e2e {

// ============================================================================
// SetUp
// ============================================================================

void E2eFixture::SetUp() {
    // Resolve benchmark seed once per process so that every benchmark case
    // inherits a stable derivation source. `benchSeed()` prints the one-line
    // `[ SEED   ]` banner on first call.
    (void)benchSeed();

    const char* benchDir = TOPO_BENCHMARKS_DIR;
    const char* fixDir = TOPO_E2E_FIXTURES_DIR;
    const char* buildExe = TOPO_BUILD_EXE;
    const char* llvmDir = TOPO_LLVM_BINDIR;

    ASSERT_NE(benchDir, nullptr) << "TOPO_BENCHMARKS_DIR not defined";
    ASSERT_NE(fixDir, nullptr) << "TOPO_E2E_FIXTURES_DIR not defined";
    ASSERT_NE(buildExe, nullptr) << "TOPO_BUILD_EXE not defined";
    ASSERT_NE(llvmDir, nullptr) << "TOPO_LLVM_BINDIR not defined";

    benchmarksDir_ = fs::path(benchDir);
    fixturesDir_ = fs::path(fixDir);
    projectsDir_ = benchmarksDir_; // backward compat alias
    topoBuildExe_ = fs::path(buildExe);
    llvmBinDir_ = fs::path(llvmDir);

    ASSERT_TRUE(fs::exists(benchmarksDir_)) << "Benchmarks dir not found: " << benchmarksDir_;
    ASSERT_TRUE(fs::exists(fixturesDir_)) << "Fixtures dir not found: " << fixturesDir_;
    ASSERT_TRUE(fs::exists(topoBuildExe_)) << "topo-build not found: " << topoBuildExe_;

    // In standalone topo-llvm, the topo-build CLI (from topo-cli's install
    // prefix) lives in a different dir than topo-build-llvm-{cpp,rust,mixed}
    // (built in this repo). topo-build resolves backend tools via:
    //   (1) sibling lookup next to argv[0]
    //   (2) PATH search
    // Inject the backend-tool build dirs into PATH so the subprocess find
    // succeeds. TOPO_BUILD_LLVM_TOOLS_DIRS is a delimited list set at
    // compile time by the e2e CMakeLists via $<TARGET_FILE_DIR:...>.
#ifdef TOPO_BUILD_LLVM_TOOLS_DIRS
    {
        const std::string toolsDirs = TOPO_BUILD_LLVM_TOOLS_DIRS;
        const char* oldPath = std::getenv("PATH");
        std::string newPath = toolsDirs;
        if (oldPath && *oldPath) {
#ifdef _WIN32
            newPath += ";";
#else
            newPath += ":";
#endif
            newPath += oldPath;
        }
#ifdef _WIN32
        _putenv_s("PATH", newPath.c_str());
#else
        setenv("PATH", newPath.c_str(), 1);
#endif
    }
#endif

    // JVM benchmarks directory (optional — only needed for Java E2E tests)
#ifdef TOPO_JVM_BENCHMARKS_DIR
    jvmBenchmarksDir_ = fs::path(TOPO_JVM_BENCHMARKS_DIR);
#endif
}

// ============================================================================
// topoBuild
// ============================================================================

RunResult E2eFixture::topoBuild(const std::string& projectName,
                                const std::string& expectedOutput) {
    fs::path projDir = projectsDir_ / projectName;
    std::string exe = topoBuildExe_.generic_string();
    std::string workDir = projDir.generic_string();

    // Fast path: if the caller told us what the expected
    // output binary is AND it already exists, skip the topo-build invocation
    // entirely. The `topo-bench-artifacts` CTest setup fixture pre-builds
    // every benchmark variant so the measurement phase never re-enters
    // topo-build and therefore doesn't mutate shared project state
    // (.topo-cache, Topo.toml swaps). Callers that cannot predict the output
    // (equivalence / functional suites) pass an empty string and fall
    // through to the traditional inline build path.
    if (!expectedOutput.empty()) {
        fs::path bin = binaryPath(projectName, expectedOutput);
        if (fs::exists(bin)) {
            return RunResult{0, "[prebuilt] " + bin.generic_string()};
        }
    }

    // Always --dump-ir: produces <output>.ll alongside the binary, which the
    // harness parses for `!topo.fired.<PassName>` markers to distinguish
    // "pass fired" from "pass was trivially skipped". Cost is marginal (one
    // textual .ll write per build); E2E builds already write object files
    // and link binaries.
    auto r = platform::runProcessCapture(exe, {"--dump-ir"}, workDir);
    // Merge stderr into the result: topo-build prints the ninja frontend
    // progress to stdout, but the BACKEND tool's diagnostics (`error: backend
    // tool '…' failed`, `error: linking failed`, a missing -L/-l, a crash) go
    // to STDERR. The build asserts (EquivalenceTests / FunctionalTests) print
    // RunResult.output on failure, so dropping stderr left every standalone
    // build failure showing only "[1/2] CC …" with no cause. A build's output
    // is never compared (only runBinary's stdout is), so merging is safe here.
    return RunResult{r.exitCode, r.stdoutOutput + r.stderrOutput};
}

// ============================================================================
// topoBaseBuild
// ============================================================================

RunResult E2eFixture::topoBaseBuild(const std::string& projectName,
                                    const std::string& expectedOutput) {
    fs::path projDir = projectsDir_ / projectName;

    // Fast path first — avoid the toml swap + .topo-cache clean when we
    // already have the pre-built base binary.
    if (!expectedOutput.empty()) {
        fs::path bin = binaryPath(projectName, expectedOutput);
        if (fs::exists(bin)) {
            return RunResult{0, "[prebuilt] " + bin.generic_string()};
        }
    }

    fs::path topoToml = projDir / "Topo.toml";
    fs::path baseToml = projDir / "Topo-base.toml";
    fs::path saved = projDir / "Topo.toml.saved";

    if (!fs::exists(baseToml)) {
        return RunResult{-1, "Topo-base.toml not found in " + projDir.generic_string()};
    }

    // Swap Topo.toml → Topo-base.toml
    fs::copy_file(topoToml, saved, fs::copy_options::overwrite_existing);
    fs::copy_file(baseToml, topoToml, fs::copy_options::overwrite_existing);

    // Clean incremental cache
    {
        std::error_code ec;
        fs::remove_all(projDir / ".topo-cache", ec);
    }

    auto result = topoBuild(projectName);

    // Restore original
    fs::copy_file(saved, topoToml, fs::copy_options::overwrite_existing);
    fs::remove(saved);

    return result;
}

// ============================================================================
// topoForcedBuild
// ============================================================================

RunResult E2eFixture::topoForcedBuild(const std::string& projectName,
                                      const std::string& expectedOutput) {
    fs::path projDir = projectsDir_ / projectName;

    // Fast path — see topoBaseBuild.
    if (!expectedOutput.empty()) {
        fs::path bin = binaryPath(projectName, expectedOutput);
        if (fs::exists(bin)) {
            return RunResult{0, "[prebuilt] " + bin.generic_string()};
        }
    }

    fs::path topoToml = projDir / "Topo.toml";
    fs::path forcedToml = projDir / "Topo-forced.toml";
    fs::path saved = projDir / "Topo.toml.saved";

    if (!fs::exists(forcedToml)) {
        return RunResult{-1, "Topo-forced.toml not found in " + projDir.generic_string()};
    }

    // Swap Topo.toml → Topo-forced.toml
    fs::copy_file(topoToml, saved, fs::copy_options::overwrite_existing);
    fs::copy_file(forcedToml, topoToml, fs::copy_options::overwrite_existing);

    // Clean incremental cache
    {
        std::error_code ec;
        fs::remove_all(projDir / ".topo-cache", ec);
    }

    auto result = topoBuild(projectName);

    // Restore original
    fs::copy_file(saved, topoToml, fs::copy_options::overwrite_existing);
    fs::remove(saved);

    return result;
}

// ============================================================================
// topoBaselineBuild (deprecated alias)
// ============================================================================

RunResult E2eFixture::topoBaselineBuild(const std::string& projectName) {
    return topoBaseBuild(projectName);
}

// ============================================================================
// vanillaBuild
// ============================================================================

RunResult E2eFixture::vanillaBuild(const std::string& projectName) {
    auto config = parseTopoToml(projectName);
    fs::path projDir = projectsDir_ / projectName;

    // Fast path: vanilla artefact is always `build/baseline`
    // (platform-adjusted via binaryPath). Skip the clang++ invocation when
    // the binary is already present — it matches the layout produced by
    // TopoBenchArtifactsDriver.cmake's _topo_vanilla_cpp step.
    {
        fs::path bin = binaryPath(projectName, "baseline");
        if (fs::exists(bin)) {
            return RunResult{0, "[prebuilt] " + bin.generic_string()};
        }
    }

    // Resolve clang++
    std::string clangxx = (llvmBinDir_ / ("clang++" + std::string(platform::ExeSuffix))).generic_string();

    // Build args: clang++ -O2 -std=<standard> -I<include>... <sources> -o <output>
    std::vector<std::string> args;
    args.push_back("-O2");
    args.push_back("-std=" + config.standard);

    // macOS: bundled clang++ needs explicit SDK path
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

    for (const auto& inc : config.include) {
        fs::path incPath = inc;
        if (incPath.is_relative()) {
            incPath = projDir / incPath;
        }
        args.push_back("-I" + incPath.generic_string());
    }

    // Expand source globs
    for (const auto& srcPattern : config.sources) {
        // Simple glob: if pattern contains *, find matching files
        if (srcPattern.find('*') != std::string::npos) {
            fs::path srcDir = projDir / fs::path(srcPattern).parent_path();
            std::string ext = fs::path(srcPattern).extension().string();
            if (fs::exists(srcDir)) {
                for (const auto& entry : fs::directory_iterator(srcDir)) {
                    if (entry.is_regular_file() && entry.path().extension() == ext) {
                        args.push_back(entry.path().generic_string());
                    }
                }
            }
        } else {
            args.push_back((projDir / srcPattern).generic_string());
        }
    }

    // Resolve `[build].link_libs` and link them, mirroring how
    // `topo::build::injectAutoLinkLibs` (topo-core/include/topo/Build/
    // AutoLink.h) wires the topo binary's link line. `TomlConfig` (declared
    // in E2eHarness.h, owned elsewhere) has no link_libs field, so re-scan
    // the toml here for the single `link_libs = [ ... ]` array — benchmark
    // tomls have a stable single-line shape (same assumption the CMake
    // driver's regex mirror makes in TopoBenchArtifactsDriver.cmake).
    //
    // Without this, any benchmark with non-empty link_libs (currently
    // benchmarks/pipeline → ["topo-parallel"]) fails the vanilla link with
    // undefined `topo::parallel::*` symbols on any fresh worktree lacking a
    // prebuilt baseline, masking the rest of the equivalence suite.
    std::vector<std::string> linkLibs;
    {
        fs::path tomlPath = projDir / "Topo.toml";
        std::ifstream tf(tomlPath);
        std::string tline;
        std::string tsection;
        while (std::getline(tf, tline)) {
            if (!tline.empty() && tline.back() == '\r') tline.pop_back();
            size_t s = tline.find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            tline = tline.substr(s);
            if (tline[0] == '#') continue;
            if (tline[0] == '[') {
                auto e = tline.find(']');
                if (e != std::string::npos) tsection = tline.substr(1, e - 1);
                continue;
            }
            auto eq = tline.find('=');
            if (eq == std::string::npos) continue;
            std::string k = tline.substr(0, eq);
            std::string v = tline.substr(eq + 1);
            auto trim = [](std::string& str) {
                size_t a = str.find_first_not_of(" \t");
                size_t b = str.find_last_not_of(" \t");
                if (a == std::string::npos) { str.clear(); return; }
                str = str.substr(a, b - a + 1);
            };
            trim(k);
            trim(v);
            if (tsection == "build" && k == "link_libs") {
                auto lb = v.find('[');
                auto rb = v.find(']');
                if (lb != std::string::npos && rb != std::string::npos) {
                    std::string inner = v.substr(lb + 1, rb - lb - 1);
                    std::istringstream ss(inner);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        size_t a = item.find_first_not_of(" \t\"");
                        size_t b = item.find_last_not_of(" \t\"");
                        if (a != std::string::npos)
                            linkLibs.push_back(item.substr(a, b - a + 1));
                    }
                }
            }
        }
    }

    if (!linkLibs.empty()) {
        // Mirror injectAutoLinkLibs's transitive closure: topo-parallel /
        // topo-adaptive / topo-arena each pull libtopo-pass-event (the
        // archives reference topo_pass_event_emit*). Static-archive order
        // requires the callee (topo-pass-event) to follow its caller, so
        // append after the explicit libs (dedup keeps a single copy).
        bool needsPassEvent = false;
        for (const auto& l : linkLibs) {
            if (l == "topo-parallel" || l == "topo-adaptive" || l == "topo-arena") {
                needsPassEvent = true;
                break;
            }
        }
        if (needsPassEvent &&
            std::find(linkLibs.begin(), linkLibs.end(), "topo-pass-event") ==
                linkLibs.end()) {
            linkLibs.push_back("topo-pass-event");
        }

        // Resolve the runtime lib search dir. The topo backend tools point
        // -L at `$<TARGET_FILE_DIR:topo-parallel>` via the
        // TOPO_BUILD_LIBDIR_SDK define; the topo runtime static archives
        // (libtopo-parallel.a, libtopo-pass-event.a, ...) live in
        // `<CMAKE_BINARY_DIR>/topo-llvm/runtime`. The harness has no define
        // for that dir, so derive it by walking up from the known
        // topo-build executable (`<build>/topo-cli/tools/topo-build/...`)
        // until a `topo-llvm/runtime` dir holding the parallel archive is
        // found — robust to relative-depth changes across layouts/worktrees.
        // Static-archive filename: `libtopo-parallel.a` (Unix) /
        // `topo-parallel.lib` (MSVC, no `lib` prefix).
        const std::string parallelArchive =
            std::string(platform::IsWindows ? "" : "lib") + "topo-parallel" +
            std::string(platform::StaticLibSuffix);
        fs::path runtimeDir;
        // Primary: the runtime-archive dir baked in at compile time as
        // $<TARGET_FILE_DIR:topo-parallel>. The ancestor-walk below only finds
        // the archive when `topoBuildExe_` lives inside the build tree (the
        // unified meta build). In standalone topo-llvm CI `topoBuildExe_` is
        // the INSTALLED topo-build (topo-cli's prefix), so the walk never
        // reaches `<build>/topo-llvm/runtime` and `-ltopo-parallel` failed to
        // resolve — masking the whole `pipeline` benchmark (its only
        // link_libs consumer) as "Vanilla build failed". The define is
        // layout-independent and works in both.
#ifdef TOPO_LLVM_RUNTIME_DIR
        {
            fs::path cand = TOPO_LLVM_RUNTIME_DIR;
            if (fs::exists(cand / parallelArchive)) runtimeDir = cand;
        }
#endif
        if (runtimeDir.empty()) {
            for (fs::path anc = topoBuildExe_.parent_path();
                 !anc.empty() && anc != anc.root_path(); anc = anc.parent_path()) {
                fs::path cand = anc / "topo-llvm" / "runtime";
                if (fs::exists(cand / parallelArchive)) {
                    runtimeDir = cand;
                    break;
                }
            }
        }
        if (!runtimeDir.empty()) {
            args.push_back("-L" + runtimeDir.generic_string());
        }
        for (const auto& lib : linkLibs) {
            args.push_back("-l" + lib);
        }
    }

    // Output: build in project's build/ directory
    fs::path buildDir = projDir / "build";
    fs::create_directories(buildDir);
    std::string outputName = "baseline" + std::string(platform::ExeSuffix);
    fs::path outputPath = buildDir / outputName;
    args.push_back("-o");
    args.push_back(outputPath.generic_string());

    auto r = platform::runProcessCapture(clangxx, args);
    // Merge stderr: clang++ writes its diagnostics (a compile error, a
    // `cannot find -ltopo-parallel`, an SDK/-isysroot failure) to STDERR, and
    // the equivalence asserts print `RunResult.output` on a failed vanilla
    // build. Dropping stderr left "Vanilla build failed:" with an EMPTY body,
    // making every standalone vanilla-link failure undiagnosable. A vanilla
    // build's output is never compared (only runBinary's stdout is), so
    // merging is safe — mirrors topoBuild's stderr handling.
    return RunResult{r.exitCode, r.stdoutOutput + r.stderrOutput};
}

// ============================================================================
// vanillaSharedBuild
// ============================================================================

RunResult E2eFixture::vanillaSharedBuild(const std::string& projectName, const std::vector<std::string>& extraDefines) {
    auto config = parseTopoToml(projectName);
    fs::path projDir = projectsDir_ / projectName;

    std::string clangxx = (llvmBinDir_ / ("clang++" + std::string(platform::ExeSuffix))).generic_string();

    // --- Step 1: Compile each source to an object file ---
    std::vector<std::string> objFiles;

    // Expand source globs
    std::vector<std::string> allSources;
    for (const auto& srcPattern : config.sources) {
        if (srcPattern.find('*') != std::string::npos) {
            fs::path srcDir = projDir / fs::path(srcPattern).parent_path();
            std::string ext = fs::path(srcPattern).extension().string();
            if (fs::exists(srcDir)) {
                for (const auto& entry : fs::directory_iterator(srcDir)) {
                    if (entry.is_regular_file() && entry.path().extension() == ext) {
                        allSources.push_back(entry.path().generic_string());
                    }
                }
            }
        } else {
            allSources.push_back((projDir / srcPattern).generic_string());
        }
    }

    fs::path buildDir = projDir / "build";
    fs::create_directories(buildDir);

    // macOS: resolve SDK path once for all compile/link steps
    std::vector<std::string> sysrootArgs;
    if constexpr (platform::IsMacOS) {
        auto sdkResult = platform::runProcessCapture("xcrun", {"--show-sdk-path"});
        if (sdkResult.exitCode == 0 && !sdkResult.stdoutOutput.empty()) {
            std::string sdkPath = sdkResult.stdoutOutput;
            while (!sdkPath.empty() && (sdkPath.back() == '\n' || sdkPath.back() == '\r'))
                sdkPath.pop_back();
            sysrootArgs.push_back("-isysroot");
            sysrootArgs.push_back(sdkPath);
        }
    }

    for (const auto& srcFile : allSources) {
        std::vector<std::string> args;
        args.push_back("-O2");
        args.push_back("-std=" + config.standard);
        args.push_back("-c");
        args.insert(args.end(), sysrootArgs.begin(), sysrootArgs.end());

        // Include directories
        for (const auto& inc : config.include) {
            fs::path incPath = inc;
            if (incPath.is_relative()) {
                incPath = projDir / incPath;
            }
            args.push_back("-I" + incPath.generic_string());
        }

        // Extra defines
        for (const auto& def : extraDefines) {
            args.push_back("-D" + def);
        }

        // Unix: position-independent code + hidden visibility by default
        if constexpr (!platform::IsWindows) {
            args.push_back("-fPIC");
            args.push_back("-fvisibility=hidden");
        }

        args.push_back(srcFile);

        // Output object file
        std::string objName = fs::path(srcFile).stem().string() + std::string(platform::ObjectFileSuffix);
        fs::path objPath = buildDir / objName;
        args.push_back("-o");
        args.push_back(objPath.generic_string());

        auto r = platform::runProcessCapture(clangxx, args);
        if (r.exitCode != 0) {
            return RunResult{r.exitCode, r.stdoutOutput};
        }
        objFiles.push_back(objPath.generic_string());
    }

    // --- Step 2: Link into shared library ---
    std::vector<std::string> linkArgs;
    linkArgs.push_back("-shared");
    linkArgs.push_back("-O2");
    linkArgs.insert(linkArgs.end(), sysrootArgs.begin(), sysrootArgs.end());

    for (const auto& obj : objFiles) {
        linkArgs.push_back(obj);
    }

    std::string libName = "baseline" + std::string(platform::SharedLibSuffix);
    fs::path libPath = buildDir / libName;
    linkArgs.push_back("-o");
    linkArgs.push_back(libPath.generic_string());

    // Windows: produce import library
    if constexpr (platform::IsWindows) {
        fs::path impLibPath = buildDir / "baseline.lib";
        linkArgs.push_back("-Wl,/IMPLIB:" + impLibPath.generic_string());
    }

    auto r = platform::runProcessCapture(clangxx, linkArgs);
    return RunResult{r.exitCode, r.stdoutOutput};
}

// ============================================================================
// runBinary
// ============================================================================

RunResult E2eFixture::runBinary(const std::string& projectName, const std::string& outputName) {
    fs::path binPath = binaryPath(projectName, outputName);
    std::string exe = binPath.generic_string();
    fs::path projDir = projectsDir_ / projectName;
    std::string workDir = projDir.generic_string();

    // Set library search path so the JIT engine shared library can be found
    // at runtime. Each CTest case invokes its own gtest process, so the
    // setenv here only mutates this case's environment — no cross-case
    // interference even when benchmarks run concurrently under `ctest -j N`.
#ifdef TOPO_JIT_ENGINE_DIR
    const std::string jitDir = TOPO_JIT_ENGINE_DIR;
#ifdef _WIN32
    const char* envName = "PATH";
#elif defined(__APPLE__)
    const char* envName = "DYLD_LIBRARY_PATH";
#else
    const char* envName = "LD_LIBRARY_PATH";
#endif
    std::string savedEnv;
    const char* oldVal = std::getenv(envName);
    if (oldVal) savedEnv = oldVal;

    std::string newVal = jitDir;
    if (!savedEnv.empty()) {
#ifdef _WIN32
        newVal += ";" + savedEnv;
#else
        newVal += ":" + savedEnv;
#endif
    }
#ifdef _WIN32
    _putenv_s(envName, newVal.c_str());
#else
    setenv(envName, newVal.c_str(), 1);
#endif
#endif // TOPO_JIT_ENGINE_DIR

    auto r = platform::runProcessCapture(exe, {}, workDir);

#ifdef TOPO_JIT_ENGINE_DIR
    // Restore original value
    if (savedEnv.empty()) {
#ifdef _WIN32
        _putenv_s(envName, "");
#else
        unsetenv(envName);
#endif
    } else {
#ifdef _WIN32
        _putenv_s(envName, savedEnv.c_str());
#else
        setenv(envName, savedEnv.c_str(), 1);
#endif
    }
#endif // TOPO_JIT_ENGINE_DIR

    return RunResult{r.exitCode, r.stdoutOutput};
}

// ============================================================================
// runJar
// ============================================================================

RunResult E2eFixture::runJar(const std::string& jarPath, const std::vector<std::string>& args) {
    // Find java executable
    std::string javaExe = "java";

    // Try JAVA_HOME first
    const char* javaHome = std::getenv("JAVA_HOME");
    if (javaHome && std::strlen(javaHome) > 0) {
        fs::path candidate = fs::path(javaHome) / "bin" / "java";
        if (fs::exists(candidate)) {
            javaExe = candidate.generic_string();
        }
    }

    std::vector<std::string> javaArgs;
    javaArgs.push_back("-ea"); // enable assertions
    javaArgs.push_back("-jar");
    javaArgs.push_back(jarPath);
    javaArgs.insert(javaArgs.end(), args.begin(), args.end());

    auto r = platform::runProcessCapture(javaExe, javaArgs);
    return RunResult{r.exitCode, r.stdoutOutput};
}

// ============================================================================
// compileDriver
// ============================================================================

RunResult E2eFixture::compileDriver(const std::string& projectName,
                                    const std::string& driverSource,
                                    const std::string& outputName,
                                    const std::vector<std::string>& includeDirs,
                                    const std::string& linkLib) {
    fs::path projDir = projectsDir_ / projectName;
    std::string clangxx = (llvmBinDir_ / ("clang++" + std::string(platform::ExeSuffix))).generic_string();

    std::vector<std::string> args;
    args.push_back("-O2");
    args.push_back("-std=c++17");

    // macOS: bundled clang++ needs explicit SDK path
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

    for (const auto& inc : includeDirs) {
        fs::path incPath = inc;
        if (incPath.is_relative()) {
            incPath = projDir / incPath;
        }
        args.push_back("-I" + incPath.generic_string());
    }

    args.push_back((projDir / driverSource).generic_string());

    if (!linkLib.empty()) {
        // Check project root first, then build/ subdirectory
        fs::path libPath = projDir / linkLib;
        if (!fs::exists(libPath)) libPath = projDir / "build" / linkLib;
        args.push_back(libPath.generic_string());
    }

    fs::path buildDir = projDir / "build";
    fs::create_directories(buildDir);
    fs::path outputPath = buildDir / (outputName + std::string(platform::ExeSuffix));
    args.push_back("-o");
    args.push_back(outputPath.generic_string());

    auto r = platform::runProcessCapture(clangxx, args);
    return RunResult{r.exitCode, r.stdoutOutput};
}

// ============================================================================
// assertOutputMatches
// ============================================================================

static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        // Remove trailing \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

void E2eFixture::assertOutputMatches(const std::string& actual, const std::string& expected) {
    auto actualLines = splitLines(actual);
    auto expectedLines = splitLines(expected);

    size_t ai = 0;
    for (size_t ei = 0; ei < expectedLines.size(); ++ei) {
        const auto& pat = expectedLines[ei];

        // ? prefix: optional line — skip if not matched
        if (!pat.empty() && pat[0] == '?') {
            std::string optPat = pat.substr(1);
            // Trim leading space
            if (!optPat.empty() && optPat[0] == ' ') optPat = optPat.substr(1);
            if (ai < actualLines.size()) {
                // Try to match; if fails, just skip this expected line
                std::string regexPat = std::regex_replace(optPat, std::regex(R"(\{\{NUM\}\})"), R"(\d+)");
                regexPat = std::regex_replace(regexPat, std::regex(R"(\{\{ANY\}\})"), R"(.*)");
                try {
                    if (std::regex_match(actualLines[ai], std::regex(regexPat))) {
                        ++ai;
                    }
                } catch (...) {
                    // Regex error — treat as literal
                    if (actualLines[ai] == optPat) ++ai;
                }
            }
            continue;
        }

        // ~ prefix: regex line
        if (!pat.empty() && pat[0] == '~') {
            std::string regexStr = pat.substr(1);
            if (!regexStr.empty() && regexStr[0] == ' ') regexStr = regexStr.substr(1);
            ASSERT_LT(ai, actualLines.size()) << "Expected more output lines. Pattern: " << pat;
            EXPECT_TRUE(std::regex_search(actualLines[ai], std::regex(regexStr)))
                << "Line " << ai << " does not match regex: " << regexStr << "\nActual: " << actualLines[ai];
            ++ai;
            continue;
        }

        // ... : skip any number of actual lines until next pattern matches
        if (pat == "...") {
            // Peek at next expected line to find what to match
            if (ei + 1 >= expectedLines.size()) {
                // ... at end: consume all remaining
                ai = actualLines.size();
                continue;
            }
            continue; // The next expected line will scan forward
        }

        // Normal line: may contain {{NUM}} wildcards
        std::string regexPat = pat;
        // Escape regex special chars except our wildcards
        // First replace {{NUM}} and {{ANY}} with placeholders
        std::string escaped;
        size_t pos = 0;
        while (pos < regexPat.size()) {
            if (regexPat.substr(pos, 7) == "{{NUM}}") {
                escaped += R"(-?\d+\.?\d*)";
                pos += 7;
            } else if (regexPat.substr(pos, 7) == "{{ANY}}") {
                escaped += ".*";
                pos += 7;
            } else {
                char c = regexPat[pos];
                // Escape regex metacharacters
                if (std::string("\\^$.|+[](){}").find(c) != std::string::npos) {
                    escaped += '\\';
                }
                escaped += c;
                ++pos;
            }
        }

        // If previous pattern was "...", scan forward
        if (ei > 0 && expectedLines[ei - 1] == "...") {
            bool found = false;
            while (ai < actualLines.size()) {
                try {
                    if (std::regex_match(actualLines[ai], std::regex(escaped))) {
                        found = true;
                        break;
                    }
                } catch (...) {
                    if (actualLines[ai] == pat) {
                        found = true;
                        break;
                    }
                }
                ++ai;
            }
            ASSERT_TRUE(found) << "Pattern after '...' not found: " << pat << "\nRemaining output:\n" << actual;
            ++ai;
            continue;
        }

        ASSERT_LT(ai, actualLines.size()) << "Expected more output. Pattern: " << pat << "\nFull output:\n" << actual;

        try {
            EXPECT_TRUE(std::regex_match(actualLines[ai], std::regex(escaped)))
                << "Line " << ai << " mismatch.\n"
                << "Expected pattern: " << pat << "\n"
                << "Actual:           " << actualLines[ai];
        } catch (...) {
            EXPECT_EQ(actualLines[ai], pat) << "Line " << ai << " mismatch (literal comparison)";
        }
        ++ai;
    }
}

// ============================================================================
// getBinarySize
// ============================================================================

uintmax_t E2eFixture::getBinarySize(const std::string& projectName, const std::string& outputName) {
    fs::path path = binaryPath(projectName, outputName);
    if (!fs::exists(path)) return 0;
    return fs::file_size(path);
}

// ============================================================================
// getExportedSymbolCount
// ============================================================================

int E2eFixture::getExportedSymbolCount(const std::string& projectName, const std::string& outputName) {
    fs::path binPath = binaryPath(projectName, outputName);
    std::string nm = (llvmBinDir_ / ("llvm-nm" + std::string(platform::ExeSuffix))).generic_string();

    // llvm-nm --defined-only --extern-only
    auto r = platform::runProcessCapture(nm, {"--defined-only", "--extern-only", binPath.generic_string()});
    if (r.exitCode != 0) return -1;

    // Count non-empty lines
    int count = 0;
    auto lines = splitLines(r.stdoutOutput);
    for (const auto& line : lines) {
        if (!line.empty()) ++count;
    }
    return count;
}

// ============================================================================
// binaryPath / sharedLibPath
// ============================================================================

fs::path E2eFixture::binaryPath(const std::string& projectName, const std::string& outputName) {
    // topo-build places output in the project root; vanillaBuild places
    // output in the project's build/ subdirectory.  Check both locations.
    fs::path root = projectsDir_ / projectName / (outputName + std::string(platform::ExeSuffix));
    if (fs::exists(root)) return root;
    return projectsDir_ / projectName / "build" / (outputName + std::string(platform::ExeSuffix));
}

fs::path E2eFixture::sharedLibPath(const std::string& projectName, const std::string& outputName) {
    fs::path root = projectsDir_ / projectName / (outputName + std::string(platform::SharedLibSuffix));
    if (fs::exists(root)) return root;
    return projectsDir_ / projectName / "build" / (outputName + std::string(platform::SharedLibSuffix));
}

// ============================================================================
// parseTopoToml — minimal parser for build fields
// ============================================================================

E2eFixture::TomlConfig E2eFixture::parseTopoToml(const std::string& projectName) {
    TomlConfig config;
    fs::path tomlPath = projectsDir_ / projectName / "Topo.toml";

    std::ifstream f(tomlPath);
    if (!f.is_open()) return config;

    std::string line;
    std::string currentSection;

    while (std::getline(f, line)) {
        // Remove trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Skip comments
        if (line[0] == '#') continue;

        // Section header
        if (line[0] == '[') {
            auto end = line.find(']');
            if (end != std::string::npos) {
                currentSection = line.substr(1, end - 1);
            }
            continue;
        }

        // Key = value
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\"");
            size_t b = s.find_last_not_of(" \t\"");
            if (a == std::string::npos) {
                s.clear();
                return;
            }
            s = s.substr(a, b - a + 1);
        };
        trim(key);
        trim(val);

        if (currentSection == "build") {
            if (key == "standard") {
                config.standard = val;
            } else if (key == "output") {
                config.output = val;
            } else if (key == "output_type") {
                config.outputType = val;
            } else if (key == "sources" || key == "include") {
                // Parse array: ["a", "b", "c"]
                std::vector<std::string>& target = (key == "sources") ? config.sources : config.include;
                // Remove brackets
                auto lb = val.find('[');
                auto rb = val.find(']');
                if (lb != std::string::npos && rb != std::string::npos) {
                    std::string inner = val.substr(lb + 1, rb - lb - 1);
                    std::istringstream ss(inner);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        // Trim quotes and spaces
                        size_t a = item.find_first_not_of(" \t\"");
                        size_t b = item.find_last_not_of(" \t\"");
                        if (a != std::string::npos) {
                            target.push_back(item.substr(a, b - a + 1));
                        }
                    }
                }
            }
        }
    }

    return config;
}

// ============================================================================
// Pass-fired signal helpers
// ============================================================================
//
// topo-build run with --dump-ir emits the post-transform module to
// `<output>.ll`. Each Topo pass marks itself via module-level named metadata
// of the form:
//
//   !topo.fired.PipelineCodeGenPass = !{!42}
//   !42 = !{i32 1}
//
// To detect a marker without parsing LLVM IR structurally, we scan the .ll
// file textually for the line-anchored pattern
// `!topo.fired.<PassName> = !{...}`. The integer count lives in the
// referenced MDNode, which we resolve in a second pass when needed.

namespace {

// Locate the .ll file written by `--dump-ir`. topo-build places it at
// `<outputPath>.ll` — in practice, alongside the compiled binary. Try both
// project root (where the binary lives for topo-build outputs) and
// `build/<name>.ll` (where vanillaBuild places its baseline).
fs::path findDumpedIR(const fs::path& projectDir, const std::string& outputName) {
    fs::path root = projectDir / (outputName + ".ll");
    if (fs::exists(root)) return root;
    fs::path inBuild = projectDir / "build" / (outputName + ".ll");
    if (fs::exists(inBuild)) return inBuild;
    return {};
}

struct MarkerInfo {
    std::string mdRef;   // "!42"
    std::string passName;
};

// One-pass textual scan: collect every `!topo.fired.<Name> = !{!N}` line.
std::vector<MarkerInfo> scanMarkers(const std::string& contents) {
    std::vector<MarkerInfo> markers;
    // Match "!topo.fired.Name = !{!123}" (whitespace tolerant).
    // Capture group 1: pass name (alphanumeric + underscore)
    // Capture group 2: !N reference
    static const std::regex re(R"(^\s*!topo\.fired\.([A-Za-z_][A-Za-z0-9_]*)\s*=\s*!\{\s*(![0-9]+)\s*\})",
                               std::regex::ECMAScript);
    std::istringstream iss(contents);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            markers.push_back(MarkerInfo{m[2].str(), m[1].str()});
        }
    }
    return markers;
}

// Given the `!42` reference produced by scanMarkers, find its definition line
// and extract the i32 count. Lines look like `!42 = !{i32 5}`.
unsigned resolveMarkerCount(const std::string& contents, const std::string& mdRef) {
    // Match "<mdRef> = !{i32 <N>}"
    std::string pattern = "^\\s*" + std::regex_replace(mdRef, std::regex("!"), R"(\!)") +
                          R"(\s*=\s*!\{\s*i32\s+([0-9]+)\s*\})";
    std::regex re(pattern, std::regex::ECMAScript);
    std::istringstream iss(contents);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            try {
                return static_cast<unsigned>(std::stoul(m[1].str()));
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

std::string readIRFile(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // anonymous namespace

std::set<std::string> E2eFixture::collectPassFiredMarkers(const std::string& projectName,
                                                          const std::string& outputName) {
    fs::path projDir = projectsDir_ / projectName;
    fs::path irPath = findDumpedIR(projDir, outputName);
    if (irPath.empty()) return {};

    std::string contents = readIRFile(irPath);
    auto markers = scanMarkers(contents);

    std::set<std::string> names;
    for (const auto& m : markers) names.insert(m.passName);
    return names;
}

unsigned E2eFixture::getPassFiredCount(const std::string& projectName,
                                       const std::string& outputName,
                                       const std::string& passName) {
    fs::path projDir = projectsDir_ / projectName;
    fs::path irPath = findDumpedIR(projDir, outputName);
    if (irPath.empty()) return 0;

    std::string contents = readIRFile(irPath);
    auto markers = scanMarkers(contents);
    for (const auto& m : markers) {
        if (m.passName == passName) {
            return resolveMarkerCount(contents, m.mdRef);
        }
    }
    return 0;
}

void E2eFixture::assertPassFired(const std::string& projectName,
                                 const std::string& outputName,
                                 const std::string& passName) {
    unsigned count = getPassFiredCount(projectName, outputName, passName);
    fs::path irPath = findDumpedIR(projectsDir_ / projectName, outputName);
    EXPECT_GT(count, 0u)
        << "Pass '" << passName << "' did not fire on build '"
        << projectName << "/" << outputName << "'.\n"
        << "IR file: " << irPath.generic_string() << "\n"
        << "Either the workload lacks the pattern the pass targets, the pass "
           "was disabled, or the equivalence assertion is trivially satisfied.\n"
        << "A pass-fired check is required so the equivalence assertion is not trivially satisfied.";
}

void E2eFixture::assertPassNotFired(const std::string& projectName,
                                    const std::string& outputName,
                                    const std::string& passName) {
    unsigned count = getPassFiredCount(projectName, outputName, passName);
    EXPECT_EQ(count, 0u)
        << "Pass '" << passName << "' unexpectedly fired (" << count
        << " transformations) on build '" << projectName << "/" << outputName
        << "' — this build was expected to be a no-op (pass-off config).";
}

// ============================================================================
// Benchmark sampling primitives
// ============================================================================
//
// `measureWithVarianceAdapt` runs `fn` at least `minRuns` times and keeps
// resampling until the coefficient of variation falls at or below
// `cvTarget`, capped at `maxRuns`. `benchSeed()` lazily resolves the
// process-global benchmark seed (env `TOPO_BENCH_SEED` or random_device).
//
// Keep this block free of module-specific state — both LLVM and JVM E2E
// harnesses include the same header and ship with their own translation
// unit, so any unrelated coupling would create ODR drift.

namespace {

std::uint32_t resolveBenchSeed() {
    const char* env = std::getenv("TOPO_BENCH_SEED");
    if (env && env[0] != '\0') {
        try {
            unsigned long v = std::stoul(env);
            return static_cast<std::uint32_t>(v);
        } catch (...) {
            // Fall through to random_device on parse error.
        }
    }
    std::random_device rd;
    return static_cast<std::uint32_t>(rd());
}

} // namespace

std::uint32_t benchSeed() {
    // C++11 static init is thread-safe; lambda runs exactly once.
    static const std::uint32_t seed = []() {
        std::uint32_t s = resolveBenchSeed();
        const char* env = std::getenv("TOPO_BENCH_SEED");
        bool fromEnv = env && env[0] != '\0';
        // The seed controls GTest test-order shuffling, not microsecond
        // timings. Workloads have no RNG and the seed never propagates
        // into binary measurement — see the header doc for the full
        // caveat list. Keep the log line accurate so triagers don't
        // chase a determinism leak that doesn't structurally exist.
        std::printf(
            "[ SEED   ] TOPO_BENCH_SEED=%u (%s)\n",
            s,
            fromEnv
                ? "gtest test-order reproducible (timings remain env-sensitive)"
                : "random_device — set TOPO_BENCH_SEED to reproduce gtest test order");
        std::fflush(stdout);
        // Propagate to GTest so --gtest_shuffle (when enabled) reproduces.
        ::testing::GTEST_FLAG(random_seed) = static_cast<std::int32_t>(s & 0x7fffffff);
        return s;
    }();
    return seed;
}

BenchStats measureWithVarianceAdapt(std::function<double()> fn,
                                    int minRuns,
                                    int maxRuns,
                                    double cvTarget) {
    if (minRuns < 1) minRuns = 1;
    if (maxRuns < minRuns) maxRuns = minRuns;
    if (cvTarget <= 0.0) cvTarget = 0.05;

    BenchStats stats;
    stats.samples.reserve(static_cast<std::size_t>(maxRuns));

    auto recompute = [&]() {
        std::vector<double> valid;
        valid.reserve(stats.samples.size());
        for (const auto& s : stats.samples) {
            if (s.us > 0.0) valid.push_back(s.us);
        }
        stats.runs = static_cast<int>(stats.samples.size());
        if (valid.empty()) {
            stats.median = stats.mean = stats.stdev = stats.cv = 0.0;
            return;
        }
        std::sort(valid.begin(), valid.end());
        const std::size_t n = valid.size();
        stats.median = (n % 2 == 1) ? valid[n / 2]
                                    : 0.5 * (valid[n / 2 - 1] + valid[n / 2]);
        double sum = 0.0;
        for (double v : valid) sum += v;
        stats.mean = sum / static_cast<double>(n);
        if (n >= 2) {
            double sq = 0.0;
            for (double v : valid) {
                double d = v - stats.mean;
                sq += d * d;
            }
            stats.stdev = std::sqrt(sq / static_cast<double>(n - 1));
        } else {
            stats.stdev = 0.0;
        }
        stats.cv = (stats.mean > 0.0) ? (stats.stdev / stats.mean) : 0.0;
    };

    for (int i = 0; i < maxRuns; ++i) {
        double us = 0.0;
        try {
            us = fn();
        } catch (...) {
            us = -1.0;
        }
        stats.samples.push_back(BenchSample{us, i});
        recompute();

        if (i + 1 >= minRuns && stats.runs >= 2 && stats.mean > 0.0 &&
            stats.cv <= cvTarget) {
            // Variance target met after at least minRuns samples.
            stats.resampleCapHit = false;
            return stats;
        }
    }

    // Reached the cap without converging; final stats are recorded as-is.
    stats.resampleCapHit = true;
    return stats;
}

// ============================================================================
// Interleaved variance-adaptive measurement
// ============================================================================
//
// Identical per-probe stats math to `measureWithVarianceAdapt`, but the
// sampling order is rotated across all probes so that transient system
// noise affects every mode roughly evenly. See the header doc for the
// reproducibility rationale (vanilla measured in a quiet window vs auto
// measured in a noisy window was the original source of 2-3x ratio
// swings on the same seed).

namespace {

// Recompute aggregate fields on `s` from its `samples` vector. Mirrors
// the local `recompute` lambda in `measureWithVarianceAdapt`. Kept as a
// free function so the interleaved variant can call it on each probe.
void recomputeStats(BenchStats& s) {
    std::vector<double> valid;
    valid.reserve(s.samples.size());
    for (const auto& sample : s.samples) {
        if (sample.us > 0.0) valid.push_back(sample.us);
    }
    s.runs = static_cast<int>(s.samples.size());
    if (valid.empty()) {
        s.median = s.mean = s.stdev = s.cv = 0.0;
        return;
    }
    std::sort(valid.begin(), valid.end());
    const std::size_t n = valid.size();
    s.median = (n % 2 == 1) ? valid[n / 2]
                            : 0.5 * (valid[n / 2 - 1] + valid[n / 2]);
    double sum = 0.0;
    for (double v : valid) sum += v;
    s.mean = sum / static_cast<double>(n);
    if (n >= 2) {
        double sq = 0.0;
        for (double v : valid) {
            double d = v - s.mean;
            sq += d * d;
        }
        s.stdev = std::sqrt(sq / static_cast<double>(n - 1));
    } else {
        s.stdev = 0.0;
    }
    s.cv = (s.mean > 0.0) ? (s.stdev / s.mean) : 0.0;
}

} // namespace

std::vector<BenchStats> measureWithVarianceAdaptInterleaved(
    std::vector<std::function<double()>> fns,
    int minRuns,
    int maxRuns,
    double cvTarget) {
    if (minRuns < 1) minRuns = 1;
    if (maxRuns < minRuns) maxRuns = minRuns;
    if (cvTarget <= 0.0) cvTarget = 0.05;

    const std::size_t k = fns.size();
    std::vector<BenchStats> out(k);
    for (auto& s : out) s.samples.reserve(static_cast<std::size_t>(maxRuns));

    // `active[i] == true` while probe i still needs more samples. We
    // keep collecting from active probes; an already-converged probe is
    // still sampled if any sibling probe is still active, so that all
    // probes accumulate samples in the same wall-clock windows (that is
    // the entire point of interleaving — see header doc).
    std::vector<bool> hasFn(k, false);
    for (std::size_t i = 0; i < k; ++i) hasFn[i] = static_cast<bool>(fns[i]);

    for (int round = 0; round < maxRuns; ++round) {
        for (std::size_t i = 0; i < k; ++i) {
            if (!hasFn[i]) continue;
            double us = 0.0;
            try {
                us = fns[i]();
            } catch (...) {
                us = -1.0;
            }
            out[i].samples.push_back(BenchSample{us, round});
            recomputeStats(out[i]);
        }

        // Convergence check after the full round so every probe has
        // exactly `round + 1` samples (assuming non-skipped).
        if (round + 1 >= minRuns) {
            bool allConverged = true;
            for (std::size_t i = 0; i < k; ++i) {
                if (!hasFn[i]) continue;
                const auto& s = out[i];
                if (s.runs < 2 || s.mean <= 0.0 || s.cv > cvTarget) {
                    allConverged = false;
                    break;
                }
            }
            if (allConverged) {
                for (auto& s : out) s.resampleCapHit = false;
                return out;
            }
        }
    }

    for (std::size_t i = 0; i < k; ++i) {
        if (hasFn[i]) out[i].resampleCapHit = true;
    }
    return out;
}

// ============================================================================
// Category contract helpers
// ============================================================================
//
// Thresholds below implement the feature-taxonomy category contract. They
// are the initial baseline values pending calibration. If you change a
// number here, keep it consistent with the project's feature-taxonomy
// policy (gate script checks).
//
// ANSI escape sequences are honoured by ctest's `--output-on-failure` when
// stdout is a TTY. They are plain bytes in log files, which is acceptable
// for CI artefact grep ("ERROR" / "WARN" substrings still match).

namespace {

constexpr const char* kAnsiReset = "\x1b[0m";
constexpr const char* kAnsiYellow = "\x1b[33m";
constexpr const char* kAnsiRed = "\x1b[31m";
constexpr double kAbsTimeFloorUs = 10000.0; // below this, skip threshold checks

enum class Verdict { Pass, Warn, Error };

// Record a verdict line on stdout with colour and the relevant numbers.
void emitVerdict(Verdict v, const char* passName, const char* label,
                 double ratio, const char* note) {
    switch (v) {
    case Verdict::Pass:
        std::printf("[  PASS  ]   %s %s: %.3f — %s\n", passName, label, ratio, note);
        break;
    case Verdict::Warn:
        std::printf("%s[ WARN   ]%s   %s %s: %.3f — %s\n",
                    kAnsiYellow, kAnsiReset, passName, label, ratio, note);
        ::testing::Test::RecordProperty("warn",
                                        std::string(passName) + " " + label);
        break;
    case Verdict::Error:
        std::printf("%s[ ERROR  ]%s   %s %s: %.3f — %s\n",
                    kAnsiRed, kAnsiReset, passName, label, ratio, note);
        ADD_FAILURE() << passName << " " << label << ": ratio " << ratio
                      << " — " << note;
        break;
    }
    std::fflush(stdout);
}

// Absolute rule 3: skip thresholds when the relevant absolute time is
// below 10 ms. Returns true iff the caller should skip its verdict.
bool belowAbsFloor(double us, const char* passName, const char* label) {
    if (us <= 0.0) return true; // no signal yet
    if (us < kAbsTimeFloorUs) {
        std::printf(
            "[  SKIP  ]   %s %s: absolute time %.0f us below 10ms floor "
            "(§4 rule 3) — threshold check skipped\n",
            passName, label, us);
        std::fflush(stdout);
        return true;
    }
    return false;
}

// Absolute rule 4: demote Error → Warn when any relevant stats block
// ran into the resample cap (CV unreliable).
Verdict demoteIfCapHit(Verdict v, const BenchStats& a, const BenchStats& b,
                       const char* passName, const char* label) {
    if (v != Verdict::Error) return v;
    if (!a.resampleCapHit && !b.resampleCapHit) return v;
    std::printf(
        "%s[ WARN   ]%s   %s %s: resample cap hit (CV>0.05 after 10 runs) "
        "— signal unreliable; ERROR demoted to WARN per §4 rule 4\n",
        kAnsiYellow, kAnsiReset, passName, label);
    ::testing::Test::RecordProperty("warn",
                                    std::string(passName) + " " + label +
                                        " (cap-hit)");
    std::fflush(stdout);
    return Verdict::Warn;
}

// Absolute rule 1: auto/base > 1.10 is an unconditional ERROR across all
// categories. Applied by every helper before the category-specific logic.
void enforceAutoBaseHardRule(const BenchStats& base, const BenchStats& autoStats,
                             const char* passName) {
    if (base.runs == 0 || autoStats.runs == 0) return;
    if (base.mean <= 0.0 || autoStats.mean <= 0.0) return;
    if (belowAbsFloor(base.mean, passName, "auto/base (hard rule #1)")) return;
    const double ratio = autoStats.mean / base.mean;
    if (ratio > 1.10) {
        Verdict v = demoteIfCapHit(Verdict::Error, base, autoStats,
                                   passName, "auto/base (hard rule #1)");
        emitVerdict(v, passName, "auto/base",
                    ratio,
                    "auto must not be > 1.10× base (§4 absolute rule #1)");
    }
}

// TODO: `forced` bytecode/IR diff enforcement
// lives in the equivalence test layer — see gate script check 6. Benchmark
// helpers intentionally skip it here.

// Emit a PASS line so the operator sees the benchmark was evaluated even
// when nothing went wrong.
void emitPass(const char* passName, const char* label, double ratio) {
    emitVerdict(Verdict::Pass, passName, label, ratio, "within contract");
}

} // anonymous namespace

void assertOptCategoryContract(const BenchStats& vanilla,
                               const BenchStats& base,
                               const BenchStats& autoStats,
                               const BenchStats& forced,
                               Workload workload,
                               const char* passName) {
    (void)vanilla; // OPT contract uses base as denominator
    enforceAutoBaseHardRule(base, autoStats, passName);

    if (base.runs == 0 || base.mean <= 0.0) return;
    if (belowAbsFloor(base.mean, passName, "OPT")) return;

    auto ratioVerdict = [&](double ratio, double pass, double warn,
                            bool lowerIsBetter, const char* label) {
        Verdict v;
        if (lowerIsBetter) {
            if (ratio <= pass) v = Verdict::Pass;
            else if (ratio <= warn) v = Verdict::Warn;
            else v = Verdict::Error;
        } else {
            if (ratio >= pass) v = Verdict::Pass;
            else if (ratio >= warn) v = Verdict::Warn;
            else v = Verdict::Error;
        }
        v = demoteIfCapHit(v, base, forced, passName, label);
        emitVerdict(v, passName, label, ratio,
                    lowerIsBetter ? "lower ratio is better"
                                  : "higher ratio is expected (cost visible)");
    };

    if (workload == Workload::Friendly) {
        // friendly: forced/base ≤ 0.90 PASS; auto/base ≤ 0.95 PASS
        if (forced.runs > 0 && forced.mean > 0.0) {
            ratioVerdict(forced.mean / base.mean, 0.90, 0.95,
                         /*lowerIsBetter=*/true, "forced/base");
        }
        if (autoStats.runs > 0 && autoStats.mean > 0.0) {
            double r = autoStats.mean / base.mean;
            Verdict v;
            if (r <= 0.95) v = Verdict::Pass;
            else if (r <= 1.05) v = Verdict::Warn;
            else v = Verdict::Error;
            v = demoteIfCapHit(v, base, autoStats, passName, "auto/base");
            if (v == Verdict::Pass) emitPass(passName, "auto/base", r);
            else emitVerdict(v, passName, "auto/base", r,
                             "friendly auto must be ≤ 0.95× base (§4)");
        }
    } else {
        // unfriendly: auto/base ∈ [0.95, 1.05]; forced/base > 1.05
        if (autoStats.runs > 0 && autoStats.mean > 0.0) {
            double r = autoStats.mean / base.mean;
            Verdict v;
            if (r >= 0.95 && r <= 1.05) v = Verdict::Pass;
            else if (r <= 1.10) v = Verdict::Warn;
            else v = Verdict::Error;
            v = demoteIfCapHit(v, base, autoStats, passName, "auto/base");
            if (v == Verdict::Pass) emitPass(passName, "auto/base", r);
            else emitVerdict(v, passName, "auto/base", r,
                             "unfriendly auto must stay within ±5% of base (§4)");
        }
        if (forced.runs > 0 && forced.mean > 0.0) {
            double r = forced.mean / base.mean;
            Verdict v;
            if (r > 1.05) v = Verdict::Pass; // cost visible — expected
            else if (r >= 0.95) v = Verdict::Warn; // cost not visible
            else v = Verdict::Error;       // friendly/unfriendly labels suspect
            v = demoteIfCapHit(v, base, forced, passName, "forced/base");
            if (v == Verdict::Pass) emitPass(passName, "forced/base", r);
            else emitVerdict(v, passName, "forced/base", r,
                             "unfriendly forced is expected to show cost (§4)");
        }
    }
}

void assertEnhanceCategoryContract(const BenchStats& vanilla,
                                   const BenchStats& base,
                                   const BenchStats& autoStats,
                                   const BenchStats& forced,
                                   Workload workload,
                                   const char* passName) {
    (void)workload;
    (void)base;
    enforceAutoBaseHardRule(base, autoStats, passName);
    if (vanilla.runs == 0 || vanilla.mean <= 0.0) return;
    if (belowAbsFloor(vanilla.mean, passName, "ENHANCE")) return;

    auto check = [&](const BenchStats& s, const char* label) {
        if (s.runs == 0 || s.mean <= 0.0) return;
        double r = s.mean / vanilla.mean;
        Verdict v;
        if (r <= 1.05) v = Verdict::Pass;
        else if (r <= 1.10) v = Verdict::Warn;
        else v = Verdict::Error;
        v = demoteIfCapHit(v, vanilla, s, passName, label);
        if (v == Verdict::Pass) emitPass(passName, label, r);
        else emitVerdict(v, passName, label, r,
                         "ENHANCE must not regress > 1.05× vanilla (§4)");
    };
    check(autoStats, "auto/vanilla");
    check(forced, "forced/vanilla");
}

void assertCoveredCategoryContract(const BenchStats& vanilla,
                                   const BenchStats& base,
                                   const BenchStats& autoStats,
                                   const BenchStats& forced,
                                   Workload workload,
                                   const char* passName) {
    (void)workload;
    (void)base;
    enforceAutoBaseHardRule(base, autoStats, passName);
    if (vanilla.runs == 0 || vanilla.mean <= 0.0) return;
    if (belowAbsFloor(vanilla.mean, passName, "COVERED")) return;

    auto check = [&](const BenchStats& s, const char* label) {
        if (s.runs == 0 || s.mean <= 0.0) return;
        double r = s.mean / vanilla.mean;
        Verdict v;
        if (r >= 0.95 && r <= 1.05) v = Verdict::Pass;
        else if (r >= 0.90 && r <= 1.10) v = Verdict::Warn;
        else v = Verdict::Error;
        v = demoteIfCapHit(v, vanilla, s, passName, label);
        if (v == Verdict::Pass) emitPass(passName, label, r);
        else emitVerdict(v, passName, label, r,
                         "COVERED expected ≈ vanilla (O2/C2/HW covered)");
    };
    check(autoStats, "auto/vanilla");
    check(forced, "forced/vanilla");
}

void assertInstrumentCategoryContract(const BenchStats& vanilla,
                                      const BenchStats& base,
                                      const BenchStats& autoStats,
                                      const BenchStats& forced,
                                      Workload workload,
                                      const char* passName) {
    (void)workload;
    (void)base;
    enforceAutoBaseHardRule(base, autoStats, passName);
    if (vanilla.runs == 0 || vanilla.mean <= 0.0) return;
    if (belowAbsFloor(vanilla.mean, passName, "INSTRUMENT")) return;

    auto check = [&](const BenchStats& s, const char* label) {
        if (s.runs == 0 || s.mean <= 0.0) return;
        double r = s.mean / vanilla.mean;
        Verdict v;
        if (r <= 1.10) v = Verdict::Pass;
        else if (r <= 1.20) v = Verdict::Warn;
        else v = Verdict::Error;
        v = demoteIfCapHit(v, vanilla, s, passName, label);
        if (v == Verdict::Pass) emitPass(passName, label, r);
        else emitVerdict(v, passName, label, r,
                         "INSTRUMENT overhead must stay ≤ 1.10× vanilla (§4)");
    };
    check(autoStats, "auto/vanilla");
    check(forced, "forced/vanilla");
}

void assertRuntimeCategoryContract(const BenchStats& vanilla,
                                   const BenchStats& base,
                                   const BenchStats& autoStats,
                                   const BenchStats& forced,
                                   Workload workload,
                                   const char* passName) {
    (void)workload;
    (void)base;
    enforceAutoBaseHardRule(base, autoStats, passName);
    if (vanilla.runs == 0 || vanilla.mean <= 0.0) return;
    if (belowAbsFloor(vanilla.mean, passName, "RUNTIME")) return;

    auto check = [&](const BenchStats& s, const char* label) {
        if (s.runs == 0 || s.mean <= 0.0) return;
        double r = s.mean / vanilla.mean;
        Verdict v;
        if (r <= 1.10) v = Verdict::Pass;
        else if (r <= 1.25) v = Verdict::Warn;
        else v = Verdict::Error;
        v = demoteIfCapHit(v, vanilla, s, passName, label);
        if (v == Verdict::Pass) emitPass(passName, label, r);
        else emitVerdict(v, passName, label, r,
                         "RUNTIME overhead must stay ≤ 1.10× vanilla (§4)");
    };
    check(autoStats, "auto/vanilla");
    check(forced, "forced/vanilla");
}

} // namespace topo::test::e2e
