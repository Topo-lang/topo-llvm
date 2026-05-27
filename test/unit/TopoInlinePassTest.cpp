#include "topo/Transforms/TopoInlinePass.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Build/PassConfig.h"
#include "topo/Sema/SymbolTable.h"
#include "topo/Sema/VisibilityCollector.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class TopoInlinePassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

static VisibilityEntry makeEntry(const std::string& name, Visibility vis) {
    VisibilityEntry e;
    e.qualifiedName = name;
    e.visibility = vis;
    return e;
}

static llvm::Function* createTrivialFunc(llvm::Module& m, const std::string& name) {
    auto& ctx = m.getContext();
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* fty = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
    auto* f = llvm::Function::Create(fty, llvm::GlobalValue::InternalLinkage, name, m);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", f);
    llvm::IRBuilder<> b(bb);
    b.CreateRet(b.CreateAdd(f->getArg(0), llvm::ConstantInt::get(i32Ty, 1)));
    return f;
}

// Create a function with `instCount` additions in its body so that
// countInstructions() exceeds the configured size threshold.
static llvm::Function* createLargeFunc(llvm::Module& m, const std::string& name, unsigned instCount) {
    auto& ctx = m.getContext();
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* fty = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
    auto* f = llvm::Function::Create(fty, llvm::GlobalValue::InternalLinkage, name, m);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", f);
    llvm::IRBuilder<> b(bb);
    llvm::Value* acc = f->getArg(0);
    for (unsigned i = 0; i < instCount; ++i) {
        acc = b.CreateAdd(acc, llvm::ConstantInt::get(i32Ty, static_cast<int>(i)));
    }
    b.CreateRet(acc);
    return f;
}

static llvm::Function* createCaller(llvm::Module& m, const std::string& name, llvm::Function* callee, unsigned times) {
    auto& ctx = m.getContext();
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* fty = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
    auto* f = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, name, m);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", f);
    llvm::IRBuilder<> b(bb);
    llvm::Value* acc = f->getArg(0);
    for (unsigned i = 0; i < times; ++i) {
        acc = b.CreateCall(callee, {acc});
    }
    b.CreateRet(acc);
    return f;
}

TEST_F(TopoInlinePassTest, InternalVisibilityMarkedAlwaysInline) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("internal_vis", ctx);
    auto* f = createTrivialFunc(*module, "internal_fn");

    SymbolMapping mapping;
    mapping.matched["ns::internal_fn"] = f;

    std::vector<VisibilityEntry> entries;
    entries.push_back(makeEntry("ns::internal_fn", Visibility::Internal));

    int annotated = TopoInlinePass::run(*module, OptLevel::O0, entries, mapping,
                                        /*symbols=*/nullptr, /*config=*/nullptr);
    EXPECT_GE(annotated, 1);
    EXPECT_TRUE(f->hasFnAttribute(llvm::Attribute::AlwaysInline));
}

TEST_F(TopoInlinePassTest, SingleCallerPrivateMarkedAlwaysInline) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("single_caller", ctx);
    auto* callee = createTrivialFunc(*module, "helper");
    createCaller(*module, "caller_one", callee, /*times=*/1);

    SymbolMapping mapping;
    mapping.matched["ns::helper"] = callee;

    std::vector<VisibilityEntry> entries;
    entries.push_back(makeEntry("ns::helper", Visibility::Private));

    TopoInlinePass::run(*module, OptLevel::O2, entries, mapping, /*symbols=*/nullptr, /*config=*/nullptr);
    EXPECT_TRUE(callee->hasFnAttribute(llvm::Attribute::AlwaysInline));
    EXPECT_FALSE(callee->hasFnAttribute(llvm::Attribute::InlineHint));
}

TEST_F(TopoInlinePassTest, MultiCallerPrivateGetsInlineHintAtO2) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("multi_caller", ctx);
    auto* callee = createTrivialFunc(*module, "helper");
    createCaller(*module, "caller_one", callee, /*times=*/1);
    createCaller(*module, "caller_two", callee, /*times=*/1);

    SymbolMapping mapping;
    mapping.matched["ns::helper"] = callee;

    std::vector<VisibilityEntry> entries;
    entries.push_back(makeEntry("ns::helper", Visibility::Private));

    TopoInlinePass::run(*module, OptLevel::O2, entries, mapping, /*symbols=*/nullptr, /*config=*/nullptr);
    EXPECT_TRUE(callee->hasFnAttribute(llvm::Attribute::InlineHint));
    EXPECT_FALSE(callee->hasFnAttribute(llvm::Attribute::AlwaysInline));
}

