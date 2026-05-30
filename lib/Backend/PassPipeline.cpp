#include "topo/Backend/PassPipeline.h"
#include "LayoutBenchmark.h"
#include "LayoutCostModel.h"
#include "VariantBenchmark.h"
#include "topo/Build/PassCategoryRegistry.h"
#include "topo/Transforms/AdaptiveDispatchPass.h"
#include "topo/Transforms/ContainmentInterceptionPass.h"
#include "topo/Transforms/DataLayoutPass.h"
#include "topo/Transforms/IndirectionPass.h"
#include "topo/Transforms/ObservabilityPass.h"
#include "topo/Transforms/PipelineCodeGenPass.h"
#include "topo/Transforms/LifetimeArenaPass.h"
#include "topo/Transforms/ReturnSpecializationPass.h"
#include "topo/Transforms/TopoFlattenPass.h"
#include "topo/Transforms/TopoInlinePass.h"
#include "topo/Transforms/TopoLayoutPass.h"
#include "topo/Transforms/TopoParallelPass.h"
#include "topo/Transforms/LoopParallelizePass.h"
#include "topo/Transforms/PassFiredMarker.h"
#include "topo/Transforms/PrefetchPass.h"
#include "topo/Transforms/TopoReorderPass.h"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/Scalar/SROA.h>

#include <chrono>
#include <cstdlib>
#include <memory>

namespace topo {

static llvm::OptimizationLevel toPassBuilderLevel(OptLevel level) {
    switch (level) {
    case OptLevel::O0: return llvm::OptimizationLevel::O0;
    case OptLevel::O1: return llvm::OptimizationLevel::O1;
    case OptLevel::O2: return llvm::OptimizationLevel::O2;
    case OptLevel::O3: return llvm::OptimizationLevel::O3;
    }
    return llvm::OptimizationLevel::O2;
}

/// Create a TargetMachine from the module's triple so PassBuilder gets
/// accurate cost models (inline thresholds, vector widths, etc.).
static std::unique_ptr<llvm::TargetMachine> createTargetMachine(llvm::Module& module) {
    static bool initialized = false;
    if (!initialized) {
        llvm::InitializeNativeTarget();
        initialized = true;
    }

    auto triple = module.getTargetTriple();
    if (triple.empty()) return nullptr;

    std::string error;
    auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) return nullptr;

    return std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple, "generic", "", llvm::TargetOptions{}, std::nullopt));
}

// Returns void by design: PassManager::run does not report failure (it
// reports analysis preservation, not status — pipeline failures surface
// via llvm::report_fatal_error / abort, not a return code). The previous
// `bool` signature implied an error channel that never existed; the
// function unconditionally returned true and both callers ignored the
// value. The audit finding renamed
// (topo-llvm-runstandardpipeline-discards-status) called this out.
//
// One real degradation mode lives here and is now surfaced explicitly:
// createTargetMachine(module) returns nullptr when the module's triple
// is empty or its target backend is not registered. The pipeline still
// runs, but PassBuilder receives a null TargetMachine and the resulting
// cost model loses target-specific inline thresholds / vector widths.
// We log a one-line warning to llvm::errs so the operator sees the
// degradation instead of it being silently rolled into "everything
// looks fine."
static void runStandardPipeline(llvm::Module& module, OptLevel level, BuildMode mode) {
    auto tm = createTargetMachine(module);

    if (!tm) {
        llvm::errs() << "topo: standard pipeline running WITHOUT target machine "
                        "(triple='" << module.getTargetTriple().str()
                     << "') — cost model degraded to PassBuilder defaults\n";
    } else {
        // Sync data layout so cost queries match the real target
        module.setDataLayout(tm->createDataLayout());
    }

    llvm::PassBuilder pb(tm.get());

    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    llvm::ModulePassManager mpm;
    if (mode == BuildMode::Aggressive) {
        mpm = pb.buildThinLTODefaultPipeline(toPassBuilderLevel(level), nullptr);
    } else {
        mpm = pb.buildPerModuleDefaultPipeline(toPassBuilderLevel(level));
    }
    mpm.run(module, mam);
    // Return value (PreservedAnalyses) intentionally discarded — it is
    // not a status, and downstream Topo passes don't consume the
    // preservation set.
}

bool PassPipeline::run(llvm::Module& module, OptLevel level) {
    if (level == OptLevel::O0) return true;
    runStandardPipeline(module, level, BuildMode::Dev);
    return true;
}

