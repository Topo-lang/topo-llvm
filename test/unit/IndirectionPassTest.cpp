#include "topo/Transforms/IndirectionPass.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class IndirectionPassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

// Helper: create unique_ptr-like struct type
struct UniquePtrTestModule {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::StructType* uniquePtrTy = nullptr;
    llvm::Function* testFunc = nullptr;
    std::vector<VisibilityEntry> entries;

    void build(llvm::LLVMContext& ctx, bool addStore = false) {
        module = std::make_unique<llvm::Module>("test_indirection", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);

        // Create unique_ptr-like struct: { ptr }
        uniquePtrTy = llvm::StructType::create(ctx, {ptrTy}, "class.std::unique_ptr");

        // Function that uses unique_ptr: takes unique_ptr alloca, does
        // multiple loads
        auto* funcTy = llvm::FunctionType::get(i32Ty, {}, false);
        testFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "test_func", *module);

        auto* bb = llvm::BasicBlock::Create(ctx, "entry", testFunc);
        llvm::IRBuilder<> builder(bb);

        // Alloca unique_ptr
        auto* alloca = builder.CreateAlloca(uniquePtrTy, nullptr, "uptr");

        // GEP to internal pointer (field 0)
        auto* gep1 = builder.CreateStructGEP(uniquePtrTy, alloca, 0, "ptr.gep1");
        auto* load1 = builder.CreateLoad(ptrTy, gep1, "ptr.load1");

        // Second redundant load of same pointer
        auto* gep2 = builder.CreateStructGEP(uniquePtrTy, alloca, 0, "ptr.gep2");
        auto* load2 = builder.CreateLoad(ptrTy, gep2, "ptr.load2");

        if (addStore) {
            // Store to internal pointer (simulates reset/move)
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            auto* gep3 = builder.CreateStructGEP(uniquePtrTy, alloca, 0, "ptr.gep3");
            builder.CreateStore(nullPtr, gep3);
        }

        // Use both loaded pointers (prevent DCE)
        auto* intVal1 = builder.CreatePtrToInt(load1, i32Ty);
        auto* intVal2 = builder.CreatePtrToInt(load2, i32Ty);
        auto* sum = builder.CreateAdd(intVal1, intVal2);
        builder.CreateRet(sum);

        // Add function symbol
        FunctionSymbol sym;
        sym.qualifiedName = "test::test_func";
        sym.simpleName = "test_func";
        sym.visibility = Visibility::Public;
        symbols.addFunction(sym);

        mapping.matched["test::test_func"] = testFunc;
        entries.push_back({"test::test_func", Visibility::Public});
    }
};

// Helper: create a pipeline module for pointer attr inference tests
struct PipelineTestModule {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* pipelineFunc = nullptr;
    llvm::Function* stageFunc = nullptr;
    std::vector<VisibilityEntry> entries;

    void build(llvm::LLVMContext& ctx, bool addConcurrentStage = false) {
        module = std::make_unique<llvm::Module>("test_pipeline_indir", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);

        // Stage function: takes a pointer
        auto* stageTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
        stageFunc = llvm::Function::Create(stageTy, llvm::GlobalValue::ExternalLinkage, "stage_a", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageFunc);
            llvm::IRBuilder<> b(bb);
            auto* loaded = b.CreateLoad(i32Ty, stageFunc->getArg(0));
            b.CreateRet(loaded);
        }

        llvm::Function* stageBFunc = nullptr;
        if (addConcurrentStage) {
            stageBFunc = llvm::Function::Create(stageTy, llvm::GlobalValue::ExternalLinkage, "stage_b", *module);
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageBFunc);
            llvm::IRBuilder<> b(bb);
            auto* loaded = b.CreateLoad(i32Ty, stageBFunc->getArg(0));
            b.CreateRet(loaded);
        }

        // Pipeline function
        auto* pipeTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
        pipelineFunc = llvm::Function::Create(pipeTy, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            // Alloca to pass to stage (known non-null)
            auto* alloca = b.CreateAlloca(i32Ty);
            auto* r = b.CreateCall(stageFunc, {alloca});
            if (addConcurrentStage) {
                b.CreateCall(stageBFunc, {alloca});
            }
            b.CreateRet(r);
        }

        // Symbols
        FunctionSymbol stageSym;
        stageSym.qualifiedName = "ns::stage_a";
        stageSym.simpleName = "stage_a";
        stageSym.visibility = Visibility::Protected;
        symbols.addFunction(stageSym);

        LogicBlockEntry lb;
        lb.qualifiedName = "ns::pipeline";
        lb.simpleName = "pipeline";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::stage_a"};

        PipelineAnalysis analysis;
        analysis.stages = {{"ns::stage_a", 0}};

        if (addConcurrentStage) {
            FunctionSymbol stageBSym;
            stageBSym.qualifiedName = "ns::stage_b";
            stageBSym.simpleName = "stage_b";
            stageBSym.visibility = Visibility::Protected;
            symbols.addFunction(stageBSym);

            lb.calledFunctions.push_back("ns::stage_b");
            analysis.stages["ns::stage_b"] = 0; // Same stage = concurrent

            mapping.matched["ns::stage_b"] = stageBFunc;
            entries.push_back({"ns::stage_b", Visibility::Protected});
        }

        analysis.sourceNodes = {"stage_a"};
        analysis.terminalNode = addConcurrentStage ? "stage_b" : "stage_a";
        lb.pipelineAnalysis = analysis;
        symbols.addLogicBlock(lb);

        mapping.matched["ns::pipeline"] = pipelineFunc;
        mapping.matched["ns::stage_a"] = stageFunc;
        entries.push_back({"ns::pipeline", Visibility::Public});
        entries.push_back({"ns::stage_a", Visibility::Protected});
    }
};

// Helper: create struct with non-standard names + ownership in SymbolTable
struct OwnershipTestModule {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::StructType* structTy = nullptr;
    llvm::Function* testFunc = nullptr;
    std::vector<VisibilityEntry> entries;

    /// Build a function that takes a struct by value (via alloca + store from arg).
    /// The struct has a pointer field (simulates smart pointer internals).
    /// @param structName   LLVM struct name (e.g. "struct.MyBox" — no "unique_ptr")
    /// @param ownership    Ownership kind to declare in SymbolTable
    /// @param addSecondLoad  Whether to add a second GEP+Load (enables deref opt)
    void build(llvm::LLVMContext& ctx,
               const std::string& structName,
               OwnershipKind ownership,
               bool addSecondLoad = true) {
        module = std::make_unique<llvm::Module>("test_ownership", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);

        // Struct with a pointer field (like a smart pointer)
        structTy = llvm::StructType::create(ctx, {ptrTy}, structName);

        // Function takes the struct as argument
        auto* funcTy = llvm::FunctionType::get(i32Ty, {structTy}, false);
        testFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "process", *module);

        auto* bb = llvm::BasicBlock::Create(ctx, "entry", testFunc);
        llvm::IRBuilder<> builder(bb);

