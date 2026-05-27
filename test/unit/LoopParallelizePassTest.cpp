#include <gtest/gtest.h>

#include "topo/Transforms/LoopParallelizePass.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

using namespace topo;

class LoopParallelizePassTest : public ::testing::Test {
protected:
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;

    void SetUp() override { module = std::make_unique<llvm::Module>("test", ctx); }

    /// Create a function with a simple loop:
    ///   for (i = 0; i < N; ++i) { arr[i] = i; }
    llvm::Function* createFuncWithLoop(const std::string& name) {
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* funcTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy, i32Ty}, false);
        auto* func = llvm::Function::Create(funcTy, llvm::Function::ExternalLinkage, name, module.get());

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
        auto* header = llvm::BasicBlock::Create(ctx, "loop.header", func);
        auto* body = llvm::BasicBlock::Create(ctx, "loop.body", func);
        auto* exit = llvm::BasicBlock::Create(ctx, "loop.exit", func);

        llvm::IRBuilder<> builder(entry);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* phi = builder.CreatePHI(i32Ty, 2, "i");
        phi->addIncoming(llvm::ConstantInt::get(i32Ty, 0), entry);
        auto* cmp = builder.CreateICmpSLT(phi, func->getArg(1), "cmp");
        builder.CreateCondBr(cmp, body, exit);

        builder.SetInsertPoint(body);
        auto* gep = builder.CreateGEP(i32Ty, func->getArg(0), {phi}, "ptr");
        builder.CreateStore(phi, gep);
        auto* inc = builder.CreateAdd(phi, llvm::ConstantInt::get(i32Ty, 1), "inc");
        phi->addIncoming(inc, body);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
        builder.CreateRetVoid();

        return func;
    }

    void addParallelStageSetup(const std::string& ns, const std::vector<std::string>& funcNames, int stage) {
        // Create functions and add to mapping
        for (const auto& name : funcNames) {
            std::string qualified = ns + "::" + name;
            auto* func = createFuncWithLoop(name);
            mapping.matched[qualified] = func;

            FunctionSymbol sym;
            sym.qualifiedName = qualified;
            sym.simpleName = name;
            sym.visibility = Visibility::Public;
            symbols.addFunction(sym);
        }

        // Create a logic block with both functions in the same stage
        LogicBlockEntry entry;
        entry.qualifiedName = ns + "::run";
        entry.simpleName = "run";
        for (const auto& name : funcNames) {
            entry.calledFunctions.push_back(name);
            entry.stages.push_back(stage);
        }
        symbols.addLogicBlock(entry);
    }

    /// Same as addParallelStageSetup but also sets cardinality and/or
    /// accessPattern on the first function in the list.
    void addParallelStageWithHints(const std::string& ns,
                                   const std::vector<std::string>& funcNames,
                                   int stage,
                                   std::optional<CardinalityHint> cardinality,
                                   AccessPattern accessPattern = AccessPattern::None) {
        // Create functions and add to mapping
        bool first = true;
        for (const auto& name : funcNames) {
            std::string qualified = ns + "::" + name;
            auto* func = createFuncWithLoop(name);
            mapping.matched[qualified] = func;

            FunctionSymbol sym;
            sym.qualifiedName = qualified;
            sym.simpleName = name;
            sym.visibility = Visibility::Public;
            if (first) {
                sym.cardinality = cardinality;
                sym.accessPattern = accessPattern;
                first = false;
            }
            symbols.addFunction(sym);
        }

        LogicBlockEntry entry;
        entry.qualifiedName = ns + "::run";
        entry.simpleName = "run";
        for (const auto& name : funcNames) {
            entry.calledFunctions.push_back(name);
            entry.stages.push_back(stage);
        }
        symbols.addLogicBlock(entry);
    }
};