bool PassPipeline::run(llvm::Module& module,
                       OptLevel level,
                       const PassPipelineConfig& cfg) {
    // Unpack — the body below was authored against the 19-parameter
    // signature and references each config by its short name. Aliasing
    // here keeps the body unchanged across the refactor.
    const auto* entries              = cfg.entries;
    const auto* mapping              = cfg.mapping;
    const auto* symbols              = cfg.symbols;
    const auto  mode                 = cfg.mode;
    const auto* parallelCfg          = cfg.parallelCfg;
    // Reception slot for topo_cost_* runtime-sampling results:
    // BackendRequest.runtimeCosts is designed to feed auto-mode pass
    // decisions, but that consumer is not yet wired (every caller
    // currently passes nullptr), so the parameter is intentionally
    // unread for now — keep the slot, silence the warning.
    [[maybe_unused]] const auto* runtimeCosts = cfg.runtimeCosts;
    const auto* adaptiveCfg          = cfg.adaptiveCfg;
    const auto* dataLayoutCfg        = cfg.dataLayoutCfg;
    const auto* indirectionCfg       = cfg.indirectionCfg;
    const bool  indirectionExplicit  = cfg.indirectionExplicit;
    const auto* observabilityCfg     = cfg.observabilityCfg;
    const auto* lifetimeCfg          = cfg.lifetimeCfg;
    const auto* loopParallelCfg      = cfg.loopParallelCfg;
    const auto* prefetchCfg          = cfg.prefetchCfg;
    const auto* inlineCfg            = cfg.inlineCfg;
    const auto* pipelineCfg          = cfg.pipelineCfg;
    const auto* containmentCfg       = cfg.containmentCfg;
    auto*       reports              = cfg.reports;

    if (level == OptLevel::O0) return true;

    // Track auto-mode pass decisions for summary diagnostic
    int passesApplied = 0, passesSkippedNoBenefit = 0, passesSkippedBenchFail = 0;

    // Helper: stamp a PassReportHeader with name, category, and the count
    // recorded by markPassFired. Decision / reason are filled by each Pass's
    // own block. Skips entirely when no report sink is provided.
    auto stampHeader = [](backend::PassReportHeader& h, const char* name,
                          int firedCount, std::string decision,
                          std::string reason, int64_t elapsedNs) {
        h.passName = name;
        if (auto cat = topo::categoryOf(name)) h.category = topo::toString(*cat);
        h.fired = firedCount > 0;
        h.firedCount = firedCount;
        h.decision = std::move(decision);
        h.reason = std::move(reason);
        h.elapsedNs = elapsedNs;
    };

    // Create TargetMachine for TTI queries (used by TopoParallelPass)
    auto tm = createTargetMachine(module);

    // Step 1: Custom Topo passes (before standard optimization)
    if (entries && mapping && symbols) {
        // Containment interception (runtime enforcement of .topo containment).
        // Scans non-external functions for calls to restricted APIs (file /
        // network / process / etc.) and inserts
        // `__topo_containment_violation(caller, callee)` before each.  Must
        // run before any physical inlining so that declared-external
        // callees are still distinct and not yet absorbed into user code —
        // otherwise a user function that lawfully delegates a restricted
        // API to an external adapter would be flagged after inlining.
        // Gated on `[containment].mode`; default Off for backwards
        // compatibility (topo-containment runtime link is opt-in via
        // AutoLink.h's containmentCfg branch).
        if (containmentCfg && containmentCfg->isEnabled()) {
            auto t0 = std::chrono::steady_clock::now();
            auto* ciReport = reports ? &reports->containmentInterception : nullptr;
            int n = ContainmentInterceptionPass::run(module, *symbols, *mapping, ciReport);
            markPassFired(module, "ContainmentInterceptionPass", static_cast<unsigned>(n));
            if (reports) {
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                stampHeader(reports->containmentInterception.header,
                            "ContainmentInterceptionPass", n,
                            n > 0 ? "intercepted" : "no_targets",
                            n > 0 ? "wrapped restricted-API call sites in __topo_containment_violation"
                                  : "no calls to restricted APIs",
                            static_cast<int64_t>(elapsedNs));
            }
        } else if (reports) {
            stampHeader(reports->containmentInterception.header,
                        "ContainmentInterceptionPass", 0, "disabled",
                        containmentCfg ? "[containment].mode = off"
                                       : "no [containment] config",
                        0);
        }

        // Generate pipeline function bodies (creates new call sites).
        // Gate on [pipeline].mode: default Auto runs when any pipeline
        // logic block exists; `off` skips entirely so base benchmarks
        // see the untransformed DAG.  Older callers that don't pass a
        // PipelineConfig get the previous always-on behaviour.
        bool runPipelineCodeGen = !pipelineCfg || pipelineCfg->isEnabled();
        if (runPipelineCodeGen) {
            int n = PipelineCodeGenPass::run(module, *symbols, *mapping);
            markPassFired(module, "PipelineCodeGenPass", static_cast<unsigned>(n));
        }

        // Dead field elimination on sret/direct struct returns
        {
            auto t0 = std::chrono::steady_clock::now();
            auto* rsReport = reports ? &reports->returnSpecialization : nullptr;
            int n = ReturnSpecializationPass::run(module, *entries, *mapping, symbols, rsReport);
            markPassFired(module, "ReturnSpecializationPass", static_cast<unsigned>(n));
            if (reports) {
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                stampHeader(reports->returnSpecialization.header,
                            "ReturnSpecializationPass", n,
                            n > 0 ? "eliminated" : "no_eliminations",
                            n > 0 ? "dead return-struct fields removed"
                                  : "no unused fields on sret/direct returns",
                            static_cast<int64_t>(elapsedNs));
            }
        }

        // Inline private/protected callees (before DataLayout so GEP patterns
        // are already exposed without DataLayoutPass needing its own inlining).
        // With symbols, pipeline functor callees are forced alwaysinline.
        {
            auto t0 = std::chrono::steady_clock::now();
            auto* tiReport = reports ? &reports->topoInline : nullptr;
            int n = TopoInlinePass::run(module, level, *entries, *mapping, symbols, inlineCfg, tiReport);
            markPassFired(module, "TopoInlinePass", static_cast<unsigned>(n));
            if (reports) {
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                stampHeader(reports->topoInline.header, "TopoInlinePass", n,
                            n > 0 ? "marked_alwaysinline" : "no_inlines",
                            n > 0 ? "private / protected / pipeline-functor callees marked alwaysinline"
                                  : "no callees eligible for forced inlining",
                            static_cast<int64_t>(elapsedNs));
            }
        }

        // Insert software prefetch BEFORE AlwaysInliner: functions with
        // access(streaming)/access(tiled) are still separate entities here,
        // so their loops are visible and the inserted llvm.prefetch intrinsic
        // survives inlining (it rides along in the inlined loop body).
        // Running this after AlwaysInliner would mean the functions no longer
        // exist in mapping.matched, and PrefetchPass would silently no-op.
        if (prefetchCfg && prefetchCfg->isEnabled()) {
            auto t0 = std::chrono::steady_clock::now();
            auto* prReport = reports ? &reports->prefetch : nullptr;
            int n = PrefetchPass::run(module, *symbols, *mapping, *prefetchCfg, prReport);
            markPassFired(module, "PrefetchPass", static_cast<unsigned>(n));
            if (reports) {
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                stampHeader(reports->prefetch.header, "PrefetchPass", n,
                            n > 0 ? "inserted" : "no_streaming_loops",
                            n > 0 ? "llvm.prefetch intrinsics inserted at streaming/tiled loop heads"
                                  : "no functions with access(streaming|tiled) attribute",
                            static_cast<int64_t>(elapsedNs));
            }
        } else if (reports) {
            stampHeader(reports->prefetch.header, "PrefetchPass", 0, "disabled",
                        prefetchCfg ? "[prefetch].mode = off"
                                    : "no [prefetch] config",
                        0);
        }

        // Run LLVM's AlwaysInliner to physically inline functions marked
        // alwaysinline by TopoInlinePass, then SROA to promote allocas to SSA.
        // This exposes the GEP patterns needed by DataLayoutPass.
        {
            llvm::ModulePassManager miniMPM;
            miniMPM.addPass(llvm::AlwaysInlinerPass());

            llvm::FunctionPassManager fpm;
            fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
            miniMPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));

            auto miniTM = createTargetMachine(module);
            llvm::PassBuilder miniPB(miniTM.get());
            llvm::LoopAnalysisManager miniLAM;
            llvm::FunctionAnalysisManager miniFAM;
            llvm::CGSCCAnalysisManager miniCGAM;
            llvm::ModuleAnalysisManager miniMAM;
            miniPB.registerModuleAnalyses(miniMAM);
            miniPB.registerCGSCCAnalyses(miniCGAM);
            miniPB.registerFunctionAnalyses(miniFAM);
            miniPB.registerLoopAnalyses(miniLAM);
            miniPB.crossRegisterProxies(miniLAM, miniFAM, miniCGAM, miniMAM);

            miniMPM.run(module, miniMAM);
        }

        // Data layout optimization for pipeline arrays
        if (dataLayoutCfg && dataLayoutCfg->isEnabled()) {
            auto dataLayoutStart = std::chrono::steady_clock::now();
            int firedCount = 0;
            std::string decision;
            std::string reason;

            if (dataLayoutCfg->mode == FeatureMode::Auto) {
                // Auto-select: generate AoS+SoA variants, benchmark, pick winner
                auto variants = DataLayoutPass::generateVariants(module, *symbols, *mapping, *dataLayoutCfg);
                unsigned soaAccepted = 0;
                for (auto& variant : variants) {
                    // Extract dominant cardinality hint and access pattern
                    // from the pipeline's called functions
                    std::optional<CardinalityHint> hint;
                    AccessPattern accessPat = AccessPattern::None;
                    auto* lb = symbols->findLogicBlock(variant.pipelineName);
                    if (lb) {
                        for (const auto& cf : lb->calledFunctions) {
                            auto* fnSym = symbols->findFunction(cf);
                            if (fnSym) {
                                if (!hint && fnSym->cardinality) hint = fnSym->cardinality;
                                if (accessPat == AccessPattern::None && fnSym->accessPattern != AccessPattern::None)
                                    accessPat = fnSym->accessPattern;
                            }
                        }
                    }
                    auto result = LayoutBenchmark::run(module,
                                                       variant,
                                                       dataLayoutCfg->benchmarkWarmup,
                                                       dataLayoutCfg->benchmarkIterations,
                                                       hint,
                                                       accessPat);

                    // Emit optimization remark with TTI cost breakdown
                    // for cross-compilation targets where static analysis
                    // was used instead of runtime benchmarking.
                    if (LayoutCostModel::isCrossCompilation(module)) {
                        LayoutCostBreakdown breakdown;
                        LayoutCostModel::estimateWithBreakdown(module, variant, breakdown, hint, accessPat);
                        auto remarkStr = breakdown.formatRemark();
                        if (result) {
                            remarkStr += "\n  decision=" +
                                         std::string(result->winner == LayoutBenchmarkResult::SoA ? "SoA" : "AoS") +
                                         " speedup=" + std::to_string(result->speedup);
                        }
                        llvm::errs() << "remark: " << variant.pipelineName << ": " << remarkStr << "\n";
                    }

                    // Record this candidate in the report (auto mode).
                    if (reports) {
                        backend::DataLayoutCandidate c;
                        c.pipelineName = variant.pipelineName;
                        if (result) {
                            c.baselineNs = static_cast<int64_t>(result->aosMedianNs);
                            c.variantNs = static_cast<int64_t>(result->soaMedianNs);
                            c.speedup = result->speedup;
                            c.winner = (result->winner == LayoutBenchmarkResult::SoA) ? "soa" : "aos";
                            c.applied = (result->winner == LayoutBenchmarkResult::SoA);
                        } else {
                            c.winner = "benchmark_failed";
                            c.applied = false;
                        }
                        reports->dataLayout.candidates.push_back(std::move(c));
                    }

                    if (result && result->winner == LayoutBenchmarkResult::SoA) {
                        // SoA wins: replace original with SoA variant
                        variant.aosFn->replaceAllUsesWith(variant.soaFn);
                        variant.soaFn->takeName(variant.aosFn);
                        variant.aosFn->eraseFromParent();
                        ++soaAccepted;
                    } else {
                        // AoS wins or benchmark failed: discard SoA clone
                        variant.soaFn->eraseFromParent();
                    }
                }
                markPassFired(module, "DataLayoutPass", soaAccepted);
                firedCount = static_cast<int>(soaAccepted);
                if (variants.empty()) {
                    decision = "no_candidates";
                    reason = "no pipeline with topo::array<Struct, N> found";
                } else if (soaAccepted == variants.size()) {
                    decision = "auto_soa";
                    reason = "benchmark selected SoA for every candidate";
                } else if (soaAccepted == 0) {
                    decision = "auto_aos";
                    reason = "benchmark kept AoS for every candidate";
                } else {
                    decision = "auto_mixed";
                    reason = "benchmark selected SoA for " + std::to_string(soaAccepted) +
                             " of " + std::to_string(variants.size()) + " candidates";
                }
            } else if (dataLayoutCfg->mode == FeatureMode::Force) {
                int n = DataLayoutPass::runForceSoA(module, *dataLayoutCfg);
                markPassFired(module, "DataLayoutPass", static_cast<unsigned>(n));
                firedCount = n;
                decision = "forced_soa";
                reason = "[optimize.data-layout].mode = force";
                if (reports) {
                    // Force mode runs DataLayoutPass globally on every
                    // qualifying topo::array — per-array detail isn't
                    // currently surfaced by runForceSoA. Record the
                    // aggregate count as a single synthetic candidate so the
                    // sidecar still shows what fired.
                    backend::DataLayoutCandidate c;
                    c.pipelineName = "<module-wide>";
                    c.winner = "forced_soa";
                    c.applied = n > 0;
                    reports->dataLayout.candidates.push_back(std::move(c));
                }
            }

            if (reports) {
                auto elapsed = std::chrono::steady_clock::now() - dataLayoutStart;
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
                stampHeader(reports->dataLayout.header, "DataLayoutPass",
                            firedCount, std::move(decision), std::move(reason),
                            static_cast<int64_t>(elapsedNs));
            }
        } else if (reports) {
            stampHeader(reports->dataLayout.header, "DataLayoutPass", 0,
                        "disabled",
                        dataLayoutCfg ? "[optimize.data-layout].mode = off"
                                      : "no [optimize.data-layout] config",
                        0);
        }

        // Shared baseline cache across IndirectionPass, TopoParallelPass, and
        // AdaptiveDispatchPass auto-mode benchmarks. These passes benchmark the
        // same pipeline functions; caching avoids redundant baseline measurements.
        VariantBenchmark::BaselineCache baselineCache;

        // Indirect access optimization (after inline: smart ptr bodies visible)
        if (indirectionCfg && indirectionCfg->isEnabled()) {
            auto t0 = std::chrono::steady_clock::now();
            std::string decisionStr;
            std::string reasonStr;
            IndirectionStats finalStats;
            int firedCount = 0;
            if (indirectionCfg->mode == FeatureMode::Auto) {
                bool beneficial = false;
                bool anyBenchmarkSucceeded = false;
                for (const auto& [name, lb] : symbols->logicBlocks()) {
                    if (!lb.isPipeline) continue;
                    auto it = mapping->matched.find(lb.qualifiedName);
                    if (it == mapping->matched.end() || !it->second) continue;
                    std::string targetFunc = it->second->getName().str();
                    auto result = VariantBenchmark::run(
                        module,
                        targetFunc,
                        [&](llvm::Module& clone) {
                            auto rebound = mapping->rebind(clone);
                            IndirectionPass::run(clone, *entries, rebound, *symbols, *indirectionCfg);
                        },
                        indirectionCfg->benchmarkWarmup,
                        indirectionCfg->benchmarkIterations,
                        baselineCache);
                    if (result) {
                        anyBenchmarkSucceeded = true;
                        if (result->winner == BenchmarkResult::Variant) {
                            beneficial = true;
                            break;
                        }
                    }
                }
                if (beneficial) {
                    finalStats = IndirectionPass::run(module, *entries, *mapping, *symbols, *indirectionCfg);
                    markPassFired(module, "IndirectionPass", static_cast<unsigned>(finalStats.total()));
                    ++passesApplied;
                    decisionStr = "auto_applied";
                    reasonStr = "benchmark showed indirection optimisations beneficial";
                } else if (anyBenchmarkSucceeded) {
                    ++passesSkippedNoBenefit;
                    decisionStr = "auto_skipped_no_benefit";
                    reasonStr = "benchmark showed no improvement from indirection optimisations";
                } else {
                    ++passesSkippedBenchFail;
                    decisionStr = "auto_skipped_bench_fail";
                    reasonStr = "benchmark could not run to completion";
                }
                firedCount = finalStats.total();
            } else {
                finalStats = IndirectionPass::run(module, *entries, *mapping, *symbols, *indirectionCfg);
                markPassFired(module, "IndirectionPass", static_cast<unsigned>(finalStats.total()));
                firedCount = finalStats.total();
                decisionStr = firedCount > 0 ? "forced_applied" : "forced_no_targets";
                reasonStr = "[optimize.indirection].mode = force";
            }
            if (reports) {
                auto& r = reports->indirection;
                r.uniquePtrPromoted = finalStats.uniquePtrPromoted;
                r.sharedPtrOptimized = finalStats.sharedPtrOptimized;
                r.sharedPtrDereferenced = finalStats.sharedPtrDereferenced;
                r.refcountEliminated = finalStats.refcountEliminated;
                r.vectorLowered = finalStats.vectorLowered;
                r.pointerAttrsAdded = finalStats.pointerAttrsAdded;
                r.callsDevirtualized = finalStats.callsDevirtualized;
                r.vtableConstantsAnnotated = finalStats.vtableConstantsAnnotated;
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                stampHeader(r.header, "IndirectionPass", firedCount,
                            std::move(decisionStr), std::move(reasonStr),
                            static_cast<int64_t>(elapsedNs));
            }
        } else if (!indirectionExplicit) {
            // User hasn't configured [optimize.indirection] → scan and warn
            IndirectionPass::diagnose(module, *entries, *mapping, *symbols);
            if (reports) {
                stampHeader(reports->indirection.header, "IndirectionPass", 0,
                            "diagnostic_only",
                            "no [optimize.indirection] config — Pass ran in diagnose-only mode",
                            0);
            }
        } else if (reports) {
            stampHeader(reports->indirection.header, "IndirectionPass", 0,
                        "disabled", "[optimize.indirection].mode = off", 0);
        }

        // Auto-parallelize pipeline stages (after inline, before flatten)
        if (parallelCfg && parallelCfg->isEnabled()) {
            auto t0 = std::chrono::steady_clock::now();
            int firedCount = 0;
            std::string decisionStr;
            std::string reasonStr;
            if (parallelCfg->mode == FeatureMode::Auto) {
                bool beneficial = false;
                bool anyBenchmarkSucceeded = false;
                for (const auto& [name, lb] : symbols->logicBlocks()) {
                    if (!lb.isPipeline) continue;
                    auto it = mapping->matched.find(lb.qualifiedName);
                    if (it == mapping->matched.end() || !it->second) continue;
                    std::string targetFunc = it->second->getName().str();
                    auto result = VariantBenchmark::run(
                        module,
                        targetFunc,
                        [&](llvm::Module& clone) {
                            auto rebound = mapping->rebind(clone);
                            TopoParallelPass::run(clone, *symbols, rebound, *parallelCfg);
                        },
                        parallelCfg->benchmarkWarmup,
                        parallelCfg->benchmarkIterations,
                        baselineCache);
                    if (reports && result) {
                        backend::TopoParallelCandidate c;
                        c.pipelineName = name;
                        c.baselineNs = static_cast<int64_t>(result->baselineMedianNs);
                        c.variantNs = static_cast<int64_t>(result->variantMedianNs);
                        c.speedup = result->speedup;
                        c.winner = (result->winner == BenchmarkResult::Variant) ? "parallel" : "serial";
                        c.applied = false; // set below once the global decision is made
                        reports->topoParallel.candidates.push_back(std::move(c));
                    }
                    if (result) {
                        anyBenchmarkSucceeded = true;
                        if (result->winner == BenchmarkResult::Variant) {
                            beneficial = true;
                            break;
                        }
                    }
                }
                if (beneficial) {
                    int n = TopoParallelPass::run(module, *symbols, *mapping, *parallelCfg);
                    markPassFired(module, "TopoParallelPass", static_cast<unsigned>(n));
                    ++passesApplied;
                    firedCount = n;
                    decisionStr = "auto_parallel";
                    reasonStr = "benchmark showed parallelisation beneficial";
                    if (reports) {
                        for (auto& c : reports->topoParallel.candidates)
                            if (c.winner == "parallel") c.applied = true;
                    }
                } else if (anyBenchmarkSucceeded) {
                    ++passesSkippedNoBenefit;
                    decisionStr = "auto_skipped_no_benefit";
                    reasonStr = "benchmark showed no improvement from parallelisation";
                } else {
                    ++passesSkippedBenchFail;
                    decisionStr = "auto_skipped_bench_fail";
                    reasonStr = "benchmark could not run to completion";
                }
            } else {
                int n = TopoParallelPass::run(module, *symbols, *mapping, *parallelCfg);
                markPassFired(module, "TopoParallelPass", static_cast<unsigned>(n));
                firedCount = n;
                decisionStr = n > 0 ? "forced_applied" : "forced_no_targets";
                reasonStr = "[parallel].mode = force";
                if (reports && n > 0) {
                    // No benchmark data in force mode — record one candidate per
                    // pipeline so consumers see the per-pipeline applied set.
                    for (const auto& [name, lb] : symbols->logicBlocks()) {
                        if (!lb.isPipeline) continue;
                        backend::TopoParallelCandidate c;
                        c.pipelineName = name;
                        c.winner = "forced";
                        c.applied = true;
                        reports->topoParallel.candidates.push_back(std::move(c));
                    }
                }
            }
            if (reports) {
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                stampHeader(reports->topoParallel.header, "TopoParallelPass", firedCount,
                            std::move(decisionStr), std::move(reasonStr),
                            static_cast<int64_t>(elapsedNs));
            }
        } else if (reports) {
            stampHeader(reports->topoParallel.header, "TopoParallelPass", 0,
                        "disabled",
                        parallelCfg ? "[parallel].mode = off" : "no [parallel] config",
                        0);
        }

        // Loop parallelization (two steps):
        //   Step 1: metadata-only (parallel_accesses + unroll hints) — always
        //           applied when enabled; zero-cost when LLVM decides not to vectorize.
        //   Step 2: iteration-space partitioning via topo_task_spawn — opt-in
        //           via config.partitionEnabled; outlines loop body and spawns
        //           parallel tasks for eligible loops.
        if (loopParallelCfg && loopParallelCfg->isEnabled()) {
            auto t0 = std::chrono::steady_clock::now();
            auto* lpReport = reports ? &reports->loopParallelize : nullptr;
            int n = LoopParallelizePass::run(module, *symbols, *mapping, *loopParallelCfg, lpReport);
            markPassFired(module, "LoopParallelizePass", static_cast<unsigned>(n));
            if (reports) {
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                stampHeader(reports->loopParallelize.header, "LoopParallelizePass", n,
                            n > 0 ? "annotated" : "no_targets",
                            n > 0 ? "loop metadata (parallel_accesses + unroll) injected"
                                  : "no eligible loops",
                            static_cast<int64_t>(elapsedNs));
            }
        } else if (reports) {
            stampHeader(reports->loopParallelize.header, "LoopParallelizePass", 0,
                        "disabled",
                        loopParallelCfg ? "[loop_parallel].mode = off"
                                        : "no [loop_parallel] config",
                        0);
        }

        // PrefetchPass moved earlier (above, before AlwaysInliner) so that
        // it operates on functions still declared with access(streaming) in
        // the SymbolTable. See the rationale comment at the earlier invocation.

        // Adaptive dispatch: insert JIT function pointer dispatch.
        //
        // AdaptiveDispatchPass is instrumentation-only — it emits an atomic
        // pointer load + branch at each pipeline entry plus cost_begin/end
        // callbacks around the AOT path. It produces no immediate runtime
        // speedup by design; the JIT benefit only materializes once the
        // runtime monitor replaces the dispatch pointer.
        //
        // A speedup-gated auto benchmark (as used by Indirection and
        // TopoParallel) would therefore reject adaptive in every case: the
        // variant is always equal-or-slower than the baseline at compile
        // time. Auto mode applies unconditionally instead — instrumentation
        // overhead is on the order of a few cycles per pipeline entry,
        // well within the noise band — which keeps off / auto / force
        // mapping cleanly to "no artifacts / instrumented / instrumented".
        if (adaptiveCfg && adaptiveCfg->isEnabled()) {
            auto t0 = std::chrono::steady_clock::now();
            auto* adReport = reports ? &reports->adaptiveDispatch : nullptr;
            int n = AdaptiveDispatchPass::run(module, *symbols, *mapping, *adaptiveCfg,
                                              parallelCfg, /*excludeFuncs=*/nullptr, adReport);
            markPassFired(module, "AdaptiveDispatchPass", static_cast<unsigned>(n));
            if (adaptiveCfg->mode == FeatureMode::Auto) ++passesApplied;
            if (reports) {
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                stampHeader(reports->adaptiveDispatch.header, "AdaptiveDispatchPass", n,
                            n > 0 ? "instrumented" : "no_targets",
                            n > 0 ? "JIT dispatch pointer + cost_begin/end inserted at pipeline entries"
                                  : "no pipeline entries to instrument",
                            static_cast<int64_t>(elapsedNs));
            }
        } else if (reports) {
            stampHeader(reports->adaptiveDispatch.header, "AdaptiveDispatchPass", 0,
                        "disabled",
                        adaptiveCfg ? "[adaptive].mode = off" : "no [adaptive] config",
                        0);
        }

        // Observability: insert tracing spans at stage boundaries
        if (observabilityCfg && observabilityCfg->isEnabled()) {
            int n = ObservabilityPass::run(module, *symbols, *mapping, *observabilityCfg);
            markPassFired(module, "ObservabilityPass", static_cast<unsigned>(n));
        }

        // Lifetime arena: replace heap allocations with arena allocations
        if (lifetimeCfg && lifetimeCfg->isEnabled()) {
            auto t0 = std::chrono::steady_clock::now();
            int firedCount = 0;
            std::string decisionStr;
            std::string reasonStr;
            if (lifetimeCfg->mode == FeatureMode::Auto) {
                auto ownerNames = LifetimeArenaPass::collectOwnerFunctions(module, *symbols, *mapping);
                bool beneficial = false;
                bool anyBenchmarkSucceeded = false;
                for (const auto& ownerName : ownerNames) {
                    auto result = VariantBenchmark::run(
                        module,
                        ownerName,
                        [&](llvm::Module& clone) {
                            auto rebound = mapping->rebind(clone);
                            LifetimeArenaPass::run(clone, *symbols, rebound, *lifetimeCfg);
                        },
                        lifetimeCfg->benchmarkWarmup,
                        lifetimeCfg->benchmarkIterations,
                        baselineCache);
                    if (reports && result) {
                        backend::LifetimeArenaCandidate c;
                        c.ownerName = ownerName;
                        c.baselineNs = static_cast<int64_t>(result->baselineMedianNs);
                        c.variantNs = static_cast<int64_t>(result->variantMedianNs);
                        c.speedup = result->speedup;
                        c.winner = (result->winner == BenchmarkResult::Variant) ? "arena" : "heap";
                        c.applied = false;
                        reports->lifetimeArena.candidates.push_back(std::move(c));
                    }
                    if (result) {
                        anyBenchmarkSucceeded = true;
                        if (result->winner == BenchmarkResult::Variant) {
                            beneficial = true;
                            break;
                        }
                    }
                }
                if (beneficial) {
                    int n = LifetimeArenaPass::run(module, *symbols, *mapping, *lifetimeCfg);
                    markPassFired(module, "LifetimeArenaPass", static_cast<unsigned>(n));
                    ++passesApplied;
                    firedCount = n;
                    decisionStr = "auto_arena";
                    reasonStr = "benchmark showed arena allocation beneficial";
                    if (reports) {
                        for (auto& c : reports->lifetimeArena.candidates)
                            if (c.winner == "arena") c.applied = true;
                    }
                } else if (anyBenchmarkSucceeded) {
                    ++passesSkippedNoBenefit;
                    decisionStr = "auto_skipped_no_benefit";
                    reasonStr = "benchmark showed no improvement from arena allocation";
                } else {
                    ++passesSkippedBenchFail;
                    decisionStr = "auto_skipped_bench_fail";
                    reasonStr = "benchmark could not run to completion";
                }
            } else {
                int n = LifetimeArenaPass::run(module, *symbols, *mapping, *lifetimeCfg);
                markPassFired(module, "LifetimeArenaPass", static_cast<unsigned>(n));
                firedCount = n;
                decisionStr = n > 0 ? "forced_applied" : "forced_no_targets";
                reasonStr = "[lifetime].mode = force";
                if (reports && n > 0) {
                    // No benchmark data in force mode — record one candidate per
                    // owner from collectOwnerFunctions.
                    auto ownerNames = LifetimeArenaPass::collectOwnerFunctions(module, *symbols, *mapping);
                    for (const auto& ownerName : ownerNames) {
                        backend::LifetimeArenaCandidate c;
                        c.ownerName = ownerName;
                        c.winner = "forced";
                        c.applied = true;
                        reports->lifetimeArena.candidates.push_back(std::move(c));
                    }
                }
            }
            if (reports) {
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                stampHeader(reports->lifetimeArena.header, "LifetimeArenaPass", firedCount,
                            std::move(decisionStr), std::move(reasonStr),
                            static_cast<int64_t>(elapsedNs));
            }
        } else if (reports) {
            stampHeader(reports->lifetimeArena.header, "LifetimeArenaPass", 0,
                        "disabled",
                        lifetimeCfg ? "[lifetime].mode = off" : "no [lifetime] config",
                        0);
        }

        // Flatten: remove dead private functions (+ protected in aggressive)
        {
            auto t0 = std::chrono::steady_clock::now();
            auto* tfReport = reports ? &reports->topoFlatten : nullptr;
            int n = TopoFlattenPass::run(module, *entries, *mapping, mode, tfReport);
            markPassFired(module, "TopoFlattenPass", static_cast<unsigned>(n));
            if (reports) {
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                stampHeader(reports->topoFlatten.header, "TopoFlattenPass", n,
                            n > 0 ? "demoted" : "no_targets",
                            n > 0 ? "private/protected functions demoted to internal linkage for GlobalDCE"
                                  : "no functions eligible for linkage demotion",
                            static_cast<int64_t>(elapsedNs));
            }
        }
    } else if (entries && mapping) {
        // No SymbolTable — run without pipeline codegen and demand info
        {
            int n = ReturnSpecializationPass::run(module, *entries, *mapping);
            markPassFired(module, "ReturnSpecializationPass", static_cast<unsigned>(n));
        }

        {
            int n = TopoInlinePass::run(module, level, *entries, *mapping, nullptr, inlineCfg);
            markPassFired(module, "TopoInlinePass", static_cast<unsigned>(n));
        }
        {
            int n = TopoFlattenPass::run(module, *entries, *mapping, mode);
            markPassFired(module, "TopoFlattenPass", static_cast<unsigned>(n));
        }
    }

    // Strip noinline attributes left by -fno-inline-functions (or -O0).
    // TopoInlinePass already made visibility-based decisions; the remaining
    // noinline would block LLVM's standard inliner from optimizing further.
    for (auto& func : module) {
        if (!func.isDeclaration()) func.removeFnAttr(llvm::Attribute::NoInline);
    }

    // Annotate logical stage call-sites BEFORE the standard inliner runs.
    // TopoReorderPass attaches `!topo.stage` metadata to stage call
    // instructions in logic-block functions; nothing downstream consumes that
    // metadata, so its only observable effect is the
    // `!topo.fired.TopoReorderPass` guard. runStandardPipeline's inliner —
    // especially buildThinLTODefaultPipeline in Aggressive mode — inlines the
    // stage callees into their caller and erases the call-sites, and HOW MANY
    // survive is inliner-threshold-dependent across host toolchains (1 on
    // macOS, 0 on the linux CI runner → the fired-marker vanished there).
    // Running the annotation on the freshly-compiled IR (call-sites still
    // present, original mangled names still matched in `mapping`) makes the
    // marker deterministic and is semantically inert (metadata-only, no
    // physical reorder). TopoLayoutPass stays AFTER optimization: it assigns
    // functions to per-stage sections and wants the optimized module.
    if (symbols && mapping) {
        auto stageAnalysis = analysis::analyzeStages(*symbols);
        int nReorder = TopoReorderPass::run(module, stageAnalysis, *mapping);
        markPassFired(module, "TopoReorderPass", static_cast<unsigned>(nReorder));
    }

    // Step 2: Standard LLVM optimization pipeline
    runStandardPipeline(module, level, mode);

    // Step 3: Custom Topo passes that require the optimized module
    if (symbols && mapping) {
        int nLayout = TopoLayoutPass::run(module, *symbols, *mapping);
        markPassFired(module, "TopoLayoutPass", static_cast<unsigned>(nLayout));
    }

    if (passesApplied + passesSkippedNoBenefit + passesSkippedBenchFail > 0) {
        llvm::errs() << "topo: pass summary: " << passesApplied << " applied, "
                     << passesSkippedNoBenefit << " skipped (no benefit), "
                     << passesSkippedBenchFail << " skipped (benchmark failed)\n";
    }

    // For the 10 judging Passes that haven't been wired to populate their own
    // report yet, stamp a baseline header so the sidecar shows "wired but no
    // detail recorded" rather than an empty file. As each Pass converts (task
    // 4) it owns its block above and this fallback becomes a no-op for it.
    if (reports) {
        auto stampFallback = [&](backend::PassReportHeader& h, const char* name) {
            if (!h.passName.empty()) return; // already filled
            stampHeader(h, name, 0, "not_yet_reported",
                        "Pass executed but detail capture not yet implemented", 0);
        };
        stampFallback(reports->indirection.header,             "IndirectionPass");
        stampFallback(reports->topoParallel.header,            "TopoParallelPass");
        stampFallback(reports->lifetimeArena.header,           "LifetimeArenaPass");
        stampFallback(reports->returnSpecialization.header,    "ReturnSpecializationPass");
        stampFallback(reports->topoInline.header,              "TopoInlinePass");
        stampFallback(reports->topoFlatten.header,             "TopoFlattenPass");
        stampFallback(reports->adaptiveDispatch.header,        "AdaptiveDispatchPass");
        stampFallback(reports->prefetch.header,                "PrefetchPass");
        stampFallback(reports->containmentInterception.header, "ContainmentInterceptionPass");
        stampFallback(reports->loopParallelize.header,         "LoopParallelizePass");
    }

    return true;
}

