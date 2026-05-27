#include "VariantBenchmark.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"
#include "topo/Platform/TempFile.h"
#include "topo/Platform/ToolResolution.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace topo {

namespace {

namespace fs = std::filesystem;

/// Infer the pointee type of a function parameter by analyzing its GEP uses.
llvm::Type* inferPointeeType(llvm::Function* func, unsigned argIdx) {
    if (argIdx >= func->arg_size()) return nullptr;
    auto* arg = func->getArg(argIdx);
    for (auto* user : arg->users()) {
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) return gep->getSourceElementType();
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(user)) {
            if (auto* callee = call->getCalledFunction()) {
                for (unsigned i = 0; i < call->arg_size(); ++i) {
                    if (call->getArgOperand(i) == arg) {
                        if (auto* ty = inferPointeeType(callee, i)) return ty;
                    }
                }
            }
        }
    }
    return nullptr;
}

/// Create a void() wrapper function that calls the target function with
/// zero-initialized arguments of the correct types.
/// When bufferSizeBytes is provided, pointer arguments use that size
/// instead of the default 256KB buffer.
llvm::Function* createBenchWrapper(llvm::Module& module,
                                   llvm::StringRef targetFuncName,
                                   llvm::StringRef wrapperName,
                                   std::optional<uint64_t> bufferSizeBytes = std::nullopt) {
    auto* targetFunc = module.getFunction(targetFuncName);
    if (!targetFunc) return nullptr;

    auto& ctx = module.getContext();
    auto& DL = module.getDataLayout();
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* wrapperFTy = llvm::FunctionType::get(voidTy, {}, false);
    auto* wrapper = llvm::Function::Create(wrapperFTy, llvm::GlobalValue::ExternalLinkage, wrapperName, module);

    auto* bb = llvm::BasicBlock::Create(ctx, "entry", wrapper);
    llvm::IRBuilder<> builder(bb);

    auto* i8Ty = llvm::Type::getInt8Ty(ctx);
    auto* funcTy = targetFunc->getFunctionType();
    std::vector<llvm::Value*> args;

    for (unsigned i = 0; i < funcTy->getNumParams(); ++i) {
        auto* paramTy = funcTy->getParamType(i);
        if (paramTy->isPointerTy()) {
            auto* pointeeTy = inferPointeeType(targetFunc, i);
            if (!pointeeTy) {
                uint64_t rawBufSize = bufferSizeBytes.value_or(256 * 1024);
                auto* bufTy = llvm::ArrayType::get(i8Ty, rawBufSize);
                auto* buf = builder.CreateAlloca(bufTy, nullptr, "bench.buf");
                buf->setAlignment(llvm::Align(64));
                uint64_t bufSize = DL.getTypeAllocSize(bufTy);
                builder.CreateMemSet(buf, llvm::ConstantInt::get(i8Ty, 0), bufSize, llvm::MaybeAlign(64));
                args.push_back(buf);
            } else {
                // When bufferSizeBytes is specified, compute element count
                // and allocate an array of that many elements.
                uint64_t elemSize = DL.getTypeAllocSize(pointeeTy);
                uint64_t numElems = 1;
                if (bufferSizeBytes && elemSize > 0) {
                    numElems = *bufferSizeBytes / elemSize;
                    if (numElems == 0) numElems = 1;
                }
                llvm::Type* allocTy = pointeeTy;
                if (bufferSizeBytes && numElems > 1) {
                    allocTy = llvm::ArrayType::get(pointeeTy, numElems);
                }
                auto* alloca = builder.CreateAlloca(allocTy, nullptr, "bench.arg");
                alloca->setAlignment(llvm::Align(64));
                uint64_t allocSize = DL.getTypeAllocSize(allocTy);
                builder.CreateMemSet(alloca, llvm::ConstantInt::get(i8Ty, 0), allocSize, llvm::MaybeAlign(64));
                args.push_back(alloca);
            }
        } else {
            args.push_back(llvm::Constant::getNullValue(paramTy));
        }
    }

    builder.CreateCall(targetFunc, args);
    builder.CreateRetVoid();

    return wrapper;
}

