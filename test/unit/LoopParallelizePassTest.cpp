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

// =====================================================================
// Step 2: miscompile-class safety gates (decline rather than miscompile)
// =====================================================================

namespace {

/// Register `funcNames` as a parallel stage so partitionLoopsPhase2 considers
/// each function's loops. The functions must already exist in `mod`.
void registerParallelStage(SymbolTable& symbols,
                           SymbolMapping& mapping,
                           const std::string& ns,
                           const std::vector<std::pair<std::string, llvm::Function*>>& funcs,
                           int stage) {
    for (const auto& [name, func] : funcs) {
        std::string qualified = ns + "::" + name;
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
    for (const auto& [name, func] : funcs) {
        (void)func;
        entry.calledFunctions.push_back(name);
        entry.stages.push_back(stage);
    }
    symbols.addLogicBlock(entry);
}

} // namespace

// Memory-carried recurrence: a[i] = a[i-1] + b[i]. Splitting this across
// concurrent partitions races on overlapping a[] slices. Must decline.
TEST_F(LoopParallelizePassTest, Phase2SkipsMemoryCarriedRecurrence) {
    auto makeFn = [&](const std::string& name) -> llvm::Function* {
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* funcTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy, ptrTy}, false);
        auto* func = llvm::Function::Create(funcTy, llvm::Function::ExternalLinkage, name, module.get());

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
        auto* header = llvm::BasicBlock::Create(ctx, "loop.header", func);
        auto* body = llvm::BasicBlock::Create(ctx, "loop.body", func);
        auto* exit = llvm::BasicBlock::Create(ctx, "loop.exit", func);
        auto* a = func->getArg(0);
        auto* b = func->getArg(1);

        llvm::IRBuilder<> builder(entry);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* phi = builder.CreatePHI(i32Ty, 2, "i");
        // start at 1 so a[i-1] is in range
        phi->addIncoming(llvm::ConstantInt::get(i32Ty, 1), entry);
        auto* cmp = builder.CreateICmpSLT(phi, llvm::ConstantInt::get(i32Ty, 2048), "cmp");
        builder.CreateCondBr(cmp, body, exit);

        builder.SetInsertPoint(body);
        auto* prevIdx = builder.CreateSub(phi, llvm::ConstantInt::get(i32Ty, 1), "prev");
        auto* prevPtr = builder.CreateGEP(i32Ty, a, {prevIdx}, "a.prev.ptr");
        auto* prevVal = builder.CreateLoad(i32Ty, prevPtr, "a.prev");
        auto* bPtr = builder.CreateGEP(i32Ty, b, {phi}, "b.ptr");
        auto* bVal = builder.CreateLoad(i32Ty, bPtr, "b.cur");
        auto* sum = builder.CreateAdd(prevVal, bVal, "sum");
        auto* curPtr = builder.CreateGEP(i32Ty, a, {phi}, "a.cur.ptr");
        builder.CreateStore(sum, curPtr);
        auto* inc = builder.CreateAdd(phi, llvm::ConstantInt::get(i32Ty, 1), "inc");
        phi->addIncoming(inc, body);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
        builder.CreateRetVoid();
        return func;
    };

    registerParallelStage(symbols, mapping, "sim", {{"rec_a", makeFn("rec_a")}, {"rec_b", makeFn("rec_b")}}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.partitionEnabled = true;
    config.reductionEnabled = true;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    EXPECT_GT(result, 0); // Step 1 annotation still applies
    EXPECT_EQ(module->getFunction("topo_task_spawn"), nullptr); // never partitioned
    EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}