        // Alloca for the struct param + store from arg (enables isOwnedAlloca)
        auto* alloca = builder.CreateAlloca(structTy, nullptr, "param");
        builder.CreateStore(testFunc->getArg(0), alloca);

        // First GEP+Load of internal pointer
        auto* gep1 = builder.CreateStructGEP(structTy, alloca, 0, "ptr.gep1");
        auto* load1 = builder.CreateLoad(ptrTy, gep1, "ptr.load1");

        if (addSecondLoad) {
            // Second redundant load (enables promotion)
            auto* gep2 = builder.CreateStructGEP(structTy, alloca, 0, "ptr.gep2");
            auto* load2 = builder.CreateLoad(ptrTy, gep2, "ptr.load2");
            auto* v1 = builder.CreatePtrToInt(load1, i32Ty);
            auto* v2 = builder.CreatePtrToInt(load2, i32Ty);
            auto* sum = builder.CreateAdd(v1, v2);
            builder.CreateRet(sum);
        } else {
            auto* v1 = builder.CreatePtrToInt(load1, i32Ty);
            builder.CreateRet(v1);
        }

        // Register function in SymbolTable with ownership
        FunctionSymbol sym;
        sym.qualifiedName = "ns::process";
        sym.simpleName = "process";
        sym.visibility = Visibility::Protected;
        Parameter param;
        param.name = "arg";
        param.type.ownership = ownership;
        param.type.nameParts = {"Node"};
        sym.params.push_back(param);
        symbols.addFunction(sym);

        mapping.matched["ns::process"] = testFunc;
        entries.push_back({"ns::process", Visibility::Protected});
    }

    /// Build a pipeline module with a stage function that uses the struct.
    /// Needed for shared_ptr auto-deref (which only runs in exclusive stages).
    void buildPipeline(llvm::LLVMContext& ctx,
                       const std::string& structName,
                       OwnershipKind ownership,
                       bool addSecondLoad = true) {
        module = std::make_unique<llvm::Module>("test_ownership_pipe", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);

        structTy = llvm::StructType::create(ctx, {ptrTy}, structName);

        // Stage function takes struct by value
        auto* funcTy = llvm::FunctionType::get(i32Ty, {structTy}, false);
        testFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "stage_fn", *module);

        auto* bb = llvm::BasicBlock::Create(ctx, "entry", testFunc);
        llvm::IRBuilder<> builder(bb);

        auto* alloca = builder.CreateAlloca(structTy, nullptr, "param");
        builder.CreateStore(testFunc->getArg(0), alloca);

        auto* gep1 = builder.CreateStructGEP(structTy, alloca, 0, "ptr.gep1");
        auto* load1 = builder.CreateLoad(ptrTy, gep1, "ptr.load1");

        if (addSecondLoad) {
            auto* gep2 = builder.CreateStructGEP(structTy, alloca, 0, "ptr.gep2");
            auto* load2 = builder.CreateLoad(ptrTy, gep2, "ptr.load2");
            auto* v1 = builder.CreatePtrToInt(load1, i32Ty);
            auto* v2 = builder.CreatePtrToInt(load2, i32Ty);
            builder.CreateRet(builder.CreateAdd(v1, v2));
        } else {
            builder.CreateRet(builder.CreatePtrToInt(load1, i32Ty));
        }

        // Register as pipeline stage
        FunctionSymbol stageSym;
        stageSym.qualifiedName = "ns::stage_fn";
        stageSym.simpleName = "stage_fn";
        stageSym.visibility = Visibility::Protected;
        Parameter param;
        param.name = "arg";
        param.type.ownership = ownership;
        param.type.nameParts = {"Texture"};
        stageSym.params.push_back(param);
        symbols.addFunction(stageSym);

        LogicBlockEntry lb;
        lb.qualifiedName = "ns::pipeline";
        lb.simpleName = "pipeline";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::stage_fn"};

        PipelineAnalysis analysis;
        analysis.stages = {{"ns::stage_fn", 0}};
        analysis.sourceNodes = {"stage_fn"};
        analysis.terminalNode = "stage_fn";
        lb.pipelineAnalysis = analysis;
        symbols.addLogicBlock(lb);

        mapping.matched["ns::stage_fn"] = testFunc;
        entries.push_back({"ns::stage_fn", Visibility::Protected});
    }
};

// ==================== Ownership-aware tests ====================

TEST_F(IndirectionPassTest, Owned_NoNameMatch_Promoted) {
    // struct.MyBox has no "unique_ptr" in its name — only OwnershipKind::Owned
    // should trigger promotion
    llvm::LLVMContext ctx;
    OwnershipTestModule tm;
    tm.build(ctx, "struct.MyBox", OwnershipKind::Owned);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_GE(stats.uniquePtrPromoted, 1) << "Owned alloca should trigger promotion even without 'unique_ptr' in name";
}

TEST_F(IndirectionPassTest, Shared_NoNameMatch_Detected) {
    // struct.MyRef has no "shared_ptr" in name — OwnershipKind::Shared in a
    // pipeline exclusive stage should trigger atomic downgrade
    llvm::LLVMContext ctx;
    OwnershipTestModule tm;
    tm.buildPipeline(ctx, "struct.MyRef", OwnershipKind::Shared);

    // Insert an AtomicRMW SeqCst in the stage function to be downgraded
    auto& entryBB = tm.testFunc->getEntryBlock();
    llvm::IRBuilder<> builder(&entryBB, entryBB.getTerminator()->getIterator());
    auto* ptrTy = llvm::PointerType::get(ctx, 0);
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);
    auto* alloca = builder.CreateAlloca(i64Ty, nullptr, "ctrl");
    builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add,
                            alloca,
                            llvm::ConstantInt::get(i64Ty, 1),
                            llvm::MaybeAlign(8),
                            llvm::AtomicOrdering::SequentiallyConsistent);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    // The shared_ptr auto-deref + atomic downgrade should fire
    EXPECT_GE(stats.sharedPtrOptimized + stats.sharedPtrDereferenced, 1)
        << "Shared alloca in exclusive stage should trigger optimization";
}

TEST_F(IndirectionPassTest, Weak_NoAutoDeref) {
    // OwnershipKind::Weak should NOT trigger any promotion or deref
    llvm::LLVMContext ctx;
    OwnershipTestModule tm;
    tm.build(ctx, "struct.WeakHandle", OwnershipKind::Weak);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_EQ(stats.uniquePtrPromoted, 0) << "Weak ownership should not trigger unique_ptr promotion";
}

TEST_F(IndirectionPassTest, Owned_WithAutoDeref_Nonnull) {
    // Verify that owned path produces loads with nonnull and invariant.load metadata
    llvm::LLVMContext ctx;
    OwnershipTestModule tm;
    tm.build(ctx, "struct.OwnedResource", OwnershipKind::Owned);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);

    bool foundNonnull = false;
    bool foundInvariant = false;
    for (auto& bb : *tm.testFunc) {
        for (auto& inst : bb) {
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                if (load->getMetadata("nonnull")) foundNonnull = true;
                if (load->getMetadata("invariant.load")) foundInvariant = true;
            }
        }
    }
    EXPECT_TRUE(foundNonnull) << "Loads from owned alloca should have nonnull metadata";
    EXPECT_TRUE(foundInvariant) << "Loads from owned alloca should have invariant.load metadata";
}

