#ifndef TOPO_BACKEND_VARIANTBENCHMARK_H
#define TOPO_BACKEND_VARIANTBENCHMARK_H

#include <llvm/IR/Module.h>

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace topo {

struct BenchmarkResult {
    enum Winner { Baseline, Variant };
    Winner winner;
    double baselineMedianNs;
    double variantMedianNs;
    double speedup; // baseline/variant ratio (>1 means variant is faster)
};

class VariantBenchmark {
public:
    /// Benchmark a transformation against the baseline.
    /// 1. AOT-compile and measure the original function (baseline)
    /// 2. Clone module, apply `applyVariant`, AOT-compile and measure
    /// 3. Return winner (variant must be >10% faster to win)
    /// Returns nullopt on cross-compilation or compilation failure.
    ///
    /// optLevel: LLVM optimization level to apply before compilation (default O0 = no opt).
    /// Use O2 for passes that only add metadata (e.g., LoopParallelizePass).
    static std::optional<BenchmarkResult> run(llvm::Module& baseModule,
                                              llvm::StringRef targetFuncName,
                                              std::function<void(llvm::Module&)> applyVariant,
                                              int warmup,
                                              int iterations,
                                              int optLevel = 2);

    /// Map from function name to measured baseline median ns.
    using BaselineCache = std::unordered_map<std::string, double>;

    /// Same as run(), but reuses a cached baseline measurement if available.
    /// If the baseline for targetFuncName is in `cache`, skips the baseline measurement.
    /// Otherwise, measures it and stores in `cache` for future reuse.
    static std::optional<BenchmarkResult> run(llvm::Module& baseModule,
                                              llvm::StringRef targetFuncName,
                                              std::function<void(llvm::Module&)> applyVariant,
                                              int warmup,
                                              int iterations,
                                              BaselineCache& cache,
                                              int optLevel = 2);

    // --- Public utility functions (reused by LayoutBenchmark) ---

    /// Check if module's target triple matches host.
    static bool isNativeTarget(const llvm::Module& module);

    /// Serialize module to bitcode, parse in fresh LLVMContext.
    static std::pair<std::unique_ptr<llvm::Module>, std::unique_ptr<llvm::LLVMContext>> cloneModuleToNewContext(
        const llvm::Module& srcModule);

    /// AOT-compile a function, benchmark it in a subprocess, return median ns.
    /// Compiles the module to an executable with an embedded timing harness,
    /// runs it as a subprocess with a 30-second timeout.
    /// When bufferElements is provided, the benchmark wrapper allocates
    /// bufferElements worth of data instead of the fixed 256KB default.
    static std::optional<double> compileAndMeasure(const llvm::Module& srcModule,
                                                   llvm::StringRef targetFuncName,
                                                   int warmup,
                                                   int iterations,
                                                   int optLevel = 2,
                                                   std::optional<uint64_t> bufferElements = std::nullopt);
};

} // namespace topo

#endif // TOPO_BACKEND_VARIANTBENCHMARK_H
