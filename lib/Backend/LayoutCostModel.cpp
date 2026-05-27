#include "LayoutCostModel.h"

#include <cmath>
#include <sstream>

#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/Instructions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

namespace topo {

namespace {

/// Create a TargetMachine from the module's target triple.
/// Falls back to the host triple if the module has no triple set.
/// For cross-compilation, the target backend must be linked in (via LLVM
/// component libraries). If the target is unavailable, returns nullptr
/// and the caller falls back to conservative defaults.
static std::unique_ptr<llvm::TargetMachine> createTargetMachineForModule(llvm::Module& module) {
    static bool initialized = false;
    if (!initialized) {
        llvm::InitializeNativeTarget();
        initialized = true;
    }

    auto triple = module.getTargetTriple();
    if (triple.getArch() == llvm::Triple::UnknownArch) triple = llvm::Triple(llvm::sys::getDefaultTargetTriple());

    std::string error;
    auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) return nullptr;

    // Use "generic" CPU for cross-compilation to avoid host-specific features.
    // For native targets, use the host CPU for accurate TTI costs.
    std::string cpu = "generic";
    auto hostTriple = llvm::Triple(llvm::sys::getDefaultTargetTriple());
    if (triple.getArch() == hostTriple.getArch()) cpu = llvm::sys::getHostCPUName().str();

    return std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple, cpu, "", llvm::TargetOptions{}, std::nullopt));
}

/// Compute granular TTI costs for a function, broken down by instruction class.
struct FunctionCostDetail {
    uint64_t memoryCost = 0;
    uint64_t arithmeticCost = 0;
    uint64_t otherCost = 0;
};

static FunctionCostDetail computeFunctionTTICostDetailed(llvm::Function& func, llvm::TargetTransformInfo& TTI) {
    FunctionCostDetail detail;

    for (auto& BB : func) {
        for (auto& I : BB) {
            auto ic = TTI.getInstructionCost(&I, llvm::TargetTransformInfo::TCK_RecipThroughput);
            uint64_t cost = ic.isValid() ? static_cast<uint64_t>(ic.getValue()) : 1;

            if (llvm::isa<llvm::LoadInst>(&I) || llvm::isa<llvm::StoreInst>(&I)) {
                // Use getMemoryOpCost for more accurate memory cost modeling
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                    auto memCost = TTI.getMemoryOpCost(llvm::Instruction::Load,
                                                       load->getType(),
                                                       load->getAlign(),
                                                       load->getPointerAddressSpace(),
                                                       llvm::TargetTransformInfo::TCK_RecipThroughput);
                    cost = memCost.isValid() ? static_cast<uint64_t>(memCost.getValue()) : cost;
                } else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                    auto memCost = TTI.getMemoryOpCost(llvm::Instruction::Store,
                                                       store->getValueOperand()->getType(),
                                                       store->getAlign(),
                                                       store->getPointerAddressSpace(),
                                                       llvm::TargetTransformInfo::TCK_RecipThroughput);
                    cost = memCost.isValid() ? static_cast<uint64_t>(memCost.getValue()) : cost;
                }
                detail.memoryCost += cost;
            } else if (I.isBinaryOp() || I.isUnaryOp() || llvm::isa<llvm::CmpInst>(&I)) {
                // Use getArithmeticInstrCost for ALU operations
                if (I.isBinaryOp()) {
                    auto arithCost = TTI.getArithmeticInstrCost(
                        I.getOpcode(), I.getType(), llvm::TargetTransformInfo::TCK_RecipThroughput);
                    cost = arithCost.isValid() ? static_cast<uint64_t>(arithCost.getValue()) : cost;
                }
                detail.arithmeticCost += cost;
            } else {
                detail.otherCost += cost;
            }
        }
    }

    return detail;
}