TEST_F(IndirectionPassTest, Shared_BatchElimination) {
    // Adjacent sub-1 / add-1 AtomicRMW on same pointer should be eliminated
    llvm::LLVMContext ctx;
    OwnershipTestModule tm;
    tm.buildPipeline(ctx,
                     "struct.SharedRes",
                     OwnershipKind::Shared,
                     /*addSecondLoad=*/false);

    // Insert sub-1 then add-1 on same pointer (simulates dec/inc pair)
    auto& entryBB = tm.testFunc->getEntryBlock();
    llvm::IRBuilder<> builder(&entryBB, entryBB.getTerminator()->getIterator());
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);
    auto* ctrlAlloca = builder.CreateAlloca(i64Ty, nullptr, "refcnt");

    builder.CreateAtomicRMW(llvm::AtomicRMWInst::Sub,
                            ctrlAlloca,
                            llvm::ConstantInt::get(i64Ty, 1),
                            llvm::MaybeAlign(8),
                            llvm::AtomicOrdering::AcquireRelease);
    builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add,
                            ctrlAlloca,
                            llvm::ConstantInt::get(i64Ty, 1),
                            llvm::MaybeAlign(8),
                            llvm::AtomicOrdering::AcquireRelease);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_GE(stats.refcountEliminated, 2) << "Adjacent sub-1/add-1 pair should be downgraded";

    // Verify no AtomicRMW instructions remain (replaced with non-atomic ops)
    int atomicCount = 0;
    int loadCount = 0;
    int storeCount = 0;
    for (auto& bb : *tm.testFunc) {
        for (auto& inst : bb) {
            if (llvm::isa<llvm::AtomicRMWInst>(&inst)) ++atomicCount;
            if (llvm::isa<llvm::LoadInst>(&inst)) ++loadCount;
            if (llvm::isa<llvm::StoreInst>(&inst)) ++storeCount;
        }
    }
    EXPECT_EQ(atomicCount, 0) << "Atomic operations should have been replaced with non-atomic equivalents";
    // Each atomic op becomes load+op+store, so we expect at least 2 new loads
    // and 2 new stores (one pair per original atomic)
    EXPECT_GE(loadCount, 2) << "Non-atomic load replacements should be present";
    EXPECT_GE(storeCount, 2) << "Non-atomic store replacements should be present";
}

// ==================== 10a unique_ptr tests ====================

TEST_F(IndirectionPassTest, Disabled_NoChanges) {
    llvm::LLVMContext ctx;
    UniquePtrTestModule tm;
    tm.build(ctx);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Off;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_EQ(stats.total(), 0);
}

TEST_F(IndirectionPassTest, UniquePtr_Promoted) {
    llvm::LLVMContext ctx;
    UniquePtrTestModule tm;
    tm.build(ctx, /*addStore=*/false);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_GE(stats.uniquePtrPromoted, 1);
}

TEST_F(IndirectionPassTest, UniquePtr_StoreSkip) {
    llvm::LLVMContext ctx;
    UniquePtrTestModule tm;
    tm.build(ctx, /*addStore=*/true);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_EQ(stats.uniquePtrPromoted, 0);
}

TEST_F(IndirectionPassTest, UniquePtr_MoveSkip) {
    // unique_ptr that escapes (passed to external call) should not be promoted
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_move", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    auto* uptrTy = llvm::StructType::create(ctx, {ptrTy}, "class.std::unique_ptr.move");

    // External function that takes the unique_ptr
    auto* extTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy}, false);
    auto* extFunc = llvm::Function::Create(extTy, llvm::GlobalValue::ExternalLinkage, "external_sink", *module);
    (void)extFunc;

    auto* funcTy = llvm::FunctionType::get(i32Ty, {}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "test_move", *module);

    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    auto* alloca = builder.CreateAlloca(uptrTy, nullptr, "uptr");

    // Load internal pointer
    auto* gep = builder.CreateStructGEP(uptrTy, alloca, 0);
    auto* load = builder.CreateLoad(ptrTy, gep);

    // Pass alloca to external function (escape!)
    builder.CreateCall(extFunc, {alloca});

    auto* intVal = builder.CreatePtrToInt(load, i32Ty);
    builder.CreateRet(intVal);

    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*module, entries, mapping, symbols, config);
    EXPECT_EQ(stats.uniquePtrPromoted, 0);
}

TEST_F(IndirectionPassTest, UniquePtr_NonnullMetadata) {
    llvm::LLVMContext ctx;
    UniquePtrTestModule tm;
    tm.build(ctx, /*addStore=*/false);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);

    // Check that loads have nonnull and invariant.load metadata
    bool foundNonnull = false;
    bool foundInvariant = false;
    for (auto& bb : *tm.testFunc) {
        for (auto& inst : bb) {
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                if (load->getMetadata("nonnull")) foundNonnull = true;
                if (load->getMetadata("invariant.load")) foundInvariant = true;
            }
        }
    }
    EXPECT_TRUE(foundNonnull);
    EXPECT_TRUE(foundInvariant) << "Loads should be marked with !invariant.load for LLVM GVN/EarlyCSE";
}

// ==================== 10b shared_ptr tests ====================

TEST_F(IndirectionPassTest, SharedPtr_ExclusiveStage) {
    // shared_ptr in exclusive stage should have atomics downgraded
    // This is a structural test -- the actual optimization depends on
    // finding AtomicRMW instructions in the IR
    llvm::LLVMContext ctx;
    PipelineTestModule tm;
    tm.build(ctx, /*addConcurrentStage=*/false);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    // No AtomicRMW in our test IR, so 0 optimized
    EXPECT_EQ(stats.sharedPtrOptimized, 0);
}

TEST_F(IndirectionPassTest, SharedPtr_ConcurrentSkip) {
    llvm::LLVMContext ctx;
    PipelineTestModule tm;
    tm.build(ctx, /*addConcurrentStage=*/true);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_EQ(stats.sharedPtrOptimized, 0);
}

TEST_F(IndirectionPassTest, SharedPtr_SequentialOpt) {
    // Two stages in sequence (different stage numbers) -- both are exclusive
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_seq", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    auto* stageTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
    auto* stageA = llvm::Function::Create(stageTy, llvm::GlobalValue::ExternalLinkage, "seqA", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageA);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 1));
    }
    auto* stageB = llvm::Function::Create(stageTy, llvm::GlobalValue::ExternalLinkage, "seqB", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageB);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 2));
    }

    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;

    FunctionSymbol symA;
    symA.qualifiedName = "ns::seqA";
    symA.simpleName = "seqA";
    symA.visibility = Visibility::Protected;
    symbols.addFunction(symA);

    FunctionSymbol symB;
    symB.qualifiedName = "ns::seqB";
    symB.simpleName = "seqB";
    symB.visibility = Visibility::Protected;
    symbols.addFunction(symB);

    LogicBlockEntry lb;
    lb.qualifiedName = "ns::pipe";
    lb.simpleName = "pipe";
    lb.isPipeline = true;
    lb.calledFunctions = {"ns::seqA", "ns::seqB"};

    PipelineAnalysis analysis;
    analysis.stages = {{"ns::seqA", 0}, {"ns::seqB", 1}}; // Sequential stages
    lb.pipelineAnalysis = analysis;
    symbols.addLogicBlock(lb);

    mapping.matched["ns::seqA"] = stageA;
    mapping.matched["ns::seqB"] = stageB;

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*module, entries, mapping, symbols, config);
    // Sequential stages are both exclusive, but no atomics to optimize
    EXPECT_EQ(stats.sharedPtrOptimized, 0);
}