// Legacy 19-parameter shim — forwards to the struct-form overload.
// Kept while we migrate downstream callers; new code should construct
// PassPipelineConfig directly. See PassPipeline.h for the deprecation
// notice. We deliberately disable -Wdeprecated-declarations inside the
// shim's body so the call to ourselves doesn't trip the warning.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
bool PassPipeline::run(llvm::Module& module,
                       OptLevel level,
                       const std::vector<VisibilityEntry>* entries,
                       const SymbolMapping* mapping,
                       const SymbolTable* symbols,
                       BuildMode mode,
                       const ParallelConfig* parallelCfg,
                       const std::unordered_map<std::string, uint64_t>* runtimeCosts,
                       const AdaptiveConfig* adaptiveCfg,
                       const DataLayoutConfig* dataLayoutCfg,
                       const IndirectionConfig* indirectionCfg,
                       bool indirectionExplicit,
                       const ObservabilityConfig* observabilityCfg,
                       const LifetimeConfig* lifetimeCfg,
                       const LoopParallelConfig* loopParallelCfg,
                       const PrefetchConfig* prefetchCfg,
                       const InlineConfig* inlineCfg,
                       const PipelineConfig* pipelineCfg,
                       const ContainmentConfig* containmentCfg,
                       backend::PassReports* reports) {
    PassPipelineConfig cfg;
    cfg.entries              = entries;
    cfg.mapping              = mapping;
    cfg.symbols              = symbols;
    cfg.mode                 = mode;
    cfg.parallelCfg          = parallelCfg;
    cfg.runtimeCosts         = runtimeCosts;
    cfg.adaptiveCfg          = adaptiveCfg;
    cfg.dataLayoutCfg        = dataLayoutCfg;
    cfg.indirectionCfg       = indirectionCfg;
    cfg.indirectionExplicit  = indirectionExplicit;
    cfg.observabilityCfg     = observabilityCfg;
    cfg.lifetimeCfg          = lifetimeCfg;
    cfg.loopParallelCfg      = loopParallelCfg;
    cfg.prefetchCfg          = prefetchCfg;
    cfg.inlineCfg            = inlineCfg;
    cfg.pipelineCfg          = pipelineCfg;
    cfg.containmentCfg       = containmentCfg;
    cfg.reports              = reports;
    return run(module, level, cfg);
}
#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace topo