/// Run LLVM's standard optimization pipeline on a module at the given level.
void runOptPipeline(llvm::Module& module, int optLevel) {
    if (optLevel <= 0) return;

    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    llvm::OptimizationLevel level;
    switch (optLevel) {
    case 1: level = llvm::OptimizationLevel::O1; break;
    case 3: level = llvm::OptimizationLevel::O3; break;
    default: level = llvm::OptimizationLevel::O2; break;
    }

    auto mpm = pb.buildPerModuleDefaultPipeline(level);
    mpm.run(module, mam);
}

/// Embedded benchmark stub source. Compiled once per session and linked
/// with each benchmark variant's object file to produce a self-contained
/// executable that prints the median sample time in nanoseconds.
static constexpr const char* kBenchStubSource = R"CPP(
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <algorithm>

    extern "C" void __bench_wrapper();

    int main(int argc, char** argv) {
        int warmup = argc > 1 ? std::atoi(argv[1]) : 3;
        int iterations = argc > 2 ? std::atoi(argv[2]) : 7;
        int itersPerSample = iterations > 7 ? iterations / 7 : 1;
        constexpr int numSamples = 7;

        for (int i = 0; i < warmup; ++i)
            __bench_wrapper();

        double samples[numSamples];
        for (int s = 0; s < numSamples; ++s) {
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < itersPerSample; ++i)
                __bench_wrapper();
            auto t1 = std::chrono::steady_clock::now();
            double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            samples[s] = ns / itersPerSample;
        }
        std::sort(samples, samples + numSamples);
        std::printf("%.2f\n", samples[numSamples / 2]);
        return 0;
    }
)CPP";

/// Lazy-initialized shared state for a benchmark session.
/// Resolves clang++, creates a temp directory, and compiles the benchmark
/// stub once. Thread-safe via std::call_once. Cleaned up on process exit.
struct BenchSession {
    fs::path sessionDir;
    fs::path stubObjPath;
    std::string clangPath;
    std::vector<std::string> sysrootArgs;
    std::once_flag initFlag;
    bool valid = false;

    bool init() {
        std::call_once(initFlag, [this] { valid = doInit(); });
        return valid;
    }

    ~BenchSession() {
        if (!sessionDir.empty()) {
            std::error_code ec;
            fs::remove_all(sessionDir, ec);
        }
    }

private:
    bool doInit() {
        // Resolve clang++
        clangPath = platform::resolveLLVMTool("clang++");
        if (clangPath.empty()) {
            llvm::errs() << "topo: auto-benchmark disabled: "
                         << "clang++ not found\n";
            return false;
        }

        // Create session temp directory. `topo::platform::tempDirectory()`
        // honours TMPDIR / XDG_RUNTIME_DIR / TEMP / TMP and is portable to
        // Windows; the prior raw std-library call silently fell through
        // to its libstdc++ default on misconfigured CI.
        std::error_code ec;
        sessionDir = topo::platform::tempDirectory() / ("topo_bench_" + std::to_string(getpid()));
        fs::create_directories(sessionDir, ec);
        if (ec) return false;

        // macOS: resolve sysroot for bundled clang++
        if constexpr (platform::IsMacOS) {
            auto result = platform::runProcessCapture("xcrun", {"--show-sdk-path"});
            if (result.exitCode == 0 && !result.stdoutOutput.empty()) {
                std::string sdkPath = result.stdoutOutput;
                while (!sdkPath.empty() && (sdkPath.back() == '\n' || sdkPath.back() == '\r'))
                    sdkPath.pop_back();
                sysrootArgs.push_back("-isysroot");
                sysrootArgs.push_back(sdkPath);
            }
        }

        // Write and compile the benchmark stub (once per session)
        fs::path stubSrcPath = sessionDir / "bench_stub.cpp";
        {
            std::ofstream ofs(stubSrcPath);
            if (!ofs) return false;
            ofs << kBenchStubSource;
        }

        stubObjPath = sessionDir / ("bench_stub" + std::string(platform::ObjectFileSuffix));
        std::vector<std::string> compileArgs = {"-O2", "-std=c++17", "-c"};
        compileArgs.insert(compileArgs.end(), sysrootArgs.begin(), sysrootArgs.end());
        compileArgs.push_back(stubSrcPath.string());
        compileArgs.push_back("-o");
        compileArgs.push_back(stubObjPath.string());

        auto result = platform::runProcess(clangPath, compileArgs);
        if (result.exitCode != 0) {
            llvm::errs() << "topo: auto-benchmark disabled: "
                         << "failed to compile benchmark stub\n";
            return false;
        }

        return true;
    }
};