// ==================== 10c vector tests ====================

TEST_F(IndirectionPassTest, Vector_NoResize) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_vec", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    // vector-like struct: { ptr, ptr, ptr } (begin, end, capacity)
    auto* vecTy = llvm::StructType::create(ctx, {ptrTy, ptrTy, ptrTy}, "class.std::vector");

    auto* funcTy = llvm::FunctionType::get(i32Ty, {}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "vec_read", *module);

    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    auto* alloca = builder.CreateAlloca(vecTy, nullptr, "vec");

    // Initialize the begin pointer (field 0) so the span lowering reads a
    // constructed value rather than uninitialized stack. A backing buffer
    // alloca stands in for the heap allocation.
    auto* storage = builder.CreateAlloca(i32Ty, nullptr, "storage");
    auto* beginGep = builder.CreateStructGEP(vecTy, alloca, 0, "vec.begin.init");
    builder.CreateStore(storage, beginGep);

    // Only read from field 0 (begin pointer) -- no resize
    auto* gep = builder.CreateStructGEP(vecTy, alloca, 0);
    auto* load = builder.CreateLoad(ptrTy, gep);
    auto* intVal = builder.CreatePtrToInt(load, i32Ty);
    builder.CreateRet(intVal);

    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*module, entries, mapping, symbols, config);
    EXPECT_GE(stats.vectorLowered, 1);
}

TEST_F(IndirectionPassTest, Vector_ResizeSkip) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_vec_resize", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    auto* vecTy = llvm::StructType::create(ctx, {ptrTy, ptrTy, ptrTy}, "class.std::vector.resize");

    auto* funcTy = llvm::FunctionType::get(i32Ty, {}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "vec_resize", *module);

    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    auto* alloca = builder.CreateAlloca(vecTy, nullptr, "vec");

    // Store to field 1 (end pointer) -- simulates resize
    auto* endGep = builder.CreateStructGEP(vecTy, alloca, 1);
    auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
    builder.CreateStore(nullPtr, endGep);

    builder.CreateRet(llvm::ConstantInt::get(i32Ty, 0));

    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*module, entries, mapping, symbols, config);
    EXPECT_EQ(stats.vectorLowered, 0);
}

TEST_F(IndirectionPassTest, Vector_NonnullAttr) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_vec_nonnull", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    auto* vecTy = llvm::StructType::create(ctx, {ptrTy, ptrTy, ptrTy}, "class.std::vector.nonnull");

    auto* funcTy = llvm::FunctionType::get(i32Ty, {}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "vec_nonnull", *module);

    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    auto* alloca = builder.CreateAlloca(vecTy, nullptr, "vec");

    // Initialize the begin pointer (field 0) with a provably non-null value
    // (a backing-buffer alloca). nonnull is only sound once the field holds a
    // constructed, non-null pointer.
    auto* storage = builder.CreateAlloca(i32Ty, nullptr, "storage");
    auto* beginGep = builder.CreateStructGEP(vecTy, alloca, 0, "vec.begin.init");
    builder.CreateStore(storage, beginGep);

    auto* gep = builder.CreateStructGEP(vecTy, alloca, 0);
    auto* load = builder.CreateLoad(ptrTy, gep);
    auto* intVal = builder.CreatePtrToInt(load, i32Ty);
    builder.CreateRet(intVal);

    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.pointerAttrInference = false;

    IndirectionPass::run(*module, entries, mapping, symbols, config);

    // Check for nonnull metadata on the promoted data pointer load
    bool foundNonnull = false;
    for (auto& fnBB : *func) {
        for (auto& inst : fnBB) {
            if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                if (loadInst->getMetadata("nonnull")) {
                    foundNonnull = true;
                }
            }
        }
    }
    EXPECT_TRUE(foundNonnull);
}

// ==================== 10d pointer attr tests ====================

TEST_F(IndirectionPassTest, PointerAttr_SkippedWhenNoTransforms) {
    // 10d is gated: pointer attr inference only runs when 10a/10b/10c
    // actually fired. With all three disabled, no attrs should be added.
    llvm::LLVMContext ctx;
    PipelineTestModule tm;
    tm.build(ctx, /*addConcurrentStage=*/false);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_EQ(stats.pointerAttrsAdded, 0);

    // Pointer arg should NOT have nonnull since 10d was skipped
    EXPECT_FALSE(tm.stageFunc->getArg(0)->hasNonNullAttr());
}

TEST_F(IndirectionPassTest, PointerAttr_NoaliasSkippedWhenNoTransforms) {
    // Same gating: noalias should not be inferred when no other transforms ran
    llvm::LLVMContext ctx;
    PipelineTestModule tm;
    tm.build(ctx, /*addConcurrentStage=*/false); // Single stage = exclusive

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_EQ(stats.pointerAttrsAdded, 0);

    // Single stage -- noalias should NOT be added since 10d was skipped
    EXPECT_FALSE(tm.stageFunc->getArg(0)->hasAttribute(llvm::Attribute::NoAlias));
}

TEST_F(IndirectionPassTest, PointerAttr_ExternalBlocks) {
    // External call site blocks nonnull inference
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_ext", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    auto* stageTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
    auto* stageFunc = llvm::Function::Create(stageTy, llvm::GlobalValue::ExternalLinkage, "ext_stage", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageFunc);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
    }

    // External caller that passes unknown pointer
    auto* callerTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy}, false);
    auto* caller = llvm::Function::Create(callerTy, llvm::GlobalValue::ExternalLinkage, "external_caller", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", caller);
        llvm::IRBuilder<> b(bb);
        // Pass raw arg (not alloca/GEP -- unknown provenance)
        b.CreateCall(stageFunc, {caller->getArg(0)});
        b.CreateRetVoid();
    }

    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;

    FunctionSymbol sym;
    sym.qualifiedName = "ns::ext_stage";
    sym.simpleName = "ext_stage";
    sym.visibility = Visibility::Protected;
    symbols.addFunction(sym);

    LogicBlockEntry lb;
    lb.qualifiedName = "ns::pipe";
    lb.simpleName = "pipe";
    lb.isPipeline = true;
    lb.calledFunctions = {"ns::ext_stage"};
    PipelineAnalysis analysis;
    analysis.stages = {{"ns::ext_stage", 0}};
    lb.pipelineAnalysis = analysis;
    symbols.addLogicBlock(lb);

    mapping.matched["ns::ext_stage"] = stageFunc;

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;

    auto stats = IndirectionPass::run(*module, entries, mapping, symbols, config);
    // External caller passes unknown pointer -- nonnull should NOT be inferred
    EXPECT_FALSE(stageFunc->getArg(0)->hasNonNullAttr());
}

