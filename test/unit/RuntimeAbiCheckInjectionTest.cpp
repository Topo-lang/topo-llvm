// Verifies that runtime-touching IR passes inject the ABI-version
// ctor pattern described in topo-llvm/runtime/ABI-COMPAT.md.
//
// Two layers of coverage:
//   - Direct: call injectAbiCheckCtor() on a hand-built module and
//     check the synthesized ctor IR (icmp ne against the expected
//     literal, abort() in the mismatch branch, llvm.global_ctors
//     entry).
//   - Pass-level: drive each runtime-touching Pass over a minimal
//     fixture and confirm the ctor it emits is wired and unique.

#include "topo/Transforms/AdaptiveDispatchPass.h"
#include "topo/Transforms/ContainmentInterceptionPass.h"
#include "topo/Transforms/LifetimeArenaPass.h"
#include "topo/Transforms/LoopParallelizePass.h"
#include "topo/Transforms/ObservabilityPass.h"
#include "topo/Transforms/RuntimeAbiCheck.h"
#include "topo/Transforms/RuntimeAbiVersions.h"
#include "topo/Transforms/TopoParallelPass.h"

#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/User.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace topo;

namespace {

// Walk @llvm.global_ctors and collect the i8* / function pointers that
// reference the ctor entries the linker will run at process start.
std::vector<llvm::Function*> collectGlobalCtorFunctions(llvm::Module& module) {
    std::vector<llvm::Function*> ctors;
    auto* gv = module.getGlobalVariable("llvm.global_ctors");
    if (!gv || !gv->hasInitializer()) return ctors;
    auto* arr = llvm::dyn_cast<llvm::ConstantArray>(gv->getInitializer());
    if (!arr) return ctors;
    for (unsigned i = 0; i < arr->getNumOperands(); ++i) {
        auto* entry = llvm::dyn_cast<llvm::ConstantStruct>(arr->getOperand(i));
        if (!entry) continue;
        // Layout: { i32 priority, ptr fn, ptr data }
        auto* fnConst = entry->getOperand(1);
        if (auto* fn = llvm::dyn_cast<llvm::Function>(fnConst->stripPointerCasts())) {
            ctors.push_back(fn);
        }
    }
    return ctors;
}

// Inspect the ctor body to confirm it has the shape:
//   %v = call i32 @<versionSymbol>()
//   %ne = icmp ne i32 %v, <expectedVersion>
//   br i1 %ne, label %mismatch, label %ok
//   mismatch: ... call abort() ... unreachable
//   ok: ret void
// Returns true if all required ops are present.
struct CtorShape {
    bool hasVersionCall = false;
    bool hasIcmpNeAgainstExpected = false;
    bool hasAbortCall = false;
    bool hasWriteCall = false;
};

CtorShape analyzeCtor(llvm::Function* ctor,
                      const std::string& versionSymbol,
                      std::uint32_t expectedVersion) {
    CtorShape s;
    for (auto& bb : *ctor) {
        for (auto& inst : bb) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                auto* callee = call->getCalledFunction();
                if (!callee) continue;
                auto name = callee->getName().str();
                if (name == versionSymbol) s.hasVersionCall = true;
                else if (name == "abort") s.hasAbortCall = true;
                else if (name == "write") s.hasWriteCall = true;
            }
            if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                if (cmp->getPredicate() == llvm::ICmpInst::ICMP_NE) {
                    auto* rhs = llvm::dyn_cast<llvm::ConstantInt>(cmp->getOperand(1));
                    if (rhs && rhs->getZExtValue() == expectedVersion) {
                        s.hasIcmpNeAgainstExpected = true;
                    }
                }
            }
        }
    }
    return s;
}

class RuntimeAbiCheckInjectionTest : public ::testing::Test {};

TEST_F(RuntimeAbiCheckInjectionTest, DirectInjectSynthesizesCtorWithMatchingShape) {
    llvm::LLVMContext ctx;
    llvm::Module module("direct_test", ctx);

    injectAbiCheckCtor(module, "parallel", "topo_parallel_version", topo::abi::kParallelVersion);

    auto* ctor = module.getFunction(runtimeAbiCheckCtorName("parallel"));
    ASSERT_NE(ctor, nullptr) << "ctor was not synthesized";

    // ctor should be referenced by llvm.global_ctors
    auto ctors = collectGlobalCtorFunctions(module);
    EXPECT_NE(std::find(ctors.begin(), ctors.end(), ctor), ctors.end())
        << "ctor not registered in @llvm.global_ctors";

    auto shape = analyzeCtor(ctor, "topo_parallel_version", topo::abi::kParallelVersion);
    EXPECT_TRUE(shape.hasVersionCall) << "expected call to topo_parallel_version()";
    EXPECT_TRUE(shape.hasIcmpNeAgainstExpected) << "expected icmp ne against the pinned version literal";
    EXPECT_TRUE(shape.hasAbortCall) << "expected call to abort() on the mismatch path";
    EXPECT_TRUE(shape.hasWriteCall) << "expected write(STDERR_FILENO, diagnostic, len) on the mismatch path";
}