TEST_F(TopoInlinePassTest, ProtectedNoChangeAtO0) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("protected_o0", ctx);
    auto* f = createTrivialFunc(*module, "protected_fn");

    SymbolMapping mapping;
    mapping.matched["ns::protected_fn"] = f;

    std::vector<VisibilityEntry> entries;
    entries.push_back(makeEntry("ns::protected_fn", Visibility::Protected));

    int annotated = TopoInlinePass::run(*module, OptLevel::O0, entries, mapping,
                                        /*symbols=*/nullptr, /*config=*/nullptr);
    EXPECT_EQ(annotated, 0);
    EXPECT_FALSE(f->hasFnAttribute(llvm::Attribute::AlwaysInline));
    EXPECT_FALSE(f->hasFnAttribute(llvm::Attribute::InlineHint));
}

TEST_F(TopoInlinePassTest, ProtectedGetsInlineHintAtO2) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("protected_o2", ctx);
    auto* f = createTrivialFunc(*module, "protected_fn");

    SymbolMapping mapping;
    mapping.matched["ns::protected_fn"] = f;

    std::vector<VisibilityEntry> entries;
    entries.push_back(makeEntry("ns::protected_fn", Visibility::Protected));

    TopoInlinePass::run(*module, OptLevel::O2, entries, mapping, /*symbols=*/nullptr, /*config=*/nullptr);
    EXPECT_TRUE(f->hasFnAttribute(llvm::Attribute::InlineHint));
}

TEST_F(TopoInlinePassTest, RecursiveFunctorCalleeSkipped) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("recursive_callee", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* unaryTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

    // Recursive helper: calls itself.
    auto* recursive = llvm::Function::Create(unaryTy, llvm::GlobalValue::InternalLinkage, "recursive", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", recursive);
        llvm::IRBuilder<> b(bb);
        auto* arg = recursive->getArg(0);
        auto* sub = b.CreateSub(arg, llvm::ConstantInt::get(i32Ty, 1));
        b.CreateCall(recursive, {sub});
        b.CreateRet(sub);
    }

    // Pipeline functor calls recursive.
    auto* functor = llvm::Function::Create(unaryTy, llvm::GlobalValue::ExternalLinkage, "functor", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", functor);
        llvm::IRBuilder<> b(bb);
        auto* r = b.CreateCall(recursive, {functor->getArg(0)});
        b.CreateRet(r);
    }

    SymbolMapping mapping;
    mapping.matched["ns::recursive"] = recursive;
    mapping.matched["ns::functor"] = functor;

    std::vector<VisibilityEntry> entries;
    // Multi-caller private → step 1 would set InlineHint. Step 2 must
    // skip the upgrade because recursive was detected in a cycle.
    entries.push_back(makeEntry("ns::recursive", Visibility::Private));

    // Second caller to ensure phase 1 uses InlineHint, not AlwaysInline.
    createCaller(*module, "other_caller", recursive, /*times=*/1);

    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "ns::functor";
    lb.simpleName = "functor";
    lb.isPipeline = true;
    symbols.addLogicBlock(lb);

    TopoInlinePass::run(*module, OptLevel::O2, entries, mapping, &symbols, /*config=*/nullptr);
    // recursive callee must NOT end up AlwaysInline because of cycle detection.
    EXPECT_FALSE(recursive->hasFnAttribute(llvm::Attribute::AlwaysInline));
}