// ==================== General tests ====================

TEST_F(IndirectionPassTest, NonTopoFunction_Skip) {
    // Functions not in mapping should be unaffected
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_nontopo", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);

    auto* funcTy = llvm::FunctionType::get(i32Ty, {}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "random_func", *module);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRet(llvm::ConstantInt::get(i32Ty, 42));

    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;
    // Empty mapping -- no Topo functions

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;

    auto stats = IndirectionPass::run(*module, entries, mapping, symbols, config);
    EXPECT_EQ(stats.total(), 0);
}

TEST_F(IndirectionPassTest, O0_Skip) {
    // IndirectionPass should not run when disabled
    llvm::LLVMContext ctx;
    UniquePtrTestModule tm;
    tm.build(ctx);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Off; // Simulates O0

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_EQ(stats.total(), 0);
}

// ==================== 10e devirtualization tests ====================

/// Helper: create a module with a vtable-based indirect call in a pipeline stage.
/// Base class has a virtual method; derived class overrides it.
/// The pipeline stage function receives a parameter typed as the concrete derived class.
struct DevirtTestModule {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* stageFunc = nullptr;
    llvm::Function* derivedMethod = nullptr;
    std::vector<VisibilityEntry> entries;

    void build(llvm::LLVMContext& ctx, bool useConcrete = true) {
        module = std::make_unique<llvm::Module>("test_devirt", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);

        // The derived class method (target of devirtualization)
        auto* methodTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
        derivedMethod =
            llvm::Function::Create(methodTy, llvm::GlobalValue::ExternalLinkage, "DerivedShape_area", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", derivedMethod);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(llvm::ConstantInt::get(i32Ty, 42));
        }

        // Stage function: receives object pointer, does vtable dispatch
        auto* stageTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
        stageFunc = llvm::Function::Create(stageTy, llvm::GlobalValue::ExternalLinkage, "compute_stage", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageFunc);
            llvm::IRBuilder<> b(bb);

            auto* objPtr = stageFunc->getArg(0);

            // Simulate vtable dispatch:
            //   %vtable = load ptr, ptr %objPtr        ; load vtable
            //   %fptr   = getelementptr ptr, %vtable, 0 ; first virtual method
            //   %target = load ptr, ptr %fptr           ; load function pointer
            //   %result = call i32 %target(ptr %objPtr) ; indirect call
            auto* vtable = b.CreateLoad(ptrTy, objPtr, "vtable");
            auto* fptr = b.CreateGEP(ptrTy, vtable, llvm::ConstantInt::get(i32Ty, 0), "fptr");
            auto* target = b.CreateLoad(ptrTy, fptr, "target");
            auto* result = b.CreateCall(llvm::FunctionType::get(i32Ty, {ptrTy}, false), target, {objPtr});
            b.CreateRet(result);
        }

        // --- SymbolTable setup ---

        // Base class: Shape with virtual method "area"
        ClassSymbol baseCls;
        baseCls.qualifiedName = "gfx::Shape";
        baseCls.simpleName = "Shape";
        baseCls.visibility = Visibility::Public;
        baseCls.memberFunctions = {"gfx::Shape::area"};
        symbols.addClassSymbol(baseCls);

        // Derived class: DerivedShape overrides "area"
        ClassSymbol derivedCls;
        derivedCls.qualifiedName = "gfx::DerivedShape";
        derivedCls.simpleName = "DerivedShape";
        derivedCls.visibility = Visibility::Public;
        TypeNode baseType;
        baseType.nameParts = {"gfx", "Shape"};
        derivedCls.baseClass = baseType;
        derivedCls.memberFunctions = {"gfx::DerivedShape::area"};
        symbols.addClassSymbol(derivedCls);

        // Base class method symbol
        FunctionSymbol baseMethodSym;
        baseMethodSym.qualifiedName = "gfx::Shape::area";
        baseMethodSym.simpleName = "area";
        baseMethodSym.visibility = Visibility::Public;
        symbols.addFunction(baseMethodSym);

        // Derived class method symbol
        FunctionSymbol derivedMethodSym;
        derivedMethodSym.qualifiedName = "gfx::DerivedShape::area";
        derivedMethodSym.simpleName = "area";
        derivedMethodSym.visibility = Visibility::Public;
        symbols.addFunction(derivedMethodSym);

        // Stage function symbol — parameter type is concrete DerivedShape
        FunctionSymbol stageSym;
        stageSym.qualifiedName = "gfx::compute_stage";
        stageSym.simpleName = "compute_stage";
        stageSym.visibility = Visibility::Protected;
        Parameter param;
        param.name = "shape";
        if (useConcrete) {
            param.type.nameParts = {"gfx", "DerivedShape"}; // Concrete type
        } else {
            param.type.nameParts = {"gfx", "Shape"}; // Base type (abstract)
        }
        param.type.modifier = TypeNode::Ptr;
        stageSym.params.push_back(param);
        symbols.addFunction(stageSym);

        // Pipeline logic block
        LogicBlockEntry lb;
        lb.qualifiedName = "gfx::render";
        lb.simpleName = "render";
        lb.isPipeline = true;
        lb.calledFunctions = {"gfx::compute_stage"};
        PipelineAnalysis analysis;
        analysis.stages = {{"gfx::compute_stage", 0}};
        analysis.sourceNodes = {"compute_stage"};
        analysis.terminalNode = "compute_stage";
        lb.pipelineAnalysis = analysis;
        symbols.addLogicBlock(lb);

        // LLVM function mappings
        mapping.matched["gfx::compute_stage"] = stageFunc;
        mapping.matched["gfx::DerivedShape::area"] = derivedMethod;
        entries.push_back({"gfx::compute_stage", Visibility::Protected});
        entries.push_back({"gfx::DerivedShape::area", Visibility::Public});
    }
};

TEST_F(IndirectionPassTest, Devirt_ConcreteTypeResolved) {
    // When the .topo declaration specifies a concrete derived type for a
    // pipeline stage parameter, the virtual call should be devirtualized.
    llvm::LLVMContext ctx;
    DevirtTestModule tm;
    tm.build(ctx, /*useConcrete=*/true);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_GE(stats.callsDevirtualized, 1) << "Virtual call should be devirtualized when concrete type is known";

    // Verify the call is now direct
    bool foundDirectCall = false;
    for (auto& bb : *tm.stageFunc) {
        for (auto& inst : bb) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() == tm.derivedMethod) foundDirectCall = true;
            }
        }
    }
    EXPECT_TRUE(foundDirectCall) << "Indirect call should be replaced with direct call to derived method";
}