/// Estimate cache line utilization for a function's memory accesses.
///
/// AoS pattern: accessing field F of struct S in array[N] touches a full
/// cache line per element (stride = sizeof(S)), but only sizeof(F) bytes
/// are useful. Utilization = sizeof(F) / sizeof(S), capped by cache line.
///
/// SoA pattern: accessing column F[N] touches contiguous sizeof(F) bytes.
/// Utilization approaches 1.0 for sequential access.
static double estimateCacheUtilization(llvm::Function& func,
                                       llvm::TargetTransformInfo& TTI,
                                       const llvm::DataLayout& DL,
                                       bool isSoA) {
    unsigned cacheLineSize = TTI.getCacheLineSize();
    if (cacheLineSize == 0) cacheLineSize = 64; // Conservative default

    uint64_t totalBytesAccessed = 0;
    uint64_t totalCacheLinesUsed = 0;

    for (auto& BB : func) {
        for (auto& I : BB) {
            llvm::Type* accessTy = nullptr;
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&I))
                accessTy = load->getType();
            else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&I))
                accessTy = store->getValueOperand()->getType();

            if (!accessTy || accessTy->isVoidTy()) continue;

            uint64_t accessSize = DL.getTypeStoreSize(accessTy);
            totalBytesAccessed += accessSize;

            // For SoA, sequential access means elements pack into cache lines.
            // For AoS, struct stride means each element may start a new line.
            if (isSoA) {
                // SoA: contiguous column access. Elements pack efficiently.
                // Each cache line holds cacheLineSize/accessSize elements.
                totalCacheLinesUsed += 1; // amortized: share cache line
            } else {
                // AoS: each field access potentially touches a new cache line
                // (stride = struct size > cache line for large structs).
                totalCacheLinesUsed += 1;
            }
        }
    }

    if (totalCacheLinesUsed == 0 || totalBytesAccessed == 0) return 0.5; // neutral default

    // SoA: high utilization because columns are contiguous
    // AoS: lower utilization because of struct stride waste
    if (isSoA) {
        // In SoA, the useful fraction of each cache line is high
        // because column elements are packed contiguously.
        return 0.9;
    }

    // AoS: estimate based on the average access size vs cache line.
    // If we access 4 bytes from a 64-byte cache line, utilization = 4/64 ~= 0.06.
    // But in practice, spatial locality from adjacent fields helps.
    // Use a heuristic: average_access_size / (cache_line_size / 2).
    double avgAccess = static_cast<double>(totalBytesAccessed) / static_cast<double>(totalCacheLinesUsed);
    double utilization = avgAccess / static_cast<double>(cacheLineSize);

    // Clamp to reasonable range
    return std::max(0.05, std::min(1.0, utilization));
}

/// Estimate the effective vector width for the target.
/// Returns the number of 32-bit elements that fit in the target's widest
/// vector register (e.g., 4 for SSE, 8 for AVX2, 16 for AVX-512).
static unsigned estimateVectorWidth(llvm::TargetTransformInfo& TTI) {
    // Query the register bit width for vector operations.
    // Use fixed-width vector type inquiry.
    auto regWidth = TTI.getRegisterBitWidth(llvm::TargetTransformInfo::RGK_FixedWidthVector);
    if (regWidth.isNonZero()) return regWidth.getFixedValue() / 32;

    return 4; // Conservative: assume SSE2 (128-bit)
}

/// Apply vectorization potential adjustment.
/// SoA benefits more from vectorization because columns are contiguous.
/// AoS requires gather instructions for vectorized field access.
static double applyVectorizationFactor(double cost, unsigned vectorWidth, bool isSoA) {
    if (vectorWidth <= 1) return cost;

    if (isSoA) {
        // SoA columns are contiguous: vector loads/stores are efficient.
        // Scale memory cost down by vector width (amortized).
        double factor = 1.0 / static_cast<double>(vectorWidth);
        // Blend: memory ops benefit fully, arithmetic partially.
        return cost * (0.3 + 0.7 * factor);
    }

    // AoS: vectorization requires gather/scatter, adding overhead.
    // Penalize based on gather cost (roughly 2-4x a contiguous load).
    return cost * 1.2;
}