static BenchSession gBenchSession;

} // anonymous namespace

bool VariantBenchmark::isNativeTarget(const llvm::Module& module) {
    auto moduleTriple = llvm::Triple(module.getTargetTriple());
    if (moduleTriple.getArch() == llvm::Triple::UnknownArch) return true; // No triple specified — assume native
    auto hostTriple = llvm::Triple(llvm::sys::getDefaultTargetTriple());
    return moduleTriple.getArch() == hostTriple.getArch();
}

std::pair<std::unique_ptr<llvm::Module>, std::unique_ptr<llvm::LLVMContext>> VariantBenchmark::cloneModuleToNewContext(
    const llvm::Module& srcModule) {
    llvm::SmallVector<char, 0> buffer;
    llvm::raw_svector_ostream os(buffer);
    llvm::WriteBitcodeToFile(srcModule, os);

    auto newCtx = std::make_unique<llvm::LLVMContext>();
    auto memBuf = llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(buffer.data(), buffer.size()), "", false);
    auto moduleOrErr = llvm::parseBitcodeFile(*memBuf, *newCtx);
    if (!moduleOrErr) {
        llvm::consumeError(moduleOrErr.takeError());
        return {nullptr, nullptr};
    }
    return {std::move(*moduleOrErr), std::move(newCtx)};
}