TEST_F(LoopParallelizePassTest, DisabledConfigNoChanges) {
    addParallelStageSetup("sim", {"compute_a", "compute_b"}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Off;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    EXPECT_EQ(result, 0);
}

TEST_F(LoopParallelizePassTest, ParallelStageFunctionsAnnotated) {
    addParallelStageSetup("sim", {"compute_a", "compute_b"}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Auto;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    EXPECT_GT(result, 0);
}

TEST_F(LoopParallelizePassTest, ReportRecordsAnnotatedLoopsPerFunction) {
    addParallelStageSetup("sim", {"compute_a", "compute_b"}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Auto;

    backend::LoopParallelizeReport report;
    int result = LoopParallelizePass::run(*module, symbols, mapping, config, &report);
    EXPECT_GT(result, 0);
    ASSERT_FALSE(report.entries.empty());
    int totalLoops = 0;
    for (const auto& e : report.entries) {
        EXPECT_GT(e.annotatedLoops, 0);
        totalLoops += e.annotatedLoops;
    }
    EXPECT_EQ(totalLoops, result);
}

TEST_F(LoopParallelizePassTest, SingleStageFunctionSkipped) {
    // Only one function in stage - not parallel
    std::string qualified = "sim::solo";
    auto* func = createFuncWithLoop("solo");
    mapping.matched[qualified] = func;

    FunctionSymbol sym;
    sym.qualifiedName = qualified;
    sym.simpleName = "solo";
    sym.visibility = Visibility::Public;
    symbols.addFunction(sym);

    LogicBlockEntry entry;
    entry.qualifiedName = "sim::run";
    entry.simpleName = "run";
    entry.calledFunctions.push_back("solo");
    entry.stages.push_back(1);
    symbols.addLogicBlock(entry);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Auto;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    EXPECT_EQ(result, 0);
}

TEST_F(LoopParallelizePassTest, ExcludeListSkipsFunctions) {
    addParallelStageSetup("sim", {"compute_a", "compute_b"}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.exclude.push_back("compute_a");

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    // Only compute_b should be annotated (1 loop)
    EXPECT_EQ(result, 1);
}

TEST_F(LoopParallelizePassTest, AccessGroupMetadataPresent) {
    addParallelStageSetup("sim", {"compute_a", "compute_b"}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Auto;

    LoopParallelizePass::run(*module, symbols, mapping, config);

    // Verify access group metadata on memory instructions
    bool foundAccessGroup = false;
    for (auto& F : *module) {
        for (auto& BB : F) {
            for (auto& I : BB) {
                if (I.getMetadata(llvm::LLVMContext::MD_access_group)) {
                    foundAccessGroup = true;
                    break;
                }
            }
            if (foundAccessGroup) break;
        }
        if (foundAccessGroup) break;
    }
    EXPECT_TRUE(foundAccessGroup);
}

TEST_F(LoopParallelizePassTest, VectorizeEnableMetadataAbsent) {
    addParallelStageSetup("sim", {"compute_a", "compute_b"}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Auto;

    LoopParallelizePass::run(*module, symbols, mapping, config);

    // vectorize.enable must NOT be injected — it overrides LLVM's cost model.
    // Only parallel_accesses should be present.
    bool foundVecEnable = false;
    for (auto& F : *module) {
        for (auto& BB : F) {
            for (auto& I : BB) {
                if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&I)) {
                    if (auto* md = br->getMetadata(llvm::LLVMContext::MD_loop)) {
                        for (unsigned i = 1; i < md->getNumOperands(); ++i) {
                            if (auto* inner = llvm::dyn_cast<llvm::MDNode>(md->getOperand(i))) {
                                if (inner->getNumOperands() >= 1) {
                                    if (auto* str = llvm::dyn_cast<llvm::MDString>(inner->getOperand(0))) {
                                        if (str->getString() == "llvm.loop.vectorize.enable") {
                                            foundVecEnable = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    EXPECT_FALSE(foundVecEnable);
}

// --- Helper: search loop metadata for a given MDString key ---

static bool hasLoopMetadata(llvm::Module& M, const std::string& key) {
    for (auto& F : M) {
        for (auto& BB : F) {
            for (auto& I : BB) {
                if (auto* md = I.getMetadata(llvm::LLVMContext::MD_loop)) {
                    for (unsigned i = 1; i < md->getNumOperands(); ++i) {
                        if (auto* inner = llvm::dyn_cast<llvm::MDNode>(md->getOperand(i))) {
                            if (inner->getNumOperands() >= 1) {
                                if (auto* str = llvm::dyn_cast<llvm::MDString>(inner->getOperand(0))) {
                                    if (str->getString() == key) return true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

// --- Cardinality-based unroll hint tests ---

// Removed: CardinalitySmallFullUnroll, CardinalityMediumPartialUnroll,
// CardinalityLargeNoUnroll. The cardinality-based unroll bucket
// selection was deleted from the Pass — Topo passes do not gate
// on workload-side cost heuristics. Unroll decisions now belong entirely
// to LLVM's standard LoopUnroll pass + cost model.

TEST_F(LoopParallelizePassTest, CardinalityNoLongerInjectsUnrollMetadata) {
    // Verify the Pass no longer injects llvm.loop.unroll.* metadata even
    // when cardinality is declared. The standard LoopUnroll pass remains
    // free to choose any unroll factor based on its own analysis.
    CardinalityHint hint{0, 8};
    addParallelStageWithHints("sim", {"card_a", "card_b"}, 2, hint, AccessPattern::None);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Auto;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    EXPECT_GT(result, 0);
    EXPECT_FALSE(hasLoopMetadata(*module, "llvm.loop.unroll.full"));
    EXPECT_FALSE(hasLoopMetadata(*module, "llvm.loop.unroll.count"));
}

// =====================================================================
// Step 2: Partition-based loop parallelization tests
// =====================================================================

/// Create a function with a loop of known constant trip count:
///   for (i = 0; i < tripCount; ++i) { arr[i] = i; }
llvm::Function* createFuncWithConstTripCountLoop(llvm::LLVMContext& ctx,
                                                 llvm::Module& mod,
                                                 const std::string& name,
                                                 int tripCount) {
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);
    auto* funcTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::Function::ExternalLinkage, name, mod);

    auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
    auto* header = llvm::BasicBlock::Create(ctx, "loop.header", func);
    auto* body = llvm::BasicBlock::Create(ctx, "loop.body", func);
    auto* exit = llvm::BasicBlock::Create(ctx, "loop.exit", func);

    llvm::IRBuilder<> builder(entry);
    builder.CreateBr(header);

    builder.SetInsertPoint(header);
    auto* phi = builder.CreatePHI(i32Ty, 2, "i");
    phi->addIncoming(llvm::ConstantInt::get(i32Ty, 0), entry);
    auto* cmp = builder.CreateICmpSLT(phi, llvm::ConstantInt::get(i32Ty, tripCount), "cmp");
    builder.CreateCondBr(cmp, body, exit);

    builder.SetInsertPoint(body);
    auto* gep = builder.CreateGEP(i32Ty, func->getArg(0), {phi}, "ptr");
    builder.CreateStore(phi, gep);
    auto* inc = builder.CreateAdd(phi, llvm::ConstantInt::get(i32Ty, 1), "inc");
    phi->addIncoming(inc, body);
    builder.CreateBr(header);

    builder.SetInsertPoint(exit);
    builder.CreateRetVoid();

    return func;
}

TEST_F(LoopParallelizePassTest, Phase2DisabledByDefault) {
    addParallelStageSetup("sim", {"part_a", "part_b"}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    // partitionEnabled defaults to false

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    // Should only annotate (Step 1), not partition
    EXPECT_GT(result, 0);

    // Verify no topo_task_spawn calls were inserted
    auto* spawnFunc = module->getFunction("topo_task_spawn");
    EXPECT_EQ(spawnFunc, nullptr);
}

TEST_F(LoopParallelizePassTest, Phase2PartitionsLargeLoop) {
    // Create functions with large constant trip count loops
    std::string ns = "sim";
    std::vector<std::string> funcNames = {"big_loop_a", "big_loop_b"};

    for (const auto& name : funcNames) {
        std::string qualified = ns + "::" + name;
        auto* func = createFuncWithConstTripCountLoop(ctx, *module, name, 2048);
        mapping.matched[qualified] = func;

        FunctionSymbol sym;
        sym.qualifiedName = qualified;
        sym.simpleName = name;
        sym.visibility = Visibility::Public;
        symbols.addFunction(sym);
    }

    LogicBlockEntry entry;
    entry.qualifiedName = ns + "::run";
    entry.simpleName = "run";
    for (const auto& name : funcNames) {
        entry.calledFunctions.push_back(name);
        entry.stages.push_back(2);
    }
    symbols.addLogicBlock(entry);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.partitionEnabled = true;
    config.chunkSize = 64;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    EXPECT_GT(result, 0);

    // Verify runtime functions were declared
    EXPECT_NE(module->getFunction("topo_task_spawn"), nullptr);
    EXPECT_NE(module->getFunction("topo_task_await_all"), nullptr);
    EXPECT_NE(module->getFunction("topo_parallel_ensure_init"), nullptr);
}

// Removed: Phase2SkipsSmallTripCount. The `minTripCount` threshold was
// deleted from the Pass — Topo passes do not gate on
// workload-side cost heuristics. Small-trip-count loops still partition
// (one chunk, correct results); LLVM cost model + benchmark decide
// whether the runtime overhead is acceptable.

TEST_F(LoopParallelizePassTest, Phase2SkipsLoopWithAtomicOps) {
    // Create a function with a loop containing an atomicrmw instruction
    std::string ns = "sim";
    std::vector<std::string> funcNames = {"atomic_a", "atomic_b"};

    for (const auto& name : funcNames) {
        std::string qualified = ns + "::" + name;

        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* funcTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy}, false);
        auto* func = llvm::Function::Create(funcTy, llvm::Function::ExternalLinkage, name, module.get());

        auto* entryBB = llvm::BasicBlock::Create(ctx, "entry", func);
        auto* header = llvm::BasicBlock::Create(ctx, "loop.header", func);
        auto* body = llvm::BasicBlock::Create(ctx, "loop.body", func);
        auto* exit = llvm::BasicBlock::Create(ctx, "loop.exit", func);

        llvm::IRBuilder<> builder(entryBB);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* phi = builder.CreatePHI(i32Ty, 2, "i");
        phi->addIncoming(llvm::ConstantInt::get(i32Ty, 0), entryBB);
        auto* cmp = builder.CreateICmpSLT(phi, llvm::ConstantInt::get(i32Ty, 2048), "cmp");
        builder.CreateCondBr(cmp, body, exit);

        builder.SetInsertPoint(body);
        // AtomicRMW — makes loop unsafe for partitioning
        builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add,
                                func->getArg(0),
                                llvm::ConstantInt::get(i32Ty, 1),
                                llvm::MaybeAlign(4),
                                llvm::AtomicOrdering::SequentiallyConsistent);
        auto* inc = builder.CreateAdd(phi, llvm::ConstantInt::get(i32Ty, 1), "inc");
        phi->addIncoming(inc, body);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
        builder.CreateRetVoid();

        mapping.matched[qualified] = func;

        FunctionSymbol sym;
        sym.qualifiedName = qualified;
        sym.simpleName = name;
        sym.visibility = Visibility::Public;
        symbols.addFunction(sym);
    }

    LogicBlockEntry entry;
    entry.qualifiedName = ns + "::run";
    entry.simpleName = "run";
    for (const auto& name : funcNames) {
        entry.calledFunctions.push_back(name);
        entry.stages.push_back(2);
    }
    symbols.addLogicBlock(entry);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.partitionEnabled = true;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    // Step 1 metadata should still be applied, but no partitioning
    EXPECT_GT(result, 0);
    EXPECT_EQ(module->getFunction("topo_task_spawn"), nullptr);
}

TEST_F(LoopParallelizePassTest, Phase2WorkerFunctionCreated) {
    std::string ns = "sim";
    std::vector<std::string> funcNames = {"worker_a", "worker_b"};

    for (const auto& name : funcNames) {
        std::string qualified = ns + "::" + name;
        auto* func = createFuncWithConstTripCountLoop(ctx, *module, name, 4096);
        mapping.matched[qualified] = func;

        FunctionSymbol sym;
        sym.qualifiedName = qualified;
        sym.simpleName = name;
        sym.visibility = Visibility::Public;
        symbols.addFunction(sym);
    }

    LogicBlockEntry entry;
    entry.qualifiedName = ns + "::run";
    entry.simpleName = "run";
    for (const auto& name : funcNames) {
        entry.calledFunctions.push_back(name);
        entry.stages.push_back(2);
    }
    symbols.addLogicBlock(entry);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.partitionEnabled = true;

    LoopParallelizePass::run(*module, symbols, mapping, config);

    // Verify that a worker function was created with the expected naming
    bool foundWorker = false;
    for (auto& F : *module) {
        if (F.getName().contains("topo_loop_worker")) {
            foundWorker = true;
            // Worker should be internal linkage
            EXPECT_EQ(F.getLinkage(), llvm::GlobalValue::InternalLinkage);
            // Worker takes a single void* argument
            EXPECT_EQ(F.arg_size(), 1u);
            break;
        }
    }
    EXPECT_TRUE(foundWorker);
}

TEST_F(LoopParallelizePassTest, Phase2DynamicStrategyMorePartitions) {
    std::string ns = "sim";
    std::vector<std::string> funcNames = {"dyn_a", "dyn_b"};

    for (const auto& name : funcNames) {
        std::string qualified = ns + "::" + name;
        auto* func = createFuncWithConstTripCountLoop(ctx, *module, name, 8192);
        mapping.matched[qualified] = func;

        FunctionSymbol sym;
        sym.qualifiedName = qualified;
        sym.simpleName = name;
        sym.visibility = Visibility::Public;
        symbols.addFunction(sym);
    }

    LogicBlockEntry entry;
    entry.qualifiedName = ns + "::run";
    entry.simpleName = "run";
    for (const auto& name : funcNames) {
        entry.calledFunctions.push_back(name);
        entry.stages.push_back(2);
    }
    symbols.addLogicBlock(entry);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.partitionEnabled = true;
    config.partitionStrategy = LoopPartitionStrategy::Dynamic;
    config.chunkSize = 64;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    EXPECT_GT(result, 0);

    // Verify module is valid IR
    EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}

TEST_F(LoopParallelizePassTest, Phase2InstrumentedPartition) {
    std::string ns = "sim";
    std::vector<std::string> funcNames = {"inst_a", "inst_b"};

    for (const auto& name : funcNames) {
        std::string qualified = ns + "::" + name;
        auto* func = createFuncWithConstTripCountLoop(ctx, *module, name, 4096);
        mapping.matched[qualified] = func;

        FunctionSymbol sym;
        sym.qualifiedName = qualified;
        sym.simpleName = name;
        sym.visibility = Visibility::Public;
        symbols.addFunction(sym);
    }

    LogicBlockEntry entry;
    entry.qualifiedName = ns + "::run";
    entry.simpleName = "run";
    for (const auto& name : funcNames) {
        entry.calledFunctions.push_back(name);
        entry.stages.push_back(2);
    }
    symbols.addLogicBlock(entry);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.partitionEnabled = true;
    config.instrument = true;

    LoopParallelizePass::run(*module, symbols, mapping, config);

    // Verify cost sampling functions were declared when instrumented
    EXPECT_NE(module->getFunction("topo_cost_begin"), nullptr);
    EXPECT_NE(module->getFunction("topo_cost_end"), nullptr);
}