TEST_F(RuntimeAbiCheckInjectionTest, IdempotentSecondCallDoesNotDuplicateCtor) {
    llvm::LLVMContext ctx;
    llvm::Module module("idempotent_test", ctx);

    injectAbiCheckCtor(module, "parallel", "topo_parallel_version", topo::abi::kParallelVersion);
    injectAbiCheckCtor(module, "parallel", "topo_parallel_version", topo::abi::kParallelVersion);

    // Exactly one ctor function with this name should exist.
    int count = 0;
    for (auto& f : module) {
        if (f.getName() == runtimeAbiCheckCtorName("parallel")) ++count;
    }
    EXPECT_EQ(count, 1);

    // Exactly one entry in @llvm.global_ctors should reference it.
    int ctorRefs = 0;
    auto ctors = collectGlobalCtorFunctions(module);
    for (auto* f : ctors) {
        if (f->getName() == runtimeAbiCheckCtorName("parallel")) ++ctorRefs;
    }
    EXPECT_EQ(ctorRefs, 1);
}

TEST_F(RuntimeAbiCheckInjectionTest, DistinctLibrariesProduceDistinctCtors) {
    llvm::LLVMContext ctx;
    llvm::Module module("multi_lib_test", ctx);

    injectAbiCheckCtor(module, "parallel", "topo_parallel_version", topo::abi::kParallelVersion);
    injectAbiCheckCtor(module, "arena", "topo_arena_version", topo::abi::kArenaVersion);
    injectAbiCheckCtor(module, "adaptive", "topo_adaptive_version", topo::abi::kAdaptiveVersion);

    EXPECT_NE(module.getFunction("topo_parallel_abi_check_ctor"), nullptr);
    EXPECT_NE(module.getFunction("topo_arena_abi_check_ctor"), nullptr);
    EXPECT_NE(module.getFunction("topo_adaptive_abi_check_ctor"), nullptr);

    auto ctors = collectGlobalCtorFunctions(module);
    EXPECT_GE(ctors.size(), 3u);
}

// ===================================================================
// Pass-level coverage: each runtime-touching pass should wire the
// ABI-check ctor when it actually emits a runtime call.
// ===================================================================

// Tiny pipeline fixture reused across pass-level cases.
struct TinyPipeline {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* pipelineFunc = nullptr;
    llvm::Function* aFunc = nullptr;
    llvm::Function* bFunc = nullptr;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("tiny", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* funcTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

        aFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "a", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", aFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateAdd(aFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 1)));
        }
        bFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "b", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", bFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateMul(bFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 2)));
        }
        pipelineFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            auto* r1 = b.CreateCall(aFunc, {pipelineFunc->getArg(0)});
            auto* r2 = b.CreateCall(bFunc, {pipelineFunc->getArg(0)});
            b.CreateRet(b.CreateAdd(r1, r2));
        }

        FunctionSymbol aSym;
        aSym.qualifiedName = "ns::a";
        aSym.simpleName = "a";
        aSym.visibility = Visibility::Protected;
        symbols.addFunction(aSym);

        FunctionSymbol bSym;
        bSym.qualifiedName = "ns::b";
        bSym.simpleName = "b";
        bSym.visibility = Visibility::Protected;
        symbols.addFunction(bSym);

        LogicBlockEntry lb;
        lb.qualifiedName = "ns::pipeline";
        lb.simpleName = "pipeline";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::a", "ns::b"};
        lb.edges = {{"load", "a"}, {"load", "b"}};

        PipelineAnalysis analysis;
        analysis.stages = {{"a", 1}, {"b", 1}};
        analysis.sourceNodes = {"load"};
        analysis.terminalNode = "compose";
        lb.pipelineAnalysis = analysis;
        symbols.addLogicBlock(lb);

        mapping.matched["ns::pipeline"] = pipelineFunc;
        mapping.matched["ns::a"] = aFunc;
        mapping.matched["ns::b"] = bFunc;
    }
};