/// AOT-compile a function into a self-contained benchmark executable,
/// run it as a subprocess with a 30-second timeout, and parse the
/// median timing result from its stdout.
std::optional<double> VariantBenchmark::compileAndMeasure(const llvm::Module& srcModule,
                                                          llvm::StringRef targetFuncName,
                                                          int warmup,
                                                          int iterations,
                                                          int optLevel,
                                                          std::optional<uint64_t> bufferElements) {
    // Initialize session (resolves clang++, compiles stub once)
    if (!gBenchSession.init()) return std::nullopt;

    auto& session = gBenchSession;

    // Clone module to new context.
    // Declare context before module so that module is destroyed first
    // (C++ destroys locals in reverse declaration order). Module's
    // destructor calls LLVMContext::removeModule, so context must outlive it.
    std::unique_ptr<llvm::LLVMContext> clonedCtx;
    std::unique_ptr<llvm::Module> mod;
    {
        auto pair = cloneModuleToNewContext(srcModule);
        mod = std::move(pair.first);
        clonedCtx = std::move(pair.second);
    }
    if (!mod) {
        llvm::errs() << "topo: remark: benchmark skipped for " << targetFuncName
                     << ": module clone failed\n";
        return std::nullopt;
    }

    // Create benchmark wrapper function in IR.
    // The wrapper must be named "__bench_wrapper" to match the stub's
    // extern "C" declaration, regardless of the wrapperName parameter.
    // When bufferElements is specified, compute buffer size in bytes.
    // We pass raw byte count; createBenchWrapper divides by element size
    // when a pointee type is available.
    std::optional<uint64_t> bufferSizeBytes;
    if (bufferElements) {
        // Use a conservative element size estimate (16 bytes) when we
        // cannot infer the actual type at this level. The wrapper's
        // inferPointeeType will refine this per-argument.
        bufferSizeBytes = *bufferElements * 16;
    }
    if (!createBenchWrapper(*mod, targetFuncName, "__bench_wrapper", bufferSizeBytes)) {
        llvm::errs() << "topo: remark: benchmark skipped for " << targetFuncName
                     << ": benchmark wrapper creation failed\n";
        return std::nullopt;
    }

    // Remove user's main() to avoid duplicate symbol with bench stub's main()
    if (auto* userMain = mod->getFunction("main")) userMain->eraseFromParent();

    // Apply LLVM optimizations (matches final AOT product's optimization level)
    runOptPipeline(*mod, optLevel);

    // Generate unique temp file paths for this benchmark call
    static std::atomic<int> counter{0};
    int id = counter.fetch_add(1);
    std::string prefix = "bench_" + std::to_string(id);

    fs::path bcPath = session.sessionDir / (prefix + ".bc");
    fs::path objPath = session.sessionDir / (prefix + std::string(platform::ObjectFileSuffix));
    fs::path exePath = session.sessionDir / (prefix + std::string(platform::ExeSuffix));

    // Write module bitcode to temp file
    {
        std::error_code ec;
        llvm::raw_fd_ostream os(bcPath.string(), ec);
        if (ec) {
            llvm::errs() << "topo: remark: benchmark skipped for " << targetFuncName
                         << ": failed to write bitcode to temp file\n";
            return std::nullopt;
        }
        llvm::WriteBitcodeToFile(*mod, os);
    }

    // Module no longer needed — release before context
    mod.reset();

    // Compile bitcode to object file
    {
        std::vector<std::string> args = {"-O2", "-c"};
        args.insert(args.end(), session.sysrootArgs.begin(), session.sysrootArgs.end());
        args.push_back(bcPath.string());
        args.push_back("-o");
        args.push_back(objPath.string());

        auto result = platform::runProcess(session.clangPath, args);
        if (result.exitCode != 0) {
            llvm::errs() << "topo: remark: benchmark skipped for " << targetFuncName
                         << ": bitcode compilation failed\n";
            std::error_code ec;
            fs::remove(bcPath, ec);
            return std::nullopt;
        }
    }

    // Link object with benchmark stub into executable.
    //
    // Transform passes (LifetimeArenaPass, TopoParallelPass, AdaptiveDispatchPass,
    // ObservabilityPass, ...) emit calls to topo runtime symbols. Static-link the
    // runtime libs so variant binaries resolve these; otherwise the link fails,
    // compileAndMeasure returns nullopt, and auto-mode sees "benchmark failed"
    // instead of "pass beneficial".
    {
        std::vector<std::string> args;
        args.insert(args.end(), session.sysrootArgs.begin(), session.sysrootArgs.end());
        args.push_back(objPath.string());
        args.push_back(session.stubObjPath.string());
#ifdef TOPO_BENCH_RUNTIME_LIBDIR
        args.push_back(std::string("-L") + TOPO_BENCH_RUNTIME_LIBDIR);
        // Order matters for static libs: callers first, callees last.
        // topo-adaptive → topo-jit → topo-parallel; topo-arena / topo-observe
        // have no inter-runtime deps.
        args.push_back("-ltopo-adaptive");
        args.push_back("-ltopo-jit");
        args.push_back("-ltopo-parallel");
        args.push_back("-ltopo-arena");
        args.push_back("-ltopo-observe");
        // AdaptiveDispatchPass-instrumented variants reference
        // topo_pass_event_emit. topo-adaptive (caller) is listed first, so
        // the pass-event archive (callee) must follow it for static-lib
        // symbol resolution.
        args.push_back("-ltopo-pass-event");
        args.push_back("-pthread");
#endif
        args.push_back("-o");
        args.push_back(exePath.string());

        auto result = platform::runProcess(session.clangPath, args);
        if (result.exitCode != 0) {
            llvm::errs() << "topo: remark: benchmark skipped for " << targetFuncName
                         << ": linking benchmark executable failed\n";
            std::error_code ec;
            fs::remove(bcPath, ec);
            fs::remove(objPath, ec);
            return std::nullopt;
        }
    }

    // Execute benchmark with timeout
    std::optional<double> median;
    {
        std::vector<std::string> args = {std::to_string(warmup), std::to_string(iterations)};

        auto result = platform::runProcessCaptureWithTimeout(exePath.string(), args, 30000);
        if (result.exitCode == 0 && !result.stdoutOutput.empty()) {
            char* end = nullptr;
            double val = std::strtod(result.stdoutOutput.c_str(), &end);
            if (end != result.stdoutOutput.c_str() && val > 0) median = val;
        }
    }

    // Clean up per-call temp files
    {
        std::error_code ec;
        fs::remove(bcPath, ec);
        fs::remove(objPath, ec);
        fs::remove(exePath, ec);
    }

    return median;
}