TEST_F(IndirectionPassTest, Devirt_InlineHintAdded) {
    // Devirtualized targets should get the inlinehint attribute.
    llvm::LLVMContext ctx;
    DevirtTestModule tm;
    tm.build(ctx, /*useConcrete=*/true);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);

    EXPECT_TRUE(tm.derivedMethod->hasFnAttribute(llvm::Attribute::InlineHint))
        << "Devirtualized target should have inlinehint attribute";
}

TEST_F(IndirectionPassTest, Devirt_BaseTypeNotResolved) {
    // When the parameter type is the base class (not concrete), and the base
    // class has multiple derived classes, devirtualization should NOT occur.
    llvm::LLVMContext ctx;
    DevirtTestModule tm;
    tm.build(ctx, /*useConcrete=*/false);

    // Add a second derived class to prevent unique-derived resolution
    ClassSymbol secondDerived;
    secondDerived.qualifiedName = "gfx::CircleShape";
    secondDerived.simpleName = "CircleShape";
    secondDerived.visibility = Visibility::Public;
    TypeNode baseType;
    baseType.nameParts = {"gfx", "Shape"};
    secondDerived.baseClass = baseType;
    secondDerived.memberFunctions = {"gfx::CircleShape::area"};
    tm.symbols.addClassSymbol(secondDerived);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_EQ(stats.callsDevirtualized, 0) << "Virtual call should NOT be devirtualized when concrete type is unknown";
}

TEST_F(IndirectionPassTest, Devirt_DisabledByConfig) {
    // The devirtualize sub-switch can be turned off independently.
    llvm::LLVMContext ctx;
    DevirtTestModule tm;
    tm.build(ctx, /*useConcrete=*/true);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;
    config.devirtualize = false; // Explicitly disabled

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    EXPECT_EQ(stats.callsDevirtualized, 0) << "Devirtualization should not run when config.devirtualize is false";
}

// ==================== Auto-benchmark tests ====================

TEST_F(IndirectionPassTest, AutoMode_BenchmarkAppliesWhenBeneficial) {
    // Auto mode should apply optimizations when IndirectionPass produces
    // beneficial IR changes (unique_ptr promotion reduces redundant loads).
    // This test verifies the pass still runs and produces results in Auto mode
    // — the actual benchmark decision happens in PassPipeline, but the pass
    // itself must accept Auto as a valid enabled mode.
    llvm::LLVMContext ctx;
    UniquePtrTestModule tm;
    tm.build(ctx);

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Auto;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;

    auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
    // Auto mode should still apply (pass-level Auto behaves as enabled)
    EXPECT_GE(stats.uniquePtrPromoted, 1);
}

TEST_F(IndirectionPassTest, AutoMode_ForceConfigOverride) {
    // Verify that overriding Auto→Force in config produces identical results.
    // This is the mechanism PassPipeline uses after benchmark decision.
    // Run Auto first, then Force in a separate scope to avoid context conflicts.
    int autoPromoted = 0;
    {
        llvm::LLVMContext ctx;
        UniquePtrTestModule tm;
        tm.build(ctx);

        IndirectionConfig config;
        config.mode = topo::FeatureMode::Auto;
        config.sharedPtrExclusive = false;
        config.vectorSpanLowering = false;
        config.pointerAttrInference = false;

        auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
        autoPromoted = stats.uniquePtrPromoted;
    }

    int forcePromoted = 0;
    {
        llvm::LLVMContext ctx;
        UniquePtrTestModule tm;
        tm.build(ctx);

        IndirectionConfig config;
        config.mode = topo::FeatureMode::Force;
        config.sharedPtrExclusive = false;
        config.vectorSpanLowering = false;
        config.pointerAttrInference = false;

        auto stats = IndirectionPass::run(*tm.module, tm.entries, tm.mapping, tm.symbols, config);
        forcePromoted = stats.uniquePtrPromoted;
    }

    EXPECT_EQ(autoPromoted, forcePromoted);
    EXPECT_GE(autoPromoted, 1);
}

// ==================== Devirt safety regression tests ====================

// Regression for the speculative-devirt InvokeInst crash: an indirect virtual
// call inside an EH scope lowers to an indirect `invoke` (a terminator). The
// old code collected it as a speculative-devirt candidate and called
// splitBasicBlock(call->getNextNode()) — but a terminator has no next node, so
// getNextNode() is null and the split dereferenced it. With multiple derived
// types declared the call IS a speculative-devirt candidate, so this exercises
// the exact path. The pass must not crash and must leave valid IR.
TEST_F(IndirectionPassTest, SpeculativeDevirt_IndirectInvoke_NoCrash) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_devirt_invoke", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    // Personality function (required for invoke + landingpad)
    auto* personalityTy = llvm::FunctionType::get(i32Ty, true);
    auto* personality =
        llvm::Function::Create(personalityTy, llvm::GlobalValue::ExternalLinkage, "__gxx_personality_v0", *module);

    // Stage function: receives object pointer, dispatches via vtable using an
    // INVOKE (not a call) — i.e. a virtual call in a try block.
    auto* stageTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
    auto* stageFunc = llvm::Function::Create(stageTy, llvm::GlobalValue::ExternalLinkage, "eh_stage", *module);
    stageFunc->setPersonalityFn(personality);

    auto* entry = llvm::BasicBlock::Create(ctx, "entry", stageFunc);
    auto* normalBB = llvm::BasicBlock::Create(ctx, "cont", stageFunc);
    auto* lpadBB = llvm::BasicBlock::Create(ctx, "lpad", stageFunc);
    {
        llvm::IRBuilder<> b(entry);
        auto* objPtr = stageFunc->getArg(0);
        // vtable dispatch pattern: load vtable, GEP slot 0, load fptr.
        auto* vtable = b.CreateLoad(ptrTy, objPtr, "vtable");
        auto* fptr = b.CreateGEP(ptrTy, vtable, llvm::ConstantInt::get(i32Ty, 0), "fptr");
        auto* target = b.CreateLoad(ptrTy, fptr, "target");
        // Indirect INVOKE through the loaded function pointer (terminator).
        b.CreateInvoke(llvm::FunctionType::get(i32Ty, {ptrTy}, false), target, normalBB, lpadBB, {objPtr});
    }
    {
        llvm::IRBuilder<> b(lpadBB);
        auto* lp = b.CreateLandingPad(llvm::StructType::get(ptrTy, i32Ty), 0);
        lp->setCleanup(true);
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
    }
    {
        llvm::IRBuilder<> b(normalBB);
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 1));
    }

    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;

    // Base Shape with two derived types -> multiple-derived => speculative path.
    ClassSymbol baseCls;
    baseCls.qualifiedName = "gfx::Shape";
    baseCls.simpleName = "Shape";
    baseCls.visibility = Visibility::Public;
    baseCls.memberFunctions = {"gfx::Shape::area"};
    symbols.addClassSymbol(baseCls);

    for (const char* d : {"gfx::Rect", "gfx::Circle"}) {
        ClassSymbol derived;
        derived.qualifiedName = d;
        derived.simpleName = d;
        derived.visibility = Visibility::Public;
        TypeNode baseType;
        baseType.nameParts = {"gfx", "Shape"};
        derived.baseClass = baseType;
        derived.memberFunctions = {std::string(d) + "::area"};
        symbols.addClassSymbol(derived);
    }

    FunctionSymbol stageSym;
    stageSym.qualifiedName = "gfx::eh_stage";
    stageSym.simpleName = "eh_stage";
    stageSym.visibility = Visibility::Protected;
    Parameter param;
    param.name = "shape";
    param.type.nameParts = {"gfx", "Shape"}; // base type with multiple derived
    param.type.modifier = TypeNode::Ptr;
    stageSym.params.push_back(param);
    symbols.addFunction(stageSym);

    LogicBlockEntry lb;
    lb.qualifiedName = "gfx::render";
    lb.simpleName = "render";
    lb.isPipeline = true;
    lb.calledFunctions = {"gfx::eh_stage"};
    PipelineAnalysis analysis;
    analysis.stages = {{"gfx::eh_stage", 0}};
    lb.pipelineAnalysis = analysis;
    symbols.addLogicBlock(lb);

    mapping.matched["gfx::eh_stage"] = stageFunc;

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Force;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;
    config.devirtualize = false; // isolate the 10f speculative path
    config.vtableOptimize = true;

    // Must not crash on the indirect invoke...
    IndirectionPass::run(*module, entries, mapping, symbols, config);

    // ...and the invoke must still be intact and the IR valid (declined, not
    // miscompiled).
    EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs())) << "speculative devirt left invalid IR for indirect invoke";
    bool foundInvoke = false;
    for (auto& bb : *stageFunc) {
        for (auto& inst : bb) {
            if (llvm::isa<llvm::InvokeInst>(&inst)) foundInvoke = true;
        }
    }
    EXPECT_TRUE(foundInvoke) << "indirect invoke should be left untouched (speculative devirt declined)";
}