/// Apply hint-based adjustment factors to a raw SoA cost.
///
/// Access pattern adjustments:
///   streaming     -> 0.7x  (SoA is cache-friendly for sequential access)
///   random        -> 1.5x  (SoA is cache-unfriendly for random access)
///   gather_scatter -> 2.0x  (SoA requires expensive gather/scatter)
///
/// Cardinality adjustments:
///   < 1k elements  -> 1.3x  (SoA overhead dominates at small sizes)
///   > 10k elements -> 0.8x  (SoA cache benefits at large sizes)
static double adjustSoACost(double rawCost, const std::optional<CardinalityHint>& hint, AccessPattern accessPattern) {
    double adjusted = rawCost;

    // Access pattern factor
    switch (accessPattern) {
    case AccessPattern::Streaming: adjusted *= 0.7; break;
    case AccessPattern::Random: adjusted *= 1.5; break;
    case AccessPattern::GatherScatter: adjusted *= 2.0; break;
    case AccessPattern::Tiled:
    case AccessPattern::None: break;
    }

    // Cardinality factor
    if (hint) {
        int64_t lo = hint->min > 0 ? hint->min : 1;
        int64_t hi = hint->max > 0 ? hint->max : lo;
        double representative = std::sqrt(static_cast<double>(lo) * static_cast<double>(hi));
        if (representative < 1000.0)
            adjusted *= 1.3;
        else if (representative > 10000.0)
            adjusted *= 0.8;
    }

    return adjusted;
}