TEST_F(RuntimeAbiCheckInjectionTest, TopoParallelPassInjectsParallelAbiCheck) {
    llvm::LLVMContext ctx;
    TinyPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Force;

    int n = TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config);
    ASSERT_GT(n, 0);

    EXPECT_NE(tp.module->getFunction(runtimeAbiCheckCtorName("parallel")), nullptr);
    EXPECT_NE(tp.module->getNamedValue("topo_parallel_version"), nullptr)
        << "topo_parallel_version reference must be emitted when at least one pipeline was rewritten";
}

TEST_F(RuntimeAbiCheckInjectionTest, TopoParallelPassSkipsAbiCheckWhenNoPipelineMatches) {
    // Regression: when [parallel] is enabled by config but the symbol
    // table contains no pipeline logic block, the pass must NOT inject
    // the parallel ABI ctor. Injecting it unconditionally produces an
    // undefined reference to topo_parallel_version that fails to link
    // unless libtopo-parallel happens to be on the link line.
    //
    // Aligns TopoParallelPass with the other 5 runtime-touching passes
    // (Adaptive / ContainmentInterception / LifetimeArena / Observability
    // / LoopParallelize) — all of them gate the ctor on a non-zero
    // rewrite count.
    llvm::LLVMContext ctx;
    llvm::Module module("empty_test", ctx);

    SymbolTable symbols;   // intentionally empty — no logic blocks
    SymbolMapping mapping; // intentionally empty — nothing to match

    ParallelConfig config;
    config.mode = topo::FeatureMode::Force;

    int n = TopoParallelPass::run(module, symbols, mapping, config);
    EXPECT_EQ(n, 0) << "no pipeline present, nothing should be parallelized";

    // The parallel ABI ctor must NOT have been registered.
    EXPECT_EQ(module.getFunction(runtimeAbiCheckCtorName("parallel")), nullptr)
        << "ABI ctor must not be injected when no pipeline was rewritten";
    EXPECT_EQ(module.getNamedValue("topo_parallel_version"), nullptr)
        << "topo_parallel_version reference must not be emitted when no pipeline was rewritten";
}

TEST_F(RuntimeAbiCheckInjectionTest, SharedParallelCtorIsNotDuplicatedByLoopParallelize) {
    // Two passes both touch libtopo-parallel. Run TopoParallelPass first
    // (wires the ctor), then LoopParallelizePass on the same module —
    // the second pass must reuse the existing ctor, not register a new
    // one.
    llvm::LLVMContext ctx;
    TinyPipeline tp;
    tp.build(ctx);

    ParallelConfig pcfg;
    pcfg.mode = topo::FeatureMode::Force;
    TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, pcfg);

    LoopParallelConfig lpcfg;
    lpcfg.mode = topo::FeatureMode::Force;
    lpcfg.partitionEnabled = true;
    LoopParallelizePass::run(*tp.module, tp.symbols, tp.mapping, lpcfg, nullptr);

    // Exactly one parallel ABI ctor function should exist.
    int count = 0;
    for (auto& f : *tp.module) {
        if (f.getName() == runtimeAbiCheckCtorName("parallel")) ++count;
    }
    EXPECT_EQ(count, 1);

    // Exactly one entry in @llvm.global_ctors should reference it.
    int ctorRefs = 0;
    auto ctors = collectGlobalCtorFunctions(*tp.module);
    for (auto* f : ctors) {
        if (f->getName() == runtimeAbiCheckCtorName("parallel")) ++ctorRefs;
    }
    EXPECT_EQ(ctorRefs, 1);
}

TEST_F(RuntimeAbiCheckInjectionTest, ObservabilityPassInjectsObserveAbiCheck) {
    llvm::LLVMContext ctx;
    TinyPipeline tp;
    tp.build(ctx);

    ObservabilityConfig config;
    config.mode = topo::FeatureMode::Force;

    int n = ObservabilityPass::run(*tp.module, tp.symbols, tp.mapping, config);
    if (n == 0) {
        // ObservabilityPass may not instrument depending on logic-block
        // shape; if it didn't emit runtime calls, no ctor is expected
        // (the gate is intentional — see RuntimeAbiCheck.h).
        EXPECT_EQ(tp.module->getFunction(runtimeAbiCheckCtorName("observe")), nullptr);
    } else {
        EXPECT_NE(tp.module->getFunction(runtimeAbiCheckCtorName("observe")), nullptr);
    }
}

} // anonymous namespace