// Regression for the inferPointerAttrs out-of-bounds param read: when a stage
// function `func` is passed as an ARGUMENT to some other call g(func, ...) (so
// `func` is a user of a CallBase but not its callee), the old loop read
// call->getArgOperand(i) with func's parameter index against g's arg list —
// past the args when i >= g->arg_size(). The fix skips users where func is not
// the callee. Here func has a pointer param at index 1, but the only "call"
// using func passes it as g's single (index-0) argument; reading index 1 would
// be OOB. The pass must not crash and must not wrongly mark the param nonnull.
TEST_F(IndirectionPassTest, InferPointerAttrs_FuncAsArgument_NoOOB) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_infer_oob", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    // A vector type so a span-lowering fires — inferPointerAttrs is gated on at
    // least one other transform having fired (see IndirectionPass::run), so we
    // need a lowered vector to open that gate and reach the inference loop.
    auto* vecTy = llvm::StructType::create(ctx, {ptrTy, ptrTy, ptrTy}, "class.std::vector.oob");

    // Stage function: (i32, ptr) -> i32. Pointer param is at index 1.
    auto* stageTy = llvm::FunctionType::get(i32Ty, {i32Ty, ptrTy}, false);
    auto* stageFunc = llvm::Function::Create(stageTy, llvm::GlobalValue::ExternalLinkage, "stage_fn", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageFunc);
        llvm::IRBuilder<> b(bb);
        // Vector with an initialized begin pointer -> span lowering will fire.
        auto* vec = b.CreateAlloca(vecTy, nullptr, "vec");
        auto* storage = b.CreateAlloca(i32Ty, nullptr, "storage");
        auto* beginGep = b.CreateStructGEP(vecTy, vec, 0, "vec.begin.init");
        b.CreateStore(storage, beginGep);
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
    }

    // Another function g(ptr) -> void; its body calls g passing stageFunc as the
    // (single, index-0) pointer argument. So stageFunc is a user of a CallBase
    // where it is the ARGUMENT, not the callee. Index 1 against g's 1-arg list
    // is out of bounds.
    auto* gTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy}, false);
    auto* gFunc = llvm::Function::Create(gTy, llvm::GlobalValue::ExternalLinkage, "g_consumer", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", gFunc);
        llvm::IRBuilder<> b(bb);
        b.CreateCall(gFunc, {stageFunc}); // passes stageFunc as an argument
        b.CreateRetVoid();
    }

    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;

    FunctionSymbol stageSym;
    stageSym.qualifiedName = "ns::stage_fn";
    stageSym.simpleName = "stage_fn";
    stageSym.visibility = Visibility::Protected;
    symbols.addFunction(stageSym);

    LogicBlockEntry lb;
    lb.qualifiedName = "ns::pipe";
    lb.simpleName = "pipe";
    lb.isPipeline = true;
    lb.calledFunctions = {"ns::stage_fn"};
    PipelineAnalysis analysis;
    analysis.stages = {{"ns::stage_fn", 0}};
    lb.pipelineAnalysis = analysis;
    symbols.addLogicBlock(lb);

    mapping.matched["ns::stage_fn"] = stageFunc;

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Force;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = true;   // fires -> opens the inference gate
    config.pointerAttrInference = true; // exercise inferPointerAttrs
    config.devirtualize = false;
    config.vtableOptimize = false;

    // Must not crash / read OOB.
    auto stats = IndirectionPass::run(*module, entries, mapping, symbols, config);
    ASSERT_GE(stats.vectorLowered, 1) << "vector lowering must fire to reach inferPointerAttrs";

    // The only call using stageFunc passes it as an argument, never as the
    // callee, so there is no proven-non-null call site for param 1 — it must
    // NOT be marked nonnull.
    EXPECT_FALSE(stageFunc->getArg(1)->hasNonNullAttr())
        << "param wrongly marked nonnull from a call where func is an argument, not the callee";
    EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}