// Reduction accumulator read mid-iteration: sum += i; out[i] = sum.
// (rhs is the induction var, so the loop is store-only and bypasses the
// memory-dep gate, isolating the reduction use-restriction.) A
// partition-local identity-seeded accumulator would expose a partial
// instead of the global running value. Must decline (use-restriction).
TEST_F(LoopParallelizePassTest, Phase2SkipsReductionReadMidIteration) {
    auto makeFn = [&](const std::string& name) -> llvm::Function* {
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* funcTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy}, false);
        auto* func = llvm::Function::Create(funcTy, llvm::Function::ExternalLinkage, name, module.get());

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
        auto* header = llvm::BasicBlock::Create(ctx, "loop.header", func);
        auto* body = llvm::BasicBlock::Create(ctx, "loop.body", func);
        auto* exit = llvm::BasicBlock::Create(ctx, "loop.exit", func);
        auto* out = func->getArg(0);

        llvm::IRBuilder<> builder(entry);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* i = builder.CreatePHI(i32Ty, 2, "i");
        i->addIncoming(llvm::ConstantInt::get(i32Ty, 0), entry);
        auto* sum = builder.CreatePHI(i32Ty, 2, "sum");
        sum->addIncoming(llvm::ConstantInt::get(i32Ty, 0), entry);
        auto* cmp = builder.CreateICmpSLT(i, llvm::ConstantInt::get(i32Ty, 2048), "cmp");
        builder.CreateCondBr(cmp, body, exit);

        builder.SetInsertPoint(body);
        auto* nextSum = builder.CreateAdd(sum, i, "sum.next");
        // mid-iteration read of the accumulator: out[i] = sum.next
        auto* outPtr = builder.CreateGEP(i32Ty, out, {i}, "out.ptr");
        builder.CreateStore(nextSum, outPtr);
        auto* inc = builder.CreateAdd(i, llvm::ConstantInt::get(i32Ty, 1), "inc");
        i->addIncoming(inc, body);
        sum->addIncoming(nextSum, body);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
        builder.CreateRetVoid();
        return func;
    };

    registerParallelStage(symbols, mapping, "sim", {{"mid_a", makeFn("mid_a")}, {"mid_b", makeFn("mid_b")}}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.partitionEnabled = true;
    config.reductionEnabled = true;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    EXPECT_GT(result, 0);
    EXPECT_EQ(module->getFunction("topo_task_spawn"), nullptr); // declined
    EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}

// Secondary affine induction variable used as an index: for (i,j=5) {
// out[i] = j; j += 2; }. The worker only remaps the canonical IV, so j
// would be read as undef. Must decline.
TEST_F(LoopParallelizePassTest, Phase2SkipsSecondaryInductionVariable) {
    auto makeFn = [&](const std::string& name) -> llvm::Function* {
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* funcTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy}, false);
        auto* func = llvm::Function::Create(funcTy, llvm::Function::ExternalLinkage, name, module.get());

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
        auto* header = llvm::BasicBlock::Create(ctx, "loop.header", func);
        auto* body = llvm::BasicBlock::Create(ctx, "loop.body", func);
        auto* exit = llvm::BasicBlock::Create(ctx, "loop.exit", func);
        auto* out = func->getArg(0);

        llvm::IRBuilder<> builder(entry);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* i = builder.CreatePHI(i32Ty, 2, "i");
        i->addIncoming(llvm::ConstantInt::get(i32Ty, 0), entry);
        auto* j = builder.CreatePHI(i32Ty, 2, "j");
        j->addIncoming(llvm::ConstantInt::get(i32Ty, 5), entry);
        auto* cmp = builder.CreateICmpSLT(i, llvm::ConstantInt::get(i32Ty, 2048), "cmp");
        builder.CreateCondBr(cmp, body, exit);

        builder.SetInsertPoint(body);
        // out[i] = j  — j observed mid-iteration as a stored value
        auto* outPtr = builder.CreateGEP(i32Ty, out, {i}, "out.ptr");
        builder.CreateStore(j, outPtr);
        auto* inc = builder.CreateAdd(i, llvm::ConstantInt::get(i32Ty, 1), "inc");
        auto* jNext = builder.CreateAdd(j, llvm::ConstantInt::get(i32Ty, 2), "j.next");
        i->addIncoming(inc, body);
        j->addIncoming(jNext, body);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
        builder.CreateRetVoid();
        return func;
    };

    registerParallelStage(symbols, mapping, "sim", {{"sec_a", makeFn("sec_a")}, {"sec_b", makeFn("sec_b")}}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.partitionEnabled = true;
    config.reductionEnabled = true;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    EXPECT_GT(result, 0);
    EXPECT_EQ(module->getFunction("topo_task_spawn"), nullptr); // declined
    EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}