TEST_F(TopoInlinePassTest, LargeFunctorCalleeGetsAlwaysInline) {
    // `functorSizeThreshold` was removed — Topo passes do
    // not gate on workload-side cost heuristics. All private/internal
    // functor callees uniformly receive AlwaysInline; LLVM's standard
    // inliner is the cost model authority for what actually inlines.
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("large_callee", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* unaryTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

    auto* large = createLargeFunc(*module, "large_callee", /*instCount=*/200);
    createCaller(*module, "caller_one", large, /*times=*/1);
    createCaller(*module, "caller_two", large, /*times=*/1);

    auto* functor = llvm::Function::Create(unaryTy, llvm::GlobalValue::ExternalLinkage, "functor_lg", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", functor);
        llvm::IRBuilder<> b(bb);
        auto* r = b.CreateCall(large, {functor->getArg(0)});
        b.CreateRet(r);
    }

    SymbolMapping mapping;
    mapping.matched["ns::large_callee"] = large;
    mapping.matched["ns::functor_lg"] = functor;

    std::vector<VisibilityEntry> entries;
    entries.push_back(makeEntry("ns::large_callee", Visibility::Private));

    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "ns::functor_lg";
    lb.simpleName = "functor_lg";
    lb.isPipeline = true;
    symbols.addLogicBlock(lb);

    InlineConfig config;
    TopoInlinePass::run(*module, OptLevel::O2, entries, mapping, &symbols, &config);
    EXPECT_TRUE(large->hasFnAttribute(llvm::Attribute::AlwaysInline));
}

TEST_F(TopoInlinePassTest, SymbolTableNullOnlyPhase1) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("null_symbols", ctx);
    auto* internalFn = createTrivialFunc(*module, "internal_fn");
    auto* privateFn = createTrivialFunc(*module, "private_fn");
    createCaller(*module, "caller_only", privateFn, /*times=*/1);

    SymbolMapping mapping;
    mapping.matched["ns::internal_fn"] = internalFn;
    mapping.matched["ns::private_fn"] = privateFn;

    std::vector<VisibilityEntry> entries;
    entries.push_back(makeEntry("ns::internal_fn", Visibility::Internal));
    entries.push_back(makeEntry("ns::private_fn", Visibility::Private));

    int annotated = TopoInlinePass::run(*module, OptLevel::O2, entries, mapping,
                                        /*symbols=*/nullptr, /*config=*/nullptr);
    EXPECT_GE(annotated, 2);
    EXPECT_TRUE(internalFn->hasFnAttribute(llvm::Attribute::AlwaysInline));
    // private_fn has one caller → AlwaysInline.
    EXPECT_TRUE(privateFn->hasFnAttribute(llvm::Attribute::AlwaysInline));
}

TEST_F(TopoInlinePassTest, PublicFunctionNotAnnotated) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("public_noop", ctx);
    auto* f = createTrivialFunc(*module, "public_fn");

    SymbolMapping mapping;
    mapping.matched["ns::public_fn"] = f;

    std::vector<VisibilityEntry> entries;
    entries.push_back(makeEntry("ns::public_fn", Visibility::Public));

    int annotated = TopoInlinePass::run(*module, OptLevel::O3, entries, mapping,
                                        /*symbols=*/nullptr, /*config=*/nullptr);
    EXPECT_EQ(annotated, 0);
    EXPECT_FALSE(f->hasFnAttribute(llvm::Attribute::AlwaysInline));
    EXPECT_FALSE(f->hasFnAttribute(llvm::Attribute::InlineHint));
}

// Sidecar report receives one entry per annotated callee with the reason
// the annotation was applied.
TEST_F(TopoInlinePassTest, ReportRecordsCalleesWithReasons) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("report_inline", ctx);
    auto* internal = createTrivialFunc(*module, "internal_fn");
    auto* private_ = createTrivialFunc(*module, "private_fn");
    createCaller(*module, "caller_x", private_, /*times=*/1);

    SymbolMapping mapping;
    mapping.matched["ns::internal_fn"] = internal;
    mapping.matched["ns::private_fn"] = private_;

    std::vector<VisibilityEntry> entries = {
        makeEntry("ns::internal_fn", Visibility::Internal),
        makeEntry("ns::private_fn", Visibility::Private),
    };

    backend::TopoInlineReport report;
    int annotated = TopoInlinePass::run(*module, OptLevel::O2, entries, mapping,
                                        /*symbols=*/nullptr, /*config=*/nullptr, &report);
    EXPECT_GE(annotated, 2);
    ASSERT_EQ(report.entries.size(), 2u);

    bool seenInternal = false, seenPrivate = false;
    for (const auto& e : report.entries) {
        if (e.callee == "ns::internal_fn") { EXPECT_EQ(e.reason, "internal"); seenInternal = true; }
        if (e.callee == "ns::private_fn") { EXPECT_EQ(e.reason, "private_single_caller"); seenPrivate = true; }
    }
    EXPECT_TRUE(seenInternal);
    EXPECT_TRUE(seenPrivate);
}

} // namespace