/// Core TTI estimation implementation shared by both estimate() and
/// estimateWithBreakdown().
static std::optional<LayoutBenchmarkResult> estimateImpl(llvm::Module& module,
                                                         const LayoutVariantPair& variant,
                                                         LayoutCostBreakdown* breakdown,
                                                         std::optional<CardinalityHint> hint,
                                                         AccessPattern accessPattern) {
    auto TM = createTargetMachineForModule(module);
    if (!TM) return std::nullopt;

    // Both functions must exist and have bodies
    if (!variant.aosFn || variant.aosFn->isDeclaration()) return std::nullopt;
    if (!variant.soaFn || variant.soaFn->isDeclaration()) return std::nullopt;

    auto aosTTI = TM->getTargetTransformInfo(*variant.aosFn);
    auto soaTTI = TM->getTargetTransformInfo(*variant.soaFn);

    // Granular cost breakdown
    auto aosDetail = computeFunctionTTICostDetailed(*variant.aosFn, aosTTI);
    auto soaDetail = computeFunctionTTICostDetailed(*variant.soaFn, soaTTI);

    auto& DL = module.getDataLayout();
    double aosCacheUtil = estimateCacheUtilization(*variant.aosFn, aosTTI, DL, /*isSoA=*/false);
    double soaCacheUtil = estimateCacheUtilization(*variant.soaFn, soaTTI, DL, /*isSoA=*/true);

    unsigned vectorWidth = estimateVectorWidth(aosTTI);

    bool crossCompile = LayoutCostModel::isCrossCompilation(module);

    // Populate breakdown if requested
    if (breakdown) {
        breakdown->aosMemoryCost = aosDetail.memoryCost;
        breakdown->aosArithmeticCost = aosDetail.arithmeticCost;
        breakdown->aosOtherCost = aosDetail.otherCost;
        breakdown->soaMemoryCost = soaDetail.memoryCost;
        breakdown->soaArithmeticCost = soaDetail.arithmeticCost;
        breakdown->soaOtherCost = soaDetail.otherCost;
        breakdown->aosCacheUtilization = aosCacheUtil;
        breakdown->soaCacheUtilization = soaCacheUtil;
        breakdown->targetVectorWidth = vectorWidth;
        breakdown->isCrossCompilation = crossCompile;
    }

    // Compute weighted costs incorporating cache utilization and vectorization.
    //
    // Memory cost is scaled inversely by cache utilization: poor utilization
    // means more cache line fetches for the same logical accesses.
    double aosMemWeighted = static_cast<double>(aosDetail.memoryCost);
    if (aosCacheUtil > 0) aosMemWeighted /= aosCacheUtil;
    double soaMemWeighted = static_cast<double>(soaDetail.memoryCost);
    if (soaCacheUtil > 0) soaMemWeighted /= soaCacheUtil;

    double aosCost =
        aosMemWeighted + static_cast<double>(aosDetail.arithmeticCost) + static_cast<double>(aosDetail.otherCost);
    double soaCostRaw =
        soaMemWeighted + static_cast<double>(soaDetail.arithmeticCost) + static_cast<double>(soaDetail.otherCost);

    // Apply vectorization potential
    aosCost = applyVectorizationFactor(aosCost, vectorWidth, /*isSoA=*/false);
    soaCostRaw = applyVectorizationFactor(soaCostRaw, vectorWidth, /*isSoA=*/true);

    if (aosCost <= 0 || soaCostRaw <= 0) return std::nullopt;

    double soaCostAdjusted = adjustSoACost(soaCostRaw, hint, accessPattern);

    // Conservative threshold: SoA must be >30% cheaper to win
    // (higher bar than runtime benchmark since static analysis is less precise)
    double speedup = aosCost / soaCostAdjusted;
    LayoutBenchmarkResult::Winner winner = (speedup > 1.30) ? LayoutBenchmarkResult::SoA : LayoutBenchmarkResult::AoS;

    return LayoutBenchmarkResult{winner, aosCost, soaCostAdjusted, speedup};
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

bool LayoutCostModel::isCrossCompilation(const llvm::Module& module) {
    auto moduleTriple = llvm::Triple(module.getTargetTriple());
    if (moduleTriple.getArch() == llvm::Triple::UnknownArch) return false; // No triple specified: assume native
    auto hostTriple = llvm::Triple(llvm::sys::getDefaultTargetTriple());
    return moduleTriple.getArch() != hostTriple.getArch();
}

std::optional<LayoutBenchmarkResult> LayoutCostModel::estimate(llvm::Module& module,
                                                               const LayoutVariantPair& variant,
                                                               std::optional<CardinalityHint> hint,
                                                               AccessPattern accessPattern) {
    return estimateImpl(module, variant, /*breakdown=*/nullptr, hint, accessPattern);
}

std::optional<LayoutBenchmarkResult> LayoutCostModel::estimateWithBreakdown(llvm::Module& module,
                                                                            const LayoutVariantPair& variant,
                                                                            LayoutCostBreakdown& breakdown,
                                                                            std::optional<CardinalityHint> hint,
                                                                            AccessPattern accessPattern) {
    return estimateImpl(module, variant, &breakdown, hint, accessPattern);
}

std::string LayoutCostBreakdown::formatRemark() const {
    std::ostringstream os;
    os << "DataLayout TTI cost analysis";
    if (isCrossCompilation) os << " (cross-compilation fallback)";
    os << ":\n";

    os << "  AoS: memory=" << aosMemoryCost << " arith=" << aosArithmeticCost << " other=" << aosOtherCost
       << " total=" << aosTotalCost() << " cache_util=" << static_cast<int>(aosCacheUtilization * 100) << "%\n";

    os << "  SoA: memory=" << soaMemoryCost << " arith=" << soaArithmeticCost << " other=" << soaOtherCost
       << " total=" << soaTotalCost() << " cache_util=" << static_cast<int>(soaCacheUtilization * 100) << "%\n";

    os << "  target_vector_width=" << targetVectorWidth << " (x32-bit lanes)";

    return os.str();
}

} // namespace topo