// Indirect / opaque call inside the loop: the callee is unknown, so it may
// take a lock or perform ordered I/O the name-prefix sync scan cannot see.
// containsSyncPrimitives() must treat an unknown callee as a hazard and the
// loop must be declined rather than partitioned.
TEST_F(LoopParallelizePassTest, Phase2SkipsLoopWithIndirectCall) {
    auto makeFn = [&](const std::string& name) -> llvm::Function* {
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        // 2nd arg is a function pointer we call indirectly each iteration.
        auto* funcTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy, ptrTy}, false);
        auto* func = llvm::Function::Create(funcTy, llvm::Function::ExternalLinkage, name, module.get());

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
        auto* header = llvm::BasicBlock::Create(ctx, "loop.header", func);
        auto* body = llvm::BasicBlock::Create(ctx, "loop.body", func);
        auto* exit = llvm::BasicBlock::Create(ctx, "loop.exit", func);
        auto* fnPtr = func->getArg(1);

        // Signature of the callee invoked indirectly: void(i32).
        auto* calleeTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {i32Ty}, false);

        llvm::IRBuilder<> builder(entry);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* i = builder.CreatePHI(i32Ty, 2, "i");
        i->addIncoming(llvm::ConstantInt::get(i32Ty, 0), entry);
        auto* cmp = builder.CreateICmpSLT(i, llvm::ConstantInt::get(i32Ty, 2048), "cmp");
        builder.CreateCondBr(cmp, body, exit);

        builder.SetInsertPoint(body);
        // Indirect call through the function-pointer argument: callee unknown.
        builder.CreateCall(calleeTy, fnPtr, {i});
        auto* inc = builder.CreateAdd(i, llvm::ConstantInt::get(i32Ty, 1), "inc");
        i->addIncoming(inc, body);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
        builder.CreateRetVoid();
        return func;
    };

    registerParallelStage(symbols, mapping, "sim", {{"ind_a", makeFn("ind_a")}, {"ind_b", makeFn("ind_b")}}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.partitionEnabled = true;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    EXPECT_GT(result, 0); // Step 1 annotation still applies
    EXPECT_EQ(module->getFunction("topo_task_spawn"), nullptr); // declined
    EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}

// Regression for the exit-PHI-cast-after-terminator bug. When a reduction is
// present AND a separate non-i64 integer LCSSA exit PHI carries a
// non-reduction value, the exit-PHI fixup must cast the i64 trip count to the
// PHI's type *before* the exit predecessor's terminator. Inserting it after
// the terminator (the old behaviour) produced IR the verifier rejects. Here:
//   for (i32 i = 0; i < 2048; ++i) sum += i;   // sum is an i32 reduction
//   exit:  i.lcssa = phi i32 [i, header]       // non-reduction i32 exit PHI
// The pass must partition the reduction AND emit verifier-clean IR.
TEST_F(LoopParallelizePassTest, Phase2ReductionWithNonI64ExitPhiEmitsValidIR) {
    auto makeFn = [&](const std::string& name) -> llvm::Function* {
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* funcTy = llvm::FunctionType::get(i32Ty, {}, false);
        auto* func = llvm::Function::Create(funcTy, llvm::Function::ExternalLinkage, name, module.get());

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
        auto* header = llvm::BasicBlock::Create(ctx, "loop.header", func);
        auto* body = llvm::BasicBlock::Create(ctx, "loop.body", func);
        auto* exit = llvm::BasicBlock::Create(ctx, "loop.exit", func);

        llvm::IRBuilder<> builder(entry);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        // Canonical i32 induction variable {0,+,1}.
        auto* i = builder.CreatePHI(i32Ty, 2, "i");
        i->addIncoming(llvm::ConstantInt::get(i32Ty, 0), entry);
        // i32 reduction accumulator: sum += i (closed recurrence, no other use).
        auto* sum = builder.CreatePHI(i32Ty, 2, "sum");
        sum->addIncoming(llvm::ConstantInt::get(i32Ty, 0), entry);
        auto* cmp = builder.CreateICmpSLT(i, llvm::ConstantInt::get(i32Ty, 2048), "cmp");
        builder.CreateCondBr(cmp, body, exit);

        builder.SetInsertPoint(body);
        auto* nextSum = builder.CreateAdd(sum, i, "sum.next");
        auto* inc = builder.CreateAdd(i, llvm::ConstantInt::get(i32Ty, 1), "inc");
        i->addIncoming(inc, body);
        sum->addIncoming(nextSum, body);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
        // Two LCSSA exit PHIs: the reduction result (i32) and the induction
        // variable (i32, non-reduction). The latter drives the buggy
        // non-i64 IntCast path; without the fix the cast lands after exit's
        // terminator.
        auto* sumOut = builder.CreatePHI(i32Ty, 1, "sum.lcssa");
        sumOut->addIncoming(sum, header);
        auto* iOut = builder.CreatePHI(i32Ty, 1, "i.lcssa");
        iOut->addIncoming(i, header);
        auto* ret = builder.CreateAdd(sumOut, iOut, "ret");
        builder.CreateRet(ret);
        return func;
    };

    registerParallelStage(symbols, mapping, "sim", {{"red_a", makeFn("red_a")}, {"red_b", makeFn("red_b")}}, 2);

    LoopParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.partitionEnabled = true;
    config.reductionEnabled = true;

    int result = LoopParallelizePass::run(*module, symbols, mapping, config);
    EXPECT_GT(result, 0);
    // The reduction loop IS partitioned (Step C combine is implemented).
    EXPECT_NE(module->getFunction("topo_task_spawn"), nullptr);
    // The cast must sit before the terminator -> the module verifies clean.
    EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}