// Regression for the vtable-slot ordering defect: ClassSymbol::memberFunctions
// is declaration-ordered and includes static methods, but a vtable slot only
// counts instance methods. With a leading STATIC method, slot 0 must resolve to
// the first INSTANCE method, not the static one. The fix filters static methods
// out of the vtable view before indexing.
TEST_F(IndirectionPassTest, Devirt_StaticMethodSkippedInVtableOrder) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_devirt_static", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    auto* methodTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
    // The instance method that occupies vtable slot 0 (the intended target).
    auto* areaFn = llvm::Function::Create(methodTy, llvm::GlobalValue::ExternalLinkage, "Widget_area", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", areaFn);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 7));
    }
    // The static method that precedes it in declaration order (NOT in vtable).
    auto* makeFn = llvm::Function::Create(methodTy, llvm::GlobalValue::ExternalLinkage, "Widget_make", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", makeFn);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
    }

    auto* stageTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
    auto* stageFunc = llvm::Function::Create(stageTy, llvm::GlobalValue::ExternalLinkage, "use_widget", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageFunc);
        llvm::IRBuilder<> b(bb);
        auto* objPtr = stageFunc->getArg(0);
        auto* vtable = b.CreateLoad(ptrTy, objPtr, "vtable");
        auto* fptr = b.CreateGEP(ptrTy, vtable, llvm::ConstantInt::get(i32Ty, 0), "fptr"); // slot 0
        auto* target = b.CreateLoad(ptrTy, fptr, "target");
        auto* res = b.CreateCall(llvm::FunctionType::get(i32Ty, {ptrTy}, false), target, {objPtr});
        b.CreateRet(res);
    }

    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;

    // Concrete leaf class Widget; declaration order is [make (static), area].
    ClassSymbol widgetCls;
    widgetCls.qualifiedName = "ui::Widget";
    widgetCls.simpleName = "Widget";
    widgetCls.visibility = Visibility::Public;
    widgetCls.memberFunctions = {"ui::Widget::make", "ui::Widget::area"};
    symbols.addClassSymbol(widgetCls);

    FunctionSymbol makeSym;
    makeSym.qualifiedName = "ui::Widget::make";
    makeSym.simpleName = "make";
    makeSym.visibility = Visibility::Public;
    makeSym.isStatic = true; // static -> no vtable slot
    symbols.addFunction(makeSym);

    FunctionSymbol areaSym;
    areaSym.qualifiedName = "ui::Widget::area";
    areaSym.simpleName = "area";
    areaSym.visibility = Visibility::Public;
    symbols.addFunction(areaSym);

    FunctionSymbol stageSym;
    stageSym.qualifiedName = "ui::use_widget";
    stageSym.simpleName = "use_widget";
    stageSym.visibility = Visibility::Protected;
    Parameter param;
    param.name = "w";
    param.type.nameParts = {"ui", "Widget"}; // concrete leaf
    param.type.modifier = TypeNode::Ptr;
    stageSym.params.push_back(param);
    symbols.addFunction(stageSym);

    LogicBlockEntry lb;
    lb.qualifiedName = "ui::render";
    lb.simpleName = "render";
    lb.isPipeline = true;
    lb.calledFunctions = {"ui::use_widget"};
    PipelineAnalysis analysis;
    analysis.stages = {{"ui::use_widget", 0}};
    lb.pipelineAnalysis = analysis;
    symbols.addLogicBlock(lb);

    mapping.matched["ui::use_widget"] = stageFunc;
    mapping.matched["ui::Widget::make"] = makeFn;
    mapping.matched["ui::Widget::area"] = areaFn;

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Force;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;
    config.devirtualize = true;
    config.vtableOptimize = false;

    IndirectionPass::run(*module, entries, mapping, symbols, config);

    // Slot 0 must resolve to the instance method area(), never the static make().
    bool calledArea = false;
    bool calledMake = false;
    for (auto& bb : *stageFunc) {
        for (auto& inst : bb) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() == areaFn) calledArea = true;
                if (call->getCalledFunction() == makeFn) calledMake = true;
            }
        }
    }
    EXPECT_TRUE(calledArea) << "vtable slot 0 should resolve to first instance method (area)";
    EXPECT_FALSE(calledMake) << "vtable slot 0 must not resolve to the leading static method (make)";
    EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}

// Regression for the non-constant GEP index: a vtable GEP with a runtime
// (non-constant) index is not a recognizable slot, so isVtableCall must report
// no match rather than treating it as slot 0 (which would devirtualize to
// method[0]). With a runtime index the call must be left indirect.
TEST_F(IndirectionPassTest, Devirt_NonConstantSlot_NotResolved) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_devirt_runtime_idx", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    auto* methodTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
    auto* areaFn = llvm::Function::Create(methodTy, llvm::GlobalValue::ExternalLinkage, "D_area", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", areaFn);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 1));
    }

    // Stage takes (ptr obj, i32 slot) and indexes the vtable by the RUNTIME slot.
    auto* stageTy = llvm::FunctionType::get(i32Ty, {ptrTy, i32Ty}, false);
    auto* stageFunc = llvm::Function::Create(stageTy, llvm::GlobalValue::ExternalLinkage, "dyn_dispatch", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageFunc);
        llvm::IRBuilder<> b(bb);
        auto* objPtr = stageFunc->getArg(0);
        auto* slot = stageFunc->getArg(1); // non-constant index
        auto* vtable = b.CreateLoad(ptrTy, objPtr, "vtable");
        auto* fptr = b.CreateGEP(ptrTy, vtable, slot, "fptr");
        auto* target = b.CreateLoad(ptrTy, fptr, "target");
        auto* res = b.CreateCall(llvm::FunctionType::get(i32Ty, {ptrTy}, false), target, {objPtr});
        b.CreateRet(res);
    }

    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;

    ClassSymbol baseCls;
    baseCls.qualifiedName = "g::Base";
    baseCls.simpleName = "Base";
    baseCls.visibility = Visibility::Public;
    baseCls.memberFunctions = {"g::Base::area"};
    symbols.addClassSymbol(baseCls);

    ClassSymbol derivedCls;
    derivedCls.qualifiedName = "g::D";
    derivedCls.simpleName = "D";
    derivedCls.visibility = Visibility::Public;
    TypeNode baseType;
    baseType.nameParts = {"g", "Base"};
    derivedCls.baseClass = baseType;
    derivedCls.memberFunctions = {"g::D::area"};
    symbols.addClassSymbol(derivedCls);

    FunctionSymbol areaSym;
    areaSym.qualifiedName = "g::D::area";
    areaSym.simpleName = "area";
    areaSym.visibility = Visibility::Public;
    symbols.addFunction(areaSym);

    FunctionSymbol stageSym;
    stageSym.qualifiedName = "g::dyn_dispatch";
    stageSym.simpleName = "dyn_dispatch";
    stageSym.visibility = Visibility::Protected;
    Parameter param;
    param.name = "obj";
    param.type.nameParts = {"g", "D"}; // concrete
    param.type.modifier = TypeNode::Ptr;
    stageSym.params.push_back(param);
    symbols.addFunction(stageSym);

    LogicBlockEntry lb;
    lb.qualifiedName = "g::render";
    lb.simpleName = "render";
    lb.isPipeline = true;
    lb.calledFunctions = {"g::dyn_dispatch"};
    PipelineAnalysis analysis;
    analysis.stages = {{"g::dyn_dispatch", 0}};
    lb.pipelineAnalysis = analysis;
    symbols.addLogicBlock(lb);

    mapping.matched["g::dyn_dispatch"] = stageFunc;
    mapping.matched["g::D::area"] = areaFn;

    IndirectionConfig config;
    config.mode = topo::FeatureMode::Force;
    config.uniquePtrPromotion = false;
    config.sharedPtrExclusive = false;
    config.vectorSpanLowering = false;
    config.pointerAttrInference = false;
    config.devirtualize = true;
    config.vtableOptimize = false;

    auto stats = IndirectionPass::run(*module, entries, mapping, symbols, config);

    EXPECT_EQ(stats.callsDevirtualized, 0) << "runtime-indexed vtable slot must not be devirtualized to method[0]";
    bool stillIndirect = false;
    for (auto& bb : *stageFunc) {
        for (auto& inst : bb) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (!call->getCalledFunction()) stillIndirect = true;
            }
        }
    }
    EXPECT_TRUE(stillIndirect) << "call with a non-constant vtable index should remain indirect";
}

} // namespace