std::optional<BenchmarkResult> VariantBenchmark::run(llvm::Module& baseModule,
                                                     llvm::StringRef targetFuncName,
                                                     std::function<void(llvm::Module&)> applyVariant,
                                                     int warmup,
                                                     int iterations,
                                                     int optLevel) {
    if (!isNativeTarget(baseModule)) {
        llvm::errs() << "topo: remark: benchmark skipped: cross-compilation target\n";
        return std::nullopt;
    }

    // 1. Measure baseline
    auto baselineNs = compileAndMeasure(baseModule, targetFuncName, warmup, iterations, optLevel);
    if (!baselineNs) return std::nullopt;

    // 2. Clone module, apply variant transform.
    // Context declared before module so module is destroyed first.
    std::unique_ptr<llvm::LLVMContext> variantCtx;
    std::unique_ptr<llvm::Module> clonedMod;
    {
        auto pair = cloneModuleToNewContext(baseModule);
        clonedMod = std::move(pair.first);
        variantCtx = std::move(pair.second);
    }
    if (!clonedMod) {
        llvm::errs() << "topo: remark: benchmark skipped: variant module clone failed for "
                     << targetFuncName << "\n";
        return std::nullopt;
    }

    applyVariant(*clonedMod);

    // 3. Measure variant
    auto variantNs = compileAndMeasure(*clonedMod, targetFuncName, warmup, iterations, optLevel);
    if (!variantNs) return std::nullopt;

    if (*baselineNs <= 0 || *variantNs <= 0) {
        llvm::errs() << "topo: remark: benchmark skipped: invalid timing values for "
                     << targetFuncName << " (baseline=" << *baselineNs
                     << "ns, variant=" << *variantNs << "ns)\n";
        return std::nullopt;
    }

    // 4. Compare with 10% threshold
    double speedup = *baselineNs / *variantNs;
    BenchmarkResult::Winner winner = (speedup > 1.10) ? BenchmarkResult::Variant : BenchmarkResult::Baseline;

    return BenchmarkResult{winner, *baselineNs, *variantNs, speedup};
}

std::optional<BenchmarkResult> VariantBenchmark::run(llvm::Module& baseModule,
                                                     llvm::StringRef targetFuncName,
                                                     std::function<void(llvm::Module&)> applyVariant,
                                                     int warmup,
                                                     int iterations,
                                                     BaselineCache& cache,
                                                     int optLevel) {
    if (!isNativeTarget(baseModule)) {
        llvm::errs() << "topo: remark: benchmark skipped: cross-compilation target\n";
        return std::nullopt;
    }

    // 1. Measure baseline (or reuse cached)
    std::optional<double> baselineNs;
    std::string funcKey = targetFuncName.str();
    auto cacheIt = cache.find(funcKey);
    if (cacheIt != cache.end()) {
        baselineNs = cacheIt->second;
    } else {
        baselineNs = compileAndMeasure(baseModule, targetFuncName, warmup, iterations, optLevel);
        if (!baselineNs) return std::nullopt;
        cache[funcKey] = *baselineNs;
    }

    // 2. Clone module, apply variant transform.
    // Context declared before module so module is destroyed first.
    std::unique_ptr<llvm::LLVMContext> variantCtx;
    std::unique_ptr<llvm::Module> clonedMod;
    {
        auto pair = cloneModuleToNewContext(baseModule);
        clonedMod = std::move(pair.first);
        variantCtx = std::move(pair.second);
    }
    if (!clonedMod) {
        llvm::errs() << "topo: remark: benchmark skipped: variant module clone failed for "
                     << targetFuncName << "\n";
        return std::nullopt;
    }

    applyVariant(*clonedMod);

    // 3. Measure variant
    auto variantNs = compileAndMeasure(*clonedMod, targetFuncName, warmup, iterations, optLevel);
    if (!variantNs) return std::nullopt;

    if (*baselineNs <= 0 || *variantNs <= 0) {
        llvm::errs() << "topo: remark: benchmark skipped: invalid timing values for "
                     << targetFuncName << " (baseline=" << *baselineNs
                     << "ns, variant=" << *variantNs << "ns)\n";
        return std::nullopt;
    }

    double speedup = *baselineNs / *variantNs;
    BenchmarkResult::Winner winner = (speedup > 1.10) ? BenchmarkResult::Variant : BenchmarkResult::Baseline;

    return BenchmarkResult{winner, *baselineNs, *variantNs, speedup};
}

} // namespace topo
